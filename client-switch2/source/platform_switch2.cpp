#ifdef __SWITCH__

#include "platform.hpp"

#include <chrono>
#include <cstring>
#include <sys/stat.h>
#include <switch.h>

extern "C" void hidSetTouchScreenOutputRanges(u32 width, u32 height) __attribute__((weak));

namespace platform {

namespace {
bool g_socket_ok = false;
bool g_pl_ok = false;
bool g_kbd_ok = false;
bool g_mouse_ok = false;
PlFontData g_shared_font{};
bool g_has_shared_font = false;

HidKeyboardState g_prev_kbd{};
HidMouseState g_prev_mouse{};
bool g_has_prev_kbd = false;
bool g_has_prev_mouse = false;
}  // namespace

bool init() {
    if (R_SUCCEEDED(socketInitializeDefault())) {
        g_socket_ok = true;
    }

    if (R_SUCCEEDED(plInitialize(PlServiceType_User))) {
        g_pl_ok = true;
        if (R_SUCCEEDED(plGetSharedFontByType(&g_shared_font, PlSharedFontType_Standard))) {
            g_has_shared_font = true;
        }
    }

    // Switch 2 (libounce) supports 1920x1080 native touch output ranges
    if (hidSetTouchScreenOutputRanges) {
        hidSetTouchScreenOutputRanges(1920, 1080);
    }
    hidInitializeTouchScreen();
    hidInitializeKeyboard();
    g_kbd_ok = true;
    hidInitializeMouse();
    g_mouse_ok = true;

    // Ensure ASCII-only config directory exists (kaiHEN hides non-ASCII paths in OunceShell/FTP)
    mkdir("sdmc:/switch", 0755);
    mkdir("sdmc:/switch/OunceDesk", 0755);
    return g_socket_ok;
}

void shutdown() {
    if (g_pl_ok) {
        plExit();
        g_pl_ok = false;
    }
    if (g_socket_ok) {
        socketExit();
        g_socket_ok = false;
    }
}

bool is_switch_hardware() { return true; }

std::string get_config_dir() { return "sdmc:/switch/OunceDesk"; }

bool show_system_keyboard(const std::string& header, const std::string& initial, std::string& out_text, bool numeric_only) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) {
        return false;
    }
    swkbdConfigMakePresetDefault(&kbd);
    if (numeric_only) {
        swkbdConfigSetType(&kbd, SwkbdType_NumPad);
        swkbdConfigSetStringLenMax(&kbd, 16);
    } else {
        swkbdConfigSetStringLenMax(&kbd, 256);
    }
    swkbdConfigSetHeaderText(&kbd, header.c_str());
    if (!initial.empty()) {
        swkbdConfigSetInitialText(&kbd, initial.c_str());
    }

    char buf[512] = {0};
    Result rc = swkbdShow(&kbd, buf, sizeof(buf));
    swkbdClose(&kbd);
    if (R_SUCCEEDED(rc)) {
        out_text = buf;
        return true;
    }
    return false;
}

static uint8_t convert_hid_modifiers(u64 mods) {
    uint8_t out = 0;
    if (mods & HidKeyboardModifier_Control) out |= (1 << 0);
    if (mods & HidKeyboardModifier_Shift)   out |= (1 << 1);
    if (mods & HidKeyboardModifier_LeftAlt) out |= (1 << 2);
    if (mods & HidKeyboardModifier_Gui)     out |= (1 << 3);
    if (mods & HidKeyboardModifier_RightAlt) out |= (1 << 6);
    return out;
}

void poll_physical_hid(PhysicalMouseDelta& out_mouse, std::vector<PhysicalKeyTransition>& out_keys) {
    out_mouse = {};
    out_keys.clear();

    if (g_mouse_ok) {
        HidMouseState st{};
        if (hidGetMouseStates(&st, 1) > 0) {
            if (!g_has_prev_mouse) {
                g_prev_mouse = st;
                g_has_prev_mouse = true;
            }
            out_mouse.active = (st.attributes & HidMouseAttribute_IsConnected) != 0;
            out_mouse.dx = st.delta_x;
            out_mouse.dy = st.delta_y;
            out_mouse.wheel_x = st.wheel_delta_x * 120;
            out_mouse.wheel_y = st.wheel_delta_y * 120;
            uint32_t b = 0;
            if (st.buttons & HidMouseButton_Left)    b |= (1u << 0);
            if (st.buttons & HidMouseButton_Right)   b |= (1u << 1);
            if (st.buttons & HidMouseButton_Middle)  b |= (1u << 2);
            if (st.buttons & HidMouseButton_Back)    b |= (1u << 3);
            if (st.buttons & HidMouseButton_Forward) b |= (1u << 4);
            out_mouse.buttons = b;
            g_prev_mouse = st;
        }
    }

    if (g_kbd_ok) {
        HidKeyboardState st{};
        if (hidGetKeyboardStates(&st, 1) > 0) {
            if (!g_has_prev_kbd) {
                g_prev_kbd = st;
                g_has_prev_kbd = true;
                return;
            }
            uint8_t mods = convert_hid_modifiers(st.modifiers);
            for (int k = 4; k < 232; ++k) {
                bool now_down = (st.keys[k / 64] & (1ULL << (k % 64))) != 0;
                bool was_down = (g_prev_kbd.keys[k / 64] & (1ULL << (k % 64))) != 0;
                if (now_down != was_down) {
                    out_keys.push_back({static_cast<uint16_t>(k), now_down, mods});
                }
            }
            g_prev_kbd = st;
        }
    }
}

uint64_t now_us() {
    auto tp = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(tp).count());
}

bool get_shared_font(const void*& out_ptr, size_t& out_size) {
    if (!g_has_shared_font || !g_shared_font.address || g_shared_font.size == 0) {
        return false;
    }
    out_ptr = g_shared_font.address;
    out_size = g_shared_font.size;
    return true;
}

}  // namespace platform

#endif  // __SWITCH__
