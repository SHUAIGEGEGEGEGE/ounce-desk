#pragma once

#include <SDL2/SDL.h>
#include <cstdint>
#include <string>
#include <vector>

enum class ProtocolType : uint8_t {
    RDP_WINDOWS = 0,  // Windows built-in Remote Desktop (TCP 3389, MS-RDPBCGR + TLS/CredSSP NLA)
    VNC_MACOS   = 1,  // macOS built-in Screen Sharing / VNC (TCP 5900, RFB 3.8 + Apple ARD / VNC Auth)
};

enum class BackendState : uint8_t {
    DISCONNECTED,
    CONNECTING,
    AUTHENTICATING,
    STREAMING,
    ERROR_STATE,
};

enum class MouseBtn : uint8_t {
    LEFT = 1,
    RIGHT = 2,
    MIDDLE = 3,
    BACK = 4,
    FORWARD = 5,
};

enum KeyModifier : uint8_t {
    RD_MOD_LCTRL  = 1 << 0,
    RD_MOD_LSHIFT = 1 << 1,
    RD_MOD_LALT   = 1 << 2,
    RD_MOD_LMETA  = 1 << 3,  // Win on Windows, Command on macOS
    RD_MOD_RCTRL  = 1 << 4,
    RD_MOD_RSHIFT = 1 << 5,
    RD_MOD_RALT   = 1 << 6,
    RD_MOD_RMETA  = 1 << 7,
};

struct ConnectionProfile {
    std::string name;
    ProtocolType protocol = ProtocolType::RDP_WINDOWS;
    std::string host;
    uint16_t port = 3389;
    std::string username;
    std::string password;
    std::string domain;       // Optional Windows domain / workgroup
    uint16_t req_width = 1920;
    uint16_t req_height = 1080;
    uint8_t vnc_bpp = 16;     // Mac VNC color depth: 16 (fast default), 8 (ultra-fast 4K/5K WAN), 32 (lossless LAN)
    bool from_mdns = false;   // Auto-discovered via Bonjour (_rfb._tcp.local)
};

struct BackendStats {
    std::string protocol_label;  // e.g. "RDP 3389 (NLA)" or "VNC 5900 (Apple ARD)"
    int fb_width = 0;
    int fb_height = 0;
    double fps = 0.0;
    double rx_mbps = 0.0;
    uint32_t rect_updates = 0;
};

struct DirtyRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

class RemoteBackend {
public:
    virtual ~RemoteBackend() = default;

    virtual bool connect_async(const ConnectionProfile& profile) = 0;
    virtual void disconnect() = 0;

    virtual BackendState state() const = 0;
    virtual std::string status_text() const = 0;
    virtual std::string error_message() const = 0;
    virtual BackendStats stats() const = 0;
    virtual ProtocolType protocol() const = 0;

    // Upload any new framebuffer updates directly to the SDL GPU texture (returns true if updated)
    virtual bool sync_texture(SDL_Renderer* renderer, SDL_Texture*& io_texture, int& out_w, int& out_h) = 0;

    // Unified input events (normalized 0..1 mouse coordinates, USB HID Usage ID page 0x07 keys)
    virtual void send_mouse_abs(float norm_x, float norm_y) = 0;
    virtual void send_mouse_button(MouseBtn btn, bool pressed) = 0;
    virtual void send_mouse_wheel(int16_t dx, int16_t dy) = 0;
    virtual void send_key(uint16_t hid_usage, bool pressed, uint8_t modifiers) = 0;
    virtual void send_text_utf8(const std::string& utf8) = 0;
    virtual void request_full_refresh() = 0;

    // True when the remote mouse cursor is currently an I-beam text cursor (hovering a text input field).
    // Used to auto-open the Switch 2 native keyboard when the user taps a remote text box.
    virtual bool cursor_is_text() const { return false; }
    // True once the server has sent at least one cursor-shape update (so cursor_is_text() is meaningful)
    virtual bool cursor_shape_known() const { return false; }
    // Heuristic fallback when server bakes cursor/caret into framebuffer (e.g. macOS Screen Sharing):
    // returns true if a vertical blinking text caret or text box exists near normalized (norm_x, norm_y).
    virtual bool detect_text_focus_near(float /*norm_x*/, float /*norm_y*/) const { return false; }
};
