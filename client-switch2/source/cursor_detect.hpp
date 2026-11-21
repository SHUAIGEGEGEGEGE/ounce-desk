#pragma once

#include <cstdint>
#include <vector>

namespace cursor_detect {

// Heuristic: decide whether a cursor shape is an I-beam (text input) cursor.
// visible: w*h top-down mask of opaque/inverting pixels; (hx, hy) = hotspot.
// I-beam: tall & narrow visible bounding box, hotspot near the vertical centre, thin middle stem.
inline bool is_ibeam(const std::vector<uint8_t>& visible, int w, int h, int hx, int hy) {
    if (w <= 0 || h <= 0 || static_cast<int>(visible.size()) < w * h) return false;
    int minx = w, miny = h, maxx = -1, maxy = -1;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (visible[static_cast<size_t>(y) * w + x]) {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
        }
    }
    if (maxx < 0) return false;
    int bw = maxx - minx + 1;
    int bh = maxy - miny + 1;
    if (bh < 10) return false;
    if (bw * 2 > bh) return false;                       // must be at least 2x taller than wide
    // Hotspot must lie inside the box and roughly in the vertical middle (arrow/hand hotspots are at the top)
    if (hx < minx - 2 || hx > maxx + 2) return false;
    if (hy < miny + bh / 4 || hy > maxy - bh / 4) return false;
    // Middle stem must be thin (<= 1/2 of box width, max 4px at 1x)
    int mid = miny + bh / 2;
    int stem = 0;
    for (int x = minx; x <= maxx; ++x) {
        if (visible[static_cast<size_t>(mid) * w + x]) ++stem;
    }
    if (stem == 0 || stem * 2 > bw + 1) return false;
    return true;
}

// Framebuffer caret/input-box detector for servers (like macOS Screen Sharing) that render the cursor/caret
// into the framebuffer instead of sending cursor-shape pseudo-encodings:
// Checks a local patch around (cx, cy) for a vertical text caret (1..3 px wide, 11..42 px tall high-contrast bar)
// OR a clean uniform text-input background strip with horizontal border lines above & below.
inline bool has_text_caret_or_box(const uint32_t* fb, int fb_w, int fb_h, int cx, int cy) {
    if (!fb || fb_w < 64 || fb_h < 64) return false;
    cx = cx < 0 ? 0 : (cx >= fb_w ? fb_w - 1 : cx);
    cy = cy < 0 ? 0 : (cy >= fb_h ? fb_h - 1 : cy);

    auto luma = [&](int x, int y) -> int {
        uint32_t p = fb[static_cast<size_t>(y) * fb_w + x];
        int r = p & 0xFF;
        int g = (p >> 8) & 0xFF;
        int b = (p >> 16) & 0xFF;
        return (r * 77 + g * 150 + b * 29) >> 8;
    };

    // 1. Search within [-96..+48] horizontally and [-20..+20] vertically for a crisp 1..3 px vertical caret bar
    int x0 = cx - 96 < 4 ? 4 : cx - 96;
    int x1 = cx + 48 >= fb_w - 4 ? fb_w - 5 : cx + 48;
    int y0 = cy - 22 < 2 ? 2 : cy - 22;
    int y1 = cy + 22 >= fb_h - 2 ? fb_h - 3 : cy + 22;

    for (int x = x0; x <= x1; ++x) {
        int run = 0;
        int best_run = 0;
        for (int y = y0; y <= y1; ++y) {
            int lc = luma(x, y);
            int ll = luma(x - 3, y);
            int lr = luma(x + 3, y);
            int bg = (ll + lr) / 2;
            int diff_bg = (lc > bg) ? (lc - bg) : (bg - lc);
            int flat_bg = (ll > lr) ? (ll - lr) : (lr - ll);
            // Caret pixel differs sharply from uniform left/right background
            if (diff_bg >= 65 && flat_bg <= 24) {
                ++run;
                if (run > best_run) best_run = run;
            } else {
                run = 0;
            }
        }
        if (best_run >= 12 && best_run <= 44) {
            return true;
        }
    }

    // 2. Check if (cx, cy) sits inside a clean horizontal input-field band (height 18..64 px) bounded by top & bottom edges
    int center_l = luma(cx, cy);
    int top_edge = -1, bot_edge = -1;
    for (int dy = 4; dy <= 38 && cy - dy >= 1; ++dy) {
        int d = luma(cx, cy - dy) - center_l;
        if (d < 0) d = -d;
        if (d >= 18) { top_edge = cy - dy; break; }
    }
    for (int dy = 4; dy <= 38 && cy + dy < fb_h - 1; ++dy) {
        int d = luma(cx, cy + dy) - center_l;
        if (d < 0) d = -d;
        if (d >= 18) { bot_edge = cy + dy; break; }
    }
    if (top_edge >= 0 && bot_edge >= 0) {
        int box_h = bot_edge - top_edge;
        if (box_h >= 16 && box_h <= 64) {
            // Verify horizontal continuity of top & bottom borders across [-36..+36] px (distinguishes input boxes from icons/buttons/desktop!)
            int span_ok = 0;
            int samples = 0;
            for (int dx = -36; dx <= 36; dx += 6) {
                int sx = cx + dx;
                if (sx < 2 || sx >= fb_w - 2) continue;
                ++samples;
                int mid_l = luma(sx, (top_edge + bot_edge) / 2);
                int dt = luma(sx, top_edge) - mid_l; if (dt < 0) dt = -dt;
                int db = luma(sx, bot_edge) - mid_l; if (db < 0) db = -db;
                int dm = mid_l - center_l; if (dm < 0) dm = -dm;
                if (dt >= 14 && db >= 14 && dm <= 20) ++span_ok;
            }
            if (samples >= 8 && span_ok * 4 >= samples * 3) {
                return true;
            }
        }
    }

    return false;
}

}  // namespace cursor_detect
