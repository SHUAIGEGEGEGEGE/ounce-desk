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
    uint32_t buttons = 0;      // bit0=L, bit1=R, bit2=M, bit3=Back, bit4=Forward
    bool active = false;
    bool joycon_mouse = false; // true when Switch 2 Joy-Con 2 optical mouse (Nmouse) is engaged
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

std::string get_config_dir();

// Append timestamped line to sdmc:/switch/OunceDesk/ouncedesk.log (readable over FTP!)
void log_line(const std::string& msg);

uint32_t get_local_ipv4_host_order();

bool show_system_keyboard(const std::string& header, const std::string& initial,
                          std::string& out_text, bool numeric_only = false, bool password_mode = false);

void poll_native_touch(std::vector<NativeTouchPoint>& out_points);
void poll_physical_hid(PhysicalMouseDelta& out_mouse, std::vector<PhysicalKeyTransition>& out_keys);

uint64_t now_us();

bool get_shared_font(const void*& out_ptr, size_t& out_size);

}  // namespace platform
