// Streaming ingester and profiler ("Byte-Scrubber").
//
// One pass over a CSV/TSV file of any size with constant memory: it detects
// the encoding, delimiter and header, repairs common damage, loads the rows
// into a SQLite file in batches and profiles every column (nulls, types,
// min/max, distinct estimate). A 5 GB file never has to fit in RAM.
#pragma once

#include "node.hpp"

namespace maas {

struct InternalJob;

struct IngestOptions {
  std::string delimiter = "auto";  // auto, tab, comma, semicolon, pipe, or one character
  std::string header = "auto";     // auto, yes, no
  std::vector<std::string> null_values;  // empty = the default spellings of "missing"
  bool overwrite = false;
  bool delete_source = false;
};

struct IngestResult {
  bool ok = false;
  std::string error_kind;
  std::string error;
  json profile;
};

// The pure ingestion step (no daemon state), so it can be tested directly.
IngestResult ingest_file(const fs::path& source, const fs::path& database_out, const IngestOptions& options,
                         std::atomic<bool>& cancel, const std::function<void(const json&)>& progress);

class DatasetManager {
 public:
  explicit DatasetManager(Context& context);

  bool start_ingest(const std::string& relative_path, const std::string& name, const IngestOptions& options,
                    const std::string& requested_by, const std::string& explanation, json& task_out,
                    std::string& error_kind, std::string& error);
  json describe(const std::string& name, std::string& error);
  json list();
  json query(const std::string& name, const std::string& sql, int max_rows, int timeout_sec,
             std::string& error_kind, std::string& error);

  static std::string clean_name(const std::string& name);
  // Used by the task manager when an approved overwrite starts.
  std::function<void(InternalJob&)> make_ingest_body(const json& task_row);

 private:
  Context& context_;
  std::mutex mutex_;
  std::string running_task_;
};

}  // namespace maas
