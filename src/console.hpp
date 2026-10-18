// The node's own terminal: shows what happens and lets the person at the old
// laptop approve with a single key (y / n / a).
#pragma once

#include <deque>

#include "tasks.hpp"

namespace maas {

class Console : public TaskObserver {
 public:
  explicit Console(Context& context) : context_(context) {}
  void start(bool accept_keys);
  void print_help();

  void on_pending(const json& approval) override;
  void on_decided(const json& task) override;
  void on_started(const json& task) override;
  void on_finished(const json& task) override;

  std::function<void()> show_pairing;  // prints the QR code and codes again

 private:
  void input_loop();
  void show_prompt();
  void print_approval(const json& approval);

  Context& context_;
  std::mutex mutex_;
  std::deque<std::string> queue_;
  bool accept_keys_ = false;
};

}  // namespace maas
