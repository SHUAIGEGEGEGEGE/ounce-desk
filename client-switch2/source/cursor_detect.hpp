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

}  // namespace cursor_detect
