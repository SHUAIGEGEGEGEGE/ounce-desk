#include "profiles.hpp"

#include "platform.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#define CLOSE_SOCK closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSE_SOCK close
#endif

ProfileManager::~ProfileManager() { stop_mdns(); }

void ProfileManager::load(const std::string& path) {
    saved_.clear();
    std::ifstream in(path);
    if (!in.is_open()) return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        // Format: proto|name|host|port|user|domain|w|h
        std::stringstream ss(line);
        std::string proto_s, name, host, port_s, user, dom, ws, hs;
        if (std::getline(ss, proto_s, '|') &&
            std::getline(ss, name, '|') &&
            std::getline(ss, host, '|') &&
            std::getline(ss, port_s, '|') &&
            std::getline(ss, user, '|') &&
            std::getline(ss, dom, '|') &&
            std::getline(ss, ws, '|') &&
            std::getline(ss, hs)) {
            ConnectionProfile p;
            p.protocol = (proto_s == "VNC") ? ProtocolType::VNC_MACOS : ProtocolType::RDP_WINDOWS;
            p.name = name;
            p.host = host;
            p.port = static_cast<uint16_t>(std::atoi(port_s.c_str()));
            if (p.port == 0) p.port = (p.protocol == ProtocolType::VNC_MACOS) ? 5900 : 3389;
            p.username = user;
            p.domain = dom;
            p.req_width = static_cast<uint16_t>(std::max(640, std::atoi(ws.c_str())));
            p.req_height = static_cast<uint16_t>(std::max(480, std::atoi(hs.c_str())));
            p.from_mdns = false;
            saved_.push_back(p);
        }
    }
}

void ProfileManager::save(const std::string& path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) return;
    out << "# OunceDesk connections (proto|name|host|port|user|domain|w|h)\n";
    for (const auto& p : saved_) {
        out << (p.protocol == ProtocolType::VNC_MACOS ? "VNC" : "RDP") << "|"
            << p.name << "|" << p.host << "|" << p.port << "|"
            << p.username << "|" << p.domain << "|"
            << p.req_width << "|" << p.req_height << "\n";
    }
}

void ProfileManager::add_or_update(const ConnectionProfile& profile, const std::string& path) {
    ConnectionProfile clean = profile;
    clean.from_mdns = false;
    if (clean.name.empty()) {
        clean.name = (clean.protocol == ProtocolType::VNC_MACOS ? "Mac " : "Windows ") + clean.host;
    }
    bool updated = false;
    for (auto& s : saved_) {
        if (s.host == clean.host && s.port == clean.port && s.protocol == clean.protocol) {
            s = clean;
            updated = true;
            break;
        }
    }
    if (!updated) saved_.push_back(clean);
    save(path);
}

void ProfileManager::remove_at(size_t idx, const std::string& path) {
    if (idx < saved_.size()) {
        saved_.erase(saved_.begin() + static_cast<ptrdiff_t>(idx));
        save(path);
    }
}

bool ProfileManager::start_mdns() {
    if (mdns_sock_ >= 0) return true;
    int s = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
    if (s < 0) return false;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags >= 0) fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(5353);
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        bind_addr.sin_port = 0;
        bind(s, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr));
    }

    ip_mreq mreq{};
    inet_pton(AF_INET, "224.0.0.251", &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof(mreq));

    mdns_sock_ = s;
    send_mdns_query();
    return true;
}

void ProfileManager::stop_mdns() {
    if (mdns_sock_ >= 0) {
        CLOSE_SOCK(mdns_sock_);
        mdns_sock_ = -1;
    }
}

void ProfileManager::send_mdns_query() {
    if (mdns_sock_ < 0) return;
    // Standard DNS PTR query for "_rfb._tcp.local" (macOS Screen Sharing Bonjour service!)
    static constexpr uint8_t query[] = {
        0x00, 0x00, 0x00, 0x00,  // ID=0, Flags=0 (standard query)
        0x00, 0x01, 0x00, 0x00,  // QDCOUNT=1, ANCOUNT=0
        0x00, 0x00, 0x00, 0x00,  // NSCOUNT=0, ARCOUNT=0
        0x04, '_', 'r', 'f', 'b',
        0x04, '_', 't', 'c', 'p',
        0x05, 'l', 'o', 'c', 'a', 'l',
        0x00,
        0x00, 0x0C,              // QTYPE = PTR (12)
        0x00, 0x01               // QCLASS = IN (1)
    };
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &dst.sin_addr);
    sendto(mdns_sock_, reinterpret_cast<const char*>(query), sizeof(query), 0,
           reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    last_query_us_ = platform::now_us();
}

void ProfileManager::poll_mdns() {
    if (mdns_sock_ < 0) return;
    uint64_t now = platform::now_us();
    if (now - last_query_us_ > 4000000ULL) {
        send_mdns_query();
    }

    uint8_t buf[1500];
    for (int iter = 0; iter < 8; ++iter) {
        sockaddr_in from{};
        socklen_t flen = sizeof(from);
        int n = static_cast<int>(recvfrom(mdns_sock_, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                          reinterpret_cast<sockaddr*>(&from), &flen));
        if (n < 24) break;
        // Check if DNS response mentions "_rfb" (macOS Screen Sharing)
        bool mentions_rfb = false;
        for (int i = 12; i + 4 <= n; ++i) {
            if (buf[i] == '_' && buf[i + 1] == 'r' && buf[i + 2] == 'f' && buf[i + 3] == 'b') {
                mentions_rfb = true;
                break;
            }
        }
        if (!mentions_rfb) continue;

        char ip_str[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &from.sin_addr, ip_str, sizeof(ip_str));
        std::string ip(ip_str);

        bool exists = false;
        for (const auto& m : mdns_macs_) {
            if (m.host == ip) { exists = true; break; }
        }
        if (!exists && !ip.empty()) {
            ConnectionProfile mac;
            mac.name = "Mac (" + ip + ")";
            mac.protocol = ProtocolType::VNC_MACOS;
            mac.host = ip;
            mac.port = 5900;
            mac.from_mdns = true;
            mdns_macs_.push_back(mac);
        }
    }
}

std::vector<ConnectionProfile> ProfileManager::all_profiles() const {
    std::vector<ConnectionProfile> out = saved_;
    for (const auto& m : mdns_macs_) {
        bool dup = false;
        for (const auto& s : out) {
            if (s.host == m.host && s.port == m.port && s.protocol == ProtocolType::VNC_MACOS) {
                dup = true;
                break;
            }
        }
        if (!dup) out.push_back(m);
    }
    return out;
}
