// MAAS - Make Anything Server
// Shared helpers used by every part of the program: strings, time, hashing,
// random tokens, files and logging.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "json.hpp"

namespace maas {

using json = nlohmann::json;
namespace fs = std::filesystem;

extern const char* MAAS_VERSION;
extern const int MAAS_DEFAULT_PORT;

// ---------- time ----------
int64_t now_ms();   // wall clock, milliseconds since 1970
int64_t now_sec();  // wall clock, seconds since 1970
int64_t mono_ms();  // steady clock, only for measuring durations
std::string iso_time(int64_t unix_seconds);
std::string format_duration(double seconds);

// ---------- strings ----------
std::string trim(const std::string& text);
std::string to_lower(std::string text);
bool starts_with(const std::string& text, const std::string& prefix);
bool ends_with(const std::string& text, const std::string& suffix);
std::vector<std::string> split(const std::string& text, char separator);
std::string join(const std::vector<std::string>& parts, const std::string& separator);
std::string replace_all(std::string text, const std::string& from, const std::string& to);
std::string human_bytes(uint64_t bytes);
std::string digits_only(const std::string& text);
// Replace invalid UTF-8 byte sequences with U+FFFD so the text is safe in JSON.
std::string sanitize_utf8(const std::string& text);
bool is_valid_utf8(const std::string& text);
// Largest length <= max_len that does not cut a UTF-8 character in half.
size_t utf8_safe_length(const std::string& text, size_t max_len);
std::string truncate_utf8(const std::string& text, size_t max_len);
std::string strip_ansi(const std::string& text);
std::string html_escape(const std::string& text);
// json::dump that never throws on bad UTF-8 (it replaces bad bytes instead).
std::string dump_json(const json& value, int indent = -1);
bool parse_int64(const std::string& text, int64_t& out);

// ---------- random values and hashing ----------
std::string random_bytes(size_t count);
std::string random_hex(size_t byte_count);
std::string random_base32(size_t char_count);  // lowercase, no ambiguous letters
std::string random_digits(size_t count);

class Sha256 {
 public:
  Sha256();
  void update(const void* data, size_t length);
  void update(const std::string& data) { update(data.data(), data.size()); }
  std::string hex_digest();  // finishes the hash

 private:
  void transform(const uint8_t* block);
  uint32_t state_[8];
  uint8_t buffer_[64];
  uint64_t total_length_;
  size_t buffer_length_;
};

std::string sha256_hex(const std::string& data);
bool sha256_file(const fs::path& path, std::string& out_hex, std::string& error);
bool constant_time_equals(const std::string& a, const std::string& b);
uint64_t hash64(const char* data, size_t length);  // fast non-crypto hash

// ---------- files ----------
fs::path u8path(const std::string& utf8_text);
std::string path_str(const fs::path& path);  // always UTF-8
bool read_file(const fs::path& path, std::string& out, size_t max_bytes = SIZE_MAX);
bool read_file_range(const fs::path& path, uint64_t offset, size_t max_bytes, std::string& out);
uint64_t file_size_or_zero(const fs::path& path);
// Writes to a temp file next to the target, then renames it into place.
bool write_file_atomic(const fs::path& path, const std::string& data, bool private_file = false);
void ensure_dir(const fs::path& path);
uint64_t free_disk_bytes(const fs::path& path);
bool replace_file(const fs::path& from, const fs::path& to, std::string& error);

// ---------- console and logging ----------
enum class LogLevel { Debug, Info, Warn, Error };
void log_init(const fs::path& log_file, bool verbose, bool echo_to_console);
void log_msg(LogLevel level, const std::string& message);
void log_info(const std::string& message);
void log_warn(const std::string& message);
void log_error(const std::string& message);
void log_debug(const std::string& message);
void console_print(const std::string& text);  // thread-safe, no newline added
void console_line(const std::string& text);   // thread-safe, adds newline
void console_error(const std::string& text);  // to stderr
bool color_enabled();
void set_color_enabled(bool enabled);
std::string c_bold(const std::string& text);
std::string c_dim(const std::string& text);
std::string c_green(const std::string& text);
std::string c_yellow(const std::string& text);
std::string c_red(const std::string& text);
std::string c_cyan(const std::string& text);

// ---------- small JSON helpers ----------
std::string json_str(const json& object, const char* key, const std::string& fallback = "");
int64_t json_int(const json& object, const char* key, int64_t fallback = 0);
bool json_bool(const json& object, const char* key, bool fallback = false);

}  // namespace maas
