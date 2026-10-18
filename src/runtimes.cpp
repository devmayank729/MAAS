#include "runtimes.hpp"

#include "platform.hpp"

namespace maas {

fs::path portable_python_path(const Paths& paths) {
#ifdef _WIN32
  return paths.runtimes / "python" / "python.exe";
#else
  return paths.runtimes / "python" / "bin" / "python3";
#endif
}

#ifdef _WIN32
// %LOCALAPPDATA%\Microsoft\WindowsApps\python.exe is usually a stub that opens
// the Microsoft Store instead of running Python. Only accept it when a real
// Store Python package is installed next to it.
static bool is_store_stub(const std::string& path) {
  std::string lower = to_lower(path);
  if (lower.find("\\windowsapps\\") == std::string::npos) return false;
  fs::path folder = u8path(path).parent_path();
  std::error_code error;
  for (fs::directory_iterator item(folder, error); !error && item != fs::directory_iterator(); item.increment(error)) {
    std::string name = to_lower(path_str(item->path().filename()));
    if (starts_with(name, "pythonsoftwarefoundation.python.")) return false;
  }
  return true;
}
#endif

static bool probe_python(const std::string& path, Runtime& out) {
#ifdef _WIN32
  if (is_store_stub(path)) return false;
#endif
  CaptureResult version = run_capture(
      {path, "-c", "import sys;print(sys.version.split()[0]);print(sys.executable)"}, "", 10000);
  if (!version.started || version.exit_code != 0) return false;
  std::vector<std::string> lines = split(trim(version.out), '\n');
  if (lines.empty()) return false;
  std::string number = trim(lines[0]);
  if (starts_with(number, "2.")) return false;  // Python 2 cannot run modern scripts
  out.name = "python";
  out.version = number;
  out.path = lines.size() > 1 && !trim(lines[1]).empty() ? trim(lines[1]) : path;
  CaptureResult pip = run_capture({out.path, "-m", "pip", "--version"}, "", 20000);
  out.has_pip = pip.started && pip.exit_code == 0;
  return true;
}

static bool probe_version(const std::string& name, const std::string& path, const std::vector<std::string>& args,
                          Runtime& out) {
  std::vector<std::string> command = {path};
  for (const std::string& arg : args) command.push_back(arg);
  CaptureResult result = run_capture(command, "", 8000);
  if (!result.started || result.exit_code != 0) return false;
  out.name = name;
  out.path = path;
  std::string text = trim(result.out.empty() ? result.err : result.out);
  out.version = split(text, '\n')[0];
  if (out.version.size() > 80) out.version = out.version.substr(0, 80);
  out.source = "system";
  return true;
}

void Runtimes::discover(const Paths& paths, const Config& config) {
  std::map<std::string, Runtime> found;

  // Python: explicit config, then a portable copy, then the system's.
  std::vector<std::pair<std::string, std::string>> python_candidates;
  if (!config.python.empty()) python_candidates.push_back({config.python, "config"});
  std::error_code error;
  if (fs::exists(portable_python_path(paths), error)) {
    python_candidates.push_back({path_str(portable_python_path(paths)), "portable"});
  }
#ifdef _WIN32
  std::string launcher = find_in_path("py");
  if (!launcher.empty()) {
    CaptureResult located = run_capture({launcher, "-3", "-c", "import sys;print(sys.executable)"}, "", 10000);
    if (located.started && located.exit_code == 0) python_candidates.push_back({trim(located.out), "system"});
  }
  for (const char* name : {"python3", "python"}) {
    std::string path = find_in_path(name);
    if (!path.empty()) python_candidates.push_back({path, "system"});
  }
#else
  for (const char* name : {"python3", "python"}) {
    std::string path = find_in_path(name);
    if (!path.empty()) python_candidates.push_back({path, "system"});
  }
#endif
  Runtime best_python;
  bool have_python = false;
  for (const auto& candidate : python_candidates) {
    Runtime probe;
    if (!probe_python(candidate.first, probe)) continue;
    probe.source = candidate.second;
    // Prefer the first one that has pip; keep the first working one otherwise.
    if (!have_python || (!best_python.has_pip && probe.has_pip)) {
      best_python = probe;
      have_python = true;
    }
    if (best_python.has_pip) break;
  }
  if (have_python) found["python"] = best_python;

  Runtime probe;
  std::string node_path = find_in_path("node");
  if (!node_path.empty() && probe_version("node", node_path, {"--version"}, probe)) {
    probe.npm_path = find_in_path("npm");
    found["node"] = probe;
  }
#ifdef _WIN32
  std::string comspec = getenv_str("ComSpec");
  if (comspec.empty()) comspec = "C:\\Windows\\System32\\cmd.exe";
  Runtime cmd;
  cmd.name = "cmd";
  cmd.path = comspec;
  cmd.version = "Windows command prompt";
  cmd.source = "system";
  found["cmd"] = cmd;
  std::string powershell = find_in_path("powershell");
  if (!powershell.empty()) {
    Runtime shell;
    shell.name = "powershell";
    shell.path = powershell;
    shell.version = "Windows PowerShell";
    shell.source = "system";
    found["powershell"] = shell;
  }
  // Git for Windows ships bash. C:\Windows\System32\bash.exe is the WSL
  // launcher, which would run the script in a different OS, so skip it.
  std::vector<std::string> bash_candidates = {"C:\\Program Files\\Git\\bin\\bash.exe",
                                              "C:\\Program Files (x86)\\Git\\bin\\bash.exe"};
  std::string bash_on_path = find_in_path("bash");
  if (!bash_on_path.empty() && to_lower(bash_on_path).find("\\system32\\") == std::string::npos) {
    bash_candidates.insert(bash_candidates.begin(), bash_on_path);
  }
  for (const std::string& candidate : bash_candidates) {
    if (fs::exists(u8path(candidate), error) && probe_version("bash", candidate, {"--version"}, probe)) {
      found["bash"] = probe;
      break;
    }
  }
#else
  std::string bash_path = find_in_path("bash");
  if (!bash_path.empty() && probe_version("bash", bash_path, {"--version"}, probe)) found["bash"] = probe;
  std::string sh_path = find_in_path("sh");
  if (!sh_path.empty()) {
    Runtime shell;
    shell.name = "sh";
    shell.path = sh_path;
    shell.version = "POSIX sh";
    shell.source = "system";
    found["sh"] = shell;
  }
#endif
  std::string pwsh_path = find_in_path("pwsh");
  if (!pwsh_path.empty() && probe_version("pwsh", pwsh_path, {"--version"}, probe)) found["pwsh"] = probe;
  std::string r_path = find_in_path("Rscript");
  if (!r_path.empty() && probe_version("rscript", r_path, {"--version"}, probe)) found["rscript"] = probe;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    found_ = found;
  }
  ready_.store(true);
}

bool Runtimes::find(const std::string& name, Runtime& out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto item = found_.find(name);
  if (item == found_.end()) {
    // Sensible fallbacks between equivalent shells.
    if (name == "bash") item = found_.find("sh");
    if (name == "powershell") item = found_.find("pwsh");
    if (item == found_.end()) return false;
  }
  out = item->second;
  return true;
}

json Runtimes::to_json() const {
  std::lock_guard<std::mutex> lock(mutex_);
  json result = json::object();
  for (const auto& item : found_) {
    json entry;
    entry["version"] = item.second.version;
    entry["path"] = item.second.path;
    entry["source"] = item.second.source;
    if (item.first == "python") entry["pip"] = item.second.has_pip;
    if (item.first == "node") entry["npm"] = !item.second.npm_path.empty();
    result[item.first] = entry;
  }
  return result;
}

std::string Runtimes::runtime_for_extension(const std::string& extension_in) {
  std::string extension = to_lower(extension_in);
  if (extension == ".py") return "python";
  if (extension == ".js" || extension == ".mjs" || extension == ".cjs") return "node";
  if (extension == ".sh" || extension == ".bash") return "bash";
  if (extension == ".ps1") return "powershell";
  if (extension == ".bat" || extension == ".cmd") return "cmd";
  if (extension == ".r") return "rscript";
  return "";
}

std::vector<std::string> Runtimes::command_prefix(const Runtime& runtime) const {
  if (runtime.name == "python") return {runtime.path, "-u"};  // unbuffered so logs stream live
  if (runtime.name == "powershell" || runtime.name == "pwsh") {
    return {runtime.path, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File"};
  }
  if (runtime.name == "cmd") return {runtime.path, "/d", "/c"};
  return {runtime.path};
}

// ---------------------------------------------------------------- portable Python

static std::string python_build_triple() {
  std::string os = os_name();
  std::string arch = arch_name();
  if (os == "windows") {
    if (arch == "x86_64") return "x86_64-pc-windows-msvc";
    if (arch == "arm64") return "aarch64-pc-windows-msvc";
    if (arch == "x86") return "i686-pc-windows-msvc";
  } else if (os == "macos") {
    if (arch == "arm64") return "aarch64-apple-darwin";
    return "x86_64-apple-darwin";
  } else {
    bool musl = fs::exists("/etc/alpine-release");
    if (arch == "x86_64") return musl ? "x86_64-unknown-linux-musl" : "x86_64-unknown-linux-gnu";
    if (arch == "arm64") return "aarch64-unknown-linux-gnu";
    if (arch == "x86") return "i686-unknown-linux-gnu";
    if (arch == "armv7") return "armv7-unknown-linux-gnueabihf";
  }
  return "";
}

bool install_portable_python(const Paths& paths, std::string& error, bool verbose) {
  std::string curl = find_in_path("curl");
  std::string tar = find_in_path("tar");
  if (curl.empty() || tar.empty()) {
    error = "needs curl and tar (built into Windows 10+, macOS and most Linux)";
    return false;
  }
  std::string triple = python_build_triple();
  if (triple.empty()) {
    error = "no portable Python build exists for " + os_name() + "/" + arch_name();
    return false;
  }
  if (verbose) console_line("  Looking up the latest portable Python build for " + triple + " ...");
  CaptureResult listing = run_capture(
      {curl, "-fsSL", "--max-time", "60", "-H", "Accept: application/vnd.github+json",
       "https://api.github.com/repos/astral-sh/python-build-standalone/releases/latest"},
      "", 70000);
  if (!listing.started || listing.exit_code != 0) {
    error = "could not reach GitHub to find a Python build: " + trim(listing.err);
    return false;
  }
  json release;
  try {
    release = json::parse(listing.out);
  } catch (...) {
    error = "GitHub returned something unexpected";
    return false;
  }
  std::string chosen_url, chosen_name;
  // Prefer 3.12 (widest wheel support today), then 3.11, then 3.13.
  for (const char* minor : {"cpython-3.12.", "cpython-3.11.", "cpython-3.13."}) {
    for (const json& asset : release.value("assets", json::array())) {
      std::string name = json_str(asset, "name");
      if (!starts_with(name, minor)) continue;
      if (name.find("-" + triple + "-install_only") == std::string::npos) continue;
      if (!ends_with(name, ".tar.gz")) continue;
      chosen_name = name;
      chosen_url = json_str(asset, "browser_download_url");
      if (name.find("stripped") != std::string::npos) break;  // smaller download
    }
    if (!chosen_url.empty()) break;
  }
  if (chosen_url.empty()) {
    error = "no portable Python asset for " + triple + " in the latest release";
    return false;
  }
  fs::path archive = paths.runtimes / chosen_name;
  if (verbose) console_line("  Downloading " + chosen_name + " ...");
  CaptureResult download = run_capture({curl, "-fL", "--retry", "3", "--max-time", "1800", "-o",
                                        path_str(archive), chosen_url},
                                       "", 1900000);
  if (!download.started || download.exit_code != 0) {
    error = "download failed: " + trim(download.err);
    return false;
  }
  std::error_code fs_error;
  fs::path target = paths.runtimes / "python";
  fs::path staging = paths.runtimes / "python.unpacking";
  fs::remove_all(staging, fs_error);
  ensure_dir(staging);
  if (verbose) console_line("  Unpacking ...");
  CaptureResult unpack = run_capture({tar, "-xzf", path_str(archive), "-C", path_str(staging)}, "", 600000);
  fs::remove(archive, fs_error);
  if (!unpack.started || unpack.exit_code != 0) {
    error = "unpacking failed: " + trim(unpack.err);
    return false;
  }
  // The archive contains a top-level "python" folder.
  fs::remove_all(target, fs_error);
  fs::rename(staging / "python", target, fs_error);
  fs::remove_all(staging, fs_error);
  if (fs_error || !fs::exists(portable_python_path(paths))) {
    error = "unpacked Python is incomplete";
    return false;
  }
  return true;
}

}  // namespace maas
