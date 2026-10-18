#include "db.hpp"

#include "sqlite3.h"

namespace maas {

Db::~Db() { close(); }

bool Db::open(const fs::path& path, std::string& error) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  close();
  ensure_dir(path.parent_path());
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
  int result = sqlite3_open_v2(path_str(path).c_str(), &handle_, flags, nullptr);
  if (result != SQLITE_OK) {
    error = handle_ ? sqlite3_errmsg(handle_) : "cannot open database";
    close();
    return false;
  }
  sqlite3_busy_timeout(handle_, 5000);
  std::string pragma_error;
  // WAL keeps the database consistent after a power cut and lets the CLI read
  // while the daemon writes.
  exec_script("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;", pragma_error);
  return true;
}

void Db::close() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (handle_) {
    sqlite3_close_v2(handle_);
    handle_ = nullptr;
  }
}

static void bind_value(sqlite3_stmt* statement, int index, const json& value) {
  if (value.is_null()) {
    sqlite3_bind_null(statement, index);
  } else if (value.is_boolean()) {
    sqlite3_bind_int64(statement, index, value.get<bool>() ? 1 : 0);
  } else if (value.is_number_integer() || value.is_number_unsigned()) {
    sqlite3_bind_int64(statement, index, value.get<int64_t>());
  } else if (value.is_number_float()) {
    sqlite3_bind_double(statement, index, value.get<double>());
  } else if (value.is_string()) {
    const std::string& text = value.get_ref<const std::string&>();
    sqlite3_bind_text(statement, index, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
  } else {
    std::string text = dump_json(value);
    sqlite3_bind_text(statement, index, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
  }
}

static json column_value(sqlite3_stmt* statement, int column) {
  switch (sqlite3_column_type(statement, column)) {
    case SQLITE_INTEGER: return json(static_cast<int64_t>(sqlite3_column_int64(statement, column)));
    case SQLITE_FLOAT: return json(sqlite3_column_double(statement, column));
    case SQLITE_NULL: return json(nullptr);
    default: {
      const unsigned char* text = sqlite3_column_text(statement, column);
      int length = sqlite3_column_bytes(statement, column);
      return json(sanitize_utf8(std::string(reinterpret_cast<const char*>(text ? text : reinterpret_cast<const unsigned char*>("")),
                                            static_cast<size_t>(length))));
    }
  }
}

bool Db::run(const std::string& sql, const std::vector<json>& params, int* changes, std::string* error) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!handle_) {
    if (error) *error = "database is not open";
    return false;
  }
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(handle_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(handle_);
    log_error(std::string("SQL prepare failed: ") + sqlite3_errmsg(handle_) + " in: " + sql.substr(0, 120));
    return false;
  }
  for (size_t index = 0; index < params.size(); index++) bind_value(statement, static_cast<int>(index + 1), params[index]);
  int result = sqlite3_step(statement);
  while (result == SQLITE_ROW) result = sqlite3_step(statement);
  bool ok = result == SQLITE_DONE;
  if (!ok) {
    if (error) *error = sqlite3_errmsg(handle_);
    log_error(std::string("SQL failed: ") + sqlite3_errmsg(handle_) + " in: " + sql.substr(0, 120));
  }
  if (changes) *changes = sqlite3_changes(handle_);
  sqlite3_finalize(statement);
  return ok;
}

std::vector<json> Db::query(const std::string& sql, const std::vector<json>& params) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::vector<json> rows;
  if (!handle_) return rows;
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(handle_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
    log_error(std::string("SQL prepare failed: ") + sqlite3_errmsg(handle_) + " in: " + sql.substr(0, 120));
    return rows;
  }
  for (size_t index = 0; index < params.size(); index++) bind_value(statement, static_cast<int>(index + 1), params[index]);
  int column_count = sqlite3_column_count(statement);
  while (sqlite3_step(statement) == SQLITE_ROW) {
    json row = json::object();
    for (int column = 0; column < column_count; column++) {
      row[sqlite3_column_name(statement, column)] = column_value(statement, column);
    }
    rows.push_back(row);
  }
  sqlite3_finalize(statement);
  return rows;
}

json Db::query_one(const std::string& sql, const std::vector<json>& params) {
  std::vector<json> rows = query(sql, params);
  if (rows.empty()) return json(nullptr);
  return rows[0];
}

bool Db::exec_script(const std::string& sql, std::string& error) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!handle_) {
    error = "database is not open";
    return false;
  }
  char* message = nullptr;
  int result = sqlite3_exec(handle_, sql.c_str(), nullptr, nullptr, &message);
  if (result != SQLITE_OK) {
    error = message ? message : "SQL error";
    sqlite3_free(message);
    return false;
  }
  return true;
}

static const int SCHEMA_VERSION = 1;

bool migrate_state_db(Db& db, std::string& error) {
  const char* schema = R"SQL(
CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE IF NOT EXISTS tokens(
  id TEXT PRIMARY KEY, token_hash TEXT UNIQUE NOT NULL, role TEXT NOT NULL, label TEXT,
  created_at INTEGER, last_used_at INTEGER, revoked INTEGER DEFAULT 0);
CREATE TABLE IF NOT EXISTS pair_codes(
  code_hash TEXT PRIMARY KEY, role TEXT NOT NULL, created_at INTEGER, expires_at INTEGER, used INTEGER DEFAULT 0);
CREATE TABLE IF NOT EXISTS tasks(
  id TEXT PRIMARY KEY, kind TEXT NOT NULL, state TEXT NOT NULL,
  created_at INTEGER, updated_at INTEGER,
  display_name TEXT, source_path TEXT, stored_path TEXT, sha256 TEXT, size_bytes INTEGER,
  interpreter TEXT, interpreter_path TEXT, args_json TEXT, cwd TEXT,
  explanation TEXT, requested_by TEXT, payload_json TEXT,
  timeout_sec INTEGER, memory_mb INTEGER,
  approval_deadline INTEGER, decided_at INTEGER, decided_by TEXT, deny_reason TEXT,
  pid INTEGER, start_token INTEGER, started_at INTEGER, ended_at INTEGER,
  exit_code INTEGER, signal INTEGER, failure_kind TEXT, failure_json TEXT,
  log_path TEXT, peak_rss_kb INTEGER, progress_json TEXT, telegram_msg_id INTEGER);
CREATE INDEX IF NOT EXISTS tasks_state ON tasks(state);
CREATE INDEX IF NOT EXISTS tasks_created ON tasks(created_at);
CREATE TABLE IF NOT EXISTS hash_stats(
  sha256 TEXT PRIMARY KEY, failures INTEGER DEFAULT 0, successes INTEGER DEFAULT 0, allow_runs INTEGER DEFAULT 0);
CREATE TABLE IF NOT EXISTS last_approved(source_path TEXT PRIMARY KEY, sha256 TEXT, stored_path TEXT);
CREATE TABLE IF NOT EXISTS datasets(
  name TEXT PRIMARY KEY, db_path TEXT, source_path TEXT, state TEXT, rows INTEGER, columns INTEGER,
  created_at INTEGER, profile_json TEXT, task_id TEXT);
CREATE TABLE IF NOT EXISTS audit(
  id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER, actor TEXT, action TEXT, detail TEXT);
)SQL";
  if (!db.exec_script(schema, error)) return false;
  std::string version = meta_get(db, "schema_version", "0");
  int64_t current = 0;
  parse_int64(version, current);
  if (current > SCHEMA_VERSION) {
    error = "the state database was created by a newer MAAS version; update maas or run `maas clean`";
    return false;
  }
  if (current < SCHEMA_VERSION) meta_set(db, "schema_version", std::to_string(SCHEMA_VERSION));
  // Keep the audit log from growing forever on a small disk.
  db.run("DELETE FROM audit WHERE id < (SELECT MAX(id) - 50000 FROM audit)");
  return true;
}

void audit(Db& db, const std::string& actor, const std::string& action, const std::string& detail) {
  db.run("INSERT INTO audit(ts, actor, action, detail) VALUES (?,?,?,?)",
         {now_sec(), actor, action, truncate_utf8(detail, 2000)});
}

std::string meta_get(Db& db, const std::string& key, const std::string& fallback) {
  json row = db.query_one("SELECT value FROM meta WHERE key = ?", {key});
  if (row.is_null() || !row["value"].is_string()) return fallback;
  return row["value"].get<std::string>();
}

void meta_set(Db& db, const std::string& key, const std::string& value) {
  db.run("INSERT INTO meta(key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value",
         {key, value});
}

}  // namespace maas
