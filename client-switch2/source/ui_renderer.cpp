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
    const std::vector<ConnectionProfile>& win_profiles,
    const std::vector<ConnectionProfile>& mac_profiles,
    int selected_col,
    int selected_row,
    bool scanning,
    int scan_pct,
    TouchMode tmode,
    const std::string& status_banner) {

    std::vector<UIButton> buttons;

    SDL_SetRenderDrawColor(renderer_, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(renderer_);

    // Header Bar
    SDL_Rect header{0, 0, win_w, 106};
    draw_panel(header, COL_PANEL, COL_BORDER);
    draw_text(36, 22, "OunceDesk", COL_ACCENT, 4);

    std::string mode_lbl = (tmode == TouchMode::TOUCH) ? "Mode: Touch" : "Mode: Mouse";
    std::string scan_lbl = scanning ? ("Scan " + std::to_string(scan_pct) + "%") : "Scan LAN";

    UIButton b_add_rdp{{win_w - 1140, 24, 240, 58}, "add_rdp", "+ Win (3389)", true, false, false};
    UIButton b_add_mac{{win_w - 884,  24, 240, 58}, "add_mac", "+ Mac (5900)", false, true, false};
    UIButton b_mode   {{win_w - 628,  24, 204, 58}, "home_mode", mode_lbl, false, false, false};
    UIButton b_scan   {{win_w - 408,  24, 184, 58}, "scan_lan", scan_lbl, false, false, false};
    UIButton b_menu   {{win_w - 208,  24, 172, 58}, "open_menu", "+ Menu", false, false, false};
    for (auto* b : {&b_add_rdp, &b_add_mac, &b_mode, &b_scan, &b_menu}) {
        draw_button(*b, false);
        buttons.push_back(*b);
    }

    int top_y = 126;
    if (!status_banner.empty()) {
        SDL_Rect banner{36, top_y, win_w - 72, 46};
        draw_panel(banner, {30, 41, 59, 235}, COL_ACCENT);
        draw_text(54, top_y + 13, status_banner, COL_TEXT, 2);
        top_y += 62;
    }

    // Strictly separated Two-Column Layout: Left = Windows (RDP 3389), Right = macOS (VNC 5900)
    const int col_gap = 36;
    const int col_w = (win_w - 72 - col_gap) / 2;
    const int col_h = win_h - top_y - 84;

    auto draw_column = [&](int col_idx, int x0, const char* title, const char* empty_hint,
                           SDL_Color accent_col, const std::vector<ConnectionProfile>& list, const char* id_prefix) {
        SDL_Rect col_box{x0, top_y, col_w, col_h};
        draw_panel(col_box, {16, 22, 34, 235}, (selected_col == col_idx) ? accent_col : COL_BORDER);

        SDL_Rect col_hdr{x0, top_y, col_w, 56};
        draw_panel(col_hdr, {24, 34, 52, 255}, accent_col);
        draw_text(x0 + 20, top_y + 16, std::string(title) + "  [" + std::to_string(list.size()) + "]", accent_col, 2);

        if (list.empty()) {
            draw_text(x0 + 28, top_y + 92, empty_hint, COL_MUTED, 2);
            draw_text(x0 + 28, top_y + 132,
                      col_idx == 0 ? "Tap '+ Win (3389)' or 'Scan LAN'" : "Tap '+ Mac (5900)' or 'Scan LAN'",
                      COL_ACCENT, 2);
            return;
        }

        int cy = top_y + 72;
        const int card_h = 148;
        for (size_t i = 0; i < list.size(); ++i) {
            if (cy + card_h > top_y + col_h - 12) break;
            const auto& p = list[i];
            bool focused = (selected_col == col_idx && selected_row == static_cast<int>(i));

            SDL_Rect card{x0 + 18, cy, col_w - 36, card_h};
            draw_panel(card, focused ? SDL_Color{34, 48, 74, 255} : COL_CARD, focused ? accent_col : COL_BORDER);
            if (focused) {
                SDL_Rect glow{card.x - 2, card.y - 2, card.w + 4, card.h + 4};
                SDL_SetRenderDrawColor(renderer_, accent_col.r, accent_col.g, accent_col.b, 255);
                SDL_RenderDrawRect(renderer_, &glow);
            }

            SDL_Rect badge{card.x + 16, card.y + 16, 136, 32};
            draw_panel(badge, accent_col, accent_col);
            draw_text(badge.x + 12, badge.y + 8, col_idx == 0 ? "RDP 3389" : "MAC 5900", {10, 15, 25, 255}, 2);

            draw_text(card.x + 168, card.y + 18, p.name, COL_TEXT, 2);

            char info[160];
            std::snprintf(info, sizeof(info), "%s:%u  |  User: %s",
                          p.host.c_str(), p.port, p.username.empty() ? "(input)" : p.username.c_str());
            draw_text(card.x + 16, card.y + 66, info, COL_MUTED, 2);

            std::string tag = p.from_mdns
                                  ? (col_idx == 0 ? "[LAN Auto: Port 3389 Open]" : "[LAN Auto: Port 5900 RFB Verified]")
                                  : "[Saved Profile] Tap to Connect";
            draw_text(card.x + 16, card.y + 104, tag, accent_col, 2);

            buttons.push_back({card, std::string(id_prefix) + std::to_string(i), "", false, false, false});
            cy += card_h + 16;
        }
    };

    draw_column(0, 36, "Windows PC  (RDP Port 3389)", "No Windows 3389 PCs found yet.",
                COL_WIN, win_profiles, "win:");
    draw_column(1, 36 + col_w + col_gap, "macOS  (Screen Sharing Port 5900)", "No macOS 5900 Macs found yet.",
                COL_MAC, mac_profiles, "mac:");

    // Footer legend
    SDL_Rect footer{0, win_h - 66, win_w, 66};
    draw_panel(footer, COL_PANEL, COL_BORDER);
    draw_text(36, win_h - 42,
              "[A]/Tap Connect  [Y] +Win 3389  [X] +Mac 5900  [-] Delete Saved  [+] Menu / Switch Mode / Exit",
              COL_MUTED, 2);

    return buttons;
}

std::vector<UIButton> UIRenderer::render_home_menu(int win_w, int win_h, TouchMode tmode, int focused_idx) {
    std::vector<UIButton> buttons;

    SDL_SetRenderDrawColor(renderer_, 5, 8, 16, 195);
    SDL_Rect full{0, 0, win_w, win_h};
    SDL_RenderFillRect(renderer_, &full);

    int pw = 720;
    int ph = 560;
    SDL_Rect panel{(win_w - pw) / 2, (win_h - ph) / 2, pw, ph};
    draw_panel(panel, COL_PANEL, COL_ACCENT);

    draw_text(panel.x + 36, panel.y + 26, "OunceDesk Menu (+)", COL_TEXT, 3);

    int bx = panel.x + 48;
    int bw = pw - 96;
    int y = panel.y + 92;
    const int bh = 66;
    const int gap = 14;

    buttons.push_back({{bx, y, bw, bh}, "hm:mode",
                       (tmode == TouchMode::TOUCH) ? "Control Mode: Touch (Tap to switch Mouse)"
                                                   : "Control Mode: Mouse (Tap to switch Touch)",
                       true, false, false});
    y += bh + gap;
    buttons.push_back({{bx, y, bw, bh}, "hm:scan", "Rescan LAN (Windows 3389 + macOS 5900)", false, false, false});
    y += bh + gap;
    buttons.push_back({{bx, y, bw, bh}, "hm:add_rdp", "Add Windows PC (RDP 3389) via Native KB", false, false, false});
    y += bh + gap;
    buttons.push_back({{bx, y, bw, bh}, "hm:add_mac", "Add macOS (Screen Sharing 5900) via Native KB", false, false, false});
    y += bh + gap;
    buttons.push_back({{bx, y, bw, bh}, "hm:resume", "Close Menu", false, true, false});
    y += bh + gap;
    buttons.push_back({{bx, y, bw, bh}, "hm:exit", "Exit OunceDesk (Return to hbmenu)", false, false, true});

    for (size_t i = 0; i < buttons.size(); ++i) {
        draw_button(buttons[i], static_cast<int>(i) == focused_idx);
    }
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

    int mw = 960;
    int mh = 780;
    SDL_Rect modal{(win_w - mw) / 2, (win_h - mh) / 2, mw, mh};
    draw_panel(modal, COL_PANEL, COL_ACCENT);

    bool is_mac = (draft.protocol == ProtocolType::VNC_MACOS);
    draw_text(modal.x + 36, modal.y + 24,
              is_mac ? "macOS Remote Desktop (Port 5900)" : "Windows Remote Desktop (Port 3389)",
              is_mac ? COL_MAC : COL_WIN, 3);
    draw_text(modal.x + 36, modal.y + 66,
              "Tap any field or press [A] to type with Switch 2 Native Keyboard",
              COL_MUTED, 2);

    struct Row { const char* id; const char* title; std::string val; };
    std::vector<Row> rows = {
        {"f:proto", "Protocol (Tap to toggle)", is_mac ? "macOS Screen Sharing / VNC (Port 5900)" : "Windows Remote Desktop / RDP (Port 3389)"},
        {"f:host",  "Host IP Address [Native KB]", draft.host.empty() ? "(tap to enter IP, e.g. 192.168.1.50)" : draft.host},
        {"f:port",  "TCP Port [NumPad]",  std::to_string(draft.port)},
        {"f:user",  is_mac ? "macOS Username (Apple ARD) [Native KB]" : "Windows Username [Native KB]",
                    draft.username.empty() ? (is_mac ? "(optional for VNC password)" : "(required for Windows NLA)") : draft.username},
        {"f:pass",  "Password [Native KB]", draft.password.empty() ? "(tap to enter password)" : std::string(draft.password.size(), '*')},
        {"f:res",   is_mac ? "Display Name [Native KB]" : "RDP Resolution (Tap to toggle)",
                    is_mac ? (draft.name.empty() ? "My Mac" : draft.name)
                           : (std::to_string(draft.req_width) + "x" + std::to_string(draft.req_height))},
    };

    int y = modal.y + 104;
    for (size_t i = 0; i < rows.size(); ++i) {
        bool foc = (static_cast<int>(i) == focused_field);
        SDL_Rect r{modal.x + 36, y, mw - 72, 74};
        draw_panel(r, foc ? SDL_Color{34, 48, 74, 255} : COL_BG, foc ? COL_ACCENT : COL_BORDER);
        draw_text(r.x + 18, r.y + 10, rows[i].title, COL_MUTED, 2);
        draw_text(r.x + 18, r.y + 38, rows[i].val, COL_TEXT, 2);
        buttons.push_back({r, rows[i].id, "", false, false, false});
        y += 86;
    }

    if (!error_text.empty()) {
        draw_text(modal.x + 36, y + 6, error_text, COL_DANGER, 2);
    }

    int by = modal.y + mh - 88;
    UIButton b_cancel{{modal.x + 36, by, 260, 62}, "form_cancel", "Cancel", false, false, true};
    UIButton b_save  {{modal.x + 320, by, 280, 62}, "form_save", "Save Only", false, false, false};
    UIButton b_go    {{modal.x + 624, by, 300, 62}, "form_connect", "Save & Connect", true, false, false};
    draw_button(b_cancel, focused_field == 6);
    draw_button(b_save, focused_field == 7);
    draw_button(b_go, focused_field == 8);
    buttons.push_back(b_cancel);
    buttons.push_back(b_save);
    buttons.push_back(b_go);

    return buttons;
}

SDL_Rect UIRenderer::render_menu_pill(int /*win_w*/, int /*win_h*/) {
    SDL_Rect pill{16, 16, 148, 44};
    draw_panel(pill, {15, 23, 42, 185}, {56, 189, 248, 210});
    draw_text(pill.x + 16, pill.y + 14, "+ MENU", COL_TEXT, 2);
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
    std::snprintf(line2, sizeof(line2), "Mode: %s | Zoom %.1fx | [+] Menu  [X] Native KB",
                  (tmode == TouchMode::TOUCH) ? "Touch" : "Mouse", zoom);

    SDL_Rect box{win_w - 640, 14, 624, 62};
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

    SDL_SetRenderDrawColor(renderer_, 5, 8, 16, 185);
    SDL_Rect full{0, 0, win_w, win_h};
    SDL_RenderFillRect(renderer_, &full);

    int pw = std::min(win_w - 100, 1500);
    int ph = 520;
    SDL_Rect panel{(win_w - pw) / 2, (win_h - ph) / 2, pw, ph};
    draw_panel(panel, COL_PANEL, COL_ACCENT);

    bool is_mac = (active_profile.protocol == ProtocolType::VNC_MACOS);
    std::string hdr = "Menu (+)  |  " + active_profile.name + "  (" + st.protocol_label + ")";
    draw_text(panel.x + 32, panel.y + 24, hdr, COL_TEXT, 3);

    // Row 1: Primary Switch 2 Controls (Touch/Mouse Switch + Switch 2 Native Keyboard!)
    int qw = (pw - 64 - 3 * 16) / 4;
    buttons.push_back({{panel.x + 32 + 0 * (qw + 16), panel.y + 88, qw, 68}, "act:touch_mode",
                       (tmode == TouchMode::TOUCH) ? "Mode: Touch -> Mouse" : "Mode: Mouse -> Touch",
                       true, false, false});
    buttons.push_back({{panel.x + 32 + 1 * (qw + 16), panel.y + 88, qw, 68}, "act:swkbd",
                       "Native Keyboard (IME)", false, true, false});
    buttons.push_back({{panel.x + 32 + 2 * (qw + 16), panel.y + 88, qw, 68}, "act:osk",
                       "PC Special Keys", false, false, false});
    buttons.push_back({{panel.x + 32 + 3 * (qw + 16), panel.y + 88, qw, 68}, "act:refresh",
                       "Force Refresh", false, false, false});

    // Row 2: OS-Adaptive Shortcuts
    draw_text(panel.x + 32, panel.y + 180, is_mac ? "macOS Shortcuts:" : "Windows Shortcuts:", COL_MUTED, 2);
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
        buttons.push_back({{panel.x + 32 + i * (sw + 12), panel.y + 214, sw, 58},
                           shortcuts[i].id, is_mac ? shortcuts[i].mac_lbl : shortcuts[i].win_lbl, false, false, false});
    }

    // Row 3: HUD, Zoom, Resume, Disconnect, Exit App
    int bw = (pw - 64 - 4 * 14) / 5;
    buttons.push_back({{panel.x + 32 + 0 * (bw + 14), panel.y + 320, bw, 66}, "act:hud",
                       show_hud ? "Perf HUD: On" : "Perf HUD: Off", false, show_hud, false});
    buttons.push_back({{panel.x + 32 + 1 * (bw + 14), panel.y + 320, bw, 66}, "act:reset_zoom",
                       "Reset Zoom 1.0x", false, false, false});
    buttons.push_back({{panel.x + 32 + 2 * (bw + 14), panel.y + 320, bw, 66}, "act:resume",
                       "Resume", false, true, false});
    buttons.push_back({{panel.x + 32 + 3 * (bw + 14), panel.y + 320, bw, 66}, "act:disconnect",
                       "Disconnect", false, false, false});
    buttons.push_back({{panel.x + 32 + 4 * (bw + 14), panel.y + 320, bw, 66}, "act:exit_app",
                       "Exit App", false, false, true});

    for (size_t i = 0; i < buttons.size(); ++i) {
        draw_button(buttons[i], static_cast<int>(i) == focused_idx);
    }

    draw_text(panel.x + 32, panel.y + ph - 48,
              "[+] Open/Close Menu | [X] Switch 2 Native Keyboard | [-] Quick Switch Touch/Mouse",
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
