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

bool init();
void shutdown();

// Returns true if running on Switch 2 handheld/docked hardware
bool is_switch_hardware();

// ASCII-only config directory (sdmc:/switch/OunceDesk on Switch 2, ./config on Desktop)
std::string get_config_dir();

// Native system software keyboard (swkbd on Switch 2; returns false on Desktop to use OSK)
bool show_system_keyboard(const std::string& header, const std::string& initial, std::string& out_text, bool numeric_only = false);

// Poll native USB/BT keyboard & mouse attached to Switch 2 (no-op on Desktop where SDL handles them)
void poll_physical_hid(PhysicalMouseDelta& out_mouse, std::vector<PhysicalKeyTransition>& out_keys);

// Monotonic time in microseconds
uint64_t now_us();

// Optional system shared TrueType font buffer (Switch 2 plGetSharedFontByType)
bool get_shared_font(const void*& out_ptr, size_t& out_size);

}  // namespace platform
