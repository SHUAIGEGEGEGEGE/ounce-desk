#define SDL_MAIN_HANDLED
#include "../client-switch2/source/rdp_backend.cpp"
#include <cstdio>

int main() {
    int fails = 0;
    // Interleaved 16bpp 4x2: line0 COLOR_RUN(4, red 0xF800), line1 BG_RUN(4) -> copy above (red)
    {
        const uint8_t s[] = {0x64, 0x00, 0xF8, 0x04};
        std::vector<uint32_t> o;
        decompress_interleaved(s, sizeof(s), 4, 2, 16, o);
        for (auto v : o) if (v != 0xFF0000FFu) { ++fails; std::printf("interleaved1 bad %08X\n", v); break; }
    }
    // Interleaved 16bpp 4x2: line0 BG_RUN(4)=black; line1 FG_RUN(4) -> above ^ white = white
    {
        const uint8_t s[] = {0x04, 0x24};
        std::vector<uint32_t> o;
        decompress_interleaved(s, sizeof(s), 4, 2, 16, o);
        for (int i = 0; i < 4; ++i) if (o[i] != 0xFF000000u) { ++fails; std::printf("interleaved2a bad\n"); break; }
        for (int i = 4; i < 8; ++i) if (o[i] != 0xFFFFFFFFu) { ++fails; std::printf("interleaved2b bad %08X\n", o[i]); break; }
    }
    // Interleaved 24bpp FGBG image first line: LITE_SET_FG_FGBG (0xD1 -> 8 px), fg = BGR(0,0,255)=red, mask 0x0F
    {
        const uint8_t s[] = {0xD1, 0x00, 0x00, 0xFF, 0x0F};
        std::vector<uint32_t> o;
        decompress_interleaved(s, sizeof(s), 8, 1, 24, o);
        for (int i = 0; i < 4; ++i) if (o[i] != 0xFF0000FFu) { ++fails; std::printf("fgbg bad %d %08X\n", i, o[i]); break; }
        for (int i = 4; i < 8; ++i) if (o[i] != 0xFF000000u) { ++fails; std::printf("fgbg bad2 %d\n", i); break; }
    }
    // Planar RLE 2x2, no alpha: each plane line0 raw [10,20]; line1 deltas [+1,-2]
    {
        std::vector<uint8_t> s = {0x30};
        for (int p = 0; p < 3; ++p) { s.insert(s.end(), {0x20, 10, 20, 0x20, 2, 3}); }
        std::vector<uint32_t> o;
        decompress_planar(s.data(), s.size(), 2, 2, o);
        uint8_t exp[4] = {10, 20, 11, 18};
        for (int i = 0; i < 4; ++i) {
            uint32_t e = exp[i] | (exp[i] << 8) | (exp[i] << 16) | 0xFF000000u;
            if (o[i] != e) { ++fails; std::printf("planar bad %d %08X\n", i, o[i]); }
        }
    }
    // I-beam detection: 7x17 I-beam with centre hotspot vs. arrow
    {
        int w = 16, h = 20;
        std::vector<uint8_t> v(w * h, 0);
        for (int x = 4; x <= 10; ++x) { v[1 * w + x] = 1; v[18 * w + x] = 1; }
        for (int y = 1; y <= 18; ++y) v[y * w + 7] = 1;
        if (!cursor_detect::is_ibeam(v, w, h, 7, 10)) { ++fails; std::printf("ibeam not detected\n"); }
        std::vector<uint8_t> a(w * h, 0);
        for (int y = 0; y < 16; ++y) for (int x = 0; x <= y * 2 / 3; ++x) a[y * w + x] = 1;
        if (cursor_detect::is_ibeam(a, w, h, 0, 0)) { ++fails; std::printf("arrow misdetected\n"); }
    }
    std::printf(fails ? "CODEC TESTS FAILED\n" : "ALL CODEC TESTS PASSED\n");
    return fails;
}
