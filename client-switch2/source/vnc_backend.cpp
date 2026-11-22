#include "vnc_backend.hpp"

#include "crypto_utils.hpp"
#include "cursor_detect.hpp"
#include "net_connect.hpp"
#include "keymap.hpp"
#include "platform.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define CLOSE_SOCK closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#define CLOSE_SOCK close
#endif

namespace {

inline uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline int32_t sbe32(const uint8_t* p) { return static_cast<int32_t>(be32(p)); }
inline void put_be16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v & 0xFF); }
inline void put_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v & 0xFF);
}

inline uint32_t bgra_cpixel_to_rgba(const uint8_t* p) {
    // 32bpp native macOS BGRA (R@16, G@8, B@0) -> CPIXEL (3 bytes: B, G, R) -> ABGR8888 (R@0, G@8, B@16)
    return uint32_t(p[2]) | (uint32_t(p[1]) << 8) | (uint32_t(p[0]) << 16) | 0xFF000000u;
}

}  // namespace

VncBackend::~VncBackend() { disconnect(); }

void VncBackend::set_status(const std::string& s) {
    platform::log_line("[VNC] " + s);
    std::lock_guard<std::mutex> lk(info_mu_);
    status_text_ = s;
}

void VncBackend::set_error(const std::string& e) {
    platform::log_line("[VNC ERROR] " + e);
    {
        std::lock_guard<std::mutex> lk(info_mu_);
        error_msg_ = e;
        status_text_ = "错误: " + e;
    }
    state_.store(BackendState::ERROR_STATE);
}

std::string VncBackend::status_text() const {
    std::lock_guard<std::mutex> lk(info_mu_);
    return status_text_;
}

std::string VncBackend::error_message() const {
    std::lock_guard<std::mutex> lk(info_mu_);
    return error_msg_;
}

BackendStats VncBackend::stats() const {
    std::lock_guard<std::mutex> lk(info_mu_);
    return stats_;
}

bool VncBackend::connect_async(const ConnectionProfile& profile) {
    disconnect();
    profile_ = profile;
    if (profile_.port == 0) profile_.port = 5900;
    netc::split_host_port(profile.host, profile_.host, profile_.port);
    stop_.store(false);
    state_.store(BackendState::CONNECTING);
    set_status("Connecting to " + profile_.host + ":" + std::to_string(profile_.port) + " (VNC/macOS)...");
    worker_ = std::thread(&VncBackend::run_session, this);
    return true;
}

void VncBackend::disconnect() {
    stop_.store(true);
    if (sock_ >= 0) {
#ifdef _WIN32
        shutdown(sock_, SD_BOTH);
#else
        shutdown(sock_, SHUT_RDWR);
#endif
        CLOSE_SOCK(sock_);
        sock_ = -1;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    if (zstream_ok_) {
        inflateEnd(&zstream_);
        zstream_ok_ = false;
    }
    state_.store(BackendState::DISCONNECTED);
}

bool VncBackend::send_all(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lk(send_mu_);
    if (sock_ < 0) return false;
    size_t sent = 0;
    while (sent < len && !stop_.load()) {
        int n = static_cast<int>(send(sock_, reinterpret_cast<const char*>(data + sent), static_cast<int>(len - sent), 0));
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return sent == len;
}

bool VncBackend::recv_all(uint8_t* data, size_t len) {
    if (rx_cache_.empty()) rx_cache_.resize(262144);

    size_t got = 0;
    while (got < len && !stop_.load()) {
        if (rx_pos_ < rx_len_) {
            size_t avail = rx_len_ - rx_pos_;
            size_t take = std::min(avail, len - got);
            std::memcpy(data + got, rx_cache_.data() + rx_pos_, take);
            rx_pos_ += take;
            got += take;
            if (got == len) return true;
        }
        if (len - got >= 8192) {
            int n = static_cast<int>(recv(sock_, reinterpret_cast<char*>(data + got), static_cast<int>(len - got), 0));
            if (n > 0) {
                got += static_cast<size_t>(n);
                rx_bytes_.fetch_add(static_cast<uint64_t>(n));
            } else if (n == 0) {
                return false;
            } else {
#ifdef _WIN32
                int err = WSAGetLastError();
                if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) continue;
#else
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
#endif
                return false;
            }
        } else {
            rx_pos_ = 0;
            rx_len_ = 0;
            int n = static_cast<int>(recv(sock_, reinterpret_cast<char*>(rx_cache_.data()), static_cast<int>(rx_cache_.size()), 0));
            if (n > 0) {
                rx_len_ = static_cast<size_t>(n);
                rx_bytes_.fetch_add(static_cast<uint64_t>(n));
            } else if (n == 0) {
                return false;
            } else {
#ifdef _WIN32
                int err = WSAGetLastError();
                if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) continue;
#else
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
#endif
                return false;
            }
        }
    }
    return got == len;
}

void VncBackend::mark_dirty(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    has_any_pixels_ = true;
    if (!full_dirty_) {
        if (dirty_rects_.size() >= 64) {
            // Coalesce into bounding box instead of forcing full_dirty_ (avoids 45MB full upload on 4.5K/5K!)
            int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
            for (const auto& r : dirty_rects_) {
                x0 = std::min(x0, r.x);
                y0 = std::min(y0, r.y);
                x1 = std::max(x1, r.x + r.w);
                y1 = std::max(y1, r.y + r.h);
            }
            dirty_rects_.clear();
            dirty_rects_.push_back({x0, y0, x1 - x0, y1 - y0});
        } else {
            dirty_rects_.push_back({x, y, w, h});
        }
    }
}

bool VncBackend::sync_texture(SDL_Renderer* renderer, SDL_Texture*& io_texture, int& out_w, int& out_h) {
    std::lock_guard<std::mutex> lk(fb_mu_);
    if (fb_w_ <= 0 || fb_h_ <= 0 || fb_rgba_.empty() || !has_any_pixels_) return false;

    int tw = 0, th = 0;
    if (io_texture) {
        SDL_QueryTexture(io_texture, nullptr, nullptr, &tw, &th);
    }
    if (!io_texture || tw != fb_w_ || th != fb_h_) {
        if (io_texture) SDL_DestroyTexture(io_texture);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        io_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, fb_w_, fb_h_);
        full_dirty_ = true;
    }
    out_w = fb_w_;
    out_h = fb_h_;
    if (!io_texture) return false;
    // During initial high-res progressive load, never hold back bands — display them immediately!
    if (initial_load_done_ && in_fb_batch_ && !full_dirty_) return false;

    if (full_dirty_) {
        SDL_UpdateTexture(io_texture, nullptr, fb_rgba_.data(), fb_w_ * 4);
        full_dirty_ = false;
        dirty_rects_.clear();
        return true;
    }
    if (dirty_rects_.empty()) return false;
    if (dirty_rects_.size() > 8) {
        int x0 = fb_w_, y0 = fb_h_, x1 = 0, y1 = 0;
        for (const auto& r : dirty_rects_) {
            x0 = std::min(x0, std::max(0, r.x));
            y0 = std::min(y0, std::max(0, r.y));
            x1 = std::max(x1, std::min(fb_w_, r.x + r.w));
            y1 = std::max(y1, std::min(fb_h_, r.y + r.h));
        }
        dirty_rects_.clear();
        if (x1 > x0 && y1 > y0) {
            SDL_Rect sr{x0, y0, x1 - x0, y1 - y0};
            const uint32_t* ptr = fb_rgba_.data() + static_cast<size_t>(y0) * fb_w_ + x0;
            SDL_UpdateTexture(io_texture, &sr, ptr, fb_w_ * 4);
            return true;
        }
        return false;
    }
    for (const auto& r : dirty_rects_) {
        int rx = std::max(0, std::min(r.x, fb_w_ - 1));
        int ry = std::max(0, std::min(r.y, fb_h_ - 1));
        int rw = std::min(r.w, fb_w_ - rx);
        int rh = std::min(r.h, fb_h_ - ry);
        if (rw > 0 && rh > 0) {
            SDL_Rect sr{rx, ry, rw, rh};
            const uint32_t* ptr = fb_rgba_.data() + static_cast<size_t>(ry) * fb_w_ + rx;
            SDL_UpdateTexture(io_texture, &sr, ptr, fb_w_ * 4);
        }
    }
    dirty_rects_.clear();
    return true;
}

void VncBackend::send_fb_region_request(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool incremental) {
    uint8_t msg[10]{};
    msg[0] = 3;  // FramebufferUpdateRequest
    msg[1] = incremental ? 1 : 0;
    put_be16(msg + 2, x);
    put_be16(msg + 4, y);
    put_be16(msg + 6, w);
    put_be16(msg + 8, h);
    send_all(msg, sizeof(msg));
}

void VncBackend::send_fb_update_request(bool incremental) {
    int w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lk(fb_mu_);
        w = fb_w_;
        h = fb_h_;
    }
    send_fb_region_request(0, 0, static_cast<uint16_t>(w), static_cast<uint16_t>(h), incremental);
}

void VncBackend::send_pointer(uint16_t x, uint16_t y, uint8_t mask) {
    if (state_.load() != BackendState::STREAMING) return;
    uint8_t msg[6];
    msg[0] = 5;  // PointerEvent
    msg[1] = mask;
    put_be16(msg + 2, x);
    put_be16(msg + 4, y);
    send_all(msg, sizeof(msg));
}

void VncBackend::send_keysym(uint32_t keysym, bool pressed) {
    if (state_.load() != BackendState::STREAMING || keysym == 0) return;
    uint8_t msg[8]{};
    msg[0] = 4;  // KeyEvent
    msg[1] = pressed ? 1 : 0;
    put_be32(msg + 4, keysym);
    send_all(msg, sizeof(msg));
}

void VncBackend::send_mouse_abs(float norm_x, float norm_y) {
    int w = 1, h = 1;
    {
        std::lock_guard<std::mutex> lk(fb_mu_);
        w = std::max(1, fb_w_);
        h = std::max(1, fb_h_);
    }
    last_x_ = static_cast<uint16_t>(std::max(0.0f, std::min(1.0f, norm_x)) * (w - 1));
    last_y_ = static_cast<uint16_t>(std::max(0.0f, std::min(1.0f, norm_y)) * (h - 1));
    send_pointer(last_x_, last_y_, btn_mask_);
}

void VncBackend::send_mouse_button(MouseBtn btn, bool pressed) {
    uint8_t bit = 0;
    if (btn == MouseBtn::LEFT)        bit = 1 << 0;
    else if (btn == MouseBtn::MIDDLE) bit = 1 << 1;
    else if (btn == MouseBtn::RIGHT)  bit = 1 << 2;
    else return;
    if (pressed) btn_mask_ |= bit;
    else btn_mask_ &= static_cast<uint8_t>(~bit);
    send_pointer(last_x_, last_y_, btn_mask_);
}

void VncBackend::send_mouse_wheel(int16_t dx, int16_t dy) {
    // RFB pointer wheel: button 4=up (bit 3), 5=down (bit 4), 6=left (bit 5), 7=right (bit 6)
    auto pulse = [&](uint8_t bit) {
        send_pointer(last_x_, last_y_, static_cast<uint8_t>(btn_mask_ | bit));
        send_pointer(last_x_, last_y_, btn_mask_);
    };
    if (dy > 20) pulse(1 << 3);
    else if (dy < -20) pulse(1 << 4);
    if (dx < -20) pulse(1 << 5);
    else if (dx > 20) pulse(1 << 6);
}

void VncBackend::send_key(uint16_t hid_usage, bool pressed, uint8_t modifiers) {
    bool shift = (modifiers & (RD_MOD_LSHIFT | RD_MOD_RSHIFT)) != 0;
    uint32_t ks = keymap::hid_to_x11_keysym(hid_usage, shift);
    send_keysym(ks, pressed);
}

void VncBackend::send_text_utf8(const std::string& utf8) {
    size_t pos = 0;
    while (pos < utf8.size()) {
        uint32_t cp = keymap::next_utf8_codepoint(utf8.data(), utf8.size(), pos);
        if (cp == 0) break;
        uint32_t ks = (cp < 0x100) ? cp : (0x01000000u | cp);
        send_keysym(ks, true);
        send_keysym(ks, false);
    }
}

void VncBackend::request_full_refresh() {
    want_full_refresh_.store(true);
}

bool VncBackend::detect_text_focus_near(float norm_x, float norm_y) const {
    std::lock_guard<std::mutex> lk(fb_mu_);
    if (fb_w_ < 64 || fb_h_ < 64 || fb_rgba_.empty()) return false;
    int cx = static_cast<int>(std::max(0.0f, std::min(1.0f, norm_x)) * (fb_w_ - 1));
    int cy = static_cast<int>(std::max(0.0f, std::min(1.0f, norm_y)) * (fb_h_ - 1));
    return cursor_detect::has_text_caret_or_box(fb_rgba_.data(), fb_w_, fb_h_, cx, cy);
}

bool VncBackend::decode_raw_rect(int rx, int ry, int rw, int rh) {
    const size_t pix_sz = std::max<size_t>(1, vnc_bpp_ / 8);
    std::vector<uint8_t> row_buf(static_cast<size_t>(rw) * pix_sz);
    for (int y = 0; y < rh; ++y) {
        if (!recv_all(row_buf.data(), row_buf.size())) {
            return false;
        }
        std::lock_guard<std::mutex> lk(fb_mu_);
        int dy = ry + y;
        if (dy >= 0 && dy < fb_h_) {
            int copy_w = std::min(rw, fb_w_ - rx);
            if (copy_w > 0 && rx >= 0) {
                uint32_t* dst = fb_rgba_.data() + static_cast<size_t>(dy) * fb_w_ + rx;
                if (vnc_bpp_ == 8) {
                    for (int x = 0; x < copy_w; ++x) dst[x] = lut8_[row_buf[x]];
                } else if (vnc_bpp_ == 16) {
                    for (int x = 0; x < copy_w; ++x) {
                        uint16_t v = uint16_t(row_buf[x * 2]) | (uint16_t(row_buf[x * 2 + 1]) << 8);
                        dst[x] = lut16_[v];
                    }
                } else {
                    for (int x = 0; x < copy_w; ++x) {
                        const uint8_t* p = row_buf.data() + x * 4;
                        dst[x] = uint32_t(p[2]) | (uint32_t(p[1]) << 8) | (uint32_t(p[0]) << 16) | 0xFF000000u;
                    }
                }
            }
        }
    }
    std::lock_guard<std::mutex> lk(fb_mu_);
    mark_dirty(rx, ry, rw, rh);
    return true;
}

bool VncBackend::decode_copyrect(int rx, int ry, int rw, int rh) {
    uint8_t src_xy[4];
    if (!recv_all(src_xy, 4)) return false;
    int sx = be16(src_xy);
    int sy = be16(src_xy + 2);

    std::lock_guard<std::mutex> lk(fb_mu_);
    if (rw <= 0 || rh <= 0) return true;
    std::vector<uint32_t> tmp(static_cast<size_t>(rw) * rh, 0xFF000000u);
    for (int y = 0; y < rh; ++y) {
        int src_y = sy + y;
        if (src_y < 0 || src_y >= fb_h_) continue;
        for (int x = 0; x < rw; ++x) {
            int src_x = sx + x;
            if (src_x >= 0 && src_x < fb_w_) {
                tmp[static_cast<size_t>(y) * rw + x] = fb_rgba_[static_cast<size_t>(src_y) * fb_w_ + src_x];
            }
        }
    }
    for (int y = 0; y < rh; ++y) {
        int dst_y = ry + y;
        if (dst_y < 0 || dst_y >= fb_h_) continue;
        for (int x = 0; x < rw; ++x) {
            int dst_x = rx + x;
            if (dst_x >= 0 && dst_x < fb_w_) {
                fb_rgba_[static_cast<size_t>(dst_y) * fb_w_ + dst_x] = tmp[static_cast<size_t>(y) * rw + x];
            }
        }
    }
    mark_dirty(rx, ry, rw, rh);
    return true;
}

bool VncBackend::decode_zrle_rect(int rx, int ry, int rw, int rh) {
    uint8_t len_b[4];
    if (!recv_all(len_b, 4)) return false;
    uint32_t zlen = be32(len_b);
    if (zlen == 0 || zlen > 32u * 1024u * 1024u) return false;

    std::vector<uint8_t> comp(zlen);
    if (!recv_all(comp.data(), zlen)) return false;

    if (!zstream_ok_) {
        std::memset(&zstream_, 0, sizeof(zstream_));
        if (inflateInit(&zstream_) != Z_OK) return false;
        zstream_ok_ = true;
    }

    const size_t cpix_sz = (vnc_bpp_ == 8) ? 1 : (vnc_bpp_ == 16 ? 2 : 3);
    auto read_cpixel = [&](const uint8_t* p) -> uint32_t {
        if (vnc_bpp_ == 8) return lut8_[p[0]];
        if (vnc_bpp_ == 16) return lut16_[uint16_t(p[0]) | (uint16_t(p[1]) << 8)];
        return bgra_cpixel_to_rgba(p);
    };

    // Decompress persistent zlib stream
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(rw) * rh * cpix_sz + 256);
    uint8_t chunk[65536];
    zstream_.next_in = comp.data();
    zstream_.avail_in = static_cast<uInt>(comp.size());
    while (zstream_.avail_in > 0) {
        zstream_.next_out = chunk;
        zstream_.avail_out = sizeof(chunk);
        int ret = inflate(&zstream_, Z_SYNC_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) return false;
        size_t have = sizeof(chunk) - zstream_.avail_out;
        raw.insert(raw.end(), chunk, chunk + have);
        if (ret == Z_BUF_ERROR) break;
    }

    size_t pos = 0;
    auto avail = [&](size_t n) { return pos + n <= raw.size(); };

    std::lock_guard<std::mutex> lk(fb_mu_);
    for (int ty = ry; ty < ry + rh; ty += 64) {
        int th = std::min(64, ry + rh - ty);
        for (int tx = rx; tx < rx + rw; tx += 64) {
            int tw = std::min(64, rx + rw - tx);
            if (!avail(1)) { mark_dirty(rx, ry, rw, rh); return true; }
            uint8_t sub = raw[pos++];

            if (sub == 0) {
                // Raw CPIXELs (1, 2, or 3 bytes each)
                for (int y = 0; y < th; ++y) {
                    for (int x = 0; x < tw; ++x) {
                        if (!avail(cpix_sz)) { mark_dirty(rx, ry, rw, rh); return true; }
                        uint32_t c = read_cpixel(raw.data() + pos);
                        pos += cpix_sz;
                        if (ty + y < fb_h_ && tx + x < fb_w_) {
                            fb_rgba_[static_cast<size_t>(ty + y) * fb_w_ + (tx + x)] = c;
                        }
                    }
                }
            } else if (sub == 1) {
                // Solid tile
                if (!avail(cpix_sz)) { mark_dirty(rx, ry, rw, rh); return true; }
                uint32_t c = read_cpixel(raw.data() + pos);
                pos += cpix_sz;
                for (int y = 0; y < th; ++y) {
                    if (ty + y >= fb_h_) break;
                    uint32_t* dst = fb_rgba_.data() + static_cast<size_t>(ty + y) * fb_w_ + tx;
                    int cw = std::min(tw, fb_w_ - tx);
                    for (int x = 0; x < cw; ++x) dst[x] = c;
                }
            } else if (sub >= 2 && sub <= 16) {
                // Packed palette
                uint32_t pal[16]{};
                for (int i = 0; i < sub; ++i) {
                    if (!avail(cpix_sz)) { mark_dirty(rx, ry, rw, rh); return true; }
                    pal[i] = read_cpixel(raw.data() + pos);
                    pos += cpix_sz;
                }
                int bpp = (sub == 2) ? 1 : (sub <= 4 ? 2 : 4);
                int mask = (1 << bpp) - 1;
                for (int y = 0; y < th; ++y) {
                    uint8_t cur_byte = 0;
                    int bits_left = 0;
                    for (int x = 0; x < tw; ++x) {
                        if (bits_left == 0) {
                            if (!avail(1)) { mark_dirty(rx, ry, rw, rh); return true; }
                            cur_byte = raw[pos++];
                            bits_left = 8;
                        }
                        bits_left -= bpp;
                        int idx = (cur_byte >> bits_left) & mask;
                        if (ty + y < fb_h_ && tx + x < fb_w_) {
                            fb_rgba_[static_cast<size_t>(ty + y) * fb_w_ + (tx + x)] = pal[idx % sub];
                        }
                    }
                }
            } else if (sub == 128) {
                // Plain RLE
                int pixels = tw * th;
                int p_idx = 0;
                while (p_idx < pixels && avail(cpix_sz + 1)) {
                    uint32_t c = read_cpixel(raw.data() + pos);
                    pos += cpix_sz;
                    int rlen = 1;
                    while (avail(1)) {
                        uint8_t b = raw[pos++];
                        rlen += b;
                        if (b != 255) break;
                    }
                    for (int k = 0; k < rlen && p_idx < pixels; ++k, ++p_idx) {
                        int py = ty + p_idx / tw;
                        int px = tx + p_idx % tw;
                        if (py < fb_h_ && px < fb_w_) {
                            fb_rgba_[static_cast<size_t>(py) * fb_w_ + px] = c;
                        }
                    }
                }
            } else if (sub >= 130) {
                // Palette RLE
                int psize = sub - 128;
                uint32_t pal[128]{};
                for (int i = 0; i < psize; ++i) {
                    if (!avail(cpix_sz)) { mark_dirty(rx, ry, rw, rh); return true; }
                    pal[i] = read_cpixel(raw.data() + pos);
                    pos += cpix_sz;
                }
                int pixels = tw * th;
                int p_idx = 0;
                while (p_idx < pixels && avail(1)) {
                    uint8_t b0 = raw[pos++];
                    int idx = b0 & 0x7F;
                    int rlen = 1;
                    if (b0 & 0x80) {
                        while (avail(1)) {
                            uint8_t b = raw[pos++];
                            rlen += b;
                            if (b != 255) break;
                        }
                    }
                    uint32_t c = pal[idx % psize];
                    for (int k = 0; k < rlen && p_idx < pixels; ++k, ++p_idx) {
                        int py = ty + p_idx / tw;
                        int px = tx + p_idx % tw;
                        if (py < fb_h_ && px < fb_w_) {
                            fb_rgba_[static_cast<size_t>(py) * fb_w_ + px] = c;
                        }
                    }
                }
            }
        }
    }
    mark_dirty(rx, ry, rw, rh);
    return true;
}

void VncBackend::run_session() {
    std::string port_s = std::to_string(profile_.port);
    std::string auth_label;
    bool had_type_2 = false;
    bool tried_ard = false;

    auto connect_and_auth = [&](bool allow_ard) -> bool {
        if (sock_ >= 0) {
            CLOSE_SOCK(sock_);
            sock_ = -1;
        }
        rx_pos_ = 0;
        rx_len_ = 0;
        std::string cerr, caddr;
        int s = netc::tcp_connect(profile_.host, profile_.port, 6000, cerr, &caddr);
        if (s < 0) {
            set_error(cerr + " (请检查 Mac 屏幕共享是否开启，外网需路由器转发 5900 端口)");
            return false;
        }
        platform::log_line("[VNC] TCP connected to " + caddr + ":" + port_s);
        sock_ = s;
        state_.store(BackendState::AUTHENTICATING);
        set_status("正在与 macOS 屏幕共享握手 (RFB 3.8)...");
        char ver[13]{};
        if (!recv_all(reinterpret_cast<uint8_t*>(ver), 12) || std::strncmp(ver, "RFB ", 4) != 0) {
            set_error("目标端口不是有效的 VNC/RFB 服务");
            return false;
        }
        const char* cli_ver = "RFB 003.008\n";
        if (!send_all(reinterpret_cast<const uint8_t*>(cli_ver), 12)) {
            set_error("发送 RFB 版本号失败");
            return false;
        }

        uint8_t num_types = 0;
        if (!recv_all(&num_types, 1) || num_types == 0) {
            set_error("服务端拒绝 RFB 握手");
            return false;
        }
        std::vector<uint8_t> sec_types(num_types);
        if (!recv_all(sec_types.data(), num_types)) {
            set_error("读取安全认证类型失败");
            return false;
        }

        auto has_type = [&](uint8_t t) {
            return std::find(sec_types.begin(), sec_types.end(), t) != sec_types.end();
        };
        had_type_2 = has_type(2);

        uint8_t chosen = 0;
        if (allow_ard && has_type(30) && !profile_.username.empty()) {
            chosen = 30;
            tried_ard = true;
            auth_label = "VNC 5900 (Apple ARD)";
        } else if (has_type(2)) {
            chosen = 2;
            auth_label = "VNC 5900 (VNC 密码)";
        } else if (allow_ard && has_type(30)) {
            chosen = 30;
            tried_ard = true;
            auth_label = "VNC 5900 (Apple ARD)";
        } else if (has_type(1)) {
            chosen = 1;
            auth_label = "VNC 5900 (免密)";
        } else {
            set_error("不支持的 VNC 认证类型 (请填写 macOS 用户名或开启 VNC 密码)");
            return false;
        }

        platform::log_line("[VNC] Chosen security type=" + std::to_string(chosen) +
                           " user='" + profile_.username + "' passLen=" + std::to_string(profile_.password.size()));

        if (!send_all(&chosen, 1)) {
            set_error("发送安全类型失败");
            return false;
        }

        if (chosen == 2) {
            set_status("正在验证 VNC 密码...");
            uint8_t challenge[16], response[16];
            if (!recv_all(challenge, 16)) { set_error("读取 VNC 挑战码失败"); return false; }
            crypto::vnc_des_encrypt_challenge(profile_.password, challenge, response);
            if (!send_all(response, 16)) { set_error("发送 VNC 响应失败"); return false; }
        } else if (chosen == 30) {
            set_status("正在验证 macOS Apple ARD 账户密码...");
            uint8_t hdr[4];
            if (!recv_all(hdr, 4)) { set_error("读取 Apple ARD 头失败"); return false; }
            uint16_t gen = be16(hdr);
            uint16_t key_len = be16(hdr + 2);
            if (key_len == 0 || key_len > 1024) { set_error("无效的 ARD 密钥长度"); return false; }
            std::vector<uint8_t> prime(key_len), srv_pub(key_len);
            if (!recv_all(prime.data(), key_len) || !recv_all(srv_pub.data(), key_len)) {
                set_error("读取 Apple ARD DH 参数失败");
                return false;
            }
            std::vector<uint8_t> cli_pub;
            uint8_t cipher[128];
            if (!crypto::apple_ard_encrypt_credentials(
                    gen, prime.data(), key_len, srv_pub.data(),
                    profile_.username, profile_.password, cli_pub, cipher)) {
                set_error("Apple ARD 加密失败");
                return false;
            }
            std::vector<uint8_t> resp;
            resp.reserve(128 + cli_pub.size());
            resp.insert(resp.end(), cipher, cipher + 128);
            resp.insert(resp.end(), cli_pub.begin(), cli_pub.end());
            if (!send_all(resp.data(), resp.size())) {
                set_error("发送 Apple ARD 凭据失败");
                return false;
            }
        }

        uint8_t sec_res[4];
        if (!recv_all(sec_res, 4) || be32(sec_res) != 0) {
            platform::log_line("[VNC] SecurityResult failed for type " + std::to_string(chosen));
            return false;
        }
        return true;
    };

    if (!connect_and_auth(true)) {
        // Automatic fallback: if Apple ARD (30) failed and server also supports VNC Password (2), retry with Type 2!
        if (!stop_.load() && tried_ard && had_type_2) {
            platform::log_line("[VNC] Apple ARD (30) failed; automatically falling back to VNC Password (2)...");
            if (!connect_and_auth(false)) {
                set_error("Mac 登录失败：请检查 macOS 用户名与密码 (或 VNC 密码)");
                return;
            }
        } else {
            if (state_.load() != BackendState::ERROR_STATE) {
                set_error("Mac 登录失败：请检查 macOS 用户名与密码 (或 VNC 密码)");
            }
            return;
        }
    }

    // 4. ClientInit (shared = 1) -> ServerInit
    uint8_t shared = 1;
    if (!send_all(&shared, 1)) { set_error("Failed sending ClientInit"); return; }
    uint8_t sinit[24];
    if (!recv_all(sinit, 24)) { set_error("Failed reading ServerInit"); return; }
    int sw = be16(sinit);
    int sh = be16(sinit + 2);
    uint32_t name_len = be32(sinit + 20);
    std::string desk_name(std::min<uint32_t>(name_len, 256), '\0');
    if (name_len > 0) {
        std::vector<uint8_t> tmp(name_len);
        if (!recv_all(tmp.data(), name_len)) { set_error("Failed reading desktop name"); return; }
        desk_name.assign(reinterpret_cast<const char*>(tmp.data()), std::min<size_t>(tmp.size(), 128));
    }

    {
        std::lock_guard<std::mutex> lk(fb_mu_);
        fb_w_ = std::max(1, sw);
        fb_h_ = std::max(1, sh);
        fb_rgba_.assign(static_cast<size_t>(fb_w_) * fb_h_, 0xFF0A0E18u);
        dirty_rects_.clear();
        full_dirty_ = true;
        has_any_pixels_ = false;
        initial_load_done_ = false;
        in_fb_batch_ = false;
    }

    vnc_bpp_ = (profile_.vnc_bpp == 8 || profile_.vnc_bpp == 16 || profile_.vnc_bpp == 32) ? profile_.vnc_bpp : 16;
    if (vnc_bpp_ == 8) {
        for (int v = 0; v < 256; ++v) {
            uint32_t r8 = static_cast<uint32_t>(((v & 7) * 255 + 3) / 7);
            uint32_t g8 = static_cast<uint32_t>((((v >> 3) & 7) * 255 + 3) / 7);
            uint32_t b8 = static_cast<uint32_t>((((v >> 6) & 3) * 255 + 1) / 3);
            lut8_[v] = r8 | (g8 << 8) | (b8 << 16) | 0xFF000000u;
        }
    } else if (vnc_bpp_ == 16) {
        lut16_.resize(65536);
        for (uint32_t v = 0; v < 65536; ++v) {
            uint32_t r5 = (v >> 11) & 31u;
            uint32_t g6 = (v >> 5) & 63u;
            uint32_t b5 = v & 31u;
            uint32_t r8 = (r5 << 3) | (r5 >> 2);
            uint32_t g8 = (g6 << 2) | (g6 >> 4);
            uint32_t b8 = (b5 << 3) | (b5 >> 2);
            lut16_[v] = r8 | (g8 << 8) | (b8 << 16) | 0xFF000000u;
        }
    }

    {
        std::lock_guard<std::mutex> lk(info_mu_);
        stats_.protocol_label = auth_label + " " + std::to_string(vnc_bpp_) + "位";
        stats_.fb_width = sw;
        stats_.fb_height = sh;
    }

    // 5. SetPixelFormat: 16bpp RGB565 (default 5x faster ZRLE on 4K/5K!), 8bpp BGR233 (15x faster WAN), or 32bpp BGRA
    uint8_t spf[20]{};
    spf[0] = 0;  // SetPixelFormat
    spf[6] = 0;  // big-endian-flag = 0 (little endian)
    spf[7] = 1;  // true-color-flag = 1
    if (vnc_bpp_ == 8) {
        spf[4] = 8;  spf[5] = 8;
        put_be16(spf + 8, 7);   put_be16(spf + 10, 7);  put_be16(spf + 12, 3);
        spf[14] = 0; spf[15] = 3; spf[16] = 6;  // standard BGR233
    } else if (vnc_bpp_ == 16) {
        spf[4] = 16; spf[5] = 16;
        put_be16(spf + 8, 31);  put_be16(spf + 10, 63); put_be16(spf + 12, 31);
        spf[14] = 11; spf[15] = 5; spf[16] = 0; // RGB565
    } else {
        spf[4] = 32; spf[5] = 24;
        put_be16(spf + 8, 255); put_be16(spf + 10, 255); put_be16(spf + 12, 255);
        spf[14] = 16; spf[15] = 8; spf[16] = 0; // native macOS BGRA (zero server swizzle!)
    }
    if (!send_all(spf, sizeof(spf))) { set_error("SetPixelFormat failed"); return; }

    // 6. SetEncodings: ZRLE (16), CopyRect (1), Raw (0), DesktopSize (-223), CursorWithAlpha (-314), Cursor (-239)
    uint8_t encs[4 + 6 * 4]{};
    encs[0] = 2;  // SetEncodings
    put_be16(encs + 2, 6);
    put_be32(encs + 4, 16);                              // ZRLE
    put_be32(encs + 8, 1);                               // CopyRect
    put_be32(encs + 12, 0);                              // Raw
    put_be32(encs + 16, static_cast<uint32_t>(-223));    // DesktopSize
    put_be32(encs + 20, static_cast<uint32_t>(-314));    // CursorWithAlpha (macOS native alpha cursor)
    put_be32(encs + 24, static_cast<uint32_t>(-239));    // RichCursor / Cursor pseudo-encoding
    if (!send_all(encs, sizeof(encs))) { set_error("SetEncodings failed"); return; }

    state_.store(BackendState::STREAMING);
    set_status("正在加载 " + std::to_string(sw) + "x" + std::to_string(sh) + " 远程桌面首帧...");

    // Progressive horizontal strip loading for high-res 4K/5K Macs (e.g. 4480x2520):
    // fetches 6 strips so top of screen appears within <1 second instead of waiting for the entire 4.5K screen!
    const int init_bands = (static_cast<int64_t>(sw) * sh > 2073600LL) ? 6 : 1;
    auto send_init_band = [&](int idx) {
        int y0 = (sh * idx) / init_bands;
        int y1 = (sh * (idx + 1)) / init_bands;
        if (y1 > y0) {
            send_fb_region_request(0, static_cast<uint16_t>(y0),
                                   static_cast<uint16_t>(sw), static_cast<uint16_t>(y1 - y0), false);
        }
    };
    int next_init_band = 1;
    int init_bands_done = 0;
    send_init_band(0);

    uint64_t stat_t0 = platform::now_us();
    int frames_win = 0;

    // 7. Main RFB Receive Loop
    while (!stop_.load()) {
        uint8_t msg_type = 0;
        if (!recv_all(&msg_type, 1)) {
            if (!stop_.load()) set_error("VNC connection closed");
            break;
        }
        if (msg_type == 0) {
            // FramebufferUpdate
            uint8_t hdr[3];
            if (!recv_all(hdr, 3)) break;
            uint16_t nrects = be16(hdr + 1);
            // Pipeline the next strip (during initial load) or next incremental full-screen update!
            if (next_init_band < init_bands) {
                send_init_band(next_init_band++);
            } else {
                bool full = want_full_refresh_.exchange(false);
                send_fb_update_request(!full);
            }
            {
                std::lock_guard<std::mutex> lk(fb_mu_);
                in_fb_batch_ = initial_load_done_;
            }
            for (uint16_t i = 0; i < nrects && !stop_.load(); ++i) {
                uint8_t rhdr[12];
                if (!recv_all(rhdr, 12)) { stop_.store(true); break; }
                int rx = be16(rhdr);
                int ry = be16(rhdr + 2);
                int rw = be16(rhdr + 4);
                int rh = be16(rhdr + 6);
                int32_t enc = sbe32(rhdr + 8);

                if (enc == 0) {
                    if (!decode_raw_rect(rx, ry, rw, rh)) { stop_.store(true); break; }
                } else if (enc == 1) {
                    if (!decode_copyrect(rx, ry, rw, rh)) { stop_.store(true); break; }
                } else if (enc == 16) {
                    if (!decode_zrle_rect(rx, ry, rw, rh)) { stop_.store(true); break; }
                } else if (enc == -223) {
                    std::lock_guard<std::mutex> lk(fb_mu_);
                    fb_w_ = std::max(1, rw);
                    fb_h_ = std::max(1, rh);
                    fb_rgba_.assign(static_cast<size_t>(fb_w_) * fb_h_, 0xFF0A0E18u);
                    full_dirty_ = true;
                    std::lock_guard<std::mutex> ik(info_mu_);
                    stats_.fb_width = fb_w_;
                    stats_.fb_height = fb_h_;
                } else if (enc == -314) {
                    // CursorWithAlpha (-314): 4-byte sub-encoding + encoded w*h*4 RGBA pixels
                    uint8_t sub_b[4];
                    if (!recv_all(sub_b, 4)) { stop_.store(true); break; }
                    int32_t sub_enc = sbe32(sub_b);
                    std::vector<uint8_t> rgba(static_cast<size_t>(rw) * rh * 4, 0);
                    bool ok_cur = true;
                    if (sub_enc == 0 && !rgba.empty()) {
                        ok_cur = recv_all(rgba.data(), rgba.size());
                    } else if (sub_enc == 16 && rw > 0 && rh > 0) {
                        // ZRLE-compressed alpha cursor (4-byte length + zlib stream)
                        uint8_t lb[4];
                        if (!recv_all(lb, 4)) { stop_.store(true); break; }
                        uint32_t zl = be32(lb);
                        std::vector<uint8_t> skip(zl);
                        if (zl > 0 && !recv_all(skip.data(), zl)) { stop_.store(true); break; }
                        ok_cur = false;
                    }
                    if (!ok_cur && sub_enc == 0) { stop_.store(true); break; }
                    if (ok_cur && rw > 0 && rh > 0) {
                        std::vector<uint8_t> vis(static_cast<size_t>(rw) * rh, 0);
                        for (int i = 0; i < rw * rh; ++i) {
                            vis[static_cast<size_t>(i)] = (rgba[static_cast<size_t>(i) * 4 + 3] >= 48) ? 1 : 0;
                        }
                        cursor_known_.store(true);
                        cursor_text_.store(cursor_detect::is_ibeam(vis, rw, rh, rx, ry));
                    }
                } else if (enc == -239) {
                    // Cursor pseudo-encoding: rx/ry = hotspot, w*h pixels (bpp/8 bytes each) + 1bpp mask
                    size_t pix_len = static_cast<size_t>(rw) * rh * std::max<size_t>(1, vnc_bpp_ / 8);
                    size_t mask_stride = static_cast<size_t>((rw + 7) / 8);
                    size_t mask_len = mask_stride * rh;
                    std::vector<uint8_t> cbuf(pix_len + mask_len);
                    if (!cbuf.empty() && !recv_all(cbuf.data(), cbuf.size())) { stop_.store(true); break; }
                    if (rw > 0 && rh > 0) {
                        std::vector<uint8_t> vis(static_cast<size_t>(rw) * rh, 0);
                        const uint8_t* mask = cbuf.data() + pix_len;
                        for (int y = 0; y < rh; ++y)
                            for (int x = 0; x < rw; ++x)
                                vis[static_cast<size_t>(y) * rw + x] = (mask[y * mask_stride + x / 8] >> (7 - (x % 8))) & 1;
                        bool txt = cursor_detect::is_ibeam(vis, rw, rh, rx, ry);
                        if (!cursor_known_.load()) platform::log_line("[VNC] Server supports cursor-shape updates");
                        cursor_known_.store(true);
                        cursor_text_.store(txt);
                    } else {
                        cursor_text_.store(false);
                    }
                } else {
                    set_error("Unsupported RFB encoding: " + std::to_string(enc));
                    stop_.store(true);
                    break;
                }
            }
            {
                std::lock_guard<std::mutex> lk(fb_mu_);
                in_fb_batch_ = false;
                if (!initial_load_done_ && has_any_pixels_ && ++init_bands_done >= init_bands) {
                    initial_load_done_ = true;
                }
            }
            ++frames_win;
        } else if (msg_type == 1) {
            // SetColourMapEntries (skip)
            uint8_t h[5];
            if (!recv_all(h, 5)) break;
            uint16_t nc = be16(h + 3);
            std::vector<uint8_t> skip(static_cast<size_t>(nc) * 6);
            if (!recv_all(skip.data(), skip.size())) break;
        } else if (msg_type == 2) {
            // Bell (ignore)
        } else if (msg_type == 3) {
            // ServerCutText (remote clipboard -> Switch clipboard)
            uint8_t h[7];
            if (!recv_all(h, 7)) break;
            uint32_t clen = be32(h + 3);
            if (clen > 0 && clen <= 65536) {
                std::string txt(clen, '\0');
                if (!recv_all(reinterpret_cast<uint8_t*>(&txt[0]), clen)) break;
                SDL_SetClipboardText(txt.c_str());
            } else if (clen > 65536) {
                break;
            }
        }

        uint64_t now = platform::now_us();
        if (now - stat_t0 >= 1000000ULL) {
            double sec = static_cast<double>(now - stat_t0) / 1e6;
            uint64_t bytes = rx_bytes_.exchange(0);
            std::lock_guard<std::mutex> lk(info_mu_);
            stats_.fps = frames_win / sec;
            stats_.rx_mbps = (bytes * 8.0) / (sec * 1e6);
            stats_.rect_updates += static_cast<uint32_t>(frames_win);
            frames_win = 0;
            stat_t0 = now;
        }
    }
}
