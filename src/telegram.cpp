// Telegram approvals are DEFERRED in this build (see README "Not done yet").
// Config load/save is implemented; the bot itself is not started. Approvals
// work through the node terminal and the phone status page instead.
#include "telegram.hpp"

namespace maas {

bool load_telegram_config(const Paths& paths, TelegramConfig& config) {
  std::string text;
  if (!read_file(paths.telegram_file(), text, 65536)) return false;
  json value = json::parse(text, nullptr, false);
  if (!value.is_object()) return false;
  config.bot_token = json_str(value, "bot_token");
  config.bot_username = json_str(value, "bot_username");
  config.chat_id = json_int(value, "chat_id");
  config.user_id = json_int(value, "user_id");
  config.loaded = true;
  return true;
}

bool save_telegram_config(const Paths& paths, const TelegramConfig& config) {
  json value = {{"bot_token", config.bot_token},
                {"bot_username", config.bot_username},
                {"chat_id", config.chat_id},
                {"user_id", config.user_id}};
  return write_file_atomic(paths.telegram_file(), dump_json(value, 2), true);
}

bool TelegramApi::call(const std::string&, const json&, json&, std::string& error, int) {
  error = "Telegram support is not included in this build";
  return false;
}

TelegramBot::~TelegramBot() { stop(); }
bool TelegramBot::start(std::string& error) {
  error = "Telegram support is not included in this build";
  return false;
}
void TelegramBot::stop() {}
void TelegramBot::on_pending(const json&) {}
void TelegramBot::on_decided(const json&) {}
void TelegramBot::on_finished(const json&) {}
void TelegramBot::poll_loop() {}
void TelegramBot::handle_update(const json&) {}
void TelegramBot::send_text(const std::string&) {}
std::string TelegramBot::approval_text(const json&) { return ""; }

bool telegram_token_looks_valid(const std::string& token) { return token.find(':') != std::string::npos; }
bool telegram_reload(Context&) { return false; }

int telegram_setup_command(const Paths&, const std::vector<std::string>&) {
  console_error("Telegram approvals are not available in this build yet. Use the phone status page (scan the QR "
                "code) or the node's terminal to approve jobs.");
  return 2;
}

}  // namespace maas
