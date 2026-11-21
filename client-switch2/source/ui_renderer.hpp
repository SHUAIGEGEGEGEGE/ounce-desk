#pragma once

#include "backend.hpp"
#include "input_mapper.hpp"

#include <SDL2/SDL.h>
#include <string>
#include <vector>

#ifdef OUNCEDESK_HAS_TTF
#include <SDL2/SDL_ttf.h>
#endif

struct UIButton {
    SDL_Rect rect{0, 0, 0, 0};
    std::string id;
    std::string label;
    bool accent = false;
    bool active = false;
    bool danger = false;
};

struct OSKKey {
    SDL_Rect rect{0, 0, 0, 0};
    std::string label;
    uint16_t hid = 0;
    bool is_modifier = false;
    uint8_t mod_bit = 0;
};

class UIRenderer {
public:
    UIRenderer() = default;
    ~UIRenderer();

    bool init(SDL_Renderer* renderer);
    void shutdown();

    void draw_text(int x, int y, const std::string& text, SDL_Color color, int scale = 2);
    void draw_panel(const SDL_Rect& r, SDL_Color fill, SDL_Color border);
    void draw_button(const UIButton& btn, bool focused = false);

    // 1. Saved RDP (3389) & macOS VNC (5900) Connection Manager
    std::vector<UIButton> render_profile_browser(
        int win_w, int win_h,
        const std::vector<ConnectionProfile>& profiles,
        int selected_idx,
        const std::string& status_banner);

    // 2. Add / Edit / Login Connection Form Modal
    std::vector<UIButton> render_connect_form(
        int win_w, int win_h,
        const ConnectionProfile& draft,
        int focused_field,
        const std::string& error_text);

    // 3. Streaming Overlays
    SDL_Rect render_menu_pill(int win_w, int win_h);
    void render_cursor(const ViewportState& vp);
    void render_perf_hud(int win_w, int win_h, const BackendStats& st, TouchMode tmode, float zoom);

    // 4. Floating Quick Overlay Menu (Windows RDP / macOS VNC adaptive shortcuts)
    std::vector<UIButton> render_quick_overlay(
        int win_w, int win_h,
        const ConnectionProfile& active_profile,
        const BackendStats& st,
        TouchMode tmode,
        bool show_hud,
        int focused_idx);

    // 5. Built-in Full On-Screen PC Keyboard (also used for Desktop form typing)
    std::vector<OSKKey> render_osk(int win_w, int win_h, ProtocolType proto, uint8_t sticky_mods, int focused_key);

    static bool hit_test(const SDL_Rect& r, int x, int y) {
        return x >= r.x && x < (r.x + r.w) && y >= r.y && y < (r.y + r.h);
    }

private:
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* font_atlas_ = nullptr;

#ifdef OUNCEDESK_HAS_TTF
    TTF_Font* ttf_small_ = nullptr;
    TTF_Font* ttf_med_ = nullptr;
    TTF_Font* ttf_large_ = nullptr;
#endif

    void build_bitmap_atlas();
};
