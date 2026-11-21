#include "backend.hpp"
#include "input_mapper.hpp"
#include "platform.hpp"
#include "profiles.hpp"
#include "rdp_backend.hpp"
#include "ui_renderer.hpp"
#include "vnc_backend.hpp"

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

    // Saved-device manager (home menu) + sensitivity slider + macOS keyboard bubble state
    bool show_manage = false;
    bool touch_block = false;  // after a modal keyboard: ignore touch until all fingers lifted
    int manage_focus = 0;
    bool slider_dragging = false;
    SDL_Rect kbd_bubble{0, 0, 0, 0};
    uint64_t kbd_bubble_until_us = 0;
    int bubble_x = 0, bubble_y = 0;

    // Persistent settings: sdmc:/switch/OunceDesk/settings.txt
    const std::string settings_path = platform::get_config_dir() + "/settings.txt";
    auto save_settings = [&]() {
        FILE* f = std::fopen(settings_path.c_str(), "w");
        if (!f) return;
        std::fprintf(f, "sensitivity=%.2f\nauto_ime=%d\ntouch_mode=%d\n", input.sensitivity(), auto_ime ? 1 : 0,
                     input.touch_mode() == TouchMode::MOUSE ? 1 : 0);
        std::fclose(f);
    };
    {
        FILE* f = std::fopen(settings_path.c_str(), "r");
        if (f) {
            char line[128];
            while (std::fgets(line, sizeof(line), f)) {
                float fv = 0.0f;
                int iv = 0;
                if (std::sscanf(line, "sensitivity=%f", &fv) == 1) input.set_sensitivity(fv);
                else if (std::sscanf(line, "auto_ime=%d", &iv) == 1) auto_ime = (iv != 0);
                else if (std::sscanf(line, "touch_mode=%d", &iv) == 1) input.set_touch_mode(iv ? TouchMode::MOUSE : TouchMode::TOUCH);
            }
            std::fclose(f);
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

    auto open_new_form = [&](ProtocolType proto) {
        draft = {};
        draft.protocol = proto;
        draft.port = (proto == ProtocolType::VNC_MACOS) ? 5900 : 3389;
        draft.host = "";
        draft.req_width = 1920;
        draft.req_height = 1080;
        form_focus = 1;
        form_error.clear();
        show_home_menu = false;
        mode = AppMode::FORM;
    };

    auto start_connection = [&](const ConnectionProfile& p) {
        if (p.host.empty()) {
            form_error = "请输入电脑 IP / IPv6 地址或 DDNS 域名";
            return;
        }
        active_profile = p;
        profiles.add_or_update(active_profile, profiles_path);
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
            if (platform::show_system_keyboard("输入 IP / IPv6 / DDNS 域名 (可加 :端口，如 myhome.ddns.net:3390)", draft.host, v, false, false)) {
                draft.host = v;
            }
        } else if (field_idx == 2) {
            std::string v;
            if (platform::show_system_keyboard("请输入端口号 (Windows 3389 / Mac 5900)", std::to_string(draft.port), v, true, false)) {
                int p = std::atoi(v.c_str());
                if (p > 0 && p <= 65535) draft.port = static_cast<uint16_t>(p);
            }
        } else if (field_idx == 3) {
            std::string v;
            std::string hdr = (draft.protocol == ProtocolType::VNC_MACOS)
                                  ? "请输入 macOS 用户名 (若仅用 VNC 密码可留空)"
                                  : "请输入 Windows 登录用户名";
            if (platform::show_system_keyboard(hdr, draft.username, v, false, false)) {
                draft.username = v;
            }
        } else if (field_idx == 4) {
            std::string v;
            if (platform::show_system_keyboard("请输入登录密码", draft.password, v, false, true)) {
                draft.password = v;
            }
        } else if (field_idx == 5) {
            if (draft.protocol == ProtocolType::VNC_MACOS) {
                std::string v;
                if (platform::show_system_keyboard("请输入设备备注名称", draft.name, v, false, false)) draft.name = v;
            } else {
                if (draft.req_width == 1920) { draft.req_width = 1280; draft.req_height = 720; }
                else { draft.req_width = 1920; draft.req_height = 1080; }
            }
        }
    };

    auto open_streaming_swkbd = [&]() {
        if (!backend) return;
        std::string typed;
        if (platform::show_system_keyboard("请输入要发送到远程电脑的文字", "", typed, false, false)) {
            if (!typed.empty()) backend->send_text_utf8(typed);
        } else {
            // Desktop fallback when native swkbd does not exist
            show_osk = true;
        }
    };

    auto handle_home_menu_action = [&](const std::string& id) {
        if (id == "hm:mode") {
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

    uint64_t last_us = platform::now_us();

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
                    if (btn.id == "add_rdp") open_new_form(ProtocolType::RDP_WINDOWS);
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
                            form_focus = draft.username.empty() ? 3 : (draft.password.empty() ? 4 : 8);
                            form_error.clear();
                            mode = AppMode::FORM;
                        }
                    } else if (btn.id.rfind("mac:", 0) == 0) {
                        size_t idx = static_cast<size_t>(std::atoi(btn.id.c_str() + 4));
                        if (idx < mac_profs.size()) {
                            draft = mac_profs[idx];
                            form_focus = draft.username.empty() ? 3 : (draft.password.empty() ? 4 : 8);
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
                    else if (btn.id == "f:host") { form_focus = 1; edit_form_field(1); }
                    else if (btn.id == "f:port") { form_focus = 2; edit_form_field(2); }
                    else if (btn.id == "f:user") { form_focus = 3; edit_form_field(3); }
                    else if (btn.id == "f:pass") { form_focus = 4; edit_form_field(4); }
                    else if (btn.id == "f:res")  { form_focus = 5; edit_form_field(5); }
                    else if (btn.id == "form_cancel") mode = AppMode::BROWSER;
                    else if (btn.id == "form_save") { profiles.add_or_update(draft, profiles_path); mode = AppMode::BROWSER; }
                    else if (btn.id == "form_connect") start_connection(draft);
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
                        // Floating "open keyboard" bubble (macOS fallback): tap it -> Switch 2 keyboard; tap anywhere else -> dismiss
                        if (kbd_bubble_until_us != 0) {
                            bool hit_bubble = UIRenderer::hit_test(kbd_bubble, tx, ty);
                            kbd_bubble_until_us = 0;
                            if (hit_bubble) {
                                tap_tracking = false;
                                open_streaming_swkbd();
                                touch_block = true;  // ignore touches until all fingers are lifted
                                break;
                            }
                        }
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
                        form_focus = draft.username.empty() ? 3 : (draft.password.empty() ? 4 : 8);
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
                if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                    auto b = ev.cbutton.button;
                    if (b == SDL_CONTROLLER_BUTTON_B) mode = AppMode::BROWSER;
                    else if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) form_focus = (form_focus + 1) % 9;
                    else if (b == SDL_CONTROLLER_BUTTON_DPAD_UP)   form_focus = (form_focus + 8) % 9;
                    else if (b == SDL_CONTROLLER_BUTTON_A) {
                        if (form_focus < 6) edit_form_field(form_focus);
                        else if (form_focus == 6) mode = AppMode::BROWSER;
                        else if (form_focus == 7) { profiles.add_or_update(draft, profiles_path); mode = AppMode::BROWSER; }
                        else if (form_focus == 8) start_connection(draft);
                    }
                } else if (ev.type == SDL_KEYDOWN) {
                    SDL_Keycode sym = ev.key.keysym.sym;
                    if (sym == SDLK_ESCAPE) mode = AppMode::BROWSER;
                    else if (sym == SDLK_TAB) form_focus = (form_focus + 1) % 9;
                    else if (sym == SDLK_RETURN) {
                        if (form_focus == 0) edit_form_field(0);
                        else start_connection(draft);
                    } else if (sym == SDLK_BACKSPACE) {
                        if (form_focus == 1 && !draft.host.empty()) draft.host.pop_back();
                        else if (form_focus == 3 && !draft.username.empty()) draft.username.pop_back();
                        else if (form_focus == 4 && !draft.password.empty()) draft.password.pop_back();
                        else if (form_focus == 5 && draft.protocol == ProtocolType::VNC_MACOS && !draft.name.empty()) draft.name.pop_back();
                    }
                } else if (ev.type == SDL_TEXTINPUT) {
                    if (form_focus == 1) draft.host += ev.text.text;
                    else if (form_focus == 3) draft.username += ev.text.text;
                    else if (form_focus == 4) draft.password += ev.text.text;
                    else if (form_focus == 5 && draft.protocol == ProtocolType::VNC_MACOS) draft.name += ev.text.text;
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
                    if (desktop_tap && kbd_bubble_until_us != 0) {
                        bool hit_b = UIRenderer::hit_test(kbd_bubble, dtap_x, dtap_y);
                        kbd_bubble_until_us = 0;
                        if (hit_b) { open_streaming_swkbd(); continue; }
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

        // Auto-open Switch 2 native keyboard when the tap landed on a remote text input (I-beam cursor)
        if (ime_check_at_us != 0 && platform::now_us() >= ime_check_at_us) {
            ime_check_at_us = 0;
            if (auto_ime && mode == AppMode::STREAMING && backend && !show_overlay && !show_osk) {
                if (backend->cursor_is_text()) {
                    platform::log_line("[UI] Tap on remote text field -> opening Switch 2 keyboard");
                    std::string typed;
                    if (platform::show_system_keyboard("请输入文字 (点取消/空白处返回)", "", typed, false, false)) {
                        if (!typed.empty()) backend->send_text_utf8(typed);
                    }
                    prev_native_touches.clear();
                    touch_block = true;
                    last_us = platform::now_us();
                } else if (backend->protocol() == ProtocolType::VNC_MACOS || !backend->cursor_shape_known()) {
                    // macOS Screen Sharing often does not report the I-beam cursor shape -> show a small
                    // floating "keyboard" bubble next to the tap. Tap it = Switch 2 keyboard; tap elsewhere = dismiss.
                    float px = last_tap_x, py = last_tap_y;
                    if (input.touch_mode() == TouchMode::MOUSE) {
                        const auto& vp = input.viewport();
                        px = vp.dst_rect.x + vp.cursor_x * vp.dst_rect.w;
                        py = vp.dst_rect.y + vp.cursor_y * vp.dst_rect.h;
                    }
                    bubble_x = std::max(10, std::min(LOGICAL_W - 260, static_cast<int>(px) + 24));
                    bubble_y = static_cast<int>(py) - 100;
                    if (bubble_y < 70) bubble_y = static_cast<int>(py) + 50;
                    bubble_y = std::max(10, std::min(LOGICAL_H - 74, bubble_y));
                    kbd_bubble_until_us = platform::now_us() + 3000000ULL;
                }
            }
        }
        if (kbd_bubble_until_us != 0 && (platform::now_us() > kbd_bubble_until_us || mode != AppMode::STREAMING || show_overlay)) {
            kbd_bubble_until_us = 0;
        }

        // Render frame in exact 1920x1080 logical space
        if (mode == AppMode::BROWSER || mode == AppMode::FORM) {
            active_buttons = ui.render_profile_browser(
                LOGICAL_W, LOGICAL_H, win_profs, mac_profs, selected_col, selected_row,
                profiles.is_scanning(), profiles.scan_progress_pct(), input.touch_mode(), status_banner);
            if (mode == AppMode::FORM) {
                active_buttons = ui.render_connect_form(LOGICAL_W, LOGICAL_H, draft, form_focus, form_error);
            } else if (show_manage) {
                active_buttons = ui.render_device_manager(LOGICAL_W, LOGICAL_H, profiles.saved_profiles(), manage_focus);
            } else if (show_home_menu) {
                active_buttons = ui.render_home_menu(LOGICAL_W, LOGICAL_H, input.touch_mode(), input.sensitivity(), home_menu_focus);
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
            if (kbd_bubble_until_us != 0 && !show_overlay && !show_osk) {
                kbd_bubble = ui.render_kbd_bubble(bubble_x, bubble_y);
            }
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
