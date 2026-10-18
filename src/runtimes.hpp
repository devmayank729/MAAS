// Finds the language runtimes (Python, Node, shells) that scripts can use,
// and downloads a portable Python into ~/.maas/runtimes when none exists.
#pragma once

#include "config.hpp"

namespace maas {

struct Runtime {
  std::string name;     // python, node, bash, sh, powershell, pwsh, cmd, rscript
  std::string path;     // absolute path of the executable
  std::string version;
  std::string source;   // system, portable, config
  bool has_pip = false; // python only
  std::string npm_path; // node only
};

class Runtimes {
 public:
  void discover(const Paths& paths, const Config& config);
  bool find(const std::string& name, Runtime& out) const;
  json to_json() const;
  bool ready() const { return ready_.load(); }

  // Which runtime runs a file, from its extension (".py" -> "python").
  static std::string runtime_for_extension(const std::string& extension);
  // Interpreter + flags placed before the script path.
  std::vector<std::string> command_prefix(const Runtime& runtime) const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, Runtime> found_;
  std::atomic<bool> ready_{false};
};

// Downloads python-build-standalone into paths.runtimes/python (user space,
// no admin rights, no OS changes). Needs internet once.
bool install_portable_python(const Paths& paths, std::string& error, bool verbose);
fs::path portable_python_path(const Paths& paths);

}  // namespace maas
