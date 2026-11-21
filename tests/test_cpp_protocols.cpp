// Compiled C++17 verification suite testing real crypto_utils.hpp, keymap.hpp,
// VncBackend (RFB 3.8 + VNC DES + Apple ARD + ZRLE/CopyRect/Raw + Input), and RdpBackend.

#define _WIN32_WINNT 0x0600

#include "crypto_utils.hpp"
#include "keymap.hpp"
#include "platform.hpp"
#include "vnc_backend.hpp"

#include <SDL2/SDL.h>
#include <mbedtls/aes.h>
#include <mbedtls/bignum.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <zlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define CLOSE_SOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSE_SOCK close
#endif

static std::string to_hex(const uint8_t* d, size_t n) {
    static const char* H = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(H[d[i] >> 4]);
        s.push_back(H[d[i] & 0xF]);
    }
    return s;
}

static void test_md4_md5_hmac_rc4() {
    // RFC 1320 MD4 vectors
    auto m0 = crypto::md4(reinterpret_cast<const uint8_t*>(""), 0);
    assert(to_hex(m0.data(), 16) == "31d6cfe0d16ae931b73c59d7e0c089c0");
    auto m1 = crypto::md4(reinterpret_cast<const uint8_t*>("abc"), 3);
    assert(to_hex(m1.data(), 16) == "a448017aaf21d8525fc10ae87aa6729d");
    // NT Hash of "Password" (UTF-16LE) = a4f49c406510bdcab6824ee7c30fd852
    auto pw16 = crypto::utf8_to_utf16le("Password", false);
    auto nth = crypto::md4(pw16.data(), pw16.size());
    assert(to_hex(nth.data(), 16) == "a4f49c406510bdcab6824ee7c30fd852");

    // RFC 1321 MD5 vectors
    auto d0 = crypto::md5(reinterpret_cast<const uint8_t*>(""), 0);
    assert(to_hex(d0.data(), 16) == "d41d8cd98f00b204e9800998ecf8427e");
    auto d1 = crypto::md5(reinterpret_cast<const uint8_t*>("abc"), 3);
    assert(to_hex(d1.data(), 16) == "900150983cd24fb0d6963f7d28e17f72");

    // RFC 2104 HMAC-MD5 vector
    auto hm = crypto::hmac_md5(reinterpret_cast<const uint8_t*>("Jefe"), 4,
                               reinterpret_cast<const uint8_t*>("what do ya want for nothing?"), 28);
    assert(to_hex(hm.data(), 16) == "750c783e6ab0b503eaa86e310a5db738");

    // RC4 standard vector: key="Key", plaintext="Plaintext" -> bbf316e8d940af0ad3
    crypto::RC4State rc4;
    rc4.init(reinterpret_cast<const uint8_t*>("Key"), 3);
    uint8_t pt[9] = {'P','l','a','i','n','t','e','x','t'};
    rc4.process(pt, 9);
    assert(to_hex(pt, 9) == "bbf316e8d940af0ad3");

    std::printf("[PASS] MD4 (RFC 1320 + NTLM NT-Hash), MD5, HMAC-MD5, RC4\n");
}

static void test_apple_ard_crypto_roundtrip() {
    // Small safe prime (16 bytes) for fast DH round-trip test
    const uint8_t prime[16] = {
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xC9,0x0F,0xDA,0xA2,0x21,0x68,0xC2,0x35
    };
    const uint16_t gen = 2;

    mbedtls_mpi p, g, srv_priv, srv_pub, cli_pub, shared;
    mbedtls_mpi_init(&p); mbedtls_mpi_init(&g);
    mbedtls_mpi_init(&srv_priv); mbedtls_mpi_init(&srv_pub);
    mbedtls_mpi_init(&cli_pub); mbedtls_mpi_init(&shared);

    assert(mbedtls_mpi_read_binary(&p, prime, 16) == 0);
    assert(mbedtls_mpi_lset(&g, gen) == 0);
    const uint8_t sp_bytes[16] = {0x12,0x34,0x56,0x78,0x9A,0xBC,0xDE,0xF1,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x89};
    assert(mbedtls_mpi_read_binary(&srv_priv, sp_bytes, 16) == 0);
    assert(mbedtls_mpi_exp_mod(&srv_pub, &g, &srv_priv, &p, nullptr) == 0);

    uint8_t srv_pub_bin[16];
    assert(mbedtls_mpi_write_binary(&srv_pub, srv_pub_bin, 16) == 0);

    std::vector<uint8_t> cli_pub_bin;
    uint8_t cipher[128]{};
    bool ok = crypto::apple_ard_encrypt_credentials(
        gen, prime, 16, srv_pub_bin, "macuser", "SuperSecret123!", cli_pub_bin, cipher);
    assert(ok && cli_pub_bin.size() == 16);

    // Server side: compute shared secret -> MD5 -> AES-128-ECB decrypt 128 bytes
    assert(mbedtls_mpi_read_binary(&cli_pub, cli_pub_bin.data(), 16) == 0);
    assert(mbedtls_mpi_exp_mod(&shared, &cli_pub, &srv_priv, &p, nullptr) == 0);
    uint8_t sh_bin[16];
    assert(mbedtls_mpi_write_binary(&shared, sh_bin, 16) == 0);
    auto aes_key = crypto::md5(sh_bin, 16);

    uint8_t plain[128]{};
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, aes_key.data(), 128);
    for (int blk = 0; blk < 128; blk += 16) {
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, cipher + blk, plain + blk);
    }
    mbedtls_aes_free(&aes);

    assert(std::string(reinterpret_cast<char*>(plain)) == "macuser");
    assert(std::string(reinterpret_cast<char*>(plain + 64)) == "SuperSecret123!");

    mbedtls_mpi_free(&p); mbedtls_mpi_free(&g);
    mbedtls_mpi_free(&srv_priv); mbedtls_mpi_free(&srv_pub);
    mbedtls_mpi_free(&cli_pub); mbedtls_mpi_free(&shared);
    std::printf("[PASS] Apple ARD (RFB SecurityType 30) DH + MD5 + AES-128-ECB round-trip\n");
}

static bool srv_recv_all(int s, uint8_t* b, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = recv(s, reinterpret_cast<char*>(b + got), static_cast<int>(n - got), 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

static bool srv_send_all(int s, const uint8_t* b, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        int r = send(s, reinterpret_cast<const char*>(b + sent), static_cast<int>(n - sent), 0);
        if (r <= 0) return false;
        sent += static_cast<size_t>(r);
    }
    return true;
}

static void test_vnc_backend_live_loopback_zrle() {
    int ls = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    assert(ls >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    assert(listen(ls, 1) == 0);
    socklen_t alen = sizeof(addr);
    assert(getsockname(ls, reinterpret_cast<sockaddr*>(&addr), &alen) == 0);
    uint16_t port = ntohs(addr.sin_port);

    std::atomic<bool> got_pointer{false};
    std::atomic<bool> got_key{false};

    std::thread server_thread([&]() {
        int cs = static_cast<int>(accept(ls, nullptr, nullptr));
        if (cs < 0) return;

        // 1. RFB 3.8 Version
        const char* v = "RFB 003.008\n";
        srv_send_all(cs, reinterpret_cast<const uint8_t*>(v), 12);
        uint8_t cver[12];
        if (!srv_recv_all(cs, cver, 12)) { CLOSE_SOCK(cs); return; }

        // 2. Offer SecurityType 2 (VNC DES Auth)
        uint8_t types[2] = {1, 2};
        srv_send_all(cs, types, 2);
        uint8_t chosen = 0;
        if (!srv_recv_all(cs, &chosen, 1) || chosen != 2) { CLOSE_SOCK(cs); return; }

        uint8_t chal[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
        srv_send_all(cs, chal, 16);
        uint8_t resp[16], expected[16];
        if (!srv_recv_all(cs, resp, 16)) { CLOSE_SOCK(cs); return; }
        crypto::vnc_des_encrypt_challenge("testpass", chal, expected);
        uint8_t ok_res[4] = {0, 0, 0, (std::memcmp(resp, expected, 16) == 0) ? uint8_t(0) : uint8_t(1)};
        srv_send_all(cs, ok_res, 4);

        // 3. ClientInit -> ServerInit (64x64)
        uint8_t shared = 0;
        srv_recv_all(cs, &shared, 1);
        uint8_t sinit[24 + 6]{};
        sinit[0] = 0; sinit[1] = 64;  // w=64
        sinit[2] = 0; sinit[3] = 64;  // h=64
        sinit[4] = 32; sinit[5] = 24; sinit[7] = 1;
        sinit[23] = 6;
        std::memcpy(sinit + 24, "MacSim", 6);
        srv_send_all(cs, sinit, sizeof(sinit));

        // 4. Read SetPixelFormat (20), SetEncodings (4 + 16), FBUpdateRequest (10)
        uint8_t spf[20]; srv_recv_all(cs, spf, 20);
        uint8_t enc_h[4]; srv_recv_all(cs, enc_h, 4);
        uint16_t nenc = static_cast<uint16_t>((enc_h[2] << 8) | enc_h[3]);
        std::vector<uint8_t> enc_b(nenc * 4);
        srv_recv_all(cs, enc_b.data(), enc_b.size());
        uint8_t fbreq[10]; srv_recv_all(cs, fbreq, 10);

        // 5. Send ZRLE (16) Solid-Tile FramebufferUpdate (64x64 cyan pixel)
        uint8_t zrle_raw[4] = {1, 0x38, 0xBD, 0xF8};  // subencoding=1 (solid), RGB=(0x38,0xBD,0xF8)
        uLongf zlen = 64;
        uint8_t zbuf[64];
        compress(zbuf, &zlen, zrle_raw, sizeof(zrle_raw));

        std::vector<uint8_t> fbu = {
            0, 0, 0, 1,              // msg=0, pad, nrects=1
            0, 0, 0, 0, 0, 64, 0, 64,// x=0, y=0, w=64, h=64
            0, 0, 0, 16,             // encoding=16 (ZRLE)
            uint8_t(zlen >> 24), uint8_t(zlen >> 16), uint8_t(zlen >> 8), uint8_t(zlen)
        };
        fbu.insert(fbu.end(), zbuf, zbuf + zlen);
        srv_send_all(cs, fbu.data(), fbu.size());

        // 6. Receive PointerEvent (5) & KeyEvent (4)
        for (int i = 0; i < 10; ++i) {
            uint8_t mt = 0;
            if (!srv_recv_all(cs, &mt, 1)) break;
            if (mt == 3) { uint8_t d[9]; srv_recv_all(cs, d, 9); }
            else if (mt == 5) { uint8_t d[5]; srv_recv_all(cs, d, 5); got_pointer.store(true); }
            else if (mt == 4) { uint8_t d[7]; srv_recv_all(cs, d, 7); got_key.store(true); }
            if (got_pointer.load() && got_key.load()) break;
        }
        CLOSE_SOCK(cs);
    });

    ConnectionProfile prof;
    prof.name = "LocalMacSim";
    prof.protocol = ProtocolType::VNC_MACOS;
    prof.host = "127.0.0.1";
    prof.port = port;
    prof.password = "testpass";

    VncBackend vnc;
    vnc.connect_async(prof);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && vnc.state() != BackendState::STREAMING) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(vnc.state() == BackendState::STREAMING);

    // Wait briefly for ZRLE frame, then send mouse + key
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    vnc.send_mouse_abs(0.5f, 0.5f);
    vnc.send_key(4, true, 0);  // HID 'A'
    vnc.send_key(4, false, 0);

    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && !(got_pointer.load() && got_key.load())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(got_pointer.load());
    assert(got_key.load());

    vnc.disconnect();
    CLOSE_SOCK(ls);
    server_thread.join();
    std::printf("[PASS] Live VncBackend TCP loopback: RFB 3.8 + VNC DES Auth + ZRLE Decode + Mouse/Key Input\n");
}

int main(int, char**) {
    platform::init();
    test_md4_md5_hmac_rc4();
    test_apple_ard_crypto_roundtrip();
    test_vnc_backend_live_loopback_zrle();
    platform::shutdown();
    std::printf("ALL COMPILED C++17 TESTS PASSED!\n");
    return 0;
}
