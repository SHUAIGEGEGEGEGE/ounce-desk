#include "backend.hpp"
#include "input_mapper.hpp"
#include "keymap.hpp"
#include "platform.hpp"
#include "profiles.hpp"
#include "rdp_backend.hpp"
#include "ui_renderer.hpp"
#include "vnc_backend.hpp"
#include "wg_tunnel.hpp"

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include <SDL2/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

enum class AppMode {
    BROWSER,
    FORM,
    CONNECTING,
    STREAMING,
};

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    platform::init();
    wg::install_connect_hook();

    const std::string wg_dir = platform::get_config_dir() + "/wg";
#ifdef _WIN32
    _mkdir(wg_dir.c_str());
#else
    mkdir(wg_dir.c_str(), 0755);
#endif

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        platform::shutdown();
        return 1;
    }

    constexpr int LOGICAL_W = 1920;
    constexpr int LOGICAL_H = 1080;
    Uint32 win_flags = platform::is_switch_hardware()
                           ? (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN)
                           : (SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);

    SDL_Window* window = SDL_CreateWindow(
        "OunceDesk - Switch 2 Windows RDP (3389) & macOS (5900)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        LOGICAL_W, LOGICAL_H, win_flags);
    if (!window) {
        SDL_Quit();
        platform::shutdown();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    // Lock logical space to exact 1920x1080 so UI + libounce hidSetTouchScreenOutputRanges(1920,1080) map 1:1 with zero drift!
    SDL_RenderSetLogicalSize(renderer, LOGICAL_W, LOGICAL_H);

    SDL_GameController* pad = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            pad = SDL_GameControllerOpen(i);
            break;
        }
    }

    UIRenderer ui;
    ui.init(renderer);

    ProfileManager profiles;
    const std::string profiles_path = platform::get_config_dir() + "/profiles.txt";
    profiles.load(profiles_path);
    profiles.start_lan_scan();

    InputMapper input;
    std::unique_ptr<RemoteBackend> backend;
    SDL_Texture* fb_texture = nullptr;
    int fb_w = 0, fb_h = 0;

    AppMode mode = AppMode::BROWSER;
    ConnectionProfile draft;
    ConnectionProfile active_profile;
    int selected_col = 0;  // 0 = Windows 3389, 1 = macOS 5900
    int selected_row = 0;
    int form_focus = 1;
    int home_menu_focus = 0;
    int overlay_focus = 0;
    int osk_focus = 0;
    uint8_t osk_sticky_mods = 0;
    bool show_home_menu = false;
    bool show_overlay = false;
    bool show_osk = false;
    bool show_hud = true;

    std::string status_banner;
    std::string form_error;
    std::vector<UIButton> active_buttons;
    std::vector<OSKKey> active_osk;
    SDL_Rect menu_pill{16, 16, 148, 44};

    // Previous frame native Switch 2 touch points for exact DOWN/MOVE/UP synthesis
    std::map<uint32_t, platform::NativeTouchPoint> prev_native_touches;

    // Auto Switch 2 keyboard: tap on a remote text box (cursor turns into I-beam) -> open swkbd;
    // tap on blank area (normal arrow cursor) -> nothing / cancel.
    bool auto_ime = true;
    uint64_t ime_check_at_us = 0;
    uint64_t tap_down_us = 0;
    float tap_x0 = 0.0f, tap_y0 = 0.0f;
    float tap_max_dist = 0.0f;
    bool tap_tracking = false;
    float last_tap_x = 960.0f, last_tap_y = 540.0f;
    std::string remote_typed_buf;
    float remote_typed_nx = -10.0f;
    float remote_typed_ny = -10.0f;

    // Saved-device manager (home menu) + WireGuard manager + sensitivity slider + macOS keyboard bubble state
    bool show_manage = false;
    bool show_wg = false;
    bool touch_block = false;  // after a modal keyboard: ignore touch until all fingers lifted
    int manage_focus = 0;
    int wg_focus = 0;
    std::string saved_wg_name;
    std::vector<std::string> wg_configs;

    auto reload_wg_configs = [&]() {
        wg_configs = wg::list_configs(wg_dir);
        auto root_cfgs = wg::list_configs(platform::get_config_dir());
        for (const auto& c : root_cfgs) {
            if (std::find(wg_configs.begin(), wg_configs.end(), c) == wg_configs.end()) {
                wg_configs.push_back(c);
            }
        }
    };
    auto start_wg_by_name = [&](const std::string& name) -> bool {
        wg::Config cfg;
        std::string err;
        std::string p1 = wg_dir + "/" + name + ".conf";
        std::string p2 = platform::get_config_dir() + "/" + name + ".conf";
        if (!wg::load_config_file(p1, cfg, err) && !wg::load_config_file(p2, cfg, err)) {
            status_banner = "WireGuard 配置加载失败: " + err;
            return false;
        }
        wg::Tunnel::instance().start(cfg);
        saved_wg_name = name;
        status_banner = "正在启动 WireGuard 隧道: " + name;
        return true;
    };
    reload_wg_configs();

    bool slider_dragging = false;

    // Persistent settings: sdmc:/switch/OunceDesk/settings.txt
    const std::string settings_path = platform::get_config_dir() + "/settings.txt";
    auto save_settings = [&]() {
        FILE* f = std::fopen(settings_path.c_str(), "w");
        if (!f) return;
        std::fprintf(f, "sensitivity=%.2f\nauto_ime=%d\ntouch_mode=%d\nwg_config=%s\n",
                     input.sensitivity(), auto_ime ? 1 : 0,
                     input.touch_mode() == TouchMode::MOUSE ? 1 : 0,
                     saved_wg_name.c_str());
        std::fclose(f);
    };
    {
        FILE* f = std::fopen(settings_path.c_str(), "r");
        if (f) {
            char line[256];
            while (std::fgets(line, sizeof(line), f)) {
                float fv = 0.0f;
                int iv = 0;
                char sv[128]{};
                if (std::sscanf(line, "sensitivity=%f", &fv) == 1) input.set_sensitivity(fv);
                else if (std::sscanf(line, "auto_ime=%d", &iv) == 1) auto_ime = (iv != 0);
                else if (std::sscanf(line, "touch_mode=%d", &iv) == 1) input.set_touch_mode(iv ? TouchMode::MOUSE : TouchMode::TOUCH);
                else if (std::sscanf(line, "wg_config=%127s", sv) == 1) saved_wg_name = sv;
            }
            std::fclose(f);
        }
        if (!saved_wg_name.empty()) {
            start_wg_by_name(saved_wg_name);
        }
    }

    // Returns true if the button id was a sensitivity slider control
    auto apply_sens_control = [&](const UIButton& btn, int tap_x) -> bool {
        if (btn.id == "sens:dec") input.set_sensitivity(input.sensitivity() - 0.1f);
        else if (btn.id == "sens:inc") input.set_sensitivity(input.sensitivity() + 0.1f);
        else if (btn.id == "sens:track") {
            input.set_sensitivity(UIRenderer::sens_from_track_x(btn.rect, tap_x));
            slider_dragging = true;
        } else {
            return false;
        }
        save_settings();
        return true;
    };
    auto drag_slider_to = [&](int x) {
        for (const auto& b : active_buttons) {
            if (b.id == "sens:track") {
                input.set_sensitivity(UIRenderer::sens_from_track_x(b.rect, x));
                break;
            }
        }
    };

    bool running = true;

    auto form_field_count = [&]() -> int {
        return (draft.protocol == ProtocolType::VNC_MACOS) ? 6 : 7;
    };
    auto form_total_items = [&]() -> int {
        return form_field_count() + 3;
    };

    auto open_new_form = [&](ProtocolType proto) {
        draft = {};
        draft.protocol = proto;
        draft.port = (proto == ProtocolType::VNC_MACOS) ? 5900 : 3389;
        draft.host = "";
        draft.req_width = 1920;
        draft.req_height = 1080;
        form_focus = 2;  // Focus Server Address by default (Name at index 1 is optional)
        form_error.clear();
        show_home_menu = false;
        mode = AppMode::FORM;
    };

    auto start_connection = [&](const ConnectionProfile& p, bool also_save) {
        if (p.host.empty()) {
            form_error = "请输入服务器地址";
            return;
        }
        active_profile = p;
        if (active_profile.name.empty()) {
            active_profile.name = active_profile.host;
        }
        remote_typed_buf.clear();
        remote_typed_nx = -10.0f;
        remote_typed_ny = -10.0f;
        if (also_save) {
            profiles.add_or_update(active_profile, profiles_path);
        }
        if (backend) {
            backend->disconnect();
            backend.reset();
        }
        if (fb_texture) {
            SDL_DestroyTexture(fb_texture);
            fb_texture = nullptr;
        }
        if (p.protocol == ProtocolType::VNC_MACOS) {
            backend = std::make_unique<VncBackend>();
        } else {
            backend = std::make_unique<RdpBackend>();
        }
        backend->connect_async(active_profile);
        mode = AppMode::CONNECTING;
        show_home_menu = false;
        show_overlay = false;
        show_osk = false;
        form_error.clear();
    };

    auto edit_form_field = [&](int field_idx) {
        if (field_idx == 0) {
            draft.protocol = (draft.protocol == ProtocolType::RDP_WINDOWS)
                                 ? ProtocolType::VNC_MACOS
                                 : ProtocolType::RDP_WINDOWS;
            draft.port = (draft.protocol == ProtocolType::VNC_MACOS) ? 5900 : 3389;
        } else if (field_idx == 1) {
            std::string v;
            if (platform::show_system_keyboard("请输入服务器名称 (选填)", draft.name, v, false, false)) {
                draft.name = v;
            }
        } else if (field_idx == 2) {
            std::string v;
            if (platform::show_system_keyboard("请输入服务器地址 (IP 或域名)", draft.host, v, false, false)) {
                draft.host = v;
            }
        } else if (field_idx == 3) {
            std::string v;
            if (platform::show_system_keyboard("请输入端口号 (Windows 3389 / Mac 5900)", std::to_string(draft.port), v, true, false)) {
                if (v.empty()) {
                    draft.port = (draft.protocol == ProtocolType::VNC_MACOS) ? 5900 : 3389;
                } else {
                    int p = std::atoi(v.c_str());
                    if (p > 0 && p <= 65535) draft.port = static_cast<uint16_t>(p);
                }
            }
        } else if (field_idx == 4) {
            std::string v;
            std::string hdr = (draft.protocol == ProtocolType::VNC_MACOS)
                                  ? "请输入用户名 (若仅用 VNC 密码可留空)"
                                  : "请输入用户名";
            if (platform::show_system_keyboard(hdr, draft.username, v, false, false)) {
                draft.username = v;
            }
        } else if (field_idx == 5) {
            std::string v;
            if (platform::show_system_keyboard("请输入登录密码", draft.password, v, false, true)) {
                draft.password = v;
            }
        } else if (field_idx == 6 && draft.protocol == ProtocolType::RDP_WINDOWS) {
            if (draft.req_width == 1920) { draft.req_width = 1280; draft.req_height = 720; }
            else { draft.req_width = 1920; draft.req_height = 1080; }
        }
    };

    uint64_t last_us = platform::now_us();

    auto utf8_codepoint_len = [](const std::string& s) -> size_t {
        size_t pos = 0, cnt = 0;
        while (pos < s.size()) {
            if (keymap::next_utf8_codepoint(s.data(), s.size(), pos) == 0) break;
            ++cnt;
        }
        return cnt;
    };

    auto current_remote_norm = [&](float& out_nx, float& out_ny) {
        const auto& vp = input.viewport();
        out_nx = vp.cursor_x;
        out_ny = vp.cursor_y;
        if (input.touch_mode() == TouchMode::TOUCH && vp.dst_rect.w > 0 && vp.dst_rect.h > 0) {
            out_nx = (last_tap_x - static_cast<float>(vp.dst_rect.x)) / static_cast<float>(vp.dst_rect.w);
            out_ny = (last_tap_y - static_cast<float>(vp.dst_rect.y)) / static_cast<float>(vp.dst_rect.h);
        }
    };

    auto open_streaming_swkbd = [&]() {
        if (!backend) return;
        float nx = 0.5f, ny = 0.5f;
        current_remote_norm(nx, ny);
        float dx = nx - remote_typed_nx;
        float dy = ny - remote_typed_ny;
        // Same input box if within ~12% horizontal and ~5% vertical of where we last typed
        bool same_field = (!remote_typed_buf.empty() && (dx * dx < 0.12f * 0.12f) && (dy * dy < 0.05f * 0.05f));

        // Prepend 24 Zero-Width Spaces (U+200B, 3 bytes each: E2 80 8B) BEFORE the remembered text:
        //  - Visually 0-width invisible in Switch 2's nintendo_udsg system font!
        //  - Gives the user 24 Backspace presses even when the remote input box already had characters not typed by us!
        constexpr const char* ZWS = "\xE2\x80\x8B";
        constexpr size_t ANCHOR_COUNT = 24;
        std::string seed;
        seed.reserve(ANCHOR_COUNT * 3 + remote_typed_buf.size());
        for (size_t i = 0; i < ANCHOR_COUNT; ++i) seed.append(ZWS);
        if (same_field) seed.append(remote_typed_buf);

        std::string typed;
        if (platform::show_system_keyboard("输入或删除文字 (按退格可删远程已有内容)", seed, typed, false, false)) {
            // Count how many leading U+200B anchors remain
            size_t anchors_left = 0;
            size_t scan = 0;
            while (scan + 3 <= typed.size() &&
                   static_cast<uint8_t>(typed[scan]) == 0xE2 &&
                   static_cast<uint8_t>(typed[scan + 1]) == 0x80 &&
                   static_cast<uint8_t>(typed[scan + 2]) == 0x8B) {
                ++anchors_left;
                scan += 3;
            }
            // Strip ALL U+200B anchors anywhere in the returned text
            std::string clean;
            clean.reserve(typed.size());
            for (size_t i = 0; i < typed.size();) {
                if (i + 3 <= typed.size() &&
                    static_cast<uint8_t>(typed[i]) == 0xE2 &&
                    static_cast<uint8_t>(typed[i + 1]) == 0x80 &&
                    static_cast<uint8_t>(typed[i + 2]) == 0x8B) {
                    i += 3;
                } else {
                    clean.push_back(typed[i++]);
                }
            }

            size_t old_chars = same_field ? utf8_codepoint_len(remote_typed_buf) : 0;
            size_t extra_bs = (anchors_left < ANCHOR_COUNT) ? (ANCHOR_COUNT - anchors_left) : 0;
            size_t total_bs = old_chars + extra_bs;

            for (size_t i = 0; i < total_bs; ++i) {
                backend->send_key(42, true, 0);   // HID 42 = Backspace
                backend->send_key(42, false, 0);
            }
            if (!clean.empty()) {
                backend->send_text_utf8(clean);
            }
            remote_typed_buf = clean;
            remote_typed_nx = nx;
            remote_typed_ny = ny;
        } else if (!platform::is_switch_hardware()) {
            // Only fall back to built-in OSK on Desktop PCs that have no system keyboard applet!
            // On Switch 2, pressing B / Cancel simply closes the native keyboard cleanly.
            show_osk = true;
        }
        prev_native_touches.clear();
        touch_block = true;
        last_us = platform::now_us();
    };

    auto handle_home_menu_action = [&](const std::string& id) {
        if (id == "hm:wg") {
            reload_wg_configs();
            show_wg = true;
            wg_focus = 0;
        } else if (id == "hm:mode") {
            input.toggle_touch_mode();
            save_settings();
        } else if (id == "hm:manage") {
            show_manage = true;
            manage_focus = 0;
        } else if (id == "sens:dec" || id == "sens:inc") {
            input.set_sensitivity(input.sensitivity() + (id == "sens:inc" ? 0.1f : -0.1f));
            save_settings();
        } else if (id == "hm:scan") {
            profiles.start_lan_scan();
            show_home_menu = false;
            status_banner = "正在扫描局域网内的 Windows (3389) 与 macOS (5900) 电脑...";
        } else if (id == "hm:add_rdp") {
            open_new_form(ProtocolType::RDP_WINDOWS);
        } else if (id == "hm:add_mac") {
            open_new_form(ProtocolType::VNC_MACOS);
        } else if (id == "hm:resume") {
            show_home_menu = false;
        } else if (id == "hm:exit") {
            running = false;
        }
    };

    auto handle_overlay_action = [&](const std::string& id) {
        if (!backend) return;
        if (id.rfind("sc:", 0) == 0) {
            input.send_shortcut(*backend, active_profile.protocol, id.substr(3));
            show_overlay = false;
        } else if (id == "act:touch_mode") {
            input.toggle_touch_mode();
            save_settings();
        } else if (id == "sens:dec" || id == "sens:inc") {
            input.set_sensitivity(input.sensitivity() + (id == "sens:inc" ? 0.1f : -0.1f));
            save_settings();
        } else if (id == "act:auto_ime") {
            auto_ime = !auto_ime;
            save_settings();
        } else if (id == "act:refresh") {
            backend->request_full_refresh();
            show_overlay = false;
        } else if (id == "act:hud") {
            show_hud = !show_hud;
        } else if (id == "act:reset_zoom") {
            input.reset_zoom();
            show_overlay = false;
        } else if (id == "act:osk") {
            show_overlay = false;
            show_osk = true;
        } else if (id == "act:swkbd") {
            show_overlay = false;
            open_streaming_swkbd();
        } else if (id == "act:resume") {
            show_overlay = false;
        } else if (id == "act:disconnect") {
            backend->disconnect();
            backend.reset();
            show_overlay = false;
            show_osk = false;
            mode = AppMode::BROWSER;
            status_banner = "已断开远程连接。";
        } else if (id == "act:exit_app") {
            running = false;
        }
    };

    auto trigger_osk_key = [&](const OSKKey& k) {
        if (!backend) return;
        if (k.hid == 0) { show_osk = false; return; }
        if (k.is_modifier) { osk_sticky_mods ^= k.mod_bit; return; }

        if (osk_sticky_mods & RD_MOD_LCTRL)  backend->send_key(224, true, osk_sticky_mods);
        if (osk_sticky_mods & RD_MOD_LSHIFT) backend->send_key(225, true, osk_sticky_mods);
        if (osk_sticky_mods & RD_MOD_LALT)   backend->send_key(226, true, osk_sticky_mods);
        if (osk_sticky_mods & RD_MOD_LMETA)  backend->send_key(227, true, osk_sticky_mods);

        backend->send_key(k.hid, true, osk_sticky_mods);
        backend->send_key(k.hid, false, osk_sticky_mods);

        if (osk_sticky_mods & RD_MOD_LMETA)  backend->send_key(227, false, 0);
        if (osk_sticky_mods & RD_MOD_LALT)   backend->send_key(226, false, 0);
        if (osk_sticky_mods & RD_MOD_LSHIFT) backend->send_key(225, false, 0);
        if (osk_sticky_mods & RD_MOD_LCTRL)  backend->send_key(224, false, 0);
        osk_sticky_mods = 0;
    };

    while (running) {
        uint64_t now = platform::now_us();
        float dt = std::min(0.1f, static_cast<float>(now - last_us) / 1e6f);
        last_us = now;

        auto win_profs = profiles.windows_profiles();
        auto mac_profs = profiles.macos_profiles();
        const auto& cur_list = (selected_col == 0) ? win_profs : mac_profs;
        if (!cur_list.empty()) {
            selected_row = std::max(0, std::min(selected_row, static_cast<int>(cur_list.size()) - 1));
        } else {
            selected_row = 0;
        }

        // Sync backend connection transitions
        if (backend && (mode == AppMode::CONNECTING || mode == AppMode::STREAMING)) {
            auto st = backend->state();
            if (st == BackendState::STREAMING && mode == AppMode::CONNECTING) {
                mode = AppMode::STREAMING;
                status_banner.clear();
            } else if (st == BackendState::ERROR_STATE) {
                status_banner = backend->error_message();
                backend->disconnect();
                backend.reset();
                show_overlay = false;
                show_osk = false;
                mode = AppMode::BROWSER;
            }
        }

        // Unified UI tap dispatcher (works identically for Switch 2 native touch AND Desktop mouse)
        auto dispatch_ui_tap = [&](int tap_x, int tap_y) {
            if (mode == AppMode::BROWSER) {
                if (show_wg) {
                    for (const auto& btn : active_buttons) {
                        if (!UIRenderer::hit_test(btn.rect, tap_x, tap_y)) continue;
                        if (btn.id == "wg:back") {
                            show_wg = false;
                        } else if (btn.id == "wg:reload") {
                            reload_wg_configs();
                        } else if (btn.id == "wg:off") {
                            wg::Tunnel::instance().stop();
                            saved_wg_name.clear();
                            save_settings();
                            status_banner = "WireGuard 隧道已关闭";
                        } else if (btn.id.rfind("wg:use:", 0) == 0) {
                            if (start_wg_by_name(btn.id.substr(7))) save_settings();
                        }
                        break;
                    }
                    return;
                }
                if (show_manage) {
                    for (const auto& btn : active_buttons) {
                        if (!UIRenderer::hit_test(btn.rect, tap_x, tap_y)) continue;
                        if (btn.id == "dm:back") {
                            show_manage = false;
                        } else if (btn.id.rfind("dm:del:", 0) == 0) {
                            size_t idx = static_cast<size_t>(std::atoi(btn.id.c_str() + 7));
                            auto saved = profiles.saved_profiles();
                            if (idx < saved.size()) {
                                profiles.remove_saved(saved[idx], profiles_path);
                                status_banner = "已删除设备: " + saved[idx].name;
                            }
                        }
                        break;
                    }
                    return;
                }
                if (show_home_menu) {
                    for (const auto& btn : active_buttons) {
                        if (UIRenderer::hit_test(btn.rect, tap_x, tap_y)) {
                            if (!apply_sens_control(btn, tap_x)) handle_home_menu_action(btn.id);
                            break;
                        }
                    }
                    return;
                }
                for (const auto& btn : active_buttons) {
                    if (!UIRenderer::hit_test(btn.rect, tap_x, tap_y)) continue;
                    if (btn.id == "open_wg") {
                        reload_wg_configs();
                        show_wg = true;
                        wg_focus = 0;
                    } else if (btn.id == "add_rdp") open_new_form(ProtocolType::RDP_WINDOWS);
                    else if (btn.id == "add_mac") open_new_form(ProtocolType::VNC_MACOS);
                    else if (btn.id == "home_mode") { input.toggle_touch_mode(); save_settings(); }
                    else if (btn.id == "scan_lan") {
                        profiles.start_lan_scan();
                        status_banner = "正在扫描局域网内的 Windows (3389) 与 macOS (5900) 电脑...";
                    } else if (btn.id == "open_menu") {
                        show_home_menu = true;
                        home_menu_focus = 0;
                    } else if (btn.id.rfind("win:", 0) == 0) {
                        size_t idx = static_cast<size_t>(std::atoi(btn.id.c_str() + 4));
                        if (idx < win_profs.size()) {
                            draft = win_profs[idx];
                            int nf = (draft.protocol == ProtocolType::VNC_MACOS) ? 6 : 7;
                            form_focus = draft.username.empty() ? 4 : (draft.password.empty() ? 5 : (nf + 2));
                            form_error.clear();
                            mode = AppMode::FORM;
                        }
                    } else if (btn.id.rfind("mac:", 0) == 0) {
                        size_t idx = static_cast<size_t>(std::atoi(btn.id.c_str() + 4));
                        if (idx < mac_profs.size()) {
                            draft = mac_profs[idx];
                            int nf = (draft.protocol == ProtocolType::VNC_MACOS) ? 6 : 7;
                            form_focus = draft.username.empty() ? 4 : (draft.password.empty() ? 5 : (nf + 2));
                            form_error.clear();
                            mode = AppMode::FORM;
                        }
                    }
                    break;
                }
            } else if (mode == AppMode::FORM) {
                for (const auto& btn : active_buttons) {
                    if (!UIRenderer::hit_test(btn.rect, tap_x, tap_y)) continue;
                    if (btn.id == "f:proto") { form_focus = 0; edit_form_field(0); }
                    else if (btn.id == "f:name") { form_focus = 1; edit_form_field(1); }
                    else if (btn.id == "f:host") { form_focus = 2; edit_form_field(2); }
                    else if (btn.id == "f:port") { form_focus = 3; edit_form_field(3); }
                    else if (btn.id == "f:user") { form_focus = 4; edit_form_field(4); }
                    else if (btn.id == "f:pass") { form_focus = 5; edit_form_field(5); }
                    else if (btn.id == "f:res")  { form_focus = 6; edit_form_field(6); }
                    else if (btn.id == "form_cancel") mode = AppMode::BROWSER;
                    else if (btn.id == "form_save") {
                        if (draft.host.empty()) {
                            form_error = "请输入服务器地址";
                        } else {
                            profiles.add_or_update(draft, profiles_path);
                            status_banner = "已保存配置: " + (draft.name.empty() ? draft.host : draft.name);
                            mode = AppMode::BROWSER;
                        }
                    }
                    else if (btn.id == "form_connect") start_connection(draft, false);
                    break;
                }
            } else if (mode == AppMode::STREAMING && backend) {
                if (show_overlay) {
                    for (const auto& btn : active_buttons) {
                        if (UIRenderer::hit_test(btn.rect, tap_x, tap_y)) {
                            if (!apply_sens_control(btn, tap_x)) handle_overlay_action(btn.id);
                            break;
                        }
                    }
                } else if (show_osk) {
                    for (const auto& k : active_osk) {
                        if (UIRenderer::hit_test(k.rect, tap_x, tap_y)) { trigger_osk_key(k); break; }
                    }
                }
            }
        };

        // 1. Poll exact 1:1 1920x1080 native Switch 2 touch states (bypasses switch-sdl2 1280x720 scaling bug!)
        if (platform::uses_native_touch()) {
            std::vector<platform::NativeTouchPoint> cur_pts;
            platform::poll_native_touch(cur_pts);
            std::map<uint32_t, platform::NativeTouchPoint> cur_map;
            for (const auto& pt : cur_pts) cur_map[pt.id] = pt;
            if (touch_block) {
                if (cur_map.empty()) touch_block = false;
                prev_native_touches.clear();
                cur_map.clear();
            }

            // Newly pressed fingers (DOWN)
            for (const auto& kv : cur_map) {
                if (prev_native_touches.find(kv.first) == prev_native_touches.end()) {
                    int tx = static_cast<int>(kv.second.x);
                    int ty = static_cast<int>(kv.second.y);
                    if (mode == AppMode::STREAMING && backend && !show_overlay && !show_osk) {
                        if (cur_map.size() == 1 && prev_native_touches.empty()) {
                            tap_tracking = true;
                            tap_down_us = now;
                            tap_x0 = kv.second.x;
                            tap_y0 = kv.second.y;
                            tap_max_dist = 0.0f;
                        } else {
                            tap_tracking = false;  // multi-finger gesture: never a text-box tap
                        }
                        if (UIRenderer::hit_test(menu_pill, tx, ty)) {
                            tap_tracking = false;
                            show_overlay = true;
                            overlay_focus = 0;
                        } else if (input.on_touch_down(kv.first, kv.second.x, kv.second.y, *backend)) {
                            tap_tracking = false;
                            show_overlay = true;
                            overlay_focus = 0;
                        }
                    } else {
                        dispatch_ui_tap(tx, ty);
                    }
                } else if (mode == AppMode::STREAMING && backend && !show_overlay && !show_osk) {
                    // Existing finger moved (MOVE)
                    if (tap_tracking) {
                        float dx = kv.second.x - tap_x0, dy = kv.second.y - tap_y0;
                        tap_max_dist = std::max(tap_max_dist, dx * dx + dy * dy);
                    }
                    input.on_touch_move(kv.first, kv.second.x, kv.second.y, *backend, LOGICAL_W, LOGICAL_H);
                } else if (slider_dragging && (show_home_menu || show_overlay)) {
                    // Drag the mouse sensitivity slider knob
                    drag_slider_to(static_cast<int>(kv.second.x));
                }
            }
            if (slider_dragging && cur_map.empty()) {
                slider_dragging = false;
                save_settings();
            }
            // Released fingers (UP)
            for (const auto& kv : prev_native_touches) {
                if (cur_map.find(kv.first) == cur_map.end()) {
                    if (mode == AppMode::STREAMING && backend) {
                        input.on_touch_up(kv.first, *backend);
                        if (tap_tracking && cur_map.empty()) {
                            // Short, still, single-finger tap -> check remote cursor shape shortly after click
                            if (now - tap_down_us < 450000ULL && tap_max_dist < 40.0f * 40.0f) {
                                ime_check_at_us = now + 320000ULL;
                                last_tap_x = tap_x0;
                                last_tap_y = tap_y0;
                            }
                            tap_tracking = false;
                        }
                    }
                }
            }
            if (touch_block) cur_map.clear();
            prev_native_touches = std::move(cur_map);
        }

        // 2. Poll SDL events (Gamepad buttons, Desktop mouse/keyboard)
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { running = false; break; }
            if (ev.type == SDL_CONTROLLERDEVICEADDED && !pad) {
                pad = SDL_GameControllerOpen(ev.cdevice.which);
            }

            // Desktop mouse tap -> logical 1920x1080 tap
            bool desktop_tap = (!platform::uses_native_touch() &&
                                ev.type == SDL_MOUSEBUTTONDOWN &&
                                ev.button.button == SDL_BUTTON_LEFT);
            int dtap_x = desktop_tap ? ev.button.x : 0;
            int dtap_y = desktop_tap ? ev.button.y : 0;

            if (mode == AppMode::BROWSER) {
                if (show_wg) {
                    if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                        auto b = ev.cbutton.button;
                        int n = static_cast<int>(active_buttons.size());
                        if (b == SDL_CONTROLLER_BUTTON_B) show_wg = false;
                        else if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN && n > 0) wg_focus = (wg_focus + 1) % n;
                        else if (b == SDL_CONTROLLER_BUTTON_DPAD_UP && n > 0) wg_focus = (wg_focus - 1 + n) % n;
                        else if (b == SDL_CONTROLLER_BUTTON_A && wg_focus < n) {
                            const SDL_Rect& r = active_buttons[wg_focus].rect;
                            dispatch_ui_tap(r.x + r.w / 2, r.y + r.h / 2);
                        }
                    } else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) {
                        show_wg = false;
                    } else if (desktop_tap) {
                        dispatch_ui_tap(dtap_x, dtap_y);
                    }
                    continue;
                }
                if (show_manage) {
                    if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                        auto b = ev.cbutton.button;
                        int n = static_cast<int>(active_buttons.size());
                        if (b == SDL_CONTROLLER_BUTTON_B) show_manage = false;
                        else if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN && n > 0) manage_focus = (manage_focus + 1) % n;
                        else if (b == SDL_CONTROLLER_BUTTON_DPAD_UP && n > 0) manage_focus = (manage_focus - 1 + n) % n;
                        else if (b == SDL_CONTROLLER_BUTTON_A && manage_focus < n) {
                            const SDL_Rect& r = active_buttons[manage_focus].rect;
                            dispatch_ui_tap(r.x + r.w / 2, r.y + r.h / 2);
                            manage_focus = 0;
                        }
                    } else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) {
                        show_manage = false;
                    } else if (desktop_tap) {
                        dispatch_ui_tap(dtap_x, dtap_y);
                    }
                    continue;
                }
                if (show_home_menu) {
                    if (ev.type == SDL_CONTROLLERBUTTONDOWN &&
                        (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT || ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) {
                        input.set_sensitivity(input.sensitivity() + (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT ? 0.1f : -0.1f));
                        save_settings();
                        continue;
                    }
                    if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                        auto b = ev.cbutton.button;
                        if (b == SDL_CONTROLLER_BUTTON_B || b == SDL_CONTROLLER_BUTTON_START) show_home_menu = false;
                        else if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN && !active_buttons.empty())
                            home_menu_focus = (home_menu_focus + 1) % static_cast<int>(active_buttons.size());
                        else if (b == SDL_CONTROLLER_BUTTON_DPAD_UP && !active_buttons.empty())
                            home_menu_focus = (home_menu_focus - 1 + static_cast<int>(active_buttons.size())) % static_cast<int>(active_buttons.size());
                        else if (b == SDL_CONTROLLER_BUTTON_A && home_menu_focus < static_cast<int>(active_buttons.size()))
                            handle_home_menu_action(active_buttons[home_menu_focus].id);
                    } else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) {
                        show_home_menu = false;
                    } else if (desktop_tap) {
                        dispatch_ui_tap(dtap_x, dtap_y);
                    }
                    continue;
                }

                if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                    auto b = ev.cbutton.button;
                    if (b == SDL_CONTROLLER_BUTTON_START) { show_home_menu = true; home_menu_focus = 0; }
                    else if (b == SDL_CONTROLLER_BUTTON_Y) open_new_form(ProtocolType::RDP_WINDOWS);
                    else if (b == SDL_CONTROLLER_BUTTON_X) open_new_form(ProtocolType::VNC_MACOS);
                    else if (b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) { selected_col = 1; selected_row = 0; }
                    else if (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT)  { selected_col = 0; selected_row = 0; }
                    else if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN)  selected_row++;
                    else if (b == SDL_CONTROLLER_BUTTON_DPAD_UP)    selected_row = std::max(0, selected_row - 1);
                    else if (b == SDL_CONTROLLER_BUTTON_BACK && !cur_list.empty()) {
                        profiles.remove_saved(cur_list[selected_row], profiles_path);
                    } else if (b == SDL_CONTROLLER_BUTTON_A && !cur_list.empty()) {
                        draft = cur_list[selected_row];
                        int nf = (draft.protocol == ProtocolType::VNC_MACOS) ? 6 : 7;
                        form_focus = draft.username.empty() ? 4 : (draft.password.empty() ? 5 : (nf + 2));
                        form_error.clear();
                        mode = AppMode::FORM;
                    }
                } else if (ev.type == SDL_KEYDOWN) {
                    if (ev.key.keysym.sym == SDLK_ESCAPE) { show_home_menu = true; home_menu_focus = 0; }
                    else if (ev.key.keysym.sym == SDLK_r) open_new_form(ProtocolType::RDP_WINDOWS);
                    else if (ev.key.keysym.sym == SDLK_m) open_new_form(ProtocolType::VNC_MACOS);
                } else if (desktop_tap) {
                    dispatch_ui_tap(dtap_x, dtap_y);
                }
            } else if (mode == AppMode::FORM) {
                const int nf = form_field_count();
                const int total_items = form_total_items();
                if (form_focus >= total_items) form_focus = total_items - 1;
                if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                    auto b = ev.cbutton.button;
                    if (b == SDL_CONTROLLER_BUTTON_B) mode = AppMode::BROWSER;
                    else if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) form_focus = (form_focus + 1) % total_items;
                    else if (b == SDL_CONTROLLER_BUTTON_DPAD_UP)   form_focus = (form_focus + total_items - 1) % total_items;
                    else if (b == SDL_CONTROLLER_BUTTON_A) {
                        if (form_focus < nf) edit_form_field(form_focus);
                        else if (form_focus == nf + 0) mode = AppMode::BROWSER;
                        else if (form_focus == nf + 1) {
                            if (draft.host.empty()) {
                                form_error = "请输入服务器地址";
                            } else {
                                profiles.add_or_update(draft, profiles_path);
                                status_banner = "已保存配置: " + (draft.name.empty() ? draft.host : draft.name);
                                mode = AppMode::BROWSER;
                            }
                        }
                        else if (form_focus == nf + 2) start_connection(draft, false);
                    }
                } else if (ev.type == SDL_KEYDOWN) {
                    SDL_Keycode sym = ev.key.keysym.sym;
                    if (sym == SDLK_ESCAPE) mode = AppMode::BROWSER;
                    else if (sym == SDLK_TAB) form_focus = (form_focus + 1) % total_items;
                    else if (sym == SDLK_RETURN) {
                        if (form_focus == 0 || (form_focus == 6 && draft.protocol == ProtocolType::RDP_WINDOWS)) edit_form_field(form_focus);
                        else start_connection(draft, false);
                    } else if (sym == SDLK_BACKSPACE) {
                        if (form_focus == 1 && !draft.name.empty()) draft.name.pop_back();
                        else if (form_focus == 2 && !draft.host.empty()) draft.host.pop_back();
                        else if (form_focus == 4 && !draft.username.empty()) draft.username.pop_back();
                        else if (form_focus == 5 && !draft.password.empty()) draft.password.pop_back();
                    }
                } else if (ev.type == SDL_TEXTINPUT) {
                    if (form_focus == 1) draft.name += ev.text.text;
                    else if (form_focus == 2) draft.host += ev.text.text;
                    else if (form_focus == 4) draft.username += ev.text.text;
                    else if (form_focus == 5) draft.password += ev.text.text;
                } else if (desktop_tap) {
                    dispatch_ui_tap(dtap_x, dtap_y);
                }
            } else if (mode == AppMode::CONNECTING) {
                if ((ev.type == SDL_CONTROLLERBUTTONDOWN && (ev.cbutton.button == SDL_CONTROLLER_BUTTON_B || ev.cbutton.button == SDL_CONTROLLER_BUTTON_START)) ||
                    (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)) {
                    if (backend) { backend->disconnect(); backend.reset(); }
                    mode = AppMode::BROWSER;
                    status_banner = "已取消连接。";
                }
            } else if (mode == AppMode::STREAMING && backend) {
                // '+' button directly opens/closes Menu!
                if (ev.type == SDL_CONTROLLERBUTTONDOWN && ev.cbutton.button == SDL_CONTROLLER_BUTTON_START) {
                    show_overlay = !show_overlay;
                    show_osk = false;
                    overlay_focus = 0;
                    continue;
                }

                if (show_overlay) {
                    if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                        auto b = ev.cbutton.button;
                        if (b == SDL_CONTROLLER_BUTTON_B) show_overlay = false;
                        else if (b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT && !active_buttons.empty())
                            overlay_focus = (overlay_focus + 1) % static_cast<int>(active_buttons.size());
                        else if (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT && !active_buttons.empty())
                            overlay_focus = (overlay_focus - 1 + static_cast<int>(active_buttons.size())) % static_cast<int>(active_buttons.size());
                        else if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN && !active_buttons.empty())
                            overlay_focus = (overlay_focus + 4) % static_cast<int>(active_buttons.size());
                        else if (b == SDL_CONTROLLER_BUTTON_DPAD_UP && !active_buttons.empty())
                            overlay_focus = (overlay_focus - 4 + static_cast<int>(active_buttons.size())) % static_cast<int>(active_buttons.size());
                        else if (b == SDL_CONTROLLER_BUTTON_A && overlay_focus < static_cast<int>(active_buttons.size()))
                            handle_overlay_action(active_buttons[overlay_focus].id);
                    } else if (ev.type == SDL_KEYDOWN && (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_F12)) {
                        show_overlay = false;
                    } else if (desktop_tap) {
                        dispatch_ui_tap(dtap_x, dtap_y);
                    }
                } else if (show_osk) {
                    if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                        auto b = ev.cbutton.button;
                        if (b == SDL_CONTROLLER_BUTTON_B || b == SDL_CONTROLLER_BUTTON_X) show_osk = false;
                        else if (b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT && !active_osk.empty()) osk_focus = (osk_focus + 1) % static_cast<int>(active_osk.size());
                        else if (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT && !active_osk.empty())  osk_focus = (osk_focus - 1 + static_cast<int>(active_osk.size())) % static_cast<int>(active_osk.size());
                        else if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN && !active_osk.empty())  osk_focus = (osk_focus + 13) % static_cast<int>(active_osk.size());
                        else if (b == SDL_CONTROLLER_BUTTON_DPAD_UP && !active_osk.empty())    osk_focus = (osk_focus - 13 + static_cast<int>(active_osk.size())) % static_cast<int>(active_osk.size());
                        else if (b == SDL_CONTROLLER_BUTTON_A && osk_focus < static_cast<int>(active_osk.size()))
                            trigger_osk_key(active_osk[osk_focus]);
                    } else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) {
                        show_osk = false;
                    } else if (desktop_tap) {
                        dispatch_ui_tap(dtap_x, dtap_y);
                    }
                } else {
                    if (desktop_tap && UIRenderer::hit_test(menu_pill, dtap_x, dtap_y)) {
                        show_overlay = true;
                        overlay_focus = 0;
                        continue;
                    }
                    bool open_swkbd = false;
                    if (input.handle_sdl_event(ev, *backend, LOGICAL_W, LOGICAL_H, open_swkbd)) {
                        show_overlay = true;
                        overlay_focus = 0;
                    }
                    if (open_swkbd) {
                        open_streaming_swkbd();
                    }
                    // Desktop build: left click release also triggers the text-box check
                    if (!platform::uses_native_touch() && ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) {
                        ime_check_at_us = platform::now_us() + 320000ULL;
                        last_tap_x = static_cast<float>(ev.button.x);
                        last_tap_y = static_cast<float>(ev.button.y);
                    }
                    // Gamepad A (left click) also triggers the text-box check in mouse mode
                    if (ev.type == SDL_CONTROLLERBUTTONUP && ev.cbutton.button == SDL_CONTROLLER_BUTTON_A) {
                        ime_check_at_us = platform::now_us() + 320000ULL;
                    }
                }
            }
        }

        if (mode == AppMode::STREAMING && backend && !show_overlay && !show_osk) {
            input.update_frame(dt, pad, *backend);
        }

        // Auto-open Switch 2 native keyboard when the tap landed on a remote text input (I-beam cursor or macOS text caret/box)
        if (ime_check_at_us != 0 && platform::now_us() >= ime_check_at_us) {
            ime_check_at_us = 0;
            if (auto_ime && mode == AppMode::STREAMING && backend && !show_overlay && !show_osk) {
                bool is_text_target = backend->cursor_is_text();
                if (!is_text_target && (backend->protocol() == ProtocolType::VNC_MACOS || !backend->cursor_shape_known())) {
                    // Compute normalized remote coordinates of the tap / cursor and inspect framebuffer for caret or input box
                    const auto& vp = input.viewport();
                    float nx = vp.cursor_x;
                    float ny = vp.cursor_y;
                    if (input.touch_mode() == TouchMode::TOUCH && vp.dst_rect.w > 0 && vp.dst_rect.h > 0) {
                        nx = (last_tap_x - static_cast<float>(vp.dst_rect.x)) / static_cast<float>(vp.dst_rect.w);
                        ny = (last_tap_y - static_cast<float>(vp.dst_rect.y)) / static_cast<float>(vp.dst_rect.h);
                    }
                    is_text_target = backend->detect_text_focus_near(nx, ny);
                }
                if (is_text_target) {
                    platform::log_line("[UI] Tap on remote text field -> opening native keyboard directly");
                    open_streaming_swkbd();
                }
            }
        }

        // Render frame in exact 1920x1080 logical space
        if (mode == AppMode::BROWSER || mode == AppMode::FORM) {
            auto& wgt = wg::Tunnel::instance();
            auto wgst = wgt.state();
            std::string wg_badge;
            if (wgst == wg::State::UP) wg_badge = "WG: " + wgt.active_name();
            else if (wgst == wg::State::CONNECTING) wg_badge = "WG: 连接中...";
            else if (wgst == wg::State::FAILED) wg_badge = "WG: 异常";

            active_buttons = ui.render_profile_browser(
                LOGICAL_W, LOGICAL_H, win_profs, mac_profs, selected_col, selected_row,
                profiles.is_scanning(), profiles.scan_progress_pct(), input.touch_mode(), status_banner,
                wg_badge, wgst == wg::State::UP);
            if (mode == AppMode::FORM) {
                active_buttons = ui.render_connect_form(LOGICAL_W, LOGICAL_H, draft, form_focus, form_error);
            } else if (show_wg) {
                active_buttons = ui.render_wg_manager(
                    LOGICAL_W, LOGICAL_H, wg_configs, wgt.active_name(), wgt.status_text(),
                    wgst == wg::State::UP, wgt.rx_bytes(), wgt.tx_bytes(), wg_focus);
            } else if (show_manage) {
                active_buttons = ui.render_device_manager(LOGICAL_W, LOGICAL_H, profiles.saved_profiles(), manage_focus);
            } else if (show_home_menu) {
                active_buttons = ui.render_home_menu(LOGICAL_W, LOGICAL_H, input.touch_mode(), input.sensitivity(), wg_badge, home_menu_focus);
            }
        } else if (mode == AppMode::CONNECTING && backend) {
            SDL_SetRenderDrawColor(renderer, 11, 15, 25, 255);
            SDL_RenderClear(renderer);
            SDL_Rect box{LOGICAL_W / 2 - 440, LOGICAL_H / 2 - 90, 880, 180};
            ui.draw_panel(box, {22, 29, 44, 245}, {56, 189, 248, 255});
            ui.draw_text(box.x + 36, box.y + 42, backend->status_text(), {241, 245, 249, 255}, 2);
            ui.draw_text(box.x + 36, box.y + 108, "按 [B] 或 [+] 键取消连接", {148, 163, 184, 255}, 2);
        } else if (mode == AppMode::STREAMING && backend) {
            backend->sync_texture(renderer, fb_texture, fb_w, fb_h);
            SDL_SetRenderDrawColor(renderer, 6, 8, 12, 255);
            SDL_RenderClear(renderer);

            if (fb_texture && fb_w > 0 && fb_h > 0) {
                input.update_viewport_rect(LOGICAL_W, LOGICAL_H, fb_w, fb_h);
                SDL_RenderCopy(renderer, fb_texture, nullptr, &input.viewport().dst_rect);
            } else {
                ui.draw_text(LOGICAL_W / 2 - 200, LOGICAL_H / 2 - 12, "正在等待远程桌面画面...", {200, 215, 235, 255}, 2);
            }

            if (input.touch_mode() == TouchMode::MOUSE) {
                ui.render_cursor(input.viewport());
            }
            menu_pill = ui.render_menu_pill(LOGICAL_W, LOGICAL_H);
            auto st = backend->stats();
            if (show_hud) ui.render_perf_hud(LOGICAL_W, LOGICAL_H, st, input.touch_mode(), input.viewport().zoom);
            if (show_overlay) {
                active_buttons = ui.render_quick_overlay(
                    LOGICAL_W, LOGICAL_H, active_profile, st, input.touch_mode(), show_hud, auto_ime, input.sensitivity(), overlay_focus);
            } else if (show_osk) {
                active_osk = ui.render_osk(LOGICAL_W, LOGICAL_H, active_profile.protocol, osk_sticky_mods, osk_focus);
            }
        }

        SDL_RenderPresent(renderer);
    }

    if (backend) backend->disconnect();
    wg::Tunnel::instance().stop();
    if (fb_texture) SDL_DestroyTexture(fb_texture);
    profiles.stop_lan_scan();
    ui.shutdown();
    if (pad) SDL_GameControllerClose(pad);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    platform::shutdown();
    return 0;
}
