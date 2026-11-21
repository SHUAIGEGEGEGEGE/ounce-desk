#pragma once

#include "backend.hpp"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <zlib.h>

class VncBackend final : public RemoteBackend {
public:
    VncBackend() = default;
    ~VncBackend() override;

    bool connect_async(const ConnectionProfile& profile) override;
    void disconnect() override;

    BackendState state() const override { return state_.load(); }
    std::string status_text() const override;
    std::string error_message() const override;
    BackendStats stats() const override;
    ProtocolType protocol() const override { return ProtocolType::VNC_MACOS; }

    bool sync_texture(SDL_Renderer* renderer, SDL_Texture*& io_texture, int& out_w, int& out_h) override;

    void send_mouse_abs(float norm_x, float norm_y) override;
    void send_mouse_button(MouseBtn btn, bool pressed) override;
    void send_mouse_wheel(int16_t dx, int16_t dy) override;
    void send_key(uint16_t hid_usage, bool pressed, uint8_t modifiers) override;
    void send_text_utf8(const std::string& utf8) override;
    void request_full_refresh() override;
    bool cursor_is_text() const override { return cursor_text_.load(); }
    bool cursor_shape_known() const override { return cursor_known_.load(); }

private:
    std::atomic<bool> cursor_text_{false};
    std::atomic<bool> cursor_known_{false};
    ConnectionProfile profile_;
    int sock_ = -1;
    std::atomic<bool> stop_{true};
    std::atomic<BackendState> state_{BackendState::DISCONNECTED};
    std::thread worker_;

    mutable std::mutex info_mu_;
    std::string status_text_;
    std::string error_msg_;
    BackendStats stats_;

    std::mutex send_mu_;
    uint8_t btn_mask_ = 0;
    uint16_t last_x_ = 0;
    uint16_t last_y_ = 0;

    std::mutex fb_mu_;
    int fb_w_ = 0;
    int fb_h_ = 0;
    std::vector<uint32_t> fb_rgba_;
    std::vector<DirtyRect> dirty_rects_;
    bool full_dirty_ = false;

    z_stream zstream_{};
    bool zstream_ok_ = false;
    std::vector<uint8_t> rx_cache_;
    size_t rx_pos_ = 0;
    size_t rx_len_ = 0;
    bool in_fb_batch_ = false;
    std::atomic<uint64_t> rx_bytes_{0};
    std::atomic<bool> want_full_refresh_{false};

    bool send_all(const uint8_t* data, size_t len);
    bool recv_all(uint8_t* data, size_t len);
    void set_status(const std::string& s);
    void set_error(const std::string& e);
    void send_fb_update_request(bool incremental);
    void send_pointer(uint16_t x, uint16_t y, uint8_t mask);
    void send_keysym(uint32_t keysym, bool pressed);

    bool decode_raw_rect(int rx, int ry, int rw, int rh);
    bool decode_copyrect(int rx, int ry, int rw, int rh);
    bool decode_zrle_rect(int rx, int ry, int rw, int rh);
    void mark_dirty(int x, int y, int w, int h);
    void run_session();
};
