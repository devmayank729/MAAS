#include "config.hpp"

#include "platform.hpp"

namespace maas {

fs::path default_maas_home() {
  std::string override_home = getenv_str("MAAS_HOME");
  if (!override_home.empty()) return u8path(override_home);
  return home_dir() / ".maas";
}

Paths make_paths(const fs::path& home) {
  Paths paths;
  paths.home = home;
  paths.bin = home / "bin";
  paths.config_dir = home / "config";
  paths.state_dir = home / "state";
  paths.workspace = home / "workspace";
  paths.uploads = paths.workspace / "uploads";
  paths.logs = home / "logs";
  paths.task_logs = home / "logs" / "tasks";
  paths.datasets = home / "datasets";
  paths.store = home / "store";
  paths.runtimes = home / "runtimes";
  paths.site_packages = home / "site-packages";
  paths.node_packages = home / "node-packages";
  paths.trash = home / "trash";
  return paths;
}

void create_layout(const Paths& paths) {
  const fs::path folders[] = {paths.home,     paths.bin,       paths.config_dir,    paths.state_dir,
                              paths.workspace, paths.uploads,  paths.logs,          paths.task_logs,
                              paths.datasets, paths.store,     paths.runtimes,      paths.site_packages,
                              paths.node_packages, paths.trash};
  for (const fs::path& folder : folders) ensure_dir(folder);
  set_private_permissions(paths.home);
  set_private_permissions(paths.config_dir);
  set_private_permissions(paths.state_dir);
}

std::string sanitize_node_name(const std::string& name) {
  std::string result;
  for (char character : to_lower(name)) {
    bool allowed = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
    if (allowed) result.push_back(character);
    else if (!result.empty() && result.back() != '-') result.push_back('-');
    if (result.size() >= 40) break;
  }
  while (!result.empty() && result.back() == '-') result.pop_back();
  if (result.empty()) result = "maas-node";
  return result;
}

json config_to_json(const Config& config) {
  json value;
  value["name"] = config.name;
  value["port"] = config.port;
  value["bind"] = config.bind;
  value["allow_public"] = config.allow_public;
  value["approve"] = config.approve;
  value["approval_timeout_sec"] = config.approval_timeout_sec;
  value["max_concurrent_jobs"] = config.max_concurrent_jobs;
  value["default_timeout_sec"] = config.default_timeout_sec;
  value["max_timeout_sec"] = config.max_timeout_sec;
  value["default_memory_mb"] = config.default_memory_mb;
  value["max_log_mb"] = config.max_log_mb;
  value["max_file_mb"] = config.max_file_mb;
  value["disk_floor_mb"] = config.disk_floor_mb;
  value["max_failures_per_hash"] = config.max_failures_per_hash;
  value["max_executions_per_hour"] = config.max_executions_per_hour;
  value["package_allowlist"] = config.package_allowlist;
  value["python"] = config.python;
  value["mdns"] = config.mdns;
  return value;
}

// Turns a byte offset from the JSON parser into "line X, column Y".
static std::string line_and_column(const std::string& text, size_t offset) {
  size_t line = 1, column = 1;
  for (size_t index = 0; index < text.size() && index < offset; index++) {
    if (text[index] == '\n') {
      line++;
      column = 1;
    } else {
      column++;
    }
  }
  return "line " + std::to_string(line) + ", column " + std::to_string(column);
}

namespace {

// Small schema checker: every known key has a type and an allowed range.
struct Checker {
  const json& source;
  std::vector<std::string>& problems;

  bool get_int(const char* key, int64_t& target, int64_t minimum, int64_t maximum) {
    auto found = source.find(key);
    if (found == source.end()) return true;
    if (!found->is_number_integer()) {
      problems.push_back(std::string("\"") + key + "\" must be a whole number, got " + found->type_name());
      return false;
    }
    int64_t value = found->get<int64_t>();
    if (value < minimum || value > maximum) {
      problems.push_back(std::string("\"") + key + "\" must be between " + std::to_string(minimum) + " and " +
                         std::to_string(maximum) + ", got " + std::to_string(value));
      return false;
    }
    target = value;
    return true;
  }
  bool get_int(const char* key, int& target, int64_t minimum, int64_t maximum) {
    int64_t value = target;
    bool ok = get_int(key, value, minimum, maximum);
    target = static_cast<int>(value);
    return ok;
  }
  bool get_bool(const char* key, bool& target) {
    auto found = source.find(key);
    if (found == source.end()) return true;
    if (!found->is_boolean()) {
      problems.push_back(std::string("\"") + key + "\" must be true or false, got " + found->type_name());
      return false;
    }
    target = found->get<bool>();
    return true;
  }
  bool get_string(const char* key, std::string& target) {
    auto found = source.find(key);
    if (found == source.end()) return true;
    if (!found->is_string()) {
      problems.push_back(std::string("\"") + key + "\" must be text in quotes, got " + found->type_name());
      return false;
    }
    target = found->get<std::string>();
    return true;
  }
};

}  // namespace

bool load_config(const Paths& paths, Config& config, std::vector<std::string>& warnings, std::string& error) {
  fs::path file = paths.config_file();
  std::error_code fs_error;
  if (!fs::exists(file, fs_error)) {
    config.name = sanitize_node_name("maas-node");
    write_file_atomic(file, dump_json(config_to_json(config), 2) + "\n", true);
    return true;
  }
  std::string text;
  if (!read_file(file, text, 1024 * 1024)) {
    error = "cannot read " + path_str(file);
    return false;
  }
  json source;
  try {
    source = json::parse(text);
  } catch (const json::parse_error& parse_problem) {
    error = path_str(file) + " is not valid JSON at " + line_and_column(text, parse_problem.byte) +
            ". Fix it, or delete the file to get fresh defaults.";
    return false;
  }
  if (!source.is_object()) {
    error = path_str(file) + " must contain a JSON object { ... }";
    return false;
  }
  const char* known_keys[] = {"name",
                              "port",
                              "bind",
                              "allow_public",
                              "approve",
                              "approval_timeout_sec",
                              "max_concurrent_jobs",
                              "default_timeout_sec",
                              "max_timeout_sec",
                              "default_memory_mb",
                              "max_log_mb",
                              "max_file_mb",
                              "disk_floor_mb",
                              "max_failures_per_hash",
                              "max_executions_per_hour",
                              "package_allowlist",
                              "python",
                              "mdns"};
  for (auto item = source.begin(); item != source.end(); ++item) {
    bool known = false;
    for (const char* key : known_keys) {
      if (item.key() == key) known = true;
    }
    if (!known) warnings.push_back("config: unknown key \"" + item.key() + "\" is ignored (typo?)");
  }
  std::vector<std::string> problems;
  Checker checker{source, problems};
  checker.get_string("name", config.name);
  checker.get_int("port", config.port, 1, 65535);
  checker.get_string("bind", config.bind);
  checker.get_bool("allow_public", config.allow_public);
  checker.get_string("approve", config.approve);
  checker.get_int("approval_timeout_sec", config.approval_timeout_sec, 10, 86400);
  checker.get_int("max_concurrent_jobs", config.max_concurrent_jobs, 0, 64);
  checker.get_int("default_timeout_sec", config.default_timeout_sec, 1, 7 * 86400);
  checker.get_int("max_timeout_sec", config.max_timeout_sec, 1, 30 * 86400);
  checker.get_int("default_memory_mb", config.default_memory_mb, 0, 1024 * 1024);
  checker.get_int("max_log_mb", config.max_log_mb, 1, 100000);
  checker.get_int("max_file_mb", config.max_file_mb, 1, 10000000);
  checker.get_int("disk_floor_mb", config.disk_floor_mb, 0, 1000000);
  checker.get_int("max_failures_per_hash", config.max_failures_per_hash, 1, 1000);
  checker.get_int("max_executions_per_hour", config.max_executions_per_hour, 1, 100000);
  checker.get_string("python", config.python);
  checker.get_bool("mdns", config.mdns);
  auto allowlist = source.find("package_allowlist");
  if (allowlist != source.end()) {
    if (!allowlist->is_array()) {
      problems.push_back("\"package_allowlist\" must be a list like [\"pandas\", \"numpy\"]");
    } else {
      config.package_allowlist.clear();
      for (const json& item : *allowlist) {
        if (item.is_string()) config.package_allowlist.push_back(to_lower(item.get<std::string>()));
        else problems.push_back("\"package_allowlist\" may only contain package names in quotes");
      }
    }
  }
  const std::string approve_modes[] = {"auto", "terminal", "phone", "telegram"};
  bool approve_ok = false;
  for (const std::string& mode : approve_modes) {
    if (config.approve == mode) approve_ok = true;
  }
  if (!approve_ok) problems.push_back("\"approve\" must be one of auto, terminal, phone, telegram");
  if (config.default_timeout_sec > config.max_timeout_sec) {
    problems.push_back("\"default_timeout_sec\" cannot be larger than \"max_timeout_sec\"");
  }
  std::string clean_name = sanitize_node_name(config.name);
  if (clean_name != config.name) {
    warnings.push_back("config: node name \"" + config.name + "\" was changed to \"" + clean_name +
                       "\" (only a-z, 0-9 and - work on the network)");
    config.name = clean_name;
  }
  if (!problems.empty()) {
    error = "problems in " + path_str(file) + ":\n  - " + join(problems, "\n  - ");
    return false;
  }
  return true;
}

}  // namespace maas
