// Thin wrapper over SQLite for the node's state database (maas_state.db).
#pragma once

#include "common.hpp"

struct sqlite3;

namespace maas {

class Db {
 public:
  Db() = default;
  ~Db();
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  bool open(const fs::path& path, std::string& error);
  void close();
  bool is_open() const { return handle_ != nullptr; }

  // Runs one statement with ? placeholders. Values are bound by JSON type.
  bool run(const std::string& sql, const std::vector<json>& params = {}, int* changes = nullptr,
           std::string* error = nullptr);
  // Returns each row as a JSON object keyed by column name.
  std::vector<json> query(const std::string& sql, const std::vector<json>& params = {});
  json query_one(const std::string& sql, const std::vector<json>& params = {});  // null if no row
  bool exec_script(const std::string& sql, std::string& error);

  std::recursive_mutex& mutex() { return mutex_; }

 private:
  sqlite3* handle_ = nullptr;
  std::recursive_mutex mutex_;
};

// Creates tables and applies upgrades. Safe to call every start.
bool migrate_state_db(Db& db, std::string& error);

void audit(Db& db, const std::string& actor, const std::string& action, const std::string& detail);

std::string meta_get(Db& db, const std::string& key, const std::string& fallback = "");
void meta_set(Db& db, const std::string& key, const std::string& value);

}  // namespace maas
