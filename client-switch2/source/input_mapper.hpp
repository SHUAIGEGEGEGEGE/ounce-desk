#pragma once

#include "backend.hpp"
#include "platform.hpp"

#include <SDL2/SDL.h>
#include <cstdint>
#include <map>
#include <string>

enum class TouchMode {
    TRACKPAD,  // 1-finger drag/tap=LClick, 2-finger tap=RClick, 2-finger drag=Scroll, pinch=Zoom
    DIRECT,    // Direct touch on desktop pixel
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

    void set_touch_mode(TouchMode mode) { touch_mode_ = mode; }
    TouchMode touch_mode() const { return touch_mode_; }
    void toggle_touch_mode() {
        touch_mode_ = (touch_mode_ == TouchMode::TRACKPAD) ? TouchMode::DIRECT : TouchMode::TRACKPAD;
    }

    ViewportState& viewport() { return vp_; }
    const ViewportState& viewport() const { return vp_; }

    void reset_zoom() {
        vp_.zoom = 1.0f;
        vp_.pan_x = 0.5f;
        vp_.pan_y = 0.5f;
    }

    void update_viewport_rect(int win_w, int win_h, int fb_w, int fb_h);

    bool handle_sdl_event(const SDL_Event& ev, RemoteBackend& backend, int win_w, int win_h, bool& out_toggle_osk);
    void update_frame(float dt_sec, SDL_GameController* pad, RemoteBackend& backend);
    void send_shortcut(RemoteBackend& backend, ProtocolType proto, const std::string& action);

private:
    TouchMode touch_mode_ = TouchMode::TRACKPAD;
    ViewportState vp_;

    struct FingerInfo {
        float x = 0.0f, y = 0.0f;
        float start_x = 0.0f, start_y = 0.0f;
        uint64_t down_us = 0;
        bool moved = false;
    };
    std::map<SDL_FingerID, FingerInfo> fingers_;
    float prev_pinch_dist_ = 0.0f;
    float prev_two_mid_x_ = 0.0f;
    float prev_two_mid_y_ = 0.0f;
    bool two_finger_gesture_ = false;
    bool direct_dragging_ = false;

    uint32_t prev_phys_mouse_btns_ = 0;
    bool pad_a_down_ = false;
    bool pad_b_down_ = false;
    bool pad_y_down_ = false;
    bool pad_zr_down_ = false;
    bool pad_zl_down_ = false;

    void send_cursor(RemoteBackend& backend);
    bool screen_to_norm(float sx, float sy, float& out_nx, float& out_ny) const;
    static void send_combo(RemoteBackend& backend, uint16_t mod_hid, uint8_t mod_mask, uint16_t key_hid);
};
