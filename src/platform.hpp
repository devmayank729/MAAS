// Everything that differs between Windows, macOS and Linux lives behind this
// header. platform_posix.cpp implements it for macOS/Linux and
// platform_win.cpp for Windows.
#pragma once

#include "common.hpp"

namespace maas {

// ---------- running jobs with limits ----------
struct SpawnSpec {
  std::vector<std::string> argv;  // argv[0] must be an absolute path
  std::string cwd;
  std::map<std::string, std::string> env_set;  // added on top of the daemon's environment
  std::string log_path;                       // stdout and stderr are appended here
  int64_t memory_limit_mb = 0;                // 0 = no limit
  int64_t virtual_limit_mb = 0;               // POSIX RLIMIT_AS backstop, 0 = none
  int64_t cpu_limit_sec = 0;                  // CPU seconds, 0 = none
  int64_t file_size_limit_mb = 0;             // largest file the job may write
  int nice_level = 10;                        // lower priority so the daemon stays responsive
};

struct ProcHandle {
  int64_t pid = 0;
  int64_t start_token = 0;  // process start time, guards against PID reuse
  void* win_process = nullptr;
  void* win_job = nullptr;
};

struct ExitInfo {
  bool exited = false;   // normal exit with exit_code
  int exit_code = 0;
  int signal = 0;        // POSIX signal that ended it, 0 if none
  uint32_t win_status = 0;  // raw Windows exit status (NTSTATUS values like 0xC0000005)
};

bool spawn_job(const SpawnSpec& spec, ProcHandle& out, std::string& error);
// Returns true once the job has ended (fills info). Never blocks.
bool poll_job(ProcHandle& handle, ExitInfo& info);
void kill_job(ProcHandle& handle, bool force);  // kills the whole process tree
void release_job(ProcHandle& handle);
uint64_t job_rss_kb(const ProcHandle& handle);   // current resident memory of the tree
uint64_t job_peak_kb(const ProcHandle& handle);  // peak (Windows only, else 0)
int64_t process_start_token(int64_t pid);
bool process_alive(int64_t pid, int64_t start_token);
void kill_orphan(int64_t pid, int64_t start_token);  // after a daemon crash
std::string signal_name(int signal_number);

// The POSIX re-exec wrapper: `maas __exec --mem N ... -- program args`.
int exec_wrapper_main(int argc, char** argv);

// ---------- short helper commands (curl, python --version, ...) ----------
struct CaptureResult {
  bool started = false;
  bool timed_out = false;
  int exit_code = -1;
  std::string out;
  std::string err;
  std::string error;  // why it could not start
};
CaptureResult run_capture(const std::vector<std::string>& argv, const std::string& stdin_data,
                          int timeout_ms, const std::map<std::string, std::string>& env = {},
                          const std::string& cwd = "");

// Start a fully detached background process (for `maas start --detach`).
bool spawn_detached(const std::vector<std::string>& argv, const std::string& log_path,
                    std::string& error);

// ---------- system information ----------
std::string self_exe_path();
std::string find_in_path(const std::string& name);  // "" when not found
fs::path home_dir();
std::string host_name();
std::string os_name();      // "linux", "macos", "windows"
std::string os_pretty();    // "Ubuntu 22.04.4 LTS", "macOS 14.5", "Windows 10 (build 19045)"
std::string arch_name();    // "x86_64", "arm64", "x86", "armv7"
struct MemInfo {
  uint64_t total_kb = 0;
  uint64_t available_kb = 0;
};
MemInfo mem_info();
std::string cpu_model();
struct CpuFeatures {
  bool sse42 = false;
  bool avx = false;
  bool avx2 = false;
  bool fma = false;
  bool avx512f = false;
  bool neon = false;
};
CpuFeatures cpu_features();
int cpu_cores();
std::string getenv_str(const char* name);
bool is_stdin_tty();
bool is_stdout_tty();
void console_setup();  // UTF-8 + colour on Windows consoles, colour detection everywhere
void install_shutdown_handler(std::function<void()> on_shutdown);
std::string read_secret_line(const std::string& prompt);  // no echo
std::string read_line_tty(const std::string& prompt);     // reads from the terminal even when stdin is piped
bool is_wsl();
void set_private_permissions(const fs::path& path);
bool is_running_as_admin();

// ---------- network ----------
struct InterfaceAddr {
  std::string name;
  std::string ipv4;
  std::string netmask;
  bool loopback = false;
  bool up = true;
  int index = 0;
};
std::vector<InterfaceAddr> list_ipv4_interfaces();
std::string default_route_ipv4();  // address the OS would use to reach the internet, or ""
bool port_is_free(const std::string& host, int port);

// ---------- one daemon per machine ----------
class InstanceLock {
 public:
  ~InstanceLock();
  bool acquire(const fs::path& path);  // false if another daemon holds it
  void release();

 private:
  intptr_t handle_ = -1;
  fs::path path_;
};
bool lock_is_held(const fs::path& path);

}  // namespace maas
