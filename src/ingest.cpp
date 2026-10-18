#include "ingest.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>

#include "sqlite3.h"
#include "tasks.hpp"
#include "workspace.hpp"

namespace maas {

// ======================================================================
// Reading the file as UTF-8, whatever it was saved as
// ======================================================================

namespace {

enum class Encoding { Utf8, Utf8Bom, Utf16Le, Utf16Be };

const char* encoding_name(Encoding encoding) {
  switch (encoding) {
    case Encoding::Utf8Bom: return "utf-8 (with BOM)";
    case Encoding::Utf16Le: return "utf-16le";
    case Encoding::Utf16Be: return "utf-16be";
    default: return "utf-8";
  }
}

void append_utf8(std::string& out, uint32_t code_point) {
  if (code_point < 0x80) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
}

class DecodedReader {
 public:
  bool open(const fs::path& path, std::string& error_kind, std::string& error) {
    input_.open(path, std::ios::binary);
    if (!input_) {
      error_kind = "FILE_NOT_FOUND";
      error = "cannot open " + path_str(path);
      return false;
    }
    total_ = file_size_or_zero(path);
    std::string head(4096, '\0');
    input_.read(&head[0], static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<size_t>(input_.gcount()));
    input_.clear();
    input_.seekg(0);
    if (!sniff(head, error_kind, error)) return false;
    if (skip_ > 0) {
      input_.seekg(static_cast<std::streamoff>(skip_));
      raw_read_ = skip_;
    }
    return true;
  }

  // Decoded UTF-8 bytes; false at end of file.
  bool read_chunk(std::string& out) {
    out.clear();
    std::string raw(1 << 20, '\0');
    input_.read(&raw[0], static_cast<std::streamsize>(raw.size()));
    size_t got = static_cast<size_t>(input_.gcount());
    if (got == 0 && carry_.empty()) return false;
    raw.resize(got);
    raw_read_ += got;
    if (encoding_ == Encoding::Utf8 || encoding_ == Encoding::Utf8Bom) {
      out = raw;
    } else {
      std::string data = carry_ + raw;
      carry_.clear();
      size_t usable = data.size() & ~static_cast<size_t>(1);  // whole 2-byte units only
      for (size_t index = 0; index + 2 <= usable; index += 2) {
        uint16_t unit = encoding_ == Encoding::Utf16Le
                            ? static_cast<uint16_t>(static_cast<unsigned char>(data[index]) |
                                                    (static_cast<unsigned char>(data[index + 1]) << 8))
                            : static_cast<uint16_t>((static_cast<unsigned char>(data[index]) << 8) |
                                                    static_cast<unsigned char>(data[index + 1]));
        if (high_surrogate_ != 0) {
          if (unit >= 0xDC00 && unit <= 0xDFFF) {
            uint32_t code = 0x10000 + ((static_cast<uint32_t>(high_surrogate_) - 0xD800) << 10) + (unit - 0xDC00);
            append_utf8(out, code);
            high_surrogate_ = 0;
            continue;
          }
          append_utf8(out, 0xFFFD);
          high_surrogate_ = 0;
        }
        if (unit >= 0xD800 && unit <= 0xDBFF) {
          high_surrogate_ = unit;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
          append_utf8(out, 0xFFFD);
        } else {
          append_utf8(out, unit);
        }
      }
      if (usable < data.size()) carry_ = data.substr(usable);
      if (got == 0) carry_.clear();
    }
    if (cr_only_) {
      for (char& character : out) {
        if (character == '\r') character = '\n';
      }
    }
    return !out.empty() || got > 0;
  }

  uint64_t raw_read() const { return raw_read_; }
  uint64_t total() const { return total_; }
  Encoding encoding() const { return encoding_; }
  void set_cr_only(bool value) { cr_only_ = value; }

 private:
  bool sniff(const std::string& head, std::string& error_kind, std::string& error) {
    auto byte = [&](size_t index) { return index < head.size() ? static_cast<unsigned char>(head[index]) : 0u; };
    if (head.size() >= 2 && byte(0) == 0x1F && byte(1) == 0x8B) return reject_compressed("gzip", error_kind, error);
    if (head.size() >= 4 && byte(0) == 'P' && byte(1) == 'K' && byte(2) == 3 && byte(3) == 4) {
      return reject_compressed("zip (or .xlsx)", error_kind, error);
    }
    if (head.size() >= 6 && byte(0) == 0xFD && byte(1) == '7' && byte(2) == 'z' && byte(3) == 'X') {
      return reject_compressed("xz", error_kind, error);
    }
    if (head.size() >= 4 && byte(0) == 0x28 && byte(1) == 0xB5 && byte(2) == 0x2F && byte(3) == 0xFD) {
      return reject_compressed("zstd", error_kind, error);
    }
    if (head.size() >= 3 && byte(0) == 'B' && byte(1) == 'Z' && byte(2) == 'h') return reject_compressed("bzip2", error_kind, error);
    if (head.size() >= 4 && head.compare(0, 4, "PAR1") == 0) {
      error_kind = "UNSUPPORTED_FORMAT";
      error = "this is a Parquet file; read it with a script (pyarrow or pandas.read_parquet) instead";
      return false;
    }
    if (head.size() >= 16 && head.compare(0, 15, "SQLite format 3") == 0) {
      error_kind = "UNSUPPORTED_FORMAT";
      error = "this is already a SQLite database; open it from a script with sqlite3";
      return false;
    }
    if (byte(0) == 0xEF && byte(1) == 0xBB && byte(2) == 0xBF) {
      encoding_ = Encoding::Utf8Bom;
      skip_ = 3;
      return true;
    }
    if (byte(0) == 0xFF && byte(1) == 0xFE) {
      encoding_ = Encoding::Utf16Le;
      skip_ = 2;
      return true;
    }
    if (byte(0) == 0xFE && byte(1) == 0xFF) {
      encoding_ = Encoding::Utf16Be;
      skip_ = 2;
      return true;
    }
    // UTF-16 without a BOM is full of zero bytes in every other position.
    // Do not strip NUL bytes blindly: detect the encoding first.
    size_t even_zero = 0, odd_zero = 0;
    for (size_t index = 0; index < head.size(); index++) {
      if (head[index] == '\0') {
        if (index % 2 == 0) even_zero++;
        else odd_zero++;
      }
    }
    size_t half = head.size() / 2;
    if (half > 8 && odd_zero > half * 6 / 10 && even_zero < half / 10) {
      encoding_ = Encoding::Utf16Le;
      return true;
    }
    if (half > 8 && even_zero > half * 6 / 10 && odd_zero < half / 10) {
      encoding_ = Encoding::Utf16Be;
      return true;
    }
    if (even_zero + odd_zero > head.size() / 20) {
      error_kind = "BINARY_INPUT";
      error = "this looks like a binary file, not CSV/TSV text";
      return false;
    }
    std::string trimmed = trim(head.substr(0, 64));
    if (!trimmed.empty() && (trimmed[0] == '{' || trimmed[0] == '[')) {
      error_kind = "UNSUPPORTED_FORMAT";
      error = "this looks like JSON; convert it with a script (pandas.read_json(lines=True)) first";
      return false;
    }
    return true;
  }

  bool reject_compressed(const std::string& kind, std::string& error_kind, std::string& error) {
    error_kind = "COMPRESSED_INPUT";
    error = "the file is " + kind + " compressed; decompress it first (for example with a short Python script)";
    return false;
  }

  std::ifstream input_;
  Encoding encoding_ = Encoding::Utf8;
  size_t skip_ = 0;
  uint64_t total_ = 0;
  uint64_t raw_read_ = 0;
  std::string carry_;
  uint16_t high_surrogate_ = 0;
  bool cr_only_ = false;
};

// ======================================================================
// Splitting text into records and fields
// ======================================================================

class RecordParser {
 public:
  using RecordCallback = std::function<void(std::vector<std::string>&)>;

  RecordParser(char delimiter, bool quotes, RecordCallback callback)
      : delimiter_(delimiter), quotes_(quotes), callback_(std::move(callback)) {}

  void feed(const char* data, size_t size) {
    size_t position = 0;
    while (true) {
      char character;
      if (!replay_.empty()) {
        Replay& top = replay_.back();
        if (top.position >= top.text.size()) {
          replay_.pop_back();
          continue;
        }
        character = top.text[top.position++];
      } else if (position < size) {
        character = data[position++];
        if (character == '\n') physical_lines_++;
      } else {
        break;
      }
      process(character);
    }
  }

  void finish() {
    if (state_ != State::FieldStart || !fields_.empty() || !current_.empty()) {
      end_field();
      end_record();
    }
  }

  uint64_t quote_repairs() const { return quote_repairs_; }
  uint64_t truncated_fields() const { return truncated_fields_; }
  uint64_t physical_lines() const { return physical_lines_; }

 private:
  enum class State { FieldStart, Unquoted, Quoted, QuoteSeen };
  struct Replay {
    std::string text;
    size_t position = 0;
  };

  static const size_t kMaxField = 16 * 1024 * 1024;
  static const size_t kMaxQuotedSpan = 1024 * 1024;

  void append(char character) {
    if (current_.size() < kMaxField) {
      current_.push_back(character);
    } else if (!field_truncated_) {
      field_truncated_ = true;
      truncated_fields_++;
    }
  }

  void end_field() {
    fields_.push_back(std::move(current_));
    current_.clear();
    field_truncated_ = false;
  }

  void end_record() {
    if (!fields_.empty()) {
      std::string& last = fields_.back();
      if (!last.empty() && last.back() == '\r') last.pop_back();  // CRLF line endings
    }
    callback_(fields_);
    fields_.clear();
    record_raw_.clear();
    literal_quotes_ = false;
    state_ = State::FieldStart;
  }

  // An opening quote that never closes would swallow the rest of the file.
  // When a quoted field grows past 1 MB we re-read this record treating the
  // quote as an ordinary character up to the next line break.
  void repair() {
    quote_repairs_++;
    Replay replay;
    replay.text.swap(record_raw_);
    fields_.clear();
    current_.clear();
    field_truncated_ = false;
    state_ = State::FieldStart;
    literal_quotes_ = true;
    replay_.push_back(std::move(replay));
  }

  void process(char character) {
    if (quotes_) record_raw_.push_back(character);
    switch (state_) {
      case State::FieldStart:
        if (quotes_ && !literal_quotes_ && character == '"') {
          state_ = State::Quoted;
          quoted_span_ = 0;
          return;
        }
        state_ = State::Unquoted;
        [[fallthrough]];  // treat it as an unquoted character
      case State::Unquoted:
        if (character == delimiter_) {
          end_field();
          state_ = State::FieldStart;
        } else if (character == '\n') {
          end_field();
          end_record();
        } else {
          append(character);
        }
        return;
      case State::Quoted:
        if (character == '"') {
          state_ = State::QuoteSeen;
        } else {
          append(character);
          if (++quoted_span_ > kMaxQuotedSpan) repair();
        }
        return;
      case State::QuoteSeen:
        if (character == '"') {
          append('"');  // "" inside quotes is an escaped quote
          state_ = State::Quoted;
        } else if (character == delimiter_) {
          end_field();
          state_ = State::FieldStart;
        } else if (character == '\n') {
          end_field();
          end_record();
        } else if (character == '\r') {
          // CR of a CRLF right after a closing quote: ignore.
        } else {
          append(character);  // text after a closing quote: keep it, leniently
          state_ = State::Unquoted;
        }
        return;
    }
  }

  char delimiter_;
  bool quotes_;
  RecordCallback callback_;
  State state_ = State::FieldStart;
  std::vector<std::string> fields_;
  std::string current_;
  std::string record_raw_;
  std::vector<Replay> replay_;
  bool literal_quotes_ = false;
  bool field_truncated_ = false;
  size_t quoted_span_ = 0;
  uint64_t quote_repairs_ = 0;
  uint64_t truncated_fields_ = 0;
  uint64_t physical_lines_ = 0;
};

// ======================================================================
// Values, types and statistics
// ======================================================================

std::string trim_spaces(const std::string& value) {
  size_t start = 0, end = value.size();
  while (start < end && (value[start] == ' ' || value[start] == '\t')) start++;
  while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t')) end--;
  if (start == 0 && end == value.size()) return value;
  return value.substr(start, end - start);
}

bool parse_integer_value(const std::string& text, int64_t& out) {
  if (text.empty() || text.size() > 19) return false;
  size_t position = (text[0] == '-' || text[0] == '+') ? 1 : 0;
  if (position == text.size()) return false;
  // "007" stays text: leading zeros usually mean an ID or code, not a number.
  if (text[position] == '0' && text.size() - position > 1) return false;
  for (size_t index = position; index < text.size(); index++) {
    if (text[index] < '0' || text[index] > '9') return false;
  }
  if (text.size() - position > 18) return false;
  return parse_int64(text, out);
}

// "0042" or "007.5" look like codes; keep them as text. "0", "0.5" are numbers.
bool has_leading_zero(const std::string& text) {
  size_t position = (!text.empty() && (text[0] == '-' || text[0] == '+')) ? 1 : 0;
  return text.size() > position + 1 && text[position] == '0' && text[position + 1] != '.';
}

bool parse_real_value(const std::string& text, double& out) {
  if (text.empty() || text.size() > 40) return false;
  bool has_digit = false;
  for (char character : text) {
    if (character >= '0' && character <= '9') has_digit = true;
    else if (character != '.' && character != '-' && character != '+' && character != 'e' && character != 'E') {
      return false;
    }
  }
  if (!has_digit) return false;
  char* end = nullptr;
  out = std::strtod(text.c_str(), &end);
  if (!end || *end != '\0') return false;
  return std::isfinite(out);
}

// Lossless repair of non-UTF-8 text: each invalid byte is read as Latin-1.
std::string repair_text(const std::string& text, bool& repaired) {
  repaired = false;
  if (is_valid_utf8(text)) return text;
  repaired = true;
  std::string result;
  result.reserve(text.size() + 8);
  size_t position = 0;
  while (position < text.size()) {
    unsigned char first = static_cast<unsigned char>(text[position]);
    size_t length = 0;
    if (first < 0x80) length = 1;
    else if ((first & 0xE0) == 0xC0) length = 2;
    else if ((first & 0xF0) == 0xE0) length = 3;
    else if ((first & 0xF8) == 0xF0) length = 4;
    bool valid = length > 0 && position + length <= text.size();
    if (valid && length > 1) {
      std::string piece = text.substr(position, length);
      valid = is_valid_utf8(piece);
    }
    if (valid) {
      result.append(text, position, length);
      position += length;
    } else {
      append_utf8(result, first);
      position++;
    }
  }
  return result;
}

struct ColumnStats {
  uint64_t nulls = 0;
  uint64_t integers = 0;
  uint64_t reals = 0;
  uint64_t texts = 0;
  double min_number = std::numeric_limits<double>::infinity();
  double max_number = -std::numeric_limits<double>::infinity();
  size_t min_length = std::numeric_limits<size_t>::max();
  size_t max_length = 0;
  std::vector<uint8_t> registers = std::vector<uint8_t>(4096, 0);  // HyperLogLog, about 1.6% error
  std::vector<std::string> samples;

  void add_distinct(const std::string& value) {
    uint64_t hash = hash64(value.data(), value.size());
    size_t index = static_cast<size_t>(hash >> 52);  // top 12 bits pick the register
    uint64_t rest = (hash << 12) | (1ull << 11);
    uint8_t rank = 1;
    while ((rest & (1ull << 63)) == 0) {
      rank++;
      rest <<= 1;
    }
    if (rank > registers[index]) registers[index] = rank;
  }

  double distinct_estimate() const {
    const double count = 4096.0;
    double sum = 0;
    size_t zeros = 0;
    for (uint8_t value : registers) {
      sum += std::ldexp(1.0, -static_cast<int>(value));
      if (value == 0) zeros++;
    }
    double alpha = 0.7213 / (1.0 + 1.079 / count);
    double estimate = alpha * count * count / sum;
    if (estimate <= 2.5 * count && zeros > 0) estimate = count * std::log(count / static_cast<double>(zeros));
    return estimate;
  }
};

const std::vector<std::string>& default_nulls() {
  static const std::vector<std::string> values = {"",      "NA",   "N/A", "n/a",  "NULL", "null", "Null", "None",
                                                   "none",  "nan",  "NaN", "NAN",  "\\N",  "#N/A", "nil",  "<NA>"};
  return values;
}

std::string sql_identifier(const std::string& name) { return "\"" + replace_all(name, "\"", "\"\"") + "\""; }

std::string clean_column_name(const std::string& raw, size_t index) {
  std::string text = trim(raw);
  if (starts_with(text, "\xEF\xBB\xBF")) text = text.substr(3);
  std::string result;
  for (char character : text) {
    bool keep = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') || character == '_';
    if (keep) result.push_back(character);
    else if (!result.empty() && result.back() != '_') result.push_back('_');
    if (result.size() >= 60) break;
  }
  while (!result.empty() && result.back() == '_') result.pop_back();
  while (!result.empty() && result.front() == '_') result.erase(result.begin());
  if (result.empty()) result = "col_" + std::to_string(index + 1);
  if (result[0] >= '0' && result[0] <= '9') result = "c_" + result;
  std::string lower = to_lower(result);
  if (lower == "rowid" || lower == "oid" || lower == "_rowid_") result += "_col";
  return result;
}

struct SniffResult {
  char delimiter = '\t';
  bool has_header = true;
  size_t columns = 1;
  std::vector<std::string> header_fields;
  bool cr_only = false;
};

bool looks_numeric(const std::string& value) {
  int64_t integer = 0;
  double real = 0;
  std::string trimmed = trim_spaces(value);
  return parse_integer_value(trimmed, integer) || parse_real_value(trimmed, real) ||
         (trimmed.size() > 1 && trimmed[0] == '0' && parse_real_value(trimmed, real));
}

std::vector<std::vector<std::string>> parse_sample(const std::string& sample, char delimiter, size_t limit) {
  std::vector<std::vector<std::string>> records;
  RecordParser parser(delimiter, true, [&](std::vector<std::string>& fields) {
    if (records.size() < limit) records.push_back(fields);
  });
  parser.feed(sample.data(), sample.size());
  // No finish(): the last record of a cut-off sample may be incomplete.
  return records;
}

SniffResult sniff_layout(const std::string& sample_in, const std::string& extension, const IngestOptions& options) {
  SniffResult result;
  std::string sample = sample_in;
  if (sample.find('\n') == std::string::npos && sample.find('\r') != std::string::npos) {
    result.cr_only = true;  // old Mac line endings
    for (char& character : sample) {
      if (character == '\r') character = '\n';
    }
  }
  // If the whole file fits in the sample, the last record is complete.
  if (!sample.empty() && sample.back() != '\n') sample.push_back('\n');

  std::string wanted = to_lower(options.delimiter);
  if (wanted == "tab" || wanted == "\\t" || wanted == "\t") result.delimiter = '\t';
  else if (wanted == "comma" || wanted == ",") result.delimiter = ',';
  else if (wanted == "semicolon" || wanted == ";") result.delimiter = ';';
  else if (wanted == "pipe" || wanted == "|") result.delimiter = '|';
  else if (wanted.size() == 1) result.delimiter = wanted[0];
  else {
    const char candidates[] = {'\t', ',', ';', '|'};
    double best_score = -1;
    char best = '\t';
    for (char candidate : candidates) {
      std::vector<std::vector<std::string>> records = parse_sample(sample, candidate, 2000);
      if (records.empty()) continue;
      std::map<size_t, size_t> counts;
      for (const auto& record : records) counts[record.size()]++;
      size_t mode = 1, mode_count = 0;
      for (const auto& pair : counts) {
        if (pair.second > mode_count) {
          mode = pair.first;
          mode_count = pair.second;
        }
      }
      if (mode < 2) continue;
      double consistency = static_cast<double>(mode_count) / static_cast<double>(records.size());
      double score = consistency * 10.0 + std::log(static_cast<double>(mode));
      if ((candidate == '\t' && (extension == ".tsv" || extension == ".tab")) ||
          (candidate == ',' && extension == ".csv")) {
        score += 1.0;
      }
      if (score > best_score) {
        best_score = score;
        best = candidate;
      }
    }
    result.delimiter = best;
  }

  std::vector<std::vector<std::string>> records = parse_sample(sample, result.delimiter, 300);
  if (records.empty()) return result;
  std::map<size_t, size_t> counts;
  for (const auto& record : records) counts[record.size()]++;
  size_t mode = 1, mode_count = 0;
  for (const auto& pair : counts) {
    if (pair.second > mode_count) {
      mode = pair.first;
      mode_count = pair.second;
    }
  }
  const std::vector<std::string>& first = records[0];
  std::string header_choice = to_lower(options.header);
  if (header_choice == "yes" || header_choice == "true") {
    result.has_header = true;
  } else if (header_choice == "no" || header_choice == "false") {
    result.has_header = false;
  } else {
    bool any_numeric_in_first = false;
    for (const std::string& value : first) {
      if (!trim_spaces(value).empty() && looks_numeric(value)) any_numeric_in_first = true;
    }
    bool text_over_numbers = false;
    for (size_t column = 0; column < first.size(); column++) {
      std::string value = trim_spaces(first[column]);
      if (value.empty() || looks_numeric(value)) continue;
      size_t numeric = 0, seen = 0;
      for (size_t row = 1; row < records.size(); row++) {
        if (column >= records[row].size()) continue;
        std::string cell = trim_spaces(records[row][column]);
        if (cell.empty()) continue;
        seen++;
        if (looks_numeric(cell)) numeric++;
      }
      if (seen > 0 && numeric * 2 > seen) text_over_numbers = true;
    }
    if (text_over_numbers) {
      result.has_header = true;
    } else if (any_numeric_in_first) {
      result.has_header = false;
    } else {
      std::set<std::string> unique;
      bool all_short_and_filled = true;
      for (const std::string& value : first) {
        std::string trimmed = trim_spaces(value);
        if (trimmed.empty() || trimmed.size() > 64) all_short_and_filled = false;
        unique.insert(to_lower(trimmed));
      }
      result.has_header = all_short_and_filled && unique.size() == first.size();
    }
  }
  if (result.has_header) {
    result.header_fields = first;
    result.columns = std::max(first.size(), mode);
  } else {
    result.columns = mode;
  }
  if (result.columns == 0) result.columns = 1;
  return result;
}

}  // namespace

// ======================================================================
// The ingestion pass
// ======================================================================

IngestResult ingest_file(const fs::path& source, const fs::path& database_out, const IngestOptions& options,
                         std::atomic<bool>& cancel, const std::function<void(const json&)>& progress) {
  IngestResult result;
  int64_t started = mono_ms();
  DecodedReader reader;
  if (!reader.open(source, result.error_kind, result.error)) return result;

  // Sniff the layout from the first 256 KB.
  std::string sample;
  {
    DecodedReader sampler;
    std::string ignored_kind, ignored_error;
    sampler.open(source, ignored_kind, ignored_error);
    std::string chunk;
    while (sample.size() < 256 * 1024 && sampler.read_chunk(chunk)) sample += chunk;
    if (sample.size() > 256 * 1024) {
      // Cut at the last line break so the sample ends on a whole record.
      size_t cut = sample.rfind('\n', 256 * 1024);
      if (cut != std::string::npos) sample.resize(cut + 1);
    }
  }
  if (trim(sample).empty()) {
    result.error_kind = "EMPTY_FILE";
    result.error = "the file is empty";
    return result;
  }
  std::string extension = to_lower(path_str(source.extension()));
  SniffResult layout = sniff_layout(sample, extension, options);
  reader.set_cr_only(layout.cr_only);
  if (layout.columns > 4000) {
    result.error_kind = "TOO_MANY_COLUMNS";
    result.error = "the file has " + std::to_string(layout.columns) + " columns; the limit is 4000";
    return result;
  }

  std::vector<std::string> names;
  std::vector<std::string> original_names;
  std::set<std::string> used;
  for (size_t index = 0; index < layout.columns; index++) {
    std::string raw = index < layout.header_fields.size() ? layout.header_fields[index] : "";
    std::string name = clean_column_name(raw, index);
    std::string unique = name;
    int suffix = 2;
    while (used.count(to_lower(unique))) unique = name + "_" + std::to_string(suffix++);
    used.insert(to_lower(unique));
    names.push_back(unique);
    original_names.push_back(sanitize_utf8(trim(raw)));
  }

  std::set<std::string> null_set;
  const std::vector<std::string>& null_list = options.null_values.empty() ? default_nulls() : options.null_values;
  for (const std::string& value : null_list) null_set.insert(value);
  null_set.insert("");

  fs::path partial = database_out;
  partial += ".partial";
  std::error_code fs_error;
  fs::remove(partial, fs_error);
  ensure_dir(partial.parent_path());
  sqlite3* database = nullptr;
  if (sqlite3_open_v2(path_str(partial).c_str(), &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
      SQLITE_OK) {
    result.error_kind = "STORE_FAILED";
    result.error = "cannot create the dataset database";
    if (database) sqlite3_close(database);
    return result;
  }
  // The dataset file can always be rebuilt from the source, so trade crash
  // safety for speed while loading; memory stays capped by cache_size.
  sqlite3_exec(database,
               "PRAGMA page_size=8192; PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; PRAGMA temp_store=FILE; "
               "PRAGMA cache_size=-16000;",
               nullptr, nullptr, nullptr);
  std::string create = "CREATE TABLE data (";
  for (size_t index = 0; index < names.size(); index++) {
    if (index > 0) create += ", ";
    create += sql_identifier(names[index]);
  }
  create += ")";
  char* message = nullptr;
  if (sqlite3_exec(database, create.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
    result.error_kind = "STORE_FAILED";
    result.error = std::string("cannot create table: ") + (message ? message : "");
    sqlite3_free(message);
    sqlite3_close(database);
    return result;
  }
  std::string insert = "INSERT INTO data VALUES (";
  for (size_t index = 0; index < names.size(); index++) insert += index == 0 ? "?" : ",?";
  insert += ")";
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(database, insert.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
    result.error_kind = "STORE_FAILED";
    result.error = sqlite3_errmsg(database);
    sqlite3_close(database);
    return result;
  }
  sqlite3_exec(database, "BEGIN", nullptr, nullptr, nullptr);

  std::vector<ColumnStats> stats(names.size());
  uint64_t rows = 0, short_rows = 0, long_rows = 0, empty_lines = 0, encoding_repairs = 0, record_number = 0;
  json malformed = json::array();
  bool header_pending = layout.has_header;
  bool store_failed = false;
  std::string store_error;
  std::vector<std::string> cleaned(names.size());
  int64_t last_progress = 0;

  auto report = [&](bool force) {
    int64_t now = mono_ms();
    if (!force && now - last_progress < 1000) return;
    last_progress = now;
    double elapsed = static_cast<double>(now - started) / 1000.0;
    uint64_t done = reader.raw_read();
    uint64_t total = reader.total();
    json state;
    state["rows"] = rows;
    state["bytes_read"] = done;
    state["bytes_total"] = total;
    state["percent"] = total > 0 ? static_cast<int>(100.0 * static_cast<double>(done) / static_cast<double>(total)) : 0;
    state["rows_per_sec"] = elapsed > 0 ? static_cast<int64_t>(static_cast<double>(rows) / elapsed) : 0;
    if (done > 0 && total > done && elapsed > 1) {
      state["eta_sec"] = static_cast<int64_t>(elapsed * static_cast<double>(total - done) / static_cast<double>(done));
    }
    if (progress) progress(state);
  };

  RecordParser parser(layout.delimiter, true, [&](std::vector<std::string>& fields) {
    if (store_failed) return;
    record_number++;
    if (fields.size() == 1 && trim_spaces(fields[0]).empty()) {
      empty_lines++;
      return;
    }
    if (header_pending) {
      header_pending = false;
      return;
    }
    size_t expected = names.size();
    if (fields.size() < expected) {
      short_rows++;
      if (malformed.size() < 10) {
        malformed.push_back({{"record", record_number}, {"issue", "too few fields (filled with NULL)"},
                             {"found", fields.size()}, {"expected", expected}});
      }
    } else if (fields.size() > expected) {
      bool extras_empty = true;
      for (size_t index = expected; index < fields.size(); index++) {
        if (!trim_spaces(fields[index]).empty()) extras_empty = false;
      }
      if (!extras_empty) {
        long_rows++;
        if (malformed.size() < 10) {
          malformed.push_back({{"record", record_number}, {"issue", "too many fields (extra ones dropped)"},
                               {"found", fields.size()}, {"expected", expected}});
        }
      }
    }
    for (size_t index = 0; index < expected; index++) {
      int column = static_cast<int>(index + 1);
      ColumnStats& column_stats = stats[index];
      if (index >= fields.size()) {
        sqlite3_bind_null(statement, column);
        column_stats.nulls++;
        continue;
      }
      std::string value = trim_spaces(fields[index]);
      if (null_set.count(value)) {
        sqlite3_bind_null(statement, column);
        column_stats.nulls++;
        continue;
      }
      int64_t integer = 0;
      double real = 0;
      if (parse_integer_value(value, integer)) {
        sqlite3_bind_int64(statement, column, integer);
        column_stats.integers++;
        double as_double = static_cast<double>(integer);
        if (as_double < column_stats.min_number) column_stats.min_number = as_double;
        if (as_double > column_stats.max_number) column_stats.max_number = as_double;
      } else if (!has_leading_zero(value) && parse_real_value(value, real)) {
        sqlite3_bind_double(statement, column, real);
        column_stats.reals++;
        if (real < column_stats.min_number) column_stats.min_number = real;
        if (real > column_stats.max_number) column_stats.max_number = real;
      } else {
        bool repaired = false;
        cleaned[index] = repair_text(fields[index], repaired);
        if (repaired) encoding_repairs++;
        const std::string& text = cleaned[index];
        sqlite3_bind_text(statement, column, text.data(), static_cast<int>(text.size()), SQLITE_STATIC);
        column_stats.texts++;
        if (text.size() < column_stats.min_length) column_stats.min_length = text.size();
        if (text.size() > column_stats.max_length) column_stats.max_length = text.size();
      }
      column_stats.add_distinct(value);
      if (column_stats.samples.size() < 5) {
        std::string sample_value = truncate_utf8(sanitize_utf8(value), 60);
        bool seen = false;
        for (const std::string& existing : column_stats.samples) {
          if (existing == sample_value) seen = true;
        }
        if (!seen) column_stats.samples.push_back(sample_value);
      }
    }
    int step = sqlite3_step(statement);
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    if (step != SQLITE_DONE) {
      store_failed = true;
      store_error = sqlite3_errmsg(database);
      return;
    }
    rows++;
    if (rows % 20000 == 0) {
      sqlite3_exec(database, "COMMIT; BEGIN", nullptr, nullptr, nullptr);
      report(false);
    }
  });

  std::string chunk;
  bool cancelled = false;
  while (reader.read_chunk(chunk)) {
    parser.feed(chunk.data(), chunk.size());
    if (store_failed) break;
    if (cancel.load()) {
      cancelled = true;
      break;
    }
    report(false);
  }
  if (!cancelled && !store_failed) parser.finish();
  sqlite3_exec(database, "COMMIT", nullptr, nullptr, nullptr);
  sqlite3_finalize(statement);
  sqlite3_close(database);

  if (cancelled || store_failed) {
    fs::remove(partial, fs_error);
    result.error_kind = cancelled ? "CANCELLED" : "STORE_FAILED";
    result.error = cancelled ? "ingestion was cancelled" : "writing the dataset failed: " + store_error;
    if (store_failed && (store_error.find("full") != std::string::npos || store_error.find("disk") != std::string::npos)) {
      result.error_kind = "DISK_FULL";
    }
    return result;
  }
  report(true);
  std::string replace_error;
  fs::remove(database_out, fs_error);
  if (!replace_file(partial, database_out, replace_error)) {
    result.error_kind = "STORE_FAILED";
    result.error = "could not finish the dataset file: " + replace_error;
    return result;
  }

  double elapsed = static_cast<double>(mono_ms() - started) / 1000.0;
  json columns = json::array();
  for (size_t index = 0; index < names.size(); index++) {
    const ColumnStats& column_stats = stats[index];
    json column;
    column["name"] = names[index];
    if (!original_names[index].empty() && original_names[index] != names[index]) {
      column["original_name"] = original_names[index];
    }
    uint64_t non_null = rows - column_stats.nulls;
    std::string type = "empty";
    if (non_null > 0) {
      if (column_stats.integers == non_null) type = "integer";
      else if (column_stats.integers + column_stats.reals == non_null) type = "real";
      else if (column_stats.texts == non_null) type = "text";
      else {
        int numeric_percent = static_cast<int>(100.0 * static_cast<double>(column_stats.integers + column_stats.reals) /
                                               static_cast<double>(non_null));
        type = "mixed (" + std::to_string(numeric_percent) + "% numeric)";
      }
    }
    column["type"] = type;
    column["nulls"] = column_stats.nulls;
    column["null_pct"] = rows > 0 ? std::round(1000.0 * static_cast<double>(column_stats.nulls) / static_cast<double>(rows)) / 10.0 : 0.0;
    double distinct = column_stats.distinct_estimate();
    if (static_cast<double>(non_null) < distinct) distinct = static_cast<double>(non_null);
    column["distinct_estimate"] = static_cast<int64_t>(distinct + 0.5);
    if (column_stats.integers + column_stats.reals > 0) {
      column["min"] = column_stats.min_number;
      column["max"] = column_stats.max_number;
    }
    if (column_stats.texts > 0) {
      column["min_length"] = column_stats.min_length;
      column["max_length"] = column_stats.max_length;
    }
    column["samples"] = column_stats.samples;
    columns.push_back(column);
  }
  std::string delimiter_name = layout.delimiter == '\t'  ? "tab"
                               : layout.delimiter == ',' ? "comma"
                               : layout.delimiter == ';' ? "semicolon"
                               : layout.delimiter == '|' ? "pipe"
                                                         : std::string(1, layout.delimiter);
  json profile;
  profile["rows"] = rows;
  profile["columns"] = columns;
  profile["column_count"] = names.size();
  profile["source_bytes"] = reader.total();
  profile["encoding"] = encoding_name(reader.encoding());
  profile["delimiter"] = delimiter_name;
  profile["header_detected"] = layout.has_header;
  profile["line_endings"] = layout.cr_only ? "CR (old Mac)" : "LF/CRLF";
  profile["elapsed_sec"] = std::round(elapsed * 10) / 10;
  profile["rows_per_sec"] = elapsed > 0 ? static_cast<int64_t>(static_cast<double>(rows) / elapsed) : 0;
  profile["database_bytes"] = file_size_or_zero(database_out);
  json issues;
  issues["short_rows"] = short_rows;
  issues["long_rows"] = long_rows;
  issues["empty_lines_skipped"] = empty_lines;
  issues["quote_repairs"] = parser.quote_repairs();
  issues["encoding_repairs"] = encoding_repairs;
  issues["truncated_fields"] = parser.truncated_fields();
  issues["examples"] = malformed;
  profile["issues"] = issues;
  result.ok = true;
  result.profile = profile;
  return result;
}

// ======================================================================
// DatasetManager: the daemon side
// ======================================================================

DatasetManager::DatasetManager(Context& context) : context_(context) {}

std::string DatasetManager::clean_name(const std::string& name) {
  std::string result;
  for (char character : to_lower(name)) {
    bool keep = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
    if (keep) result.push_back(character);
    else if (!result.empty() && result.back() != '_') result.push_back('_');
    if (result.size() >= 64) break;
  }
  while (!result.empty() && result.back() == '_') result.pop_back();
  if (!result.empty() && result[0] >= '0' && result[0] <= '9') result = "d_" + result;
  return result;
}

std::function<void(InternalJob&)> DatasetManager::make_ingest_body(const json& task_row) {
  json payload = json::parse(json_str(task_row, "payload_json", "{}"), nullptr, false);
  std::string name = json_str(payload, "name");
  std::string relative = json_str(payload, "path");
  IngestOptions options;
  options.delimiter = json_str(payload, "delimiter", "auto");
  options.header = json_str(payload, "header", "auto");
  options.delete_source = json_bool(payload, "delete_source", false);
  for (const json& item : payload.value("null_values", json::array())) {
    if (item.is_string()) options.null_values.push_back(item.get<std::string>());
  }
  Context* context = &context_;
  DatasetManager* self = this;
  return [context, self, name, relative, options](InternalJob& job) {
    JailResult source = resolve_in_workspace(context->paths.workspace, relative, true);
    if (!source.ok) {
      job.failure_kind = "FILE_NOT_FOUND";
      job.failure = {{"summary", source.error}};
      std::lock_guard<std::mutex> lock(self->mutex_);
      self->running_task_.clear();
      return;
    }
    fs::path database = context->paths.datasets / (name + ".db");
    context->db.run(
        "INSERT INTO datasets(name, db_path, source_path, state, rows, columns, created_at, profile_json, task_id) "
        "VALUES (?,?,?,'ingesting',0,0,?,NULL,?) ON CONFLICT(name) DO UPDATE SET state = 'ingesting', task_id = "
        "excluded.task_id, source_path = excluded.source_path",
        {name, path_str(database), source.relative, now_sec(), job.task_id});
    log_info("📥 Ingesting " + source.relative + " into dataset \"" + name + "\"");
    IngestResult result = ingest_file(source.path, database, options, job.cancel, job.report_progress);
    if (result.ok) {
      context->db.run("UPDATE datasets SET state = 'ready', rows = ?, columns = ?, created_at = ?, profile_json = ?, "
                      "db_path = ? WHERE name = ?",
                      {json_int(result.profile, "rows"), json_int(result.profile, "column_count"), now_sec(),
                       dump_json(result.profile), path_str(database), name});
      job.failure_kind = "OK";
      json issues = result.profile.value("issues", json::object());
      job.failure = {{"summary", "loaded " + std::to_string(json_int(result.profile, "rows")) + " rows x " +
                                     std::to_string(json_int(result.profile, "column_count")) + " columns"},
                     {"dataset", name},
                     {"next", "call describe_dataset or query_sql"}};
      log_info("✅ Dataset \"" + name + "\" ready: " + std::to_string(json_int(result.profile, "rows")) + " rows in " +
               format_duration(result.profile.value("elapsed_sec", 0.0)));
      if (options.delete_source) {
        std::error_code error;
        fs::remove(source.path, error);
      }
    } else {
      json row = context->db.query_one("SELECT profile_json FROM datasets WHERE name = ?", {name});
      bool had_previous = !row.is_null() && !json_str(row, "profile_json").empty();
      context->db.run("UPDATE datasets SET state = ? WHERE name = ?", {had_previous ? "ready" : "failed", name});
      job.failure_kind = result.error_kind;
      job.failure = {{"summary", result.error}};
    }
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->running_task_.clear();
  };
}

bool DatasetManager::start_ingest(const std::string& relative_path, const std::string& name_in,
                                  const IngestOptions& options, const std::string& requested_by,
                                  const std::string& explanation, json& task_out, std::string& error_kind,
                                  std::string& error) {
  JailResult source = resolve_in_workspace(context_.paths.workspace, relative_path, true);
  if (!source.ok) {
    error_kind = "FILE_NOT_FOUND";
    error = source.error + " (uploads go to uploads/; see list_files)";
    return false;
  }
  std::error_code fs_error;
  if (fs::is_directory(source.path, fs_error)) {
    error_kind = "BAD_PATH";
    error = "that is a folder; give a file";
    return false;
  }
  if (ends_with(source.relative, ".part")) {
    error_kind = "UPLOAD_INCOMPLETE";
    error = "that upload has not finished yet";
    return false;
  }
  std::string name = clean_name(name_in.empty() ? path_str(source.path.stem()) : name_in);
  if (name.empty()) {
    error_kind = "BAD_INPUT";
    error = "give the dataset a name made of letters and digits";
    return false;
  }
  uint64_t size = file_size_or_zero(source.path);
  uint64_t need = size + size / 3 + static_cast<uint64_t>(context_.config.disk_floor_mb) * 1024 * 1024;
  if (free_disk_bytes(context_.paths.datasets) < need) {
    error_kind = "DISK_FULL";
    error = "not enough free disk: need about " + human_bytes(need) + " for a " + human_bytes(size) + " file";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_task_.empty()) {
      json running = context_.tasks->get_row(running_task_);
      if (!running.is_null() && json_str(running, "state") == "RUNNING") {
        error_kind = "BUSY";
        error = "another dataset is being ingested (task " + running_task_ + "); wait for it";
        return false;
      }
      running_task_.clear();
    }
  }
  json existing = context_.db.query_one("SELECT state FROM datasets WHERE name = ?", {name});
  json payload = {{"name", name},
                  {"path", source.relative},
                  {"delimiter", options.delimiter},
                  {"header", options.header},
                  {"null_values", options.null_values},
                  {"delete_source", options.delete_source},
                  {"size", size}};
  if (!existing.is_null() && json_str(existing, "state") == "ready") {
    if (!options.overwrite) {
      error_kind = "EXISTS";
      error = "dataset \"" + name + "\" already exists; pick another name or pass overwrite: true (needs approval)";
      return false;
    }
    // Replacing data someone may rely on is a Tier 2 action: it needs a human.
    TaskRequest request;
    request.kind = "ingest";
    request.display_name = "replace dataset " + name + " from " + source.relative;
    request.source_path = source.relative;
    request.explanation = explanation;
    request.requested_by = requested_by;
    request.payload = payload;
    CreateResult created = context_.tasks->create(request);
    if (!created.ok) {
      error_kind = created.error_kind;
      error = created.error;
      return false;
    }
    task_out = created.task;
    return true;
  }
  json fake_row = {{"payload_json", dump_json(payload)}};
  std::string task_id =
      context_.tasks->run_internal("ingest", "ingest " + source.relative + " -> " + name, payload, requested_by,
                                   make_ingest_body(fake_row));
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_task_ = task_id;
  }
  task_out = context_.tasks->view_by_id(task_id);
  return true;
}

json DatasetManager::list() {
  json result = json::array();
  for (const json& row : context_.db.query("SELECT name, state, rows, columns, created_at, source_path, db_path "
                                           "FROM datasets ORDER BY name")) {
    json item;
    item["name"] = json_str(row, "name");
    item["state"] = json_str(row, "state");
    item["rows"] = json_int(row, "rows");
    item["columns"] = json_int(row, "columns");
    item["source"] = json_str(row, "source_path");
    item["size"] = file_size_or_zero(u8path(json_str(row, "db_path")));
    item["created_at"] = iso_time(json_int(row, "created_at"));
    result.push_back(item);
  }
  return result;
}

json DatasetManager::describe(const std::string& name_in, std::string& error) {
  std::string name = clean_name(name_in);
  json row = context_.db.query_one("SELECT * FROM datasets WHERE name = ?", {name});
  if (row.is_null()) {
    error = "no dataset called \"" + name + "\" (see list_datasets)";
    return nullptr;
  }
  json result;
  result["name"] = name;
  result["state"] = json_str(row, "state");
  result["source"] = json_str(row, "source_path");
  json profile = json::parse(json_str(row, "profile_json", "{}"), nullptr, false);
  if (profile.is_object()) result["profile"] = profile;
  result["sql_table"] = "data";
  result["example_query"] = "SELECT * FROM data LIMIT 5";
  result["use_from_python"] = "import os, sqlite3; con = sqlite3.connect(os.path.join(os.environ['MAAS_DATASETS'], '" +
                              name + ".db')); rows = con.execute('SELECT * FROM data LIMIT 5').fetchall()";
  result["use_from_pandas"] = "pandas.read_sql_query('SELECT ...', con, chunksize=100000) keeps memory small";
  return result;
}

namespace {

int query_authorizer(void*, int action, const char* first, const char*, const char*, const char*) {
  switch (action) {
    case SQLITE_SELECT:
    case SQLITE_READ:
    case SQLITE_FUNCTION:
    case SQLITE_RECURSIVE:
      return SQLITE_OK;
    case SQLITE_PRAGMA: {
      std::string pragma = first ? to_lower(first) : "";
      if (pragma == "table_info" || pragma == "table_xinfo" || pragma == "index_list" || pragma == "index_info") {
        return SQLITE_OK;
      }
      return SQLITE_DENY;
    }
    default:
      return SQLITE_DENY;  // no writes, ATTACH, temp tables, triggers...
  }
}

int query_progress(void* deadline_pointer) {
  int64_t deadline = *static_cast<int64_t*>(deadline_pointer);
  return mono_ms() > deadline ? 1 : 0;
}

}  // namespace

json DatasetManager::query(const std::string& name_in, const std::string& sql_in, int max_rows, int timeout_sec,
                           std::string& error_kind, std::string& error) {
  std::string name = clean_name(name_in);
  json row = context_.db.query_one("SELECT db_path, state FROM datasets WHERE name = ?", {name});
  if (row.is_null()) {
    error_kind = "NOT_FOUND";
    error = "no dataset called \"" + name + "\" (see list_datasets)";
    return nullptr;
  }
  fs::path database_path = u8path(json_str(row, "db_path"));
  std::error_code fs_error;
  if (!fs::exists(database_path, fs_error)) {
    error_kind = "NOT_READY";
    error = "dataset \"" + name + "\" is " + json_str(row, "state") + "; wait for ingestion to finish";
    return nullptr;
  }
  std::string sql = trim(sql_in);
  while (!sql.empty() && sql.back() == ';') sql = trim(sql.substr(0, sql.size() - 1));
  if (sql.empty()) {
    error_kind = "BAD_INPUT";
    error = "empty query";
    return nullptr;
  }
  if (max_rows <= 0) max_rows = 100;
  if (max_rows > 1000) max_rows = 1000;
  if (timeout_sec <= 0) timeout_sec = 30;
  if (timeout_sec > 300) timeout_sec = 300;

  sqlite3* database = nullptr;
  if (sqlite3_open_v2(path_str(database_path).c_str(), &database, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX,
                      nullptr) != SQLITE_OK) {
    error_kind = "STORE_FAILED";
    error = "cannot open dataset";
    if (database) sqlite3_close(database);
    return nullptr;
  }
  sqlite3_exec(database, "PRAGMA query_only=1; PRAGMA cache_size=-32000; PRAGMA temp_store=FILE;", nullptr, nullptr,
               nullptr);
  sqlite3_set_authorizer(database, query_authorizer, nullptr);
  int64_t deadline = mono_ms() + static_cast<int64_t>(timeout_sec) * 1000;
  sqlite3_progress_handler(database, 20000, query_progress, &deadline);
  sqlite3_stmt* statement = nullptr;
  const char* tail = nullptr;
  int64_t started = mono_ms();
  if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, &tail) != SQLITE_OK) {
    std::string message = sqlite3_errmsg(database);
    error_kind = message.find("not authorized") != std::string::npos ? "READ_ONLY" : "SQL_ERROR";
    error = error_kind == "READ_ONLY" ? "query_sql is read-only: only SELECT queries are allowed" : message;
    sqlite3_close(database);
    return nullptr;
  }
  if (tail && !trim(tail).empty()) {
    sqlite3_finalize(statement);
    sqlite3_close(database);
    error_kind = "BAD_INPUT";
    error = "send one statement at a time";
    return nullptr;
  }
  json columns = json::array();
  int column_count = sqlite3_column_count(statement);
  for (int column = 0; column < column_count; column++) columns.push_back(sqlite3_column_name(statement, column));
  json rows = json::array();
  size_t approximate_bytes = 0;
  bool truncated = false;
  int step = SQLITE_ROW;
  while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
    if (static_cast<int>(rows.size()) >= max_rows || approximate_bytes > 48 * 1024) {
      truncated = true;
      break;
    }
    json values = json::array();
    for (int column = 0; column < column_count; column++) {
      switch (sqlite3_column_type(statement, column)) {
        case SQLITE_INTEGER:
          values.push_back(static_cast<int64_t>(sqlite3_column_int64(statement, column)));
          approximate_bytes += 8;
          break;
        case SQLITE_FLOAT:
          values.push_back(sqlite3_column_double(statement, column));
          approximate_bytes += 12;
          break;
        case SQLITE_NULL:
          values.push_back(nullptr);
          approximate_bytes += 4;
          break;
        case SQLITE_BLOB:
          values.push_back("<blob " + std::to_string(sqlite3_column_bytes(statement, column)) + " bytes>");
          approximate_bytes += 16;
          break;
        default: {
          const unsigned char* text = sqlite3_column_text(statement, column);
          int length = sqlite3_column_bytes(statement, column);
          std::string value(reinterpret_cast<const char*>(text ? text : reinterpret_cast<const unsigned char*>("")),
                            static_cast<size_t>(length));
          value = truncate_utf8(sanitize_utf8(value), 500);
          approximate_bytes += value.size() + 2;
          values.push_back(value);
        }
      }
    }
    rows.push_back(values);
  }
  bool interrupted = step == SQLITE_INTERRUPT;
  std::string step_error = (step != SQLITE_ROW && step != SQLITE_DONE) ? sqlite3_errmsg(database) : "";
  sqlite3_finalize(statement);
  sqlite3_close(database);
  if (interrupted) {
    error_kind = "TIMEOUT";
    error = "the query ran longer than " + std::to_string(timeout_sec) +
            " s. Add a LIMIT, filter with WHERE, or pass a larger timeout_sec (max 300).";
    return nullptr;
  }
  if (!step_error.empty()) {
    error_kind = "SQL_ERROR";
    error = step_error;
    return nullptr;
  }
  json result;
  result["columns"] = columns;
  result["rows"] = rows;
  result["row_count"] = rows.size();
  result["truncated"] = truncated;
  if (truncated) result["note"] = "more rows exist; aggregate in SQL or raise max_rows (max 1000)";
  result["elapsed_ms"] = mono_ms() - started;
  return result;
}

}  // namespace maas
