#include "common.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <stdlib.h>
#else
#include <sys/random.h>
#endif
#endif

namespace maas {

const char* MAAS_VERSION = "0.1.0";
const int MAAS_DEFAULT_PORT = 7420;

// ---------------------------------------------------------------- time

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

int64_t now_sec() { return now_ms() / 1000; }

int64_t mono_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string iso_time(int64_t unix_seconds) {
  std::time_t value = static_cast<std::time_t>(unix_seconds);
  std::tm parts{};
#ifdef _WIN32
  gmtime_s(&parts, &value);
#else
  gmtime_r(&value, &parts);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &parts);
  return buffer;
}

std::string format_duration(double seconds) {
  char buffer[64];
  if (seconds < 1) {
    std::snprintf(buffer, sizeof(buffer), "%.0f ms", seconds * 1000);
  } else if (seconds < 120) {
    std::snprintf(buffer, sizeof(buffer), "%.1f s", seconds);
  } else if (seconds < 7200) {
    long long whole_seconds = static_cast<long long>(seconds);
    std::snprintf(buffer, sizeof(buffer), "%lld min %lld s", whole_seconds / 60, whole_seconds % 60);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.1f h", seconds / 3600);
  }
  return buffer;
}

// ---------------------------------------------------------------- strings

std::string trim(const std::string& text) {
  size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) start++;
  size_t end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) end--;
  return text.substr(start, end - start);
}

std::string to_lower(std::string text) {
  for (char& character : text) {
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
  }
  return text;
}

bool starts_with(const std::string& text, const std::string& prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> split(const std::string& text, char separator) {
  std::vector<std::string> parts;
  std::string current;
  for (char character : text) {
    if (character == separator) {
      parts.push_back(current);
      current.clear();
    } else {
      current.push_back(character);
    }
  }
  parts.push_back(current);
  return parts;
}

std::string join(const std::vector<std::string>& parts, const std::string& separator) {
  std::string result;
  for (size_t index = 0; index < parts.size(); index++) {
    if (index > 0) result += separator;
    result += parts[index];
  }
  return result;
}

std::string replace_all(std::string text, const std::string& from, const std::string& to) {
  if (from.empty()) return text;
  size_t position = 0;
  while ((position = text.find(from, position)) != std::string::npos) {
    text.replace(position, from.size(), to);
    position += to.size();
  }
  return text;
}

std::string human_bytes(uint64_t bytes) {
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024 && unit < 4) {
    value /= 1024;
    unit++;
  }
  char buffer[32];
  if (unit == 0) {
    std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.1f %s", value, units[unit]);
  }
  return buffer;
}

std::string digits_only(const std::string& text) {
  std::string result;
  for (char character : text) {
    if (character >= '0' && character <= '9') result.push_back(character);
  }
  return result;
}

// Returns the length of a valid UTF-8 sequence starting at position, or 0.
static size_t utf8_sequence_length(const std::string& text, size_t position) {
  const unsigned char first = static_cast<unsigned char>(text[position]);
  if (first < 0x80) return 1;
  size_t length = 0;
  uint32_t code_point = 0;
  if ((first & 0xE0) == 0xC0) {
    length = 2;
    code_point = first & 0x1F;
  } else if ((first & 0xF0) == 0xE0) {
    length = 3;
    code_point = first & 0x0F;
  } else if ((first & 0xF8) == 0xF0) {
    length = 4;
    code_point = first & 0x07;
  } else {
    return 0;
  }
  if (position + length > text.size()) return 0;
  for (size_t offset = 1; offset < length; offset++) {
    const unsigned char next = static_cast<unsigned char>(text[position + offset]);
    if ((next & 0xC0) != 0x80) return 0;
    code_point = (code_point << 6) | (next & 0x3F);
  }
  // Reject overlong forms, UTF-16 surrogates and values past U+10FFFF.
  if (length == 2 && code_point < 0x80) return 0;
  if (length == 3 && code_point < 0x800) return 0;
  if (length == 4 && (code_point < 0x10000 || code_point > 0x10FFFF)) return 0;
  if (code_point >= 0xD800 && code_point <= 0xDFFF) return 0;
  return length;
}

bool is_valid_utf8(const std::string& text) {
  size_t position = 0;
  while (position < text.size()) {
    if (static_cast<unsigned char>(text[position]) < 0x80) {
      position++;
      continue;
    }
    size_t length = utf8_sequence_length(text, position);
    if (length == 0) return false;
    position += length;
  }
  return true;
}

std::string sanitize_utf8(const std::string& text) {
  if (is_valid_utf8(text)) return text;
  std::string result;
  result.reserve(text.size() + 16);
  size_t position = 0;
  while (position < text.size()) {
    size_t length = utf8_sequence_length(text, position);
    if (length == 0) {
      result += "\xEF\xBF\xBD";  // U+FFFD replacement character
      position++;
    } else {
      result.append(text, position, length);
      position += length;
    }
  }
  return result;
}

size_t utf8_safe_length(const std::string& text, size_t max_len) {
  if (text.size() <= max_len) return text.size();
  size_t length = max_len;
  // Step back over continuation bytes (10xxxxxx) so we do not split a character.
  while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) length--;
  return length;
}

std::string truncate_utf8(const std::string& text, size_t max_len) {
  if (text.size() <= max_len) return text;
  return text.substr(0, utf8_safe_length(text, max_len)) + "…";
}

std::string strip_ansi(const std::string& text) {
  std::string result;
  result.reserve(text.size());
  size_t position = 0;
  while (position < text.size()) {
    char character = text[position];
    if (character == '\x1b' && position + 1 < text.size() && text[position + 1] == '[') {
      position += 2;
      while (position < text.size()) {
        char code = text[position];
        position++;
        if (code >= 0x40 && code <= 0x7E) break;  // final byte of the escape sequence
      }
      continue;
    }
    if (character == '\r') {
      // Progress bars rewrite the same line with \r; keep only real line breaks.
      if (position + 1 < text.size() && text[position + 1] == '\n') {
        position++;
        continue;
      }
      result.push_back('\n');
      position++;
      continue;
    }
    result.push_back(character);
    position++;
  }
  return result;
}

std::string html_escape(const std::string& text) {
  std::string result;
  result.reserve(text.size());
  for (char character : text) {
    switch (character) {
      case '&': result += "&amp;"; break;
      case '<': result += "&lt;"; break;
      case '>': result += "&gt;"; break;
      case '"': result += "&quot;"; break;
      default: result.push_back(character);
    }
  }
  return result;
}

std::string dump_json(const json& value, int indent) {
  return value.dump(indent, ' ', false, json::error_handler_t::replace);
}

bool parse_int64(const std::string& text, int64_t& out) {
  if (text.empty() || text.size() > 19) return false;
  size_t position = 0;
  bool negative = false;
  if (text[0] == '-' || text[0] == '+') {
    negative = text[0] == '-';
    position = 1;
    if (text.size() == 1) return false;
  }
  int64_t value = 0;
  for (; position < text.size(); position++) {
    char character = text[position];
    if (character < '0' || character > '9') return false;
    value = value * 10 + (character - '0');
  }
  out = negative ? -value : value;
  return true;
}

// ---------------------------------------------------------------- random

std::string random_bytes(size_t count) {
  std::string result(count, '\0');
#ifdef _WIN32
  BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&result[0]), static_cast<ULONG>(count),
                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#elif defined(__APPLE__)
  arc4random_buf(&result[0], count);
#else
  size_t filled = 0;
  while (filled < count) {
    ssize_t got = getrandom(&result[filled], count - filled, 0);
    if (got > 0) {
      filled += static_cast<size_t>(got);
    } else {
      // Very old kernels without getrandom: fall back to /dev/urandom.
      int file = open("/dev/urandom", O_RDONLY);
      if (file >= 0) {
        ssize_t read_count = read(file, &result[filled], count - filled);
        close(file);
        if (read_count > 0) filled += static_cast<size_t>(read_count);
      }
    }
  }
#endif
  return result;
}

std::string random_hex(size_t byte_count) {
  static const char* hex_digits = "0123456789abcdef";
  std::string bytes = random_bytes(byte_count);
  std::string result;
  for (unsigned char byte : bytes) {
    result.push_back(hex_digits[byte >> 4]);
    result.push_back(hex_digits[byte & 15]);
  }
  return result;
}

std::string random_base32(size_t char_count) {
  // 32 characters with no 0/o/1/l/i so codes are easy to read aloud.
  static const char* alphabet = "abcdefghjkmnpqrstuvwxyz23456789";
  static const size_t alphabet_size = 31;
  std::string result;
  while (result.size() < char_count) {
    std::string bytes = random_bytes(char_count);
    for (unsigned char byte : bytes) {
      if (byte >= 248) continue;  // 248 = 31 * 8, rejects bias
      result.push_back(alphabet[byte % alphabet_size]);
      if (result.size() == char_count) break;
    }
  }
  return result;
}

std::string random_digits(size_t count) {
  std::string result;
  while (result.size() < count) {
    std::string bytes = random_bytes(count);
    for (unsigned char byte : bytes) {
      if (byte >= 250) continue;
      result.push_back(static_cast<char>('0' + byte % 10));
      if (result.size() == count) break;
    }
  }
  return result;
}

// ---------------------------------------------------------------- SHA-256

static const uint32_t SHA_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static inline uint32_t rotate_right(uint32_t value, int bits) {
  return (value >> bits) | (value << (32 - bits));
}

Sha256::Sha256() : total_length_(0), buffer_length_(0) {
  const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::memcpy(state_, initial, sizeof(state_));
}

void Sha256::transform(const uint8_t* block) {
  uint32_t words[64];
  for (int index = 0; index < 16; index++) {
    words[index] = (uint32_t(block[index * 4]) << 24) | (uint32_t(block[index * 4 + 1]) << 16) |
                   (uint32_t(block[index * 4 + 2]) << 8) | uint32_t(block[index * 4 + 3]);
  }
  for (int index = 16; index < 64; index++) {
    uint32_t s0 = rotate_right(words[index - 15], 7) ^ rotate_right(words[index - 15], 18) ^
                  (words[index - 15] >> 3);
    uint32_t s1 = rotate_right(words[index - 2], 17) ^ rotate_right(words[index - 2], 19) ^
                  (words[index - 2] >> 10);
    words[index] = words[index - 16] + s0 + words[index - 7] + s1;
  }
  uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
  for (int index = 0; index < 64; index++) {
    uint32_t big_s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
    uint32_t choose = (e & f) ^ (~e & g);
    uint32_t temp1 = h + big_s1 + choose + SHA_K[index] + words[index];
    uint32_t big_s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
    uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    uint32_t temp2 = big_s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
  state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha256::update(const void* data, size_t length) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  total_length_ += length;
  while (length > 0) {
    size_t take = 64 - buffer_length_;
    if (take > length) take = length;
    std::memcpy(buffer_ + buffer_length_, bytes, take);
    buffer_length_ += take;
    bytes += take;
    length -= take;
    if (buffer_length_ == 64) {
      transform(buffer_);
      buffer_length_ = 0;
    }
  }
}

std::string Sha256::hex_digest() {
  uint64_t bit_length = total_length_ * 8;
  uint8_t padding = 0x80;
  update(&padding, 1);
  uint8_t zero = 0;
  while (buffer_length_ != 56) update(&zero, 1);
  uint8_t length_bytes[8];
  for (int index = 0; index < 8; index++) {
    length_bytes[index] = static_cast<uint8_t>(bit_length >> (56 - 8 * index));
  }
  update(length_bytes, 8);
  static const char* hex_digits = "0123456789abcdef";
  std::string result;
  for (int index = 0; index < 8; index++) {
    for (int shift = 28; shift >= 0; shift -= 4) {
      result.push_back(hex_digits[(state_[index] >> shift) & 15]);
    }
  }
  return result;
}

std::string sha256_hex(const std::string& data) {
  Sha256 hasher;
  hasher.update(data);
  return hasher.hex_digest();
}

bool sha256_file(const fs::path& path, std::string& out_hex, std::string& error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "cannot open " + path_str(path);
    return false;
  }
  Sha256 hasher;
  std::vector<char> buffer(1 << 16);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    std::streamsize got = input.gcount();
    if (got > 0) hasher.update(buffer.data(), static_cast<size_t>(got));
  }
  out_hex = hasher.hex_digest();
  return true;
}

bool constant_time_equals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char difference = 0;
  for (size_t index = 0; index < a.size(); index++) {
    difference |= static_cast<unsigned char>(a[index] ^ b[index]);
  }
  return difference == 0;
}

uint64_t hash64(const char* data, size_t length) {
  // FNV-1a followed by a splitmix64 finish so nearby values spread out well.
  uint64_t hash = 1469598103934665603ULL;
  for (size_t index = 0; index < length; index++) {
    hash ^= static_cast<unsigned char>(data[index]);
    hash *= 1099511628211ULL;
  }
  hash ^= hash >> 30;
  hash *= 0xbf58476d1ce4e5b9ULL;
  hash ^= hash >> 27;
  hash *= 0x94d049bb133111ebULL;
  hash ^= hash >> 31;
  return hash;
}

// ---------------------------------------------------------------- files

fs::path u8path(const std::string& utf8_text) {
#ifdef _WIN32
  return fs::u8path(utf8_text);
#else
  return fs::path(utf8_text);
#endif
}

std::string path_str(const fs::path& path) {
#ifdef _WIN32
  auto converted = path.u8string();
  return std::string(converted.begin(), converted.end());
#else
  return path.string();
#endif
}

bool read_file(const fs::path& path, std::string& out, size_t max_bytes) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  out.clear();
  std::vector<char> buffer(1 << 16);
  while (input && out.size() < max_bytes) {
    size_t want = buffer.size();
    if (max_bytes - out.size() < want) want = max_bytes - out.size();
    input.read(buffer.data(), static_cast<std::streamsize>(want));
    std::streamsize got = input.gcount();
    if (got <= 0) break;
    out.append(buffer.data(), static_cast<size_t>(got));
  }
  return true;
}

bool read_file_range(const fs::path& path, uint64_t offset, size_t max_bytes, std::string& out) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  input.seekg(static_cast<std::streamoff>(offset));
  out.assign(max_bytes, '\0');
  input.read(&out[0], static_cast<std::streamsize>(max_bytes));
  std::streamsize got = input.gcount();
  out.resize(got > 0 ? static_cast<size_t>(got) : 0);
  return true;
}

uint64_t file_size_or_zero(const fs::path& path) {
  std::error_code error;
  uint64_t size = fs::file_size(path, error);
  return error ? 0 : size;
}

#ifndef _WIN32
static void make_private(const fs::path& path) { chmod(path.c_str(), 0600); }
#endif

bool write_file_atomic(const fs::path& path, const std::string& data, bool private_file) {
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  fs::path temp = path;
  temp += ".tmp-" + random_hex(4);
  {
    std::ofstream output(temp, std::ios::binary | std::ios::trunc);
    if (!output) return false;
#ifndef _WIN32
    if (private_file) make_private(temp);
#else
    (void)private_file;
#endif
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    output.flush();
    if (!output) {
      output.close();
      fs::remove(temp, error);
      return false;
    }
  }
  std::string rename_error;
  if (!replace_file(temp, path, rename_error)) {
    fs::remove(temp, error);
    return false;
  }
  return true;
}

bool replace_file(const fs::path& from, const fs::path& to, std::string& error_text) {
#ifdef _WIN32
  std::wstring source = from.wstring();
  std::wstring target = to.wstring();
  if (MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return true;
  }
  error_text = "MoveFileEx failed with code " + std::to_string(GetLastError());
  return false;
#else
  std::error_code error;
  fs::rename(from, to, error);
  if (error) {
    error_text = error.message();
    return false;
  }
  return true;
#endif
}

void ensure_dir(const fs::path& path) {
  std::error_code error;
  fs::create_directories(path, error);
}

uint64_t free_disk_bytes(const fs::path& path) {
  std::error_code error;
  fs::path probe = path;
  // space() needs an existing path; walk up until one exists.
  while (!probe.empty() && !fs::exists(probe, error)) {
    fs::path parent = probe.parent_path();
    if (parent == probe) break;
    probe = parent;
  }
  fs::space_info info = fs::space(probe, error);
  if (error) return 0;
  return info.available;
}

// ---------------------------------------------------------------- logging

static std::mutex g_console_mutex;
static std::mutex g_log_mutex;
static FILE* g_log_file = nullptr;
static fs::path g_log_path;
static bool g_verbose = false;
static bool g_echo = true;
static std::atomic<bool> g_color{false};

void log_init(const fs::path& log_file, bool verbose, bool echo_to_console) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  g_verbose = verbose;
  g_echo = echo_to_console;
  if (g_log_file) {
    std::fclose(g_log_file);
    g_log_file = nullptr;
  }
  if (log_file.empty()) return;
  ensure_dir(log_file.parent_path());
  // Rotate once the log passes 10 MB so an old laptop's disk never fills up.
  if (file_size_or_zero(log_file) > 10ull * 1024 * 1024) {
    std::error_code error;
    fs::path old_log = log_file;
    old_log += ".1";
    fs::remove(old_log, error);
    fs::rename(log_file, old_log, error);
  }
  g_log_path = log_file;
#ifdef _WIN32
  g_log_file = _wfopen(log_file.wstring().c_str(), L"ab");
#else
  g_log_file = std::fopen(log_file.c_str(), "ab");
#endif
}

void log_msg(LogLevel level, const std::string& message) {
  if (level == LogLevel::Debug && !g_verbose) return;
  const char* label = "INFO";
  if (level == LogLevel::Warn) label = "WARN";
  if (level == LogLevel::Error) label = "ERROR";
  if (level == LogLevel::Debug) label = "DEBUG";
  std::string line = iso_time(now_sec()) + " " + label + " " + sanitize_utf8(message) + "\n";
  {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_file) {
      std::fwrite(line.data(), 1, line.size(), g_log_file);
      std::fflush(g_log_file);
    }
  }
  if (g_echo && level != LogLevel::Debug) {
    std::string shown = message;
    if (level == LogLevel::Warn) shown = c_yellow("⚠ " + message);
    if (level == LogLevel::Error) shown = c_red("✗ " + message);
    console_line(shown);
  } else if (g_echo && g_verbose) {
    console_line(c_dim(message));
  }
}

void log_info(const std::string& message) { log_msg(LogLevel::Info, message); }
void log_warn(const std::string& message) { log_msg(LogLevel::Warn, message); }
void log_error(const std::string& message) { log_msg(LogLevel::Error, message); }
void log_debug(const std::string& message) { log_msg(LogLevel::Debug, message); }

void console_print(const std::string& text) {
  std::lock_guard<std::mutex> lock(g_console_mutex);
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fflush(stdout);
}

void console_line(const std::string& text) { console_print(text + "\n"); }

void console_error(const std::string& text) {
  std::lock_guard<std::mutex> lock(g_console_mutex);
  std::string line = text + "\n";
  std::fwrite(line.data(), 1, line.size(), stderr);
  std::fflush(stderr);
}

bool color_enabled() { return g_color.load(); }
void set_color_enabled(bool enabled) { g_color.store(enabled); }

static std::string paint(const char* code, const std::string& text) {
  if (!g_color.load()) return text;
  return std::string("\x1b[") + code + "m" + text + "\x1b[0m";
}

std::string c_bold(const std::string& text) { return paint("1", text); }
std::string c_dim(const std::string& text) { return paint("2", text); }
std::string c_green(const std::string& text) { return paint("32", text); }
std::string c_yellow(const std::string& text) { return paint("33", text); }
std::string c_red(const std::string& text) { return paint("31", text); }
std::string c_cyan(const std::string& text) { return paint("36", text); }

// ---------------------------------------------------------------- json

std::string json_str(const json& object, const char* key, const std::string& fallback) {
  if (!object.is_object()) return fallback;
  auto found = object.find(key);
  if (found == object.end() || found->is_null()) return fallback;
  if (found->is_string()) return found->get<std::string>();
  return dump_json(*found);
}

int64_t json_int(const json& object, const char* key, int64_t fallback) {
  if (!object.is_object()) return fallback;
  auto found = object.find(key);
  if (found == object.end() || found->is_null()) return fallback;
  if (found->is_number_integer() || found->is_number_unsigned()) return found->get<int64_t>();
  if (found->is_number_float()) return static_cast<int64_t>(found->get<double>());
  if (found->is_boolean()) return found->get<bool>() ? 1 : 0;
  if (found->is_string()) {
    int64_t value = 0;
    if (parse_int64(trim(found->get<std::string>()), value)) return value;
  }
  return fallback;
}

bool json_bool(const json& object, const char* key, bool fallback) {
  if (!object.is_object()) return fallback;
  auto found = object.find(key);
  if (found == object.end() || found->is_null()) return fallback;
  if (found->is_boolean()) return found->get<bool>();
  if (found->is_number()) return found->get<double>() != 0;
  if (found->is_string()) {
    std::string value = to_lower(found->get<std::string>());
    return value == "true" || value == "yes" || value == "1";
  }
  return fallback;
}

}  // namespace maas
