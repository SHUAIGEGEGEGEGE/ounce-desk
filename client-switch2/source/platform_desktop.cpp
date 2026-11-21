#ifndef __SWITCH__

#include "platform.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#define CLOSE_SOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSE_SOCK close
#endif

namespace platform {

namespace {
std::mutex g_log_mu;
}

void log_line(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_log_mu);
    FILE* f = std::fopen("config/ouncedesk.log", "a");
    if (f) {
        uint64_t ms = now_us() / 1000ULL;
        std::fprintf(f, "[%llu ms] %s\n", static_cast<unsigned long long>(ms), msg.c_str());
        std::fclose(f);
    }
}

bool init() {
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return false;
    }
#endif
    std::error_code ec;
    std::filesystem::create_directories("config", ec);
    return true;
}

void shutdown() {
#ifdef _WIN32
    WSACleanup();
#endif
}

bool is_switch_hardware() { return false; }
bool uses_native_touch() { return false; }

std::string get_config_dir() { return "config"; }

uint32_t get_local_ipv4_host_order() {
    int s = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
    if (s < 0) return 0;
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr);
    uint32_t out = 0;
    if (connect(s, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) == 0) {
        sockaddr_in local{};
        socklen_t len = sizeof(local);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            out = ntohl(local.sin_addr.s_addr);
        }
    }
    CLOSE_SOCK(s);
    return out;
}

bool show_system_keyboard(const std::string&, const std::string&, std::string&, bool, bool) {
    return false;
}

void poll_native_touch(std::vector<NativeTouchPoint>& out_points) {
    out_points.clear();
}

void poll_physical_hid(PhysicalMouseDelta& out_mouse, std::vector<PhysicalKeyTransition>& out_keys) {
    out_mouse = {};
    out_keys.clear();
}

uint64_t now_us() {
    auto tp = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(tp).count());
}

bool get_shared_font(const void*&, size_t&) { return false; }

}  // namespace platform

#endif  // !__SWITCH__
