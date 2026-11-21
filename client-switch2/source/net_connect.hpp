#pragma once
// Shared TCP connect helper: IPv4 + IPv6 + DDNS domain names, with connect timeout.
//
// Accepted host formats:
//   192.168.1.50            IPv4 literal
//   2001:db8::10            IPv6 literal (also fe80::1%wlan0)
//   [2001:db8::10]:3390     bracketed IPv6 with port override
//   myhome.ddns.net         DDNS / any DNS name (A and AAAA records are both tried)
//   myhome.ddns.net:3390    DNS name / IPv4 with port override

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace netc {

// Optional tunnel redirect hook (installed by the built-in WireGuard client). Returns true and fills
// out_port with a 127.0.0.1 forwarder port when the connection must go through the tunnel.
using TunnelRedirectFn = bool (*)(const std::string& host, uint16_t port, uint16_t& out_port, std::string& err);
inline TunnelRedirectFn& tunnel_redirect() {
    static TunnelRedirectFn fn = nullptr;
    return fn;
}

inline void trim(std::string& s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n' || s.back() == '/')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    s.erase(0, i);
}

// Split user input into host + port (port stays default_port if not specified)
inline void split_host_port(const std::string& input, std::string& out_host, uint16_t& io_port) {
    std::string s = input;
    trim(s);
    // Strip accidental URL schemes like rdp:// vnc://
    size_t scheme = s.find("://");
    if (scheme != std::string::npos) s = s.substr(scheme + 3);
    trim(s);
    out_host = s;
    if (s.empty()) return;
    if (s[0] == '[') {
        size_t rb = s.find(']');
        if (rb != std::string::npos) {
            out_host = s.substr(1, rb - 1);
            if (rb + 2 <= s.size() && s[rb + 1] == ':') {
                int p = std::atoi(s.c_str() + rb + 2);
                if (p > 0 && p <= 65535) io_port = static_cast<uint16_t>(p);
            }
        }
        return;
    }
    size_t first = s.find(':');
    size_t last = s.rfind(':');
    if (first != std::string::npos && first == last) {
        // Exactly one ':' -> host:port (IPv4 or domain)
        int p = std::atoi(s.c_str() + first + 1);
        out_host = s.substr(0, first);
        if (p > 0 && p <= 65535) io_port = static_cast<uint16_t>(p);
    }
    // 2+ colons without brackets -> bare IPv6 literal, keep as-is
}

inline void close_sock(int s) {
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

inline void set_nb(int s, bool nb) {
#ifdef _WIN32
    u_long mode = nb ? 1 : 0;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags >= 0) fcntl(s, F_SETFL, nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

// Resolve (A + AAAA) and connect with per-address timeout. Returns socket or -1 (err filled).
inline int tcp_connect(const std::string& host_in, uint16_t port_in, int timeout_ms, std::string& err,
                       std::string* out_addr = nullptr) {
    std::string host = host_in;
    uint16_t port = port_in;
    if (tunnel_redirect()) {
        uint16_t lp = 0;
        std::string terr;
        if (tunnel_redirect()(host_in, port_in, lp, terr)) {
            host = "127.0.0.1";
            port = lp;
            if (out_addr) *out_addr = "WireGuard→" + host_in;
            out_addr = nullptr;
        } else if (!terr.empty()) {
            err = terr;
            return -1;
        }
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    std::string port_s = std::to_string(port);
    int gai = getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res);
    if (gai != 0 || !res) {
        err = "无法解析主机/域名: " + host;
        return -1;
    }

    int result = -1;
    err = "无法连接 " + host + ":" + port_s;
    for (addrinfo* ai = res; ai && result < 0; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) continue;
        int s = static_cast<int>(socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (s < 0) continue;  // e.g. IPv6 unsupported on this network stack -> try next address
        set_nb(s, true);
        int rc = connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        bool ok = (rc == 0);
        if (!ok) {
            fd_set wfds, efds;
            FD_ZERO(&wfds);
            FD_ZERO(&efds);
            FD_SET(s, &wfds);
            FD_SET(s, &efds);
            timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            int sel = select(s + 1, nullptr, &wfds, &efds, &tv);
            if (sel > 0 && FD_ISSET(s, &wfds)) {
                int so_err = 0;
#ifdef _WIN32
                int elen = sizeof(so_err);
#else
                socklen_t elen = sizeof(so_err);
#endif
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &elen);
                ok = (so_err == 0);
            }
        }
        if (!ok) {
            close_sock(s);
            continue;
        }
        set_nb(s, false);
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#ifdef _WIN32
        DWORD tvv = 5000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tvv), sizeof(tvv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tvv), sizeof(tvv));
#else
        timeval tvv{5, 0};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tvv, sizeof(tvv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tvv, sizeof(tvv));
#endif
        if (out_addr) {
            char buf[64]{};
            if (ai->ai_family == AF_INET) {
                const uint8_t* b = reinterpret_cast<const uint8_t*>(&reinterpret_cast<sockaddr_in*>(ai->ai_addr)->sin_addr);
                std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
                *out_addr = buf;
            } else {
                const uint8_t* b = reinterpret_cast<const uint8_t*>(&reinterpret_cast<sockaddr_in6*>(ai->ai_addr)->sin6_addr);
                std::string v6 = "[";
                for (int i = 0; i < 16; i += 2) {
                    char g[8];
                    std::snprintf(g, sizeof(g), "%x", (b[i] << 8) | b[i + 1]);
                    if (i) v6 += ":";
                    v6 += g;
                }
                *out_addr = v6 + "]";
            }
        }
        result = s;
    }
    freeaddrinfo(res);
    return result;
}

}  // namespace netc
