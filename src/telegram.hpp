// Optional Telegram approvals: the bot sends each approval to your phone with
// Approve / Deny buttons, even when the phone is not on the node's Wi-Fi.
// Uses long polling (works behind NAT) through the system's curl, so MAAS
// needs no TLS library and uses the OS's own certificate store.
#pragma once

#include <thread>

#include "tasks.hpp"

namespace maas {

struct TelegramConfig {
  std::string bot_token;
  std::string bot_username;
  int64_t chat_id = 0;  // the one chat allowed to approve
  int64_t user_id = 0;
  std::string pending_code_hash;
  int64_t pending_code_expires = 0;
  bool loaded = false;
};

bool load_telegram_config(const Paths& paths, TelegramConfig& config);
bool save_telegram_config(const Paths& paths, const TelegramConfig& config);

class TelegramApi {
 public:
  TelegramApi(const Paths& paths, const std::string& token) : paths_(paths), token_(token) {}
  // Calls https://api.telegram.org/bot<token>/<method>. The token goes in a
  // private curl config file, never on the command line.
  bool call(const std::string& method, const json& body, json& result, std::string& error, int timeout_sec = 20);

 private:
  Paths paths_;
  std::string token_;
};

class TelegramBot : public TaskObserver {
 public:
  explicit TelegramBot(Context& context) : context_(context) {}
  ~TelegramBot() override;
  bool start(std::string& error);
  void stop();
  bool bound() const { return config_.chat_id != 0; }

  void on_pending(const json& approval) override;
  void on_decided(const json& task) override;
  void on_finished(const json& task) override;

 private:
  void poll_loop();
  void handle_update(const json& update);
  void send_text(const std::string& html);
  std::string approval_text(const json& approval);

  Context& context_;
  TelegramConfig config_;
  std::unique_ptr<TelegramApi> api_;
  std::thread poller_;
  std::atomic<bool> running_{false};
  std::mutex send_mutex_;
};

bool telegram_token_looks_valid(const std::string& token);
// (Re)starts the bot after `maas telegram` changed the config.
bool telegram_reload(Context& context);
// Interactive setup used by `maas telegram`.
int telegram_setup_command(const Paths& paths, const std::vector<std::string>& args);

}  // namespace maas
