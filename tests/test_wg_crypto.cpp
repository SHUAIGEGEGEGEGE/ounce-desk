#include "../client-switch2/source/wg_crypto.hpp"
#include <cstdio>
#include <string>

static std::vector<uint8_t> hx(const char* s) {
    std::vector<uint8_t> o;
    for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
        unsigned v;
        std::sscanf(s + i, "%2x", &v);
        o.push_back(uint8_t(v));
    }
    return o;
}
static int fails = 0;
static void chk(const char* name, const uint8_t* got, const std::vector<uint8_t>& exp) {
    if (std::memcmp(got, exp.data(), exp.size()) != 0) {
        ++fails;
        std::printf("[FAIL] %s\n  got: ", name);
        for (size_t i = 0; i < exp.size(); ++i) std::printf("%02x", got[i]);
        std::printf("\n");
    } else {
        std::printf("[PASS] %s\n", name);
    }
}

int main() {
    uint8_t o[64];
    wgc::blake2s(o, 32, reinterpret_cast<const uint8_t*>("abc"), 3);
    chk("BLAKE2s-256(abc)", o, hx("508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982"));
    wgc::blake2s(o, 32, nullptr, 0);
    chk("BLAKE2s-256('')", o, hx("69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9"));
    {
        // RFC 7693 keyed test (blake2s-256, key 00..1f, empty input) = 48a8997d...
        auto key = hx("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
        wgc::blake2s(o, 32, nullptr, 0, key.data(), 32);
        chk("BLAKE2s keyed(empty)", o, hx("48a8997da407876b3d79c0d92325ad3b89cbb754d86ab71aee047ad345fd2c49"));
    }
    {
        auto k = hx("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
        auto u = hx("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
        wgc::x25519(o, k.data(), u.data());
        chk("X25519 RFC7748 #1", o, hx("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"));
        auto a = hx("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
        wgc::x25519_base(o, a.data());
        chk("X25519 Alice pub", o, hx("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"));
        auto b = hx("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
        uint8_t bpub[32], s1[32], s2[32], apub[32];
        wgc::x25519_base(bpub, b.data());
        wgc::x25519_base(apub, a.data());
        wgc::x25519(s1, a.data(), bpub);
        wgc::x25519(s2, b.data(), apub);
        chk("X25519 shared", s1, hx("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));
        chk("X25519 shared sym", s2, hx("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));
    }
    {
        auto key = hx("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b");
        const char* msg = "Cryptographic Forum Research Group";
        wgc::poly1305(o, reinterpret_cast<const uint8_t*>(msg), std::strlen(msg), key.data());
        chk("Poly1305 RFC8439", o, hx("a8061dc1305136c6c22b8baf0c0127a9"));
    }
    {
        auto key = hx("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
        auto nonce = hx("070000004041424344454647");
        auto aad = hx("50515253c0c1c2c3c4c5c6c7");
        std::string pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
        std::vector<uint8_t> ct(pt.size() + 16);
        wgc::aead_encrypt_n(ct.data(), key.data(), nonce.data(), reinterpret_cast<const uint8_t*>(pt.data()), pt.size(), aad.data(), aad.size());
        chk("ChaCha20-Poly1305 ct[0..16]", ct.data(), hx("d31a8d34648e60db7b86afbc53ef7ec2"));
        chk("ChaCha20-Poly1305 tag", ct.data() + pt.size(), hx("1ae10b594f09e26a7e902ecbd0600691"));
        std::vector<uint8_t> back(pt.size());
        bool ok = wgc::aead_decrypt_n(back.data(), key.data(), nonce.data(), ct.data(), ct.size(), aad.data(), aad.size());
        if (!ok || std::memcmp(back.data(), pt.data(), pt.size()) != 0) { ++fails; std::printf("[FAIL] AEAD decrypt\n"); }
        else std::printf("[PASS] AEAD decrypt round-trip\n");
        ct[5] ^= 1;
        if (wgc::aead_decrypt_n(back.data(), key.data(), nonce.data(), ct.data(), ct.size(), aad.data(), aad.size())) { ++fails; std::printf("[FAIL] AEAD tamper accepted\n"); }
        else std::printf("[PASS] AEAD tamper rejected\n");
    }
    {
        auto key = hx("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
        auto n = hx("000000090000004a0000000031415927");
        wgc::hchacha20(o, key.data(), n.data());
        chk("HChaCha20", o, hx("82413b4227b27bfed30e42508a877d73a0f9e4d58a74a853c12ec41326d3ecdc"));
    }
    std::printf(fails ? "WG CRYPTO TESTS FAILED\n" : "ALL WG CRYPTO TESTS PASSED\n");
    return fails;
}
