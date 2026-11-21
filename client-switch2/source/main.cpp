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

    int win_w = 1920;
    int win_h = 1080;
    Uint32 win_flags = platform::is_switch_hardware()
                           ? (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN)
                           : (SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);

    SDL_Window* window = SDL_CreateWindow(
        "OunceDesk - Switch 2 Windows RDP (3389) & macOS Remote Desktop",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h, win_flags);
    if (!window) {
        SDL_Quit();
        platform::shutdown();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }

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
    profiles.start_mdns();

    InputMapper input;
    std::unique_ptr<RemoteBackend> backend;
    SDL_Texture* fb_texture = nullptr;
    int fb_w = 0, fb_h = 0;

    AppMode mode = AppMode::BROWSER;
    ConnectionProfile draft;
    ConnectionProfile active_profile;
    int selected_profile = 0;
    int form_focus = 1;
    int overlay_focus = 0;
    int osk_focus = 0;
    uint8_t osk_sticky_mods = 0;
    bool show_overlay = false;
    bool show_osk = false;
    bool show_hud = true;

    std::string status_banner;
    std::string form_error;
    std::vector<UIButton> active_buttons;
    std::vector<OSKKey> active_osk;
    SDL_Rect menu_pill{16, 16, 118, 40};

    auto open_new_form = [&](ProtocolType proto) {
        draft = {};
        draft.protocol = proto;
        draft.port = (proto == ProtocolType::VNC_MACOS) ? 5900 : 3389;
        draft.host = "192.168.1.";
        draft.req_width = 1920;
        draft.req_height = 1080;
        form_focus = 1;
        form_error.clear();
        mode = AppMode::FORM;
    };

    auto start_connection = [&](const ConnectionProfile& p) {
        if (p.host.empty()) {
            form_error = "Please enter Host IP address";
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
            if (platform::show_system_keyboard("Host IP Address (e.g. 192.168.1.50)", draft.host, v, false)) {
                draft.host = v;
            }
        } else if (field_idx == 2) {
            std::string v;
            if (platform::show_system_keyboard("TCP Port (3389 RDP / 5900 VNC)", std::to_string(draft.port), v, true)) {
                int p = std::atoi(v.c_str());
                if (p > 0 && p <= 65535) draft.port = static_cast<uint16_t>(p);
            }
        } else if (field_idx == 3) {
            std::string v;
            std::string hdr = (draft.protocol == ProtocolType::VNC_MACOS)
                                  ? "macOS Username (leave empty if using VNC password)"
                                  : "Windows Username";
            if (platform::show_system_keyboard(hdr, draft.username, v, false)) {
                draft.username = v;
            }
        } else if (field_idx == 4) {
            std::string v;
            if (platform::show_system_keyboard("Password", "", v, false)) {
                draft.password = v;
            }
        } else if (field_idx == 5) {
            if (draft.protocol == ProtocolType::VNC_MACOS) {
                std::string v;
                if (platform::show_system_keyboard("Display Name", draft.name, v, false)) draft.name = v;
            } else {
                if (draft.req_width == 1920) { draft.req_width = 1280; draft.req_height = 720; }
                else { draft.req_width = 1920; draft.req_height = 1080; }
            }
        }
    };

    auto handle_overlay_action = [&](const std::string& id) {
        if (!backend) return;
        if (id.rfind("sc:", 0) == 0) {
            input.send_shortcut(*backend, active_profile.protocol, id.substr(3));
            show_overlay = false;
        } else if (id == "act:touch_mode") {
            input.toggle_touch_mode();
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
            std::string typed;
            if (platform::show_system_keyboard("Type text to send to Windows / macOS", "", typed, false)) {
                backend->send_text_utf8(typed);
            } else {
                show_osk = true;
            }
        } else if (id == "act:resume") {
            show_overlay = false;
        } else if (id == "act:disconnect") {
            backend->disconnect();
            backend.reset();
            show_overlay = false;
            show_osk = false;
            mode = AppMode::BROWSER;
            status_banner = "Disconnected.";
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

    bool running = true;
    uint64_t last_us = platform::now_us();

    while (running) {
        uint64_t now = platform::now_us();
        float dt = std::min(0.1f, static_cast<float>(now - last_us) / 1e6f);
        last_us = now;

        SDL_GetWindowSize(window, &win_w, &win_h);

        if (mode == AppMode::BROWSER) {
            profiles.poll_mdns();
        }
        auto all_profs = profiles.all_profiles();
        if (!all_profs.empty()) {
            selected_profile = std::max(0, std::min(selected_profile, static_cast<int>(all_profs.size()) - 1));
        } else {
            selected_profile = 0;
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

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { running = false; break; }
            if (ev.type == SDL_CONTROLLERDEVICEADDED && !pad) {
                pad = SDL_GameControllerOpen(ev.cdevice.which);
            }

            bool ui_tap = false;
            int tap_x = 0, tap_y = 0;
            if (ev.type == SDL_FINGERDOWN) {
                ui_tap = true;
                tap_x = static_cast<int>(ev.tfinger.x * win_w);
                tap_y = static_cast<int>(ev.tfinger.y * win_h);
            } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT && !platform::is_switch_hardware()) {
                ui_tap = true;
                tap_x = ev.button.x;
                tap_y = ev.button.y;
            }

            if (mode == AppMode::BROWSER) {
                if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                    auto b = ev.cbutton.button;
                    if (b == SDL_CONTROLLER_BUTTON_START) running = false;
                    else if (b == SDL_CONTROLLER_BUTTON_Y) open_new_form(ProtocolType::RDP_WINDOWS);
                    else if (b == SDL_CONTROLLER_BUTTON_X) open_new_form(ProtocolType::VNC_MACOS);
                    else if (b == SDL_CONTROLLER_BUTTON_BACK && selected_profile < static_cast<int>(profiles.saved_profiles().size())) {
                        profiles.remove_at(static_cast<size_t>(selected_profile), profiles_path);
                    }
                    else if (b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) selected_profile++;
                    else if (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT)  selected_profile = std::max(0, selected_profile - 1);
                    else if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN)  selected_profile += 2;
                    else if (b == SDL_CONTROLLER_BUTTON_DPAD_UP)    selected_profile = std::max(0, selected_profile - 2);
                    else if (b == SDL_CONTROLLER_BUTTON_A && !all_profs.empty()) {
                        draft = all_profs[selected_profile];
                        form_focus = draft.password.empty() ? 4 : 8;
                        form_error.clear();
                        mode = AppMode::FORM;
                    }
                } else if (ev.type == SDL_KEYDOWN) {
                    if (ev.key.keysym.sym == SDLK_ESCAPE) running = false;
                    else if (ev.key.keysym.sym == SDLK_r) open_new_form(ProtocolType::RDP_WINDOWS);
                    else if (ev.key.keysym.sym == SDLK_m) open_new_form(ProtocolType::VNC_MACOS);
                } else if (ui_tap) {
                    for (const auto& btn : active_buttons) {
                        if (!UIRenderer::hit_test(btn.rect, tap_x, tap_y)) continue;
                        if (btn.id == "add_rdp") open_new_form(ProtocolType::RDP_WINDOWS);
                        else if (btn.id == "add_mac") open_new_form(ProtocolType::VNC_MACOS);
                        else if (btn.id == "scan_mdns") {
                            profiles.send_mdns_query();
                            status_banner = "Scanning LAN via Bonjour (_rfb._tcp.local) for Macs...";
                        } else if (btn.id.rfind("profile:", 0) == 0) {
                            size_t idx = static_cast<size_t>(std::atoi(btn.id.c_str() + 8));
                            if (idx < all_profs.size()) {
                                draft = all_profs[idx];
                                form_focus = draft.password.empty() ? 4 : 8;
                                form_error.clear();
                                mode = AppMode::FORM;
                            }
                        }
                        break;
                    }
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
                } else if (ui_tap) {
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
                }
            } else if (mode == AppMode::CONNECTING) {
                if ((ev.type == SDL_CONTROLLERBUTTONDOWN && ev.cbutton.button == SDL_CONTROLLER_BUTTON_B) ||
                    (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)) {
                    if (backend) { backend->disconnect(); backend.reset(); }
                    mode = AppMode::BROWSER;
                    status_banner = "Connection canceled.";
                }
            } else if (mode == AppMode::STREAMING && backend) {
                if (ev.type == SDL_CONTROLLERBUTTONDOWN && ev.cbutton.button == SDL_CONTROLLER_BUTTON_START && pad) {
                    bool l = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
                    bool r = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;
                    if (l && r) { show_overlay = !show_overlay; show_osk = false; continue; }
                }
                if (show_overlay) {
                    if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                        auto b = ev.cbutton.button;
                        if (b == SDL_CONTROLLER_BUTTON_B) show_overlay = false;
                        else if (b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT && !active_buttons.empty())
                            overlay_focus = (overlay_focus + 1) % static_cast<int>(active_buttons.size());
                        else if (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT && !active_buttons.empty())
                            overlay_focus = (overlay_focus - 1 + static_cast<int>(active_buttons.size())) % static_cast<int>(active_buttons.size());
                        else if (b == SDL_CONTROLLER_BUTTON_A && overlay_focus < static_cast<int>(active_buttons.size()))
                            handle_overlay_action(active_buttons[overlay_focus].id);
                    } else if (ev.type == SDL_KEYDOWN && (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_F12)) {
                        show_overlay = false;
                    } else if (ui_tap) {
                        for (const auto& btn : active_buttons) {
                            if (UIRenderer::hit_test(btn.rect, tap_x, tap_y)) { handle_overlay_action(btn.id); break; }
                        }
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
                    } else if (ui_tap) {
                        for (const auto& k : active_osk) {
                            if (UIRenderer::hit_test(k.rect, tap_x, tap_y)) { trigger_osk_key(k); break; }
                        }
                    }
                } else {
                    if (ui_tap && UIRenderer::hit_test(menu_pill, tap_x, tap_y)) {
                        show_overlay = true;
                        continue;
                    }
                    bool toggle_osk = false;
                    if (input.handle_sdl_event(ev, *backend, win_w, win_h, toggle_osk)) show_overlay = true;
                    if (toggle_osk) show_osk = !show_osk;
                }
            }
        }

        if (mode == AppMode::STREAMING && backend && !show_overlay && !show_osk) {
            input.update_frame(dt, pad, *backend);
        }

        // Render frame
        if (mode == AppMode::BROWSER || mode == AppMode::FORM) {
            active_buttons = ui.render_profile_browser(win_w, win_h, all_profs, selected_profile, status_banner);
            if (mode == AppMode::FORM) {
                active_buttons = ui.render_connect_form(win_w, win_h, draft, form_focus, form_error);
            }
        } else if (mode == AppMode::CONNECTING && backend) {
            SDL_SetRenderDrawColor(renderer, 11, 15, 25, 255);
            SDL_RenderClear(renderer);
            SDL_Rect box{win_w / 2 - 420, win_h / 2 - 90, 840, 180};
            ui.draw_panel(box, {22, 29, 44, 245}, {56, 189, 248, 255});
            ui.draw_text(box.x + 36, box.y + 42, backend->status_text(), {241, 245, 249, 255}, 2);
            ui.draw_text(box.x + 36, box.y + 108, "Press [B] / Esc to cancel", {148, 163, 184, 255}, 2);
        } else if (mode == AppMode::STREAMING && backend) {
            backend->sync_texture(renderer, fb_texture, fb_w, fb_h);
            SDL_SetRenderDrawColor(renderer, 6, 8, 12, 255);
            SDL_RenderClear(renderer);

            if (fb_texture && fb_w > 0 && fb_h > 0) {
                input.update_viewport_rect(win_w, win_h, fb_w, fb_h);
                SDL_RenderCopy(renderer, fb_texture, nullptr, &input.viewport().dst_rect);
            } else {
                ui.draw_text(win_w / 2 - 200, win_h / 2 - 12, "Waiting for remote desktop...", {200, 215, 235, 255}, 2);
            }

            if (input.touch_mode() == TouchMode::TRACKPAD || platform::is_switch_hardware()) {
                ui.render_cursor(input.viewport());
            }
            menu_pill = ui.render_menu_pill(win_w, win_h);
            auto st = backend->stats();
            if (show_hud) ui.render_perf_hud(win_w, win_h, st, input.touch_mode(), input.viewport().zoom);
            if (show_overlay) {
                active_buttons = ui.render_quick_overlay(win_w, win_h, active_profile, st, input.touch_mode(), show_hud, overlay_focus);
            } else if (show_osk) {
                active_osk = ui.render_osk(win_w, win_h, active_profile.protocol, osk_sticky_mods, osk_focus);
            }
        }

        SDL_RenderPresent(renderer);
    }

    if (backend) backend->disconnect();
    if (fb_texture) SDL_DestroyTexture(fb_texture);
    profiles.stop_mdns();
    ui.shutdown();
    if (pad) SDL_GameControllerClose(pad);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    platform::shutdown();
    return 0;
}
