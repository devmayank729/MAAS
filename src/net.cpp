#include "net.hpp"

#include <cstring>
#include <set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_type;
#define MAAS_INVALID_SOCKET INVALID_SOCKET
#define maas_close_socket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int socket_type;
#define MAAS_INVALID_SOCKET (-1)
#define maas_close_socket close
#endif

namespace maas {

// ---------------------------------------------------------------- addresses

static bool parse_ipv4(const std::string& text, uint32_t& value) {
  std::string address = text;
  if (starts_with(address, "::ffff:")) address = address.substr(7);  // IPv4-mapped IPv6
  struct in_addr parsed;
  if (inet_pton(AF_INET, address.c_str(), &parsed) != 1) return false;
  value = ntohl(parsed.s_addr);
  return true;
}

bool is_loopback_address(const std::string& address) {
  uint32_t value = 0;
  if (parse_ipv4(address, value)) return (value >> 24) == 127;
  return address == "::1";
}

bool is_private_address(const std::string& address) {
  uint32_t value = 0;
  if (!parse_ipv4(address, value)) {
    std::string lower = to_lower(address);
    return lower == "::1" || starts_with(lower, "fe80:") || starts_with(lower, "fc") || starts_with(lower, "fd");
  }
  uint32_t first = value >> 24;
  uint32_t second = (value >> 16) & 0xFF;
  if (first == 10 || first == 127) return true;
  if (first == 172 && second >= 16 && second <= 31) return true;
  if (first == 192 && second == 168) return true;
  if (first == 169 && second == 254) return true;
  if (first == 100 && second >= 64 && second <= 127) return true;  // CGNAT, used by Tailscale
  return false;
}

static bool is_virtual_interface(const std::string& name_in) {
  std::string name = to_lower(name_in);
  const char* prefixes[] = {"docker", "br-",  "veth",     "virbr",  "vmnet", "vboxnet", "lxc",
                            "lxd",    "cni",  "flannel",  "podman", "utun",  "awdl",    "llw",
                            "zt",     "tap",  "vethernet", "vmware", "virtualbox", "hyper-v", "wsl", "bluetooth"};
  for (const char* prefix : prefixes) {
    if (starts_with(name, prefix)) return true;
  }
  return name.find("virtual") != std::string::npos || name.find("loopback") != std::string::npos;
}

static int address_rank(const InterfaceAddr& item) {
  uint32_t value = 0;
  if (!parse_ipv4(item.ipv4, value)) return 0;
  uint32_t first = value >> 24, second = (value >> 16) & 0xFF;
  if (first == 192 && second == 168) return 50;  // home routers and phone hotspots
  if (first == 172 && second >= 16 && second <= 31) return first == 172 && second == 17 ? 5 : 40;
  if (first == 10) return 45;
  if (first == 100 && second >= 64 && second <= 127) return 20;  // Tailscale
  if (first == 169 && second == 254) return 10;  // link-local (direct cable)
  return 15;
}

AddressChoice choose_addresses() {
  AddressChoice choice;
  std::vector<InterfaceAddr> interfaces = list_ipv4_interfaces();
  std::string routed = default_route_ipv4();
  std::vector<InterfaceAddr> candidates;
  for (const InterfaceAddr& item : interfaces) {
    if (!item.up || item.loopback || is_loopback_address(item.ipv4)) continue;
    if (is_virtual_interface(item.name) && item.ipv4 != routed) continue;
    candidates.push_back(item);
  }
  // The interface the OS routes the internet through is usually the Wi-Fi the
  // other laptop is on. Without internet (offline hotspot) rank by type.
  for (const InterfaceAddr& item : candidates) {
    if (item.ipv4 == routed && is_private_address(routed)) choice.primary = routed;
  }
  if (choice.primary.empty()) {
    int best = -1;
    for (const InterfaceAddr& item : candidates) {
      int rank = address_rank(item);
      if (rank > best) {
        best = rank;
        choice.primary = item.ipv4;
      }
    }
  }
  for (const InterfaceAddr& item : candidates) {
    if (item.ipv4 != choice.primary) choice.others.push_back(item.ipv4);
  }
  if (choice.primary.empty()) {
    choice.primary = "127.0.0.1";
    choice.warning = "no network connection found: only this computer can reach the node. Join Wi-Fi or a phone "
                     "hotspot, then restart maas.";
  } else if (!is_private_address(choice.primary)) {
    choice.warning = "the node's address " + choice.primary +
                     " is public. MAAS only answers private addresses unless allow_public is set.";
  }
  return choice;
}

bool parse_http_url(const std::string& url_in, std::string& host, int& port, std::string& base_url) {
  std::string url = trim(url_in);
  if (url.empty()) return false;
  if (starts_with(to_lower(url), "https://")) return false;  // the node speaks plain HTTP on the LAN
  if (starts_with(to_lower(url), "http://")) url = url.substr(7);
  size_t slash = url.find('/');
  std::string authority = slash == std::string::npos ? url : url.substr(0, slash);
  size_t hash = authority.find('#');
  if (hash != std::string::npos) authority = authority.substr(0, hash);
  port = MAAS_DEFAULT_PORT;
  size_t colon = authority.rfind(':');
  if (colon != std::string::npos) {
    int64_t parsed = 0;
    if (!parse_int64(authority.substr(colon + 1), parsed) || parsed <= 0 || parsed > 65535) return false;
    port = static_cast<int>(parsed);
    authority = authority.substr(0, colon);
  }
  if (authority.empty()) return false;
  host = authority;
  base_url = "http://" + host + ":" + std::to_string(port);
  return true;
}

// ---------------------------------------------------------------- DNS packets

namespace {

const uint16_t kTypeA = 1, kTypePtr = 12, kTypeTxt = 16, kTypeSrv = 33, kTypeAny = 255;
const char* kServiceName = "_maas._tcp.local";

void put16(std::string& out, uint16_t value) {
  out.push_back(static_cast<char>(value >> 8));
  out.push_back(static_cast<char>(value & 0xFF));
}

void put32(std::string& out, uint32_t value) {
  put16(out, static_cast<uint16_t>(value >> 16));
  put16(out, static_cast<uint16_t>(value & 0xFFFF));
}

void put_name(std::string& out, const std::string& name) {
  for (const std::string& label : split(name, '.')) {
    if (label.empty()) continue;
    out.push_back(static_cast<char>(label.size() > 63 ? 63 : label.size()));
    out.append(label.substr(0, 63));
  }
  out.push_back('\0');
}

uint16_t get16(const std::string& data, size_t offset) {
  if (offset + 2 > data.size()) return 0;
  return static_cast<uint16_t>((static_cast<unsigned char>(data[offset]) << 8) | static_cast<unsigned char>(data[offset + 1]));
}

uint32_t get32(const std::string& data, size_t offset) {
  return (static_cast<uint32_t>(get16(data, offset)) << 16) | get16(data, offset + 2);
}

// Reads a possibly compressed name; returns false on malformed input.
bool read_name(const std::string& data, size_t& offset, std::string& name) {
  name.clear();
  size_t position = offset;
  bool jumped = false;
  int guard = 0;
  while (true) {
    if (position >= data.size() || ++guard > 128) return false;
    unsigned char length = static_cast<unsigned char>(data[position]);
    if (length == 0) {
      position++;
      break;
    }
    if ((length & 0xC0) == 0xC0) {
      if (position + 1 >= data.size()) return false;
      size_t target = ((length & 0x3F) << 8) | static_cast<unsigned char>(data[position + 1]);
      if (!jumped) offset = position + 2;
      jumped = true;
      position = target;
      continue;
    }
    position++;
    if (position + length > data.size()) return false;
    if (!name.empty()) name.push_back('.');
    name.append(data, position, length);
    position += length;
  }
  if (!jumped) offset = position;
  return true;
}

void put_record(std::string& out, const std::string& name, uint16_t type, bool cache_flush, uint32_t ttl,
                const std::string& rdata) {
  put_name(out, name);
  put16(out, type);
  put16(out, static_cast<uint16_t>(cache_flush ? 0x8001 : 0x0001));
  put32(out, ttl);
  put16(out, static_cast<uint16_t>(rdata.size()));
  out += rdata;
}

std::string rdata_a(const std::string& ip) {
  struct in_addr parsed;
  std::string data(4, '\0');
  if (inet_pton(AF_INET, ip.c_str(), &parsed) == 1) std::memcpy(&data[0], &parsed.s_addr, 4);
  return data;
}

std::string rdata_name(const std::string& name) {
  std::string data;
  put_name(data, name);
  return data;
}

std::string rdata_srv(int port, const std::string& target) {
  std::string data;
  put16(data, 0);
  put16(data, 0);
  put16(data, static_cast<uint16_t>(port));
  put_name(data, target);
  return data;
}

std::string rdata_txt(const std::vector<std::string>& entries) {
  std::string data;
  for (const std::string& entry : entries) {
    std::string piece = entry.substr(0, 255);
    data.push_back(static_cast<char>(piece.size()));
    data += piece;
  }
  if (data.empty()) data.push_back('\0');
  return data;
}

bool equal_names(const std::string& a, const std::string& b) {
  std::string left = to_lower(a), right = to_lower(b);
  if (ends_with(left, ".")) left.pop_back();
  if (ends_with(right, ".")) right.pop_back();
  return left == right;
}

void enable_reuse(socket_type sock) {
  int enable = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enable), sizeof(enable));
#ifdef SO_REUSEPORT
  setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&enable), sizeof(enable));
#endif
}

bool wait_readable(socket_type sock, int timeout_ms) {
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(sock, &readable);
  struct timeval timeout;
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;
  return select(static_cast<int>(sock) + 1, &readable, nullptr, nullptr, &timeout) > 0;
}

struct ParsedRecord {
  std::string name;
  uint16_t type = 0;
  std::string rdata_name;  // PTR target or SRV target
  int port = 0;
  std::string ip;
  std::vector<std::string> txt;
};

bool parse_response(const std::string& packet, std::vector<ParsedRecord>& records) {
  if (packet.size() < 12) return false;
  uint16_t flags = get16(packet, 2);
  if ((flags & 0x8000) == 0) return false;  // not a response
  size_t questions = get16(packet, 4);
  size_t total = static_cast<size_t>(get16(packet, 6)) + get16(packet, 8) + get16(packet, 10);
  size_t offset = 12;
  std::string name;
  for (size_t index = 0; index < questions; index++) {
    if (!read_name(packet, offset, name)) return false;
    offset += 4;
  }
  for (size_t index = 0; index < total; index++) {
    ParsedRecord record;
    if (!read_name(packet, offset, record.name)) return false;
    if (offset + 10 > packet.size()) return false;
    record.type = get16(packet, offset);
    uint16_t length = get16(packet, offset + 8);
    size_t data_start = offset + 10;
    if (data_start + length > packet.size()) return false;
    if (record.type == kTypeA && length == 4) {
      char text[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, packet.data() + data_start, text, sizeof(text));
      record.ip = text;
    } else if (record.type == kTypePtr) {
      size_t name_offset = data_start;
      read_name(packet, name_offset, record.rdata_name);
    } else if (record.type == kTypeSrv && length >= 7) {
      record.port = get16(packet, data_start + 4);
      size_t name_offset = data_start + 6;
      read_name(packet, name_offset, record.rdata_name);
    } else if (record.type == kTypeTxt) {
      size_t position = data_start;
      while (position < data_start + length) {
        size_t piece = static_cast<unsigned char>(packet[position]);
        position++;
        if (position + piece > data_start + length) break;
        if (piece > 0) record.txt.push_back(packet.substr(position, piece));
        position += piece;
      }
    }
    records.push_back(record);
    offset = data_start + length;
  }
  return true;
}

void ensure_network() {
#ifdef _WIN32
  static std::once_flag once;
  std::call_once(once, []() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  });
#endif
}

}  // namespace

// ---------------------------------------------------------------- responder

MdnsResponder::~MdnsResponder() { stop(); }

bool MdnsResponder::start(const MdnsRecordInfo& info, std::string& error) {
  ensure_network();
  info_ = info;
  socket_type sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock == MAAS_INVALID_SOCKET) {
    error = "cannot create UDP socket";
    return false;
  }
  enable_reuse(sock);
  struct sockaddr_in bind_address;
  std::memset(&bind_address, 0, sizeof(bind_address));
  bind_address.sin_family = AF_INET;
  bind_address.sin_port = htons(5353);
  bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sock, reinterpret_cast<struct sockaddr*>(&bind_address), sizeof(bind_address)) != 0) {
    maas_close_socket(sock);
    error = "port 5353 is busy (another mDNS service); the IP address still works";
    return false;
  }
  struct ip_mreq membership;
  std::memset(&membership, 0, sizeof(membership));
  inet_pton(AF_INET, "224.0.0.251", &membership.imr_multiaddr);
  inet_pton(AF_INET, info.ip.c_str(), &membership.imr_interface);
  if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&membership), sizeof(membership)) != 0) {
    membership.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&membership), sizeof(membership));
  }
  struct in_addr interface_address;
  inet_pton(AF_INET, info.ip.c_str(), &interface_address);
  setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&interface_address), sizeof(interface_address));
  unsigned char ttl = 255;
  setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
  unsigned char loop = 1;
  setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof(loop));
  socket_ = static_cast<intptr_t>(sock);
  running_.store(true);
  thread_ = std::thread([this]() { run(); });
  return true;
}

void MdnsResponder::stop() {
  if (!running_.exchange(false)) return;
  announce(0);  // goodbye packet
  if (thread_.joinable()) thread_.join();
  maas_close_socket(static_cast<socket_type>(socket_));
  socket_ = -1;
}

static std::string build_records(const MdnsRecordInfo& info, uint32_t ttl, bool include_service, std::string& packet,
                                 uint16_t id, const std::string& question_section, int questions) {
  std::string host = info.name + ".local";
  std::string instance = info.name + "." + kServiceName;
  std::string answers;
  int count = 0;
  put_record(answers, host, kTypeA, true, ttl, rdata_a(info.ip));
  count++;
  if (include_service) {
    put_record(answers, kServiceName, kTypePtr, false, ttl == 0 ? 0 : 4500, rdata_name(instance));
    put_record(answers, instance, kTypeSrv, true, ttl, rdata_srv(info.port, host));
    put_record(answers, instance, kTypeTxt, true, ttl == 0 ? 0 : 4500, rdata_txt(info.txt));
    count += 3;
  }
  packet.clear();
  put16(packet, id);
  put16(packet, 0x8400);  // response, authoritative
  put16(packet, static_cast<uint16_t>(questions));
  put16(packet, static_cast<uint16_t>(count));
  put16(packet, 0);
  put16(packet, 0);
  packet += question_section;
  packet += answers;
  return packet;
}

void MdnsResponder::announce(uint32_t ttl) {
  if (socket_ < 0) return;
  std::string packet;
  build_records(info_, ttl, true, packet, 0, "", 0);
  struct sockaddr_in group;
  std::memset(&group, 0, sizeof(group));
  group.sin_family = AF_INET;
  group.sin_port = htons(5353);
  inet_pton(AF_INET, "224.0.0.251", &group.sin_addr);
  sendto(static_cast<socket_type>(socket_), packet.data(), static_cast<int>(packet.size()), 0,
         reinterpret_cast<struct sockaddr*>(&group), sizeof(group));
}

void MdnsResponder::run() {
  socket_type sock = static_cast<socket_type>(socket_);
  announce(120);
  int64_t second_announce = mono_ms() + 1000;
  std::string buffer(9000, '\0');
  std::string host = info_.name + ".local";
  std::string instance = info_.name + "." + kServiceName;
  while (running_.load()) {
    if (second_announce > 0 && mono_ms() > second_announce) {
      announce(120);
      second_announce = 0;
    }
    if (!wait_readable(sock, 500)) continue;
    struct sockaddr_in source;
    socklen_t source_length = sizeof(source);
    int got = static_cast<int>(recvfrom(sock, &buffer[0], static_cast<int>(buffer.size()), 0,
                                        reinterpret_cast<struct sockaddr*>(&source), &source_length));
    if (got < 12) continue;
    std::string packet = buffer.substr(0, static_cast<size_t>(got));
    uint16_t flags = get16(packet, 2);
    if (flags & 0x8000) continue;  // ignore responses
    size_t questions = get16(packet, 4);
    size_t offset = 12;
    bool want_host = false, want_service = false, unicast = false;
    std::string question_section;
    for (size_t index = 0; index < questions && index < 32; index++) {
      size_t start = offset;
      std::string name;
      if (!read_name(packet, offset, name)) break;
      if (offset + 4 > packet.size()) break;
      uint16_t type = get16(packet, offset);
      uint16_t klass = get16(packet, offset + 2);
      offset += 4;
      if (klass & 0x8000) unicast = true;  // "QU" bit: answer directly
      bool type_ok_host = type == kTypeA || type == kTypeAny;
      if (equal_names(name, host) && type_ok_host) want_host = true;
      if ((equal_names(name, kServiceName) && (type == kTypePtr || type == kTypeAny)) ||
          (equal_names(name, instance) && (type == kTypeSrv || type == kTypeTxt || type == kTypeAny))) {
        want_service = true;
      }
      question_section += packet.substr(start, offset - start);
    }
    if (!want_host && !want_service) continue;
    bool legacy = ntohs(source.sin_port) != 5353;  // one-shot query from an ordinary port
    std::string response;
    if (legacy) {
      // RFC 6762 section 6.7: echo the id and question, short TTL, unicast back.
      build_records(info_, 10, want_service, response, get16(packet, 0), question_section,
                    static_cast<int>(questions));
      sendto(sock, response.data(), static_cast<int>(response.size()), 0, reinterpret_cast<struct sockaddr*>(&source),
             source_length);
      continue;
    }
    build_records(info_, 120, want_service, response, 0, "", 0);
    if (unicast) {
      sendto(sock, response.data(), static_cast<int>(response.size()), 0, reinterpret_cast<struct sockaddr*>(&source),
             source_length);
    } else {
      struct sockaddr_in group;
      std::memset(&group, 0, sizeof(group));
      group.sin_family = AF_INET;
      group.sin_port = htons(5353);
      inet_pton(AF_INET, "224.0.0.251", &group.sin_addr);
      sendto(sock, response.data(), static_cast<int>(response.size()), 0, reinterpret_cast<struct sockaddr*>(&group),
             sizeof(group));
    }
  }
}

// ---------------------------------------------------------------- querier

static void send_query(socket_type sock, const std::string& name, uint16_t type) {
  std::string packet;
  put16(packet, static_cast<uint16_t>(random_digits(4)[0] * 97));
  put16(packet, 0);
  put16(packet, 1);
  put16(packet, 0);
  put16(packet, 0);
  put16(packet, 0);
  put_name(packet, name);
  put16(packet, type);
  put16(packet, 0x0001);
  struct sockaddr_in group;
  std::memset(&group, 0, sizeof(group));
  group.sin_family = AF_INET;
  group.sin_port = htons(5353);
  inet_pton(AF_INET, "224.0.0.251", &group.sin_addr);
  // Ask on every real interface: the laptop may have Wi-Fi and Ethernet.
  std::vector<InterfaceAddr> interfaces = list_ipv4_interfaces();
  bool sent = false;
  for (const InterfaceAddr& item : interfaces) {
    if (!item.up || item.loopback) continue;
    struct in_addr interface_address;
    if (inet_pton(AF_INET, item.ipv4.c_str(), &interface_address) != 1) continue;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&interface_address),
               sizeof(interface_address));
    sendto(sock, packet.data(), static_cast<int>(packet.size()), 0, reinterpret_cast<struct sockaddr*>(&group),
           sizeof(group));
    sent = true;
  }
  if (!sent) {
    sendto(sock, packet.data(), static_cast<int>(packet.size()), 0, reinterpret_cast<struct sockaddr*>(&group),
           sizeof(group));
  }
}

static socket_type open_query_socket() {
  ensure_network();
  socket_type sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock == MAAS_INVALID_SOCKET) return sock;
  struct sockaddr_in local;
  std::memset(&local, 0, sizeof(local));
  local.sin_family = AF_INET;
  local.sin_port = 0;  // ephemeral port: responders answer us directly
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  bind(sock, reinterpret_cast<struct sockaddr*>(&local), sizeof(local));
  unsigned char ttl = 255;
  setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
  unsigned char loop = 1;
  setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof(loop));
  return sock;
}

std::vector<DiscoveredNode> mdns_browse(int timeout_ms) {
  std::vector<DiscoveredNode> nodes;
  socket_type sock = open_query_socket();
  if (sock == MAAS_INVALID_SOCKET) return nodes;
  std::map<std::string, DiscoveredNode> by_instance;
  std::map<std::string, std::string> host_ips;
  int64_t deadline = mono_ms() + timeout_ms;
  int64_t next_send = 0;
  std::string buffer(9000, '\0');
  while (mono_ms() < deadline) {
    if (mono_ms() >= next_send) {
      send_query(sock, kServiceName, kTypePtr);
      next_send = mono_ms() + 700;
    }
    if (!wait_readable(sock, 200)) continue;
    struct sockaddr_in source;
    socklen_t source_length = sizeof(source);
    int got = static_cast<int>(recvfrom(sock, &buffer[0], static_cast<int>(buffer.size()), 0,
                                        reinterpret_cast<struct sockaddr*>(&source), &source_length));
    if (got <= 0) continue;
    std::vector<ParsedRecord> records;
    if (!parse_response(buffer.substr(0, static_cast<size_t>(got)), records)) continue;
    char source_text[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &source.sin_addr, source_text, sizeof(source_text));
    for (const ParsedRecord& record : records) {
      if (record.type == kTypePtr && equal_names(record.name, kServiceName)) {
        by_instance[to_lower(record.rdata_name)].name = split(record.rdata_name, '.')[0];
      } else if (record.type == kTypeSrv) {
        DiscoveredNode& node = by_instance[to_lower(record.name)];
        node.name = split(record.name, '.')[0];
        node.port = record.port;
        if (node.ip.empty()) node.ip = source_text;
        host_ips[to_lower(record.rdata_name)];
      } else if (record.type == kTypeTxt) {
        DiscoveredNode& node = by_instance[to_lower(record.name)];
        for (const std::string& entry : record.txt) {
          size_t equals = entry.find('=');
          if (equals != std::string::npos) node.txt[entry.substr(0, equals)] = entry.substr(equals + 1);
        }
      } else if (record.type == kTypeA) {
        host_ips[to_lower(record.name)] = record.ip;
      }
    }
    for (auto& item : by_instance) {
      std::string host = to_lower(item.second.name + ".local");
      auto found = host_ips.find(host);
      if (found != host_ips.end() && !found->second.empty()) item.second.ip = found->second;
    }
  }
  maas_close_socket(sock);
  for (auto& item : by_instance) {
    if (item.second.port > 0 && !item.second.ip.empty()) nodes.push_back(item.second);
  }
  return nodes;
}

bool mdns_resolve(const std::string& host_name, std::string& ip_out, int timeout_ms) {
  socket_type sock = open_query_socket();
  if (sock == MAAS_INVALID_SOCKET) return false;
  std::string wanted = ends_with(to_lower(host_name), ".local") ? host_name : host_name + ".local";
  int64_t deadline = mono_ms() + timeout_ms;
  int64_t next_send = 0;
  std::string buffer(9000, '\0');
  bool found = false;
  while (!found && mono_ms() < deadline) {
    if (mono_ms() >= next_send) {
      send_query(sock, wanted, kTypeA);
      next_send = mono_ms() + 500;
    }
    if (!wait_readable(sock, 150)) continue;
    int got = static_cast<int>(recv(sock, &buffer[0], static_cast<int>(buffer.size()), 0));
    if (got <= 0) continue;
    std::vector<ParsedRecord> records;
    if (!parse_response(buffer.substr(0, static_cast<size_t>(got)), records)) continue;
    for (const ParsedRecord& record : records) {
      if (record.type == kTypeA && equal_names(record.name, wanted) && !record.ip.empty()) {
        ip_out = record.ip;
        found = true;
      }
    }
  }
  maas_close_socket(sock);
  return found;
}

}  // namespace maas
