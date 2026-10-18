// MAAS entry point.
//   maas / maas start   run this machine as a node (prints QR + pairing codes)
//   maas pair           new pairing codes for a running node
//   maas status | stop  talk to the running node on this machine
//   maas version
#include <condition_variable>
#include <thread>

#include "console.hpp"
#include "httplib.h"
#include "ingest.hpp"
#include "net.hpp"
#include "qr.hpp"
#include "server.hpp"
#include "tasks.hpp"
#include "telegram.hpp"

using namespace maas;

static void print_usage() {
  console_line("MAAS " + std::string(MAAS_VERSION) + " - turn this computer into an AI-operable node\n\n"
               "  maas start [--port N] [--ephemeral]   run this machine as a node (default)\n"
               "  maas pair                             show fresh pairing codes for the running node\n"
               "  maas status                           show the running node's jobs\n"
               "  maas stop                             stop the running node\n"
               "  maas runtime [install python]         list runtimes, or fetch a portable Python (no admin)\n"
               "  maas version\n");
}

static bool has_flag(const std::vector<std::string>& args, const std::string& flag) {
  for (const std::string& item : args) {
    if (item == flag) return true;
  }
  return false;
}

static std::string flag_value(const std::vector<std::string>& args, const std::string& flag) {
  for (size_t index = 0; index + 1 < args.size(); index++) {
    if (args[index] == flag) return args[index + 1];
  }
  return "";
}

// Prints the QR (phone pairing) and the client pairing command.
static void print_pairing(const std::string& base_url, const std::string& mdns_url, const std::string& phone_code,
                          const std::string& client_code) {
  std::string pair_url = base_url + "/pair#k=" + phone_code;
  console_line("\n  " + c_bold("1. Scan with your phone") + " to approve jobs (phone on the same Wi-Fi):");
  console_print(render_qr(pair_url));
  console_line("  " + c_dim(pair_url));
  console_line("\n  " + c_bold("2. Connect your AI client") + " (Claude Code) from your main laptop:");
  console_line("     curl -s -X POST " + base_url + "/api/pair -H 'Content-Type: application/json' -d '{\"code\":\"" +
               client_code + "\"}'");
  console_line(c_dim("     then: claude mcp add --transport http maas-node " + base_url +
                     "/mcp --header \"Authorization: Bearer <token>\""));
  if (!mdns_url.empty()) console_line(c_dim("     (also reachable as " + mdns_url + " where mDNS works)"));
  console_line(c_dim("     Codes work once; phone code 30 min, client code 15 min. `maas pair` makes new ones."));
}

static json read_running(const Paths& paths) {
  std::string text;
  if (!read_file(paths.runtime_file(), text, 65536)) return nullptr;
  json value = json::parse(text, nullptr, false);
  return value.is_object() ? value : json(nullptr);
}

static bool local_call(const Paths& paths, const std::string& method, const std::string& path, json& out) {
  json running = read_running(paths);
  std::string secret;
  if (running.is_null() || !read_file(paths.local_secret_file(), secret, 256)) return false;
  httplib::Client client("127.0.0.1", static_cast<int>(json_int(running, "port", MAAS_DEFAULT_PORT)));
  client.set_connection_timeout(3, 0);
  client.set_read_timeout(10, 0);
  httplib::Headers headers = {{"X-Maas-Local", trim(secret)}};
  httplib::Result result = method == "POST" ? client.Post(path, headers, "{}", "application/json") : client.Get(path, headers);
  if (!result || result->status != 200) return false;
  out = json::parse(result->body, nullptr, false);
  return true;
}

static int command_status(const Paths& paths) {
  json status;
  if (!local_call(paths, "GET", "/api/local/status", status)) {
    console_line("No MAAS node is running on this machine. Start one with: maas start");
    return 1;
  }
  console_line(c_bold(json_str(status, "name")) + "  " + dump_json(status["urls"]) + "  uptime " +
               format_duration(static_cast<double>(json_int(status, "uptime_sec"))) +
               (json_bool(status, "paused") ? c_yellow("  PAUSED") : ""));
  console_line("pending approvals: " + std::to_string(status["pending"].size()) +
               ", running: " + std::to_string(json_int(status, "running")));
  for (const json& task : status["tasks"]) {
    std::string state = json_str(task, "state");
    if (state == "EXITED") state = json_str(task, "failure_kind");
    console_line("  " + json_str(task, "task_id") + "  " + state + "  " + json_str(task, "name"));
  }
  return 0;
}

static int command_stop(const Paths& paths) {
  json reply;
  if (!local_call(paths, "POST", "/api/local/stop", reply)) {
    console_line("No MAAS node is running on this machine.");
    return 1;
  }
  console_line("Stopping the node.");
  return 0;
}

static int command_pair(const Paths& paths) {
  json running = read_running(paths);
  if (running.is_null() || !lock_is_held(paths.lock_file())) {
    console_line("No MAAS node is running on this machine. Start one with: maas start");
    return 1;
  }
  Db db;
  std::string error;
  if (!db.open(paths.state_db(), error)) {
    console_error("cannot open state database: " + error);
    return 1;
  }
  Auth auth(db);
  print_pairing(json_str(running, "url"), json_str(running, "mdns_url"), auth.create_pair_code("approver", 1800),
                auth.create_pair_code("client", 900));
  return 0;
}

static int command_start(const std::vector<std::string>& args) {
  Context context;
  context.ephemeral = has_flag(args, "--ephemeral");
  fs::path home = default_maas_home();
  if (context.ephemeral) home = fs::temp_directory_path() / ("maas-ephemeral-" + random_hex(4));
  context.paths = make_paths(home);
  create_layout(context.paths);

  std::vector<std::string> warnings;
  std::string error;
  if (!load_config(context.paths, context.config, warnings, error)) {
    console_error(c_red("✗ ") + error);
    return 2;
  }
  std::string port_text = flag_value(args, "--port");
  if (!port_text.empty()) {
    int64_t port = 0;
    if (!parse_int64(port_text, port) || port < 1 || port > 65535) {
      console_error("--port must be a number from 1 to 65535");
      return 2;
    }
    context.config.port = static_cast<int>(port);
  }

  InstanceLock lock;
  if (!lock.acquire(context.paths.lock_file())) {
    json running = read_running(context.paths);
    console_line("MAAS is already running on this machine" +
                 (running.is_null() ? std::string(".") : " at " + json_str(running, "url")) +
                 "\nUse `maas status`, `maas pair` or `maas stop`.");
    return 0;
  }
  context.service_mode = has_flag(args, "--service") || !is_stdin_tty();
  log_init(context.paths.daemon_log(), has_flag(args, "--verbose"), true);
  for (const std::string& warning : warnings) log_warn(warning);
  if (is_running_as_admin()) log_warn("running as root/administrator is not needed and not recommended");
  if (is_wsl()) {
    log_warn("WSL detected: other devices usually cannot reach WSL. Run the Windows build of maas instead.");
  }

  if (!context.db.open(context.paths.state_db(), error) || !migrate_state_db(context.db, error)) {
    console_error(c_red("✗ state database: ") + error + "\n  (`maas` keeps it in " + path_str(context.paths.state_dir) +
                  ")");
    return 2;
  }
  context.auth.reset(new Auth(context.db));
  context.local_secret = random_hex(24);
  write_file_atomic(context.paths.local_secret_file(), context.local_secret, true);
  context.node.node_id = meta_get(context.db, "node_id");
  if (context.node.node_id.empty()) {
    context.node.node_id = random_hex(16);
    meta_set(context.db, "node_id", context.node.node_id);
  }
  context.node.name = context.config.name;
  context.node.started_at = now_sec();
  int64_t year = std::stoll(iso_time(now_sec()).substr(0, 4));
  if (year < 2025) log_warn("the system clock says " + std::to_string(year) + "; fix the date or HTTPS (pip) will fail");

  // Limits sized to this machine.
  MemInfo memory = mem_info();
  int64_t total_mb = static_cast<int64_t>(memory.total_kb / 1024);
  if (total_mb <= 0) total_mb = 2048;
  context.max_memory_mb = total_mb * 85 / 100;
  context.default_memory_mb = context.config.default_memory_mb > 0 ? context.config.default_memory_mb : total_mb * 60 / 100;
  if (context.default_memory_mb > context.max_memory_mb) context.default_memory_mb = context.max_memory_mb;
  context.max_concurrent_jobs = context.config.max_concurrent_jobs > 0 ? context.config.max_concurrent_jobs
                                                                       : (cpu_cores() <= 2 ? 1 : 2);

  AddressChoice addresses = choose_addresses();
  context.node.primary_ip = addresses.primary;
  context.node.all_ips = addresses.others;
  if (!addresses.warning.empty()) log_warn(addresses.warning);

  context.tasks.reset(new TaskManager(context));
  context.datasets.reset(new DatasetManager(context));
  DatasetManager* datasets = context.datasets.get();
  context.tasks->internal_body_factory = [datasets](const json& row) { return datasets->make_ingest_body(row); };
  context.tasks->recover_after_restart();

  Console console(context);
  context.console = &console;
  context.terminal_approvals = !context.service_mode && context.config.approve != "phone";
  context.tasks->add_observer(&console);

  // Runtime detection runs subprocesses; do it in the background.
  std::thread runtime_thread([&context]() { context.runtimes.discover(context.paths, context.config); });

  HttpServer server(context);
  int port = 0;
  if (!server.bind(context.config.port, port, error)) {
    console_error(c_red("✗ ") + error);
    runtime_thread.join();
    return 2;
  }
  if (port != context.config.port) {
    log_warn("port " + std::to_string(context.config.port) + " is in use; using " + std::to_string(port) +
             " (re-pair clients if they used the old port)");
  }
  context.node.port = port;
  write_file_atomic(context.paths.runtime_file(),
                    dump_json({{"url", context.base_url()},
                               {"mdns_url", context.config.mdns ? context.mdns_url() : ""},
                               {"port", port},
                               {"name", context.node.name}}),
                    true);

  MdnsResponder mdns;
  if (context.config.mdns && !is_loopback_address(context.node.primary_ip)) {
    MdnsRecordInfo info;
    info.name = context.node.name;
    info.ip = context.node.primary_ip;
    info.port = port;
    info.txt = {"v=1", "id=" + context.node.node_id.substr(0, 8), "ram=" + std::to_string(total_mb),
                "cores=" + std::to_string(cpu_cores()), "avx2=" + std::string(cpu_features().avx2 ? "1" : "0"),
                "os=" + os_name()};
    std::string mdns_error;
    if (!mdns.start(info, mdns_error)) log_warn("mDNS name disabled: " + mdns_error);
  }

  context.tasks->start();

  std::mutex done_mutex;
  std::condition_variable done_cv;
  bool done = false;
  context.request_shutdown = [&]() {
    if (context.shutting_down.exchange(true)) return;
    std::lock_guard<std::mutex> guard(done_mutex);
    done = true;
    done_cv.notify_all();
  };
  install_shutdown_handler(context.request_shutdown);

  std::thread server_thread([&server]() { server.run(); });

  CpuFeatures features = cpu_features();
  console_line("\n  " + c_bold("MAAS node \"" + context.node.name + "\" is running") + c_dim(std::string("  v") + MAAS_VERSION));
  console_line("  " + os_pretty() + ", " + cpu_model() + " (" + std::to_string(cpu_cores()) + " threads" +
               (features.avx2 ? ", AVX2" : features.avx ? ", AVX, no AVX2" : "") + "), " + std::to_string(total_mb) +
               " MB RAM, " + human_bytes(free_disk_bytes(context.paths.home)) + " free");
  console_line("  Address  " + c_cyan(context.base_url()));
  for (const std::string& other : addresses.others) console_line(c_dim("           also http://" + other + ":" + std::to_string(port)));
  console_line("  Limits   " + std::to_string(context.default_memory_mb) + " MB per job, " +
               std::to_string(context.max_concurrent_jobs) + " job(s) at a time, approvals expire after " +
               std::to_string(context.config.approval_timeout_sec / 60) + " min");
  if (context.ephemeral) console_line(c_yellow("  Ephemeral: everything is deleted when you stop the node."));

  std::string base = context.base_url();
  std::string mdns_url = context.config.mdns ? context.mdns_url() : "";
  Auth* auth = context.auth.get();
  console.show_pairing = [base, mdns_url, auth]() {
    print_pairing(base, mdns_url, auth->create_pair_code("approver", 1800), auth->create_pair_code("client", 900));
  };
  console.show_pairing();
  console.start(context.terminal_approvals);
  console.print_help();

  {
    std::unique_lock<std::mutex> guard(done_mutex);
    done_cv.wait(guard, [&]() { return done; });
  }
  console_line("\n  Stopping: ending jobs, saying goodbye on the network...");
  server.stop();
  if (server_thread.joinable()) server_thread.join();
  context.tasks->stop();
  mdns.stop();
  if (runtime_thread.joinable()) runtime_thread.join();
  std::error_code fs_error;
  fs::remove(context.paths.runtime_file(), fs_error);
  fs::remove(context.paths.local_secret_file(), fs_error);
  context.db.close();
  lock.release();
  if (context.ephemeral) fs::remove_all(context.paths.home, fs_error);
  console_line("  Stopped.");
  return 0;
}

int main(int argc, char** argv) {
  // The job wrapper must run before anything else touches the process.
  if (argc >= 2 && std::string(argv[1]) == "__exec") return exec_wrapper_main(argc, argv);
  console_setup();
  std::vector<std::string> args;
  for (int index = 2; index < argc; index++) args.push_back(argv[index]);
  std::string command = argc >= 2 ? argv[1] : "start";
  if (starts_with(command, "--")) {
    args.insert(args.begin(), command);
    command = "start";
  }
  try {
    Paths paths = make_paths(default_maas_home());
    if (command == "start") return command_start(args);
    if (command == "pair") return command_pair(paths);
    if (command == "status") return command_status(paths);
    if (command == "stop") return command_stop(paths);
    if (command == "telegram") return telegram_setup_command(paths, args);
    if (command == "runtime") {
      if (args.size() >= 2 && args[0] == "install" && args[1] == "python") {
        create_layout(paths);
        std::string error;
        if (!install_portable_python(paths, error, true)) {
          console_error(c_red("✗ ") + error);
          return 1;
        }
        console_line(c_green("✓ Portable Python installed in ") + path_str(paths.runtimes / "python") +
                     ". Restart the node (maas stop, then maas start) to use it.");
        return 0;
      }
      Config config;
      std::vector<std::string> warnings;
      std::string error;
      load_config(paths, config, warnings, error);
      Runtimes runtimes;
      runtimes.discover(paths, config);
      console_line(dump_json(runtimes.to_json(), 2));
      return 0;
    }
    if (command == "version" || command == "--version") {
      console_line(std::string("maas ") + MAAS_VERSION + " (" + os_name() + "/" + arch_name() + ")");
      return 0;
    }
    if (command == "help" || command == "-h" || command == "--help") {
      print_usage();
      return 0;
    }
  } catch (const std::exception& problem) {
    console_error(std::string("maas: ") + problem.what());
    return 1;
  }
  console_error("unknown command: " + command + "\n");
  print_usage();
  return 2;
}
