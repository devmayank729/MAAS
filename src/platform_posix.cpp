// Linux and macOS implementation of platform.hpp.
#ifndef _WIN32

#include "platform.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(__APPLE__)
#include <libproc.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#endif

#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

extern char** environ;

namespace maas {

// ---------------------------------------------------------------- helpers

static std::vector<std::string> build_environment(const std::map<std::string, std::string>& overrides) {
  std::vector<std::string> result;
  for (char** entry = environ; entry && *entry; entry++) {
    std::string item = *entry;
    size_t equals = item.find('=');
    std::string key = equals == std::string::npos ? item : item.substr(0, equals);
    // A key in overrides replaces the variable; "-KEY" removes it.
    if (overrides.count(key) || overrides.count("-" + key)) continue;
    result.push_back(item);
  }
  for (const auto& pair : overrides) {
    if (!pair.first.empty() && pair.first[0] == '-') continue;
    result.push_back(pair.first + "=" + pair.second);
  }
  return result;
}

static std::vector<char*> to_c_array(std::vector<std::string>& items) {
  std::vector<char*> pointers;
  for (std::string& item : items) pointers.push_back(&item[0]);
  pointers.push_back(nullptr);
  return pointers;
}

static void close_inherited_fds() {
#if defined(__linux__) && defined(SYS_close_range)
  if (syscall(SYS_close_range, 3U, ~0U, 0U) == 0) return;
#endif
  long max_fd = sysconf(_SC_OPEN_MAX);
  if (max_fd < 0 || max_fd > 65536) max_fd = 65536;
#if defined(__linux__)
  // Faster path: only close descriptors that are actually open.
  DIR* directory = opendir("/proc/self/fd");
  if (directory) {
    std::vector<int> open_fds;
    int directory_fd = dirfd(directory);
    while (dirent* entry = readdir(directory)) {
      int fd = std::atoi(entry->d_name);
      if (fd > 2 && fd != directory_fd) open_fds.push_back(fd);
    }
    closedir(directory);
    for (int fd : open_fds) close(fd);
    return;
  }
#endif
  for (long fd = 3; fd < max_fd; fd++) close(static_cast<int>(fd));
}

std::string signal_name(int signal_number) {
  switch (signal_number) {
    case SIGKILL: return "SIGKILL";
    case SIGTERM: return "SIGTERM";
    case SIGINT: return "SIGINT";
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGILL: return "SIGILL";
    case SIGBUS: return "SIGBUS";
    case SIGFPE: return "SIGFPE";
    case SIGXCPU: return "SIGXCPU";
    case SIGXFSZ: return "SIGXFSZ";
    case SIGPIPE: return "SIGPIPE";
    case SIGHUP: return "SIGHUP";
    default: return "signal " + std::to_string(signal_number);
  }
}

// ---------------------------------------------------------------- the exec wrapper

// Runs inside the freshly spawned child. posix_spawn cannot set resource limits,
// so the daemon spawns itself in this mode; we apply limits and then exec the
// real program, which keeps the same PID.
int exec_wrapper_main(int argc, char** argv) {
  int64_t memory_mb = 0, virtual_mb = 0, cpu_sec = 0, file_mb = 0, parent_pid = 0;
  int nice_level = 10;
  std::string cwd;
  int program_index = -1;
  for (int index = 2; index < argc; index++) {
    std::string flag = argv[index];
    if (flag == "--") {
      program_index = index + 1;
      break;
    }
    if (index + 1 >= argc) break;
    std::string value = argv[index + 1];
    int64_t number = 0;
    parse_int64(value, number);
    if (flag == "--mem") memory_mb = number;
    else if (flag == "--vmem") virtual_mb = number;
    else if (flag == "--cpu") cpu_sec = number;
    else if (flag == "--fsize") file_mb = number;
    else if (flag == "--nice") nice_level = static_cast<int>(number);
    else if (flag == "--parent") parent_pid = number;
    else if (flag == "--cwd") cwd = value;
    index++;
  }
  (void)memory_mb;
  if (program_index < 0 || program_index >= argc) {
    std::fprintf(stderr, "MAAS_EXEC_ERROR: no program given to the exec wrapper\n");
    return 127;
  }

  // New session and process group: lets the daemon kill the whole tree with
  // one killpg(), and detaches the job from the daemon's terminal.
  setsid();
#if defined(__linux__)
  // If the daemon dies, the kernel kills this job too.
  prctl(PR_SET_PDEATHSIG, SIGKILL);
  if (parent_pid > 0 && getppid() != static_cast<pid_t>(parent_pid)) _exit(1);
#endif

  // Reset signals the daemon ignores (SIGPIPE) so the job behaves normally.
  struct sigaction default_action;
  std::memset(&default_action, 0, sizeof(default_action));
  default_action.sa_handler = SIG_DFL;
  const int signals_to_reset[] = {SIGPIPE, SIGINT, SIGTERM, SIGHUP, SIGCHLD, SIGQUIT};
  for (int signal_number : signals_to_reset) sigaction(signal_number, &default_action, nullptr);
  sigset_t empty_set;
  sigemptyset(&empty_set);
  sigprocmask(SIG_SETMASK, &empty_set, nullptr);

  setpriority(PRIO_PROCESS, 0, nice_level);

  struct rlimit limit;
  limit.rlim_cur = 0;
  limit.rlim_max = 0;
  setrlimit(RLIMIT_CORE, &limit);  // core dumps would fill a small disk
#if defined(__linux__)
  // macOS does not enforce RLIMIT_AS reliably, so it is Linux only. It is a
  // generous backstop; the daemon's RSS watchdog enforces the real limit.
  if (virtual_mb > 0) {
    limit.rlim_cur = static_cast<rlim_t>(virtual_mb) * 1024 * 1024;
    limit.rlim_max = limit.rlim_cur;
    setrlimit(RLIMIT_AS, &limit);
  }
#else
  (void)virtual_mb;
#endif
  if (cpu_sec > 0) {
    limit.rlim_cur = static_cast<rlim_t>(cpu_sec);
    limit.rlim_max = static_cast<rlim_t>(cpu_sec + 10);
    setrlimit(RLIMIT_CPU, &limit);
  }
  if (file_mb > 0) {
    limit.rlim_cur = static_cast<rlim_t>(file_mb) * 1024 * 1024;
    limit.rlim_max = limit.rlim_cur;
    setrlimit(RLIMIT_FSIZE, &limit);
  }

  close_inherited_fds();

  if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
    std::fprintf(stderr, "MAAS_EXEC_ERROR: cannot enter working folder %s: %s\n", cwd.c_str(),
                 std::strerror(errno));
    _exit(126);
  }
  execv(argv[program_index], &argv[program_index]);
  std::fprintf(stderr, "MAAS_EXEC_ERROR: cannot start %s: %s\n", argv[program_index], std::strerror(errno));
  _exit(127);
}

// ---------------------------------------------------------------- jobs

bool spawn_job(const SpawnSpec& spec, ProcHandle& out, std::string& error) {
  std::string self = self_exe_path();
  if (self.empty()) {
    error = "cannot locate the maas executable";
    return false;
  }
  std::vector<std::string> args = {self,
                                   "__exec",
                                   "--parent",
                                   std::to_string(getpid()),
                                   "--mem",
                                   std::to_string(spec.memory_limit_mb),
                                   "--vmem",
                                   std::to_string(spec.virtual_limit_mb),
                                   "--cpu",
                                   std::to_string(spec.cpu_limit_sec),
                                   "--fsize",
                                   std::to_string(spec.file_size_limit_mb),
                                   "--nice",
                                   std::to_string(spec.nice_level),
                                   "--cwd",
                                   spec.cwd,
                                   "--"};
  for (const std::string& item : spec.argv) args.push_back(item);
  std::vector<char*> argv_pointers = to_c_array(args);
  std::vector<std::string> environment = build_environment(spec.env_set);
  std::vector<char*> env_pointers = to_c_array(environment);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  // The OS writes the job's output straight into the log file, so the daemon
  // never drains a pipe and a noisy job cannot block it.
  posix_spawn_file_actions_addopen(&actions, 1, spec.log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
  posix_spawn_file_actions_adddup2(&actions, 1, 2);

  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  sigset_t empty_set;
  sigemptyset(&empty_set);
  posix_spawnattr_setsigmask(&attributes, &empty_set);
  short flags = POSIX_SPAWN_SETSIGMASK;
#if defined(__APPLE__)
  flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
  posix_spawnattr_setflags(&attributes, flags);

  pid_t pid = 0;
  int result = posix_spawn(&pid, self.c_str(), &actions, &attributes, argv_pointers.data(), env_pointers.data());
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  if (result != 0) {
    error = std::string("posix_spawn failed: ") + std::strerror(result);
    return false;
  }
  out.pid = pid;
  out.start_token = process_start_token(pid);
  return true;
}

bool poll_job(ProcHandle& handle, ExitInfo& info) {
  if (handle.pid <= 0) return true;
  int status = 0;
  pid_t result = waitpid(static_cast<pid_t>(handle.pid), &status, WNOHANG);
  if (result == 0) return false;
  if (result < 0) {
    if (errno == EINTR) return false;
    info.exited = true;
    info.exit_code = -1;
    return true;
  }
  if (WIFEXITED(status)) {
    info.exited = true;
    info.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    info.exited = false;
    info.signal = WTERMSIG(status);
  }
  return true;
}

void kill_job(ProcHandle& handle, bool force) {
  if (handle.pid <= 0) return;
  int signal_number = force ? SIGKILL : SIGTERM;
  killpg(static_cast<pid_t>(handle.pid), signal_number);
  kill(static_cast<pid_t>(handle.pid), signal_number);
}

void release_job(ProcHandle& handle) { handle.pid = 0; }

#if defined(__linux__)
// Reads /proc/<pid>/stat and returns the fields after the command name.
static bool read_proc_stat(long pid, std::vector<std::string>& fields) {
  std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
  if (!input) return false;
  std::string content;
  std::getline(input, content);
  size_t close_paren = content.rfind(')');
  if (close_paren == std::string::npos) return false;
  std::istringstream stream(content.substr(close_paren + 2));
  fields.clear();
  std::string item;
  while (stream >> item) fields.push_back(item);
  return fields.size() > 20;
}
#endif

uint64_t job_rss_kb(const ProcHandle& handle) {
  if (handle.pid <= 0) return 0;
#if defined(__linux__)
  // Sum every process in the job's process group (scripts can start children).
  static const long page_kb = sysconf(_SC_PAGESIZE) / 1024;
  uint64_t total_kb = 0;
  DIR* directory = opendir("/proc");
  if (!directory) return 0;
  std::vector<std::string> fields;
  while (dirent* entry = readdir(directory)) {
    if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
    long pid = std::atol(entry->d_name);
    if (!read_proc_stat(pid, fields)) continue;
    // fields[0] is the state (field 3), so the process group (field 5) is
    // fields[2] and resident pages (field 24) is fields[21].
    long group = std::atol(fields[2].c_str());
    if (group != handle.pid) continue;
    total_kb += static_cast<uint64_t>(std::atoll(fields[21].c_str())) * static_cast<uint64_t>(page_kb);
  }
  closedir(directory);
  return total_kb;
#elif defined(__APPLE__)
  uint64_t total_kb = 0;
  pid_t members[512];
  int bytes = proc_listpgrppids(static_cast<pid_t>(handle.pid), members, sizeof(members));
  int count = bytes > 0 ? bytes / static_cast<int>(sizeof(pid_t)) : 0;
  if (count == 0) {
    members[0] = static_cast<pid_t>(handle.pid);
    count = 1;
  }
  for (int index = 0; index < count; index++) {
    struct proc_taskinfo task_info;
    int got = proc_pidinfo(members[index], PROC_PIDTASKINFO, 0, &task_info, sizeof(task_info));
    if (got == static_cast<int>(sizeof(task_info))) total_kb += task_info.pti_resident_size / 1024;
  }
  return total_kb;
#else
  return 0;
#endif
}

uint64_t job_peak_kb(const ProcHandle&) { return 0; }

int64_t process_start_token(int64_t pid) {
#if defined(__linux__)
  std::vector<std::string> fields;
  if (!read_proc_stat(static_cast<long>(pid), fields)) return 0;
  return std::atoll(fields[19].c_str());  // field 22: start time in clock ticks since boot
#elif defined(__APPLE__)
  struct proc_bsdinfo info;
  int got = proc_pidinfo(static_cast<pid_t>(pid), PROC_PIDTBSDINFO, 0, &info, sizeof(info));
  if (got != static_cast<int>(sizeof(info))) return 0;
  return static_cast<int64_t>(info.pbi_start_tvsec) * 1000000 + info.pbi_start_tvusec;
#else
  (void)pid;
  return 0;
#endif
}

bool process_alive(int64_t pid, int64_t start_token) {
  if (pid <= 0) return false;
  if (kill(static_cast<pid_t>(pid), 0) != 0 && errno != EPERM) return false;
  if (start_token == 0) return true;
  return process_start_token(pid) == start_token;
}

void kill_orphan(int64_t pid, int64_t start_token) {
  if (!process_alive(pid, start_token)) return;
  killpg(static_cast<pid_t>(pid), SIGKILL);
  kill(static_cast<pid_t>(pid), SIGKILL);
}

// ---------------------------------------------------------------- run_capture

CaptureResult run_capture(const std::vector<std::string>& argv_in, const std::string& stdin_data,
                          int timeout_ms, const std::map<std::string, std::string>& env,
                          const std::string& cwd) {
  CaptureResult result;
  if (argv_in.empty()) {
    result.error = "empty command";
    return result;
  }
  std::vector<std::string> args = argv_in;
  if (args[0].find('/') == std::string::npos) {
    std::string found = find_in_path(args[0]);
    if (found.empty()) {
      result.error = args[0] + " not found on PATH";
      return result;
    }
    args[0] = found;
  }
  std::vector<char*> argv_pointers = to_c_array(args);
  std::vector<std::string> environment = build_environment(env);
  std::vector<char*> env_pointers = to_c_array(environment);

  int in_pipe[2], out_pipe[2], err_pipe[2];
  if (pipe(in_pipe) != 0) {
    result.error = "pipe failed";
    return result;
  }
  if (pipe(out_pipe) != 0) {
    close(in_pipe[0]);
    close(in_pipe[1]);
    result.error = "pipe failed";
    return result;
  }
  if (pipe(err_pipe) != 0) {
    close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]);
    result.error = "pipe failed";
    return result;
  }
  const char* working_dir = cwd.empty() ? nullptr : cwd.c_str();

  pid_t pid = fork();
  if (pid < 0) {
    result.error = std::string("fork failed: ") + std::strerror(errno);
    close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]);
    close(err_pipe[0]); close(err_pipe[1]);
    return result;
  }
  if (pid == 0) {
    // Child: only async-signal-safe calls from here until exec.
    setpgid(0, 0);
    dup2(in_pipe[0], 0);
    dup2(out_pipe[1], 1);
    dup2(err_pipe[1], 2);
    struct sigaction default_action;
    std::memset(&default_action, 0, sizeof(default_action));
    default_action.sa_handler = SIG_DFL;
    sigaction(SIGPIPE, &default_action, nullptr);
    long max_fd = 1024;
    for (long fd = 3; fd < max_fd; fd++) close(static_cast<int>(fd));
    if (working_dir && chdir(working_dir) != 0) _exit(126);
    execve(argv_pointers[0], argv_pointers.data(), env_pointers.data());
    _exit(127);
  }
  close(in_pipe[0]);
  close(out_pipe[1]);
  close(err_pipe[1]);
  result.started = true;

  int stdin_fd = in_pipe[1];
  size_t stdin_written = 0;
  if (stdin_data.empty()) {
    close(stdin_fd);
    stdin_fd = -1;
  } else {
    fcntl(stdin_fd, F_SETFL, fcntl(stdin_fd, F_GETFL) | O_NONBLOCK);
  }
  int stdout_fd = out_pipe[0];
  int stderr_fd = err_pipe[0];
  int64_t deadline = mono_ms() + timeout_ms;
  char buffer[16384];
  while (stdout_fd >= 0 || stderr_fd >= 0) {
    int64_t remaining = deadline - mono_ms();
    if (remaining <= 0) {
      result.timed_out = true;
      break;
    }
    struct pollfd watched[3];
    int count = 0;
    int out_slot = -1, err_slot = -1, in_slot = -1;
    if (stdout_fd >= 0) {
      watched[count].fd = stdout_fd;
      watched[count].events = POLLIN;
      out_slot = count++;
    }
    if (stderr_fd >= 0) {
      watched[count].fd = stderr_fd;
      watched[count].events = POLLIN;
      err_slot = count++;
    }
    if (stdin_fd >= 0) {
      watched[count].fd = stdin_fd;
      watched[count].events = POLLOUT;
      in_slot = count++;
    }
    int ready = poll(watched, static_cast<nfds_t>(count), static_cast<int>(remaining > 1000 ? 1000 : remaining));
    if (ready < 0 && errno != EINTR) break;
    if (ready <= 0) continue;
    if (out_slot >= 0 && (watched[out_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
      ssize_t got = read(stdout_fd, buffer, sizeof(buffer));
      if (got > 0) result.out.append(buffer, static_cast<size_t>(got));
      else { close(stdout_fd); stdout_fd = -1; }
    }
    if (err_slot >= 0 && (watched[err_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
      ssize_t got = read(stderr_fd, buffer, sizeof(buffer));
      if (got > 0) result.err.append(buffer, static_cast<size_t>(got));
      else { close(stderr_fd); stderr_fd = -1; }
    }
    if (in_slot >= 0 && (watched[in_slot].revents & (POLLOUT | POLLERR | POLLHUP))) {
      ssize_t wrote = write(stdin_fd, stdin_data.data() + stdin_written, stdin_data.size() - stdin_written);
      if (wrote > 0) stdin_written += static_cast<size_t>(wrote);
      if (wrote < 0 || stdin_written >= stdin_data.size()) {
        close(stdin_fd);
        stdin_fd = -1;
      }
    }
  }
  if (stdin_fd >= 0) close(stdin_fd);
  if (stdout_fd >= 0) close(stdout_fd);
  if (stderr_fd >= 0) close(stderr_fd);
  if (result.timed_out) {
    killpg(pid, SIGKILL);
    kill(pid, SIGKILL);
  }
  int status = 0;
  // Wait for exit (bounded so a stuck child can never hang the daemon).
  int64_t wait_deadline = mono_ms() + (result.timed_out ? 2000 : timeout_ms);
  while (true) {
    pid_t done = waitpid(pid, &status, WNOHANG);
    if (done == pid) break;
    if (done < 0 && errno != EINTR) break;
    if (mono_ms() > wait_deadline) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      result.timed_out = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
  else result.exit_code = 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
  if (result.exit_code == 127 && result.out.empty() && result.err.empty()) {
    result.error = "could not execute " + args[0];
  }
  return result;
}

bool spawn_detached(const std::vector<std::string>& argv_in, const std::string& log_path, std::string& error) {
  std::vector<std::string> args = argv_in;
  std::vector<char*> argv_pointers = to_c_array(args);
  std::vector<std::string> environment = build_environment({});
  std::vector<char*> env_pointers = to_c_array(environment);
  pid_t first = fork();
  if (first < 0) {
    error = std::strerror(errno);
    return false;
  }
  if (first == 0) {
    setsid();
    pid_t second = fork();
    if (second != 0) _exit(0);
    int null_fd = open("/dev/null", O_RDONLY);
    int log_fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (null_fd >= 0) dup2(null_fd, 0);
    if (log_fd >= 0) {
      dup2(log_fd, 1);
      dup2(log_fd, 2);
    }
    for (long fd = 3; fd < 1024; fd++) close(static_cast<int>(fd));
    execve(argv_pointers[0], argv_pointers.data(), env_pointers.data());
    _exit(127);
  }
  int status = 0;
  waitpid(first, &status, 0);
  return true;
}

// ---------------------------------------------------------------- system info

std::string self_exe_path() {
#if defined(__linux__)
  char buffer[4096];
  ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (length <= 0) return "";
  buffer[length] = '\0';
  return buffer;
#elif defined(__APPLE__)
  char buffer[4096];
  uint32_t size = sizeof(buffer);
  if (_NSGetExecutablePath(buffer, &size) != 0) return "";
  char resolved[PATH_MAX];
  if (realpath(buffer, resolved)) return resolved;
  return buffer;
#else
  return "";
#endif
}

static bool is_executable_file(const std::string& path) {
  struct stat info;
  if (stat(path.c_str(), &info) != 0) return false;
  if (!S_ISREG(info.st_mode)) return false;
  return access(path.c_str(), X_OK) == 0;
}

std::string find_in_path(const std::string& name) {
  if (name.empty()) return "";
  if (name.find('/') != std::string::npos) return is_executable_file(name) ? name : "";
  std::string path_value = getenv_str("PATH");
  if (path_value.empty()) path_value = "/usr/local/bin:/usr/bin:/bin";
  for (const std::string& directory : split(path_value, ':')) {
    if (directory.empty()) continue;
    std::string candidate = directory + "/" + name;
    if (is_executable_file(candidate)) return candidate;
  }
  return "";
}

fs::path home_dir() {
  std::string home = getenv_str("HOME");
  if (!home.empty()) return fs::path(home);
  struct passwd* entry = getpwuid(getuid());
  if (entry && entry->pw_dir) return fs::path(entry->pw_dir);
  return fs::path("/tmp");
}

std::string host_name() {
  char buffer[256];
  if (gethostname(buffer, sizeof(buffer)) != 0) return "maas-node";
  buffer[sizeof(buffer) - 1] = '\0';
  std::string name = buffer;
  size_t dot = name.find('.');
  if (dot != std::string::npos) name = name.substr(0, dot);
  return name.empty() ? "maas-node" : name;
}

std::string os_name() {
#if defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}

std::string os_pretty() {
  struct utsname info;
  uname(&info);
#if defined(__APPLE__)
  char version[64];
  size_t size = sizeof(version);
  if (sysctlbyname("kern.osproductversion", version, &size, nullptr, 0) == 0) {
    return std::string("macOS ") + version;
  }
  return std::string("macOS (Darwin ") + info.release + ")";
#else
  std::ifstream input("/etc/os-release");
  std::string line;
  while (std::getline(input, line)) {
    if (starts_with(line, "PRETTY_NAME=")) {
      std::string value = line.substr(12);
      if (value.size() >= 2 && value.front() == '"') value = value.substr(1, value.size() - 2);
      return value + " (kernel " + info.release + ")";
    }
  }
  return std::string("Linux ") + info.release;
#endif
}

std::string arch_name() {
  struct utsname info;
  uname(&info);
  std::string machine = info.machine;
  if (machine == "x86_64" || machine == "amd64") return "x86_64";
  if (machine == "aarch64" || machine == "arm64") return "arm64";
  if (machine == "i386" || machine == "i486" || machine == "i586" || machine == "i686") return "x86";
  if (starts_with(machine, "armv7") || starts_with(machine, "armv6")) return "armv7";
  return machine;
}

MemInfo mem_info() {
  MemInfo info;
#if defined(__linux__)
  std::ifstream input("/proc/meminfo");
  std::string key;
  uint64_t value = 0;
  std::string unit;
  uint64_t free_kb = 0, buffers_kb = 0, cached_kb = 0;
  bool have_available = false;
  while (input >> key >> value) {
    std::getline(input, unit);
    if (key == "MemTotal:") info.total_kb = value;
    else if (key == "MemAvailable:") {
      info.available_kb = value;
      have_available = true;
    } else if (key == "MemFree:") free_kb = value;
    else if (key == "Buffers:") buffers_kb = value;
    else if (key == "Cached:") cached_kb = value;
  }
  // Kernels older than 3.14 have no MemAvailable line.
  if (!have_available) info.available_kb = free_kb + buffers_kb + cached_kb;
#elif defined(__APPLE__)
  uint64_t total = 0;
  size_t size = sizeof(total);
  if (sysctlbyname("hw.memsize", &total, &size, nullptr, 0) == 0) info.total_kb = total / 1024;
  vm_statistics64_data_t stats;
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &count) ==
      KERN_SUCCESS) {
    uint64_t page = static_cast<uint64_t>(vm_page_size);
    info.available_kb = (static_cast<uint64_t>(stats.free_count) + stats.inactive_count + stats.purgeable_count) *
                        page / 1024;
  }
#endif
  return info;
}

std::string getenv_str(const char* name) {
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
}

bool is_stdin_tty() { return isatty(0) == 1; }
bool is_stdout_tty() { return isatty(1) == 1; }

void console_setup() {
  std::string term = getenv_str("TERM");
  bool colour = is_stdout_tty() && term != "dumb" && getenv_str("NO_COLOR").empty();
  set_color_enabled(colour);
  signal(SIGPIPE, SIG_IGN);  // a closed socket must not kill the daemon
}

static int g_shutdown_pipe[2] = {-1, -1};

static void on_signal(int) {
  char byte = 1;
  if (g_shutdown_pipe[1] >= 0) {
    ssize_t ignored = write(g_shutdown_pipe[1], &byte, 1);
    (void)ignored;
  }
}

void install_shutdown_handler(std::function<void()> on_shutdown) {
  if (pipe(g_shutdown_pipe) != 0) return;
  fcntl(g_shutdown_pipe[0], F_SETFD, FD_CLOEXEC);
  fcntl(g_shutdown_pipe[1], F_SETFD, FD_CLOEXEC);
  struct sigaction action;
  std::memset(&action, 0, sizeof(action));
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
  sigaction(SIGHUP, &action, nullptr);
  std::thread watcher([on_shutdown]() {
    char byte = 0;
    while (true) {
      ssize_t got = read(g_shutdown_pipe[0], &byte, 1);
      if (got == 1) break;
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) return;
    }
    on_shutdown();
  });
  watcher.detach();
}

std::string read_line_tty(const std::string& prompt) {
  FILE* terminal = std::fopen("/dev/tty", "r+");
  if (!terminal) {
    if (!is_stdin_tty()) return "";
    std::fputs(prompt.c_str(), stdout);
    std::fflush(stdout);
    char buffer[1024];
    if (!std::fgets(buffer, sizeof(buffer), stdin)) return "";
    return trim(buffer);
  }
  std::fputs(prompt.c_str(), terminal);
  std::fflush(terminal);
  char buffer[1024];
  std::string line;
  if (std::fgets(buffer, sizeof(buffer), terminal)) line = buffer;
  std::fclose(terminal);
  return trim(line);
}

std::string read_secret_line(const std::string& prompt) {
  int terminal = open("/dev/tty", O_RDWR);
  if (terminal < 0) return read_line_tty(prompt);
  ssize_t ignored = write(terminal, prompt.data(), prompt.size());
  (void)ignored;
  struct termios saved;
  bool have_saved = tcgetattr(terminal, &saved) == 0;
  if (have_saved) {
    struct termios quiet = saved;
    quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
    tcsetattr(terminal, TCSANOW, &quiet);
  }
  std::string line;
  char character = 0;
  while (read(terminal, &character, 1) == 1 && character != '\n') line.push_back(character);
  if (have_saved) tcsetattr(terminal, TCSANOW, &saved);
  ignored = write(terminal, "\n", 1);
  close(terminal);
  return trim(line);
}

bool is_wsl() {
#if defined(__linux__)
  std::string release;
  read_file("/proc/sys/kernel/osrelease", release, 4096);
  release = to_lower(release);
  return release.find("microsoft") != std::string::npos || release.find("wsl") != std::string::npos;
#else
  return false;
#endif
}

void set_private_permissions(const fs::path& path) {
  std::error_code error;
  if (fs::is_directory(path, error)) chmod(path.c_str(), 0700);
  else chmod(path.c_str(), 0600);
}

bool is_running_as_admin() { return geteuid() == 0; }

// ---------------------------------------------------------------- network

std::vector<InterfaceAddr> list_ipv4_interfaces() {
  std::vector<InterfaceAddr> result;
  struct ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return result;
  for (struct ifaddrs* item = list; item; item = item->ifa_next) {
    if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET) continue;
    InterfaceAddr entry;
    entry.name = item->ifa_name ? item->ifa_name : "";
    char text[INET_ADDRSTRLEN];
    auto* address = reinterpret_cast<struct sockaddr_in*>(item->ifa_addr);
    inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text));
    entry.ipv4 = text;
    if (item->ifa_netmask) {
      auto* mask = reinterpret_cast<struct sockaddr_in*>(item->ifa_netmask);
      inet_ntop(AF_INET, &mask->sin_addr, text, sizeof(text));
      entry.netmask = text;
    }
    entry.loopback = (item->ifa_flags & IFF_LOOPBACK) != 0;
    entry.up = (item->ifa_flags & IFF_UP) != 0 && (item->ifa_flags & IFF_RUNNING) != 0;
    entry.index = static_cast<int>(if_nametoindex(item->ifa_name));
    result.push_back(entry);
  }
  freeifaddrs(list);
  return result;
}

std::string default_route_ipv4() {
  // Connecting a UDP socket sends no packets; it only asks the kernel which
  // local address it would use to reach that destination.
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) return "";
  struct sockaddr_in target;
  std::memset(&target, 0, sizeof(target));
  target.sin_family = AF_INET;
  target.sin_port = htons(53);
  inet_pton(AF_INET, "8.8.8.8", &target.sin_addr);
  std::string result;
  if (connect(sock, reinterpret_cast<struct sockaddr*>(&target), sizeof(target)) == 0) {
    struct sockaddr_in local;
    socklen_t length = sizeof(local);
    if (getsockname(sock, reinterpret_cast<struct sockaddr*>(&local), &length) == 0) {
      char text[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text));
      result = text;
    }
  }
  close(sock);
  if (result == "0.0.0.0") result.clear();
  return result;
}

bool port_is_free(const std::string& host, int port) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) return false;
  int enable = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  struct sockaddr_in address;
  std::memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  inet_pton(AF_INET, host.empty() ? "0.0.0.0" : host.c_str(), &address.sin_addr);
  bool free_port = bind(sock, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) == 0;
  close(sock);
  return free_port;
}

// ---------------------------------------------------------------- instance lock

InstanceLock::~InstanceLock() { release(); }

bool InstanceLock::acquire(const fs::path& path) {
  ensure_dir(path.parent_path());
  int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    close(fd);
    return false;
  }
  std::string pid_text = std::to_string(getpid()) + "\n";
  if (ftruncate(fd, 0) == 0) {
    ssize_t ignored = write(fd, pid_text.data(), pid_text.size());
    (void)ignored;
  }
  handle_ = fd;
  path_ = path;
  return true;
}

void InstanceLock::release() {
  if (handle_ >= 0) {
    flock(static_cast<int>(handle_), LOCK_UN);
    close(static_cast<int>(handle_));
    handle_ = -1;
  }
}

bool lock_is_held(const fs::path& path) {
  int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) return false;
  bool held = flock(fd, LOCK_EX | LOCK_NB) != 0;
  if (!held) flock(fd, LOCK_UN);
  close(fd);
  return held;
}

}  // namespace maas

#endif  // !_WIN32
