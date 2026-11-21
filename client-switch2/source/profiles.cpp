#include "profiles.hpp"

#include "platform.hpp"

#include <algorithm>
#include <cstdio>
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
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#define CLOSE_SOCK close
#endif

namespace {

void set_nonblocking(int s, bool nb) {
#ifdef _WIN32
    u_long mode = nb ? 1 : 0;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags >= 0) fcntl(s, F_SETFL, nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

std::string ipv4_to_str(uint32_t ip_host) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                  (ip_host >> 24) & 0xFFu,
                  (ip_host >> 16) & 0xFFu,
                  (ip_host >> 8) & 0xFFu,
                  ip_host & 0xFFu);
    return buf;
}

// Parallel non-blocking TCP connect probe for a batch of IPs on a given port
std::vector<uint32_t> probe_port_batch(const std::vector<uint32_t>& ips_host, uint16_t port, int timeout_ms) {
    struct Entry {
        int sock;
        uint32_t ip_host;
    };
    std::vector<Entry> entries;
    entries.reserve(ips_host.size());

    for (uint32_t iph : ips_host) {
        int s = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
        if (s < 0) continue;
        set_nonblocking(s, true);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(iph);
        connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        entries.push_back({s, iph});
    }

    std::vector<uint32_t> open_ips;
    if (entries.empty()) return open_ips;

    uint64_t deadline = platform::now_us() + static_cast<uint64_t>(timeout_ms) * 1000ULL;
    std::vector<bool> done(entries.size(), false);

    while (platform::now_us() < deadline) {
        fd_set wfds;
        FD_ZERO(&wfds);
        int max_fd = -1;
        int remaining = 0;
        for (size_t i = 0; i < entries.size(); ++i) {
            if (!done[i]) {
                FD_SET(entries[i].sock, &wfds);
                if (entries[i].sock > max_fd) max_fd = entries[i].sock;
                ++remaining;
            }
        }
        if (remaining == 0) break;

        uint64_t left_us = deadline - platform::now_us();
        timeval tv{};
        tv.tv_sec = static_cast<long>(left_us / 1000000ULL);
        tv.tv_usec = static_cast<long>(std::min<uint64_t>(left_us % 1000000ULL, 50000ULL));
        int ready = select(max_fd + 1, nullptr, &wfds, nullptr, &tv);
        if (ready <= 0) continue;

        for (size_t i = 0; i < entries.size(); ++i) {
            if (!done[i] && FD_ISSET(entries[i].sock, &wfds)) {
                int so_err = 0;
                socklen_t elen = sizeof(so_err);
                getsockopt(entries[i].sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &elen);
                if (so_err == 0) {
                    open_ips.push_back(entries[i].ip_host);
                }
                done[i] = true;
            }
        }
    }

    for (const auto& e : entries) CLOSE_SOCK(e.sock);
    return open_ips;
}

}  // namespace

ProfileManager::~ProfileManager() { stop_lan_scan(); }

void ProfileManager::load(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    saved_.clear();
    std::ifstream in(path);
    if (!in.is_open()) return;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> fields;
        std::stringstream ss(line);
        std::string item;
        while (std::getline(ss, item, '|')) {
            fields.push_back(item);
        }
        // Keep trailing empty field if line ended with '|'
        if (!line.empty() && line.back() == '|') {
            fields.push_back("");
        }
        if (fields.size() >= 8) {
            ConnectionProfile p;
            p.protocol = (fields[0] == "VNC") ? ProtocolType::VNC_MACOS : ProtocolType::RDP_WINDOWS;
            p.name = fields[1];
            p.host = fields[2];
            p.port = static_cast<uint16_t>(std::atoi(fields[3].c_str()));
            if (p.port == 0) p.port = (p.protocol == ProtocolType::VNC_MACOS) ? 5900 : 3389;
            p.username = fields[4];
            p.domain = fields[5];
            p.req_width = static_cast<uint16_t>(std::max(640, std::atoi(fields[6].c_str())));
            p.req_height = static_cast<uint16_t>(std::max(480, std::atoi(fields[7].c_str())));
            if (fields.size() >= 9) {
                p.password = fields[8];
            }
            p.from_mdns = false;
            saved_.push_back(p);
        }
    }
}

void ProfileManager::save(const std::string& path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) return;
    out << "# OunceDesk connections (proto|name|host|port|user|domain|w|h|password)\n";
    for (const auto& p : saved_) {
        out << (p.protocol == ProtocolType::VNC_MACOS ? "VNC" : "RDP") << "|"
            << p.name << "|" << p.host << "|" << p.port << "|"
            << p.username << "|" << p.domain << "|"
            << p.req_width << "|" << p.req_height << "|"
            << p.password << "\n";
    }
}

void ProfileManager::add_or_update(const ConnectionProfile& profile, const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    ConnectionProfile clean = profile;
    clean.from_mdns = false;
    if (clean.name.empty()) {
        clean.name = (clean.protocol == ProtocolType::VNC_MACOS ? "Mac " : "Windows ") + clean.host;
    }
    bool updated = false;
    for (auto& s : saved_) {
        if (s.host == clean.host && s.port == clean.port && s.protocol == clean.protocol) {
            if (clean.password.empty() && !s.password.empty()) {
                clean.password = s.password;
            }
            if (clean.username.empty() && !s.username.empty()) {
                clean.username = s.username;
            }
            s = clean;
            updated = true;
            break;
        }
    }
    if (!updated) saved_.push_back(clean);
    save(path);
}

void ProfileManager::remove_saved(const ConnectionProfile& target, const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    saved_.erase(
        std::remove_if(saved_.begin(), saved_.end(), [&](const ConnectionProfile& p) {
            return p.host == target.host && p.port == target.port && p.protocol == target.protocol;
        }),
        saved_.end());
    save(path);
}

bool ProfileManager::probe_rfb_banner(uint32_t ip_net, std::string& out_banner) {
    out_banner.clear();
    int s = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (s < 0) return false;

#ifdef _WIN32
    DWORD tv = 350;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{0, 350000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5900);
    addr.sin_addr.s_addr = ip_net;

    bool ok = false;
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        char buf[16]{};
        int n = static_cast<int>(recv(s, buf, 12, 0));
        if (n >= 11 && std::strncmp(buf, "RFB ", 4) == 0) {
            out_banner.assign(buf, static_cast<size_t>(n));
            ok = true;
        }
    }
    CLOSE_SOCK(s);
    return ok;
}

void ProfileManager::start_lan_scan() {
    stop_lan_scan();
    stop_scan_.store(false);
    scanning_.store(true);
    scan_pct_.store(0);
    {
        std::lock_guard<std::mutex> lk(mu_);
        discovered_.clear();
    }
    scan_thread_ = std::thread(&ProfileManager::run_lan_scan, this);
}

void ProfileManager::stop_lan_scan() {
    stop_scan_.store(true);
    if (scan_thread_.joinable()) {
        scan_thread_.join();
    }
    scanning_.store(false);
}

void ProfileManager::run_lan_scan() {
    uint32_t local_ip = platform::get_local_ipv4_host_order();
    if (local_ip == 0) {
        scanning_.store(false);
        scan_pct_.store(100);
        return;
    }

    uint32_t subnet = local_ip & 0xFFFFFF00u;
    constexpr int BATCH = 32;

    for (int host_id = 1; host_id <= 254 && !stop_scan_.load(); host_id += BATCH) {
        std::vector<uint32_t> batch;
        for (int h = host_id; h < host_id + BATCH && h <= 254; ++h) {
            uint32_t iph = subnet | static_cast<uint32_t>(h);
            if (iph != local_ip) batch.push_back(iph);
        }

        // Probe Port 3389 (Windows RDP) and Port 5900 (macOS Screen Sharing / VNC)
        auto open_3389 = probe_port_batch(batch, 3389, 160);
        if (stop_scan_.load()) break;
        auto open_5900 = probe_port_batch(batch, 5900, 160);
        if (stop_scan_.load()) break;

        // 1. Every host with TCP 3389 open is strictly Windows RDP (3389)
        for (uint32_t iph : open_3389) {
            ConnectionProfile p;
            p.protocol = ProtocolType::RDP_WINDOWS;
            p.host = ipv4_to_str(iph);
            p.port = 3389;
            p.name = "Windows (" + p.host + ")";
            p.req_width = 1920;
            p.req_height = 1080;
            p.from_mdns = true;
            std::lock_guard<std::mutex> lk(mu_);
            discovered_.push_back(p);
        }

        // 2. For TCP 5900 open: verify RFB banner!
        //    - "RFB 003.889" is Apple macOS Screen Sharing's unique signature.
        //    - If host ALSO has 3389 open and banner is NOT "RFB 003.889", it is Windows -> DO NOT add to macOS!
        for (uint32_t iph : open_5900) {
            if (stop_scan_.load()) break;
            bool has_3389 = std::find(open_3389.begin(), open_3389.end(), iph) != open_3389.end();
            std::string banner;
            if (!probe_rfb_banner(htonl(iph), banner)) continue;

            bool is_apple_screen_sharing = (banner.rfind("RFB 003.889", 0) == 0);
            if (has_3389 && !is_apple_screen_sharing) {
                continue;  // Windows PC running VNC -> keep strictly in Windows 3389 column!
            }

            ConnectionProfile p;
            p.protocol = ProtocolType::VNC_MACOS;
            p.host = ipv4_to_str(iph);
            p.port = 5900;
            p.name = (is_apple_screen_sharing ? "macOS (" : "Mac/VNC (") + p.host + ")";
            p.from_mdns = true;
            std::lock_guard<std::mutex> lk(mu_);
            discovered_.push_back(p);
        }

        scan_pct_.store(std::min(100, (host_id + BATCH) * 100 / 254));
    }

    scan_pct_.store(100);
    scanning_.store(false);
}

std::vector<ConnectionProfile> ProfileManager::windows_profiles() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ConnectionProfile> out;
    for (const auto& s : saved_) {
        if (s.protocol == ProtocolType::RDP_WINDOWS) out.push_back(s);
    }
    for (const auto& d : discovered_) {
        if (d.protocol != ProtocolType::RDP_WINDOWS) continue;
        bool dup = false;
        for (const auto& e : out) {
            if (e.host == d.host && e.port == d.port) { dup = true; break; }
        }
        if (!dup) out.push_back(d);
    }
    return out;
}

std::vector<ConnectionProfile> ProfileManager::macos_profiles() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ConnectionProfile> out;
    for (const auto& s : saved_) {
        if (s.protocol == ProtocolType::VNC_MACOS) out.push_back(s);
    }
    for (const auto& d : discovered_) {
        if (d.protocol != ProtocolType::VNC_MACOS) continue;
        bool dup = false;
        for (const auto& e : out) {
            if (e.host == d.host && e.port == d.port) { dup = true; break; }
        }
        if (!dup) out.push_back(d);
    }
    return out;
}

std::vector<ConnectionProfile> ProfileManager::all_profiles() const {
    auto win = windows_profiles();
    auto mac = macos_profiles();
    win.insert(win.end(), mac.begin(), mac.end());
    return win;
}
