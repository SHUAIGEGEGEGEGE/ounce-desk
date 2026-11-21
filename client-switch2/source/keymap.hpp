#pragma once

#include <cstdint>

namespace keymap {

struct Ps2Scancode {
    uint8_t code;
    bool extended;
};

// USB HID Usage ID (Page 0x07, matches SDL_Scancode & libnx HidKeyboardKey)
// -> IBM PC/AT Set-1 Scancode used by Windows RDP (MS-RDPBCGR Fast-Path Input)
inline Ps2Scancode hid_to_ps2(uint16_t hid) {
    switch (hid) {
        case 4:  return {0x1E, false}; // A
        case 5:  return {0x30, false}; // B
        case 6:  return {0x2E, false}; // C
        case 7:  return {0x20, false}; // D
        case 8:  return {0x12, false}; // E
        case 9:  return {0x21, false}; // F
        case 10: return {0x22, false}; // G
        case 11: return {0x23, false}; // H
        case 12: return {0x17, false}; // I
        case 13: return {0x24, false}; // J
        case 14: return {0x25, false}; // K
        case 15: return {0x26, false}; // L
        case 16: return {0x32, false}; // M
        case 17: return {0x31, false}; // N
        case 18: return {0x18, false}; // O
        case 19: return {0x19, false}; // P
        case 20: return {0x10, false}; // Q
        case 21: return {0x13, false}; // R
        case 22: return {0x1F, false}; // S
        case 23: return {0x14, false}; // T
        case 24: return {0x16, false}; // U
        case 25: return {0x2F, false}; // V
        case 26: return {0x11, false}; // W
        case 27: return {0x2D, false}; // X
        case 28: return {0x15, false}; // Y
        case 29: return {0x2C, false}; // Z
        case 30: return {0x02, false}; // 1
        case 31: return {0x03, false}; // 2
        case 32: return {0x04, false}; // 3
        case 33: return {0x05, false}; // 4
        case 34: return {0x06, false}; // 5
        case 35: return {0x07, false}; // 6
        case 36: return {0x08, false}; // 7
        case 37: return {0x09, false}; // 8
        case 38: return {0x0A, false}; // 9
        case 39: return {0x0B, false}; // 0
        case 40: return {0x1C, false}; // Enter
        case 41: return {0x01, false}; // Escape
        case 42: return {0x0E, false}; // Backspace
        case 43: return {0x0F, false}; // Tab
        case 44: return {0x39, false}; // Space
        case 45: return {0x0C, false}; // -
        case 46: return {0x0D, false}; // =
        case 47: return {0x1A, false}; // [
        case 48: return {0x1B, false}; // ]
        case 49: return {0x2B, false}; // backslash
        case 51: return {0x27, false}; // ;
        case 52: return {0x28, false}; // '
        case 53: return {0x29, false}; // `
        case 54: return {0x33, false}; // ,
        case 55: return {0x34, false}; // .
        case 56: return {0x35, false}; // /
        case 57: return {0x3A, false}; // CapsLock
        case 58: return {0x3B, false}; // F1
        case 59: return {0x3C, false}; // F2
        case 60: return {0x3D, false}; // F3
        case 61: return {0x3E, false}; // F4
        case 62: return {0x3F, false}; // F5
        case 63: return {0x40, false}; // F6
        case 64: return {0x41, false}; // F7
        case 65: return {0x42, false}; // F8
        case 66: return {0x43, false}; // F9
        case 67: return {0x44, false}; // F10
        case 68: return {0x57, false}; // F11
        case 69: return {0x58, false}; // F12
        case 70: return {0x37, true};  // PrintScreen
        case 73: return {0x52, true};  // Insert
        case 74: return {0x47, true};  // Home
        case 75: return {0x49, true};  // PageUp
        case 76: return {0x53, true};  // Delete
        case 77: return {0x4F, true};  // End
        case 78: return {0x51, true};  // PageDown
        case 79: return {0x4D, true};  // Right
        case 80: return {0x4B, true};  // Left
        case 81: return {0x50, true};  // Down
        case 82: return {0x48, true};  // Up
        case 224: return {0x1D, false}; // LCtrl
        case 225: return {0x2A, false}; // LShift
        case 226: return {0x38, false}; // LAlt
        case 227: return {0x5B, true};  // LWin
        case 228: return {0x1D, true};  // RCtrl
        case 229: return {0x36, false}; // RShift
        case 230: return {0x38, true};  // RAlt
        case 231: return {0x5C, true};  // RWin
        default:  return {0, false};
    }
}

// USB HID Usage ID -> X11 KeySym used by RFB/VNC & macOS Screen Sharing
inline uint32_t hid_to_x11_keysym(uint16_t hid, bool shift) {
    if (hid >= 4 && hid <= 29) {
        return static_cast<uint32_t>((shift ? 'A' : 'a') + (hid - 4));
    }
    static constexpr char num_unshifted[10] = {'1','2','3','4','5','6','7','8','9','0'};
    static constexpr char num_shifted[10]   = {'!','@','#','$','%','^','&','*','(',')'};
    if (hid >= 30 && hid <= 39) {
        int i = hid - 30;
        return static_cast<uint32_t>(shift ? num_shifted[i] : num_unshifted[i]);
    }
    if (hid >= 58 && hid <= 69) {
        return 0xFFBEu + static_cast<uint32_t>(hid - 58);  // XK_F1 .. XK_F12
    }
    switch (hid) {
        case 40:  return 0xFF0Du; // Return
        case 41:  return 0xFF1Bu; // Escape
        case 42:  return 0xFF08u; // BackSpace
        case 43:  return 0xFF09u; // Tab
        case 44:  return 0x0020u; // space
        case 45:  return shift ? '_' : '-';
        case 46:  return shift ? '+' : '=';
        case 47:  return shift ? '{' : '[';
        case 48:  return shift ? '}' : ']';
        case 49:  return shift ? '|' : '\\';
        case 51:  return shift ? ':' : ';';
        case 52:  return shift ? '"' : '\'';
        case 53:  return shift ? '~' : '`';
        case 54:  return shift ? '<' : ',';
        case 55:  return shift ? '>' : '.';
        case 56:  return shift ? '?' : '/';
        case 57:  return 0xFFE5u; // Caps_Lock
        case 73:  return 0xFF63u; // Insert
        case 74:  return 0xFF50u; // Home
        case 75:  return 0xFF55u; // Page_Up
        case 76:  return 0xFFFFu; // Delete
        case 77:  return 0xFF57u; // End
        case 78:  return 0xFF56u; // Page_Down
        case 79:  return 0xFF53u; // Right
        case 80:  return 0xFF51u; // Left
        case 81:  return 0xFF54u; // Down
        case 82:  return 0xFF52u; // Up
        case 224: return 0xFFE3u; // Control_L
        case 225: return 0xFFE1u; // Shift_L
        case 226: return 0xFFE9u; // Alt_L (Option on macOS)
        case 227: return 0xFFEB;  // Super_L (Command on macOS Screen Sharing)
        case 228: return 0xFFE4u; // Control_R
        case 229: return 0xFFE2u; // Shift_R
        case 230: return 0xFFEAu; // Alt_R
        case 231: return 0xFFECu; // Super_R
        default:  return 0;
    }
}

// Convert UTF-8 string into Unicode code points (for RDP Unicode & VNC KeySym 0x01000000|cp)
inline uint32_t next_utf8_codepoint(const char* s, size_t len, size_t& pos) {
    if (pos >= len) return 0;
    uint8_t c0 = static_cast<uint8_t>(s[pos]);
    if (c0 < 0x80) { ++pos; return c0; }
    if ((c0 & 0xE0) == 0xC0 && pos + 1 < len) {
        uint32_t cp = ((c0 & 0x1Fu) << 6) | (static_cast<uint8_t>(s[pos + 1]) & 0x3Fu);
        pos += 2;
        return cp;
    }
    if ((c0 & 0xF0) == 0xE0 && pos + 2 < len) {
        uint32_t cp = ((c0 & 0x0Fu) << 12) |
                      ((static_cast<uint8_t>(s[pos + 1]) & 0x3Fu) << 6) |
                      (static_cast<uint8_t>(s[pos + 2]) & 0x3Fu);
        pos += 3;
        return cp;
    }
    if ((c0 & 0xF8) == 0xF0 && pos + 3 < len) {
        uint32_t cp = ((c0 & 0x07u) << 18) |
                      ((static_cast<uint8_t>(s[pos + 1]) & 0x3Fu) << 12) |
                      ((static_cast<uint8_t>(s[pos + 2]) & 0x3Fu) << 6) |
                      (static_cast<uint8_t>(s[pos + 3]) & 0x3Fu);
        pos += 4;
        return cp;
    }
    ++pos;
    return '?';
}

}  // namespace keymap
