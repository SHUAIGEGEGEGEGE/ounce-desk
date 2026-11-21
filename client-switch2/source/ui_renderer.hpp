#pragma once

#include "backend.hpp"
#include "input_mapper.hpp"

#include <SDL2/SDL.h>
#include <string>
#include <unordered_map>
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

    // 1. Two-Column Home Screen: Windows (RDP 3389) vs macOS (Screen Sharing 5900)
    std::vector<UIButton> render_profile_browser(
        int win_w, int win_h,
        const std::vector<ConnectionProfile>& win_profiles,
        const std::vector<ConnectionProfile>& mac_profiles,
        int selected_col,   // 0 = Windows 3389, 1 = macOS 5900
        int selected_row,
        bool scanning,
        int scan_pct,
        TouchMode tmode,
        const std::string& status_banner,
        const std::string& wg_badge = "",
        bool wg_active = false);

    // 2. Home '+' Menu (Touch/Mouse mode toggle, Rescan, Quick Add, WireGuard, Exit App)
    std::vector<UIButton> render_home_menu(int win_w, int win_h, TouchMode tmode, float sensitivity, const std::string& wg_summary, int focused_idx);

    // 2b. Saved device manager (delete saved Windows / macOS devices)
    std::vector<UIButton> render_device_manager(int win_w, int win_h, const std::vector<ConnectionProfile>& saved, int focused_idx);

    // 2c. WireGuard userspace VPN manager (select .conf, connect/disconnect, live rx/tx)
    std::vector<UIButton> render_wg_manager(
        int win_w, int win_h,
        const std::vector<std::string>& configs,
        const std::string& active_name,
        const std::string& status_line,
        bool is_up,
        uint64_t rx_bytes,
        uint64_t tx_bytes,
        int focused_idx);

    // Mouse sensitivity slider (ids: sens:dec, sens:track, sens:inc). Returns track rect via out_track.
    void render_sens_slider(int x, int y, int w, float sensitivity, std::vector<UIButton>& out);
    static float sens_from_track_x(const SDL_Rect& track, int x);

    // 3. Add / Edit / Login Connection Form Modal (Touch + Switch 2 Native Keyboard)
    std::vector<UIButton> render_connect_form(
        int win_w, int win_h,
        const ConnectionProfile& draft,
        int focused_field,
        const std::string& error_text);

    // 4. Streaming Overlays
    SDL_Rect render_menu_pill(int win_w, int win_h);
    void render_cursor(const ViewportState& vp);
    void render_perf_hud(int win_w, int win_h, const BackendStats& st, TouchMode tmode, float zoom);

    // 5. Streaming '+' Menu (Touch/Mouse switch, Native swkbd, Shortcuts, Disconnect, Exit App)
    std::vector<UIButton> render_quick_overlay(
        int win_w, int win_h,
        const ConnectionProfile& active_profile,
        const BackendStats& st,
        TouchMode tmode,
        bool show_hud,
        bool auto_ime,
        float sensitivity,
        int focused_idx);

    // 6. Built-in PC Special Keys Keyboard (for Ctrl/Alt/Cmd/F-keys/Arrows)
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

    struct CachedText {
        SDL_Texture* tex = nullptr;
        int w = 0;
        int h = 0;
    };
    std::unordered_map<std::string, CachedText> text_cache_;
    void clear_text_cache();
#endif

    void build_bitmap_atlas();
};
