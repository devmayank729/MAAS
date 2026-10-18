#include "console.hpp"

#include <iostream>
#include <thread>

namespace maas {

void Console::start(bool accept_keys) {
  accept_keys_ = accept_keys;
  if (!accept_keys_) return;
  std::thread reader([this]() { input_loop(); });
  reader.detach();  // blocks on stdin; ends with the process
}

void Console::print_help() {
  if (!accept_keys_) return;
  console_line(c_dim("  Keys:  y approve · n deny · a approve + allow 10 more runs · p pause/resume · k stop all · "
                     "s status · c show QR again · q quit"));
}

static std::string box_line(const std::string& label, const std::string& value) {
  std::string padded = label;
  while (padded.size() < 9) padded.push_back(' ');
  return c_yellow("  │ ") + c_dim(padded) + value;
}

void Console::print_approval(const json& approval) {
  std::string kind = json_str(approval, "kind");
  std::string id = json_str(approval, "id");
  std::string title = kind == "script" ? "Run script" : kind == "install" ? "Install packages"
                      : kind == "delete" ? "Delete file" : "Replace dataset";
  std::string text = "\n" + c_yellow("  ┌─ Approval needed: " + title + "  (" + id + ")") + "\n";
  if (kind == "script") {
    text += box_line("Script", c_bold(json_str(approval, "name")) + "  (" + json_str(approval, "interpreter") + ", " +
                                   human_bytes(json_int(approval, "size")) + ", " +
                                   std::to_string(json_int(approval, "lines")) + " lines)") + "\n";
    text += box_line("SHA-256", json_str(approval, "sha256").substr(0, 16)) + "\n";
    json args = approval.value("args", json::array());
    std::vector<std::string> parts;
    for (const json& item : args) parts.push_back(item.is_string() ? item.get<std::string>() : dump_json(item));
    if (!parts.empty()) text += box_line("Args", join(parts, " ")) + "\n";
  } else if (kind == "install") {
    std::vector<std::string> names;
    for (const json& item : approval.value("packages", json::array())) names.push_back(item.get<std::string>());
    text += box_line("Packages", c_bold(join(names, ", ")) + " (" + json_str(approval, "manager") + ")") + "\n";
  } else if (kind == "delete") {
    text += box_line("Path", json_str(approval, "path")) + "\n";
  } else {
    text += box_line("Task", json_str(approval, "name")) + "\n";
  }
  text += box_line("Limits", std::to_string(json_int(approval, "memory_mb")) + " MB, " +
                                 std::to_string(json_int(approval, "timeout_sec")) + " s") + "\n";
  std::string ai = json_str(approval, "ai_says");
  if (!ai.empty()) text += box_line("AI says", c_dim("\"" + truncate_utf8(ai, 200) + "\"")) + "\n";
  if (approval.contains("previous_failures")) {
    text += box_line("Note", c_red("this exact script failed " + std::to_string(json_int(approval, "previous_failures")) +
                                   " time(s) before")) + "\n";
  }
  std::string listing = approval.contains("diff") ? json_str(approval, "diff") : json_str(approval, "preview");
  if (kind == "script" && !listing.empty()) {
    text += box_line(approval.contains("diff") ? "Changes" : "Starts", "") + "\n";
    std::vector<std::string> lines = split(listing, '\n');
    size_t shown = 0;
    for (const std::string& line : lines) {
      if (shown >= 18) {
        text += c_yellow("  │   ") + c_dim("... (full script on the phone page)") + "\n";
        break;
      }
      std::string clean = truncate_utf8(strip_ansi(line), 110);
      if (approval.contains("diff") && starts_with(clean, "+")) clean = c_green(clean);
      if (approval.contains("diff") && starts_with(clean, "-")) clean = c_red(clean);
      text += c_yellow("  │   ") + clean + "\n";
      shown++;
    }
  }
  text += c_yellow("  └─") + " expires in " + std::to_string(json_int(approval, "expires_in_sec") / 60) + " min";
  console_line(text);
}

void Console::show_prompt() {
  if (!accept_keys_) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.empty()) return;
  std::string extra = queue_.size() > 1 ? " (" + std::to_string(queue_.size()) + " waiting)" : "";
  console_print(c_bold("  Approve " + queue_.front() + "? [y/n/a]" + extra + " "));
}

void Console::on_pending(const json& approval) {
  print_approval(approval);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(json_str(approval, "id"));
  }
  if (!accept_keys_) console_line(c_dim("  Approve it on your phone (status page) or Telegram."));
  show_prompt();
}

void Console::on_decided(const json& task) {
  std::string id = json_str(task, "task_id");
  bool was_queued = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto item = queue_.begin(); item != queue_.end(); ++item) {
      if (*item == id) {
        queue_.erase(item);
        was_queued = true;
        break;
      }
    }
  }
  std::string state = json_str(task, "state");
  std::string who = json_str(task, "decided_by");
  if (state == "APPROVED") console_line(c_green("\n  ✔ Approved " + id) + c_dim(" by " + who));
  else if (state == "DENIED") console_line(c_red("\n  ✘ Denied " + id) + c_dim(" by " + who));
  else if (state == "EXPIRED") console_line(c_red("\n  ✘ " + id + " expired") + c_dim(" (nobody decided in time)"));
  if (was_queued) show_prompt();
}

void Console::on_started(const json& task) {
  console_line(c_cyan("  ▶ Started ") + json_str(task, "name") + c_dim(" (" + json_str(task, "task_id") + ")"));
}

void Console::on_finished(const json& task) {
  std::string kind = json_str(task, "failure_kind");
  std::string name = json_str(task, "name");
  json result = task.value("result", json::object());
  std::string duration = task.contains("duration_sec") ? format_duration(static_cast<double>(json_int(task, "duration_sec"))) : "";
  if (kind == "OK") {
    console_line(c_green("  ■ Finished ") + name + c_dim(" in " + duration));
  } else {
    console_line(c_red("  ■ " + kind + " ") + name + c_dim(" — " + json_str(result, "summary")));
  }
  show_prompt();
}

void Console::input_loop() {
  std::string line;
  while (std::getline(std::cin, line)) {
    std::string command = to_lower(trim(line));
    if (command.empty()) {
      show_prompt();
      continue;
    }
    std::string current;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!queue_.empty()) current = queue_.front();
    }
    if (command == "y" || command == "yes" || command == "n" || command == "no" || command == "a") {
      if (current.empty()) {
        console_line(c_dim("  Nothing is waiting for approval."));
        continue;
      }
      bool approve = command != "n" && command != "no";
      std::string error;
      context_.tasks->decide(current, approve, command == "a" ? 10 : 0, "node terminal", error);
      if (!error.empty()) {
        console_line(c_dim("  " + current + " was " + error.substr(8) + " already."));
        std::lock_guard<std::mutex> lock(mutex_);
        if (!queue_.empty() && queue_.front() == current) queue_.pop_front();
      }
    } else if (command == "p") {
      context_.tasks->set_paused(!context_.tasks->paused(), "node terminal");
    } else if (command == "k") {
      int count = context_.tasks->kill_all("node terminal");
      console_line(c_red("  🛑 Stopped " + std::to_string(count) + " tasks."));
    } else if (command == "s") {
      json tasks = context_.tasks->list(8, "");
      console_line(c_bold("  Recent tasks") + (context_.tasks->paused() ? c_yellow(" (paused)") : ""));
      if (tasks.empty()) console_line(c_dim("  none yet"));
      for (const json& task : tasks) {
        std::string state = json_str(task, "state");
        if (state == "EXITED") state = json_str(task, "failure_kind");
        console_line("  " + json_str(task, "task_id") + "  " + state + "  " + json_str(task, "name"));
      }
    } else if (command == "c") {
      if (show_pairing) show_pairing();
    } else if (command == "q" || command == "quit" || command == "exit") {
      if (context_.request_shutdown) context_.request_shutdown();
      return;
    } else if (command == "h" || command == "help" || command == "?") {
      print_help();
    } else {
      console_line(c_dim("  Unknown key. Type h for help."));
    }
  }
}

}  // namespace maas
