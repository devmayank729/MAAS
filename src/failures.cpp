#include "failures.hpp"

#include <regex>

#ifndef _WIN32
#include <csignal>
static const int kSigIll = SIGILL, kSigKill = SIGKILL, kSigXcpu = SIGXCPU, kSigXfsz = SIGXFSZ;
static const int kSigSegv = SIGSEGV, kSigAbrt = SIGABRT, kSigBus = SIGBUS;
#else
// Windows jobs never end by signal (signal stays 0), so these never match.
static const int kSigIll = -1, kSigKill = -2, kSigXcpu = -3, kSigXfsz = -4;
static const int kSigSegv = -5, kSigAbrt = -6, kSigBus = -7;
#endif

namespace maas {

std::string pip_name_for_module(const std::string& module) {
  static const std::map<std::string, std::string> names = {
      {"cv2", "opencv-python-headless"}, {"sklearn", "scikit-learn"}, {"PIL", "pillow"},
      {"yaml", "pyyaml"},                {"bs4", "beautifulsoup4"},  {"dateutil", "python-dateutil"},
      {"Crypto", "pycryptodome"},        {"attr", "attrs"},          {"dotenv", "python-dotenv"},
      {"docx", "python-docx"},           {"pptx", "python-pptx"},    {"fitz", "pymupdf"},
      {"serial", "pyserial"},            {"jwt", "pyjwt"},           {"skimage", "scikit-image"},
      {"MySQLdb", "mysqlclient"},        {"psycopg2", "psycopg2-binary"}, {"google", "protobuf"},
      {"Levenshtein", "python-Levenshtein"}, {"magic", "python-magic"}, {"usb", "pyusb"},
      {"OpenSSL", "pyopenssl"},          {"lxml", "lxml"},           {"tqdm", "tqdm"}};
  auto found = names.find(module);
  return found == names.end() ? module : found->second;
}

static std::string last_nonempty_line(const std::string& text) {
  std::vector<std::string> lines = split(text, '\n');
  for (size_t index = lines.size(); index-- > 0;) {
    std::string line = trim(lines[index]);
    if (!line.empty()) return line;
  }
  return "";
}

static bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

json classify_failure(const FailureInput& input) {
  json result;
  const std::string& log = input.log_tail;
  std::string last_line = truncate_utf8(last_nonempty_line(log), 400);
  if (!last_line.empty()) result["last_output_line"] = last_line;
  double peak_mb = static_cast<double>(input.peak_kb) / 1024.0;
  if (input.peak_kb > 0) result["peak_memory_mb"] = static_cast<int64_t>(peak_mb + 0.5);

  auto finish = [&](const std::string& kind, const std::string& summary, const std::string& hint) {
    result["failure_kind"] = kind;
    result["summary"] = summary;
    if (!hint.empty()) result["hint"] = hint;
    return result;
  };

  // 1. MAAS stopped the job itself: that is the most reliable cause.
  if (input.kill_reason == "OOM_KILLED") {
    return finish("OOM_KILLED",
                  "stopped by MAAS: memory went above the " + std::to_string(input.memory_mb) + " MB limit",
                  "process the data in chunks (pandas chunksize=, SQL over the dataset, generators), use smaller "
                  "dtypes, or ask for a higher memory_mb if the node has RAM free");
  }
  if (input.kill_reason == "TIMEOUT") {
    return finish("TIMEOUT", "stopped by MAAS after the " + std::to_string(input.timeout_sec) + " s time limit",
                  "make it faster, print progress, or pass a larger timeout_sec");
  }
  if (input.kill_reason == "LOG_LIMIT") {
    return finish("LOG_LIMIT", "stopped by MAAS: the job printed too much output",
                  "print less (summaries or every Nth row) and write big results to a file instead");
  }
  if (input.kill_reason == "DISK_FULL") {
    return finish("DISK_FULL", "stopped by MAAS: the node's disk is nearly full",
                  "write smaller outputs or delete old files in the workspace");
  }
  if (input.kill_reason == "USER_KILLED" || input.kill_reason == "SHUTDOWN") {
    return finish("KILLED",
                  input.kill_reason == "SHUTDOWN" ? "stopped because the MAAS node shut down"
                                                  : "stopped by a person or by terminate_process",
                  "");
  }

  bool success = input.exit.exited && input.exit.exit_code == 0 && input.exit.signal == 0;
  if (success) {
    result["failure_kind"] = "OK";
    result["summary"] = "finished successfully";
    return result;
  }

  // 2. Messages the runtime printed. Checked before raw exit codes because they
  // name the actual problem.
  if (contains(log, "MAAS_EXEC_ERROR")) {
    return finish("RUNTIME_NOT_FOUND", truncate_utf8(last_line, 300),
                  "the interpreter could not start; check get_node_capabilities runtimes");
  }
  std::smatch match;
  static const std::regex python_missing_module("ModuleNotFoundError: No module named '([^']+)'");
  if (std::regex_search(log, match, python_missing_module)) {
    std::string module = match[1].str();
    std::string top = split(module, '.')[0];
    result["module"] = module;
    result["package_guess"] = pip_name_for_module(top);
    return finish("MODULE_NOT_FOUND", "Python module '" + module + "' is not installed",
                  "call install_packages with [\"" + pip_name_for_module(top) + "\"] then run again");
  }
  static const std::regex node_missing_module("Cannot find module '([^']+)'");
  if (std::regex_search(log, match, node_missing_module)) {
    std::string module = match[1].str();
    result["module"] = module;
    bool local_file = starts_with(module, ".") || starts_with(module, "/");
    if (!local_file) result["package_guess"] = module;
    return finish("MODULE_NOT_FOUND", "Node module '" + module + "' is not installed",
                  local_file ? "the file it imports does not exist; check the path"
                             : "call install_packages with manager \"npm\" and [\"" + module + "\"]");
  }
  static const std::regex python_syntax("File \"([^\"]+)\", line ([0-9]+)[^\\n]*\\n(?:[^\\n]*\\n){0,3}?\\s*(SyntaxError|IndentationError|TabError): ([^\\n]*)");
  if (std::regex_search(log, match, python_syntax)) {
    std::string file = match[1].str();
    if (!input.stored_name.empty() && ends_with(file, input.stored_name)) file = input.stored_name;
    result["file"] = file;
    result["line"] = std::stoi(match[2].str());
    result["error"] = match[3].str() + ": " + match[4].str();
    return finish("SYNTAX_ERROR", match[3].str() + " in " + file + " line " + match[2].str() + ": " + match[4].str(),
                  "fix that line with patch_file, then run again");
  }
  if (contains(log, "SyntaxError:") && input.interpreter == "node") {
    return finish("SYNTAX_ERROR", truncate_utf8(last_line, 300), "fix the syntax error, then run again");
  }
  if (contains(log, "MemoryError") || contains(log, "Cannot allocate memory") ||
      contains(log, "JavaScript heap out of memory") || contains(log, "std::bad_alloc") ||
      input.exit.win_status == 0xC0000017u || input.exit.win_status == 0xC000012Du) {
    return finish("OOM_KILLED", "the job ran out of memory",
                  "process the data in chunks or use less memory; check free RAM with get_node_capabilities");
  }
  if (contains(log, "No space left on device") || contains(log, "[Errno 28]") ||
      contains(log, "There is not enough space on the disk")) {
    return finish("DISK_FULL", "the disk filled up while the job ran", "write smaller outputs or clean the workspace");
  }
  if (contains(log, "File too large") || contains(log, "[Errno 27]") || input.exit.signal == kSigXfsz) {
    return finish("FILE_SIZE_LIMIT", "the job tried to write a file larger than the node allows",
                  "split the output into several smaller files");
  }
  if (input.exit.signal == kSigIll || input.exit.win_status == 0xC000001Du ||
      contains(log, "Illegal instruction")) {
    return finish("ILLEGAL_INSTRUCTION",
                  "the CPU hit an instruction it does not support (common with prebuilt ML packages on old CPUs)",
                  "check cpu.avx / cpu.avx2 in get_node_capabilities and install an older or 'noavx' build of the "
                  "package, or a pure-Python alternative");
  }
  static const std::regex python_file_missing("FileNotFoundError: \\[Errno 2\\] No such file or directory: '([^']+)'");
  if (std::regex_search(log, match, python_file_missing)) {
    result["path"] = match[1].str();
    return finish("FILE_NOT_FOUND", "file not found: " + match[1].str(),
                  "paths are relative to the workspace; check with list_files (uploads live in uploads/)");
  }
  if (contains(log, "PermissionError") || contains(log, "Permission denied") || contains(log, "EACCES")) {
    return finish("PERMISSION_DENIED", truncate_utf8(last_line, 300),
                  "the job may only write inside the workspace; check the path");
  }
  if (input.task_kind == "install") {
    if (contains(log, "No matching distribution found") || contains(log, "Could not find a version") ||
        contains(log, "404 Not Found") || contains(log, "E404")) {
      return finish("PACKAGE_NOT_FOUND", truncate_utf8(last_line, 300),
                    "check the package name spelling (import names often differ from package names)");
    }
    if (contains(log, "No module named pip")) {
      return finish("PIP_MISSING", "this Python has no pip",
                    "ask the user to run `maas runtime install python` on the node to get a portable Python with pip");
    }
    if (contains(log, "Could not fetch URL") || contains(log, "Temporary failure in name resolution") ||
        contains(log, "Failed to establish a new connection") || contains(log, "ENOTFOUND") ||
        contains(log, "getaddrinfo")) {
      return finish("NETWORK_ERROR", "package download failed: the node has no internet access",
                    "installing packages needs internet once; connect the node to a network with internet");
    }
    if (contains(log, "CERTIFICATE_VERIFY_FAILED") || contains(log, "certificate verify failed")) {
      return finish("TLS_ERROR", "HTTPS certificate check failed (often a wrong system clock on old laptops)",
                    "ask the user to fix the node's date and time");
    }
  }

  // 3. Signals and raw exit statuses.
  if (input.exit.signal == kSigKill) {
    // SIGKILL that MAAS did not send is usually the kernel's OOM killer.
    return finish("OOM_KILLED", "killed by the operating system (SIGKILL), most likely because RAM ran out",
                  "use less memory; check free RAM with get_node_capabilities");
  }
  if (input.exit.signal == kSigXcpu) {
    return finish("TIMEOUT", "stopped after using its CPU-time budget", "make it faster or pass a larger timeout_sec");
  }
  if (input.exit.signal == kSigSegv || input.exit.signal == kSigAbrt || input.exit.signal == kSigBus ||
      input.exit.win_status == 0xC0000005u || input.exit.win_status == 0xC00000FDu ||
      input.exit.win_status == 0xC0000409u) {
    result["signal"] = input.exit.signal;
    return finish("CRASH", "the program crashed (" + (input.exit.signal ? signal_name(input.exit.signal) : std::string("access violation")) + ")",
                  "often a native library issue; try another package version or a smaller input");
  }
  if (input.exit.signal != 0) {
    result["signal"] = input.exit.signal;
    return finish("KILLED", "ended by " + signal_name(input.exit.signal), "");
  }
  static const std::regex python_exception("\\n([A-Za-z_][A-Za-z0-9_.]*(?:Error|Exception|Interrupt|Exit)): ?([^\\n]*)\\s*$");
  std::string tail_with_newline = "\n" + log;
  if (contains(log, "Traceback (most recent call last)") && std::regex_search(tail_with_newline, match, python_exception)) {
    result["exception"] = match[1].str();
    result["message"] = truncate_utf8(match[2].str(), 300);
    static const std::regex python_location("File \"([^\"]+)\", line ([0-9]+)");
    std::string last_file;
    int last_line_number = 0;
    for (std::sregex_iterator item(log.begin(), log.end(), python_location), end; item != end; ++item) {
      last_file = (*item)[1].str();
      last_line_number = std::stoi((*item)[2].str());
    }
    if (!last_file.empty()) {
      if (!input.stored_name.empty() && ends_with(last_file, input.stored_name)) last_file = input.stored_name;
      result["file"] = last_file;
      result["line"] = last_line_number;
    }
    return finish("EXCEPTION", match[1].str() + ": " + truncate_utf8(match[2].str(), 200),
                  "read the traceback with read_process_logs (tail: true) and fix the cause");
  }
  result["exit_code"] = input.exit.exit_code;
  return finish("NONZERO_EXIT", "exited with code " + std::to_string(input.exit.exit_code),
                "read the end of the log with read_process_logs (tail: true)");
}

}  // namespace maas
