// Folder layout and the node's config file (~/.maas/config/maas.json).
#pragma once

#include "common.hpp"

namespace maas {

struct Paths {
  fs::path home;           // ~/.maas (or $MAAS_HOME)
  fs::path bin;            // installed binary
  fs::path config_dir;     // maas.json, telegram.json, clients.json
  fs::path state_dir;      // maas_state.db, lock, local secret
  fs::path workspace;      // the only folder the AI can read and write
  fs::path uploads;        // workspace/uploads, the drop zone
  fs::path logs;           // daemon log
  fs::path task_logs;      // one log file per job
  fs::path datasets;       // one SQLite file per dataset
  fs::path store;          // content-addressed copies of approved scripts
  fs::path runtimes;       // portable Python downloaded into user space
  fs::path site_packages;  // pip install --target goes here
  fs::path node_packages;  // npm install --prefix goes here
  fs::path trash;          // delete_file moves files here

  fs::path state_db() const { return state_dir / "maas_state.db"; }
  fs::path lock_file() const { return state_dir / "maas.lock"; }
  fs::path config_file() const { return config_dir / "maas.json"; }
  fs::path telegram_file() const { return config_dir / "telegram.json"; }
  fs::path clients_file() const { return config_dir / "clients.json"; }
  fs::path local_secret_file() const { return state_dir / "local.secret"; }
  fs::path runtime_file() const { return state_dir / "running.json"; }
  fs::path daemon_log() const { return logs / "maas.log"; }
};

fs::path default_maas_home();
Paths make_paths(const fs::path& home);
void create_layout(const Paths& paths);

struct Config {
  std::string name = "maas-node";
  int port = 7420;
  std::string bind = "0.0.0.0";
  bool allow_public = false;           // accept non-private source addresses
  std::string approve = "auto";        // auto | terminal | phone | telegram
  int approval_timeout_sec = 300;      // pending approvals are denied after this
  int max_concurrent_jobs = 0;         // 0 = pick from CPU count
  int default_timeout_sec = 900;
  int max_timeout_sec = 86400;
  int64_t default_memory_mb = 0;       // 0 = pick from RAM size
  int64_t max_log_mb = 200;
  int64_t max_file_mb = 8192;          // RLIMIT_FSIZE for jobs
  int64_t disk_floor_mb = 500;         // no new jobs or uploads below this free space
  int max_failures_per_hash = 2;       // refuse a script that already failed this often
  int max_executions_per_hour = 60;
  std::vector<std::string> package_allowlist;  // empty = any package (still needs approval)
  std::string python;                  // optional explicit interpreter path
  bool mdns = true;
};

// Reads the config file, creating it with defaults on first run. Problems
// that can be ignored become warnings; real mistakes (wrong types, bad
// values, broken JSON) become `error` with the line and column.
bool load_config(const Paths& paths, Config& config, std::vector<std::string>& warnings, std::string& error);
json config_to_json(const Config& config);
std::string sanitize_node_name(const std::string& name);

}  // namespace maas
