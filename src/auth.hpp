// Tokens, one-time pairing codes and brute-force protection.
//
// Roles:
//   client   - an AI client (MCP calls, uploads). It can never approve its own jobs.
//   approver - your phone or the status page (approve/deny, kill, pause, upload).
#pragma once

#include <deque>

#include "db.hpp"

namespace maas {

struct Principal {
  bool valid = false;
  std::string token_id;
  std::string role;
  std::string label;
};

struct PairResult {
  bool ok = false;
  int http_status = 400;
  std::string error;
  std::string token;
  std::string token_id;
  std::string role;
};

class Auth {
 public:
  explicit Auth(Db& db) : db_(db) {}

  std::string create_token(const std::string& role, const std::string& label, std::string& token_id_out);
  Principal check_bearer(const std::string& authorization_header, const std::string& remote_ip);
  Principal check_token(const std::string& token);

  // Phone codes are 16 letters (go in the QR code); client codes are 8 digits
  // (typed into `maas connect`). Both work once and expire.
  std::string create_pair_code(const std::string& role, int ttl_sec);
  PairResult redeem_pair_code(const std::string& code, const std::string& label, const std::string& remote_ip);

  bool is_blocked(const std::string& remote_ip);
  void record_failure(const std::string& remote_ip, const std::string& what);

  std::vector<json> list_tokens();
  bool revoke(const std::string& token_id);
  int revoke_all();

 private:
  Db& db_;
  std::mutex mutex_;
  std::map<std::string, std::deque<int64_t>> failures_by_ip_;
  std::deque<int64_t> pairing_failures_;
  std::map<std::string, int64_t> last_used_written_;
};

std::string normalize_pair_code(const std::string& code);

}  // namespace maas
