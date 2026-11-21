// Live end-to-end test of the OunceDesk userspace WireGuard client against the user's real router.
// Usage: wg_live.exe <conf> [endpoint_override_host port]
#define SDL_MAIN_HANDLED
#include "../client-switch2/source/net_connect.hpp"
#include "../client-switch2/source/platform.hpp"
#include "../client-switch2/source/wg_tunnel.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: wg_live <conf> [ep_host ep_port]\n"); return 2; }
    platform::init();
    wg::Config cfg;
    std::string err;
    if (!wg::load_config_file(argv[1], cfg, err)) { std::printf("config error: %s\n", err.c_str()); return 1; }
    std::printf("config '%s': endpoint %s:%u, tunnel ip set, keepalive %d\n", cfg.name.c_str(), cfg.endpoint_host.c_str(), cfg.endpoint_port, cfg.keepalive);
    wg::install_connect_hook();
    auto& t = wg::Tunnel::instance();
    if (argc >= 4) t.set_endpoint_override(argv[2], static_cast<uint16_t>(std::atoi(argv[3])));
    t.start(cfg);
    for (int i = 0; i < 150 && t.state() != wg::State::UP; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::printf("state=%d status=%s\n", static_cast<int>(t.state()), t.status_text().c_str());
    if (t.state() != wg::State::UP) { t.stop(); return 3; }

    int rc = 0;
    // 1) HTTP GET to router LAN web UI through the tunnel
    {
        std::string e, addr;
        int s = netc::tcp_connect("192.168.33.1", 80, 6000, e, &addr);
        if (s < 0) { std::printf("[FAIL] tcp 192.168.33.1:80 %s\n", e.c_str()); rc = 4; }
        else {
            const char* req = "GET / HTTP/1.0\r\nHost: 192.168.33.1\r\n\r\n";
            send(s, req, static_cast<int>(std::strlen(req)), 0);
            char buf[4096];
            size_t total = 0;
            std::string head;
            for (;;) {
                int n = static_cast<int>(recv(s, buf, sizeof(buf), 0));
                if (n <= 0) break;
                if (head.size() < 64) head.append(buf, std::min<size_t>(n, 64));
                total += static_cast<size_t>(n);
            }
            netc::close_sock(s);
            std::printf("[%s] HTTP via tunnel: %zu bytes, first line: %.40s\n", total ? "PASS" : "FAIL", total, head.c_str());
            if (!total) rc = 5;
        }
    }
    // 2) Windows RDP X.224 Connection Request -> expect Connection Confirm (no login, no session)
    {
        std::string e;
        int s = netc::tcp_connect("192.168.33.186", 3389, 6000, e);
        if (s < 0) { std::printf("[FAIL] tcp 192.168.33.186:3389 %s\n", e.c_str()); rc = 6; }
        else {
            const uint8_t cr[] = {0x03, 0x00, 0x00, 0x13, 0x0E, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x01, 0x00, 0x08, 0x00, 0x03, 0x00, 0x00, 0x00};
            send(s, reinterpret_cast<const char*>(cr), sizeof(cr), 0);
            uint8_t rsp[64];
            int n = static_cast<int>(recv(s, reinterpret_cast<char*>(rsp), sizeof(rsp), 0));
            netc::close_sock(s);
            bool ok = n >= 7 && rsp[0] == 0x03 && rsp[5] == 0xD0;
            std::printf("[%s] RDP X.224 via tunnel: %d bytes, CC=%s\n", ok ? "PASS" : "FAIL", n, ok ? "yes" : "no");
            if (!ok) rc = 7;
        }
    }
    // 3) macOS RFB banner
    {
        std::string e;
        int s = netc::tcp_connect("192.168.33.5", 5900, 6000, e);
        if (s < 0) { std::printf("[INFO] tcp 192.168.33.5:5900 %s\n", e.c_str()); }
        else {
            char b[16]{};
            int n = static_cast<int>(recv(s, b, 12, 0));
            netc::close_sock(s);
            std::printf("[%s] macOS RFB via tunnel: %.11s\n", n == 12 ? "PASS" : "INFO", b);
        }
    }
    std::printf("tunnel rx=%llu tx=%llu bytes\n", (unsigned long long)t.rx_bytes(), (unsigned long long)t.tx_bytes());
    t.stop();
    platform::shutdown();
    return rc;
}
