#include "auth.hpp"

namespace maas {

std::string normalize_pair_code(const std::string& code) {
  std::string result;
  for (char character : to_lower(code)) {
    bool keep = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
    if (keep) result.push_back(character);
  }
  return result;
}

std::string Auth::create_token(const std::string& role, const std::string& label, std::string& token_id_out) {
  std::string token = "maas_" + random_base32(40);
  token_id_out = "tok_" + random_base32(8);
  db_.run("INSERT INTO tokens(id, token_hash, role, label, created_at, last_used_at, revoked) VALUES (?,?,?,?,?,?,0)",
          {token_id_out, sha256_hex(token), role, truncate_utf8(label, 120), now_sec(), now_sec()});
  audit(db_, "node", "token_created", token_id_out + " role=" + role + " label=" + label);
  return token;
}

Principal Auth::check_token(const std::string& token) {
  Principal principal;
  if (token.size() < 20 || token.size() > 200) return principal;
  // We look the token up by its SHA-256 hash, so the database never holds a
  // usable token and timing reveals nothing about the real one.
  json row = db_.query_one("SELECT id, role, label FROM tokens WHERE token_hash = ? AND revoked = 0",
                           {sha256_hex(token)});
  if (row.is_null()) return principal;
  principal.valid = true;
  principal.token_id = json_str(row, "id");
  principal.role = json_str(row, "role");
  principal.label = json_str(row, "label");
  bool write_last_used = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t& last = last_used_written_[principal.token_id];
    if (now_sec() - last > 60) {
      last = now_sec();
      write_last_used = true;
    }
  }
  if (write_last_used) db_.run("UPDATE tokens SET last_used_at = ? WHERE id = ?", {now_sec(), principal.token_id});
  return principal;
}

Principal Auth::check_bearer(const std::string& authorization_header, const std::string& remote_ip) {
  Principal principal;
  if (is_blocked(remote_ip)) return principal;
  std::string header = trim(authorization_header);
  if (header.size() < 8 || to_lower(header.substr(0, 7)) != "bearer ") return principal;
  principal = check_token(trim(header.substr(7)));
  if (!principal.valid) record_failure(remote_ip, "bad token");
  return principal;
}

std::string Auth::create_pair_code(const std::string& role, int ttl_sec) {
  std::string code = role == "client" ? random_digits(8) : random_base32(16);
  db_.run("DELETE FROM pair_codes WHERE expires_at < ? OR used = 1", {now_sec()});
  db_.run("INSERT INTO pair_codes(code_hash, role, created_at, expires_at, used) VALUES (?,?,?,?,0)",
          {sha256_hex(normalize_pair_code(code)), role, now_sec(), now_sec() + ttl_sec});
  if (role == "client") return code.substr(0, 4) + "-" + code.substr(4);
  return code;
}

PairResult Auth::redeem_pair_code(const std::string& code, const std::string& label, const std::string& remote_ip) {
  PairResult result;
  if (is_blocked(remote_ip)) {
    result.http_status = 429;
    result.error = "too many wrong codes from this address; wait 5 minutes";
    return result;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t cutoff = now_sec() - 300;
    while (!pairing_failures_.empty() && pairing_failures_.front() < cutoff) pairing_failures_.pop_front();
    if (pairing_failures_.size() >= 30) {
      result.http_status = 429;
      result.error = "pairing is paused for a few minutes after many wrong codes";
      return result;
    }
  }
  std::string normalized = normalize_pair_code(code);
  std::string code_hash = sha256_hex(normalized);
  json row = db_.query_one("SELECT role, expires_at FROM pair_codes WHERE code_hash = ? AND used = 0", {code_hash});
  if (row.is_null() || json_int(row, "expires_at") < now_sec()) {
    record_failure(remote_ip, "bad pairing code");
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pairing_failures_.push_back(now_sec());
    }
    result.http_status = 403;
    result.error = row.is_null() ? "that pairing code is wrong or was already used"
                                 : "that pairing code has expired; run `maas pair` on the node for a new one";
    return result;
  }
  int changes = 0;
  // The WHERE used = 0 makes this atomic: two devices racing for one code
  // cannot both win.
  db_.run("UPDATE pair_codes SET used = 1 WHERE code_hash = ? AND used = 0", {code_hash}, &changes);
  if (changes != 1) {
    result.http_status = 403;
    result.error = "that pairing code was just used by another device";
    return result;
  }
  result.role = json_str(row, "role");
  result.token = create_token(result.role, label.empty() ? result.role : label, result.token_id);
  result.ok = true;
  result.http_status = 200;
  audit(db_, remote_ip, "paired", result.role + " " + result.token_id + " " + label);
  return result;
}

bool Auth::is_blocked(const std::string& remote_ip) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = failures_by_ip_.find(remote_ip);
  if (found == failures_by_ip_.end()) return false;
  int64_t cutoff = now_sec() - 300;
  while (!found->second.empty() && found->second.front() < cutoff) found->second.pop_front();
  return found->second.size() >= 10;
}

void Auth::record_failure(const std::string& remote_ip, const std::string& what) {
  size_t count = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::deque<int64_t>& list = failures_by_ip_[remote_ip];
    list.push_back(now_sec());
    count = list.size();
  }
  if (count == 10) log_warn("blocking " + remote_ip + " for 5 minutes after repeated failures (" + what + ")");
}

std::vector<json> Auth::list_tokens() {
  return db_.query("SELECT id, role, label, created_at, last_used_at FROM tokens WHERE revoked = 0 ORDER BY created_at");
}

bool Auth::revoke(const std::string& token_id) {
  int changes = 0;
  db_.run("UPDATE tokens SET revoked = 1 WHERE id = ?", {token_id}, &changes);
  if (changes > 0) audit(db_, "node", "token_revoked", token_id);
  return changes > 0;
}

int Auth::revoke_all() {
  int changes = 0;
  db_.run("UPDATE tokens SET revoked = 1 WHERE revoked = 0", {}, &changes);
  audit(db_, "node", "tokens_revoked_all", std::to_string(changes));
  return changes;
}

}  // namespace maas
