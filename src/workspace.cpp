#include "workspace.hpp"

#include <algorithm>
#include <chrono>

namespace maas {

static bool equal_component(const std::string& a, const std::string& b) {
#if defined(_WIN32) || defined(__APPLE__)
  return to_lower(a) == to_lower(b);  // case-insensitive file systems
#else
  return a == b;
#endif
}

bool path_is_inside(const fs::path& root, const fs::path& candidate) {
  auto root_part = root.begin();
  auto candidate_part = candidate.begin();
  for (; root_part != root.end(); ++root_part, ++candidate_part) {
    if (root_part->empty()) continue;
    if (candidate_part == candidate.end()) return false;
    if (!equal_component(path_str(*root_part), path_str(*candidate_part))) return false;
  }
  return true;
}

#ifdef _WIN32
static bool is_reserved_windows_name(const std::string& component) {
  std::string base = to_lower(component);
  size_t dot = base.find('.');
  if (dot != std::string::npos) base = base.substr(0, dot);
  static const char* reserved[] = {"con", "prn", "aux", "nul", "conin$", "conout$"};
  for (const char* name : reserved) {
    if (base == name) return true;
  }
  if (base.size() == 4 && (starts_with(base, "com") || starts_with(base, "lpt")) && base[3] >= '0' && base[3] <= '9') {
    return true;
  }
  return false;
}
#endif

JailResult resolve_in_workspace(const fs::path& workspace, const std::string& user_path_in, bool must_exist) {
  JailResult result;
  std::string user_path = user_path_in;
  if (user_path.find('\0') != std::string::npos || user_path.size() > 1024) {
    result.error = "invalid path";
    return result;
  }
  user_path = replace_all(trim(user_path), "\\", "/");
  // Let the AI pass "workspace/x" or "~/.maas/workspace/x" without failing.
  for (const char* prefix : {"./", "workspace/", "~/.maas/workspace/"}) {
    if (starts_with(user_path, prefix)) user_path = user_path.substr(std::string(prefix).size());
  }
  if (user_path == "." || user_path == "workspace") user_path.clear();
  if (starts_with(user_path, "/") || starts_with(user_path, "~") ||
      (user_path.size() >= 2 && user_path[1] == ':')) {
    result.error = "use a path relative to the workspace (like \"scripts/clean.py\"), not an absolute path";
    return result;
  }
  std::vector<std::string> components;
  for (const std::string& part : split(user_path, '/')) {
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      result.error = "\"..\" is not allowed: paths must stay inside the workspace";
      return result;
    }
#ifdef _WIN32
    if (part.find(':') != std::string::npos) {
      result.error = "\":\" is not allowed in file names on Windows";
      return result;
    }
    if (is_reserved_windows_name(part)) {
      result.error = "\"" + part + "\" is a reserved device name on Windows";
      return result;
    }
    if (part.back() == '.' || part.back() == ' ') {
      result.error = "file names may not end with a dot or space on Windows";
      return result;
    }
    for (size_t index = 0; index + 1 < part.size(); index++) {
      // 8.3 short names like PROGRA~1 can alias other folders.
      if (part[index] == '~' && part[index + 1] >= '0' && part[index + 1] <= '9') {
        result.error = "short (8.3) file names with ~N are not allowed";
        return result;
      }
    }
#endif
    for (char character : part) {
      if (static_cast<unsigned char>(character) < 0x20) {
        result.error = "control characters are not allowed in file names";
        return result;
      }
    }
    components.push_back(part);
  }
  fs::path candidate = workspace;
  for (const std::string& part : components) candidate /= u8path(part);

  std::error_code error;
  fs::path real_root = fs::weakly_canonical(workspace, error);
  if (error) real_root = workspace;
  // weakly_canonical follows symlinks in the part of the path that exists, so a
  // symlink pointing outside the workspace is caught here.
  fs::path real_candidate = fs::weakly_canonical(candidate, error);
  if (error) real_candidate = candidate;
  if (!path_is_inside(real_root, real_candidate)) {
    result.error = "that path leads outside the workspace (through a link), so it is refused";
    return result;
  }
  if (must_exist && !fs::exists(candidate, error)) {
    result.error = "not found: " + (components.empty() ? std::string(".") : join(components, "/"));
    return result;
  }
  result.ok = true;
  result.path = candidate;
  result.relative = join(components, "/");
  return result;
}

bool looks_binary(const std::string& sample) {
  size_t check = std::min<size_t>(sample.size(), 8192);
  for (size_t index = 0; index < check; index++) {
    if (sample[index] == '\0') return true;
  }
  return false;
}

static std::string modified_iso(const fs::path& path) {
  std::error_code error;
  auto stamp = fs::last_write_time(path, error);
  if (error) return "";
  // Convert file_clock to system_clock without C++20's clock_cast.
  auto system_now = std::chrono::system_clock::now();
  auto file_now = fs::file_time_type::clock::now();
  auto converted = std::chrono::time_point_cast<std::chrono::system_clock::duration>(stamp - file_now + system_now);
  int64_t seconds = std::chrono::duration_cast<std::chrono::seconds>(converted.time_since_epoch()).count();
  return iso_time(seconds);
}

json list_workspace(const fs::path& workspace, const std::string& user_path, bool recursive, size_t max_entries,
                    std::string& error) {
  JailResult target = resolve_in_workspace(workspace, user_path, true);
  if (!target.ok) {
    error = target.error;
    return nullptr;
  }
  std::error_code fs_error;
  if (!fs::is_directory(target.path, fs_error)) {
    error = "not a folder: " + target.relative;
    return nullptr;
  }
  json entries = json::array();
  bool truncated = false;
  auto add_entry = [&](const fs::directory_entry& entry) {
    if (entries.size() >= max_entries) {
      truncated = true;
      return;
    }
    std::error_code entry_error;
    json item;
    fs::path relative = fs::relative(entry.path(), workspace, entry_error);
    item["path"] = replace_all(path_str(relative), "\\", "/");
    bool is_link = entry.is_symlink(entry_error);
    bool is_dir = entry.is_directory(entry_error);
    item["type"] = is_link ? "link" : (is_dir ? "dir" : "file");
    if (!is_dir) item["size"] = file_size_or_zero(entry.path());
    item["modified"] = modified_iso(entry.path());
    std::string name = path_str(entry.path().filename());
    if (ends_with(name, ".part")) item["note"] = "upload in progress";
    entries.push_back(item);
  };
  if (recursive) {
    fs::recursive_directory_iterator iterator(target.path, fs::directory_options::skip_permission_denied, fs_error);
    for (; !fs_error && iterator != fs::recursive_directory_iterator(); iterator.increment(fs_error)) {
      if (iterator->is_symlink()) iterator.disable_recursion_pending();
      add_entry(*iterator);
      if (truncated) break;
    }
  } else {
    fs::directory_iterator iterator(target.path, fs::directory_options::skip_permission_denied, fs_error);
    for (; !fs_error && iterator != fs::directory_iterator(); iterator.increment(fs_error)) {
      add_entry(*iterator);
      if (truncated) break;
    }
  }
  json result;
  result["folder"] = target.relative.empty() ? "." : target.relative;
  result["entries"] = entries;
  result["truncated"] = truncated;
  return result;
}

json read_workspace_file(const fs::path& workspace, const std::string& user_path, uint64_t offset, size_t max_bytes,
                         std::string& error) {
  JailResult target = resolve_in_workspace(workspace, user_path, true);
  if (!target.ok) {
    error = target.error;
    return nullptr;
  }
  std::error_code fs_error;
  if (fs::is_directory(target.path, fs_error)) {
    error = target.relative + " is a folder; use list_files";
    return nullptr;
  }
  uint64_t size = file_size_or_zero(target.path);
  std::string data;
  if (!read_file_range(target.path, offset, max_bytes, data)) {
    error = "cannot read " + target.relative;
    return nullptr;
  }
  json result;
  result["path"] = target.relative;
  result["size"] = size;
  result["offset"] = offset;
  if (looks_binary(data)) {
    result["binary"] = true;
    result["content"] = "";
    result["note"] = "binary file; use a script to inspect it";
    return result;
  }
  size_t safe = utf8_safe_length(data, data.size());
  if (offset + data.size() < size) data = data.substr(0, safe);
  result["content"] = sanitize_utf8(data);
  result["next_offset"] = offset + data.size();
  result["has_more"] = offset + data.size() < size;
  if (size <= 64ull * 1024 * 1024) {
    std::string hash, hash_error;
    if (sha256_file(target.path, hash, hash_error)) result["sha256"] = hash;
  }
  return result;
}

json write_workspace_file(const fs::path& workspace, const std::string& user_path, const std::string& content,
                          bool overwrite, uint64_t disk_floor_bytes, std::string& error_kind, std::string& error) {
  JailResult target = resolve_in_workspace(workspace, user_path, false);
  if (!target.ok || target.relative.empty()) {
    error_kind = "BAD_PATH";
    error = target.ok ? "give a file name" : target.error;
    return nullptr;
  }
  std::error_code fs_error;
  if (fs::is_directory(target.path, fs_error)) {
    error_kind = "BAD_PATH";
    error = target.relative + " is a folder";
    return nullptr;
  }
  bool existed = fs::exists(target.path, fs_error);
  if (existed && !overwrite) {
    error_kind = "EXISTS";
    error = target.relative + " already exists (pass overwrite: true, or use patch_file)";
    return nullptr;
  }
  if (free_disk_bytes(workspace) < disk_floor_bytes + content.size()) {
    error_kind = "DISK_FULL";
    error = "not enough free disk space on the node";
    return nullptr;
  }
  if (!write_file_atomic(target.path, content)) {
    error_kind = "WRITE_FAILED";
    error = "could not write " + target.relative;
    return nullptr;
  }
  json result;
  result["path"] = target.relative;
  result["size"] = content.size();
  result["sha256"] = sha256_hex(content);
  result["created"] = !existed;
  return result;
}

json patch_workspace_file(const fs::path& workspace, const std::string& user_path, const std::string& base_sha256,
                          const json& edits, std::string& error_kind, std::string& error) {
  JailResult target = resolve_in_workspace(workspace, user_path, true);
  if (!target.ok) {
    error_kind = "BAD_PATH";
    error = target.error;
    return nullptr;
  }
  std::string content;
  if (file_size_or_zero(target.path) > 8ull * 1024 * 1024 || !read_file(target.path, content)) {
    error_kind = "TOO_LARGE";
    error = "patch_file works on text files up to 8 MB";
    return nullptr;
  }
  std::string current_hash = sha256_hex(content);
  if (!base_sha256.empty() && to_lower(base_sha256) != current_hash &&
      !(base_sha256.size() >= 12 && starts_with(current_hash, to_lower(base_sha256)))) {
    // Compare-and-swap: the file changed since the AI last read it.
    error_kind = "CONFLICT";
    error = "the file changed since you read it (current sha256 " + current_hash +
            "). Read it again and redo the edit.";
    return nullptr;
  }
  if (!edits.is_array() || edits.empty()) {
    error_kind = "BAD_INPUT";
    error = "edits must be a non-empty list of {old_text, new_text}";
    return nullptr;
  }
  int applied = 0;
  for (const json& edit : edits) {
    std::string old_text = json_str(edit, "old_text");
    std::string new_text = json_str(edit, "new_text");
    if (old_text.empty()) {
      error_kind = "BAD_INPUT";
      error = "edit " + std::to_string(applied + 1) + ": old_text is empty";
      return nullptr;
    }
    size_t first = content.find(old_text);
    if (first == std::string::npos) {
      error_kind = "NO_MATCH";
      error = "edit " + std::to_string(applied + 1) + ": old_text was not found in the file";
      return nullptr;
    }
    if (content.find(old_text, first + 1) != std::string::npos) {
      error_kind = "AMBIGUOUS";
      error = "edit " + std::to_string(applied + 1) +
              ": old_text appears more than once; include more surrounding lines so it is unique";
      return nullptr;
    }
    content.replace(first, old_text.size(), new_text);
    applied++;
  }
  if (!write_file_atomic(target.path, content)) {
    error_kind = "WRITE_FAILED";
    error = "could not write " + target.relative;
    return nullptr;
  }
  json result;
  result["path"] = target.relative;
  result["edits_applied"] = applied;
  result["size"] = content.size();
  result["sha256"] = sha256_hex(content);
  return result;
}

std::string line_diff(const std::string& before, const std::string& after, size_t max_lines) {
  std::vector<std::string> old_lines = split(before, '\n');
  std::vector<std::string> new_lines = split(after, '\n');
  size_t old_count = old_lines.size(), new_count = new_lines.size();
  if (old_count * new_count > 4000000) return "(files too large to diff; showing full script instead)";
  // Longest-common-subsequence table, filled from the end.
  std::vector<std::vector<uint16_t>> table(old_count + 1, std::vector<uint16_t>(new_count + 1, 0));
  for (size_t row = old_count; row-- > 0;) {
    for (size_t column = new_count; column-- > 0;) {
      if (old_lines[row] == new_lines[column]) {
        table[row][column] = static_cast<uint16_t>(table[row + 1][column + 1] + 1);
      } else {
        table[row][column] = std::max(table[row + 1][column], table[row][column + 1]);
      }
    }
  }
  struct DiffLine {
    char mark;
    std::string text;
  };
  std::vector<DiffLine> lines;
  size_t row = 0, column = 0;
  while (row < old_count || column < new_count) {
    if (row < old_count && column < new_count && old_lines[row] == new_lines[column]) {
      lines.push_back({' ', old_lines[row]});
      row++;
      column++;
    } else if (column < new_count && (row == old_count || table[row][column + 1] >= table[row + 1][column])) {
      lines.push_back({'+', new_lines[column]});
      column++;
    } else {
      lines.push_back({'-', old_lines[row]});
      row++;
    }
  }
  // Keep changed lines plus two lines of context around them.
  std::vector<bool> keep(lines.size(), false);
  for (size_t index = 0; index < lines.size(); index++) {
    if (lines[index].mark == ' ') continue;
    size_t from = index >= 2 ? index - 2 : 0;
    for (size_t near = from; near < lines.size() && near <= index + 2; near++) keep[near] = true;
  }
  std::string output;
  size_t shown = 0;
  bool gap = false;
  for (size_t index = 0; index < lines.size(); index++) {
    if (!keep[index]) {
      gap = true;
      continue;
    }
    if (gap && !output.empty()) output += "  ...\n";
    gap = false;
    output += std::string(1, lines[index].mark) + " " + lines[index].text + "\n";
    if (++shown >= max_lines) {
      output += "  ... (diff truncated)\n";
      break;
    }
  }
  if (output.empty()) output = "(no changes)";
  return output;
}

}  // namespace maas
