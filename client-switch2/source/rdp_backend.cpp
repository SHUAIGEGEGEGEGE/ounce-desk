#include "rdp_backend.hpp"

#include "crypto_utils.hpp"
#include "cursor_detect.hpp"
#include "keymap.hpp"
#include "platform.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>

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

inline uint16_t le16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
inline uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline void push_u8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
inline void push_le16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(uint8_t(v & 0xFF));
    b.push_back(uint8_t((v >> 8) & 0xFF));
}
inline void push_be16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(uint8_t((v >> 8) & 0xFF));
    b.push_back(uint8_t(v & 0xFF));
}
inline void push_le32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(uint8_t(v & 0xFF));
    b.push_back(uint8_t((v >> 8) & 0xFF));
    b.push_back(uint8_t((v >> 16) & 0xFF));
    b.push_back(uint8_t((v >> 24) & 0xFF));
}
inline void push_le64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(uint8_t((v >> (i * 8)) & 0xFF));
}

// Convert 16bpp RGB565 pixel to SDL ABGR8888 (R in low byte)
inline uint32_t rgb565_to_rgba(uint16_t px) {
    uint8_t r = static_cast<uint8_t>(((px >> 11) & 0x1F) * 255 / 31);
    uint8_t g = static_cast<uint8_t>(((px >> 5) & 0x3F) * 255 / 63);
    uint8_t b = static_cast<uint8_t>((px & 0x1F) * 255 / 31);
    return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | 0xFF000000u;
}

inline uint32_t bgr24_to_rgba(const uint8_t* p) {
    return uint32_t(p[2]) | (uint32_t(p[1]) << 8) | (uint32_t(p[0]) << 16) | 0xFF000000u;
}

inline uint32_t rgb555_to_rgba(uint16_t px) {
    uint8_t r = static_cast<uint8_t>(((px >> 10) & 0x1F) * 255 / 31);
    uint8_t g = static_cast<uint8_t>(((px >> 5) & 0x1F) * 255 / 31);
    uint8_t b = static_cast<uint8_t>((px & 0x1F) * 255 / 31);
    return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | 0xFF000000u;
}

// Convert a raw native RDP pixel value (15/16/24bpp, little-endian BGR) to SDL ABGR8888
inline uint32_t native_to_rgba(uint32_t v, int bpp) {
    if (bpp == 15) return rgb555_to_rgba(static_cast<uint16_t>(v));
    if (bpp == 16) return rgb565_to_rgba(static_cast<uint16_t>(v));
    // 24/32bpp: byte0 = B, byte1 = G, byte2 = R
    uint8_t b = static_cast<uint8_t>(v & 0xFF);
    uint8_t g = static_cast<uint8_t>((v >> 8) & 0xFF);
    uint8_t r = static_cast<uint8_t>((v >> 16) & 0xFF);
    return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | 0xFF000000u;
}

// MS-RDPBCGR 2.2.9.1.1.3.1.2.4 Interleaved RLE Bitmap Decompressor (8/15/16/24bpp).
// Exact port of FreeRDP's RleDecompress: works on NATIVE pixel values (XOR must be done in native
// format, NOT after RGBA conversion), output is in stored (bottom-up) row order, width = full
// TS_BITMAP_DATA width (incl. padding).
bool decompress_interleaved(const uint8_t* src, size_t src_len, int width, int height, int bpp,
                            std::vector<uint32_t>& out_rgba) {
    const int bytes_pp = (bpp + 7) / 8;
    if (bytes_pp < 1 || bytes_pp > 3 || width <= 0 || height <= 0) return false;
    const size_t total = static_cast<size_t>(width) * height;
    std::vector<uint32_t> px(total, 0);

    uint32_t white = (bpp == 8) ? 0xFFu : (bpp == 15) ? 0x7FFFu : (bpp == 16) ? 0xFFFFu : 0xFFFFFFu;
    uint32_t fg = white;
    bool insert_fg = false;
    bool first_line = true;
    size_t pos = 0;
    size_t dst = 0;
    const size_t row = static_cast<size_t>(width);

    auto rd_pix = [&](uint32_t& v) -> bool {
        if (pos + bytes_pp > src_len) return false;
        v = src[pos];
        if (bytes_pp >= 2) v |= uint32_t(src[pos + 1]) << 8;
        if (bytes_pp >= 3) v |= uint32_t(src[pos + 2]) << 16;
        pos += bytes_pp;
        return true;
    };
    auto put = [&](uint32_t v) {
        if (dst < total) px[dst] = v;
        ++dst;
    };
    auto above = [&]() -> uint32_t { return (dst >= row && dst - row < total) ? px[dst - row] : 0u; };
    auto fgbg = [&](uint8_t mask, uint32_t nbits) {
        for (uint32_t b = 0; b < nbits; ++b) {
            bool set = (mask >> b) & 1u;
            if (first_line) put(set ? fg : 0u);
            else { uint32_t a = above(); put(set ? (a ^ fg) : a); }
        }
    };

    while (pos < src_len && dst < total) {
        if (first_line && dst >= row) {
            first_line = false;
            insert_fg = false;
        }
        uint8_t hdr = src[pos];
        uint32_t code;
        if ((hdr & 0xC0u) != 0xC0u) code = hdr >> 5;          // REGULAR
        else if ((hdr & 0xF0u) == 0xF0u) code = hdr;          // MEGA_MEGA / SPECIAL
        else code = hdr >> 4;                                 // LITE

        uint32_t run = 0;
        // ---- run-length extraction ----
        auto run_regular = [&]() -> bool {
            run = hdr & 0x1Fu;
            if (run == 0) { if (pos + 1 >= src_len) return false; run = src[pos + 1] + 32u; pos += 2; }
            else pos += 1;
            return true;
        };
        auto run_regular_fgbg = [&]() -> bool {
            run = hdr & 0x1Fu;
            if (run == 0) { if (pos + 1 >= src_len) return false; run = src[pos + 1] + 1u; pos += 2; }
            else { run *= 8u; pos += 1; }
            return true;
        };
        auto run_lite = [&]() -> bool {
            run = hdr & 0x0Fu;
            if (run == 0) { if (pos + 1 >= src_len) return false; run = src[pos + 1] + 16u; pos += 2; }
            else pos += 1;
            return true;
        };
        auto run_lite_fgbg = [&]() -> bool {
            run = hdr & 0x0Fu;
            if (run == 0) { if (pos + 1 >= src_len) return false; run = src[pos + 1] + 1u; pos += 2; }
            else { run *= 8u; pos += 1; }
            return true;
        };
        auto run_mega = [&]() -> bool {
            if (pos + 2 >= src_len) return false;
            run = uint32_t(src[pos + 1]) | (uint32_t(src[pos + 2]) << 8);
            pos += 3;
            return true;
        };

        if (code == 0x00 || code == 0xF0) {
            // Background run
            if (!(code == 0x00 ? run_regular() : run_mega())) break;
            if (first_line) {
                if (insert_fg && run > 0) { put(fg); --run; }
                while (run-- > 0) put(0u);
            } else {
                if (insert_fg && run > 0) { put(above() ^ fg); --run; }
                while (run-- > 0) put(above());
            }
            insert_fg = true;
            continue;
        }
        insert_fg = false;

        switch (code) {
            case 0x01: case 0xF1: case 0x0C: case 0xF6: {
                bool ok = (code == 0x01) ? run_regular() : (code == 0x0C) ? run_lite() : run_mega();
                if (!ok) return true;
                if (code == 0x0C || code == 0xF6) { if (!rd_pix(fg)) return true; }
                if (first_line) { while (run-- > 0) put(fg); }
                else { while (run-- > 0) put(above() ^ fg); }
                break;
            }
            case 0x0E: case 0xF8: {
                bool ok = (code == 0x0E) ? run_lite() : run_mega();
                if (!ok) return true;
                uint32_t a = 0, b = 0;
                if (!rd_pix(a) || !rd_pix(b)) return true;
                while (run-- > 0) { put(a); put(b); }
                break;
            }
            case 0x03: case 0xF3: {
                bool ok = (code == 0x03) ? run_regular() : run_mega();
                if (!ok) return true;
                uint32_t a = 0;
                if (!rd_pix(a)) return true;
                while (run-- > 0) put(a);
                break;
            }
            case 0x02: case 0xF2: case 0x0D: case 0xF7: {
                bool ok = (code == 0x02) ? run_regular_fgbg() : (code == 0x0D) ? run_lite_fgbg() : run_mega();
                if (!ok) return true;
                if (code == 0x0D || code == 0xF7) { if (!rd_pix(fg)) return true; }
                while (run > 8) {
                    if (pos >= src_len) return true;
                    fgbg(src[pos++], 8);
                    run -= 8;
                }
                if (run > 0) {
                    if (pos >= src_len) return true;
                    fgbg(src[pos++], run);
                }
                break;
            }
            case 0x04: case 0xF4: {
                bool ok = (code == 0x04) ? run_regular() : run_mega();
                if (!ok) return true;
                while (run-- > 0) {
                    uint32_t a = 0;
                    if (!rd_pix(a)) return true;
                    put(a);
                }
                break;
            }
            case 0xF9: pos += 1; fgbg(0x03, 8); break;   // SPECIAL_FGBG_1
            case 0xFA: pos += 1; fgbg(0x05, 8); break;   // SPECIAL_FGBG_2
            case 0xFD: pos += 1; put(white); break;      // WHITE
            case 0xFE: pos += 1; put(0u); break;         // BLACK
            default:
                return true;  // invalid order: stop decoding, keep what we have
        }
    }

    out_rgba.resize(total);
    for (size_t i = 0; i < total; ++i) out_rgba[i] = native_to_rgba(px[i], bpp);
    return true;
}

// MS-RDPEGDI 2.2.2.5.1 RDP 6.0 Planar Bitmap Codec (used by Windows for compressed 32bpp bitmaps).
// Output in stored (bottom-up) row order like interleaved.
bool decompress_planar(const uint8_t* src, size_t src_len, int width, int height,
                       std::vector<uint32_t>& out_rgba) {
    if (src_len < 1 || width <= 0 || height <= 0) return false;
    const uint8_t hdr = src[0];
    const int cll = hdr & 0x07;
    const bool cs = (hdr & 0x08) != 0;
    const bool rle = (hdr & 0x10) != 0;
    const bool no_alpha = (hdr & 0x20) != 0;
    if (cll != 0 || cs) return false;  // YCoCg / chroma subsampling not negotiated by us

    const size_t plane_sz = static_cast<size_t>(width) * height;
    std::vector<uint8_t> planes[4];  // A, R, G, B
    for (auto& p : planes) p.assign(plane_sz, 0);
    size_t pos = 1;

    auto decode_rle_plane = [&](std::vector<uint8_t>& out) -> bool {
        for (int y = 0; y < height; ++y) {
            uint8_t* cur = out.data() + static_cast<size_t>(y) * width;
            const uint8_t* prev = (y > 0) ? cur - width : nullptr;
            int x = 0;
            int pixel = 0;
            while (x < width) {
                if (pos >= src_len) return false;
                uint8_t ctrl = src[pos++];
                int n_run = ctrl & 0x0F;
                int n_raw = (ctrl >> 4) & 0x0F;
                if (n_run == 1) { n_run = n_raw + 16; n_raw = 0; }
                else if (n_run == 2) { n_run = n_raw + 32; n_raw = 0; }
                if (pos + static_cast<size_t>(n_raw) > src_len) return false;
                if (!prev) {
                    while (n_raw-- > 0) {
                        pixel = src[pos++];
                        if (x < width) cur[x] = static_cast<uint8_t>(pixel);
                        ++x;
                    }
                    while (n_run-- > 0) {
                        if (x < width) cur[x] = static_cast<uint8_t>(pixel);
                        ++x;
                    }
                } else {
                    while (n_raw-- > 0) {
                        int d = src[pos++];
                        pixel = (d & 1) ? -((d >> 1) + 1) : (d >> 1);
                        if (x < width) cur[x] = static_cast<uint8_t>(prev[x] + pixel);
                        ++x;
                    }
                    while (n_run-- > 0) {
                        if (x < width) cur[x] = static_cast<uint8_t>(prev[x] + pixel);
                        ++x;
                    }
                }
            }
        }
        return true;
    };

    const int first = no_alpha ? 1 : 0;
    for (int p = first; p < 4; ++p) {
        if (rle) {
            if (!decode_rle_plane(planes[p])) break;
        } else {
            if (pos + plane_sz > src_len) break;
            std::memcpy(planes[p].data(), src + pos, plane_sz);
            pos += plane_sz;
        }
    }

    out_rgba.resize(plane_sz);
    for (size_t i = 0; i < plane_sz; ++i) {
        out_rgba[i] = uint32_t(planes[1][i]) | (uint32_t(planes[2][i]) << 8) |
                      (uint32_t(planes[3][i]) << 16) | 0xFF000000u;
    }
    return true;
}

// Custom mbedTLS TCP BIO callbacks
int bio_tcp_send(void* ctx, const unsigned char* buf, size_t len) {
    int sock = *static_cast<int*>(ctx);
    int n = static_cast<int>(send(sock, reinterpret_cast<const char*>(buf), static_cast<int>(len), 0));
    if (n < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return n;
}

int bio_tcp_recv(void* ctx, unsigned char* buf, size_t len) {
    int sock = *static_cast<int*>(ctx);
    int n = static_cast<int>(recv(sock, reinterpret_cast<char*>(buf), static_cast<int>(len), 0));
    if (n == 0) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    if (n < 0) {
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
#endif
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return n;
}

// Extract raw SubjectPublicKey BIT STRING content from peer X.509 cert for CredSSP pubKeyAuth
std::vector<uint8_t> extract_peer_pubkey_bytes(mbedtls_ssl_context* ssl) {
    const mbedtls_x509_crt* crt = mbedtls_ssl_get_peer_cert(ssl);
    if (!crt) return {};
    uint8_t der[2048];
    int len = mbedtls_pk_write_pubkey_der(const_cast<mbedtls_pk_context*>(&crt->pk), der, sizeof(der));
    if (len <= 0) return {};
    // mbedtls_pk_write_pubkey_der writes at the END of buffer
    const uint8_t* spki = der + sizeof(der) - len;
    size_t pos = 0;
    uint8_t tag = 0;
    const uint8_t* seq_val = nullptr;
    size_t seq_len = 0;
    if (!crypto::der_read_tlv(spki, static_cast<size_t>(len), pos, tag, seq_val, seq_len) || tag != 0x30) {
        return {};
    }
    // Inside SPKI SEQUENCE: [AlgorithmIdentifier SEQUENCE, subjectPublicKey BIT STRING]
    size_t spos = 0;
    const uint8_t* alg_val = nullptr;
    size_t alg_len = 0;
    if (!crypto::der_read_tlv(seq_val, seq_len, spos, tag, alg_val, alg_len)) return {};
    const uint8_t* bit_val = nullptr;
    size_t bit_len = 0;
    if (!crypto::der_read_tlv(seq_val, seq_len, spos, tag, bit_val, bit_len) || tag != 0x03 || bit_len < 2) {
        return {};
    }
    // Skip first unused-bits byte (0x00) of BIT STRING
    return std::vector<uint8_t>(bit_val + 1, bit_val + bit_len);
}

// NTLMv2 GSS_Wrap (Sign + RC4 Seal) used by CredSSP pubKeyAuth & authInfo
std::vector<uint8_t> ntlm_seal_message(
    const std::array<uint8_t, 16>& sign_key,
    crypto::RC4State& seal_rc4,
    uint32_t seq_num,
    const uint8_t* plain, size_t len) {

    std::vector<uint8_t> to_mac;
    to_mac.reserve(4 + len);
    push_le32(to_mac, seq_num);
    to_mac.insert(to_mac.end(), plain, plain + len);
    auto mac = crypto::hmac_md5(sign_key.data(), 16, to_mac.data(), to_mac.size());

    // Encrypt plaintext first, then encrypt 8-byte checksum with the same RC4 keystream
    std::vector<uint8_t> cipher(plain, plain + len);
    seal_rc4.process(cipher.data(), cipher.size());

    uint8_t cksum[8];
    std::memcpy(cksum, mac.data(), 8);
    seal_rc4.process(cksum, 8);

    std::vector<uint8_t> out;
    out.reserve(16 + cipher.size());
    push_le32(out, 1);  // NTLM Signature Version = 1
    out.insert(out.end(), cksum, cksum + 8);
    push_le32(out, seq_num);
    out.insert(out.end(), cipher.begin(), cipher.end());
    return out;
}

std::vector<uint8_t> build_ts_request(
    uint32_t version,
    const std::vector<uint8_t>* nego_token,
    const std::vector<uint8_t>* auth_info,
    const std::vector<uint8_t>* pub_key_auth) {

    std::vector<uint8_t> body;
    auto v = crypto::der_wrap(0xA0, crypto::der_integer(version));
    body.insert(body.end(), v.begin(), v.end());

    if (nego_token && !nego_token->empty()) {
        auto tok_oct = crypto::der_wrap(0x04, *nego_token);
        auto tok_field = crypto::der_wrap(0xA0, tok_oct);
        auto nego_item = crypto::der_wrap(0x30, tok_field);
        auto nego_seq = crypto::der_wrap(0x30, nego_item);
        auto nego_ctx = crypto::der_wrap(0xA1, nego_seq);
        body.insert(body.end(), nego_ctx.begin(), nego_ctx.end());
    }
    if (auth_info && !auth_info->empty()) {
        auto ai = crypto::der_wrap(0xA2, crypto::der_wrap(0x04, *auth_info));
        body.insert(body.end(), ai.begin(), ai.end());
    }
    if (pub_key_auth && !pub_key_auth->empty()) {
        auto pk = crypto::der_wrap(0xA3, crypto::der_wrap(0x04, *pub_key_auth));
        body.insert(body.end(), pk.begin(), pk.end());
    }
    return crypto::der_wrap(0x30, body);
}

bool extract_first_ts_nego_token(const uint8_t* der, size_t len, std::vector<uint8_t>& out_tok) {
    size_t pos = 0;
    uint8_t tag = 0;
    const uint8_t* val = nullptr;
    size_t vlen = 0;
    if (!crypto::der_read_tlv(der, len, pos, tag, val, vlen) || tag != 0x30) return false;
    size_t p = 0;
    while (p < vlen) {
        const uint8_t* fv = nullptr;
        size_t fl = 0;
        if (!crypto::der_read_tlv(val, vlen, p, tag, fv, fl)) break;
        if (tag == 0xA1) {
            // [1] NegoData SEQUENCE OF SEQUENCE { [0] OCTET STRING }
            size_t p1 = 0;
            const uint8_t* v1 = nullptr; size_t l1 = 0;
            if (!crypto::der_read_tlv(fv, fl, p1, tag, v1, l1) || tag != 0x30) return false;
            size_t p2 = 0;
            const uint8_t* v2 = nullptr; size_t l2 = 0;
            if (!crypto::der_read_tlv(v1, l1, p2, tag, v2, l2) || tag != 0x30) return false;
            size_t p3 = 0;
            const uint8_t* v3 = nullptr; size_t l3 = 0;
            if (!crypto::der_read_tlv(v2, l2, p3, tag, v3, l3) || tag != 0xA0) return false;
            size_t p4 = 0;
            const uint8_t* v4 = nullptr; size_t l4 = 0;
            if (!crypto::der_read_tlv(v3, l3, p4, tag, v4, l4) || tag != 0x04) return false;
            out_tok.assign(v4, v4 + l4);
            return true;
        }
    }
    return false;
}

}  // namespace

RdpBackend::~RdpBackend() { disconnect(); }

void RdpBackend::set_status(const std::string& s) {
    platform::log_line("[RDP] " + s);
    std::lock_guard<std::mutex> lk(info_mu_);
    status_text_ = s;
}

void RdpBackend::set_error(const std::string& e) {
    platform::log_line("[RDP ERROR] " + e);
    {
        std::lock_guard<std::mutex> lk(info_mu_);
        error_msg_ = e;
        status_text_ = "错误: " + e;
    }
    state_.store(BackendState::ERROR_STATE);
}

std::string RdpBackend::status_text() const {
    std::lock_guard<std::mutex> lk(info_mu_);
    return status_text_;
}

std::string RdpBackend::error_message() const {
    std::lock_guard<std::mutex> lk(info_mu_);
    return error_msg_;
}

BackendStats RdpBackend::stats() const {
    std::lock_guard<std::mutex> lk(info_mu_);
    return stats_;
}

bool RdpBackend::connect_async(const ConnectionProfile& profile) {
    disconnect();
    profile_ = profile;
    if (profile_.port == 0) profile_.port = 3389;
    if (profile_.req_width < 640) profile_.req_width = 1920;
    if (profile_.req_height < 480) profile_.req_height = 1080;

    stop_.store(false);
    state_.store(BackendState::CONNECTING);
    set_status("Connecting to " + profile_.host + ":" + std::to_string(profile_.port) + " (Windows RDP)...");
    worker_ = std::thread(&RdpBackend::run_session, this);
    return true;
}

void RdpBackend::teardown_tls() {
    if (tls_inited_) {
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&ssl_conf_);
        mbedtls_ctr_drbg_free(&ctr_drbg_);
        mbedtls_entropy_free(&entropy_);
        tls_inited_ = false;
    }
    use_tls_ = false;
}

void RdpBackend::disconnect() {
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
    teardown_tls();
    state_.store(BackendState::DISCONNECTED);
}

bool RdpBackend::raw_send_all(const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len && !stop_.load()) {
        int n = static_cast<int>(send(sock_, reinterpret_cast<const char*>(data + sent), static_cast<int>(len - sent), 0));
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return sent == len;
}

bool RdpBackend::raw_recv_all(uint8_t* data, size_t len) {
    size_t got = 0;
    while (got < len && !stop_.load()) {
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
    }
    return got == len;
}

bool RdpBackend::transport_send(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lk(send_mu_);
    if (sock_ < 0) return false;
    if (!use_tls_) return raw_send_all(data, len);
    size_t sent = 0;
    while (sent < len && !stop_.load()) {
        int n = mbedtls_ssl_write(&ssl_, data + sent, len - sent);
        if (n > 0) sent += static_cast<size_t>(n);
        else if (n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ) continue;
        else return false;
    }
    return sent == len;
}

bool RdpBackend::transport_recv(uint8_t* data, size_t len) {
    if (sock_ < 0) return false;
    if (!use_tls_) return raw_recv_all(data, len);
    size_t got = 0;
    while (got < len && !stop_.load()) {
        int n = mbedtls_ssl_read(&ssl_, data + got, len - got);
        if (n > 0) {
            got += static_cast<size_t>(n);
            rx_bytes_.fetch_add(static_cast<uint64_t>(n));
        } else if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        } else {
            return false;
        }
    }
    return got == len;
}

bool RdpBackend::upgrade_to_tls() {
    teardown_tls();
    mbedtls_ssl_init(&ssl_);
    mbedtls_ssl_config_init(&ssl_conf_);
    mbedtls_ctr_drbg_init(&ctr_drbg_);
    mbedtls_entropy_init(&entropy_);
    tls_inited_ = true;

    const char* pers = "OunceDeskRDP";
    if (mbedtls_ctr_drbg_seed(&ctr_drbg_, mbedtls_entropy_func, &entropy_,
                              reinterpret_cast<const unsigned char*>(pers), std::strlen(pers)) != 0) {
        return false;
    }
    if (mbedtls_ssl_config_defaults(&ssl_conf_, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        return false;
    }
    // Windows RDP uses a machine self-signed certificate by default
    mbedtls_ssl_conf_authmode(&ssl_conf_, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&ssl_conf_, mbedtls_ctr_drbg_random, &ctr_drbg_);
    if (mbedtls_ssl_setup(&ssl_, &ssl_conf_) != 0) return false;
    mbedtls_ssl_set_hostname(&ssl_, profile_.host.c_str());
    mbedtls_ssl_set_bio(&ssl_, &sock_, bio_tcp_send, bio_tcp_recv, nullptr);

    while (!stop_.load()) {
        int rc = mbedtls_ssl_handshake(&ssl_);
        if (rc == 0) {
            use_tls_ = true;
            return true;
        }
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            return false;
        }
    }
    return false;
}

bool RdpBackend::send_tpkt(const uint8_t* payload, size_t len) {
    uint16_t total = static_cast<uint16_t>(4 + len);
    std::vector<uint8_t> pkt;
    pkt.reserve(total);
    pkt.push_back(0x03);
    pkt.push_back(0x00);
    push_be16(pkt, total);
    pkt.insert(pkt.end(), payload, payload + len);
    return transport_send(pkt.data(), pkt.size());
}

bool RdpBackend::recv_tpkt(std::vector<uint8_t>& out_payload) {
    uint8_t hdr[4];
    if (!transport_recv(hdr, 4) || hdr[0] != 0x03) return false;
    uint16_t total = static_cast<uint16_t>((hdr[2] << 8) | hdr[3]);
    if (total < 4) return false;
    out_payload.resize(total - 4);
    if (total > 4 && !transport_recv(out_payload.data(), total - 4)) return false;
    return true;
}

bool RdpBackend::send_mcs_send_data(uint16_t channel_id, const uint8_t* data, size_t len) {
    std::vector<uint8_t> body;
    body.reserve(16 + len);
    // X.224 Data TPDU
    body.push_back(0x02);
    body.push_back(0xF0);
    body.push_back(0x80);
    // MCS SendDataRequest (0x64)
    body.push_back(0x64);
    push_be16(body, static_cast<uint16_t>(mcs_user_id_ - 1001));
    push_be16(body, channel_id);
    body.push_back(0x70);  // dataPriority=high, segmentation=begin|end
    if (len < 0x80) {
        body.push_back(static_cast<uint8_t>(len));
    } else {
        push_be16(body, static_cast<uint16_t>(0x8000u | len));
    }
    body.insert(body.end(), data, data + len);
    return send_tpkt(body.data(), body.size());
}

bool RdpBackend::send_share_control(uint16_t pdu_type, const uint8_t* data, size_t len) {
    std::vector<uint8_t> pdu;
    pdu.reserve(6 + len);
    push_le16(pdu, static_cast<uint16_t>(6 + len));
    push_le16(pdu, static_cast<uint16_t>((1u << 4) | (pdu_type & 0x0Fu)));  // version 1
    push_le16(pdu, mcs_user_id_);
    pdu.insert(pdu.end(), data, data + len);
    return send_mcs_send_data(mcs_io_channel_, pdu.data(), pdu.size());
}

bool RdpBackend::send_share_data(uint8_t pdu_type2, const uint8_t* data, size_t len) {
    std::vector<uint8_t> body;
    body.reserve(12 + len);
    push_le32(body, share_id_);
    push_u8(body, 0);     // pad1
    push_u8(body, 1);     // streamId = STREAM_LOW
    push_le16(body, static_cast<uint16_t>(4 + len));  // uncompressedLength includes pduType2..compressedLength (4 bytes)
    push_u8(body, pdu_type2);
    push_u8(body, 0);     // compressedType
    push_le16(body, 0);   // compressedLength
    body.insert(body.end(), data, data + len);
    return send_share_control(0x07, body.data(), body.size());  // PDUTYPE_DATAPDU
}

bool RdpBackend::send_fastpath_input(const uint8_t* event_data, size_t len) {
    if (state_.load() != BackendState::STREAMING) return false;
    std::vector<uint8_t> pkt;
    // fpInputHeader: action=0 (FASTPATH_INPUT_ACTION_FASTPATH), numEvents=1 (bits 2..5)
    pkt.push_back(static_cast<uint8_t>(1 << 2));
    size_t total = 2 + len;
    if (total < 0x80) {
        pkt.push_back(static_cast<uint8_t>(total));
    } else {
        total = 3 + len;
        push_be16(pkt, static_cast<uint16_t>(0x8000u | total));
    }
    pkt.insert(pkt.end(), event_data, event_data + len);
    return transport_send(pkt.data(), pkt.size());
}

bool RdpBackend::do_x224_negotiation(uint32_t& out_selected_proto) {
    std::vector<uint8_t> x224;
    std::string cookie = "Cookie: mstshash=" + (profile_.username.empty() ? std::string("OunceDesk") : profile_.username.substr(0, 9)) + "\r\n";

    // Requested protocols: HYBRID (NLA=0x02) + SSL (TLS=0x01) when username provided; SSL-only if blank
    uint32_t req_proto = profile_.username.empty() ? 0x00000001u : 0x00000003u;

    uint8_t tpdu_len = static_cast<uint8_t>(6 + cookie.size() + 8);
    x224.push_back(tpdu_len);
    x224.push_back(0xE0);  // CR-TPDU
    push_be16(x224, 0);    // DST-REF
    push_be16(x224, 0);    // SRC-REF
    x224.push_back(0x00);  // CLASS 0
    x224.insert(x224.end(), cookie.begin(), cookie.end());
    // RDP_NEG_REQ (8 bytes)
    x224.push_back(0x01);  // TYPE_RDP_NEG_REQ
    x224.push_back(0x00);  // flags
    push_le16(x224, 8);    // length
    push_le32(x224, req_proto);

    if (!send_tpkt(x224.data(), x224.size())) return false;

    std::vector<uint8_t> rsp;
    if (!recv_tpkt(rsp) || rsp.size() < 7 || rsp[1] != 0xD0) return false;  // CC-TPDU

    out_selected_proto = 0;
    if (rsp.size() >= 15 && rsp[7] == 0x02) {
        out_selected_proto = le32(rsp.data() + 11);
    } else if (rsp.size() >= 15 && rsp[7] == 0x03) {
        uint32_t fail_code = le32(rsp.data() + 11);
        set_error("Windows RDP rejected security mode (code " + std::to_string(fail_code) + "; enter Windows username/password for NLA)");
        return false;
    }
    return true;
}

bool RdpBackend::do_credssp_nla() {
    set_status("Authenticating (Windows CredSSP / NLA)...");
    auto peer_pubkey = extract_peer_pubkey_bytes(&ssl_);
    if (peer_pubkey.empty()) {
        set_error("Failed extracting Windows TLS public key for CredSSP");
        return false;
    }

    // 1. NTLM NEGOTIATE_MESSAGE (Type 1)
    std::vector<uint8_t> neg;
    const char sig[8] = {'N','T','L','M','S','S','P','\0'};
    neg.insert(neg.end(), sig, sig + 8);
    push_le32(neg, 1);  // MessageType = NEGOTIATE
    uint32_t neg_flags =
        0x00000001u |  // NTLMSSP_NEGOTIATE_UNICODE
        0x00000004u |  // NTLMSSP_REQUEST_TARGET
        0x00000010u |  // NTLMSSP_NEGOTIATE_SIGN
        0x00000020u |  // NTLMSSP_NEGOTIATE_SEAL
        0x00000200u |  // NTLMSSP_NEGOTIATE_NTLM
        0x00008000u |  // NTLMSSP_NEGOTIATE_ALWAYS_SIGN
        0x00080000u |  // NTLMSSP_NEGOTIATE_EXTENDED_SESSIONSECURITY
        0x02000000u |  // NTLMSSP_NEGOTIATE_VERSION
        0x20000000u |  // NTLMSSP_NEGOTIATE_128
        0x40000000u |  // NTLMSSP_NEGOTIATE_KEY_EXCH
        0x80000000u;   // NTLMSSP_NEGOTIATE_56
    push_le32(neg, neg_flags);
    push_le64(neg, 0);  // DomainNameFields
    push_le64(neg, 0);  // WorkstationFields
    // Version (Windows 10.0.19041, NTLM rev 15)
    push_u8(neg, 10); push_u8(neg, 0); push_le16(neg, 19041);
    push_u8(neg, 0); push_u8(neg, 0); push_u8(neg, 0); push_u8(neg, 15);

    auto ts1 = build_ts_request(3, &neg, nullptr, nullptr);
    if (!transport_send(ts1.data(), ts1.size())) return false;

    // 2. Receive Server TSRequest containing NTLM CHALLENGE_MESSAGE (Type 2)
    uint8_t der_hdr[4];
    if (!transport_recv(der_hdr, 2) || der_hdr[0] != 0x30) return false;
    size_t body_len = 0;
    size_t hdr_len = 2;
    if ((der_hdr[1] & 0x80) == 0) {
        body_len = der_hdr[1];
    } else {
        int nb = der_hdr[1] & 0x7F;
        if (nb == 0 || nb > 2 || !transport_recv(der_hdr + 2, nb)) return false;
        hdr_len = 2 + nb;
        for (int i = 0; i < nb; ++i) body_len = (body_len << 8) | der_hdr[2 + i];
    }
    std::vector<uint8_t> ts2(hdr_len + body_len);
    std::memcpy(ts2.data(), der_hdr, hdr_len);
    if (!transport_recv(ts2.data() + hdr_len, body_len)) return false;

    std::vector<uint8_t> chal;
    if (!extract_first_ts_nego_token(ts2.data(), ts2.size(), chal) || chal.size() < 48) {
        set_error("Invalid NTLM challenge from Windows");
        return false;
    }

    uint8_t srv_challenge[8];
    std::memcpy(srv_challenge, chal.data() + 24, 8);
    uint16_t ti_len = le16(chal.data() + 40);
    uint32_t ti_off = le32(chal.data() + 44);
    std::vector<uint8_t> target_info;
    if (ti_len > 0 && ti_off + ti_len <= chal.size()) {
        target_info.assign(chal.begin() + ti_off, chal.begin() + ti_off + ti_len);
    }

    // 3. Compute NTLMv2 Proof, SessionBaseKey, ExportedSessionKey, Sign/Seal Keys
    auto pass_u16 = crypto::utf8_to_utf16le(profile_.password, false);
    auto nt_hash = crypto::md4(pass_u16.data(), pass_u16.size());
    auto user_dom_u16 = crypto::utf8_to_utf16le(profile_.username, true);
    auto dom_u16 = crypto::utf8_to_utf16le(profile_.domain, false);
    user_dom_u16.insert(user_dom_u16.end(), dom_u16.begin(), dom_u16.end());
    auto resp_key_nt = crypto::hmac_md5(nt_hash.data(), 16, user_dom_u16.data(), user_dom_u16.size());

    // Windows FILETIME (100-ns ticks since 1601-01-01)
    uint64_t unix_sec = static_cast<uint64_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    uint64_t win_ft = (unix_sec + 11644473600ULL) * 10000000ULL;

    uint8_t cli_chal[8] = {0x4F, 0x55, 0x4E, 0x43, 0x45, 0x32, 0x52, 0x44};
    mbedtls_ctr_drbg_random(&ctr_drbg_, cli_chal, 8);

    std::vector<uint8_t> temp;
    push_u8(temp, 1); push_u8(temp, 1);  // RespType, HiRespType
    push_le16(temp, 0); push_le32(temp, 0);
    push_le64(temp, win_ft);
    temp.insert(temp.end(), cli_chal, cli_chal + 8);
    push_le32(temp, 0);
    temp.insert(temp.end(), target_info.begin(), target_info.end());
    push_le32(temp, 0);  // MsvAvEOL padding

    std::vector<uint8_t> proof_in;
    proof_in.insert(proof_in.end(), srv_challenge, srv_challenge + 8);
    proof_in.insert(proof_in.end(), temp.begin(), temp.end());
    auto nt_proof = crypto::hmac_md5(resp_key_nt.data(), 16, proof_in.data(), proof_in.size());

    std::vector<uint8_t> nt_resp;
    nt_resp.insert(nt_resp.end(), nt_proof.begin(), nt_proof.end());
    nt_resp.insert(nt_resp.end(), temp.begin(), temp.end());

    auto session_base_key = crypto::hmac_md5(resp_key_nt.data(), 16, nt_proof.data(), 16);

    uint8_t exported_key[16];
    mbedtls_ctr_drbg_random(&ctr_drbg_, exported_key, 16);
    uint8_t enc_session_key[16];
    std::memcpy(enc_session_key, exported_key, 16);
    crypto::RC4State kx_rc4;
    kx_rc4.init(session_base_key.data(), 16);
    kx_rc4.process(enc_session_key, 16);

    // Derive NTLMv2 Client Sign & Seal keys
    auto derive_key = [&](const char* magic_str) {
        std::vector<uint8_t> k(exported_key, exported_key + 16);
        k.insert(k.end(), magic_str, magic_str + std::strlen(magic_str) + 1);
        return crypto::md5(k.data(), k.size());
    };
    auto cli_sign_key = derive_key("session key to client-to-server signing key magic constant");
    auto cli_seal_key = derive_key("session key to client-to-server sealing key magic constant");

    crypto::RC4State cli_seal_rc4;
    cli_seal_rc4.init(cli_seal_key.data(), 16);

    // Build NTLM AUTHENTICATE_MESSAGE (Type 3)
    auto user_u16 = crypto::utf8_to_utf16le(profile_.username, false);
    auto ws_u16 = crypto::utf8_to_utf16le("SWITCH2", false);
    uint8_t lm_resp[24]{};

    std::vector<uint8_t> auth;
    auth.insert(auth.end(), sig, sig + 8);
    push_le32(auth, 3);  // MessageType = AUTHENTICATE

    uint32_t payload_off = 72;  // Header + Version(8)
    auto push_secbuf = [&](size_t len, uint32_t& off) {
        push_le16(auth, static_cast<uint16_t>(len));
        push_le16(auth, static_cast<uint16_t>(len));
        push_le32(auth, off);
        off += static_cast<uint32_t>(len);
    };
    uint32_t cur_off = payload_off;
    push_secbuf(sizeof(lm_resp), cur_off);
    push_secbuf(nt_resp.size(), cur_off);
    push_secbuf(dom_u16.size(), cur_off);
    push_secbuf(user_u16.size(), cur_off);
    push_secbuf(ws_u16.size(), cur_off);
    push_secbuf(sizeof(enc_session_key), cur_off);
    push_le32(auth, neg_flags);
    push_u8(auth, 10); push_u8(auth, 0); push_le16(auth, 19041);
    push_u8(auth, 0); push_u8(auth, 0); push_u8(auth, 0); push_u8(auth, 15);

    auth.insert(auth.end(), lm_resp, lm_resp + sizeof(lm_resp));
    auth.insert(auth.end(), nt_resp.begin(), nt_resp.end());
    auth.insert(auth.end(), dom_u16.begin(), dom_u16.end());
    auth.insert(auth.end(), user_u16.begin(), user_u16.end());
    auth.insert(auth.end(), ws_u16.begin(), ws_u16.end());
    auth.insert(auth.end(), enc_session_key, enc_session_key + sizeof(enc_session_key));

    // Seal server public key -> pubKeyAuth (SeqNum = 0)
    auto sealed_pk = ntlm_seal_message(cli_sign_key, cli_seal_rc4, 0, peer_pubkey.data(), peer_pubkey.size());
    auto ts3 = build_ts_request(3, &auth, nullptr, &sealed_pk);
    if (!transport_send(ts3.data(), ts3.size())) return false;

    // 4. Receive Server TSRequest (pubKeyAuth confirmation)
    if (!transport_recv(der_hdr, 2) || der_hdr[0] != 0x30) {
        set_error("Windows NLA rejected credentials (check username & password)");
        return false;
    }
    body_len = 0;
    hdr_len = 2;
    if ((der_hdr[1] & 0x80) == 0) {
        body_len = der_hdr[1];
    } else {
        int nb = der_hdr[1] & 0x7F;
        if (nb == 0 || nb > 2 || !transport_recv(der_hdr + 2, nb)) return false;
        hdr_len = 2 + nb;
        for (int i = 0; i < nb; ++i) body_len = (body_len << 8) | der_hdr[2 + i];
    }
    std::vector<uint8_t> ts4(hdr_len + body_len);
    std::memcpy(ts4.data(), der_hdr, hdr_len);
    if (!transport_recv(ts4.data() + hdr_len, body_len)) {
        set_error("Windows NLA authentication failed");
        return false;
    }

    // 5. Send encrypted TSCredentials (TSPasswordCreds: domain, username, password, SeqNum = 1)
    std::vector<uint8_t> pw_creds;
    auto d0 = crypto::der_wrap(0xA0, crypto::der_wrap(0x04, dom_u16));
    auto d1 = crypto::der_wrap(0xA1, crypto::der_wrap(0x04, user_u16));
    auto d2 = crypto::der_wrap(0xA2, crypto::der_wrap(0x04, pass_u16));
    pw_creds.insert(pw_creds.end(), d0.begin(), d0.end());
    pw_creds.insert(pw_creds.end(), d1.begin(), d1.end());
    pw_creds.insert(pw_creds.end(), d2.begin(), d2.end());
    auto pw_seq = crypto::der_wrap(0x30, pw_creds);

    std::vector<uint8_t> ts_creds;
    auto ctype = crypto::der_wrap(0xA0, crypto::der_integer(1));  // TSPasswordCreds
    auto cbuf = crypto::der_wrap(0xA1, crypto::der_wrap(0x04, pw_seq));
    ts_creds.insert(ts_creds.end(), ctype.begin(), ctype.end());
    ts_creds.insert(ts_creds.end(), cbuf.begin(), cbuf.end());
    auto ts_creds_der = crypto::der_wrap(0x30, ts_creds);

    auto sealed_creds = ntlm_seal_message(cli_sign_key, cli_seal_rc4, 1, ts_creds_der.data(), ts_creds_der.size());
    auto ts5 = build_ts_request(3, nullptr, &sealed_creds, nullptr);
    return transport_send(ts5.data(), ts5.size());
}

bool RdpBackend::do_mcs_connect(uint32_t selected_proto) {
    set_status("Establishing RDP MCS session...");

    // Build GCC Client Core Data (CS_CORE 0xC001)
    std::vector<uint8_t> cs_core;
    push_le16(cs_core, 0xC001);
    push_le16(cs_core, 216);               // block length
    push_le32(cs_core, 0x00080004u);       // RDP 5.0+ version
    push_le16(cs_core, profile_.req_width);
    push_le16(cs_core, profile_.req_height);
    push_le16(cs_core, 0xCA01);            // RNS_UD_COLOR_8BPP (upgraded via highColorDepth)
    push_le16(cs_core, 0xAA03);            // SASSequence
    push_le32(cs_core, 0x00000409u);       // US English keyboard layout
    push_le32(cs_core, 19041);             // clientBuild
    // clientName (32 bytes UTF-16LE "OunceDesk")
    auto cname = crypto::utf8_to_utf16le("OunceDesk", false);
    cname.resize(32, 0);
    cs_core.insert(cs_core.end(), cname.begin(), cname.end());
    push_le32(cs_core, 4);                 // keyboardType (IBM enhanced 101/102)
    push_le32(cs_core, 0);                 // keyboardSubType
    push_le32(cs_core, 12);                // keyboardFunctionKey
    for (int i = 0; i < 64; ++i) push_u8(cs_core, 0);  // imeFileName
    push_le16(cs_core, 0xCA01);            // postBeta2ColorDepth
    push_le16(cs_core, 1);                 // clientProductId
    push_le32(cs_core, 0);                 // serialNumber
    push_le16(cs_core, 24);                // highColorDepth (24/32bpp)
    push_le16(cs_core, 0x000Fu);           // supportedColorDepths (15|16|24|32)
    push_le16(cs_core, 0x0003u);           // earlyCapabilityFlags (SUPPORT_ERRINFO | WANT_32BPP_SESSION)
    for (int i = 0; i < 64; ++i) push_u8(cs_core, 0);  // clientDigProductId
    push_u8(cs_core, 0x06);                // connectionType (LAN)
    push_u8(cs_core, 0);                   // pad1octet
    push_le32(cs_core, selected_proto);

    // CS_SECURITY (0xC002)
    std::vector<uint8_t> cs_sec;
    push_le16(cs_sec, 0xC002);
    push_le16(cs_sec, 12);
    push_le32(cs_sec, 0);  // TLS handles encryption
    push_le32(cs_sec, 0);

    // CS_NET (0xC003) — 0 channels
    std::vector<uint8_t> cs_net;
    push_le16(cs_net, 0xC003);
    push_le16(cs_net, 8);
    push_le32(cs_net, 0);

    std::vector<uint8_t> gcc_ud;
    gcc_ud.insert(gcc_ud.end(), cs_core.begin(), cs_core.end());
    gcc_ud.insert(gcc_ud.end(), cs_sec.begin(), cs_sec.end());
    gcc_ud.insert(gcc_ud.end(), cs_net.begin(), cs_net.end());

    // GCC Conference Create Request (PER-encoded)
    static constexpr uint8_t gcc_hdr[] = {
        0x00, 0x05, 0x00, 0x14, 0x7C, 0x00, 0x01,
        0x81, 0x00,  // length placeholder (filled below)
        0x00, 0x08, 0x00, 0x10, 0x00, 0x01, 0xC0, 0x00,
        0x44, 0x75, 0x63, 0x61,  // "Duca" client-to-server H.221 key
    };
    std::vector<uint8_t> gcc(gcc_hdr, gcc_hdr + sizeof(gcc_hdr));
    uint16_t ud_len = static_cast<uint16_t>(gcc_ud.size());
    push_be16(gcc, static_cast<uint16_t>(0x8000u | ud_len));
    gcc.insert(gcc.end(), gcc_ud.begin(), gcc_ud.end());
    uint16_t inner_len = static_cast<uint16_t>(gcc.size() - 9);
    gcc[7] = static_cast<uint8_t>(0x80u | ((inner_len >> 8) & 0x7Fu));
    gcc[8] = static_cast<uint8_t>(inner_len & 0xFFu);

    auto make_domain_params = [](uint32_t max_ch, uint32_t max_usr, uint32_t max_tok, uint32_t max_pdu) {
        std::vector<uint8_t> p;
        auto add_i = [&](uint32_t v) { auto d = crypto::der_integer(v); p.insert(p.end(), d.begin(), d.end()); };
        add_i(max_ch); add_i(max_usr); add_i(max_tok); add_i(1); add_i(0); add_i(1); add_i(max_pdu); add_i(2);
        return crypto::der_wrap(0x30, p);
    };

    std::vector<uint8_t> mcs_body;
    auto calling = crypto::der_wrap(0x04, {0x01});
    auto called  = crypto::der_wrap(0x04, {0x01});
    auto upward  = crypto::der_wrap(0x01, {0xFF});
    auto dp_tgt  = make_domain_params(34, 2, 0, 65535);
    auto dp_min  = make_domain_params(1, 1, 1, 1056);
    auto dp_max  = make_domain_params(65535, 64535, 65535, 65535);
    auto ud_oct  = crypto::der_wrap(0x04, gcc);

    for (const auto& part : {calling, called, upward, dp_tgt, dp_min, dp_max, ud_oct}) {
        mcs_body.insert(mcs_body.end(), part.begin(), part.end());
    }
    auto mcs_ci = crypto::der_wrap(0x65, mcs_body);
    std::vector<uint8_t> ci_pkt = {0x02, 0xF0, 0x80, 0x7F};
    ci_pkt.insert(ci_pkt.end(), mcs_ci.begin(), mcs_ci.end());
    if (!send_tpkt(ci_pkt.data(), ci_pkt.size())) return false;

    // Receive MCS Connect-Response
    std::vector<uint8_t> cr;
    if (!recv_tpkt(cr) || cr.size() < 12) return false;

    // MCS Erect Domain Request
    const uint8_t erect[] = {0x02, 0xF0, 0x80, 0x04, 0x01, 0x00, 0x01, 0x00};
    if (!send_tpkt(erect, sizeof(erect))) return false;

    // MCS Attach User Request -> Confirm (extracts mcs_user_id_)
    const uint8_t attach[] = {0x02, 0xF0, 0x80, 0x28};
    if (!send_tpkt(attach, sizeof(attach))) return false;
    std::vector<uint8_t> ac;
    if (!recv_tpkt(ac) || ac.size() < 7) return false;
    mcs_user_id_ = static_cast<uint16_t>(((ac[5] << 8) | ac[6]) + 1001);

    // MCS Channel Join Request for mcs_user_id_ and mcs_io_channel_ (1003)
    auto join_ch = [&](uint16_t ch) -> bool {
        std::vector<uint8_t> cj = {0x02, 0xF0, 0x80, 0x38};
        push_be16(cj, static_cast<uint16_t>(mcs_user_id_ - 1001));
        push_be16(cj, ch);
        if (!send_tpkt(cj.data(), cj.size())) return false;
        std::vector<uint8_t> cjc;
        return recv_tpkt(cjc);
    };
    if (!join_ch(mcs_user_id_) || !join_ch(mcs_io_channel_)) return false;
    return true;
}

bool RdpBackend::do_client_info_pdu() {
    auto dom_u16  = crypto::utf8_to_utf16le(profile_.domain, false);
    auto usr_u16  = crypto::utf8_to_utf16le(profile_.username, false);
    auto pwd_u16  = crypto::utf8_to_utf16le(profile_.password, false);

    uint32_t flags =
        0x00000001u |  // INFO_MOUSE
        0x00000002u |  // INFO_DISABLECTRLALTDEL
        0x00000010u |  // INFO_UNICODE
        0x00000020u |  // INFO_MAXIMIZESHELL
        0x00000040u;   // INFO_LOGONNOTIFY
    if (!profile_.password.empty()) flags |= 0x00000008u;  // INFO_AUTOLOGON

    std::vector<uint8_t> pdu;
    // Basic Security Header (SEC_INFO_PKT = 0x0040)
    push_le16(pdu, 0x0040);
    push_le16(pdu, 0x0000);
    // TS_INFO_PACKET
    push_le32(pdu, 0);      // CodePage
    push_le32(pdu, flags);
    push_le16(pdu, static_cast<uint16_t>(dom_u16.size()));
    push_le16(pdu, static_cast<uint16_t>(usr_u16.size()));
    push_le16(pdu, static_cast<uint16_t>(pwd_u16.size()));
    push_le16(pdu, 0);      // cbAlternateShell
    push_le16(pdu, 0);      // cbWorkingDir
    pdu.insert(pdu.end(), dom_u16.begin(), dom_u16.end()); push_le16(pdu, 0);
    pdu.insert(pdu.end(), usr_u16.begin(), usr_u16.end()); push_le16(pdu, 0);
    pdu.insert(pdu.end(), pwd_u16.begin(), pwd_u16.end()); push_le16(pdu, 0);
    push_le16(pdu, 0);      // AlternateShell NUL
    push_le16(pdu, 0);      // WorkingDir NUL

    return send_mcs_send_data(mcs_io_channel_, pdu.data(), pdu.size());
}

bool RdpBackend::handle_demand_active(const uint8_t* pdu, size_t len) {
    if (len < 14) return false;
    uint16_t server_originator_id = le16(pdu + 4);
    if (server_originator_id == 0) server_originator_id = 0x03EA;
    share_id_ = le32(pdu + 6);

    // Parse server Demand Active capability sets to echo preferredBitsPerPixel
    uint16_t server_bpp = 32;
    uint16_t srv_w = profile_.req_width;
    uint16_t srv_h = profile_.req_height;
    uint16_t cb_src_desc = le16(pdu + 10);
    uint16_t cb_combined_caps = le16(pdu + 12);
    size_t caps_off = 14 + static_cast<size_t>(cb_src_desc);
    if (caps_off + 4 <= len && cb_combined_caps >= 4) {
        uint16_t srv_num_caps = le16(pdu + caps_off);
        size_t cpos = caps_off + 4;
        for (uint16_t i = 0; i < srv_num_caps && cpos + 4 <= len; ++i) {
            uint16_t ctype = le16(pdu + cpos);
            uint16_t clen  = le16(pdu + cpos + 2);
            if (clen < 4 || cpos + clen > len) break;
            if (ctype == 0x0002 && clen >= 16) {
                uint16_t bpp = le16(pdu + cpos + 4);
                if (bpp == 15 || bpp == 16 || bpp == 24 || bpp == 32) {
                    server_bpp = bpp;
                }
                uint16_t sw = le16(pdu + cpos + 12);
                uint16_t sh = le16(pdu + cpos + 14);
                if (sw >= 200 && sh >= 200 && sw <= 8192 && sh <= 8192) {
                    srv_w = sw;
                    srv_h = sh;
                }
            }
            cpos += clen;
        }
    }

    platform::log_line("[RDP] Received DemandActive: shareId=" + std::to_string(share_id_) +
                       " originator=" + std::to_string(server_originator_id) +
                       " serverBpp=" + std::to_string(server_bpp));

    // Build Client Confirm Active PDU with all 16 mandatory MS-RDPBCGR capability sets
    // (Matches FreeRDP rdp_write_confirm_active so Windows rdpcorets StateExpectingCA succeeds!)
    std::vector<uint8_t> caps;
    uint16_t num_caps = 0;

    auto add_cap = [&](uint16_t type, const std::vector<uint8_t>& body) {
        push_le16(caps, type);
        push_le16(caps, static_cast<uint16_t>(4 + body.size()));
        caps.insert(caps.end(), body.begin(), body.end());
        ++num_caps;
    };

    // 1. CAPSTYPE_GENERAL (0x0001) - 24 bytes total (20-byte body)
    {
        std::vector<uint8_t> c;
        push_le16(c, 0x0001);  // OSMAJORTYPE_WINDOWS
        push_le16(c, 0x0003);  // OSMINORTYPE_WINDOWS_NT
        push_le16(c, 0x0200);  // TS_CAPS_PROTOCOLVERSION
        push_le16(c, 0);       // pad2octetsA
        push_le16(c, 0);       // generalCompressionTypes = 0
        // extraFlags: FASTPATH_OUTPUT_SUPPORTED (0x0001) | NO_BITMAP_COMPRESSION_HDR (0x0400) |
        //             LONG_CREDENTIALS_SUPPORTED (0x0004) | AUTORECONNECT_SUPPORTED (0x0008)
        // NOTE: Do NOT set 0x0080 (ENC_SALTED_CHECKSUM) when TLS/NLA security is used!
        push_le16(c, 0x040Du);
        push_le16(c, 0);       // updateCapabilityFlag
        push_le16(c, 0);       // remoteUnshareFlag
        push_le16(c, 0);       // generalCompressionLevel
        push_u8(c, 1);         // refreshRectSupport = TRUE
        push_u8(c, 1);         // suppressOutputSupport = TRUE
        add_cap(0x0001, c);
    }
    // 2. CAPSTYPE_BITMAP (0x0002) - 28 bytes total (24-byte body)
    {
        std::vector<uint8_t> c;
        push_le16(c, server_bpp);           // preferredBitsPerPixel (echo server's bpp)
        push_le16(c, 1);                    // receive1BitPerPixel
        push_le16(c, 1);                    // receive4BitsPerPixel
        push_le16(c, 1);                    // receive8BitsPerPixel
        push_le16(c, srv_w);               // desktopWidth (echo server)
        push_le16(c, srv_h);               // desktopHeight (echo server)
        push_le16(c, 0);                    // pad2octets
        push_le16(c, 1);                    // desktopResizeFlag = TRUE
        push_le16(c, 1);                    // bitmapCompressionFlag = TRUE (RLE)
        push_u8(c, 0);                      // highColorFlags = 0
        push_u8(c, 0x08);                   // drawingFlags = DRAW_ALLOW_SKIP_ALPHA
        push_le16(c, 1);                    // multipleRectangleSupport = TRUE
        push_le16(c, 0);                    // pad2octetsB
        add_cap(0x0002, c);
    }
    // 3. CAPSTYPE_ORDER (0x0003) - 88 bytes total (84-byte body)
    {
        std::vector<uint8_t> c(84, 0);
        // Body offsets (relative to byte 4 after capability header):
        // +0..15: terminalDescriptor (16 bytes 0)
        // +16..19: pad4octetsA (0)
        // +20..21: desktopSaveXGranularity = 1
        c[20] = 1; c[21] = 0;
        // +22..23: desktopSaveYGranularity = 20
        c[22] = 20; c[23] = 0;
        // +24..25: pad2octetsA = 0
        // +26..27: maximumOrderLevel = ORD_LEVEL_1_ORDERS (1)
        c[26] = 1; c[27] = 0;
        // +28..29: numberFonts = 0
        // +30..31: orderFlags = NEGOTIATEORDERSUPPORT (0x0002) | ZEROBOUNDSDELTASSUPPORT (0x0008) | COLORINDEXSUPPORT (0x0020)
        c[30] = 0x2A; c[31] = 0x00;
        // +32..63: orderSupport[32] = all 0 (forces server to send all drawing as Bitmaps!)
        // +64..65: textFlags = 0
        // +66..67: orderSupportExFlags = 0
        // +68..71: pad4octetsB = 0
        // +72..75: desktopSaveSize = 230400 (0x00038400)
        c[72] = 0x00; c[73] = 0x84; c[74] = 0x03; c[75] = 0x00;
        // +76..83: pad2octetsC, pad2octetsD, textANSICodePage, pad2octetsE = 0
        add_cap(0x0003, c);
    }
    // 4. CAPSTYPE_BITMAPCACHE_REV2 (0x0013) - 40 bytes total (36-byte body)
    {
        std::vector<uint8_t> c(36, 0);
        add_cap(0x0013, c);
    }
    // 5. CAPSTYPE_COLORCACHE (0x000A) - 8 bytes total (4-byte body)
    {
        std::vector<uint8_t> c;
        push_le16(c, 6);  // colorTableCacheSize = 6
        push_le16(c, 0);  // pad2octets
        add_cap(0x000A, c);
    }
    // 6. CAPSTYPE_ACTIVATION (0x0007) - 12 bytes total (8-byte body)
    {
        std::vector<uint8_t> c(8, 0);
        add_cap(0x0007, c);
    }
    // 7. CAPSTYPE_CONTROL (0x0005) - 12 bytes total (8-byte body)
    {
        std::vector<uint8_t> c;
        push_le16(c, 0);  // controlFlags
        push_le16(c, 0);  // remoteDetachFlag
        push_le16(c, 2);  // controlInterest = CONTROLPRIORITY_NEVER
        push_le16(c, 2);  // detachInterest = CONTROLPRIORITY_NEVER
        add_cap(0x0005, c);
    }
    // 8. CAPSTYPE_POINTER (0x0008) - 10 bytes total (6-byte body)
    {
        std::vector<uint8_t> c;
        push_le16(c, 1);   // colorPointerFlag = TRUE
        push_le16(c, 20);  // colorPointerCacheSize = 20
        push_le16(c, 20);  // pointerCacheSize = 20
        add_cap(0x0008, c);
    }
    // 9. CAPSTYPE_SHARE (0x0009) - 8 bytes total (4-byte body)
    {
        std::vector<uint8_t> c;
        push_le16(c, 0);  // nodeId = 0
        push_le16(c, 0);  // pad2octets
        add_cap(0x0009, c);
    }
    // 10. CAPSTYPE_BRUSH (0x000F) - 8 bytes total (4-byte body)
    {
        std::vector<uint8_t> c;
        push_le32(c, 0);  // brushSupportLevel = BRUSH_DEFAULT
        add_cap(0x000F, c);
    }
    // 11. CAPSTYPE_GLYPHCACHE (0x0010) - 52 bytes total (48-byte body)
    {
        std::vector<uint8_t> c(48, 0);
        add_cap(0x0010, c);
    }
    // 12. CAPSTYPE_OFFSCREENCACHE (0x0011) - 12 bytes total (8-byte body)
    {
        std::vector<uint8_t> c(8, 0);
        add_cap(0x0011, c);
    }
    // 13. CAPSTYPE_SOUND (0x000C) - 8 bytes total (4-byte body)
    {
        std::vector<uint8_t> c;
        push_le16(c, 0);  // soundFlags = 0
        push_le16(c, 0);  // pad2octetsA
        add_cap(0x000C, c);
    }
    // 14. CAPSTYPE_INPUT (0x000D) - 88 bytes total (84-byte body)
    {
        std::vector<uint8_t> c(84, 0);
        // inputFlags: INPUT_FLAG_SCANCODES (0x0001) | INPUT_FLAG_MOUSEX (0x0004) |
        //             INPUT_FLAG_FASTPATH_INPUT (0x0008) | INPUT_FLAG_UNICODE (0x0010) |
        //             INPUT_FLAG_FASTPATH_INPUT2 (0x0020)
        uint16_t iflags = 0x003Du;
        c[0] = uint8_t(iflags & 0xFF);
        c[1] = uint8_t(iflags >> 8);
        c[4] = 0x09; c[5] = 0x04;  // keyboardLayout = 0x0409
        c[8] = 4;                  // keyboardType = 4 (IBM enhanced 101/102)
        c[16] = 12;                // keyboardFunctionKey = 12
        add_cap(0x000D, c);
    }
    // 15. CAPSTYPE_FONT (0x000E) - 8 bytes total (4-byte body)
    {
        std::vector<uint8_t> c;
        push_le16(c, 0x0001);  // fontSupportFlags = FONTSUPPORT_FONTLIST
        push_le16(c, 0);
        add_cap(0x000E, c);
    }
    // 16. CAPSTYPE_VIRTUALCHANNEL (0x0014) - 12 bytes total (8-byte body)
    {
        std::vector<uint8_t> c;
        push_le32(c, 0);     // flags = VCCAPS_NO_COMPR
        push_le32(c, 1600);  // VCChunkSize = 1600
        add_cap(0x0014, c);
    }

    const char* src_desc = "MSTSC";
    uint16_t src_len = static_cast<uint16_t>(std::strlen(src_desc) + 1);

    std::vector<uint8_t> confirm;
    push_le32(confirm, share_id_);
    push_le16(confirm, server_originator_id);
    push_le16(confirm, src_len);
    push_le16(confirm, static_cast<uint16_t>(4 + caps.size()));
    confirm.insert(confirm.end(), src_desc, src_desc + src_len);
    push_le16(confirm, num_caps);
    push_le16(confirm, 0);  // pad2Octets
    confirm.insert(confirm.end(), caps.begin(), caps.end());

    if (!send_share_control(0x03, confirm.data(), confirm.size())) return false;  // PDUTYPE_CONFIRMACTIVEPDU
    platform::log_line("[RDP] Sent ConfirmActive (16 capability sets)");

    // Synchronize PDU (0x1F)
    std::vector<uint8_t> sync;
    push_le16(sync, 1);  // messageType = SYNCMSGTYPE_SYNC
    push_le16(sync, mcs_user_id_);
    if (!send_share_data(0x1F, sync.data(), sync.size())) return false;

    // Control Cooperate (0x14, action=4) + Control Request Control (action=1)
    auto send_ctrl = [&](uint16_t action) {
        std::vector<uint8_t> b;
        push_le16(b, action); push_le16(b, 0); push_le32(b, 0);
        return send_share_data(0x14, b.data(), b.size());
    };
    if (!send_ctrl(4) || !send_ctrl(1)) return false;

    // Font List PDU (0x27) -> triggers Windows to begin streaming desktop!
    std::vector<uint8_t> flist;
    push_le16(flist, 0); push_le16(flist, 0); push_le16(flist, 0x0003); push_le16(flist, 0x0032);
    if (!send_share_data(0x27, flist.data(), flist.size())) return false;

    {
        std::lock_guard<std::mutex> lk(fb_mu_);
        fb_w_ = srv_w;
        fb_h_ = srv_h;
        if (static_cast<int>(fb_rgba_.size()) != fb_w_ * fb_h_) {
            fb_rgba_.assign(static_cast<size_t>(fb_w_) * fb_h_, 0xFF0A0E18u);
        }
        full_dirty_ = true;
    }
    {
        std::lock_guard<std::mutex> ik(info_mu_);
        stats_.fb_width = srv_w;
        stats_.fb_height = srv_h;
    }
    state_.store(BackendState::STREAMING);
    set_status("已连接 Windows 远程桌面 " + profile_.host);
    return true;
}

void RdpBackend::mark_dirty(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (dirty_rects_.size() > 64) {
        full_dirty_ = true;
        dirty_rects_.clear();
    } else if (!full_dirty_) {
        dirty_rects_.push_back({x, y, w, h});
    }
}

void RdpBackend::blit_bitmap_rect(int left, int top, int width, int height, int bpp, bool compressed,
                                  const uint8_t* data, size_t len, int draw_w, int draw_h) {
    if (width <= 0 || height <= 0 || !data || len == 0) return;
    if (draw_w <= 0 || draw_w > width) draw_w = width;
    if (draw_h <= 0 || draw_h > height) draw_h = height;
    std::vector<uint32_t> pixels;

    // Decode the FULL stored bitmap (width x height incl. padding columns), in stored bottom-up order
    if (compressed) {
        bool ok = (bpp == 32) ? decompress_planar(data, len, width, height, pixels)
                              : decompress_interleaved(data, len, width, height, bpp, pixels);
        if (!ok) {
            if (!bad_codec_logged_) {
                bad_codec_logged_ = true;
                platform::log_line("[RDP] Unsupported compressed bitmap bpp=" + std::to_string(bpp) +
                                   " hdr=" + std::to_string(len ? data[0] : 0));
            }
            return;
        }
    } else {
        int bytes_pp = (bpp + 7) / 8;
        if (bytes_pp < 2 || bytes_pp > 4) return;
        // Uncompressed scanlines are padded to a 4-byte boundary
        size_t stride = (static_cast<size_t>(width) * bytes_pp + 3) & ~static_cast<size_t>(3);
        pixels.assign(static_cast<size_t>(width) * height, 0xFF000000u);
        for (int y = 0; y < height; ++y) {
            size_t rp = static_cast<size_t>(y) * stride;
            for (int x = 0; x < width; ++x) {
                size_t pp = rp + static_cast<size_t>(x) * bytes_pp;
                if (pp + bytes_pp > len) break;
                uint32_t v = (bytes_pp == 2) ? le16(data + pp)
                                             : (uint32_t(data[pp]) | (uint32_t(data[pp + 1]) << 8) | (uint32_t(data[pp + 2]) << 16));
                pixels[static_cast<size_t>(y) * width + x] = native_to_rgba(v, bpp);
            }
        }
    }

    // RDP bitmaps are stored bottom-up -> flip vertically, and only copy the visible draw_w x draw_h area
    std::lock_guard<std::mutex> lk(fb_mu_);
    for (int y = 0; y < height; ++y) {
        int row_from_top = height - 1 - y;
        if (row_from_top >= draw_h) continue;
        int dst_y = top + row_from_top;
        if (dst_y < 0 || dst_y >= fb_h_) continue;
        int copy_w = std::min(draw_w, fb_w_ - left);
        if (copy_w > 0 && left >= 0) {
            std::memcpy(fb_rgba_.data() + static_cast<size_t>(dst_y) * fb_w_ + left,
                        pixels.data() + static_cast<size_t>(y) * width,
                        static_cast<size_t>(copy_w) * 4);
        }
    }
    mark_dirty(left, top, draw_w, draw_h);
}

void RdpBackend::handle_pointer_attr(const uint8_t* data, size_t len, int xor_bpp, bool large) {
    size_t hdr = large ? 18 : 14;
    if (len < hdr) return;
    uint16_t cache_idx = le16(data + 0);
    int hx = le16(data + 2);
    int hy = le16(data + 4);
    int w  = le16(data + 6);
    int h  = le16(data + 8);
    uint32_t and_len = large ? le32(data + 10) : le16(data + 10);
    uint32_t xor_len = large ? le32(data + 14) : le16(data + 12);
    if (w <= 0 || h <= 0 || w > 384 || h > 384) return;
    if (hdr + xor_len + and_len > len) return;
    const uint8_t* xor_d = data + hdr;
    const uint8_t* and_d = xor_d + xor_len;

    size_t xor_stride = ((static_cast<size_t>(w) * xor_bpp + 7) / 8 + 1) & ~static_cast<size_t>(1);
    size_t and_stride = ((static_cast<size_t>(w) + 7) / 8 + 1) & ~static_cast<size_t>(1);
    bool has_and = and_len >= and_stride * h;
    bool has_xor = xor_len >= xor_stride * h;

    // For 32bpp cursors with real alpha, alpha defines visibility
    bool use_alpha = false;
    if (xor_bpp == 32 && has_xor) {
        for (int y = 0; y < h && !use_alpha; ++y)
            for (int x = 0; x < w; ++x)
                if (xor_d[y * xor_stride + x * 4 + 3] != 0) { use_alpha = true; break; }
    }

    std::vector<uint8_t> vis(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y) {
        int sy = h - 1 - y;  // masks are stored bottom-up
        for (int x = 0; x < w; ++x) {
            bool and_bit = has_and ? ((and_d[sy * and_stride + x / 8] >> (7 - (x % 8))) & 1) != 0 : false;
            bool xor_nz = false;
            uint8_t alpha = 0;
            if (has_xor) {
                const uint8_t* xr = xor_d + sy * xor_stride;
                if (xor_bpp == 1) xor_nz = ((xr[x / 8] >> (7 - (x % 8))) & 1) != 0;
                else {
                    int bpp_b = (xor_bpp + 7) / 8;
                    for (int k = 0; k < std::min(bpp_b, 3); ++k) if (xr[x * bpp_b + k]) xor_nz = true;
                    if (xor_bpp == 32) alpha = xr[x * 4 + 3];
                }
            }
            bool v = use_alpha ? (alpha != 0 || (and_bit && xor_nz)) : (!and_bit || xor_nz);
            vis[static_cast<size_t>(y) * w + x] = v ? 1 : 0;
        }
    }
    bool text = cursor_detect::is_ibeam(vis, w, h, hx, hy);
    if (cache_idx >= ptr_cache_text_.size()) ptr_cache_text_.resize(cache_idx + 1, 0);
    ptr_cache_text_[cache_idx] = text ? 1 : 0;
    cursor_text_.store(text);
}

void RdpBackend::handle_fastpath_update(uint8_t ucode, const uint8_t* data, size_t len, int& frames_win) {
    switch (ucode) {
        case 0x01:  // FASTPATH_UPDATETYPE_BITMAP
            handle_bitmap_update(data, len);
            ++frames_win;
            break;
        case 0x04:  // FASTPATH_UPDATETYPE_SURFCMDS
            handle_surface_commands(data, len);
            ++frames_win;
            break;
        case 0x05:  // PTR_NULL (hidden)
        case 0x06:  // PTR_DEFAULT (arrow)
            cursor_text_.store(false);
            break;
        case 0x09:  // FASTPATH_UPDATETYPE_COLOR (24bpp TS_COLORPOINTERATTRIBUTE)
            handle_pointer_attr(data, len, 24, false);
            break;
        case 0x0A:  // FASTPATH_UPDATETYPE_CACHED
            if (len >= 2) {
                uint16_t idx = le16(data);
                cursor_text_.store(idx < ptr_cache_text_.size() && ptr_cache_text_[idx] != 0);
            }
            break;
        case 0x0B:  // FASTPATH_UPDATETYPE_POINTER (xorBpp + TS_COLORPOINTERATTRIBUTE)
            if (len >= 2) handle_pointer_attr(data + 2, len - 2, le16(data), false);
            break;
        case 0x0C:  // FASTPATH_UPDATETYPE_LARGE_POINTER
            if (len >= 2) handle_pointer_attr(data + 2, len - 2, le16(data), true);
            break;
        default:
            break;
    }
}

void RdpBackend::handle_bitmap_update(const uint8_t* data, size_t len) {
    if (len < 4) return;
    uint16_t nrects = le16(data + 2);
    size_t pos = 4;
    for (uint16_t i = 0; i < nrects && pos + 18 <= len; ++i) {
        int left   = le16(data + pos + 0);
        int top    = le16(data + pos + 2);
        int right  = le16(data + pos + 4);
        int bottom = le16(data + pos + 6);
        int w      = le16(data + pos + 8);
        int h      = le16(data + pos + 10);
        int bpp    = le16(data + pos + 12);
        uint16_t flags = le16(data + pos + 14);
        uint16_t blen  = le16(data + pos + 16);
        pos += 18;
        if (pos + blen > len) break;

        bool compressed = (flags & 0x0001) != 0;
        const uint8_t* bdata = data + pos;
        size_t bsize = blen;
        // If NO_BITMAP_COMPRESSION_HDR (0x0400) is NOT set, skip 8-byte TS_CD_HEADER
        if (compressed && (flags & 0x0400) == 0 && bsize >= 8) {
            bdata += 8;
            bsize -= 8;
        }
        int draw_w = std::min(w, right - left + 1);
        int draw_h = std::min(h, bottom - top + 1);
        // Decode using the full stored width/height (w x h); only draw the destination rectangle
        blit_bitmap_rect(left, top, w, h, bpp, compressed, bdata, bsize, draw_w, draw_h);
        pos += blen;
    }
}

void RdpBackend::handle_surface_commands(const uint8_t* data, size_t len) {
    size_t pos = 0;
    while (pos + 2 <= len) {
        uint16_t cmd = le16(data + pos);
        pos += 2;
        if (cmd == 0x0001 || cmd == 0x0004) {  // SET_SURFACE_BITS / STREAM_SURFACE_BITS
            if (pos + 20 > len) break;
            int left   = le16(data + pos + 0);
            int top    = le16(data + pos + 2);
            int bpp    = data[pos + 8];
            uint16_t codec_id = data[pos + 11];
            int w      = le16(data + pos + 12);
            int h      = le16(data + pos + 14);
            uint32_t blen = le32(data + pos + 16);
            pos += 20;
            if (pos + blen > len) break;
            if (codec_id == 0) {
                blit_bitmap_rect(left, top, w, h, bpp, false, data + pos, blen, w, h);
            }
            pos += blen;
        } else if (cmd == 0x0006) {  // FRAME_MARKER
            pos += 6;
        } else {
            break;
        }
    }
}

bool RdpBackend::sync_texture(SDL_Renderer* renderer, SDL_Texture*& io_texture, int& out_w, int& out_h) {
    std::lock_guard<std::mutex> lk(fb_mu_);
    if (fb_w_ <= 0 || fb_h_ <= 0 || fb_rgba_.empty()) return false;

    int tw = 0, th = 0;
    if (io_texture) SDL_QueryTexture(io_texture, nullptr, nullptr, &tw, &th);
    if (!io_texture || tw != fb_w_ || th != fb_h_) {
        if (io_texture) SDL_DestroyTexture(io_texture);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        io_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, fb_w_, fb_h_);
        full_dirty_ = true;
    }
    out_w = fb_w_;
    out_h = fb_h_;
    if (!io_texture) return false;

    if (full_dirty_) {
        SDL_UpdateTexture(io_texture, nullptr, fb_rgba_.data(), fb_w_ * 4);
        full_dirty_ = false;
        dirty_rects_.clear();
        return true;
    }
    if (dirty_rects_.empty()) return false;
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

void RdpBackend::send_mouse_abs(float norm_x, float norm_y) {
    int w = 1920, h = 1080;
    {
        std::lock_guard<std::mutex> lk(fb_mu_);
        w = std::max(1, fb_w_);
        h = std::max(1, fb_h_);
    }
    last_x_ = static_cast<uint16_t>(std::max(0.0f, std::min(1.0f, norm_x)) * (w - 1));
    last_y_ = static_cast<uint16_t>(std::max(0.0f, std::min(1.0f, norm_y)) * (h - 1));
    std::vector<uint8_t> ev;
    ev.push_back(static_cast<uint8_t>(1 << 5));  // FASTPATH_INPUT_EVENT_MOUSE
    push_le16(ev, 0x0800);                       // PTRFLAGS_MOVE
    push_le16(ev, last_x_);
    push_le16(ev, last_y_);
    send_fastpath_input(ev.data(), ev.size());
}

void RdpBackend::send_mouse_button(MouseBtn btn, bool pressed) {
    uint16_t pflags = pressed ? 0x8000u : 0x0000u;  // PTRFLAGS_DOWN
    if (btn == MouseBtn::LEFT)        pflags |= 0x1000u;
    else if (btn == MouseBtn::RIGHT)  pflags |= 0x2000u;
    else if (btn == MouseBtn::MIDDLE) pflags |= 0x4000u;
    else return;
    std::vector<uint8_t> ev;
    ev.push_back(static_cast<uint8_t>(1 << 5));  // FASTPATH_INPUT_EVENT_MOUSE
    push_le16(ev, pflags);
    push_le16(ev, last_x_);
    push_le16(ev, last_y_);
    send_fastpath_input(ev.data(), ev.size());
}

void RdpBackend::send_mouse_wheel(int16_t dx, int16_t dy) {
    auto send_w = [&](uint16_t base_flag, int delta) {
        if (delta == 0) return;
        uint16_t flags = base_flag;
        int mag = std::min(255, std::abs(delta));
        if (delta < 0) flags |= 0x0100u | static_cast<uint16_t>(((-mag) & 0xFF));
        else flags |= static_cast<uint16_t>(mag & 0xFF);
        std::vector<uint8_t> ev;
        ev.push_back(static_cast<uint8_t>(1 << 5));
        push_le16(ev, flags);
        push_le16(ev, last_x_);
        push_le16(ev, last_y_);
        send_fastpath_input(ev.data(), ev.size());
    };
    if (dy != 0) send_w(0x0200u, dy);  // PTRFLAGS_WHEEL
    if (dx != 0) send_w(0x0400u, dx);  // PTRFLAGS_HWHEEL
}

void RdpBackend::send_key(uint16_t hid_usage, bool pressed, uint8_t /*modifiers*/) {
    auto sc = keymap::hid_to_ps2(hid_usage);
    if (sc.code == 0) return;
    uint8_t flags = pressed ? 0x00 : 0x01;  // FASTPATH_INPUT_KBDFLAGS_RELEASE
    if (sc.extended) flags |= 0x02;         // FASTPATH_INPUT_KBDFLAGS_EXTENDED
    // eventHeader: eventCode=0 (SCANCODE) in bits 5..7, eventFlags in bits 0..4
    uint8_t ev[2] = {static_cast<uint8_t>(flags & 0x1F), sc.code};
    send_fastpath_input(ev, sizeof(ev));
}

void RdpBackend::send_text_utf8(const std::string& utf8) {
    size_t pos = 0;
    while (pos < utf8.size()) {
        uint32_t cp = keymap::next_utf8_codepoint(utf8.data(), utf8.size(), pos);
        if (cp == 0) break;
        auto send_u16 = [&](uint16_t unit) {
            // FASTPATH_INPUT_EVENT_UNICODE = 4 (bits 5..7)
            uint8_t ev_dn[3] = {static_cast<uint8_t>(4 << 5), uint8_t(unit & 0xFF), uint8_t(unit >> 8)};
            uint8_t ev_up[3] = {static_cast<uint8_t>((4 << 5) | 0x01), uint8_t(unit & 0xFF), uint8_t(unit >> 8)};
            send_fastpath_input(ev_dn, 3);
            send_fastpath_input(ev_up, 3);
        };
        if (cp <= 0xFFFF) {
            send_u16(static_cast<uint16_t>(cp));
        } else {
            cp -= 0x10000;
            send_u16(static_cast<uint16_t>(0xD800 + (cp >> 10)));
            send_u16(static_cast<uint16_t>(0xDC00 + (cp & 0x3FF)));
        }
    }
}

void RdpBackend::request_full_refresh() {
    if (state_.load() != BackendState::STREAMING) return;
    std::vector<uint8_t> req;
    push_u8(req, 1);  // numberOfAreas
    push_u8(req, 0); push_le16(req, 0);
    push_le16(req, 0); push_le16(req, 0);
    push_le16(req, static_cast<uint16_t>(fb_w_ - 1));
    push_le16(req, static_cast<uint16_t>(fb_h_ - 1));
    send_share_data(0x21, req.data(), req.size());  // PDUTYPE2_REFRESH_RECT
}

void RdpBackend::run_session() {
    // 1. Resolve & TCP connect to Windows Port 3389
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string port_s = std::to_string(profile_.port);
    if (getaddrinfo(profile_.host.c_str(), port_s.c_str(), &hints, &res) != 0 || !res) {
        set_error("Cannot resolve Windows host: " + profile_.host);
        return;
    }

    int s = static_cast<int>(socket(res->ai_family, res->ai_socktype, res->ai_protocol));
    if (s < 0) {
        freeaddrinfo(res);
        set_error("Socket creation failed");
        return;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#ifdef _WIN32
    DWORD tv = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{5, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    if (connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
        freeaddrinfo(res);
        CLOSE_SOCK(s);
        set_error("Cannot connect to " + profile_.host + ":" + port_s + " (Enable Windows Remote Desktop)");
        return;
    }
    freeaddrinfo(res);
    sock_ = s;

    // 2. X.224 Security Protocol Negotiation
    state_.store(BackendState::AUTHENTICATING);
    uint32_t selected_proto = 0;
    if (!do_x224_negotiation(selected_proto)) {
        if (state_.load() != BackendState::ERROR_STATE) set_error("RDP X.224 negotiation failed");
        return;
    }

    // 3. TLS Upgrade (required for both PROTOCOL_SSL=1 and PROTOCOL_HYBRID=2)
    if (selected_proto & 0x03u) {
        set_status("Starting TLS handshake...");
        if (!upgrade_to_tls()) {
            set_error("TLS handshake with Windows RDP failed");
            return;
        }
    }

    // 4. CredSSP / NLA (Windows 10/11 default)
    if (selected_proto & 0x02u) {
        if (!do_credssp_nla()) {
            if (state_.load() != BackendState::ERROR_STATE) set_error("CredSSP/NLA login failed");
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lk(info_mu_);
        stats_.protocol_label = (selected_proto & 0x02u) ? "RDP 3389 (NLA)" : "RDP 3389 (TLS)";
        stats_.fb_width = profile_.req_width;
        stats_.fb_height = profile_.req_height;
    }

    // 5. MCS Connect + Client Info
    if (!do_mcs_connect(selected_proto)) { set_error("RDP MCS handshake failed"); return; }
    if (!do_client_info_pdu()) { set_error("RDP Client Info PDU failed"); return; }

    uint64_t stat_t0 = platform::now_us();
    int frames_win = 0;

    // 6. Main RDP Packet Loop (handles Fast-Path Output AND Slow-Path TPKT + Deactivate-All re-login)
    while (!stop_.load()) {
        uint8_t b0 = 0;
        if (!transport_recv(&b0, 1)) {
            if (!stop_.load()) set_error("Windows RDP session closed");
            break;
        }

        if (b0 == 0x03) {
            // Slow-Path TPKT Packet
            uint8_t thdr[3];
            if (!transport_recv(thdr, 3)) break;
            uint16_t total = static_cast<uint16_t>((thdr[1] << 8) | thdr[2]);
            if (total < 4) break;
            std::vector<uint8_t> tpkt(total - 4);
            if (total > 4 && !transport_recv(tpkt.data(), total - 4)) break;

            // Locate MCS SendDataIndication (0x68) payload
            if (tpkt.size() < 8) continue;
            if ((tpkt[3] >> 2) == 8) {
                // MCS DisconnectProviderUltimatum (0x20 / 0x21)
                platform::log_line("[RDP] Received MCS DisconnectProviderUltimatum");
                if (!stop_.load()) set_error("Windows 远程桌面已断开连接");
                break;
            }
            if (tpkt.size() < 12 || tpkt[3] != 0x68) continue;
            uint16_t channel_id = static_cast<uint16_t>((tpkt[6] << 8) | tpkt[7]);
            size_t off = 10;
            if (tpkt[9] & 0x80) off = 11;  // 2-byte PER length
            if (channel_id != mcs_io_channel_ || off + 6 > tpkt.size()) continue;

            const uint8_t* pdu = tpkt.data() + off;
            size_t plen = tpkt.size() - off;
            uint16_t pdu_type = le16(pdu + 2) & 0x0Fu;

            if (pdu_type == 0x01) {
                // PDUTYPE_DEMANDACTIVEPDU (initial connect OR after Windows login screen transition!)
                if (!handle_demand_active(pdu, plen)) {
                    set_error("Confirm Active 握手失败");
                    break;
                }
            } else if (pdu_type == 0x06) {
                // PDUTYPE_DEACTIVATEALLPDU — Windows is transitioning session; wait for next Demand Active!
                set_status("正在切换 Windows 桌面会话...");
            } else if (pdu_type == 0x07 && plen >= 18) {
                // PDUTYPE_DATAPDU
                uint8_t pdu_type2 = pdu[14];
                if (pdu_type2 == 0x02 && plen >= 20) {
                    // PDUTYPE2_UPDATE -> UPDATETYPE_BITMAP (0x0001)
                    uint16_t utype = le16(pdu + 18);
                    if (utype == 0x0001) {
                        handle_bitmap_update(pdu + 18, plen - 18);
                        ++frames_win;
                    }
                } else if (pdu_type2 == 0x2F && plen >= 22) {
                    // PDUTYPE2_SET_ERROR_INFO_PDU
                    uint32_t err_info = le32(pdu + 18);
                    platform::log_line("[RDP] Server SetErrorInfo: " + std::to_string(err_info));
                }
            }
        } else if ((b0 & 0x03) == 0x00) {
            // Fast-Path Output Packet (primary high-speed Windows 10/11 desktop path!)
            uint8_t len1 = 0;
            if (!transport_recv(&len1, 1)) break;
            size_t fp_len = 0;
            size_t hdr_bytes = 2;
            if (len1 & 0x80) {
                uint8_t len2 = 0;
                if (!transport_recv(&len2, 1)) break;
                fp_len = ((len1 & 0x7Fu) << 8) | len2;
                hdr_bytes = 3;
            } else {
                fp_len = len1;
            }
            if (fp_len <= hdr_bytes) continue;
            std::vector<uint8_t> fp(fp_len - hdr_bytes);
            if (!transport_recv(fp.data(), fp.size())) break;

            size_t pos = 0;
            // Skip optional 8-byte FIPS/MAC signature if FASTPATH_OUTPUT_SECURE_CHECKSUM or ENCRYPTED is set
            if ((b0 & 0xC0) != 0 && fp.size() >= 8) {
                pos += 8;
            }
            while (pos < fp.size()) {
                uint8_t uhdr = fp[pos++];
                uint8_t ucode = uhdr & 0x0Fu;
                uint8_t frag  = (uhdr >> 4) & 0x03;
                uint8_t comp  = (uhdr >> 6) & 0x03;
                uint8_t comp_flags = 0;
                if (comp & 0x02) {
                    if (pos >= fp.size()) break;
                    comp_flags = fp[pos++];
                }
                if (pos + 2 > fp.size()) break;
                uint16_t usize = le16(fp.data() + pos);
                pos += 2;
                if (pos + usize > fp.size()) break;

                // Only decode un-compressed updates (we never negotiate bulk compression)
                if ((comp_flags & 0x20) == 0) {
                    const uint8_t* ud = fp.data() + pos;
                    if (frag == 0) {
                        // FASTPATH_FRAGMENT_SINGLE
                        handle_fastpath_update(ucode, ud, usize, frames_win);
                    } else if (frag == 2) {
                        // FASTPATH_FRAGMENT_FIRST
                        fp_frag_buf_.assign(ud, ud + usize);
                        fp_frag_code_ = ucode;
                    } else {
                        // NEXT (3) / LAST (1)
                        if (!fp_frag_buf_.empty() && fp_frag_code_ == ucode) {
                            fp_frag_buf_.insert(fp_frag_buf_.end(), ud, ud + usize);
                            if (frag == 1) {
                                handle_fastpath_update(ucode, fp_frag_buf_.data(), fp_frag_buf_.size(), frames_win);
                                fp_frag_buf_.clear();
                            }
                        }
                    }
                }
                pos += usize;
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
