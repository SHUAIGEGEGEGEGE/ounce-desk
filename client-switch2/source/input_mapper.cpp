#include "input_mapper.hpp"

#include <algorithm>
#include <cmath>

namespace {
float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

float apply_stick_curve(int16_t raw) {
    float n = static_cast<float>(raw) / 32767.0f;
    constexpr float deadzone = 0.14f;
    float mag = std::fabs(n);
    if (mag < deadzone) return 0.0f;
    float scaled = (mag - deadzone) / (1.0f - deadzone);
    float curved = scaled * (0.35f + 0.65f * scaled * scaled);
    return (n < 0.0f) ? -curved : curved;
}
}  // namespace

void InputMapper::update_viewport_rect(int win_w, int win_h, int fb_w, int fb_h) {
    if (fb_w <= 0 || fb_h <= 0) {
        vp_.dst_rect = {0, 0, win_w, win_h};
        return;
    }
    float win_ar = static_cast<float>(win_w) / static_cast<float>(win_h);
    float fb_ar = static_cast<float>(fb_w) / static_cast<float>(fb_h);
    int base_w = win_w, base_h = win_h;
    if (fb_ar > win_ar) base_h = static_cast<int>(win_w / fb_ar);
    else base_w = static_cast<int>(win_h * fb_ar);

    int zw = static_cast<int>(base_w * vp_.zoom);
    int zh = static_cast<int>(base_h * vp_.zoom);
    if (vp_.zoom <= 1.01f) {
        vp_.pan_x = 0.5f;
        vp_.pan_y = 0.5f;
        vp_.dst_rect = {(win_w - base_w) / 2, (win_h - base_h) / 2, base_w, base_h};
    } else {
        vp_.pan_x = clampf(vp_.pan_x, 0.1f, 0.9f);
        vp_.pan_y = clampf(vp_.pan_y, 0.1f, 0.9f);
        vp_.dst_rect = {win_w / 2 - static_cast<int>(vp_.pan_x * zw),
                        win_h / 2 - static_cast<int>(vp_.pan_y * zh), zw, zh};
    }
}

bool InputMapper::screen_to_norm(float sx, float sy, float& out_nx, float& out_ny) const {
    if (vp_.dst_rect.w <= 0 || vp_.dst_rect.h <= 0) return false;
    out_nx = clampf((sx - vp_.dst_rect.x) / static_cast<float>(vp_.dst_rect.w), 0.0f, 1.0f);
    out_ny = clampf((sy - vp_.dst_rect.y) / static_cast<float>(vp_.dst_rect.h), 0.0f, 1.0f);
    return true;
}

void InputMapper::send_cursor(RemoteBackend& backend) {
    vp_.cursor_x = clampf(vp_.cursor_x, 0.0f, 1.0f);
    vp_.cursor_y = clampf(vp_.cursor_y, 0.0f, 1.0f);
    backend.send_mouse_abs(vp_.cursor_x, vp_.cursor_y);
}

bool InputMapper::on_touch_down(uint64_t finger_id, float sx, float sy, RemoteBackend& backend, bool suppress_direct_click) {
    fingers_[finger_id] = {sx, sy, sx, sy, platform::now_us(), false};
    if (fingers_.size() >= 3) {
        if (direct_dragging_) {
            backend.send_mouse_button(MouseBtn::LEFT, false);
            direct_dragging_ = false;
        }
        fingers_.clear();
        two_finger_gesture_ = false;
        return true;  // 3-finger tap opens Menu
    }
    if (fingers_.size() == 2) {
        if (direct_dragging_) {
            backend.send_mouse_button(MouseBtn::LEFT, false);
            direct_dragging_ = false;
        }
        two_finger_gesture_ = true;
        auto it = fingers_.begin();
        const auto& f0 = it->second;
        ++it;
        const auto& f1 = it->second;
        prev_pinch_dist_ = std::hypot(f1.x - f0.x, f1.y - f0.y);
        prev_two_mid_x_ = 0.5f * (f0.x + f1.x);
        prev_two_mid_y_ = 0.5f * (f0.y + f1.y);
    } else if (fingers_.size() == 1 && touch_mode_ == TouchMode::TOUCH) {
        float nx, ny;
        if (screen_to_norm(sx, sy, nx, ny)) {
            vp_.cursor_x = nx;
            vp_.cursor_y = ny;
            send_cursor(backend);
            if (!suppress_direct_click) {
                backend.send_mouse_button(MouseBtn::LEFT, true);
                direct_dragging_ = true;
            }
        }
    }
    return false;
}

void InputMapper::on_touch_move(uint64_t finger_id, float sx, float sy, RemoteBackend& backend, int win_w, int win_h) {
    auto it = fingers_.find(finger_id);
    if (it == fingers_.end()) return;
    float dx = sx - it->second.x;
    float dy = sy - it->second.y;
    it->second.x = sx;
    it->second.y = sy;
    if (std::hypot(sx - it->second.start_x, sy - it->second.start_y) > 18.0f) {
        it->second.moved = true;
    }

    if (fingers_.size() == 2) {
        auto fit = fingers_.begin();
        const auto& f0 = fit->second;
        ++fit;
        const auto& f1 = fit->second;
        float dist = std::hypot(f1.x - f0.x, f1.y - f0.y);
        float mid_x = 0.5f * (f0.x + f1.x);
        float mid_y = 0.5f * (f0.y + f1.y);
        float ddist = dist - prev_pinch_dist_;
        float dmx = mid_x - prev_two_mid_x_;
        float dmy = mid_y - prev_two_mid_y_;

        if (std::fabs(ddist) > 12.0f && prev_pinch_dist_ > 20.0f) {
            vp_.zoom = clampf(vp_.zoom * (dist / prev_pinch_dist_), 1.0f, 3.0f);
            prev_pinch_dist_ = dist;
        } else if (std::fabs(dmx) > 2.0f || std::fabs(dmy) > 2.0f) {
            if (vp_.zoom > 1.05f) {
                vp_.pan_x = clampf(vp_.pan_x - dmx / (win_w * vp_.zoom), 0.1f, 0.9f);
                vp_.pan_y = clampf(vp_.pan_y - dmy / (win_h * vp_.zoom), 0.1f, 0.9f);
            } else {
                backend.send_mouse_wheel(static_cast<int16_t>(dmx * 3.0f), static_cast<int16_t>(dmy * 3.5f));
            }
        }
        prev_two_mid_x_ = mid_x;
        prev_two_mid_y_ = mid_y;
    } else if (fingers_.size() == 1) {
        if (touch_mode_ == TouchMode::MOUSE) {
            // Trackpad-style precision mouse movement with smooth acceleration
            float speed = std::hypot(dx, dy);
            float accel = 1.0f + clampf(speed / 28.0f, 0.0f, 1.4f);
            vp_.cursor_x += (dx * accel * sensitivity_) / static_cast<float>(std::max(1, vp_.dst_rect.w));
            vp_.cursor_y += (dy * accel * sensitivity_) / static_cast<float>(std::max(1, vp_.dst_rect.h));
            send_cursor(backend);
        } else if (touch_mode_ == TouchMode::TOUCH && (direct_dragging_ || it->second.moved)) {
            if (!direct_dragging_) {
                backend.send_mouse_button(MouseBtn::LEFT, true);
                direct_dragging_ = true;
            }
            // Exact 1:1 direct touch drag
            float nx, ny;
            if (screen_to_norm(sx, sy, nx, ny)) {
                vp_.cursor_x = nx;
                vp_.cursor_y = ny;
                send_cursor(backend);
            }
        }
    }
}

bool InputMapper::on_touch_up(uint64_t finger_id, RemoteBackend& backend, bool suppress_tap_click) {
    bool primary_tap = false;
    auto it = fingers_.find(finger_id);
    if (it != fingers_.end()) {
        uint64_t held_us = platform::now_us() - it->second.down_us;
        bool was_tap = !it->second.moved && held_us < 380000ULL;
        if (direct_dragging_) {
            backend.send_mouse_button(MouseBtn::LEFT, false);
            direct_dragging_ = false;
        } else if (two_finger_gesture_ && fingers_.size() == 2 && was_tap) {
            // 2-finger tap = Right Click (works in both Touch & Mouse modes!)
            backend.send_mouse_button(MouseBtn::RIGHT, true);
            backend.send_mouse_button(MouseBtn::RIGHT, false);
        } else if (!two_finger_gesture_ && fingers_.size() == 1 && touch_mode_ == TouchMode::MOUSE && was_tap) {
            // 1-finger tap in Mouse mode = Left Click
            if (!suppress_tap_click) {
                backend.send_mouse_button(MouseBtn::LEFT, true);
                backend.send_mouse_button(MouseBtn::LEFT, false);
            }
        }
        if (!two_finger_gesture_ && fingers_.size() == 1 && was_tap) {
            primary_tap = true;
        }
        fingers_.erase(it);
    }
    if (fingers_.empty()) two_finger_gesture_ = false;
    return primary_tap;
}

bool InputMapper::handle_sdl_event(const SDL_Event& ev, RemoteBackend& backend, int win_w, int win_h, bool& out_open_swkbd) {
    out_open_swkbd = false;

    switch (ev.type) {
        case SDL_FINGERDOWN:
            if (!platform::uses_native_touch()) {
                return on_touch_down(static_cast<uint64_t>(ev.tfinger.fingerId),
                                     ev.tfinger.x * win_w, ev.tfinger.y * win_h, backend);
            }
            break;
        case SDL_FINGERMOTION:
            if (!platform::uses_native_touch()) {
                on_touch_move(static_cast<uint64_t>(ev.tfinger.fingerId),
                              ev.tfinger.x * win_w, ev.tfinger.y * win_h, backend, win_w, win_h);
            }
            break;
        case SDL_FINGERUP:
            if (!platform::uses_native_touch()) {
                on_touch_up(static_cast<uint64_t>(ev.tfinger.fingerId), backend);
            }
            break;
        case SDL_MOUSEMOTION: {
            if (!platform::is_switch_hardware()) {
                float nx, ny;
                if (screen_to_norm(static_cast<float>(ev.motion.x), static_cast<float>(ev.motion.y), nx, ny)) {
                    vp_.cursor_x = nx;
                    vp_.cursor_y = ny;
                    send_cursor(backend);
                }
            }
            break;
        }
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            if (!platform::is_switch_hardware()) {
                bool pressed = (ev.type == SDL_MOUSEBUTTONDOWN);
                if (ev.button.button == SDL_BUTTON_LEFT) backend.send_mouse_button(MouseBtn::LEFT, pressed);
                else if (ev.button.button == SDL_BUTTON_RIGHT) backend.send_mouse_button(MouseBtn::RIGHT, pressed);
                else if (ev.button.button == SDL_BUTTON_MIDDLE) backend.send_mouse_button(MouseBtn::MIDDLE, pressed);
            }
            break;
        }
        case SDL_MOUSEWHEEL: {
            if (!platform::is_switch_hardware()) {
                backend.send_mouse_wheel(static_cast<int16_t>(ev.wheel.x * 120), static_cast<int16_t>(ev.wheel.y * 120));
            }
            break;
        }
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            if (!platform::is_switch_hardware()) {
                if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F12) return true;
                uint8_t mods = 0;
                Uint16 sm = SDL_GetModState();
                if (sm & KMOD_LCTRL) mods |= RD_MOD_LCTRL;
                if (sm & KMOD_LSHIFT) mods |= RD_MOD_LSHIFT;
                if (sm & KMOD_LALT) mods |= RD_MOD_LALT;
                if (sm & KMOD_LGUI) mods |= RD_MOD_LMETA;
                if (sm & KMOD_RCTRL) mods |= RD_MOD_RCTRL;
                if (sm & KMOD_RSHIFT) mods |= RD_MOD_RSHIFT;
                if (sm & KMOD_RALT) mods |= RD_MOD_RALT;
                if (sm & KMOD_RGUI) mods |= RD_MOD_RMETA;
                backend.send_key(static_cast<uint16_t>(ev.key.keysym.scancode), ev.type == SDL_KEYDOWN, mods);
            }
            break;
        }
        case SDL_CONTROLLERBUTTONDOWN: {
            // Note on Switch 2 SDL_GameController layout (Xbox positional):
            //   Physical Y (Left)   = SDL_CONTROLLER_BUTTON_X -> Backspace (handled in update_frame)
            //   Physical X (Top)    = SDL_CONTROLLER_BUTTON_Y -> Manual keyboard shortcut
            //   Physical B (Bottom) = SDL_CONTROLLER_BUTTON_A -> Left click / 2nd press on input box opens keyboard
            //   Physical A (Right)  = SDL_CONTROLLER_BUTTON_B -> Right click
            const auto kbd_btn = platform::is_switch_hardware()
                                     ? SDL_CONTROLLER_BUTTON_Y
                                     : SDL_CONTROLLER_BUTTON_X;
            if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) toggle_touch_mode();
            else if (ev.cbutton.button == kbd_btn) out_open_swkbd = true;
            else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP) backend.send_key(82, true, 0);
            else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) backend.send_key(81, true, 0);
            else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) backend.send_key(80, true, 0);
            else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) backend.send_key(79, true, 0);
            break;
        }
        case SDL_CONTROLLERBUTTONUP: {
            if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP) backend.send_key(82, false, 0);
            else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) backend.send_key(81, false, 0);
            else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) backend.send_key(80, false, 0);
            else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) backend.send_key(79, false, 0);
            break;
        }
        default:
            break;
    }
    return false;
}

void InputMapper::update_frame(float dt_sec, SDL_GameController* pad, RemoteBackend& backend) {
    platform::PhysicalMouseDelta pm{};
    std::vector<platform::PhysicalKeyTransition> pkeys;
    platform::poll_physical_hid(pm, pkeys);
    if (pm.active) {
        if (pm.dx != 0 || pm.dy != 0) {
            vp_.cursor_x = clampf(vp_.cursor_x + pm.dx * sensitivity_ / 1920.0f, 0.0f, 1.0f);
            vp_.cursor_y = clampf(vp_.cursor_y + pm.dy * sensitivity_ / 1080.0f, 0.0f, 1.0f);
            send_cursor(backend);
        }
        if (pm.wheel_x != 0 || pm.wheel_y != 0) {
            backend.send_mouse_wheel(static_cast<int16_t>(pm.wheel_x), static_cast<int16_t>(pm.wheel_y));
        }
        uint32_t diff = pm.buttons ^ prev_phys_mouse_btns_;
        if (diff) {
            static constexpr MouseBtn map[5] = {
                MouseBtn::LEFT, MouseBtn::RIGHT, MouseBtn::MIDDLE, MouseBtn::BACK, MouseBtn::FORWARD
            };
            for (int i = 0; i < 5; ++i) {
                if (diff & (1u << i)) backend.send_mouse_button(map[i], (pm.buttons & (1u << i)) != 0);
            }
            prev_phys_mouse_btns_ = pm.buttons;
        }
    }
    for (const auto& k : pkeys) backend.send_key(k.hid_usage, k.pressed, k.modifiers);

    if (!pad) return;

    float lx = apply_stick_curve(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX));
    float ly = apply_stick_curve(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY));
    bool sniper = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
    float base_speed = sniper ? 0.22f : 0.92f;
    if (lx != 0.0f || ly != 0.0f) {
        vp_.cursor_x += lx * base_speed * sensitivity_ * dt_sec;
        vp_.cursor_y += ly * base_speed * sensitivity_ * dt_sec;
        send_cursor(backend);
    }

    float rx = apply_stick_curve(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX));
    float ry = apply_stick_curve(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY));
    if (rx != 0.0f || ry != 0.0f) {
        int16_t wx = static_cast<int16_t>(rx * 220.0f * dt_sec * 10.0f);
        int16_t wy = static_cast<int16_t>(-ry * 260.0f * dt_sec * 10.0f);
        if (wx != 0 || wy != 0) backend.send_mouse_wheel(wx, wy);
    }

    const auto bs_btn = platform::is_switch_hardware()
                            ? SDL_CONTROLLER_BUTTON_X   // Physical Y (Left) on Switch 2!
                            : SDL_CONTROLLER_BUTTON_Y;
    bool a_now = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) != 0;
    bool b_now = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B) != 0;
    bool bs_now = SDL_GameControllerGetButton(pad, bs_btn) != 0;
    bool zr_now = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000;
    bool zl_now = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000;

    if ((a_now || zr_now) != (pad_a_down_ || pad_zr_down_)) {
        backend.send_mouse_button(MouseBtn::LEFT, a_now || zr_now);
    }
    if ((b_now || zl_now) != (pad_b_down_ || pad_zl_down_)) {
        backend.send_mouse_button(MouseBtn::RIGHT, b_now || zl_now);
    }
    if (bs_now != pad_bs_down_) {
        // Physical Y on Switch 2 = Backspace (HID 42)
        backend.send_key(42, bs_now, 0);
        if (bs_now) {
            pad_bs_next_repeat_us_ = platform::now_us() + 420000ULL;
        }
    } else if (bs_now && platform::now_us() >= pad_bs_next_repeat_us_) {
        // Hold Y to continuously delete characters
        backend.send_key(42, false, 0);
        backend.send_key(42, true, 0);
        pad_bs_next_repeat_us_ = platform::now_us() + 75000ULL;
    }

    pad_a_down_ = a_now; pad_b_down_ = b_now; pad_bs_down_ = bs_now;
    pad_zr_down_ = zr_now; pad_zl_down_ = zl_now;
}

void InputMapper::sync_pad_buttons(SDL_GameController* pad) {
    if (!pad) {
        pad_a_down_ = pad_b_down_ = pad_bs_down_ = pad_zr_down_ = pad_zl_down_ = false;
        return;
    }
    const auto bs_btn = platform::is_switch_hardware()
                            ? SDL_CONTROLLER_BUTTON_X
                            : SDL_CONTROLLER_BUTTON_Y;
    pad_a_down_ = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) != 0;
    pad_b_down_ = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B) != 0;
    pad_bs_down_ = SDL_GameControllerGetButton(pad, bs_btn) != 0;
    pad_zr_down_ = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000;
    pad_zl_down_ = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000;
}

void InputMapper::send_combo(RemoteBackend& backend, uint16_t mod_hid, uint8_t mod_mask, uint16_t key_hid) {
    if (mod_hid != 0) backend.send_key(mod_hid, true, mod_mask);
    backend.send_key(key_hid, true, mod_mask);
    backend.send_key(key_hid, false, mod_mask);
    if (mod_hid != 0) backend.send_key(mod_hid, false, 0);
}

void InputMapper::send_shortcut(RemoteBackend& backend, ProtocolType proto, const std::string& action) {
    const bool is_mac = (proto == ProtocolType::VNC_MACOS);
    const uint16_t primary_mod = is_mac ? 227 : 224;
    const uint8_t primary_mask = is_mac ? RD_MOD_LMETA : RD_MOD_LCTRL;

    if (action == "copy") send_combo(backend, primary_mod, primary_mask, 6);
    else if (action == "paste") send_combo(backend, primary_mod, primary_mask, 25);
    else if (action == "undo") send_combo(backend, primary_mod, primary_mask, 29);
    else if (action == "switch_app") {
        if (is_mac) send_combo(backend, 227, RD_MOD_LMETA, 43);
        else send_combo(backend, 226, RD_MOD_LALT, 43);
    } else if (action == "launcher") {
        if (is_mac) send_combo(backend, 227, RD_MOD_LMETA, 44);
        else { backend.send_key(227, true, RD_MOD_LMETA); backend.send_key(227, false, 0); }
    } else if (action == "desktop") {
        if (is_mac) send_combo(backend, 224, RD_MOD_LCTRL, 82);
        else send_combo(backend, 227, RD_MOD_LMETA, 7);
    } else if (action == "taskmgr") {
        if (is_mac) {
            backend.send_key(227, true, RD_MOD_LMETA);
            backend.send_key(226, true, RD_MOD_LMETA | RD_MOD_LALT);
            backend.send_key(41, true, RD_MOD_LMETA | RD_MOD_LALT);
            backend.send_key(41, false, RD_MOD_LMETA | RD_MOD_LALT);
            backend.send_key(226, false, RD_MOD_LMETA);
            backend.send_key(227, false, 0);
        } else {
            backend.send_key(224, true, RD_MOD_LCTRL);
            backend.send_key(225, true, RD_MOD_LCTRL | RD_MOD_LSHIFT);
            backend.send_key(41, true, RD_MOD_LCTRL | RD_MOD_LSHIFT);
            backend.send_key(41, false, RD_MOD_LCTRL | RD_MOD_LSHIFT);
            backend.send_key(225, false, RD_MOD_LCTRL);
            backend.send_key(224, false, 0);
        }
    }
}
