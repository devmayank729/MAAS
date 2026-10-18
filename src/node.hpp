// The running node: everything the daemon's parts share.
#pragma once

#include <memory>

#include "auth.hpp"
#include "config.hpp"
#include "db.hpp"
#include "runtimes.hpp"

namespace maas {

class TaskManager;
class DatasetManager;
class TelegramBot;
class Console;

struct NodeInfo {
  std::string name;        // mDNS name, e.g. "maas-node"
  std::string node_id;     // stable random id, shown in discovery records
  std::string primary_ip;  // address put in the QR code
  std::vector<std::string> all_ips;
  int port = 0;
  int64_t started_at = 0;
};

struct Context {
  Paths paths;
  Config config;
  Db db;
  std::unique_ptr<Auth> auth;
  Runtimes runtimes;
  std::unique_ptr<TaskManager> tasks;
  std::unique_ptr<DatasetManager> datasets;
  TelegramBot* telegram = nullptr;
  Console* console = nullptr;
  NodeInfo node;
  std::string local_secret;  // lets `maas status/stop` on this machine talk to the daemon
  bool ephemeral = false;
  bool service_mode = false;  // no terminal attached
  bool terminal_approvals = false;
  int64_t default_memory_mb = 1024;
  int64_t max_memory_mb = 2048;
  int max_concurrent_jobs = 1;
  std::atomic<bool> shutting_down{false};
  std::function<void()> request_shutdown;

  std::string base_url() const {
    return "http://" + (node.primary_ip.empty() ? std::string("127.0.0.1") : node.primary_ip) + ":" +
           std::to_string(node.port);
  }
  std::string mdns_url() const { return "http://" + node.name + ".local:" + std::to_string(node.port); }
};

}  // namespace maas
