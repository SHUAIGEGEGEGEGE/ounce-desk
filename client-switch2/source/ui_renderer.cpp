#include "ui_renderer.hpp"

#include "font_bitmap.hpp"
#include "platform.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
constexpr SDL_Color COL_BG       = {11, 15, 25, 255};
constexpr SDL_Color COL_PANEL    = {22, 29, 44, 242};
constexpr SDL_Color COL_CARD     = {27, 36, 54, 245};
constexpr SDL_Color COL_BORDER   = {58, 74, 102, 255};
constexpr SDL_Color COL_ACCENT   = {56, 189, 248, 255};
constexpr SDL_Color COL_ACTIVE   = {37, 99, 235, 255};
constexpr SDL_Color COL_DANGER   = {220, 38, 38, 255};
constexpr SDL_Color COL_WIN      = {14, 165, 233, 255};
constexpr SDL_Color COL_MAC      = {52, 211, 153, 255};
constexpr SDL_Color COL_TEXT     = {241, 245, 249, 255};
constexpr SDL_Color COL_MUTED    = {148, 163, 184, 255};
}  // namespace

UIRenderer::~UIRenderer() { shutdown(); }

bool UIRenderer::init(SDL_Renderer* renderer) {
    shutdown();
    renderer_ = renderer;
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    build_bitmap_atlas();

#ifdef OUNCEDESK_HAS_TTF
    if (TTF_Init() == 0) {
        const void* font_ptr = nullptr;
        size_t font_size = 0;
        if (platform::get_shared_font(font_ptr, font_size)) {
            SDL_RWops* rw1 = SDL_RWFromConstMem(font_ptr, static_cast<int>(font_size));
            SDL_RWops* rw2 = SDL_RWFromConstMem(font_ptr, static_cast<int>(font_size));
            SDL_RWops* rw3 = SDL_RWFromConstMem(font_ptr, static_cast<int>(font_size));
            if (rw1) ttf_small_ = TTF_OpenFontRW(rw1, 1, 20);
            if (rw2) ttf_med_ = TTF_OpenFontRW(rw2, 1, 26);
            if (rw3) ttf_large_ = TTF_OpenFontRW(rw3, 1, 38);
        }
    }
#endif
    return true;
}

void UIRenderer::shutdown() {
    if (font_atlas_) {
        SDL_DestroyTexture(font_atlas_);
        font_atlas_ = nullptr;
    }
#ifdef OUNCEDESK_HAS_TTF
    if (ttf_small_) { TTF_CloseFont(ttf_small_); ttf_small_ = nullptr; }
    if (ttf_med_)   { TTF_CloseFont(ttf_med_);   ttf_med_ = nullptr; }
    if (ttf_large_) { TTF_CloseFont(ttf_large_); ttf_large_ = nullptr; }
    TTF_Quit();
#endif
    renderer_ = nullptr;
}

void UIRenderer::build_bitmap_atlas() {
    constexpr int W = 96 * 8;
    constexpr int H = 8;
    std::vector<uint32_t> pixels(W * H, 0);
    for (int g = 0; g < 96; ++g) {
        for (int row = 0; row < 8; ++row) {
            uint8_t bits = bitmap_font::GLYPHS[g][row];
            for (int col = 0; col < 8; ++col) {
                if (bits & (0x80 >> col)) {
                    pixels[row * W + g * 8 + col] = 0xFFFFFFFFu;
                }
            }
        }
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    font_atlas_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, W, H);
    if (font_atlas_) {
        SDL_UpdateTexture(font_atlas_, nullptr, pixels.data(), W * 4);
        SDL_SetTextureBlendMode(font_atlas_, SDL_BLENDMODE_BLEND);
    }
}

void UIRenderer::draw_text(int x, int y, const std::string& text, SDL_Color color, int scale) {
    if (!renderer_ || text.empty()) return;

#ifdef OUNCEDESK_HAS_TTF
    TTF_Font* f = (scale >= 4) ? ttf_large_ : (scale >= 3 ? ttf_med_ : ttf_small_);
    if (f) {
        SDL_Surface* surf = TTF_RenderUTF8_Blended(f, text.c_str(), color);
        if (surf) {
            SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer_, surf);
            if (tex) {
                SDL_Rect dst{x, y, surf->w, surf->h};
                SDL_RenderCopy(renderer_, tex, nullptr, &dst);
                SDL_DestroyTexture(tex);
            }
            SDL_FreeSurface(surf);
            return;
        }
    }
#endif

    if (!font_atlas_) return;
    SDL_SetTextureColorMod(font_atlas_, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(font_atlas_, color.a);
    int cx = x;
    int gw = 8 * scale;
    int gh = 8 * scale;
    for (unsigned char ch : text) {
        int idx = (ch >= 32 && ch <= 126) ? (ch - 32) : ('?' - 32);
        SDL_Rect src{idx * 8, 0, 8, 8};
        SDL_Rect dst{cx, y, gw, gh};
        SDL_RenderCopy(renderer_, font_atlas_, &src, &dst);
        cx += gw;
    }
}

void UIRenderer::draw_panel(const SDL_Rect& r, SDL_Color fill, SDL_Color border) {
    SDL_SetRenderDrawColor(renderer_, fill.r, fill.g, fill.b, fill.a);
    SDL_RenderFillRect(renderer_, &r);
    SDL_SetRenderDrawColor(renderer_, border.r, border.g, border.b, border.a);
    SDL_RenderDrawRect(renderer_, &r);
}

void UIRenderer::draw_button(const UIButton& btn, bool focused) {
    SDL_Color fill = COL_CARD;
    SDL_Color border = COL_BORDER;
    if (btn.danger) {
        fill = {127, 29, 29, 240};
        border = COL_DANGER;
    } else if (btn.active) {
        fill = COL_ACTIVE;
        border = COL_ACCENT;
    } else if (btn.accent) {
        fill = {14, 116, 144, 240};
        border = COL_ACCENT;
    }
    if (focused) {
        border = COL_ACCENT;
        fill.r = static_cast<Uint8>(std::min(255, fill.r + 24));
        fill.g = static_cast<Uint8>(std::min(255, fill.g + 24));
        fill.b = static_cast<Uint8>(std::min(255, fill.b + 30));
    }

    draw_panel(btn.rect, fill, border);
    if (focused) {
        SDL_Rect outer{btn.rect.x - 2, btn.rect.y - 2, btn.rect.w + 4, btn.rect.h + 4};
        SDL_SetRenderDrawColor(renderer_, COL_ACCENT.r, COL_ACCENT.g, COL_ACCENT.b, 255);
        SDL_RenderDrawRect(renderer_, &outer);
    }

    int scale = 2;
    int tw = static_cast<int>(btn.label.size()) * 8 * scale;
    int tx = btn.rect.x + std::max(10, (btn.rect.w - tw) / 2);
    int ty = btn.rect.y + std::max(6, (btn.rect.h - 8 * scale) / 2);
    draw_text(tx, ty, btn.label, COL_TEXT, scale);
}

std::vector<UIButton> UIRenderer::render_profile_browser(
    int win_w, int win_h,
    const std::vector<ConnectionProfile>& profiles,
    int selected_idx,
    const std::string& status_banner) {

    std::vector<UIButton> buttons;

    SDL_SetRenderDrawColor(renderer_, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(renderer_);

    // Header bar
    SDL_Rect header{0, 0, win_w, 108};
    draw_panel(header, COL_PANEL, COL_BORDER);
    draw_text(44, 24, "OunceDesk", COL_ACCENT, 4);
    draw_text(364, 42, "Windows RDP (3389)  +  macOS Screen Sharing (5900)", COL_MUTED, 2);

    UIButton b_add_rdp{{win_w - 760, 26, 250, 56}, "add_rdp", "+ Windows (3389)", true, false, false};
    UIButton b_add_mac{{win_w - 494, 26, 240, 56}, "add_mac", "+ macOS (5900)", false, true, false};
    UIButton b_scan{{win_w - 238, 26, 194, 56}, "scan_mdns", "Scan Macs", false, false, false};
    draw_button(b_add_rdp, false);
    draw_button(b_add_mac, false);
    draw_button(b_scan, false);
    buttons.push_back(b_add_rdp);
    buttons.push_back(b_add_mac);
    buttons.push_back(b_scan);

    if (!status_banner.empty()) {
        SDL_Rect banner{44, 126, win_w - 88, 48};
        draw_panel(banner, {30, 41, 59, 235}, COL_ACCENT);
        draw_text(64, 142, status_banner, COL_TEXT, 2);
    }

    int grid_top = status_banner.empty() ? 148 : 196;
    if (profiles.empty()) {
        SDL_Rect empty_box{win_w / 2 - 480, win_h / 2 - 140, 960, 280};
        draw_panel(empty_box, COL_PANEL, COL_BORDER);
        draw_text(empty_box.x + 120, empty_box.y + 44, "No saved Windows RDP or macOS connections yet", COL_TEXT, 2);
        draw_text(empty_box.x + 76, empty_box.y + 98, "Windows: Enable Remote Desktop (3389) -> Tap '+ Windows (3389)'", COL_WIN, 2);
        draw_text(empty_box.x + 76, empty_box.y + 144, "macOS  : Enable Screen Sharing (5900) -> Auto-found via Bonjour", COL_MAC, 2);
        draw_text(empty_box.x + 180, empty_box.y + 206, "No third-party software needed on PC or Mac!", COL_ACCENT, 2);
    } else {
        const int cols = 2;
        const int pad = 32;
        const int card_w = (win_w - 88 - pad * (cols - 1)) / cols;
        const int card_h = 190;

        for (size_t i = 0; i < profiles.size(); ++i) {
            int row = static_cast<int>(i) / cols;
            int col = static_cast<int>(i) % cols;
            int cx = 44 + col * (card_w + pad);
            int cy = grid_top + row * (card_h + 26);
            if (cy + card_h > win_h - 86) break;

            const auto& p = profiles[i];
            bool focused = (static_cast<int>(i) == selected_idx);
            SDL_Rect card{cx, cy, card_w, card_h};
            draw_panel(card, focused ? SDL_Color{34, 48, 74, 255} : COL_CARD, focused ? COL_ACCENT : COL_BORDER);
            if (focused) {
                SDL_Rect glow{cx - 2, cy - 2, card_w + 4, card_h + 4};
                SDL_SetRenderDrawColor(renderer_, COL_ACCENT.r, COL_ACCENT.g, COL_ACCENT.b, 255);
                SDL_RenderDrawRect(renderer_, &glow);
            }

            bool is_mac = (p.protocol == ProtocolType::VNC_MACOS);
            SDL_Color bcol = is_mac ? COL_MAC : COL_WIN;
            SDL_Rect badge{cx + 22, cy + 22, 168, 36};
            draw_panel(badge, bcol, bcol);
            draw_text(badge.x + 12, badge.y + 10, is_mac ? "macOS 5900" : "WIN RDP 3389", {10, 15, 25, 255}, 2);

            draw_text(cx + 206, cy + 26, p.name, COL_TEXT, 3);

            char info[160];
            std::snprintf(info, sizeof(info), "%s:%u   |   User: %s",
                          p.host.c_str(), p.port, p.username.empty() ? "(prompt)" : p.username.c_str());
            draw_text(cx + 22, cy + 84, info, COL_MUTED, 2);

            std::string sub = is_mac
                                  ? (p.from_mdns ? "[Bonjour Discovered] RFB 3.8 + Apple ARD / VNC Auth"
                                                 : "[Saved] RFB 3.8 + Apple ARD / VNC Auth")
                                  : "[Saved] Native MS-RDP (TLS + CredSSP NLA) " +
                                        std::to_string(p.req_width) + "x" + std::to_string(p.req_height);
            draw_text(cx + 22, cy + 132, sub, COL_ACCENT, 2);

            buttons.push_back({card, "profile:" + std::to_string(i), "", false, false, false});
        }
    }

    SDL_Rect footer{0, win_h - 68, win_w, 68};
    draw_panel(footer, COL_PANEL, COL_BORDER);
    draw_text(44, win_h - 44,
              "[A] Connect / Login   [Y] + Windows RDP (3389)   [X] + macOS (5900)   [-] Delete   [+] Exit",
              COL_MUTED, 2);

    return buttons;
}

std::vector<UIButton> UIRenderer::render_connect_form(
    int win_w, int win_h,
    const ConnectionProfile& draft,
    int focused_field,
    const std::string& error_text) {

    std::vector<UIButton> buttons;

    SDL_SetRenderDrawColor(renderer_, 5, 8, 16, 210);
    SDL_Rect full{0, 0, win_w, win_h};
    SDL_RenderFillRect(renderer_, &full);

    int mw = 920;
    int mh = 760;
    SDL_Rect modal{(win_w - mw) / 2, (win_h - mh) / 2, mw, mh};
    draw_panel(modal, COL_PANEL, COL_ACCENT);

    bool is_mac = (draft.protocol == ProtocolType::VNC_MACOS);
    draw_text(modal.x + 36, modal.y + 26,
              is_mac ? "Connect to macOS Remote Desktop (VNC / ARD 5900)"
                     : "Connect to Windows Remote Desktop (RDP 3389)",
              COL_TEXT, 2);

    struct Row { const char* id; const char* title; std::string val; };
    std::vector<Row> rows = {
        {"f:proto", "Protocol", is_mac ? "macOS Screen Sharing / VNC (Port 5900)" : "Windows Remote Desktop / RDP (Port 3389)"},
        {"f:host",  "Host / IP", draft.host.empty() ? "(tap to enter IP, e.g. 192.168.1.50)" : draft.host},
        {"f:port",  "TCP Port",  std::to_string(draft.port)},
        {"f:user",  is_mac ? "macOS User (ARD)" : "Windows Username",
                    draft.username.empty() ? (is_mac ? "(optional for VNC password)" : "(required for Windows NLA)") : draft.username},
        {"f:pass",  "Password", draft.password.empty() ? "(tap to enter password)" : std::string(draft.password.size(), '*')},
        {"f:res",   is_mac ? "Display Name" : "RDP Resolution",
                    is_mac ? (draft.name.empty() ? "My Mac" : draft.name)
                           : (std::to_string(draft.req_width) + "x" + std::to_string(draft.req_height) + " (tap to toggle)")},
    };

    int y = modal.y + 78;
    for (size_t i = 0; i < rows.size(); ++i) {
        bool foc = (static_cast<int>(i) == focused_field);
        SDL_Rect r{modal.x + 36, y, mw - 72, 74};
        draw_panel(r, foc ? SDL_Color{34, 48, 74, 255} : COL_BG, foc ? COL_ACCENT : COL_BORDER);
        draw_text(r.x + 18, r.y + 10, rows[i].title, COL_MUTED, 2);
        draw_text(r.x + 18, r.y + 38, rows[i].val, COL_TEXT, 2);
        buttons.push_back({r, rows[i].id, "", false, false, false});
        y += 88;
    }

    if (!error_text.empty()) {
        draw_text(modal.x + 36, y + 4, error_text, COL_DANGER, 2);
    }

    int by = modal.y + mh - 88;
    UIButton b_cancel{{modal.x + 36, by, 240, 60}, "form_cancel", "Cancel", false, false, true};
    UIButton b_save{{modal.x + 300, by, 260, 60}, "form_save", "Save Only", false, false, false};
    UIButton b_go{{modal.x + 584, by, 300, 60}, "form_connect", "Save & Connect", true, false, false};
    draw_button(b_cancel, focused_field == 6);
    draw_button(b_save, focused_field == 7);
    draw_button(b_go, focused_field == 8);
    buttons.push_back(b_cancel);
    buttons.push_back(b_save);
    buttons.push_back(b_go);

    return buttons;
}

SDL_Rect UIRenderer::render_menu_pill(int /*win_w*/, int /*win_h*/) {
    SDL_Rect pill{16, 16, 118, 40};
    draw_panel(pill, {15, 23, 42, 175}, {56, 189, 248, 190});
    draw_text(pill.x + 18, pill.y + 12, "MENU", COL_TEXT, 2);
    return pill;
}

void UIRenderer::render_cursor(const ViewportState& vp) {
    int cx = vp.dst_rect.x + static_cast<int>(vp.cursor_x * vp.dst_rect.w);
    int cy = vp.dst_rect.y + static_cast<int>(vp.cursor_y * vp.dst_rect.h);
    for (int i = 0; i < 18; ++i) {
        int w = (i < 13) ? (i + 2) : (18 - i);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 235);
        SDL_RenderDrawLine(renderer_, cx - 1, cy + i, cx + w + 1, cy + i);
        if (i >= 1 && i <= 15) {
            SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
            SDL_RenderDrawLine(renderer_, cx + 1, cy + i, cx + std::max(1, w - 1), cy + i);
        }
    }
}

void UIRenderer::render_perf_hud(int win_w, int /*win_h*/, const BackendStats& st, TouchMode tmode, float zoom) {
    char line1[160];
    char line2[160];
    std::snprintf(line1, sizeof(line1), "%s | %dx%d | %.1f Mbps",
                  st.protocol_label.c_str(), st.fb_width, st.fb_height, st.rx_mbps);
    std::snprintf(line2, sizeof(line2), "Updates %.0f/s | %s | Zoom %.1fx",
                  st.fps, (tmode == TouchMode::TRACKPAD) ? "Trackpad" : "Direct", zoom);

    SDL_Rect box{win_w - 600, 14, 584, 62};
    draw_panel(box, {10, 15, 26, 195}, {56, 189, 248, 160});
    draw_text(box.x + 14, box.y + 10, line1, COL_ACCENT, 2);
    draw_text(box.x + 14, box.y + 36, line2, COL_TEXT, 2);
}

std::vector<UIButton> UIRenderer::render_quick_overlay(
    int win_w, int win_h,
    const ConnectionProfile& active_profile,
    const BackendStats& st,
    TouchMode tmode,
    bool show_hud,
    int focused_idx) {

    std::vector<UIButton> buttons;

    SDL_SetRenderDrawColor(renderer_, 5, 8, 16, 175);
    SDL_Rect full{0, 0, win_w, win_h};
    SDL_RenderFillRect(renderer_, &full);

    int pw = std::min(win_w - 120, 1440);
    int ph = 460;
    SDL_Rect panel{(win_w - pw) / 2, (win_h - ph) / 2, pw, ph};
    draw_panel(panel, COL_PANEL, COL_ACCENT);

    bool is_mac = (active_profile.protocol == ProtocolType::VNC_MACOS);
    std::string hdr = active_profile.name + "  (" + st.protocol_label + ")";
    draw_text(panel.x + 32, panel.y + 24, hdr, COL_TEXT, 3);

    draw_text(panel.x + 32, panel.y + 82, is_mac ? "macOS Shortcuts:" : "Windows Shortcuts:", COL_MUTED, 2);
    struct Sc { const char* id; const char* win_lbl; const char* mac_lbl; };
    const Sc shortcuts[7] = {
        {"sc:launcher",   "Win",      "Cmd+Spc"},
        {"sc:switch_app", "Alt+Tab",  "Cmd+Tab"},
        {"sc:copy",       "Ctrl+C",   "Cmd+C"},
        {"sc:paste",      "Ctrl+V",   "Cmd+V"},
        {"sc:undo",       "Ctrl+Z",   "Cmd+Z"},
        {"sc:desktop",    "Win+D",    "Mission"},
        {"sc:taskmgr",    "TaskMgr",  "ForceQt"},
    };
    int sw = (pw - 64 - 6 * 12) / 7;
    for (int i = 0; i < 7; ++i) {
        buttons.push_back({{panel.x + 32 + i * (sw + 12), panel.y + 116, sw, 58},
                           shortcuts[i].id, is_mac ? shortcuts[i].mac_lbl : shortcuts[i].win_lbl, false, false, false});
    }

    int qw = (pw - 64 - 3 * 16) / 4;
    buttons.push_back({{panel.x + 32 + 0 * (qw + 16), panel.y + 210, qw, 60}, "act:touch_mode",
                       (tmode == TouchMode::TRACKPAD) ? "Touch: Trackpad" : "Touch: Direct", false, true, false});
    buttons.push_back({{panel.x + 32 + 1 * (qw + 16), panel.y + 210, qw, 60}, "act:refresh",
                       "Force Refresh", false, false, false});
    buttons.push_back({{panel.x + 32 + 2 * (qw + 16), panel.y + 210, qw, 60}, "act:hud",
                       show_hud ? "Perf HUD: On" : "Perf HUD: Off", false, show_hud, false});
    buttons.push_back({{panel.x + 32 + 3 * (qw + 16), panel.y + 210, qw, 60}, "act:reset_zoom",
                       "Reset Zoom 1.0x", false, false, false});

    int bw = (pw - 64 - 3 * 16) / 4;
    buttons.push_back({{panel.x + 32 + 0 * (bw + 16), panel.y + 306, bw, 66}, "act:osk", "PC Keyboard", true, false, false});
    buttons.push_back({{panel.x + 32 + 1 * (bw + 16), panel.y + 306, bw, 66}, "act:swkbd", "Type Text (IME)", true, false, false});
    buttons.push_back({{panel.x + 32 + 2 * (bw + 16), panel.y + 306, bw, 66}, "act:resume", "Resume", false, true, false});
    buttons.push_back({{panel.x + 32 + 3 * (bw + 16), panel.y + 306, bw, 66}, "act:disconnect", "Disconnect", false, false, true});

    for (size_t i = 0; i < buttons.size(); ++i) {
        draw_button(buttons[i], static_cast<int>(i) == focused_idx);
    }

    draw_text(panel.x + 32, panel.y + ph - 44,
              "L+R+Plus or 3-finger tap opens menu | Hold L + Left Stick = Precision Sniper Cursor",
              COL_MUTED, 2);
    return buttons;
}

std::vector<OSKKey> UIRenderer::render_osk(int win_w, int win_h, ProtocolType proto, uint8_t sticky_mods, int focused_key) {
    std::vector<OSKKey> keys;
    const bool is_mac = (proto == ProtocolType::VNC_MACOS);

    int kh_total = 390;
    SDL_Rect bg{24, win_h - kh_total - 16, win_w - 48, kh_total};
    draw_panel(bg, {15, 23, 42, 238}, COL_ACCENT);

    struct Def { const char* lbl; uint16_t hid; float weight; bool is_mod; uint8_t mod_bit; };
    const std::vector<std::vector<Def>> rows = {
        {{"Esc",41,1.1f,false,0},{"1",30,1,false,0},{"2",31,1,false,0},{"3",32,1,false,0},{"4",33,1,false,0},
         {"5",34,1,false,0},{"6",35,1,false,0},{"7",36,1,false,0},{"8",37,1,false,0},{"9",38,1,false,0},
         {"0",39,1,false,0},{"-",45,1,false,0},{"=",46,1,false,0},{"Bksp",42,1.8f,false,0}},
        {{"Tab",43,1.4f,false,0},{"Q",20,1,false,0},{"W",26,1,false,0},{"E",8,1,false,0},{"R",21,1,false,0},
         {"T",23,1,false,0},{"Y",28,1,false,0},{"U",24,1,false,0},{"I",12,1,false,0},{"O",18,1,false,0},
         {"P",19,1,false,0},{"[",47,1,false,0},{"]",48,1,false,0},{"\\",49,1.4f,false,0}},
        {{"Ctrl",224,1.6f,true,RD_MOD_LCTRL},{"A",4,1,false,0},{"S",22,1,false,0},{"D",7,1,false,0},{"F",9,1,false,0},
         {"G",10,1,false,0},{"H",11,1,false,0},{"J",13,1,false,0},{"K",14,1,false,0},{"L",15,1,false,0},
         {";",51,1,false,0},{"'",52,1,false,0},{"Enter",40,2.2f,false,0}},
        {{"Shift",225,2.0f,true,RD_MOD_LSHIFT},{"Z",29,1,false,0},{"X",27,1,false,0},{"C",6,1,false,0},{"V",25,1,false,0},
         {"B",5,1,false,0},{"N",17,1,false,0},{"M",16,1,false,0},{",",54,1,false,0},{".",55,1,false,0},
         {"/",56,1,false,0},{"Up",82,1.2f,false,0},{"Del",76,1.6f,false,0}},
        {{is_mac ? "Cmd" : "Win",227,1.6f,true,RD_MOD_LMETA},
         {is_mac ? "Opt" : "Alt",226,1.5f,true,RD_MOD_LALT},
         {"Space",44,5.5f,false,0},
         {"Left",80,1.2f,false,0},{"Down",81,1.2f,false,0},{"Right",79,1.2f,false,0},
         {"Close",0,1.8f,false,0}}
    };

    int pad_x = 12, pad_y = 10;
    int inner_w = bg.w - 2 * pad_x;
    int row_h = (bg.h - 2 * pad_y - 4 * 8) / 5;

    int key_idx = 0;
    for (size_t r = 0; r < rows.size(); ++r) {
        float total_w = 0.0f;
        for (const auto& d : rows[r]) total_w += d.weight;
        int gaps = static_cast<int>(rows[r].size() - 1) * 8;
        float unit_w = static_cast<float>(inner_w - gaps) / total_w;

        float x = static_cast<float>(bg.x + pad_x);
        int y = bg.y + pad_y + static_cast<int>(r) * (row_h + 8);
        for (const auto& d : rows[r]) {
            int kw = static_cast<int>(d.weight * unit_w);
            OSKKey k{{static_cast<int>(x), y, kw, row_h}, d.lbl, d.hid, d.is_mod, d.mod_bit};
            bool active = d.is_mod && ((sticky_mods & d.mod_bit) != 0);
            UIButton b{k.rect, "", k.label, d.hid == 40, active, d.hid == 0};
            draw_button(b, key_idx == focused_key);
            keys.push_back(k);
            x += kw + 8;
            ++key_idx;
        }
    }
    return keys;
}
