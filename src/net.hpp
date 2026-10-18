// Network helpers: choosing the address for the QR code, private-address
// checks, and a small mDNS responder/querier so clients can find the node as
// "maas-node.local" or by browsing for _maas._tcp.
#pragma once

#include <thread>

#include "platform.hpp"

namespace maas {

struct AddressChoice {
  std::string primary;  // goes into the QR code and printed URL
  std::vector<std::string> others;
  std::string warning;  // set when only loopback or a public address is available
};

AddressChoice choose_addresses();
bool is_private_address(const std::string& address);  // RFC1918, loopback, link-local, CGNAT/Tailscale
bool is_loopback_address(const std::string& address);

struct MdnsRecordInfo {
  std::string name;     // "maas-node"
  std::string ip;
  int port = 0;
  std::vector<std::string> txt;  // "key=value" strings
};

class MdnsResponder {
 public:
  ~MdnsResponder();
  bool start(const MdnsRecordInfo& info, std::string& error);
  void stop();  // sends a goodbye so other machines forget us quickly

 private:
  void run();
  void announce(uint32_t ttl);
  intptr_t socket_ = -1;
  MdnsRecordInfo info_;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

struct DiscoveredNode {
  std::string name;
  std::string ip;
  int port = 0;
  std::map<std::string, std::string> txt;
};

std::vector<DiscoveredNode> mdns_browse(int timeout_ms);
bool mdns_resolve(const std::string& host_name, std::string& ip_out, int timeout_ms);

// "http://host:port/anything" -> host, port. Adds http:// and the default port.
bool parse_http_url(const std::string& url, std::string& host, int& port, std::string& base_url);

}  // namespace maas
