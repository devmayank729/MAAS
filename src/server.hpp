// The node's HTTP server: MCP (Streamable HTTP, POST /mcp), the phone page and
// its small API, resumable uploads, and a loopback-only admin API for the CLI.
#pragma once

#include <memory>

#include "node.hpp"

namespace httplib {
class Server;
}

namespace maas {

class HttpServer {
 public:
  explicit HttpServer(Context& context);
  ~HttpServer();

  // Binds the preferred port, or the next free one. Returns the chosen port.
  bool bind(int preferred_port, int& chosen_port, std::string& error);
  void run();   // blocks until stop()
  void stop();

 private:
  void setup_routes();
  std::shared_ptr<std::mutex> upload_lock(const std::string& name);

  Context& context_;
  std::unique_ptr<httplib::Server> server_;
  std::mutex upload_mutex_;
  std::map<std::string, std::shared_ptr<std::mutex>> upload_locks_;
};

// JSON-RPC dispatch, shared with tests. Returns null for notifications.
json handle_mcp_message(Context& context, const Principal& caller, const json& message);

}  // namespace maas
