// Tasks: scripts, package installs, deletions and dataset ingestion.
//
// Approval state machine (only the daemon moves states, every move is stored):
//   PENDING -> APPROVED | DENIED | EXPIRED
//   APPROVED -> RUNNING -> EXITED | KILLED
//   anything in flight when the daemon died -> LOST (never re-run automatically)
#pragma once

#include <condition_variable>
#include <thread>

#include "failures.hpp"
#include "node.hpp"
#include "platform.hpp"

namespace maas {

class TaskObserver {
 public:
  virtual ~TaskObserver() = default;
  virtual void on_pending(const json& approval) { (void)approval; }  // a human must decide
  virtual void on_decided(const json& task) { (void)task; }
  virtual void on_started(const json& task) { (void)task; }
  virtual void on_finished(const json& task) { (void)task; }
};

struct TaskRequest {
  std::string kind;  // script | install | delete
  std::string display_name;
  std::string source_path;  // workspace-relative path, or "(inline code)"
  std::string content;      // script bytes (script kind)
  std::string interpreter;  // runtime name (script kind)
  std::vector<std::string> args;
  std::string cwd_relative;
  int timeout_sec = 0;
  int64_t memory_mb = 0;
  std::string explanation;   // the AI's own words, shown labelled "AI says"
  std::string requested_by;  // label of the token that asked
  json payload = json::object();
};

struct CreateResult {
  bool ok = false;
  std::string error_kind;
  std::string error;
  json task;
};

// Handed to in-process tasks (dataset ingestion).
struct InternalJob {
  std::string task_id;
  std::atomic<bool> cancel{false};
  std::function<void(const json&)> report_progress;
  std::string failure_kind = "OK";
  json failure = json::object();
};

class TaskManager {
 public:
  explicit TaskManager(Context& context);
  ~TaskManager();

  void start();
  void stop();  // kills running jobs
  void recover_after_restart();

  CreateResult create(const TaskRequest& request);
  std::string run_internal(const std::string& kind, const std::string& display_name, const json& payload,
                           const std::string& requested_by, std::function<void(InternalJob&)> body);

  json get_row(const std::string& task_id);
  json view(const json& row);  // what the AI and the phone see
  json view_by_id(const std::string& task_id);
  json list(int limit, const std::string& state_filter);
  json approval_view(const json& row);  // the daemon-generated approval prompt
  std::vector<json> pending_approvals();

  // Blocks until the task leaves `from_state` or the timeout passes.
  bool wait_for_change(const std::string& task_id, const std::string& from_state, int timeout_ms);
  // approve=false denies. extra_runs pre-approves this exact script N more times.
  json decide(const std::string& task_id, bool approve, int extra_runs, const std::string& actor,
              std::string& error);
  bool terminate(const std::string& task_id, const std::string& actor, std::string& error);
  int kill_all(const std::string& actor);
  void set_paused(bool paused, const std::string& actor);
  bool paused() const { return paused_.load(); }
  int running_count();

  void add_observer(TaskObserver* observer);

  // Approved in-process tasks (replacing a dataset) get their work from here.
  std::function<std::function<void(InternalJob&)>(const json& row)> internal_body_factory;

 private:
  struct RunningJob {
    std::string task_id;
    std::string kind;
    std::string interpreter;
    std::string stored_name;
    ProcHandle handle;
    int64_t started_mono = 0;
    int timeout_sec = 0;
    int64_t memory_mb = 0;
    fs::path log_path;
    std::string kill_reason;
    int64_t kill_sent_mono = 0;
    bool force_sent = false;
    uint64_t peak_kb = 0;
    int64_t last_rss_check = 0;
  };

  void start_internal_for(const std::string& task_id, std::function<void(InternalJob&)> body);
  void supervisor_loop();
  void start_approved();
  bool start_job(const json& row, std::string& error);
  void monitor_jobs();
  void finalize_job(RunningJob& job, const ExitInfo& info);
  void expire_pending();
  void perform_delete(const json& row);
  void notify_changed();
  void emit(const std::string& event, const std::string& task_id);
  json build_failure_json(RunningJob& job, const ExitInfo& info);
  std::string read_log_tail(const fs::path& path, size_t bytes);

  Context& context_;
  std::thread supervisor_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> paused_{false};
  std::mutex running_mutex_;
  std::map<std::string, RunningJob> running_;
  std::mutex internal_mutex_;
  std::map<std::string, std::shared_ptr<InternalJob>> internal_jobs_;
  std::vector<std::thread> internal_threads_;
  std::mutex wait_mutex_;
  std::condition_variable wait_cv_;
  std::mutex observers_mutex_;
  std::vector<TaskObserver*> observers_;
  int64_t last_disk_check_ = 0;
};

std::string new_task_id();

}  // namespace maas
