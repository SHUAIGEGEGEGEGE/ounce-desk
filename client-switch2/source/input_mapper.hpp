#pragma once

#include "backend.hpp"
#include "platform.hpp"

#include <SDL2/SDL.h>
#include <cstdint>
#include <map>
#include <string>

enum class TouchMode {
    TOUCH,  // Direct 1:1 touch mode (tap & drag directly where finger touches)
    MOUSE,  // Mouse / trackpad mode (1-finger trackpad + cursor arrow + stick mouse)
};

struct ViewportState {
    float zoom = 1.0f;
    float pan_x = 0.5f;
    float pan_y = 0.5f;
    float cursor_x = 0.5f;
    float cursor_y = 0.5f;
    SDL_Rect dst_rect{0, 0, 1920, 1080};
};

class InputMapper {
public:
    InputMapper() = default;

    void set_touch_mode(TouchMode mode) {
        touch_mode_ = mode;
        fingers_.clear();
        direct_dragging_ = false;
        two_finger_gesture_ = false;
    }
    TouchMode touch_mode() const { return touch_mode_; }
    bool show_local_cursor() const { return touch_mode_ == TouchMode::MOUSE || hw_mouse_visible_; }
    void toggle_touch_mode() {
        set_touch_mode((touch_mode_ == TouchMode::TOUCH) ? TouchMode::MOUSE : TouchMode::TOUCH);
        hw_mouse_visible_ = (touch_mode_ == TouchMode::MOUSE);
    }

    // Mouse sensitivity multiplier (0.2x .. 3.0x), applied to trackpad, stick and USB mouse movement
    void set_sensitivity(float s) { sensitivity_ = (s < 0.2f) ? 0.2f : (s > 3.0f ? 3.0f : s); }
    float sensitivity() const { return sensitivity_; }

    ViewportState& viewport() { return vp_; }
    const ViewportState& viewport() const { return vp_; }

    void reset_zoom() {
        vp_.zoom = 1.0f;
        vp_.pan_x = 0.5f;
        vp_.pan_y = 0.5f;
    }

    void update_viewport_rect(int win_w, int win_h, int fb_w, int fb_h);
    bool screen_to_norm(float sx, float sy, float& out_nx, float& out_ny) const;

    // Exact 1:1 logical pixel (0..1920, 0..1080) touch handlers (used by native Switch 2 hidGetTouchScreenStates & Desktop SDL)
    // Returns true if 3-finger gesture requests opening the Menu.
    bool on_touch_down(uint64_t finger_id, float sx, float sy, RemoteBackend& backend, bool suppress_direct_click = false);
    void on_touch_move(uint64_t finger_id, float sx, float sy, RemoteBackend& backend, int win_w, int win_h);
    // Returns true iff releasing this finger completed a single-finger primary tap.
    bool on_touch_up(uint64_t finger_id, RemoteBackend& backend, bool suppress_tap_click = false);

    // Handle SDL events while in active STREAMING state
    bool handle_sdl_event(const SDL_Event& ev, RemoteBackend& backend, int win_w, int win_h, bool& out_open_swkbd);

    // Returns true if physical/Joy-Con mouse clicked the top-left Menu pill.
    bool update_frame(float dt_sec, SDL_GameController* pad, RemoteBackend& backend, const SDL_Rect& menu_pill);
    void sync_pad_buttons(SDL_GameController* pad);
    void send_shortcut(RemoteBackend& backend, ProtocolType proto, const std::string& action);

private:
    TouchMode touch_mode_ = TouchMode::TOUCH;
    bool hw_mouse_visible_ = false;
    float sensitivity_ = 1.0f;
    ViewportState vp_;

    struct FingerInfo {
        float x = 0.0f, y = 0.0f;
        float start_x = 0.0f, start_y = 0.0f;
        uint64_t down_us = 0;
        bool moved = false;
    };
    std::map<uint64_t, FingerInfo> fingers_;
    float prev_pinch_dist_ = 0.0f;
    float prev_two_mid_x_ = 0.0f;
    float prev_two_mid_y_ = 0.0f;
    bool two_finger_gesture_ = false;
    bool direct_dragging_ = false;

    uint32_t prev_phys_mouse_btns_ = 0;
    bool pad_a_down_ = false;
    bool pad_b_down_ = false;
    bool pad_bs_down_ = false;
    uint64_t pad_bs_next_repeat_us_ = 0;
    bool pad_zr_down_ = false;
    bool pad_zl_down_ = false;

    void send_cursor(RemoteBackend& backend);
    static void send_combo(RemoteBackend& backend, uint16_t mod_hid, uint8_t mod_mask, uint16_t key_hid);
};
