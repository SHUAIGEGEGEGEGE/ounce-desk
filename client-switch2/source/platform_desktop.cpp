#ifndef __SWITCH__

#include "platform.hpp"

#include <chrono>
#include <filesystem>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace platform {

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

std::string get_config_dir() { return "config"; }

bool show_system_keyboard(const std::string&, const std::string&, std::string&, bool) {
    // Desktop simulator uses built-in PIN/OSK + physical keyboard
    return false;
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
