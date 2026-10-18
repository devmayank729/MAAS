#include "server.hpp"

#include <fstream>

#include "httplib.h"
#include "ingest.hpp"
#include "net.hpp"
#include "tasks.hpp"
#include "telegram.hpp"
#include "tools.hpp"
#include "webpage.hpp"
#include "workspace.hpp"

namespace maas {

namespace {

const char* kSupportedVersions[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

json rpc_error(const json& id, int code, const std::string& message) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

json rpc_result(const json& id, const json& result) { return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}; }

void send_json(httplib::Response& response, int status, const json& body) {
  response.status = status;
  response.set_content(dump_json(body), "application/json");
}

void send_error(httplib::Response& response, int status, const std::string& message) {
  send_json(response, status, {{"error", message}});
}

bool valid_upload_name(const std::string& name) {
  if (name.empty() || name.size() > 128 || name[0] == '.') return false;
  for (char character : name) {
    bool ok = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-';
    if (!ok) return false;
  }
  return !ends_with(name, ".part");
}

bool host_allowed(const std::string& host_header, const Context& context) {
  std::string host = to_lower(host_header);
  if (!host.empty() && host[0] == '[') return true;  // IPv6 literal
  size_t colon = host.rfind(':');
  if (colon != std::string::npos) host = host.substr(0, colon);
  if (host.empty()) return true;
  bool is_ip = true;
  for (char character : host) {
    if (!((character >= '0' && character <= '9') || character == '.')) is_ip = false;
  }
  if (is_ip) return true;
  if (host == "localhost" || host == to_lower(host_name()) || host == context.node.name) return true;
  // Names an attacker cannot register on the public internet, so a browser
  // cannot be tricked into reaching the node through DNS rebinding.
  const char* suffixes[] = {".local", ".lan", ".home", ".home.arpa", ".localdomain", ".internal", ".ts.net"};
  for (const char* suffix : suffixes) {
    if (ends_with(host, suffix)) return true;
  }
  return false;
}

}  // namespace

json handle_mcp_message(Context& context, const Principal& caller, const json& message) {
  if (!message.is_object()) return rpc_error(nullptr, -32600, "invalid request");
  bool is_request = message.contains("id") && !message["id"].is_null();
  json id = is_request ? message["id"] : json(nullptr);
  std::string method = json_str(message, "method");
  if (method.empty()) {
    // A response or something unknown from the client: nothing to answer.
    return is_request ? rpc_error(id, -32600, "missing method") : json(nullptr);
  }
  json params = message.value("params", json::object());
  if (!is_request) return nullptr;  // notifications/initialized, notifications/cancelled, ...

  if (method == "initialize") {
    std::string requested = json_str(params, "protocolVersion");
    std::string chosen = kSupportedVersions[0];
    for (const char* version : kSupportedVersions) {
      if (requested == version) chosen = version;
    }
    json client = params.value("clientInfo", json::object());
    log_info("🤝 AI client connected: " + json_str(client, "name", "unknown") + " " + json_str(client, "version") +
             " (MCP " + chosen + ")");
    json result = {{"protocolVersion", chosen},
                   {"capabilities", {{"tools", {{"listChanged", false}}}}},
                   {"serverInfo",
                    {{"name", "maas"}, {"title", "MAAS node " + context.node.name}, {"version", MAAS_VERSION}}},
                   {"instructions", server_instructions(context)}};
    return rpc_result(id, result);
  }
  if (method == "ping") return rpc_result(id, json::object());
  if (method == "tools/list") return rpc_result(id, {{"tools", tool_definitions()}});
  if (method == "tools/call") {
    std::string name = json_str(params, "name");
    if (name.empty()) return rpc_error(id, -32602, "tools/call needs a tool name");
    return rpc_result(id, call_tool(context, caller, name, params.value("arguments", json::object())));
  }
  if (method == "resources/list") return rpc_result(id, {{"resources", json::array()}});
  if (method == "resources/templates/list") return rpc_result(id, {{"resourceTemplates", json::array()}});
  if (method == "prompts/list") return rpc_result(id, {{"prompts", json::array()}});
  if (method == "logging/setLevel") return rpc_result(id, json::object());
  return rpc_error(id, -32601, "method not found: " + method);
}

HttpServer::HttpServer(Context& context) : context_(context), server_(new httplib::Server()) {}

HttpServer::~HttpServer() { stop(); }

std::shared_ptr<std::mutex> HttpServer::upload_lock(const std::string& name) {
  std::lock_guard<std::mutex> lock(upload_mutex_);
  std::shared_ptr<std::mutex>& item = upload_locks_[name];
  if (!item) item = std::make_shared<std::mutex>();
  return item;
}

bool HttpServer::bind(int preferred_port, int& chosen_port, std::string& error) {
  setup_routes();
  // httplib turns on SO_REUSEPORT by default, which would let another program
  // share our port. Use plain SO_REUSEADDR (POSIX) / exclusive use (Windows).
  server_->set_socket_options([](socket_t sock) {
#ifdef _WIN32
    BOOL exclusive = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
    int enable = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
#endif
  });
  for (int port = preferred_port; port < preferred_port + 25 && port <= 65535; port++) {
    if (server_->bind_to_port(context_.config.bind, port)) {
      chosen_port = port;
      return true;
    }
  }
  error = "no free port between " + std::to_string(preferred_port) + " and " + std::to_string(preferred_port + 24);
  return false;
}

void HttpServer::run() { server_->listen_after_bind(); }

void HttpServer::stop() {
  if (server_) server_->stop();
}

void HttpServer::setup_routes() {
  httplib::Server& server = *server_;
  Context& context = context_;
  server.new_task_queue = []() { return new httplib::ThreadPool(8, 48); };
  server.set_payload_max_length(70ull * 1024 * 1024);
  server.set_read_timeout(60, 0);
  server.set_write_timeout(60, 0);
  server.set_keep_alive_timeout(10);

  auto principal_for = [&context](const httplib::Request& request) {
    return context.auth->check_bearer(request.get_header_value("Authorization"), request.remote_addr);
  };

  // Runs before any body is read: network policy and authentication.
  server.set_pre_routing_handler([&context, principal_for](const httplib::Request& request, httplib::Response& response) {
    const std::string& address = request.remote_addr;
    if (!context.config.allow_public && !is_private_address(address)) {
      send_error(response, 403, "MAAS only answers devices on a private network (set allow_public to change this)");
      return httplib::Server::HandlerResponse::Handled;
    }
    if (!host_allowed(request.get_header_value("Host"), context)) {
      send_error(response, 403, "unexpected Host header");
      return httplib::Server::HandlerResponse::Handled;
    }
    std::string origin = request.get_header_value("Origin");
    if (!origin.empty() && origin != "null") {
      std::string origin_host;
      int origin_port = 0;
      std::string base;
      bool same_host = parse_http_url(origin, origin_host, origin_port, base) &&
                       ("http://" + to_lower(request.get_header_value("Host"))) == to_lower(origin);
      if (!same_host) {
        // Blocks other web pages in a browser from driving the node.
        send_error(response, 403, "cross-origin requests are not allowed");
        return httplib::Server::HandlerResponse::Handled;
      }
    }
    const std::string& path = request.path;
    std::string needed;
    if (path == "/mcp" && request.method == "POST") needed = "client";
    else if (starts_with(path, "/api/upload")) needed = "upload";
    else if (starts_with(path, "/api/local/")) needed = "local";
    else if (starts_with(path, "/api/") && path != "/api/pair") needed = "approver";
    if (needed.empty()) return httplib::Server::HandlerResponse::Unhandled;
    if (needed == "local") {
      bool ok = is_loopback_address(address) &&
                constant_time_equals(request.get_header_value("X-Maas-Local"), context.local_secret);
      if (!ok) {
        send_error(response, 403, "local admin API is only for this machine");
        return httplib::Server::HandlerResponse::Handled;
      }
      return httplib::Server::HandlerResponse::Unhandled;
    }
    Principal principal = principal_for(request);
    bool allowed = principal.valid &&
                   (needed == "upload" ? (principal.role == "client" || principal.role == "approver")
                                       : principal.role == needed);
    if (!allowed) {
      if (needed == "client") {
        response.set_header("WWW-Authenticate", "Bearer realm=\"maas\"");
        send_json(response, 401,
                  {{"jsonrpc", "2.0"},
                   {"id", nullptr},
                   {"error",
                    {{"code", -32001},
                     {"message", principal.valid ? "this token cannot use MCP (it belongs to a phone)"
                                                 : "missing or revoked MAAS token: run `maas connect` again"}}}});
      } else {
        send_error(response, principal.valid ? 403 : 401,
                   principal.valid ? "this token is not allowed to do that" : "not paired (missing or revoked token)");
      }
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });

  server.set_post_routing_handler([](const httplib::Request& request, httplib::Response& response) {
    response.set_header("X-Content-Type-Options", "nosniff");
    response.set_header("Referrer-Policy", "no-referrer");
    response.set_header("X-Frame-Options", "DENY");
    if (starts_with(request.path, "/api/") || request.path == "/mcp") response.set_header("Cache-Control", "no-store");
  });

  server.set_exception_handler([](const httplib::Request&, httplib::Response& response, std::exception_ptr problem) {
    std::string message = "internal error";
    try {
      if (problem) std::rethrow_exception(problem);
    } catch (const std::exception& error) {
      message = error.what();
    } catch (...) {
    }
    log_error("HTTP handler failed: " + message);
    send_error(response, 500, message);
  });

  // ------------------------------------------------------------ the page
  auto serve_page = [](const httplib::Request&, httplib::Response& response) {
    response.set_header("Content-Security-Policy",
                        "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' "
                        "data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
    response.set_header("Cache-Control", "no-cache");
    response.set_content(page_html(), "text/html; charset=utf-8");
  };
  server.Get("/", serve_page);
  server.Get("/pair", serve_page);
  server.Get("/app.js", [](const httplib::Request&, httplib::Response& response) {
    response.set_header("Cache-Control", "no-cache");
    response.set_content(page_js(), "application/javascript; charset=utf-8");
  });
  server.Get("/favicon.ico", [](const httplib::Request&, httplib::Response& response) { response.status = 204; });
  server.Get("/health", [&context](const httplib::Request&, httplib::Response& response) {
    send_json(response, 200,
              {{"ok", true},
               {"service", "maas"},
               {"name", context.node.name},
               {"version", MAAS_VERSION},
               {"id", context.node.node_id.substr(0, 8)},
               {"port", context.node.port}});
  });

  // ------------------------------------------------------------ MCP
  server.Post("/mcp", [&context, principal_for](const httplib::Request& request, httplib::Response& response) {
    Principal caller = principal_for(request);
    if (request.body.size() > 8 * 1024 * 1024) {
      send_json(response, 413, rpc_error(nullptr, -32600, "request too large (8 MB max)"));
      return;
    }
    json message = json::parse(request.body, nullptr, false);
    if (message.is_discarded()) {
      send_json(response, 400, rpc_error(nullptr, -32700, "parse error: body is not JSON"));
      return;
    }
    if (message.is_array()) {
      if (message.empty()) {
        send_json(response, 400, rpc_error(nullptr, -32600, "empty batch"));
        return;
      }
      json replies = json::array();
      for (const json& item : message) {
        json reply = handle_mcp_message(context, caller, item);
        if (!reply.is_null()) replies.push_back(reply);
      }
      if (replies.empty()) {
        response.status = 202;
        return;
      }
      send_json(response, 200, replies);
      return;
    }
    json reply = handle_mcp_message(context, caller, message);
    if (reply.is_null()) {
      response.status = 202;  // notification accepted, no body
      return;
    }
    send_json(response, 200, reply);
  });
  // This server never opens a server-to-client stream; the spec allows 405.
  auto no_stream = [](const httplib::Request&, httplib::Response& response) {
    response.set_header("Allow", "POST");
    send_error(response, 405, "use POST /mcp");
  };
  server.Get("/mcp", no_stream);
  server.Delete("/mcp", no_stream);

  // ------------------------------------------------------------ pairing
  server.Post("/api/pair", [&context](const httplib::Request& request, httplib::Response& response) {
    json body = json::parse(request.body, nullptr, false);
    std::string code = json_str(body, "code");
    std::string label = truncate_utf8(json_str(body, "label"), 80);
    PairResult result = context.auth->redeem_pair_code(code, label, request.remote_addr);
    if (!result.ok) {
      send_error(response, result.http_status, result.error);
      return;
    }
    json urls = json::array();
    urls.push_back(context.base_url());
    if (context.config.mdns) urls.push_back(context.mdns_url());
    send_json(response, 200,
              {{"token", result.token},
               {"role", result.role},
               {"node", {{"name", context.node.name}, {"id", context.node.node_id}, {"port", context.node.port}, {"urls", urls}}}});
    std::string who = result.role == "approver" ? "📱 Phone paired" : "🤖 AI client paired";
    log_info(who + " (" + (label.empty() ? request.remote_addr : label) + ")");
  });

  // ------------------------------------------------------------ phone API
  server.Get("/api/status", [&context](const httplib::Request&, httplib::Response& response) {
    MemInfo memory = mem_info();
    json node = {{"name", context.node.name},
                 {"version", MAAS_VERSION},
                 {"os", os_pretty()},
                 {"cores", cpu_cores()},
                 {"ram_total_mb", memory.total_kb / 1024},
                 {"ram_free_mb", memory.available_kb / 1024},
                 {"disk_free_mb", free_disk_bytes(context.paths.home) / (1024 * 1024)},
                 {"uptime_sec", now_sec() - context.node.started_at}};
    send_json(response, 200,
              {{"node", node},
               {"paused", context.tasks->paused()},
               {"pending", context.tasks->pending_approvals()},
               {"tasks", context.tasks->list(15, "")},
               {"datasets", context.datasets->list()},
               {"telegram", context.telegram != nullptr}});
  });

  server.Post(R"(/api/approvals/([A-Za-z0-9_]+))", [&context, principal_for](const httplib::Request& request,
                                                                           httplib::Response& response) {
    Principal caller = principal_for(request);
    std::string task_id = request.matches[1];
    json body = json::parse(request.body, nullptr, false);
    std::string decision = json_str(body, "decision");
    if (decision != "approve" && decision != "deny" && decision != "approve_many") {
      send_error(response, 400, "decision must be approve, deny or approve_many");
      return;
    }
    std::string error;
    std::string actor = "phone (" + caller.label + ")";
    json view = context.tasks->decide(task_id, decision != "deny", decision == "approve_many" ? 10 : 0, actor, error);
    if (view.is_null()) {
      send_error(response, 404, error);
      return;
    }
    std::string message = !error.empty() ? "Already decided (" + error.substr(8) + ")."
                          : decision == "deny"  ? "Denied."
                          : decision == "approve_many" ? "Approved, plus 10 more runs of this exact script."
                                                       : "Approved. It starts now.";
    send_json(response, 200, {{"ok", error.empty()}, {"state", json_str(view, "state")}, {"message", message}});
  });

  server.Post(R"(/api/tasks/([A-Za-z0-9_]+)/kill)", [&context, principal_for](const httplib::Request& request,
                                                                            httplib::Response& response) {
    Principal caller = principal_for(request);
    std::string error;
    if (!context.tasks->terminate(request.matches[1], "phone (" + caller.label + ")", error)) {
      send_error(response, 404, error);
      return;
    }
    send_json(response, 200, {{"ok", true}});
  });

  server.Get(R"(/api/tasks/([A-Za-z0-9_]+)/log)", [&context](const httplib::Request& request,
                                                            httplib::Response& response) {
    json row = context.tasks->get_row(request.matches[1]);
    if (row.is_null()) {
      send_error(response, 404, "no such task");
      return;
    }
    fs::path log = u8path(json_str(row, "log_path"));
    uint64_t size = file_size_or_zero(log);
    uint64_t offset = size > 6000 ? size - 6000 : 0;
    std::string data;
    read_file_range(log, offset, 6000, data);
    send_json(response, 200, {{"log", strip_ansi(sanitize_utf8(data))}, {"size", size}});
  });

  server.Post("/api/pause", [&context, principal_for](const httplib::Request& request, httplib::Response& response) {
    json body = json::parse(request.body, nullptr, false);
    context.tasks->set_paused(json_bool(body, "paused", true), "phone (" + principal_for(request).label + ")");
    send_json(response, 200, {{"paused", context.tasks->paused()}});
  });

  server.Post("/api/stop-all", [&context, principal_for](const httplib::Request& request, httplib::Response& response) {
    int count = context.tasks->kill_all("phone (" + principal_for(request).label + ")");
    log_warn("🛑 Stop everything pressed on the phone: " + std::to_string(count) + " tasks stopped");
    send_json(response, 200, {{"stopped", count}});
  });

  server.Post("/api/pair-code", [&context](const httplib::Request&, httplib::Response& response) {
    std::string code = context.auth->create_pair_code("client", 900);
    log_info("📱 Phone created a connection code for an AI client: " + code);
    send_json(response, 200, {{"code", code}, {"expires_in", 900}});
  });

  // ------------------------------------------------------------ uploads (resumable)
  server.Get("/api/upload", [&context](const httplib::Request& request, httplib::Response& response) {
    std::string name = request.get_param_value("name");
    if (!valid_upload_name(name)) {
      send_error(response, 400, "file names may use letters, digits, . _ - (max 128)");
      return;
    }
    fs::path part = context.paths.uploads / (name + ".part");
    fs::path final_path = context.paths.uploads / name;
    std::error_code error;
    json body = {{"name", name}, {"partial_size", file_size_or_zero(part)}};
    if (fs::exists(final_path, error)) body["complete_size"] = file_size_or_zero(final_path);
    send_json(response, 200, body);
  });

  server.Put("/api/upload", [this, &context](const httplib::Request& request, httplib::Response& response,
                                             const httplib::ContentReader& content_reader) {
    std::string name = request.get_param_value("name");
    if (!valid_upload_name(name)) {
      send_error(response, 400, "file names may use letters, digits, . _ - (max 128)");
      return;
    }
    int64_t offset = 0, total = 0;
    parse_int64(request.get_param_value("offset"), offset);
    parse_int64(request.get_param_value("total"), total);
    std::shared_ptr<std::mutex> lock_holder = upload_lock(name);
    std::lock_guard<std::mutex> lock(*lock_holder);
    ensure_dir(context.paths.uploads);
    fs::path part = context.paths.uploads / (name + ".part");
    uint64_t current = file_size_or_zero(part);
    if (static_cast<uint64_t>(offset) != current) {
      send_json(response, 409, {{"error", "offset mismatch; resume from size"}, {"size", current}});
      return;
    }
    uint64_t floor = static_cast<uint64_t>(context.config.disk_floor_mb) * 1024 * 1024;
    uint64_t remaining = total > 0 && static_cast<uint64_t>(total) > current ? static_cast<uint64_t>(total) - current : 0;
    if (free_disk_bytes(context.paths.uploads) < remaining + floor) {
      send_json(response, 507, {{"error", "not enough free disk on the node for this file"},
                                {"free_bytes", free_disk_bytes(context.paths.uploads)}, {"size", current}});
      return;
    }
    std::ofstream output(part, std::ios::binary | std::ios::app);
    if (!output) {
      send_error(response, 500, "cannot write upload");
      return;
    }
    Sha256 hasher;
    uint64_t written = 0;
    bool too_big = false;
    content_reader([&](const char* data, size_t length) {
      if (written + length > 64ull * 1024 * 1024) {
        too_big = true;
        return false;
      }
      output.write(data, static_cast<std::streamsize>(length));
      hasher.update(data, length);
      written += length;
      return static_cast<bool>(output);
    });
    output.close();
    std::string expected = to_lower(request.get_header_value("X-Chunk-Sha256"));
    std::string actual = hasher.hex_digest();
    std::error_code error;
    if (too_big || (!expected.empty() && expected != actual) || !output) {
      fs::resize_file(part, current, error);  // throw the bad chunk away
      send_json(response, too_big ? 413 : 422,
                {{"error", too_big ? "chunks are limited to 64 MB" : "chunk checksum mismatch; send it again"},
                 {"size", current}});
      return;
    }
    send_json(response, 200, {{"size", current + written}});
  });

  server.Post("/api/upload/complete", [this, &context](const httplib::Request& request, httplib::Response& response) {
    std::string name = request.get_param_value("name");
    if (!valid_upload_name(name)) {
      send_error(response, 400, "bad file name");
      return;
    }
    int64_t total = -1;
    parse_int64(request.get_param_value("total"), total);
    bool overwrite = request.get_param_value("overwrite") == "1";
    std::shared_ptr<std::mutex> lock_holder = upload_lock(name);
    std::lock_guard<std::mutex> lock(*lock_holder);
    fs::path part = context.paths.uploads / (name + ".part");
    fs::path final_path = context.paths.uploads / name;
    uint64_t size = file_size_or_zero(part);
    std::error_code error;
    if (!fs::exists(part, error) || (total >= 0 && static_cast<uint64_t>(total) != size)) {
      send_json(response, 409, {{"error", "upload incomplete"}, {"size", size}});
      return;
    }
    if (fs::exists(final_path, error) && !overwrite) {
      send_json(response, 409, {{"error", "uploads/" + name + " already exists (overwrite=1 replaces it)"}});
      return;
    }
    std::string replace_error;
    if (!replace_file(part, final_path, replace_error)) {
      send_error(response, 500, "could not finish upload: " + replace_error);
      return;
    }
    audit(context.db, "upload", "upload_complete", name + " " + std::to_string(size));
    log_info("📦 Received uploads/" + name + " (" + human_bytes(size) + ")");
    send_json(response, 200, {{"path", "uploads/" + name}, {"size", size}});
  });

  // ------------------------------------------------------------ local admin (CLI on this machine)
  server.Get("/api/local/status", [&context](const httplib::Request&, httplib::Response& response) {
    json urls = json::array();
    urls.push_back(context.base_url());
    if (context.config.mdns) urls.push_back(context.mdns_url());
    send_json(response, 200,
              {{"name", context.node.name},
               {"urls", urls},
               {"port", context.node.port},
               {"uptime_sec", now_sec() - context.node.started_at},
               {"paused", context.tasks->paused()},
               {"running", context.tasks->running_count()},
               {"pending", context.tasks->pending_approvals()},
               {"tasks", context.tasks->list(10, "")},
               {"telegram", context.telegram != nullptr},
               {"tokens", context.auth->list_tokens().size()}});
  });
  server.Post("/api/local/stop", [&context](const httplib::Request&, httplib::Response& response) {
    send_json(response, 200, {{"stopping", true}});
    if (context.request_shutdown) {
      std::function<void()> shutdown = context.request_shutdown;
      std::thread([shutdown]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        shutdown();
      }).detach();
    }
  });
  server.Post("/api/local/telegram-reload", [&context](const httplib::Request&, httplib::Response& response) {
    bool started = telegram_reload(context);
    send_json(response, 200, {{"ok", started}});
  });
}

}  // namespace maas
