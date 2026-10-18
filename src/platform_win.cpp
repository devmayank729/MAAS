// Windows implementation of platform.hpp.
#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <psapi.h>

#include <fcntl.h>
#include <io.h>

#include <cstring>
#include <iostream>
#include <thread>

#include "platform.hpp"

namespace maas {

// ---------------------------------------------------------------- text conversion

static std::wstring widen(const std::string& text) {
  if (text.empty()) return std::wstring();
  int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring result(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &result[0], length);
  return result;
}

static std::string narrow(const std::wstring& text) {
  if (text.empty()) return std::string();
  int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &result[0], length, nullptr, nullptr);
  return result;
}

static void ensure_winsock() {
  static std::once_flag once;
  std::call_once(once, []() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  });
}

// Quote one argument the way CommandLineToArgvW / the MSVC runtime expects.
static std::wstring quote_argument(const std::wstring& argument) {
  if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;
  std::wstring result = L"\"";
  size_t backslashes = 0;
  for (wchar_t character : argument) {
    if (character == L'\\') {
      backslashes++;
      continue;
    }
    if (character == L'"') {
      result.append(backslashes * 2 + 1, L'\\');
      result.push_back(L'"');
    } else {
      result.append(backslashes, L'\\');
      result.push_back(character);
    }
    backslashes = 0;
  }
  result.append(backslashes * 2, L'\\');
  result.push_back(L'"');
  return result;
}

static bool is_batch_file(const std::string& path) {
  std::string lower = to_lower(path);
  return ends_with(lower, ".cmd") || ends_with(lower, ".bat");
}

// Returns application name and the full command line for CreateProcessW.
static void build_command(const std::vector<std::string>& argv, std::wstring& application, std::wstring& command_line) {
  command_line.clear();
  for (size_t index = 0; index < argv.size(); index++) {
    if (index > 0) command_line.push_back(L' ');
    command_line += quote_argument(widen(argv[index]));
  }
  if (is_batch_file(argv[0])) {
    // Batch files must run through cmd.exe; /s strips the outer quotes again.
    std::string comspec = getenv_str("ComSpec");
    if (comspec.empty()) comspec = "C:\\Windows\\System32\\cmd.exe";
    application = widen(comspec);
    command_line = L"cmd.exe /d /s /c \"" + command_line + L"\"";
  } else {
    application = widen(argv[0]);
  }
}

static std::vector<wchar_t> build_environment_block(const std::map<std::string, std::string>& overrides) {
  std::vector<std::wstring> entries;
  wchar_t* current = GetEnvironmentStringsW();
  if (current) {
    for (wchar_t* entry = current; *entry; entry += wcslen(entry) + 1) {
      std::wstring item = entry;
      size_t equals = item.find(L'=', 1);  // entries like "=C:=C:\" start with '='
      std::string key = to_lower(narrow(equals == std::wstring::npos ? item : item.substr(0, equals)));
      bool replaced = false;
      for (const auto& pair : overrides) {
        std::string override_key = to_lower(pair.first);
        if (override_key == key || override_key == "-" + key) {
          replaced = true;
          break;
        }
      }
      if (!replaced) entries.push_back(item);
    }
    FreeEnvironmentStringsW(current);
  }
  for (const auto& pair : overrides) {
    if (!pair.first.empty() && pair.first[0] == '-') continue;
    entries.push_back(widen(pair.first + "=" + pair.second));
  }
  std::vector<wchar_t> block;
  for (const std::wstring& item : entries) {
    block.insert(block.end(), item.begin(), item.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

// STARTUPINFOEX with an explicit list of handles the child may inherit, so it
// never inherits our sockets or other files by accident.
struct InheritList {
  std::vector<char> buffer;
  LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
  std::vector<HANDLE> handles;
  bool init(const std::vector<HANDLE>& to_inherit) {
    handles = to_inherit;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    buffer.resize(size);
    list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) return false;
    return UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(),
                                     handles.size() * sizeof(HANDLE), nullptr, nullptr) != 0;
  }
  ~InheritList() {
    if (list) DeleteProcThreadAttributeList(list);
  }
};

std::string signal_name(int signal_number) { return "signal " + std::to_string(signal_number); }

int exec_wrapper_main(int, char**) {
  std::fprintf(stderr, "the exec wrapper is not used on Windows\n");
  return 127;
}

// ---------------------------------------------------------------- jobs

bool spawn_job(const SpawnSpec& spec, ProcHandle& out, std::string& error) {
  if (spec.argv.empty()) {
    error = "empty command";
    return false;
  }
  SECURITY_ATTRIBUTES inherit_attributes;
  inherit_attributes.nLength = sizeof(inherit_attributes);
  inherit_attributes.lpSecurityDescriptor = nullptr;
  inherit_attributes.bInheritHandle = TRUE;

  HANDLE log_handle = CreateFileW(widen(spec.log_path).c_str(), FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inherit_attributes,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (log_handle == INVALID_HANDLE_VALUE) {
    error = "cannot open log file (code " + std::to_string(GetLastError()) + ")";
    return false;
  }
  HANDLE null_handle = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit_attributes,
                                   OPEN_EXISTING, 0, nullptr);

  InheritList inherit;
  std::vector<HANDLE> handles = {log_handle};
  if (null_handle != INVALID_HANDLE_VALUE) handles.push_back(null_handle);
  inherit.init(handles);

  STARTUPINFOEXW startup;
  ZeroMemory(&startup, sizeof(startup));
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = null_handle;
  startup.StartupInfo.hStdOutput = log_handle;
  startup.StartupInfo.hStdError = log_handle;
  startup.lpAttributeList = inherit.list;

  std::wstring application, command_line;
  build_command(spec.argv, application, command_line);
  std::vector<wchar_t> command_buffer(command_line.begin(), command_line.end());
  command_buffer.push_back(L'\0');
  std::vector<wchar_t> environment = build_environment_block(spec.env_set);
  std::wstring cwd = widen(spec.cwd);

  DWORD priority = spec.nice_level > 0 ? BELOW_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS;
  DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW |
                CREATE_NEW_PROCESS_GROUP | priority;
  PROCESS_INFORMATION info;
  ZeroMemory(&info, sizeof(info));
  BOOL created = CreateProcessW(application.c_str(), command_buffer.data(), nullptr, nullptr, TRUE, flags,
                                environment.data(), cwd.empty() ? nullptr : cwd.c_str(), &startup.StartupInfo, &info);
  DWORD create_error = GetLastError();
  CloseHandle(log_handle);
  if (null_handle != INVALID_HANDLE_VALUE) CloseHandle(null_handle);
  if (!created) {
    error = "CreateProcess failed (code " + std::to_string(create_error) + ")";
    return false;
  }

  // Put the suspended process in a Job Object before it runs a single
  // instruction: memory and CPU limits are then enforced by Windows itself,
  // and closing the job kills every child process too.
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    ZeroMemory(&limits, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (spec.memory_limit_mb > 0) {
      limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
      limits.JobMemoryLimit = static_cast<SIZE_T>(spec.memory_limit_mb) * 1024 * 1024;
    }
    if (spec.cpu_limit_sec > 0) {
      limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_TIME;
      limits.BasicLimitInformation.PerJobUserTimeLimit.QuadPart = spec.cpu_limit_sec * 10000000LL;
    }
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    if (!AssignProcessToJobObject(job, info.hProcess)) {
      log_warn("could not put job in a Job Object (code " + std::to_string(GetLastError()) +
               "); memory limit falls back to the watchdog");
      CloseHandle(job);
      job = nullptr;
    }
  }
  ResumeThread(info.hThread);
  CloseHandle(info.hThread);
  out.pid = info.dwProcessId;
  out.win_process = info.hProcess;
  out.win_job = job;
  out.start_token = process_start_token(out.pid);
  return true;
}

bool poll_job(ProcHandle& handle, ExitInfo& info) {
  if (!handle.win_process) return true;
  DWORD wait = WaitForSingleObject(static_cast<HANDLE>(handle.win_process), 0);
  if (wait == WAIT_TIMEOUT) return false;
  DWORD code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(handle.win_process), &code);
  info.exited = true;
  info.exit_code = static_cast<int>(code);
  info.win_status = code;
  return true;
}

void kill_job(ProcHandle& handle, bool) {
  if (handle.win_job) {
    TerminateJobObject(static_cast<HANDLE>(handle.win_job), 1);
  } else if (handle.win_process) {
    TerminateProcess(static_cast<HANDLE>(handle.win_process), 1);
  }
}

void release_job(ProcHandle& handle) {
  if (handle.win_process) CloseHandle(static_cast<HANDLE>(handle.win_process));
  if (handle.win_job) CloseHandle(static_cast<HANDLE>(handle.win_job));  // kills leftover children
  handle.win_process = nullptr;
  handle.win_job = nullptr;
  handle.pid = 0;
}

static uint64_t process_working_set_kb(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!process) process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!process) return 0;
  PROCESS_MEMORY_COUNTERS counters;
  ZeroMemory(&counters, sizeof(counters));
  uint64_t result = 0;
  if (GetProcessMemoryInfo(process, &counters, sizeof(counters))) result = counters.WorkingSetSize / 1024;
  CloseHandle(process);
  return result;
}

uint64_t job_rss_kb(const ProcHandle& handle) {
  if (handle.win_job) {
    struct {
      JOBOBJECT_BASIC_PROCESS_ID_LIST header;
      ULONG_PTR extra[255];
    } ids;
    ZeroMemory(&ids, sizeof(ids));
    if (QueryInformationJobObject(static_cast<HANDLE>(handle.win_job), JobObjectBasicProcessIdList, &ids,
                                  sizeof(ids), nullptr)) {
      uint64_t total = 0;
      for (DWORD index = 0; index < ids.header.NumberOfProcessIdsInList; index++) {
        total += process_working_set_kb(static_cast<DWORD>(ids.header.ProcessIdList[index]));
      }
      return total;
    }
  }
  return process_working_set_kb(static_cast<DWORD>(handle.pid));
}

uint64_t job_peak_kb(const ProcHandle& handle) {
  if (!handle.win_job) return 0;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
  ZeroMemory(&info, sizeof(info));
  if (!QueryInformationJobObject(static_cast<HANDLE>(handle.win_job), JobObjectExtendedLimitInformation, &info,
                                 sizeof(info), nullptr)) {
    return 0;
  }
  return info.PeakJobMemoryUsed / 1024;
}

int64_t process_start_token(int64_t pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (!process) return 0;
  FILETIME created, exited, kernel, user;
  int64_t token = 0;
  if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
    token = (static_cast<int64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
  }
  CloseHandle(process);
  return token;
}

bool process_alive(int64_t pid, int64_t start_token) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (!process) return false;
  DWORD code = 0;
  bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
  CloseHandle(process);
  if (!alive) return false;
  return start_token == 0 || process_start_token(pid) == start_token;
}

void kill_orphan(int64_t pid, int64_t start_token) {
  if (!process_alive(pid, start_token)) return;
  HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
  if (process) {
    TerminateProcess(process, 1);
    CloseHandle(process);
  }
}

// ---------------------------------------------------------------- run_capture

CaptureResult run_capture(const std::vector<std::string>& argv_in, const std::string& stdin_data, int timeout_ms,
                          const std::map<std::string, std::string>& env, const std::string& cwd) {
  CaptureResult result;
  if (argv_in.empty()) {
    result.error = "empty command";
    return result;
  }
  std::vector<std::string> args = argv_in;
  std::string resolved = find_in_path(args[0]);
  if (resolved.empty()) {
    result.error = args[0] + " not found on PATH";
    return result;
  }
  args[0] = resolved;

  SECURITY_ATTRIBUTES inherit_attributes;
  inherit_attributes.nLength = sizeof(inherit_attributes);
  inherit_attributes.lpSecurityDescriptor = nullptr;
  inherit_attributes.bInheritHandle = TRUE;
  HANDLE out_read = nullptr, out_write = nullptr, err_read = nullptr, err_write = nullptr;
  HANDLE in_read = nullptr, in_write = nullptr;
  CreatePipe(&out_read, &out_write, &inherit_attributes, 0);
  CreatePipe(&err_read, &err_write, &inherit_attributes, 0);
  CreatePipe(&in_read, &in_write, &inherit_attributes, 0);
  SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);

  InheritList inherit;
  inherit.init({out_write, err_write, in_read});
  STARTUPINFOEXW startup;
  ZeroMemory(&startup, sizeof(startup));
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = in_read;
  startup.StartupInfo.hStdOutput = out_write;
  startup.StartupInfo.hStdError = err_write;
  startup.lpAttributeList = inherit.list;

  std::wstring application, command_line;
  build_command(args, application, command_line);
  std::vector<wchar_t> command_buffer(command_line.begin(), command_line.end());
  command_buffer.push_back(L'\0');
  std::vector<wchar_t> environment = build_environment_block(env);
  std::wstring working_dir = widen(cwd);
  PROCESS_INFORMATION info;
  ZeroMemory(&info, sizeof(info));
  BOOL created = CreateProcessW(application.c_str(), command_buffer.data(), nullptr, nullptr, TRUE,
                                CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW |
                                    CREATE_SUSPENDED,
                                environment.data(), working_dir.empty() ? nullptr : working_dir.c_str(),
                                &startup.StartupInfo, &info);
  DWORD create_error = GetLastError();
  CloseHandle(out_write);
  CloseHandle(err_write);
  CloseHandle(in_read);
  if (!created) {
    CloseHandle(out_read);
    CloseHandle(err_read);
    CloseHandle(in_write);
    result.error = "could not start " + args[0] + " (code " + std::to_string(create_error) + ")";
    return result;
  }
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    ZeroMemory(&limits, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    AssignProcessToJobObject(job, info.hProcess);
  }
  ResumeThread(info.hThread);
  CloseHandle(info.hThread);
  result.started = true;

  std::thread out_reader([&]() {
    char buffer[8192];
    DWORD got = 0;
    while (ReadFile(out_read, buffer, sizeof(buffer), &got, nullptr) && got > 0) result.out.append(buffer, got);
  });
  std::thread err_reader([&]() {
    char buffer[8192];
    DWORD got = 0;
    while (ReadFile(err_read, buffer, sizeof(buffer), &got, nullptr) && got > 0) result.err.append(buffer, got);
  });
  std::thread in_writer([&]() {
    size_t written = 0;
    while (written < stdin_data.size()) {
      DWORD wrote = 0;
      DWORD chunk = static_cast<DWORD>(std::min<size_t>(stdin_data.size() - written, 65536));
      if (!WriteFile(in_write, stdin_data.data() + written, chunk, &wrote, nullptr) || wrote == 0) break;
      written += wrote;
    }
    CloseHandle(in_write);
  });
  DWORD wait = WaitForSingleObject(info.hProcess, timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : INFINITE);
  if (wait == WAIT_TIMEOUT) {
    result.timed_out = true;
    if (job) TerminateJobObject(job, 1);
    TerminateProcess(info.hProcess, 1);
    WaitForSingleObject(info.hProcess, 2000);
  }
  DWORD code = 0;
  GetExitCodeProcess(info.hProcess, &code);
  result.exit_code = static_cast<int>(code);
  if (job) CloseHandle(job);  // also kills grandchildren still holding the pipes
  in_writer.join();
  out_reader.join();
  err_reader.join();
  CloseHandle(out_read);
  CloseHandle(err_read);
  CloseHandle(info.hProcess);
  return result;
}

bool spawn_detached(const std::vector<std::string>& argv, const std::string& log_path, std::string& error) {
  SECURITY_ATTRIBUTES inherit_attributes;
  inherit_attributes.nLength = sizeof(inherit_attributes);
  inherit_attributes.lpSecurityDescriptor = nullptr;
  inherit_attributes.bInheritHandle = TRUE;
  HANDLE log_handle = CreateFileW(widen(log_path).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &inherit_attributes, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  InheritList inherit;
  if (log_handle != INVALID_HANDLE_VALUE) inherit.init({log_handle});
  STARTUPINFOEXW startup;
  ZeroMemory(&startup, sizeof(startup));
  startup.StartupInfo.cb = sizeof(startup);
  if (log_handle != INVALID_HANDLE_VALUE) {
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = log_handle;
    startup.StartupInfo.hStdError = log_handle;
    startup.lpAttributeList = inherit.list;
  }
  std::wstring application, command_line;
  build_command(argv, application, command_line);
  std::vector<wchar_t> command_buffer(command_line.begin(), command_line.end());
  command_buffer.push_back(L'\0');
  PROCESS_INFORMATION info;
  ZeroMemory(&info, sizeof(info));
  DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT | CREATE_BREAKAWAY_FROM_JOB;
  if (log_handle != INVALID_HANDLE_VALUE) flags |= EXTENDED_STARTUPINFO_PRESENT;
  BOOL created = CreateProcessW(application.c_str(), command_buffer.data(), nullptr, nullptr, TRUE, flags, nullptr,
                                nullptr, &startup.StartupInfo, &info);
  if (!created) {
    // The terminal may run us inside a job that forbids breakaway; retry without it.
    flags &= ~static_cast<DWORD>(CREATE_BREAKAWAY_FROM_JOB);
    created = CreateProcessW(application.c_str(), command_buffer.data(), nullptr, nullptr, TRUE, flags, nullptr,
                             nullptr, &startup.StartupInfo, &info);
  }
  if (log_handle != INVALID_HANDLE_VALUE) CloseHandle(log_handle);
  if (!created) {
    error = "CreateProcess failed (code " + std::to_string(GetLastError()) + ")";
    return false;
  }
  CloseHandle(info.hThread);
  CloseHandle(info.hProcess);
  return true;
}

// ---------------------------------------------------------------- system info

std::string self_exe_path() {
  std::vector<wchar_t> buffer(1024);
  while (true) {
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) return "";
    if (length < buffer.size()) return narrow(std::wstring(buffer.data(), length));
    buffer.resize(buffer.size() * 2);
  }
}

static bool file_exists_w(const std::wstring& path) {
  DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::string find_in_path(const std::string& name) {
  if (name.empty()) return "";
  std::vector<std::string> extensions;
  std::string lower = to_lower(name);
  bool has_extension = ends_with(lower, ".exe") || ends_with(lower, ".cmd") || ends_with(lower, ".bat") ||
                       ends_with(lower, ".com");
  if (has_extension) {
    extensions.push_back("");
  } else {
    std::string pathext = getenv_str("PATHEXT");
    if (pathext.empty()) pathext = ".COM;.EXE;.BAT;.CMD";
    for (const std::string& item : split(pathext, ';')) {
      if (!item.empty()) extensions.push_back(to_lower(item));
    }
  }
  bool has_directory = name.find('\\') != std::string::npos || name.find('/') != std::string::npos ||
                       name.find(':') != std::string::npos;
  if (has_directory) {
    for (const std::string& extension : extensions) {
      if (file_exists_w(widen(name + extension))) return name + extension;
    }
    return "";
  }
  std::string path_value = getenv_str("PATH");
  for (std::string directory : split(path_value, ';')) {
    directory = trim(directory);
    if (directory.size() >= 2 && directory.front() == '"' && directory.back() == '"') {
      directory = directory.substr(1, directory.size() - 2);
    }
    if (directory.empty()) continue;
    if (directory.back() != '\\' && directory.back() != '/') directory.push_back('\\');
    for (const std::string& extension : extensions) {
      std::string candidate = directory + name + extension;
      if (file_exists_w(widen(candidate))) return candidate;
    }
  }
  return "";
}

fs::path home_dir() {
  std::string profile = getenv_str("USERPROFILE");
  if (!profile.empty()) return u8path(profile);
  std::string drive = getenv_str("HOMEDRIVE");
  std::string path = getenv_str("HOMEPATH");
  if (!drive.empty() && !path.empty()) return u8path(drive + path);
  return u8path("C:\\");
}

std::string host_name() {
  wchar_t buffer[256];
  DWORD size = 256;
  if (GetComputerNameExW(ComputerNameDnsHostname, buffer, &size) && size > 0) {
    return narrow(std::wstring(buffer, size));
  }
  return "maas-node";
}

std::string os_name() { return "windows"; }

std::string os_pretty() {
  typedef LONG(WINAPI * RtlGetVersionFunction)(PRTL_OSVERSIONINFOW);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  RTL_OSVERSIONINFOW version;
  ZeroMemory(&version, sizeof(version));
  version.dwOSVersionInfoSize = sizeof(version);
  if (ntdll) {
    auto function = reinterpret_cast<RtlGetVersionFunction>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
    if (function) function(&version);
  }
  if (version.dwMajorVersion == 0) return "Windows";
  std::string name = "Windows " + std::to_string(version.dwMajorVersion);
  if (version.dwMajorVersion == 10 && version.dwBuildNumber >= 22000) name = "Windows 11";
  if (version.dwMajorVersion == 6 && version.dwMinorVersion == 1) name = "Windows 7";
  if (version.dwMajorVersion == 6 && version.dwMinorVersion >= 2) name = "Windows 8";
  return name + " (build " + std::to_string(version.dwBuildNumber) + ")";
}

std::string arch_name() {
  typedef BOOL(WINAPI * IsWow64Process2Function)(HANDLE, USHORT*, USHORT*);
  HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
  if (kernel) {
    auto function = reinterpret_cast<IsWow64Process2Function>(
        reinterpret_cast<void*>(GetProcAddress(kernel, "IsWow64Process2")));
    USHORT process_machine = 0, native_machine = 0;
    if (function && function(GetCurrentProcess(), &process_machine, &native_machine)) {
      if (native_machine == 0xAA64) return "arm64";
      if (native_machine == 0x8664) return "x86_64";
      if (native_machine == 0x014c) return "x86";
    }
  }
  SYSTEM_INFO info;
  GetNativeSystemInfo(&info);
  switch (info.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: return "x86_64";
    case PROCESSOR_ARCHITECTURE_ARM64: return "arm64";
    case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
    default: return "unknown";
  }
}

MemInfo mem_info() {
  MemInfo info;
  MEMORYSTATUSEX status;
  status.dwLength = sizeof(status);
  if (GlobalMemoryStatusEx(&status)) {
    info.total_kb = status.ullTotalPhys / 1024;
    info.available_kb = status.ullAvailPhys / 1024;
  }
  return info;
}

std::string getenv_str(const char* name) {
  std::wstring wide_name = widen(name);
  DWORD size = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
  if (size == 0) return "";
  std::wstring value(size, L'\0');
  DWORD got = GetEnvironmentVariableW(wide_name.c_str(), &value[0], size);
  value.resize(got);
  return narrow(value);
}

static bool handle_is_console(DWORD which) {
  HANDLE handle = GetStdHandle(which);
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return false;
  DWORD mode = 0;
  return GetFileType(handle) == FILE_TYPE_CHAR && GetConsoleMode(handle, &mode);
}

bool is_stdin_tty() { return handle_is_console(STD_INPUT_HANDLE); }
bool is_stdout_tty() { return handle_is_console(STD_OUTPUT_HANDLE); }

void console_setup() {
  ensure_winsock();
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  bool colour = false;
  HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  if (output != INVALID_HANDLE_VALUE && GetConsoleMode(output, &mode)) {
    // ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004 (Windows 10 and later).
    if (SetConsoleMode(output, mode | 0x0004)) colour = true;
  }
  if (!getenv_str("NO_COLOR").empty()) colour = false;
  set_color_enabled(colour);
  // Keep stdin/stdout binary so the MCP bridge writes plain \n line endings.
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
}

static std::function<void()> g_shutdown_callback;
static std::atomic<bool> g_shutdown_started{false};

static BOOL WINAPI console_handler(DWORD event_type) {
  if (event_type == CTRL_C_EVENT || event_type == CTRL_BREAK_EVENT || event_type == CTRL_CLOSE_EVENT ||
      event_type == CTRL_LOGOFF_EVENT || event_type == CTRL_SHUTDOWN_EVENT) {
    if (!g_shutdown_started.exchange(true) && g_shutdown_callback) {
      std::thread([]() { g_shutdown_callback(); }).detach();
    }
    // Closing the window only gives us a few seconds; use them to clean up.
    if (event_type == CTRL_CLOSE_EVENT) Sleep(3000);
    return TRUE;
  }
  return FALSE;
}

void install_shutdown_handler(std::function<void()> on_shutdown) {
  g_shutdown_callback = on_shutdown;
  SetConsoleCtrlHandler(console_handler, TRUE);
}

static std::string read_console_line(bool echo) {
  HANDLE input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, 0, nullptr);
  if (input == INVALID_HANDLE_VALUE) {
    std::string line;
    std::getline(std::cin, line);
    return trim(line);
  }
  DWORD saved_mode = 0;
  GetConsoleMode(input, &saved_mode);
  if (!echo) SetConsoleMode(input, saved_mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));
  std::wstring line;
  wchar_t buffer[512];
  DWORD got = 0;
  while (ReadConsoleW(input, buffer, 512, &got, nullptr) && got > 0) {
    line.append(buffer, got);
    if (line.find(L'\n') != std::wstring::npos) break;
  }
  if (!echo) {
    SetConsoleMode(input, saved_mode);
    console_print("\n");
  }
  CloseHandle(input);
  return trim(narrow(line));
}

std::string read_line_tty(const std::string& prompt) {
  console_print(prompt);
  return read_console_line(true);
}

std::string read_secret_line(const std::string& prompt) {
  console_print(prompt);
  return read_console_line(false);
}

bool is_wsl() { return false; }
void set_private_permissions(const fs::path&) {}

bool is_running_as_admin() {
  BOOL is_member = FALSE;
  SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
  PSID admin_group = nullptr;
  if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                               &admin_group)) {
    CheckTokenMembership(nullptr, admin_group, &is_member);
    FreeSid(admin_group);
  }
  return is_member == TRUE;
}

// ---------------------------------------------------------------- network

std::vector<InterfaceAddr> list_ipv4_interfaces() {
  ensure_winsock();
  std::vector<InterfaceAddr> result;
  ULONG size = 32 * 1024;
  std::vector<char> buffer(size);
  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  ULONG status = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                      reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &size);
  if (status == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(size);
    status = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()),
                                  &size);
  }
  if (status != NO_ERROR) return result;
  for (auto* adapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()); adapter; adapter = adapter->Next) {
    for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
      if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
      InterfaceAddr entry;
      entry.name = narrow(adapter->FriendlyName ? adapter->FriendlyName : L"");
      char text[INET_ADDRSTRLEN];
      auto* address = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
      inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text));
      entry.ipv4 = text;
      ULONG prefix = unicast->OnLinkPrefixLength;
      uint32_t mask = prefix == 0 ? 0 : htonl(0xFFFFFFFFu << (32 - prefix));
      in_addr mask_address;
      mask_address.s_addr = mask;
      inet_ntop(AF_INET, &mask_address, text, sizeof(text));
      entry.netmask = text;
      entry.loopback = adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
      entry.up = adapter->OperStatus == IfOperStatusUp;
      entry.index = static_cast<int>(adapter->IfIndex);
      result.push_back(entry);
    }
  }
  return result;
}

std::string default_route_ipv4() {
  ensure_winsock();
  SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock == INVALID_SOCKET) return "";
  sockaddr_in target;
  std::memset(&target, 0, sizeof(target));
  target.sin_family = AF_INET;
  target.sin_port = htons(53);
  inet_pton(AF_INET, "8.8.8.8", &target.sin_addr);
  std::string result;
  if (connect(sock, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0) {
    sockaddr_in local;
    int length = sizeof(local);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&local), &length) == 0) {
      char text[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text));
      result = text;
    }
  }
  closesocket(sock);
  if (result == "0.0.0.0") result.clear();
  return result;
}

bool port_is_free(const std::string& host, int port) {
  ensure_winsock();
  SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == INVALID_SOCKET) return false;
  BOOL exclusive = TRUE;
  setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
  sockaddr_in address;
  std::memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<u_short>(port));
  inet_pton(AF_INET, host.empty() ? "0.0.0.0" : host.c_str(), &address.sin_addr);
  bool free_port = bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  closesocket(sock);
  return free_port;
}

// ---------------------------------------------------------------- instance lock

InstanceLock::~InstanceLock() { release(); }

bool InstanceLock::acquire(const fs::path& path) {
  ensure_dir(path.parent_path());
  // Share mode READ only: a second daemon cannot open the file for writing.
  HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  std::string pid_text = std::to_string(GetCurrentProcessId()) + "\n";
  SetFilePointer(handle, 0, nullptr, FILE_BEGIN);
  SetEndOfFile(handle);
  DWORD wrote = 0;
  WriteFile(handle, pid_text.data(), static_cast<DWORD>(pid_text.size()), &wrote, nullptr);
  handle_ = reinterpret_cast<intptr_t>(handle);
  path_ = path;
  return true;
}

void InstanceLock::release() {
  if (handle_ != -1) {
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
    handle_ = -1;
  }
}

bool lock_is_held(const fs::path& path) {
  HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
  CloseHandle(handle);
  return false;
}

}  // namespace maas

#endif  // _WIN32
