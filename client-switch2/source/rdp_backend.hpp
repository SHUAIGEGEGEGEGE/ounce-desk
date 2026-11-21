#pragma once

#include "backend.hpp"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>

class RdpBackend final : public RemoteBackend {
public:
    RdpBackend() = default;
    ~RdpBackend() override;

    bool connect_async(const ConnectionProfile& profile) override;
    void disconnect() override;

    BackendState state() const override { return state_.load(); }
    std::string status_text() const override;
    std::string error_message() const override;
    BackendStats stats() const override;
    ProtocolType protocol() const override { return ProtocolType::RDP_WINDOWS; }

    bool sync_texture(SDL_Renderer* renderer, SDL_Texture*& io_texture, int& out_w, int& out_h) override;

    void send_mouse_abs(float norm_x, float norm_y) override;
    void send_mouse_button(MouseBtn btn, bool pressed) override;
    void send_mouse_wheel(int16_t dx, int16_t dy) override;
    void send_key(uint16_t hid_usage, bool pressed, uint8_t modifiers) override;
    void send_text_utf8(const std::string& utf8) override;
    void request_full_refresh() override;

private:
    ConnectionProfile profile_;
    int sock_ = -1;
    bool use_tls_ = false;
    mbedtls_ssl_context ssl_{};
    mbedtls_ssl_config ssl_conf_{};
    mbedtls_ctr_drbg_context ctr_drbg_{};
    mbedtls_entropy_context entropy_{};
    bool tls_inited_ = false;

    std::atomic<bool> stop_{true};
    std::atomic<BackendState> state_{BackendState::DISCONNECTED};
    std::thread worker_;

    uint16_t mcs_user_id_ = 1004;
    uint16_t mcs_io_channel_ = 1003;
    uint32_t share_id_ = 0;

    mutable std::mutex info_mu_;
    std::string status_text_;
    std::string error_msg_;
    BackendStats stats_;

    std::mutex send_mu_;
    uint16_t last_x_ = 0;
    uint16_t last_y_ = 0;

    std::mutex fb_mu_;
    int fb_w_ = 0;
    int fb_h_ = 0;
    std::vector<uint32_t> fb_rgba_;
    std::vector<DirtyRect> dirty_rects_;
    bool full_dirty_ = false;
    std::atomic<uint64_t> rx_bytes_{0};

    void set_status(const std::string& s);
    void set_error(const std::string& e);

    // Low-level transport (raw TCP before TLS, mbedTLS after upgrade)
    bool raw_send_all(const uint8_t* data, size_t len);
    bool raw_recv_all(uint8_t* data, size_t len);
    bool transport_send(const uint8_t* data, size_t len);
    bool transport_recv(uint8_t* data, size_t len);
    bool upgrade_to_tls();
    void teardown_tls();

    // TPKT / X.224 / MCS / RDP PDU helpers
    bool send_tpkt(const uint8_t* payload, size_t len);
    bool recv_tpkt(std::vector<uint8_t>& out_payload);
    bool send_mcs_send_data(uint16_t channel_id, const uint8_t* data, size_t len);
    bool send_share_control(uint16_t pdu_type, const uint8_t* data, size_t len);
    bool send_share_data(uint8_t pdu_type2, const uint8_t* data, size_t len);
    bool send_fastpath_input(const uint8_t* event_data, size_t len);

    // Protocol phases
    bool do_x224_negotiation(uint32_t& out_selected_proto);
    bool do_credssp_nla();
    bool do_mcs_connect(uint32_t selected_proto);
    bool do_client_info_pdu();
    bool handle_demand_active(const uint8_t* pdu, size_t len);

    // Bitmap & Surface decoding
    void handle_bitmap_update(const uint8_t* data, size_t len);
    void handle_surface_commands(const uint8_t* data, size_t len);
    void blit_bitmap_rect(int left, int top, int width, int height, int bpp, bool compressed,
                          const uint8_t* data, size_t len, int draw_w, int draw_h);
    void blit_nscodec_rect(int left, int top, int width, int height,
                           const uint8_t* data, size_t len, int draw_w, int draw_h);
    void mark_dirty(int x, int y, int w, int h);
    void send_frame_ack(uint32_t frame_id);
    void handle_pointer_attr(const uint8_t* data, size_t len, int xor_bpp, bool large);
    void handle_fastpath_update(uint8_t ucode, const uint8_t* data, size_t len, int& frames_win);

    bool bad_codec_logged_ = false;
    std::vector<uint8_t> fp_frag_buf_;   // Fast-Path fragment reassembly buffer
    uint8_t fp_frag_code_ = 0;
    std::vector<uint8_t> rx_cache_;      // 64KB userland read-ahead buffer (eliminates 1-byte syscall overhead!)
    size_t rx_pos_ = 0;
    size_t rx_len_ = 0;
    bool in_surface_frame_ = false;      // Batch dirty rects inside SurfaceFrameMarker Begin..End!
    std::vector<DirtyRect> pending_frame_rects_;
    bool pending_full_dirty_ = false;
    bool srv_frame_ack_ = false;
    std::vector<uint8_t> ptr_cache_text_;  // pointer cache index -> is I-beam text cursor
    std::atomic<bool> cursor_text_{false};
    std::atomic<bool> cursor_known_{false};

public:
    bool cursor_is_text() const override { return cursor_text_.load(); }
    bool cursor_shape_known() const override { return cursor_known_.load(); }

private:

    void run_session();
};
