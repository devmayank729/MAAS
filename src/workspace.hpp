// The workspace jail: every path the AI sends is resolved here, and anything
// that would land outside ~/.maas/workspace is refused.
#pragma once

#include "common.hpp"

namespace maas {

struct JailResult {
  bool ok = false;
  fs::path path;          // absolute path inside the workspace
  std::string relative;   // normalised "a/b/c.py" form, "" for the root
  std::string error;
};

JailResult resolve_in_workspace(const fs::path& workspace, const std::string& user_path, bool must_exist);
bool path_is_inside(const fs::path& root, const fs::path& candidate);

json list_workspace(const fs::path& workspace, const std::string& user_path, bool recursive, size_t max_entries,
                    std::string& error);
json read_workspace_file(const fs::path& workspace, const std::string& user_path, uint64_t offset,
                         size_t max_bytes, std::string& error);
json write_workspace_file(const fs::path& workspace, const std::string& user_path, const std::string& content,
                          bool overwrite, uint64_t disk_floor_bytes, std::string& error_kind, std::string& error);
json patch_workspace_file(const fs::path& workspace, const std::string& user_path, const std::string& base_sha256,
                          const json& edits, std::string& error_kind, std::string& error);
bool looks_binary(const std::string& sample);

// A short line diff (old -> new) used in approval prompts.
std::string line_diff(const std::string& before, const std::string& after, size_t max_lines);

}  // namespace maas
