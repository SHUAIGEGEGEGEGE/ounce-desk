#pragma once
// Self-contained WireGuard cryptography (no external dependency, portable C++17):
//   BLAKE2s (RFC 7693), HMAC-BLAKE2s, WireGuard KDF1/2/3, ChaCha20-Poly1305 (RFC 8439),
//   XChaCha20-Poly1305 (cookie replies), X25519 (RFC 7748, TweetNaCl-style constant time).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace wgc {

// ------------------------------------------------------------------ helpers
inline uint32_t rd32le(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
inline void wr32le(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }
inline void wr64le(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i)); }
inline uint32_t rotr32(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }
inline uint32_t rotl32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

inline void secure_zero(void* p, size_t n) {
    volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
    while (n--) *v++ = 0;
}

inline bool ct_equal(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; ++i) d |= a[i] ^ b[i];
    return d == 0;
}

// ------------------------------------------------------------------ BLAKE2s
struct Blake2s {
    uint32_t h[8];
    uint32_t t[2];
    uint8_t buf[64];
    size_t buflen;
    size_t outlen;

    static constexpr uint32_t IV[8] = {0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
                                       0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u};

    void compress(const uint8_t* block, bool last) {
        static constexpr uint8_t S[10][16] = {
            {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
            {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
            {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
            {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
            {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
            {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
            {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
            {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
            {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
            {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
        };
        uint32_t m[16], v[16];
        for (int i = 0; i < 16; ++i) m[i] = rd32le(block + 4 * i);
        for (int i = 0; i < 8; ++i) { v[i] = h[i]; v[i + 8] = IV[i]; }
        v[12] ^= t[0];
        v[13] ^= t[1];
        if (last) v[14] = ~v[14];
        auto G = [&](int a, int b, int c, int d, uint32_t x, uint32_t y) {
            v[a] = v[a] + v[b] + x; v[d] = rotr32(v[d] ^ v[a], 16);
            v[c] = v[c] + v[d];     v[b] = rotr32(v[b] ^ v[c], 12);
            v[a] = v[a] + v[b] + y; v[d] = rotr32(v[d] ^ v[a], 8);
            v[c] = v[c] + v[d];     v[b] = rotr32(v[b] ^ v[c], 7);
        };
        for (int r = 0; r < 10; ++r) {
            const uint8_t* s = S[r];
            G(0, 4, 8, 12, m[s[0]], m[s[1]]);
            G(1, 5, 9, 13, m[s[2]], m[s[3]]);
            G(2, 6, 10, 14, m[s[4]], m[s[5]]);
            G(3, 7, 11, 15, m[s[6]], m[s[7]]);
            G(0, 5, 10, 15, m[s[8]], m[s[9]]);
            G(1, 6, 11, 12, m[s[10]], m[s[11]]);
            G(2, 7, 8, 13, m[s[12]], m[s[13]]);
            G(3, 4, 9, 14, m[s[14]], m[s[15]]);
        }
        for (int i = 0; i < 8; ++i) h[i] ^= v[i] ^ v[i + 8];
    }

    void init(size_t out_len, const uint8_t* key = nullptr, size_t keylen = 0) {
        outlen = out_len;
        for (int i = 0; i < 8; ++i) h[i] = IV[i];
        h[0] ^= 0x01010000u ^ (uint32_t(keylen) << 8) ^ uint32_t(out_len);
        t[0] = t[1] = 0;
        buflen = 0;
        std::memset(buf, 0, sizeof(buf));
        if (keylen > 0) {
            uint8_t block[64]{};
            std::memcpy(block, key, keylen);
            update(block, 64);
            secure_zero(block, sizeof(block));
        }
    }

    void update(const uint8_t* in, size_t len) {
        while (len > 0) {
            if (buflen == 64) {
                t[0] += 64;
                if (t[0] < 64) t[1]++;
                compress(buf, false);
                buflen = 0;
            }
            size_t take = 64 - buflen;
            if (take > len) take = len;
            std::memcpy(buf + buflen, in, take);
            buflen += take;
            in += take;
            len -= take;
        }
    }

    void final(uint8_t* out) {
        t[0] += uint32_t(buflen);
        if (t[0] < buflen) t[1]++;
        std::memset(buf + buflen, 0, 64 - buflen);
        compress(buf, true);
        uint8_t full[32];
        for (int i = 0; i < 8; ++i) wr32le(full + 4 * i, h[i]);
        std::memcpy(out, full, outlen);
    }
};

inline void blake2s(uint8_t* out, size_t outlen, const uint8_t* in, size_t inlen,
                    const uint8_t* key = nullptr, size_t keylen = 0) {
    Blake2s s;
    s.init(outlen, key, keylen);
    s.update(in, inlen);
    s.final(out);
}

// HMAC-BLAKE2s-256 (block size 64)
inline void hmac_blake2s(uint8_t out[32], const uint8_t* key, size_t keylen, const uint8_t* in, size_t inlen) {
    uint8_t k[64]{};
    if (keylen > 64) blake2s(k, 32, key, keylen);
    else std::memcpy(k, key, keylen);
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    uint8_t inner[32];
    Blake2s s;
    s.init(32);
    s.update(ipad, 64);
    s.update(in, inlen);
    s.final(inner);
    s.init(32);
    s.update(opad, 64);
    s.update(inner, 32);
    s.final(out);
    secure_zero(k, sizeof(k));
}

// WireGuard KDF_n: t0 = HMAC(key, input); t1 = HMAC(t0, 0x1); ti = HMAC(t0, t(i-1) || i)
inline void kdf(uint8_t* o1, uint8_t* o2, uint8_t* o3, const uint8_t key[32], const uint8_t* in, size_t inlen) {
    uint8_t t0[32], t1[32], t2[32], tmp[33];
    hmac_blake2s(t0, key, 32, in, inlen);
    uint8_t one = 1;
    hmac_blake2s(t1, t0, 32, &one, 1);
    if (o1) std::memcpy(o1, t1, 32);
    if (o2 || o3) {
        std::memcpy(tmp, t1, 32);
        tmp[32] = 2;
        hmac_blake2s(t2, t0, 32, tmp, 33);
        if (o2) std::memcpy(o2, t2, 32);
    }
    if (o3) {
        std::memcpy(tmp, t2, 32);
        tmp[32] = 3;
        uint8_t t3[32];
        hmac_blake2s(t3, t0, 32, tmp, 33);
        std::memcpy(o3, t3, 32);
    }
    secure_zero(t0, 32);
}

// ------------------------------------------------------------------ ChaCha20
inline void chacha20_block(uint32_t out[16], const uint32_t in[16]) {
    uint32_t x[16];
    std::memcpy(x, in, 64);
    auto QR = [&](int a, int b, int c, int d) {
        x[a] += x[b]; x[d] = rotl32(x[d] ^ x[a], 16);
        x[c] += x[d]; x[b] = rotl32(x[b] ^ x[c], 12);
        x[a] += x[b]; x[d] = rotl32(x[d] ^ x[a], 8);
        x[c] += x[d]; x[b] = rotl32(x[b] ^ x[c], 7);
    };
    for (int i = 0; i < 10; ++i) {
        QR(0, 4, 8, 12); QR(1, 5, 9, 13); QR(2, 6, 10, 14); QR(3, 7, 11, 15);
        QR(0, 5, 10, 15); QR(1, 6, 11, 12); QR(2, 7, 8, 13); QR(3, 4, 9, 14);
    }
    for (int i = 0; i < 16; ++i) out[i] = x[i] + in[i];
}

inline void chacha20_init_state(uint32_t st[16], const uint8_t key[32], uint32_t counter, const uint8_t nonce[12]) {
    st[0] = 0x61707865u; st[1] = 0x3320646eu; st[2] = 0x79622d32u; st[3] = 0x6b206574u;
    for (int i = 0; i < 8; ++i) st[4 + i] = rd32le(key + 4 * i);
    st[12] = counter;
    st[13] = rd32le(nonce);
    st[14] = rd32le(nonce + 4);
    st[15] = rd32le(nonce + 8);
}

inline void chacha20_xor(uint8_t* out, const uint8_t* in, size_t len, const uint8_t key[32], uint32_t counter, const uint8_t nonce[12]) {
    uint32_t st[16], ks[16];
    chacha20_init_state(st, key, counter, nonce);
    uint8_t kb[64];
    while (len > 0) {
        chacha20_block(ks, st);
        for (int i = 0; i < 16; ++i) wr32le(kb + 4 * i, ks[i]);
        size_t n = len < 64 ? len : 64;
        for (size_t i = 0; i < n; ++i) out[i] = in[i] ^ kb[i];
        out += n;
        in += n;
        len -= n;
        st[12]++;
    }
}

// HChaCha20 (for XChaCha20)
inline void hchacha20(uint8_t out[32], const uint8_t key[32], const uint8_t nonce16[16]) {
    uint32_t x[16];
    x[0] = 0x61707865u; x[1] = 0x3320646eu; x[2] = 0x79622d32u; x[3] = 0x6b206574u;
    for (int i = 0; i < 8; ++i) x[4 + i] = rd32le(key + 4 * i);
    for (int i = 0; i < 4; ++i) x[12 + i] = rd32le(nonce16 + 4 * i);
    auto QR = [&](int a, int b, int c, int d) {
        x[a] += x[b]; x[d] = rotl32(x[d] ^ x[a], 16);
        x[c] += x[d]; x[b] = rotl32(x[b] ^ x[c], 12);
        x[a] += x[b]; x[d] = rotl32(x[d] ^ x[a], 8);
        x[c] += x[d]; x[b] = rotl32(x[b] ^ x[c], 7);
    };
    for (int i = 0; i < 10; ++i) {
        QR(0, 4, 8, 12); QR(1, 5, 9, 13); QR(2, 6, 10, 14); QR(3, 7, 11, 15);
        QR(0, 5, 10, 15); QR(1, 6, 11, 12); QR(2, 7, 8, 13); QR(3, 4, 9, 14);
    }
    for (int i = 0; i < 4; ++i) wr32le(out + 4 * i, x[i]);
    for (int i = 0; i < 4; ++i) wr32le(out + 16 + 4 * i, x[12 + i]);
}

// ------------------------------------------------------------------ Poly1305 (donna-32)
inline void poly1305(uint8_t mac[16], const uint8_t* m, size_t len, const uint8_t key[32]) {
    const uint32_t r0 = (rd32le(key + 0)) & 0x3ffffff;
    const uint32_t r1 = (rd32le(key + 3) >> 2) & 0x3ffff03;
    const uint32_t r2 = (rd32le(key + 6) >> 4) & 0x3ffc0ff;
    const uint32_t r3 = (rd32le(key + 9) >> 6) & 0x3f03fff;
    const uint32_t r4 = (rd32le(key + 12) >> 8) & 0x00fffff;
    const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;

    auto block = [&](const uint8_t* b, uint32_t hibit) {
        h0 += (rd32le(b + 0)) & 0x3ffffff;
        h1 += (rd32le(b + 3) >> 2) & 0x3ffffff;
        h2 += (rd32le(b + 6) >> 4) & 0x3ffffff;
        h3 += (rd32le(b + 9) >> 6) & 0x3ffffff;
        h4 += (rd32le(b + 12) >> 8) | hibit;
        uint64_t d0 = uint64_t(h0) * r0 + uint64_t(h1) * s4 + uint64_t(h2) * s3 + uint64_t(h3) * s2 + uint64_t(h4) * s1;
        uint64_t d1 = uint64_t(h0) * r1 + uint64_t(h1) * r0 + uint64_t(h2) * s4 + uint64_t(h3) * s3 + uint64_t(h4) * s2;
        uint64_t d2 = uint64_t(h0) * r2 + uint64_t(h1) * r1 + uint64_t(h2) * r0 + uint64_t(h3) * s4 + uint64_t(h4) * s3;
        uint64_t d3 = uint64_t(h0) * r3 + uint64_t(h1) * r2 + uint64_t(h2) * r1 + uint64_t(h3) * r0 + uint64_t(h4) * s4;
        uint64_t d4 = uint64_t(h0) * r4 + uint64_t(h1) * r3 + uint64_t(h2) * r2 + uint64_t(h3) * r1 + uint64_t(h4) * r0;
        uint32_t c = uint32_t(d0 >> 26); h0 = uint32_t(d0) & 0x3ffffff;
        d1 += c; c = uint32_t(d1 >> 26); h1 = uint32_t(d1) & 0x3ffffff;
        d2 += c; c = uint32_t(d2 >> 26); h2 = uint32_t(d2) & 0x3ffffff;
        d3 += c; c = uint32_t(d3 >> 26); h3 = uint32_t(d3) & 0x3ffffff;
        d4 += c; c = uint32_t(d4 >> 26); h4 = uint32_t(d4) & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;
    };

    while (len >= 16) {
        block(m, 1u << 24);
        m += 16;
        len -= 16;
    }
    if (len > 0) {
        uint8_t last[16]{};
        std::memcpy(last, m, len);
        last[len] = 1;
        block(last, 0);
    }

    uint32_t c = h1 >> 26; h1 &= 0x3ffffff;
    h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;

    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);

    uint32_t mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;

    h0 = (h0 | (h1 << 26));
    h1 = ((h1 >> 6) | (h2 << 20));
    h2 = ((h2 >> 12) | (h3 << 14));
    h3 = ((h3 >> 18) | (h4 << 8));

    uint64_t f = uint64_t(h0) + rd32le(key + 16);             h0 = uint32_t(f);
    f = uint64_t(h1) + rd32le(key + 20) + (f >> 32);          h1 = uint32_t(f);
    f = uint64_t(h2) + rd32le(key + 24) + (f >> 32);          h2 = uint32_t(f);
    f = uint64_t(h3) + rd32le(key + 28) + (f >> 32);          h3 = uint32_t(f);
    wr32le(mac + 0, h0); wr32le(mac + 4, h1); wr32le(mac + 8, h2); wr32le(mac + 12, h3);
}

// ------------------------------------------------------------------ ChaCha20-Poly1305 AEAD (RFC 8439)
inline void aead_poly_tag(uint8_t tag[16], const uint8_t polykey[32], const uint8_t* aad, size_t aadlen, const uint8_t* ct, size_t ctlen) {
    std::vector<uint8_t> mac;
    size_t pa = (16 - aadlen % 16) % 16, pc = (16 - ctlen % 16) % 16;
    mac.reserve(aadlen + pa + ctlen + pc + 16);
    mac.insert(mac.end(), aad, aad + aadlen);
    mac.insert(mac.end(), pa, 0);
    mac.insert(mac.end(), ct, ct + ctlen);
    mac.insert(mac.end(), pc, 0);
    uint8_t lens[16];
    wr64le(lens, aadlen);
    wr64le(lens + 8, ctlen);
    mac.insert(mac.end(), lens, lens + 16);
    poly1305(tag, mac.data(), mac.size(), polykey);
}

// out must hold len + 16 bytes
inline void aead_encrypt_n(uint8_t* out, const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t* pt, size_t len, const uint8_t* aad, size_t aadlen) {
    uint8_t zeros[64]{}, block0[64];
    chacha20_xor(block0, zeros, 64, key, 0, nonce);
    chacha20_xor(out, pt, len, key, 1, nonce);
    aead_poly_tag(out + len, block0, aad, aadlen, out, len);
    secure_zero(block0, sizeof(block0));
}

// in = ciphertext + 16-byte tag; out must hold inlen - 16 bytes
inline bool aead_decrypt_n(uint8_t* out, const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t* in, size_t inlen, const uint8_t* aad, size_t aadlen) {
    if (inlen < 16) return false;
    size_t len = inlen - 16;
    uint8_t zeros[64]{}, block0[64], tag[16];
    chacha20_xor(block0, zeros, 64, key, 0, nonce);
    aead_poly_tag(tag, block0, aad, aadlen, in, len);
    secure_zero(block0, sizeof(block0));
    if (!ct_equal(tag, in + len, 16)) return false;
    chacha20_xor(out, in, len, key, 1, nonce);
    return true;
}

// WireGuard nonce: 4 zero bytes || 64-bit little-endian counter
inline void wg_nonce(uint8_t nonce[12], uint64_t counter) {
    std::memset(nonce, 0, 4);
    wr64le(nonce + 4, counter);
}

inline void aead_encrypt(uint8_t* out, const uint8_t key[32], uint64_t counter,
                         const uint8_t* pt, size_t len, const uint8_t* aad, size_t aadlen) {
    uint8_t n[12];
    wg_nonce(n, counter);
    aead_encrypt_n(out, key, n, pt, len, aad, aadlen);
}

inline bool aead_decrypt(uint8_t* out, const uint8_t key[32], uint64_t counter,
                         const uint8_t* in, size_t inlen, const uint8_t* aad, size_t aadlen) {
    uint8_t n[12];
    wg_nonce(n, counter);
    return aead_decrypt_n(out, key, n, in, inlen, aad, aadlen);
}

inline bool xaead_decrypt(uint8_t* out, const uint8_t key[32], const uint8_t nonce24[24],
                          const uint8_t* in, size_t inlen, const uint8_t* aad, size_t aadlen) {
    uint8_t subkey[32], n[12]{};
    hchacha20(subkey, key, nonce24);
    std::memcpy(n + 4, nonce24 + 16, 8);
    bool ok = aead_decrypt_n(out, subkey, n, in, inlen, aad, aadlen);
    secure_zero(subkey, sizeof(subkey));
    return ok;
}

// ------------------------------------------------------------------ X25519 (TweetNaCl)
namespace x25519_detail {
typedef int64_t gf[16];

inline void car(gf o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += (int64_t(1) << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;
    }
}
inline void sel(gf p, gf q, int b) {
    int64_t c = ~(int64_t(b) - 1);
    for (int i = 0; i < 16; ++i) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}
inline void pack(uint8_t* o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; ++i) t[i] = n[i];
    car(t); car(t); car(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = int((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) {
        o[2 * i] = uint8_t(t[i] & 0xff);
        o[2 * i + 1] = uint8_t(t[i] >> 8);
    }
}
inline void unpack(gf o, const uint8_t* n) {
    for (int i = 0; i < 16; ++i) o[i] = n[2 * i] + (int64_t(n[2 * i + 1]) << 8);
    o[15] &= 0x7fff;
}
inline void A(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i]; }
inline void Z(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i]; }
inline void M(gf o, const gf a, const gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; ++i) t[i] = 0;
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    car(o);
    car(o);
}
inline void S(gf o, const gf a) { M(o, a, a); }
inline void inv(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; ++a) c[a] = i[a];
    for (int a = 253; a >= 0; --a) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}
}  // namespace x25519_detail

inline void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    using namespace x25519_detail;
    static const gf k121665 = {0xDB41, 1};
    uint8_t z[32];
    int64_t x[80];
    gf a, b, c, d, e, f;
    for (int i = 0; i < 31; ++i) z[i] = scalar[i];
    z[31] = (scalar[31] & 127) | 64;
    z[0] &= 248;
    unpack(x, point);
    for (int i = 0; i < 16; ++i) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        sel(a, b, r);
        sel(c, d, r);
        A(e, a, c);
        Z(a, a, c);
        A(c, b, d);
        Z(b, b, d);
        S(d, e);
        S(f, a);
        M(a, c, a);
        M(c, b, e);
        A(e, a, c);
        Z(a, a, c);
        S(b, a);
        Z(c, d, f);
        M(a, c, k121665);
        A(a, a, d);
        M(c, c, a);
        M(a, d, f);
        M(d, b, x);
        S(b, e);
        sel(a, b, r);
        sel(c, d, r);
    }
    for (int i = 0; i < 16; ++i) {
        x[i + 16] = a[i];
        x[i + 32] = c[i];
        x[i + 48] = b[i];
        x[i + 64] = d[i];
    }
    inv(x + 32, x + 32);
    M(x + 16, x + 16, x + 32);
    pack(out, x + 16);
    secure_zero(z, sizeof(z));
}

inline void x25519_base(uint8_t out[32], const uint8_t scalar[32]) {
    static const uint8_t nine[32] = {9};
    x25519(out, scalar, nine);
}

// ------------------------------------------------------------------ Base64 (keys)
inline bool base64_decode(const std::string& s, std::vector<uint8_t>& out) {
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char ch : s) {
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+') v = 62;
        else if (ch == '/') v = 63;
        else if (ch == '=' || ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t') continue;
        else return false;
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t((acc >> bits) & 0xFF));
        }
    }
    return true;
}

}  // namespace wgc
