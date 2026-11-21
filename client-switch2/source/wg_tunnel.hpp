#pragma once
// OunceDesk built-in userspace WireGuard client (app-only tunnel).
//
// * 100% inside the OunceDesk .nro process: no sysmodule, no system file changes, no NAND access.
// * Implements WireGuard Noise_IKpsk2 handshake + transport over a normal UDP socket, plus a tiny
//   userspace TCP/IPv4 stack. Each remote-desktop TCP connection is bridged through a private
//   127.0.0.1 loopback forwarder, so the RDP / VNC backends work unchanged.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wg {

struct Config {
    std::string name;            // file name without .conf
    uint8_t private_key[32]{};
    uint8_t peer_public[32]{};
    uint8_t preshared[32]{};     // zeros when absent
    uint32_t address = 0;        // tunnel IPv4 (host order), e.g. 10.0.0.22
    std::string endpoint_host;
    uint16_t endpoint_port = 51820;
    int keepalive = 25;
    int mtu = 1420;
};

bool parse_config(const std::string& text, Config& out, std::string& err);
bool load_config_file(const std::string& path, Config& out, std::string& err);
// List *.conf in dir (names without extension)
std::vector<std::string> list_configs(const std::string& dir);

enum class State { OFF, CONNECTING, UP, FAILED };

class Tunnel {
public:
    static Tunnel& instance();

    bool start(const Config& cfg);
    void stop();

    State state() const { return state_.load(); }
    std::string status_text() const;
    std::string active_name() const;
    uint64_t rx_bytes() const { return rx_bytes_.load(); }
    uint64_t tx_bytes() const { return tx_bytes_.load(); }

    // Create a 127.0.0.1 forwarder to dst_ip:dst_port through the tunnel. Returns local port (0 = error).
    uint16_t open_forward(uint32_t dst_ip_host, uint16_t dst_port, std::string& err);

    // Optional: override endpoint (used by desktop tests for LAN hairpin)
    void set_endpoint_override(const std::string& host, uint16_t port) { ep_override_host_ = host; ep_override_port_ = port; }

    struct Impl;

private:
    Tunnel() = default;
    ~Tunnel();
    Tunnel(const Tunnel&) = delete;
    Tunnel& operator=(const Tunnel&) = delete;

    void run();

    std::atomic<State> state_{State::OFF};
    std::atomic<bool> stop_{true};
    std::thread thread_;
    mutable std::mutex mu_;
    std::string status_;
    std::string name_;
    Config cfg_;
    Impl* impl_ = nullptr;
    std::atomic<uint64_t> rx_bytes_{0};
    std::atomic<uint64_t> tx_bytes_{0};
    std::string ep_override_host_;
    uint16_t ep_override_port_ = 0;

    void set_status(State st, const std::string& s);
};

// Install the netc::tcp_connect redirect hook (call once at startup)
void install_connect_hook();

}  // namespace wg
