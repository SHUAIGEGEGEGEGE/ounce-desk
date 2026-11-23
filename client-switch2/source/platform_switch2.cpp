#ifdef __SWITCH__

#include "platform.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/stat.h>
#include <switch.h>

namespace platform {

namespace {
bool g_socket_ok = false;
bool g_nifm_ok = false;
bool g_pl_ok = false;
bool g_kbd_ok = false;
bool g_mouse_ok = false;
bool g_nmouse_ok = false;
PlFontData g_shared_font{};
bool g_has_shared_font = false;
std::mutex g_log_mu;

HidKeyboardState g_prev_kbd{};
HidMouseState g_prev_mouse{};
bool g_has_prev_kbd = false;
bool g_has_prev_mouse = false;

#ifdef __SWITCH2__
struct NmouseTrack {
    u64 handle = 0;
    u64 sampling_number = 0;
    u32 buttons = 0;
};
NmouseTrack g_nm_tracks[16]{};
int g_nm_track_count = 0;
bool g_jcmouse_engaged = false;
u64 g_jcmouse_handle = 0;
uint64_t g_jcmouse_last_move_us = 0;
#endif
}  // namespace

void log_line(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_log_mu);
    FILE* f = std::fopen("sdmc:/switch/OunceDesk/ouncedesk.log", "a");
    if (f) {
        uint64_t ms = now_us() / 1000ULL;
        std::fprintf(f, "[%llu ms] %s\n", static_cast<unsigned long long>(ms), msg.c_str());
        std::fclose(f);
    }
}

bool init() {
    mkdir("sdmc:/switch", 0755);
    mkdir("sdmc:/switch/OunceDesk", 0755);

    if (R_SUCCEEDED(socketInitializeDefault())) {
        g_socket_ok = true;
    }
    if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User))) {
        g_nifm_ok = true;
    }
    if (R_SUCCEEDED(plInitialize(PlServiceType_User))) {
        g_pl_ok = true;
        // Prefer Simplified Chinese system font so full Chinese UI renders natively on Switch 2!
        if (R_SUCCEEDED(plGetSharedFontByType(&g_shared_font, PlSharedFontType_ChineseSimplified)) &&
            g_shared_font.address && g_shared_font.size > 0) {
            g_has_shared_font = true;
        } else if (R_SUCCEEDED(plGetSharedFontByType(&g_shared_font, PlSharedFontType_Standard)) &&
                   g_shared_font.address && g_shared_font.size > 0) {
            g_has_shared_font = true;
        }
    }

    hidSetTouchScreenOutputRanges(1920, 1080);
    hidInitializeTouchScreen();
    hidInitializeKeyboard();
    g_kbd_ok = true;
    hidInitializeMouse();
    g_mouse_ok = true;

#ifdef __SWITCH2__
    if (R_SUCCEEDED(hidInitializeNmouse(nullptr, 0, 1920, 1080, HidNmousePointerType_Default))) {
        g_nmouse_ok = true;
        // Right Joy-Con 2 mouse button layout (held side-down on desk like a real optical mouse):
        //   selectors[0] (0x80  = R, index finger)     -> 1 (HidMouseButton_Left  = 左键点击/拖拽)
        //   selectors[1] (0x200 = ZR, middle finger)   -> 2 (HidMouseButton_Right = 右键菜单)
        //   selectors[6] (0x20  = StickR press, thumb) -> 3 (HidMouseButton_Middle = 中键点击)
        HidNmouseButtonConfigJoyRightJcms rcfg{};
        rcfg.selectors[0] = 1;
        rcfg.selectors[1] = 2;
        rcfg.selectors[6] = 3;
        hidSetNmouseButtonConfigJoyRightJcms(&rcfg);

        // Left Joy-Con 2 mouse button layout (L = Left Click, ZL = Right Click, StickL = Middle Click):
        HidNmouseButtonConfigJoyLeftJcms lcfg{};
        lcfg.selectors[0] = 1;
        lcfg.selectors[1] = 2;
        lcfg.selectors[6] = 3;
        hidSetNmouseButtonConfigJoyLeftJcms(&lcfg);
    }
#endif

    log_line("=== OunceDesk started (Switch 2 kaiHEN, shared_font=" + std::string(g_has_shared_font ? "yes" : "no") +
             ", joycon_nmouse=" + std::string(g_nmouse_ok ? "yes" : "no") + ") ===");
    return g_socket_ok;
}

void shutdown() {
    log_line("=== OunceDesk shutdown ===");
#ifdef __SWITCH2__
    if (g_nmouse_ok) {
        hidFinalizeNmouse();
        g_nmouse_ok = false;
    }
#endif
    if (g_pl_ok) {
        plExit();
        g_pl_ok = false;
    }
    if (g_nifm_ok) {
        nifmExit();
        g_nifm_ok = false;
    }
    if (g_socket_ok) {
        socketExit();
        g_socket_ok = false;
    }
}

bool is_switch_hardware() { return true; }
bool uses_native_touch() { return true; }

std::string get_config_dir() { return "sdmc:/switch/OunceDesk"; }

uint32_t get_local_ipv4_host_order() {
    if (!g_nifm_ok) return 0;
    u32 ip_net = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ip_net)) && ip_net != 0) {
        return ntohl(ip_net);
    }
    return 0;
}

bool show_system_keyboard(const std::string& header, const std::string& initial,
                          std::string& out_text, bool numeric_only, bool password_mode) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) {
        return false;
    }
    if (password_mode) {
        swkbdConfigMakePresetPassword(&kbd);
        swkbdConfigSetStringLenMax(&kbd, 128);
    } else {
        swkbdConfigMakePresetDefault(&kbd);
        if (numeric_only) {
            swkbdConfigSetType(&kbd, SwkbdType_NumPad);
            swkbdConfigSetStringLenMax(&kbd, 16);
        } else {
            // Enable ALL language keyboards (including Simplified/Traditional Chinese Pinyin) + predictive dictionary!
            swkbdConfigSetType(&kbd, SwkbdType_All);
            swkbdConfigSetDicFlag(&kbd, 1);
            swkbdConfigSetReturnButtonFlag(&kbd, 0);
            swkbdConfigSetStringLenMax(&kbd, 256);
        }
    }
    swkbdConfigSetStringLenMin(&kbd, 0);
    swkbdConfigSetInitialCursorPos(&kbd, 1);
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

void poll_native_touch(std::vector<NativeTouchPoint>& out_points) {
    out_points.clear();
    HidTouchScreenState st{};
    if (hidGetTouchScreenStates(&st, 1) > 0 && st.count > 0) {
        int cnt = std::min<int>(st.count, 16);
        out_points.reserve(static_cast<size_t>(cnt));
        for (int i = 0; i < cnt; ++i) {
            const auto& t = st.touches[i];
            float x = std::max(0.0f, std::min(1920.0f, static_cast<float>(t.x)));
            float y = std::max(0.0f, std::min(1080.0f, static_cast<float>(t.y)));
            out_points.push_back({static_cast<uint32_t>(t.finger_id), x, y});
        }
    }
}

static uint8_t convert_hid_modifiers(u64 mods) {
    uint8_t out = 0;
    if (mods & HidKeyboardModifier_Control)  out |= (1 << 0);
    if (mods & HidKeyboardModifier_Shift)    out |= (1 << 1);
    if (mods & HidKeyboardModifier_LeftAlt)  out |= (1 << 2);
    if (mods & HidKeyboardModifier_Gui)      out |= (1 << 3);
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
            bool usb_conn = (st.attributes & HidMouseAttribute_IsConnected) != 0;
            if (usb_conn) {
                out_mouse.active = true;
                if (st.sampling_number != g_prev_mouse.sampling_number) {
                    out_mouse.dx += st.delta_x;
                    out_mouse.dy += st.delta_y;
                    out_mouse.wheel_x += st.wheel_delta_x * 120;
                    out_mouse.wheel_y += st.wheel_delta_y * 120;
                }
                uint32_t b = 0;
                if (st.buttons & HidMouseButton_Left)    b |= (1u << 0);
                if (st.buttons & HidMouseButton_Right)   b |= (1u << 1);
                if (st.buttons & HidMouseButton_Middle)  b |= (1u << 2);
                if (st.buttons & HidMouseButton_Back)    b |= (1u << 3);
                if (st.buttons & HidMouseButton_Forward) b |= (1u << 4);
                out_mouse.buttons |= b;
            }
            g_prev_mouse = st;
        }
    }

#ifdef __SWITCH2__
    if (g_nmouse_ok) {
        HidNmouseHandle handles[16]{};
        s32 hcount = 0;
        if (R_SUCCEEDED(hidGetAllNmouseHandles(handles, 16, &hcount)) && hcount > 0) {
            NmouseTrack next_tracks[16]{};
            int next_count = 0;
            uint64_t now = now_us();
            bool active_handle_still_present = false;

            // Scan detached Joy-Con 2 mouse sensors (Right Joy-Con prioritized if both move simultaneously)
            for (s32 pass = 0; pass < 2; ++pass) {
                for (s32 i = 0; i < hcount && next_count < 16; ++i) {
                    u32 tset = 0;
                    if (R_FAILED(hidGetNmouseTypeSet(handles[i], &tset))) continue;
                    bool is_right = (tset & HidNmouseType_JoyRight) != 0;
                    if ((pass == 0 && !is_right) || (pass == 1 && is_right)) continue;

                    HidNpadIdType npad_id = HidNpadIdType_Handheld;
                    if (R_SUCCEEDED(hidGetNmouseNpadIdType(handles[i], &npad_id)) &&
                        npad_id == HidNpadIdType_Handheld) {
                        // Attached to console rails in handheld mode -> not sliding on desk as a mouse
                        continue;
                    }

                    hidEnableNmousePointer(handles[i], true);

                    const NmouseTrack* prev_t = nullptr;
                    for (int k = 0; k < g_nm_track_count; ++k) {
                        if (g_nm_tracks[k].handle == handles[i].handle) {
                            prev_t = &g_nm_tracks[k];
                            break;
                        }
                    }

                    HidNmouseStateWithDelta states[16]{};
                    size_t total = 0;
                    if (R_FAILED(hidGetNmouseStatesWithDelta(handles[i], states, 16, &total)) || total == 0) {
                        continue;
                    }

                    s32 s_dx = 0, s_dy = 0, s_wy = 0;
                    bool sensor_tracked = false;
                    if (prev_t) {
                        for (size_t s = total; s-- > 0;) {
                            const auto& st = states[s];
                            if (st.sampling_number <= prev_t->sampling_number) continue;
                            s_dx += st.delta_x;
                            s_dy += st.delta_y;
                            s_wy += st.wheel_delta_y;
                            if ((st.flags & 2u) != 0 && (st.delta_x != 0 || st.delta_y != 0)) {
                                sensor_tracked = true;
                            }
                        }
                    }

                    NmouseTrack cur_t{handles[i].handle, states[0].sampling_number, states[0].buttons};
                    next_tracks[next_count++] = cur_t;

                    if (sensor_tracked) {
                        if (!g_jcmouse_engaged || g_jcmouse_handle != handles[i].handle) {
                            log_line(std::string("[HID] Joy-Con 2 optical mouse engaged (") +
                                     (is_right ? "Right Joy-Con" : "Left Joy-Con") + ")");
                        }
                        g_jcmouse_engaged = true;
                        g_jcmouse_handle = handles[i].handle;
                        g_jcmouse_last_move_us = now;
                    }

                    if (g_jcmouse_engaged && g_jcmouse_handle == handles[i].handle) {
                        active_handle_still_present = true;
                        bool sensor_on_surface = (states[0].flags & 2u) != 0;
                        if (sensor_on_surface || states[0].buttons != 0 || s_wy != 0) {
                            g_jcmouse_last_move_us = now;
                        }
                        // Auto-disengage 2.5s after lifting Joy-Con off the desk back into handheld/air grip
                        if (!sensor_on_surface && states[0].buttons == 0 &&
                            now - g_jcmouse_last_move_us > 2500000ULL) {
                            g_jcmouse_engaged = false;
                        } else {
                            out_mouse.active = true;
                            out_mouse.joycon_mouse = true;
                            out_mouse.dx += s_dx;
                            out_mouse.dy += s_dy;
                            out_mouse.wheel_y += s_wy * 90;
                            if (states[0].buttons & HidMouseButton_Left)   out_mouse.buttons |= (1u << 0);
                            if (states[0].buttons & HidMouseButton_Right)  out_mouse.buttons |= (1u << 1);
                            if (states[0].buttons & HidMouseButton_Middle) out_mouse.buttons |= (1u << 2);
                        }
                    }
                }
            }
            std::memcpy(g_nm_tracks, next_tracks, sizeof(NmouseTrack) * static_cast<size_t>(next_count));
            g_nm_track_count = next_count;
            if (!active_handle_still_present) {
                g_jcmouse_engaged = false;
            }
        } else {
            g_nm_track_count = 0;
            g_jcmouse_engaged = false;
        }
    }
#endif

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
