#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <mbedtls/aes.h>
#include <mbedtls/bignum.h>

namespace crypto {

// -----------------------------------------------------------------------------
// 1. Self-contained MD4 (RFC 1320) — needed for Windows NTLMv2 NT Hash
//    (independent of mbedTLS 2.x/3.x deprecations)
// -----------------------------------------------------------------------------
inline std::array<uint8_t, 16> md4(const uint8_t* data, size_t len) {
    auto rotl = [](uint32_t x, int s) { return (x << s) | (x >> (32 - s)); };
    auto F = [](uint32_t x, uint32_t y, uint32_t z) { return (x & y) | ((~x) & z); };
    auto G = [](uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (x & z) | (y & z); };
    auto H = [](uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; };

    uint32_t A = 0x67452301u, B = 0xEFCDAB89u, C = 0x98BADCFEu, D = 0x10325476u;
    std::vector<uint8_t> msg(data, data + len);
    uint64_t bit_len = static_cast<uint64_t>(len) * 8ULL;
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) msg.push_back(0);
    for (int i = 0; i < 8; ++i) msg.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFF));

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t X[16];
        for (int i = 0; i < 16; ++i) {
            const uint8_t* p = msg.data() + off + i * 4;
            X[i] = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }
        uint32_t AA = A, BB = B, CC = C, DD = D;
        static constexpr int r1[4] = {3, 7, 11, 19};
        for (int i = 0; i < 16; ++i) {
            uint32_t t = rotl(A + F(B, C, D) + X[i], r1[i % 4]);
            A = D; D = C; C = B; B = t;
        }
        static constexpr int r2[4] = {3, 5, 9, 13};
        static constexpr int idx2[16] = {0,4,8,12, 1,5,9,13, 2,6,10,14, 3,7,11,15};
        for (int i = 0; i < 16; ++i) {
            uint32_t t = rotl(A + G(B, C, D) + X[idx2[i]] + 0x5A827999u, r2[i % 4]);
            A = D; D = C; C = B; B = t;
        }
        static constexpr int r3[4] = {3, 9, 11, 15};
        static constexpr int idx3[16] = {0,8,4,12, 2,10,6,14, 1,9,5,13, 3,11,7,15};
        for (int i = 0; i < 16; ++i) {
            uint32_t t = rotl(A + H(B, C, D) + X[idx3[i]] + 0x6ED9EBA1u, r3[i % 4]);
            A = D; D = C; C = B; B = t;
        }
        A += AA; B += BB; C += CC; D += DD;
    }

    std::array<uint8_t, 16> out{};
    uint32_t st[4] = {A, B, C, D};
    for (int i = 0; i < 4; ++i) {
        out[i * 4 + 0] = static_cast<uint8_t>(st[i] & 0xFF);
        out[i * 4 + 1] = static_cast<uint8_t>((st[i] >> 8) & 0xFF);
        out[i * 4 + 2] = static_cast<uint8_t>((st[i] >> 16) & 0xFF);
        out[i * 4 + 3] = static_cast<uint8_t>((st[i] >> 24) & 0xFF);
    }
    return out;
}

// -----------------------------------------------------------------------------
// 2. Self-contained MD5 (RFC 1321) + HMAC-MD5 — used for NTLMv2 & Apple ARD
// -----------------------------------------------------------------------------
inline std::array<uint8_t, 16> md5(const uint8_t* data, size_t len) {
    static constexpr uint32_t S[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
    };
    static constexpr uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    auto rotl = [](uint32_t x, uint32_t c) { return (x << c) | (x >> (32 - c)); };

    uint32_t a0 = 0x67452301u, b0 = 0xefcdab89u, c0 = 0x98badcfeu, d0 = 0x10325476u;
    std::vector<uint8_t> msg(data, data + len);
    uint64_t bit_len = static_cast<uint64_t>(len) * 8ULL;
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) msg.push_back(0);
    for (int i = 0; i < 8; ++i) msg.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFF));

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; ++i) {
            const uint8_t* p = msg.data() + off + i * 4;
            M[i] = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (uint32_t i = 0; i < 64; ++i) {
            uint32_t F, g;
            if (i < 16)      { F = (B & C) | ((~B) & D); g = i; }
            else if (i < 32) { F = (D & B) | ((~D) & C); g = (5 * i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D;            g = (3 * i + 5) % 16; }
            else             { F = C ^ (B | (~D));       g = (7 * i) % 16; }
            F = F + A + K[i] + M[g];
            A = D; D = C; C = B; B = B + rotl(F, S[i]);
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }

    std::array<uint8_t, 16> out{};
    uint32_t st[4] = {a0, b0, c0, d0};
    for (int i = 0; i < 4; ++i) {
        out[i * 4 + 0] = static_cast<uint8_t>(st[i] & 0xFF);
        out[i * 4 + 1] = static_cast<uint8_t>((st[i] >> 8) & 0xFF);
        out[i * 4 + 2] = static_cast<uint8_t>((st[i] >> 16) & 0xFF);
        out[i * 4 + 3] = static_cast<uint8_t>((st[i] >> 24) & 0xFF);
    }
    return out;
}

inline std::array<uint8_t, 16> hmac_md5(const uint8_t* key, size_t key_len, const uint8_t* data, size_t data_len) {
    uint8_t k[64]{};
    if (key_len > 64) {
        auto kh = md5(key, key_len);
        std::memcpy(k, kh.data(), 16);
    } else {
        std::memcpy(k, key, key_len);
    }
    std::vector<uint8_t> inner(64 + data_len);
    for (int i = 0; i < 64; ++i) inner[i] = k[i] ^ 0x36;
    if (data_len > 0) std::memcpy(inner.data() + 64, data, data_len);
    auto ih = md5(inner.data(), inner.size());

    uint8_t outer[64 + 16];
    for (int i = 0; i < 64; ++i) outer[i] = k[i] ^ 0x5C;
    std::memcpy(outer + 64, ih.data(), 16);
    return md5(outer, sizeof(outer));
}

// -----------------------------------------------------------------------------
// 3. Self-contained RC4 — used by NTLMv2 NTLM_SEAL / CredSSP pubKeyAuth
// -----------------------------------------------------------------------------
struct RC4State {
    uint8_t S[256]{};
    uint8_t i = 0, j = 0;

    void init(const uint8_t* key, size_t len) {
        for (int k = 0; k < 256; ++k) S[k] = static_cast<uint8_t>(k);
        uint8_t jj = 0;
        for (int k = 0; k < 256; ++k) {
            jj = static_cast<uint8_t>(jj + S[k] + key[k % len]);
            std::swap(S[k], S[jj]);
        }
        i = j = 0;
    }

    void process(uint8_t* buf, size_t len) {
        for (size_t n = 0; n < len; ++n) {
            i = static_cast<uint8_t>(i + 1);
            j = static_cast<uint8_t>(j + S[i]);
            std::swap(S[i], S[j]);
            buf[n] ^= S[static_cast<uint8_t>(S[i] + S[j])];
        }
    }
};

// -----------------------------------------------------------------------------
// 4. Self-contained DES-ECB with VNC reversed-bit-order key (RFB SecurityType 2)
// -----------------------------------------------------------------------------
namespace des_detail {
static constexpr uint8_t PC1[56] = {
    57,49,41,33,25,17,9,1,58,50,42,34,26,18,10,2,59,51,43,35,27,19,11,3,60,52,44,36,
    63,55,47,39,31,23,15,7,62,54,46,38,30,22,14,6,61,53,45,37,29,21,13,5,28,20,12,4
};
static constexpr uint8_t PC2[48] = {
    14,17,11,24,1,5,3,28,15,6,21,10,23,19,12,4,26,8,16,7,27,20,13,2,
    41,52,31,37,47,55,30,40,51,45,33,48,44,49,39,56,34,53,46,42,50,36,29,32
};
static constexpr uint8_t SHIFTS[16] = {1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1};
static constexpr uint8_t IP[64] = {
    58,50,42,34,26,18,10,2,60,52,44,36,28,20,12,4,62,54,46,38,30,22,14,6,64,56,48,40,32,24,16,8,
    57,49,41,33,25,17,9,1,59,51,43,35,27,19,11,3,61,53,45,37,29,21,13,5,63,55,47,39,31,23,15,7
};
static constexpr uint8_t FP[64] = {
    40,8,48,16,56,24,64,32,39,7,47,15,55,23,63,31,38,6,46,14,54,22,62,30,37,5,45,13,53,21,61,29,
    36,4,44,12,52,20,60,28,35,3,43,11,51,19,59,27,34,2,42,10,50,18,58,26,33,1,41,9,49,17,57,25
};
static constexpr uint8_t E[48] = {
    32,1,2,3,4,5,4,5,6,7,8,9,8,9,10,11,12,13,12,13,14,15,16,17,
    16,17,18,19,20,21,20,21,22,23,24,25,24,25,26,27,28,29,28,29,30,31,32,1
};
static constexpr uint8_t P[32] = {
    16,7,20,21,29,12,28,17,1,15,23,26,5,18,31,10,2,8,24,14,32,27,3,9,19,13,30,6,22,11,4,25
};
static constexpr uint8_t SBOX[8][64] = {
    {14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,
     4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0,15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13},
    {15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,
     0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15,13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9},
    {10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8,13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,
     13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12},
    {7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15,13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,
     10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14},
    {2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9,14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,
     4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14,11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3},
    {12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,
     9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13},
    {4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1,13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,
     1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12},
    {13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,
     7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11}
};

inline uint64_t permute(uint64_t in, const uint8_t* tbl, int in_bits, int out_bits) {
    uint64_t out = 0;
    for (int i = 0; i < out_bits; ++i) {
        out = (out << 1) | ((in >> (in_bits - tbl[i])) & 1ULL);
    }
    return out;
}

inline void des_encrypt_block(const uint8_t key8[8], const uint8_t in8[8], uint8_t out8[8]) {
    uint64_t k64 = 0, block = 0;
    for (int i = 0; i < 8; ++i) {
        k64 = (k64 << 8) | key8[i];
        block = (block << 8) | in8[i];
    }
    uint64_t k56 = permute(k64, PC1, 64, 56);
    uint32_t C = static_cast<uint32_t>((k56 >> 28) & 0x0FFFFFFFu);
    uint32_t D = static_cast<uint32_t>(k56 & 0x0FFFFFFFu);
    uint64_t subkeys[16];
    for (int r = 0; r < 16; ++r) {
        int s = SHIFTS[r];
        C = ((C << s) | (C >> (28 - s))) & 0x0FFFFFFFu;
        D = ((D << s) | (D >> (28 - s))) & 0x0FFFFFFFu;
        subkeys[r] = permute((uint64_t(C) << 28) | D, PC2, 56, 48);
    }

    uint64_t ip = permute(block, IP, 64, 64);
    uint32_t L = static_cast<uint32_t>(ip >> 32);
    uint32_t R = static_cast<uint32_t>(ip & 0xFFFFFFFFu);
    for (int r = 0; r < 16; ++r) {
        uint64_t er = permute(R, E, 32, 48) ^ subkeys[r];
        uint32_t sout = 0;
        for (int b = 0; b < 8; ++b) {
            uint8_t six = static_cast<uint8_t>((er >> (42 - b * 6)) & 0x3Fu);
            uint8_t row = ((six & 0x20u) >> 4) | (six & 0x01u);
            uint8_t col = (six >> 1) & 0x0Fu;
            sout = (sout << 4) | SBOX[b][row * 16 + col];
        }
        uint32_t f = static_cast<uint32_t>(permute(sout, P, 32, 32));
        uint32_t next_r = L ^ f;
        L = R;
        R = next_r;
    }
    uint64_t fp = permute((uint64_t(R) << 32) | L, FP, 64, 64);
    for (int i = 7; i >= 0; --i) {
        out8[i] = static_cast<uint8_t>(fp & 0xFFu);
        fp >>= 8;
    }
}
}  // namespace des_detail

inline void vnc_des_encrypt_challenge(const std::string& password, const uint8_t challenge[16], uint8_t response[16]) {
    uint8_t key[8]{};
    for (size_t i = 0; i < 8 && i < password.size(); ++i) {
        // Classic RFB SecurityType 2 reverses the bit order of each password byte!
        uint8_t b = static_cast<uint8_t>(password[i]);
        uint8_t rev = 0;
        for (int bit = 0; bit < 8; ++bit) {
            rev = static_cast<uint8_t>((rev << 1) | ((b >> bit) & 1u));
        }
        key[i] = rev;
    }
    des_detail::des_encrypt_block(key, challenge, response);
    des_detail::des_encrypt_block(key, challenge + 8, response + 8);
}

// -----------------------------------------------------------------------------
// 5. Apple Remote Desktop (ARD, RFB SecurityType 30) DH + MD5 + AES-128-ECB
// -----------------------------------------------------------------------------
inline bool apple_ard_encrypt_credentials(
    uint16_t generator,
    const uint8_t* prime_be, size_t key_len,
    const uint8_t* server_pub_be,
    const std::string& username,
    const std::string& password,
    std::vector<uint8_t>& out_client_pub_be,
    uint8_t out_ciphertext[128]) {

    mbedtls_mpi p, g, server_pub, priv, client_pub, shared;
    mbedtls_mpi_init(&p);
    mbedtls_mpi_init(&g);
    mbedtls_mpi_init(&server_pub);
    mbedtls_mpi_init(&priv);
    mbedtls_mpi_init(&client_pub);
    mbedtls_mpi_init(&shared);

    bool ok = false;
    do {
        if (mbedtls_mpi_read_binary(&p, prime_be, key_len) != 0) break;
        if (mbedtls_mpi_lset(&g, generator) != 0) break;
        if (mbedtls_mpi_read_binary(&server_pub, server_pub_be, key_len) != 0) break;

        // Deterministic-safe private exponent (non-zero, < p)
        std::vector<uint8_t> priv_bytes(key_len);
        for (size_t i = 0; i < key_len; ++i) {
            priv_bytes[i] = static_cast<uint8_t>((0x5A + i * 37) ^ (username.size() * 13));
        }
        priv_bytes[0] &= 0x7F;
        priv_bytes[key_len - 1] |= 1;
        if (mbedtls_mpi_read_binary(&priv, priv_bytes.data(), key_len) != 0) break;

        if (mbedtls_mpi_exp_mod(&client_pub, &g, &priv, &p, nullptr) != 0) break;
        if (mbedtls_mpi_exp_mod(&shared, &server_pub, &priv, &p, nullptr) != 0) break;

        out_client_pub_be.resize(key_len);
        std::vector<uint8_t> shared_bytes(key_len);
        if (mbedtls_mpi_write_binary(&client_pub, out_client_pub_be.data(), key_len) != 0) break;
        if (mbedtls_mpi_write_binary(&shared, shared_bytes.data(), key_len) != 0) break;

        auto aes_key = md5(shared_bytes.data(), shared_bytes.size());

        // Plaintext = 64 bytes NUL-padded username + 64 bytes NUL-padded password (128 bytes total)
        uint8_t plain[128]{};
        std::memcpy(plain, username.data(), std::min<size_t>(63, username.size()));
        std::memcpy(plain + 64, password.data(), std::min<size_t>(63, password.size()));

        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_enc(&aes, aes_key.data(), 128);
        for (int blk = 0; blk < 128; blk += 16) {
            mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, plain + blk, out_ciphertext + blk);
        }
        mbedtls_aes_free(&aes);
        ok = true;
    } while (false);

    mbedtls_mpi_free(&p);
    mbedtls_mpi_free(&g);
    mbedtls_mpi_free(&server_pub);
    mbedtls_mpi_free(&priv);
    mbedtls_mpi_free(&client_pub);
    mbedtls_mpi_free(&shared);
    return ok;
}

// -----------------------------------------------------------------------------
// 6. UTF-16LE helper + ASN.1 DER encoder (for Windows RDP CredSSP / NTLMv2)
// -----------------------------------------------------------------------------
inline std::vector<uint8_t> utf8_to_utf16le(const std::string& s, bool uppercase_ascii = false) {
    std::vector<uint8_t> out;
    out.reserve(s.size() * 2);
    for (unsigned char ch : s) {
        uint16_t u = ch;
        if (uppercase_ascii && u >= 'a' && u <= 'z') u = static_cast<uint16_t>(u - 'a' + 'A');
        out.push_back(static_cast<uint8_t>(u & 0xFF));
        out.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
    }
    return out;
}

inline std::vector<uint8_t> der_wrap(uint8_t tag, const uint8_t* data, size_t len) {
    std::vector<uint8_t> out;
    out.push_back(tag);
    if (len < 0x80) {
        out.push_back(static_cast<uint8_t>(len));
    } else if (len <= 0xFF) {
        out.push_back(0x81);
        out.push_back(static_cast<uint8_t>(len));
    } else {
        out.push_back(0x82);
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(len & 0xFF));
    }
    out.insert(out.end(), data, data + len);
    return out;
}

inline std::vector<uint8_t> der_wrap(uint8_t tag, const std::vector<uint8_t>& body) {
    return der_wrap(tag, body.data(), body.size());
}

inline std::vector<uint8_t> der_integer(uint32_t v) {
    std::vector<uint8_t> b;
    if (v == 0) {
        b.push_back(0);
    } else {
        while (v > 0) {
            b.insert(b.begin(), static_cast<uint8_t>(v & 0xFF));
            v >>= 8;
        }
        if (b[0] & 0x80) b.insert(b.begin(), 0x00);
    }
    return der_wrap(0x02, b);
}

inline bool der_read_tlv(const uint8_t* data, size_t len, size_t& pos, uint8_t& out_tag, const uint8_t*& out_val, size_t& out_vlen) {
    if (pos + 2 > len) return false;
    out_tag = data[pos++];
    uint8_t lb = data[pos++];
    size_t vlen = 0;
    if ((lb & 0x80) == 0) {
        vlen = lb;
    } else {
        int nbytes = lb & 0x7F;
        if (nbytes == 0 || nbytes > 4 || pos + nbytes > len) return false;
        for (int i = 0; i < nbytes; ++i) vlen = (vlen << 8) | data[pos++];
    }
    if (pos + vlen > len) return false;
    out_val = data + pos;
    out_vlen = vlen;
    pos += vlen;
    return true;
}

}  // namespace crypto
