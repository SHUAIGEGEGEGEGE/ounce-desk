#include "../client-switch2/source/net_connect.hpp"
#include <cstdio>
#include <thread>

int main() {
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    int fails = 0;
    struct C { const char* in; const char* host; uint16_t port; } cases[] = {
        {"192.168.33.186", "192.168.33.186", 3389},
        {"192.168.33.186:3390", "192.168.33.186", 3390},
        {"myhome.ddns.net", "myhome.ddns.net", 3389},
        {" myhome.ddns.net:5901 ", "myhome.ddns.net", 5901},
        {"2408:8000::1", "2408:8000::1", 3389},
        {"[2408:8000::1]:3391", "2408:8000::1", 3391},
        {"rdp://example.com:4000/", "example.com", 4000},
    };
    for (auto& c : cases) {
        std::string h; uint16_t p = 3389;
        netc::split_host_port(c.in, h, p);
        if (h != c.host || p != c.port) { ++fails; std::printf("split fail '%s' -> '%s' %u\n", c.in, h.c_str(), p); }
    }
    // Live IPv6 loopback connect
    int ls = static_cast<int>(socket(AF_INET6, SOCK_STREAM, 0));
    sockaddr_in6 a{}; a.sin6_family = AF_INET6; a.sin6_addr = in6addr_loopback; a.sin6_port = htons(47391);
    if (ls >= 0 && bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 && listen(ls, 16) == 0) {
        std::string err, addr;
        int s = netc::tcp_connect("::1", 47391, 2000, err, &addr);
        if (s < 0) { ++fails; std::printf("ipv6 connect fail: %s\n", err.c_str()); }
        else { std::printf("IPv6 loopback connected via %s\n", addr.c_str()); netc::close_sock(s); }
        std::string h; uint16_t p = 0;
        netc::split_host_port("[::1]:47391", h, p);
        s = netc::tcp_connect(h, p, 2000, err, &addr);
        if (s < 0) { ++fails; std::printf("bracket ipv6 connect fail\n"); } else netc::close_sock(s);
        s = netc::tcp_connect("localhost", 47391, 2000, err, &addr);
        if (s < 0) { ++fails; std::printf("hostname connect fail\n"); } else { std::printf("hostname 'localhost' -> %s\n", addr.c_str()); netc::close_sock(s); }
    } else {
        std::printf("(skip IPv6 live test: no IPv6 loopback)\n");
    }
    std::string err;
    if (netc::tcp_connect("no-such-host.invalid", 3389, 1000, err) >= 0) { ++fails; }
    else std::printf("bad host -> %s\n", err.c_str());
    std::printf(fails ? "NET TESTS FAILED\n" : "ALL NET TESTS PASSED\n");
    return fails;
}
