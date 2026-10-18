#include "tasks.hpp"

#include "workspace.hpp"

namespace maas {

std::string new_task_id() { return "t_" + random_base32(8); }

static std::string upper_state(const std::string& text) {
  std::string result = text;
  for (char& character : result) {
    if (character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A');
  }
  return result;
}

static const char* kPathSeparator =
#ifdef _WIN32
    ";";
#else
    ":";
#endif

TaskManager::TaskManager(Context& context) : context_(context) {}

TaskManager::~TaskManager() { stop(); }

void TaskManager::add_observer(TaskObserver* observer) {
  std::lock_guard<std::mutex> lock(observers_mutex_);
  observers_.push_back(observer);
}

void TaskManager::start() {
  stopping_.store(false);
  supervisor_ = std::thread([this]() { supervisor_loop(); });
}

void TaskManager::stop() {
  if (stopping_.exchange(true)) return;
  {
    std::lock_guard<std::mutex> lock(running_mutex_);
    for (auto& item : running_) {
      item.second.kill_reason = "SHUTDOWN";
      kill_job(item.second.handle, true);
    }
  }
  {
    std::lock_guard<std::mutex> lock(internal_mutex_);
    for (auto& item : internal_jobs_) item.second->cancel.store(true);
  }
  notify_changed();
  if (supervisor_.joinable()) supervisor_.join();
  // Collect the jobs we just killed so they are recorded as KILLED, not LOST.
  int64_t deadline = mono_ms() + 3000;
  while (true) {
    std::vector<std::pair<RunningJob, ExitInfo>> finished;
    {
      std::lock_guard<std::mutex> lock(running_mutex_);
      for (auto item = running_.begin(); item != running_.end();) {
        ExitInfo info;
        if (poll_job(item->second.handle, info) || mono_ms() > deadline) {
          finished.push_back({item->second, info});
          item = running_.erase(item);
        } else {
          ++item;
        }
      }
    }
    for (auto& pair : finished) finalize_job(pair.first, pair.second);
    bool empty = false;
    {
      std::lock_guard<std::mutex> lock(running_mutex_);
      empty = running_.empty();
    }
    if (empty) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  // Give in-process tasks (ingestion) a moment to notice the cancel flag.
  int64_t internal_deadline = mono_ms() + 5000;
  while (mono_ms() < internal_deadline) {
    {
      std::lock_guard<std::mutex> lock(internal_mutex_);
      if (internal_jobs_.empty()) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

void TaskManager::recover_after_restart() {
  Db& db = context_.db;
  int64_t now = now_sec();
  for (const json& row : db.query("SELECT id, kind, pid, start_token FROM tasks WHERE state = 'RUNNING'")) {
    int64_t pid = json_int(row, "pid");
    if (pid > 0) kill_orphan(pid, json_int(row, "start_token"));
    json failure = {{"failure_kind", "LOST"},
                    {"summary", "the MAAS node stopped while this task was running"},
                    {"hint", "run it again if it is still needed"}};
    db.run("UPDATE tasks SET state = 'LOST', failure_kind = 'LOST', failure_json = ?, ended_at = ?, updated_at = ? "
           "WHERE id = ?",
           {dump_json(failure), now, now, json_str(row, "id")});
  }
  json never_started = {{"failure_kind", "LOST"},
                        {"summary", "approved, but the node restarted before it started"},
                        {"hint", "call it again; it needs a fresh approval"}};
  db.run("UPDATE tasks SET state = 'LOST', failure_kind = 'LOST', failure_json = ?, updated_at = ? "
         "WHERE state = 'APPROVED'",
         {dump_json(never_started), now});
  // Pending approvals are never auto-approved after a restart.
  db.run("UPDATE tasks SET state = 'EXPIRED', decided_at = ?, decided_by = 'node restart', updated_at = ? "
         "WHERE state = 'PENDING'",
         {now, now});
  db.run("UPDATE datasets SET state = 'failed' WHERE state = 'ingesting'");
}

json TaskManager::get_row(const std::string& task_id) {
  return context_.db.query_one("SELECT * FROM tasks WHERE id = ?", {task_id});
}

std::string TaskManager::read_log_tail(const fs::path& path, size_t bytes) {
  uint64_t size = file_size_or_zero(path);
  uint64_t offset = size > bytes ? size - bytes : 0;
  std::string data;
  read_file_range(path, offset, bytes, data);
  return sanitize_utf8(strip_ansi(data));
}

void TaskManager::notify_changed() { wait_cv_.notify_all(); }

void TaskManager::emit(const std::string& event, const std::string& task_id) {
  notify_changed();
  json row = get_row(task_id);
  if (row.is_null()) return;
  std::vector<TaskObserver*> copy;
  {
    std::lock_guard<std::mutex> lock(observers_mutex_);
    copy = observers_;
  }
  json payload = event == "pending" ? approval_view(row) : view(row);
  for (TaskObserver* observer : copy) {
    try {
      if (event == "pending") observer->on_pending(payload);
      else if (event == "decided") observer->on_decided(payload);
      else if (event == "started") observer->on_started(payload);
      else if (event == "finished") observer->on_finished(payload);
    } catch (const std::exception& problem) {
      log_warn(std::string("notification failed: ") + problem.what());
    }
  }
}

// ---------------------------------------------------------------- creating tasks

CreateResult TaskManager::create(const TaskRequest& request) {
  CreateResult result;
  Db& db = context_.db;
  const Config& config = context_.config;
  int64_t now = now_sec();

  if (request.kind == "script" || request.kind == "install") {
    json recent = db.query_one(
        "SELECT COUNT(*) AS count FROM tasks WHERE kind IN ('script','install') AND created_at > ?", {now - 3600});
    if (json_int(recent, "count") >= config.max_executions_per_hour) {
      result.error_kind = "RATE_LIMITED";
      result.error = "this node allows " + std::to_string(config.max_executions_per_hour) +
                     " runs per hour; wait, or ask the user to raise max_executions_per_hour";
      return result;
    }
    if (free_disk_bytes(context_.paths.home) < static_cast<uint64_t>(config.disk_floor_mb) * 1024 * 1024) {
      result.error_kind = "DISK_FULL";
      result.error = "the node has less than " + std::to_string(config.disk_floor_mb) +
                     " MB of free disk; delete files first";
      return result;
    }
  }
  json pending = db.query_one("SELECT COUNT(*) AS count FROM tasks WHERE state = 'PENDING'");
  if (json_int(pending, "count") >= 20) {
    result.error_kind = "TOO_MANY_PENDING";
    result.error = "20 approvals are already waiting; wait for the human to decide";
    return result;
  }

  std::string task_id = new_task_id();
  std::string sha;
  std::string stored_path;
  std::string interpreter_path;
  int64_t size_bytes = 0;
  int64_t allow_runs = 0;

  if (request.kind == "script") {
    if (request.content.size() > 2 * 1024 * 1024) {
      result.error_kind = "SCRIPT_TOO_LARGE";
      result.error = "scripts are limited to 2 MB; keep data in files, not in the script";
      return result;
    }
    sha = sha256_hex(request.content);
    size_bytes = static_cast<int64_t>(request.content.size());
    json stats = db.query_one("SELECT failures, allow_runs FROM hash_stats WHERE sha256 = ?", {sha});
    if (!stats.is_null()) {
      if (json_int(stats, "failures") >= config.max_failures_per_hash) {
        result.error_kind = "REPEATED_FAILURE";
        result.error = "this exact script already failed " + std::to_string(json_int(stats, "failures")) +
                       " times. Change the code to fix the cause before running it again.";
        return result;
      }
      allow_runs = json_int(stats, "allow_runs");
    }
    Runtime runtime;
    if (!context_.runtimes.find(request.interpreter, runtime)) {
      result.error_kind = "RUNTIME_NOT_FOUND";
      result.error = "no " + request.interpreter + " runtime on this node";
      if (request.interpreter == "python") {
        result.error += ". Ask the user to run `maas runtime install python` on the node (portable, no admin needed).";
      }
      return result;
    }
    interpreter_path = runtime.path;
    // Content-addressed copy: what gets approved is exactly what runs, even if
    // the workspace file is edited afterwards.
    std::string base_name = path_str(u8path(request.display_name).filename());
    if (base_name.empty()) base_name = "script";
    fs::path stored = context_.paths.store / "scripts" / sha / u8path(base_name);
    std::error_code error;
    if (!fs::exists(stored, error)) {
      if (!write_file_atomic(stored, request.content)) {
        result.error_kind = "STORE_FAILED";
        result.error = "could not save the script copy";
        return result;
      }
    }
    stored_path = path_str(stored);
  } else if (request.kind == "install") {
    sha = sha256_hex(dump_json(request.payload));
  } else if (request.kind == "delete") {
    sha = sha256_hex(json_str(request.payload, "path"));
  } else if (request.kind == "ingest") {
    sha = sha256_hex(dump_json(request.payload));
  } else {
    result.error_kind = "BAD_INPUT";
    result.error = "unknown task kind";
    return result;
  }

  int64_t memory_mb = request.memory_mb > 0 ? request.memory_mb : context_.default_memory_mb;
  if (memory_mb > context_.max_memory_mb) memory_mb = context_.max_memory_mb;
  if (memory_mb < 64) memory_mb = 64;
  int timeout_sec = request.timeout_sec > 0 ? request.timeout_sec : config.default_timeout_sec;
  if (request.kind == "install" && request.timeout_sec <= 0) timeout_sec = 1800;
  if (timeout_sec > config.max_timeout_sec) timeout_sec = config.max_timeout_sec;

  fs::path log_path = context_.paths.task_logs / (task_id + ".log");
  std::string error_text;
  bool inserted = db.run(
      "INSERT INTO tasks(id, kind, state, created_at, updated_at, display_name, source_path, stored_path, sha256, "
      "size_bytes, interpreter, interpreter_path, args_json, cwd, explanation, requested_by, payload_json, "
      "timeout_sec, memory_mb, approval_deadline, log_path) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
      {task_id, request.kind, "PENDING", now, now, request.display_name, request.source_path, stored_path, sha,
       size_bytes, request.interpreter, interpreter_path, dump_json(json(request.args)), request.cwd_relative,
       truncate_utf8(request.explanation, 2000), request.requested_by, dump_json(request.payload), timeout_sec,
       memory_mb, now + config.approval_timeout_sec, path_str(log_path)},
      nullptr, &error_text);
  if (!inserted) {
    result.error_kind = "STATE_ERROR";
    result.error = "could not record the task: " + error_text;
    return result;
  }
  audit(db, request.requested_by, "task_created", task_id + " " + request.kind + " " + request.display_name);

  bool pre_approved = false;
  if (request.kind == "script" && allow_runs > 0) {
    int changes = 0;
    db.run("UPDATE hash_stats SET allow_runs = allow_runs - 1 WHERE sha256 = ? AND allow_runs > 0", {sha}, &changes);
    if (changes == 1) {
      db.run("UPDATE tasks SET state = 'APPROVED', decided_at = ?, decided_by = ?, updated_at = ? WHERE id = ?",
             {now, "pre-approved script (" + std::to_string(allow_runs - 1) + " runs left)", now, task_id});
      pre_approved = true;
    }
  }
  emit(pre_approved ? "decided" : "pending", task_id);
  result.ok = true;
  result.task = view_by_id(task_id);
  return result;
}

std::string TaskManager::run_internal(const std::string& kind, const std::string& display_name, const json& payload,
                                      const std::string& requested_by, std::function<void(InternalJob&)> body) {
  std::string task_id = new_task_id();
  int64_t now = now_sec();
  context_.db.run(
      "INSERT INTO tasks(id, kind, state, created_at, updated_at, display_name, payload_json, requested_by, "
      "decided_by) VALUES (?,?,?,?,?,?,?,?,?)",
      {task_id, kind, "APPROVED", now, now, display_name, dump_json(payload), requested_by, "automatic"});
  start_internal_for(task_id, body);
  return task_id;
}

void TaskManager::start_internal_for(const std::string& task_id, std::function<void(InternalJob&)> body) {
  int64_t now = now_sec();
  context_.db.run("UPDATE tasks SET state = 'RUNNING', started_at = ?, updated_at = ? WHERE id = ?", {now, now, task_id});
  auto job = std::make_shared<InternalJob>();
  job->task_id = task_id;
  Db* db = &context_.db;
  job->report_progress = [db, task_id](const json& progress) {
    db->run("UPDATE tasks SET progress_json = ?, updated_at = ? WHERE id = ?", {dump_json(progress), now_sec(), task_id});
  };
  {
    std::lock_guard<std::mutex> lock(internal_mutex_);
    internal_jobs_[task_id] = job;
  }
  emit("started", task_id);
  std::thread worker([this, job, body, task_id]() {
    try {
      body(*job);
    } catch (const std::exception& problem) {
      job->failure_kind = "INTERNAL_ERROR";
      job->failure = {{"summary", problem.what()}};
    }
    bool cancelled = job->cancel.load();
    std::string state = cancelled ? "KILLED" : "EXITED";
    std::string kind_out = cancelled ? "KILLED" : job->failure_kind;
    json failure = job->failure;
    if (!failure.is_object()) failure = json::object();
    failure["failure_kind"] = kind_out;
    if (cancelled && !failure.contains("summary")) failure["summary"] = "cancelled";
    context_.db.run(
        "UPDATE tasks SET state = ?, failure_kind = ?, failure_json = ?, ended_at = ?, updated_at = ? WHERE id = ?",
        {state, kind_out, dump_json(failure), now_sec(), now_sec(), task_id});
    {
      std::lock_guard<std::mutex> lock(internal_mutex_);
      internal_jobs_.erase(task_id);
    }
    emit("finished", task_id);
  });
  worker.detach();
}

// ---------------------------------------------------------------- decisions

json TaskManager::decide(const std::string& task_id, bool approve, int extra_runs, const std::string& actor,
                         std::string& error) {
  json row = get_row(task_id);
  if (row.is_null()) {
    error = "no such task";
    return nullptr;
  }
  int64_t now = now_sec();
  int changes = 0;
  std::string new_state = approve ? "APPROVED" : "DENIED";
  // "AND state = 'PENDING'" makes double taps and late taps harmless.
  context_.db.run(
      "UPDATE tasks SET state = ?, decided_at = ?, decided_by = ?, deny_reason = ?, updated_at = ? "
      "WHERE id = ? AND state = 'PENDING'",
      {new_state, now, actor, approve ? json(nullptr) : json("denied by " + actor), now, task_id}, &changes);
  if (changes == 0) {
    error = "already " + to_lower(json_str(row, "state"));
    return view_by_id(task_id);
  }
  if (approve && json_str(row, "kind") == "script") {
    context_.db.run(
        "INSERT INTO last_approved(source_path, sha256, stored_path) VALUES (?,?,?) "
        "ON CONFLICT(source_path) DO UPDATE SET sha256 = excluded.sha256, stored_path = excluded.stored_path",
        {json_str(row, "source_path"), json_str(row, "sha256"), json_str(row, "stored_path")});
    if (extra_runs > 0) {
      if (extra_runs > 50) extra_runs = 50;
      context_.db.run(
          "INSERT INTO hash_stats(sha256, failures, successes, allow_runs) VALUES (?, 0, 0, ?) "
          "ON CONFLICT(sha256) DO UPDATE SET allow_runs = excluded.allow_runs",
          {json_str(row, "sha256"), extra_runs});
    }
  }
  audit(context_.db, actor, approve ? "approved" : "denied", task_id + " " + json_str(row, "display_name"));
  emit("decided", task_id);
  return view_by_id(task_id);
}

bool TaskManager::terminate(const std::string& task_id, const std::string& actor, std::string& error) {
  json row = get_row(task_id);
  if (row.is_null()) {
    error = "no such task";
    return false;
  }
  std::string state = json_str(row, "state");
  int64_t now = now_sec();
  if (state == "PENDING") {
    context_.db.run("UPDATE tasks SET state = 'DENIED', decided_at = ?, decided_by = ?, deny_reason = 'cancelled', "
                    "updated_at = ? WHERE id = ? AND state = 'PENDING'",
                    {now, actor, now, task_id});
    emit("decided", task_id);
    return true;
  }
  if (state == "APPROVED") {
    json failure = {{"failure_kind", "KILLED"}, {"summary", "cancelled before it started"}};
    context_.db.run("UPDATE tasks SET state = 'KILLED', failure_kind = 'KILLED', failure_json = ?, ended_at = ?, "
                    "updated_at = ? WHERE id = ? AND state = 'APPROVED'",
                    {dump_json(failure), now, now, task_id});
    emit("finished", task_id);
    return true;
  }
  if (state == "RUNNING") {
    {
      std::lock_guard<std::mutex> lock(running_mutex_);
      auto found = running_.find(task_id);
      if (found != running_.end()) {
        if (found->second.kill_reason.empty()) found->second.kill_reason = "USER_KILLED";
        kill_job(found->second.handle, false);
        found->second.kill_sent_mono = mono_ms();
        audit(context_.db, actor, "terminated", task_id);
        return true;
      }
    }
    std::lock_guard<std::mutex> lock(internal_mutex_);
    auto internal = internal_jobs_.find(task_id);
    if (internal != internal_jobs_.end()) {
      internal->second->cancel.store(true);
      audit(context_.db, actor, "cancelled", task_id);
      return true;
    }
  }
  return true;  // already finished: terminating is idempotent
}

int TaskManager::kill_all(const std::string& actor) {
  int count = 0;
  for (const json& row : context_.db.query("SELECT id FROM tasks WHERE state IN ('PENDING','APPROVED','RUNNING')")) {
    std::string error;
    if (terminate(json_str(row, "id"), actor, error)) count++;
  }
  audit(context_.db, actor, "kill_all", std::to_string(count));
  return count;
}

void TaskManager::set_paused(bool paused, const std::string& actor) {
  paused_.store(paused);
  audit(context_.db, actor, paused ? "paused" : "resumed", "");
  log_info(std::string(paused ? "⏸  Paused: approved jobs wait until resumed" : "▶  Resumed") + " (by " + actor + ")");
}

int TaskManager::running_count() {
  std::lock_guard<std::mutex> lock(running_mutex_);
  return static_cast<int>(running_.size());
}

std::vector<json> TaskManager::pending_approvals() {
  std::vector<json> result;
  for (const json& row : context_.db.query("SELECT * FROM tasks WHERE state = 'PENDING' ORDER BY created_at")) {
    result.push_back(approval_view(row));
  }
  return result;
}

bool TaskManager::wait_for_change(const std::string& task_id, const std::string& from_state, int timeout_ms) {
  int64_t deadline = mono_ms() + timeout_ms;
  while (true) {
    json row = get_row(task_id);
    if (row.is_null() || json_str(row, "state") != from_state) return true;
    int64_t remaining = deadline - mono_ms();
    if (remaining <= 0 || stopping_.load()) return false;
    std::unique_lock<std::mutex> lock(wait_mutex_);
    wait_cv_.wait_for(lock, std::chrono::milliseconds(remaining < 1000 ? remaining : 1000));
  }
}

// ---------------------------------------------------------------- the supervisor

void TaskManager::supervisor_loop() {
  while (!stopping_.load()) {
    try {
      expire_pending();
      start_approved();
      monitor_jobs();
    } catch (const std::exception& problem) {
      log_error(std::string("supervisor: ") + problem.what());
    }
    std::unique_lock<std::mutex> lock(wait_mutex_);
    wait_cv_.wait_for(lock, std::chrono::milliseconds(250));
  }
}

void TaskManager::expire_pending() {
  int64_t now = now_sec();
  for (const json& row :
       context_.db.query("SELECT id FROM tasks WHERE state = 'PENDING' AND approval_deadline < ?", {now})) {
    int changes = 0;
    context_.db.run("UPDATE tasks SET state = 'EXPIRED', decided_at = ?, decided_by = 'timeout', updated_at = ? "
                    "WHERE id = ? AND state = 'PENDING'",
                    {now, now, json_str(row, "id")}, &changes);
    if (changes == 1) emit("decided", json_str(row, "id"));
  }
}

void TaskManager::start_approved() {
  std::vector<json> approved =
      context_.db.query("SELECT * FROM tasks WHERE state = 'APPROVED' ORDER BY decided_at LIMIT 20");
  for (const json& row : approved) {
    std::string kind = json_str(row, "kind");
    if (kind == "delete") {
      perform_delete(row);
      continue;
    }
    if (kind == "ingest") {
      if (internal_body_factory) start_internal_for(json_str(row, "id"), internal_body_factory(row));
      continue;
    }
    if (paused_.load()) continue;
    if (running_count() >= context_.max_concurrent_jobs) break;
    std::string error;
    if (!start_job(row, error)) {
      json failure = {{"failure_kind", "FAILED_TO_START"}, {"summary", error}};
      if (starts_with(error, "HASH_MISMATCH")) failure["failure_kind"] = "HASH_MISMATCH";
      context_.db.run("UPDATE tasks SET state = 'EXITED', failure_kind = ?, failure_json = ?, ended_at = ?, "
                      "updated_at = ? WHERE id = ?",
                      {json_str(failure, "failure_kind"), dump_json(failure), now_sec(), now_sec(),
                       json_str(row, "id")});
      log_warn("could not start " + json_str(row, "id") + ": " + error);
      emit("finished", json_str(row, "id"));
    }
  }
}

bool TaskManager::start_job(const json& row, std::string& error) {
  const Paths& paths = context_.paths;
  std::string task_id = json_str(row, "id");
  std::string kind = json_str(row, "kind");
  int64_t memory_mb = json_int(row, "memory_mb", context_.default_memory_mb);
  int timeout_sec = static_cast<int>(json_int(row, "timeout_sec", context_.config.default_timeout_sec));
  SpawnSpec spec;
  std::string stored_name;
  std::string interpreter = json_str(row, "interpreter");

  if (kind == "script") {
    fs::path stored = u8path(json_str(row, "stored_path"));
    std::string current_hash, hash_error;
    // Re-check the bytes right before running: this closes the gap between
    // approval and execution (time-of-check vs time-of-use).
    if (!sha256_file(stored, current_hash, hash_error) || current_hash != json_str(row, "sha256")) {
      error = "HASH_MISMATCH: the approved script changed on disk, so it was not run";
      return false;
    }
    Runtime runtime;
    if (!context_.runtimes.find(interpreter, runtime)) {
      error = "the " + interpreter + " runtime is no longer available";
      return false;
    }
    spec.argv = context_.runtimes.command_prefix(runtime);
    spec.argv.push_back(path_str(stored));
    json args = json::parse(json_str(row, "args_json", "[]"), nullptr, false);
    if (args.is_array()) {
      for (const json& item : args) spec.argv.push_back(item.is_string() ? item.get<std::string>() : dump_json(item));
    }
    stored_name = path_str(stored.filename());
    if (interpreter == "python") {
      // RLIMIT_AS counts reserved (not used) memory, and numeric libraries
      // reserve a lot, so this is only a loose backstop. The watchdog below
      // enforces the real limit on resident memory.
      int64_t loose = memory_mb * 4;
      if (loose < memory_mb + 4096) loose = memory_mb + 4096;
      spec.virtual_limit_mb = loose;
    }
  } else if (kind == "install") {
    json payload = json::parse(json_str(row, "payload_json", "{}"), nullptr, false);
    std::string manager = json_str(payload, "manager", "pip");
    std::vector<std::string> packages;
    for (const json& item : payload.value("packages", json::array())) packages.push_back(item.get<std::string>());
    if (manager == "npm") {
      Runtime node;
      if (!context_.runtimes.find("node", node) || node.npm_path.empty()) {
        error = "npm is not installed on this node";
        return false;
      }
      spec.argv = {node.npm_path, "install", "--prefix", path_str(paths.node_packages), "--no-audit", "--no-fund",
                   "--loglevel=error"};
    } else {
      Runtime python;
      if (!context_.runtimes.find("python", python)) {
        error = "no Python on this node (run `maas runtime install python`)";
        return false;
      }
      spec.argv = {python.path, "-m", "pip", "install", "--target", path_str(paths.site_packages), "--upgrade",
                   "--no-input", "--disable-pip-version-check", "--progress-bar", "off", "--no-warn-script-location"};
    }
    for (const std::string& package : packages) spec.argv.push_back(package);
    interpreter = manager;
  } else {
    error = "cannot run task kind " + kind;
    return false;
  }

  fs::path cwd = paths.workspace;
  std::string cwd_relative = json_str(row, "cwd");
  if (!cwd_relative.empty()) {
    JailResult jail = resolve_in_workspace(paths.workspace, cwd_relative, true);
    if (jail.ok) cwd = jail.path;
  }
  spec.cwd = path_str(cwd);
  fs::path log_path = u8path(json_str(row, "log_path"));
  spec.log_path = path_str(log_path);
  spec.memory_limit_mb = memory_mb;
  spec.cpu_limit_sec = static_cast<int64_t>(timeout_sec) * cpu_cores() + 60;
  spec.file_size_limit_mb = context_.config.max_file_mb;
  spec.nice_level = 10;

  std::string cores = std::to_string(cpu_cores());
  spec.env_set = {{"PYTHONPATH", path_str(paths.workspace) + kPathSeparator + path_str(paths.site_packages)},
                  {"PYTHONNOUSERSITE", "1"},
                  {"PYTHONUNBUFFERED", "1"},
                  {"PYTHONIOENCODING", "utf-8"},
                  {"PYTHONUTF8", "1"},
                  {"PYTHONDONTWRITEBYTECODE", "1"},
                  {"-PYTHONHOME", ""},
                  {"-PIP_REQUIRE_VIRTUALENV", ""},
                  {"PIP_USER", "0"},
                  {"PIP_NO_INPUT", "1"},
                  {"PIP_DISABLE_PIP_VERSION_CHECK", "1"},
                  {"MPLBACKEND", "Agg"},
                  {"OMP_NUM_THREADS", cores},
                  {"OPENBLAS_NUM_THREADS", cores},
                  {"MKL_NUM_THREADS", cores},
                  {"NODE_PATH", path_str(paths.node_packages / "node_modules")},
                  {"MAAS_TASK_ID", task_id},
                  {"MAAS_WORKSPACE", path_str(paths.workspace)},
                  {"MAAS_DATASETS", path_str(paths.datasets)},
                  {"MAAS_NODE", context_.node.name}};

  std::string header = "[maas] " + iso_time(now_sec()) + " starting " + json_str(row, "display_name") +
                       " (task " + task_id + ", limit " + std::to_string(memory_mb) + " MB / " +
                       std::to_string(timeout_sec) + " s)\n";
  if (file_size_or_zero(log_path) == 0) write_file_atomic(log_path, header);

  ProcHandle handle;
  if (!spawn_job(spec, handle, error)) return false;
  int64_t now = now_sec();
  context_.db.run("UPDATE tasks SET state = 'RUNNING', pid = ?, start_token = ?, started_at = ?, updated_at = ? "
                  "WHERE id = ?",
                  {handle.pid, handle.start_token, now, now, task_id});
  RunningJob job;
  job.task_id = task_id;
  job.kind = kind;
  job.interpreter = interpreter;
  job.stored_name = stored_name;
  job.handle = handle;
  job.started_mono = mono_ms();
  job.timeout_sec = timeout_sec;
  job.memory_mb = memory_mb;
  job.log_path = log_path;
  {
    std::lock_guard<std::mutex> lock(running_mutex_);
    running_[task_id] = job;
  }
  emit("started", task_id);
  return true;
}

void TaskManager::monitor_jobs() {
  std::vector<std::pair<RunningJob, ExitInfo>> finished;
  int64_t now = mono_ms();
  bool disk_low = false;
  if (now - last_disk_check_ > 5000) {
    last_disk_check_ = now;
    uint64_t floor = static_cast<uint64_t>(context_.config.disk_floor_mb) * 1024 * 1024;
    disk_low = floor > 0 && free_disk_bytes(context_.paths.home) < floor / 2;
  }
  uint64_t max_log_bytes = static_cast<uint64_t>(context_.config.max_log_mb) * 1024 * 1024;
  {
    std::lock_guard<std::mutex> lock(running_mutex_);
    for (auto item = running_.begin(); item != running_.end();) {
      RunningJob& job = item->second;
      ExitInfo info;
      if (poll_job(job.handle, info)) {
        finished.push_back({job, info});
        item = running_.erase(item);
        continue;
      }
      auto stop_with = [&](const std::string& reason, bool force) {
        if (!job.kill_reason.empty()) return;
        job.kill_reason = reason;
        kill_job(job.handle, force);
        job.kill_sent_mono = now;
        if (force) job.force_sent = true;
        log_warn("stopping " + job.task_id + ": " + reason);
      };
      if (job.timeout_sec > 0 && now - job.started_mono > static_cast<int64_t>(job.timeout_sec) * 1000) {
        stop_with("TIMEOUT", false);
      }
      if (now - job.last_rss_check >= 1000) {
        job.last_rss_check = now;
        uint64_t rss = job_rss_kb(job.handle);
        if (rss > job.peak_kb) job.peak_kb = rss;
        if (job.memory_mb > 0 && rss > static_cast<uint64_t>(job.memory_mb) * 1024) stop_with("OOM_KILLED", true);
        if (file_size_or_zero(job.log_path) > max_log_bytes) stop_with("LOG_LIMIT", true);
      }
      if (disk_low) stop_with("DISK_FULL", true);
      // Escalate: a polite stop first, then a hard kill after 4 seconds.
      if (job.kill_sent_mono > 0 && !job.force_sent && now - job.kill_sent_mono > 4000) {
        kill_job(job.handle, true);
        job.force_sent = true;
      }
      ++item;
    }
  }
  for (auto& pair : finished) finalize_job(pair.first, pair.second);
}

json TaskManager::build_failure_json(RunningJob& job, const ExitInfo& info) {
  FailureInput input;
  input.task_kind = job.kind;
  input.interpreter = job.interpreter;
  input.exit = info;
  input.kill_reason = job.kill_reason;
  input.log_tail = read_log_tail(job.log_path, 16384);
  input.peak_kb = job.peak_kb;
  input.memory_mb = job.memory_mb;
  input.timeout_sec = job.timeout_sec;
  input.stored_name = job.stored_name;
  return classify_failure(input);
}

void TaskManager::finalize_job(RunningJob& job, const ExitInfo& info) {
  uint64_t peak = job_peak_kb(job.handle);
  if (peak > job.peak_kb) job.peak_kb = peak;
  json failure = build_failure_json(job, info);
  std::string kind = json_str(failure, "failure_kind", "NONZERO_EXIT");
  bool killed_by_person = job.kill_reason == "USER_KILLED" || job.kill_reason == "SHUTDOWN";
  std::string state = killed_by_person ? "KILLED" : "EXITED";
  int64_t now = now_sec();
  context_.db.run(
      "UPDATE tasks SET state = ?, ended_at = ?, exit_code = ?, signal = ?, failure_kind = ?, failure_json = ?, "
      "peak_rss_kb = ?, updated_at = ? WHERE id = ?",
      {state, now, info.exit_code, info.signal, kind, dump_json(failure), static_cast<int64_t>(job.peak_kb), now,
       job.task_id});
  if (job.kind == "script") {
    json row = get_row(job.task_id);
    std::string sha = json_str(row, "sha256");
    bool ok = kind == "OK";
    if (ok || !killed_by_person) {
      context_.db.run(
          "INSERT INTO hash_stats(sha256, failures, successes, allow_runs) VALUES (?, ?, ?, 0) "
          "ON CONFLICT(sha256) DO UPDATE SET failures = failures + excluded.failures, "
          "successes = successes + excluded.successes",
          {sha, ok ? 0 : 1, ok ? 1 : 0});
    }
  }
  release_job(job.handle);
  emit("finished", job.task_id);
}

void TaskManager::perform_delete(const json& row) {
  std::string task_id = json_str(row, "id");
  json payload = json::parse(json_str(row, "payload_json", "{}"), nullptr, false);
  std::string target = json_str(payload, "path");
  JailResult jail = resolve_in_workspace(context_.paths.workspace, target, true);
  json failure;
  int64_t now = now_sec();
  context_.db.run("UPDATE tasks SET state = 'RUNNING', started_at = ?, updated_at = ? WHERE id = ?", {now, now, task_id});
  if (!jail.ok || jail.relative.empty()) {
    failure = {{"failure_kind", "BAD_PATH"}, {"summary", jail.ok ? "refusing to delete the whole workspace" : jail.error}};
  } else {
    // Move to ~/.maas/trash instead of erasing, so a mistake can be undone.
    fs::path destination = context_.paths.trash / (std::to_string(now) + "-" + task_id) / u8path(jail.relative);
    ensure_dir(destination.parent_path());
    std::error_code error;
    fs::rename(jail.path, destination, error);
    if (error) {
      failure = {{"failure_kind", "DELETE_FAILED"}, {"summary", error.message()}};
    } else {
      failure = {{"failure_kind", "OK"},
                 {"summary", "moved " + jail.relative + " to the node's trash (maas clean empties it)"}};
    }
  }
  context_.db.run("UPDATE tasks SET state = 'EXITED', failure_kind = ?, failure_json = ?, ended_at = ?, updated_at = ? "
                  "WHERE id = ?",
                  {json_str(failure, "failure_kind"), dump_json(failure), now_sec(), now_sec(), task_id});
  emit("finished", task_id);
}

// ---------------------------------------------------------------- views

json TaskManager::view_by_id(const std::string& task_id) {
  json row = get_row(task_id);
  if (row.is_null()) return nullptr;
  return view(row);
}

json TaskManager::view(const json& row) {
  json result;
  std::string task_id = json_str(row, "id");
  std::string kind = json_str(row, "kind");
  std::string state = json_str(row, "state");
  result["task_id"] = task_id;
  result["kind"] = kind;
  result["state"] = state;
  result["name"] = json_str(row, "display_name");
  result["created_at"] = iso_time(json_int(row, "created_at"));
  json payload = json::parse(json_str(row, "payload_json", "{}"), nullptr, false);
  if (kind == "script") {
    result["interpreter"] = json_str(row, "interpreter");
    json args = json::parse(json_str(row, "args_json", "[]"), nullptr, false);
    result["args"] = args.is_array() ? args : json::array();
    result["sha256"] = json_str(row, "sha256").substr(0, 12);
    result["memory_limit_mb"] = json_int(row, "memory_mb");
    result["timeout_sec"] = json_int(row, "timeout_sec");
  } else if (kind == "install") {
    result["manager"] = json_str(payload, "manager", "pip");
    result["packages"] = payload.value("packages", json::array());
  } else if (kind == "delete") {
    result["path"] = json_str(payload, "path");
  } else if (kind == "ingest") {
    result["dataset"] = json_str(payload, "name");
  }
  int64_t started = json_int(row, "started_at");
  int64_t ended = json_int(row, "ended_at");
  if (started > 0) result["started_at"] = iso_time(started);
  if (ended > 0) result["ended_at"] = iso_time(ended);
  if (started > 0) result["duration_sec"] = (ended > 0 ? ended : now_sec()) - started;
  std::string log_path = json_str(row, "log_path");
  if (!log_path.empty()) result["log_bytes"] = file_size_or_zero(u8path(log_path));
  std::string progress = json_str(row, "progress_json");
  if (!progress.empty()) {
    json parsed = json::parse(progress, nullptr, false);
    if (!parsed.is_discarded()) result["progress"] = parsed;
  }
  std::string decided_by = json_str(row, "decided_by");
  if (!decided_by.empty()) result["decided_by"] = decided_by;

  if (state == "PENDING") {
    int64_t left = json_int(row, "approval_deadline") - now_sec();
    result["approval_expires_in_sec"] = left > 0 ? left : 0;
    result["hint"] = "Waiting for the human to approve this on their phone, the node's terminal or Telegram. Tell "
                     "them it is waiting, then call get_task with wait_sec to wait for the decision.";
  } else if (state == "APPROVED") {
    result["hint"] = paused() ? "Approved, but the node is paused; it starts when the human resumes."
                              : "Approved and queued; it starts as soon as a job slot is free.";
  } else if (state == "DENIED") {
    result["hint"] = "The human denied this. Do not resend the same request; ask them what they want changed.";
  } else if (state == "EXPIRED") {
    result["hint"] = "Nobody decided in time, so it was not run. Ask the user to watch for the approval, then try "
                     "again.";
  } else if (state == "RUNNING") {
    uint64_t rss = 0;
    {
      std::lock_guard<std::mutex> lock(running_mutex_);
      auto found = running_.find(task_id);
      if (found != running_.end()) rss = found->second.peak_kb;
    }
    if (rss > 0) result["peak_memory_mb_so_far"] = static_cast<int64_t>(rss / 1024);
    result["hint"] = "Running. Use read_process_logs to watch output, or get_task with wait_sec to wait for it to end.";
  } else {
    if (!row["exit_code"].is_null()) result["exit_code"] = json_int(row, "exit_code");
    result["failure_kind"] = json_str(row, "failure_kind", "UNKNOWN");
    json failure = json::parse(json_str(row, "failure_json", "{}"), nullptr, false);
    if (failure.is_object()) result["result"] = failure;
    int64_t peak = json_int(row, "peak_rss_kb");
    if (peak > 0) result["peak_memory_mb"] = peak / 1024;
  }
  return result;
}

json TaskManager::list(int limit, const std::string& state_filter) {
  std::vector<json> rows;
  if (state_filter.empty()) {
    rows = context_.db.query("SELECT * FROM tasks ORDER BY created_at DESC LIMIT ?", {limit});
  } else {
    rows = context_.db.query("SELECT * FROM tasks WHERE state = ? ORDER BY created_at DESC LIMIT ?",
                             {upper_state(state_filter), limit});
  }
  json result = json::array();
  for (const json& row : rows) result.push_back(view(row));
  return result;
}

json TaskManager::approval_view(const json& row) {
  json result;
  std::string kind = json_str(row, "kind");
  result["id"] = json_str(row, "id");
  result["kind"] = kind;
  result["state"] = json_str(row, "state");
  result["name"] = json_str(row, "display_name");
  result["requested_by"] = json_str(row, "requested_by");
  result["created_at"] = json_int(row, "created_at");
  int64_t left = json_int(row, "approval_deadline") - now_sec();
  result["expires_in_sec"] = left > 0 ? left : 0;
  result["timeout_sec"] = json_int(row, "timeout_sec");
  result["memory_mb"] = json_int(row, "memory_mb");
  std::string explanation = json_str(row, "explanation");
  if (!explanation.empty()) result["ai_says"] = explanation;
  json payload = json::parse(json_str(row, "payload_json", "{}"), nullptr, false);
  if (kind == "script") {
    std::string content;
    read_file(u8path(json_str(row, "stored_path")), content, 2 * 1024 * 1024);
    // Everything shown here comes from the real bytes, never from the AI's
    // description of them.
    result["interpreter"] = json_str(row, "interpreter");
    json args = json::parse(json_str(row, "args_json", "[]"), nullptr, false);
    result["args"] = args.is_array() ? args : json::array();
    result["sha256"] = json_str(row, "sha256");
    result["size"] = json_int(row, "size_bytes");
    std::vector<std::string> lines = split(content, '\n');
    result["lines"] = lines.size();
    result["source_path"] = json_str(row, "source_path");
    std::string safe = sanitize_utf8(content);
    result["content"] = safe.size() > 65536 ? safe.substr(0, utf8_safe_length(safe, 65536)) + "\n... (truncated)" : safe;
    std::string preview;
    for (size_t index = 0; index < lines.size() && index < 25; index++) preview += lines[index] + "\n";
    result["preview"] = sanitize_utf8(preview);
    json previous = context_.db.query_one("SELECT sha256, stored_path FROM last_approved WHERE source_path = ?",
                                          {json_str(row, "source_path")});
    if (!previous.is_null() && json_str(previous, "sha256") != json_str(row, "sha256")) {
      std::string old_content;
      if (read_file(u8path(json_str(previous, "stored_path")), old_content, 2 * 1024 * 1024)) {
        result["previous_sha256"] = json_str(previous, "sha256");
        result["diff"] = sanitize_utf8(line_diff(old_content, content, 120));
      }
    }
    json stats = context_.db.query_one("SELECT failures FROM hash_stats WHERE sha256 = ?", {json_str(row, "sha256")});
    if (!stats.is_null() && json_int(stats, "failures") > 0) result["previous_failures"] = json_int(stats, "failures");
  } else if (kind == "install") {
    result["manager"] = json_str(payload, "manager", "pip");
    result["packages"] = payload.value("packages", json::array());
    result["allowlisted"] = payload.value("allowlisted", false);
  } else if (kind == "delete") {
    result["path"] = json_str(payload, "path");
    result["size"] = json_int(payload, "size");
  }
  return result;
}

}  // namespace maas
