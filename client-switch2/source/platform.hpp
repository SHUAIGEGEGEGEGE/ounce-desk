#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace platform {

struct PhysicalMouseDelta {
    int32_t dx = 0;
    int32_t dy = 0;
    int32_t wheel_x = 0;
    int32_t wheel_y = 0;
    uint32_t buttons = 0;  // bit0=L, bit1=R, bit2=M, bit3=Back, bit4=Forward
    bool active = false;
};

struct PhysicalKeyTransition {
    uint16_t hid_usage = 0;
    bool pressed = false;
    uint8_t modifiers = 0;
};

struct NativeTouchPoint {
    uint32_t id = 0;
    float x = 0.0f;  // Exact logical pixel X in 0..1920
    float y = 0.0f;  // Exact logical pixel Y in 0..1080
};

bool init();
void shutdown();

bool is_switch_hardware();
bool uses_native_touch();

// ASCII-only config directory (sdmc:/switch/OunceDesk on Switch 2, ./config on Desktop)
std::string get_config_dir();

// Local LAN IPv4 address in host byte order (e.g. 192.168.1.42 -> 0xC0A8012A), 0 if unavailable
uint32_t get_local_ipv4_host_order();

// Native Switch 2 software keyboard (swkbd)
bool show_system_keyboard(const std::string& header, const std::string& initial,
                          std::string& out_text, bool numeric_only = false, bool password_mode = false);

// Poll exact 1:1 1920x1080 native touch points from libounce hidGetTouchScreenStates
void poll_native_touch(std::vector<NativeTouchPoint>& out_points);

// Poll native USB/BT keyboard & mouse attached to Switch 2
void poll_physical_hid(PhysicalMouseDelta& out_mouse, std::vector<PhysicalKeyTransition>& out_keys);

uint64_t now_us();

bool get_shared_font(const void*& out_ptr, size_t& out_size);

}  // namespace platform
