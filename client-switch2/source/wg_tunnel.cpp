#include "wg_tunnel.hpp"

#include "net_connect.hpp"
#include "platform.hpp"
#include "wg_crypto.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int wg_socklen_t;
#define WG_CLOSE closesocket
#define WG_EWOULDBLOCK (WSAGetLastError() == WSAEWOULDBLOCK)
#define WG_SHUT_WR SD_SEND
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef socklen_t wg_socklen_t;
#define WG_CLOSE close
#define WG_EWOULDBLOCK (errno == EAGAIN || errno == EWOULDBLOCK)
#define WG_SHUT_WR SHUT_WR
#endif

namespace wg {

namespace {

constexpr uint64_t REKEY_AFTER_US = 120ULL * 1000000ULL;
constexpr uint64_t REJECT_AFTER_US = 180ULL * 1000000ULL;
constexpr uint64_t REKEY_TIMEOUT_US = 5ULL * 1000000ULL;
constexpr size_t SNDBUF_MAX = 512 * 1024;
constexpr size_t RCV_WINDOW = 65535;

const char* CONSTRUCTION = "Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s";
const char* IDENTIFIER = "WireGuard v1 zx2c4 Jason@zx2c4.com";
const char* LABEL_MAC1 = "mac1----";
const char* LABEL_COOKIE = "cookie--";

inline void wr32(uint8_t* p, uint32_t v) { wgc::wr32le(p, v); }
inline uint32_t rd32(const uint8_t* p) { return wgc::rd32le(p); }
inline uint64_t rd64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
inline void be16w(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
inline void be32w(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
inline uint16_t be16r(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline uint32_t be32r(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

inline bool seq_lt(uint32_t a, uint32_t b) { return int32_t(a - b) < 0; }
inline bool seq_le(uint32_t a, uint32_t b) { return int32_t(a - b) <= 0; }
inline bool seq_gt(uint32_t a, uint32_t b) { return int32_t(a - b) > 0; }

void hash2(uint8_t out[32], const uint8_t* a, size_t alen, const uint8_t* b, size_t blen) {
    wgc::Blake2s s;
    s.init(32);
    s.update(a, alen);
    s.update(b, blen);
    s.final(out);
}

void set_nb(int s) {
#ifdef _WIN32
    u_long m = 1;
    ioctlsocket(s, FIONBIO, &m);
#else
    int f = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, f | O_NONBLOCK);
#endif
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool parse_ipv4(const std::string& s, uint32_t& out) {
    unsigned a, b, c, d;
    char tail;
    if (std::sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    out = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

std::string ip_str(uint32_t ip) {
    char b[32];
    std::snprintf(b, sizeof(b), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return b;
}

uint16_t inet_csum(const uint8_t* data, size_t len, uint32_t init = 0) {
    uint32_t sum = init;
    for (size_t i = 0; i + 1 < len; i += 2) sum += (uint32_t(data[i]) << 8) | data[i + 1];
    if (len & 1) sum += uint32_t(data[len - 1]) << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return uint16_t(~sum);
}

}  // namespace

// ============================================================================ config
bool parse_config(const std::string& text, Config& out, std::string& err) {
    std::istringstream in(text);
    std::string line, section;
    bool have_priv = false, have_pub = false, have_ep = false;
    while (std::getline(in, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;
        if (line[0] == '[') {
            section = line;
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        std::string lk = key;
        for (auto& c : lk) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::vector<uint8_t> raw;
        if (lk == "privatekey") {
            if (!wgc::base64_decode(val, raw) || raw.size() != 32) { err = "PrivateKey 格式错误"; return false; }
            std::memcpy(out.private_key, raw.data(), 32);
            have_priv = true;
        } else if (lk == "publickey") {
            if (!wgc::base64_decode(val, raw) || raw.size() != 32) { err = "PublicKey 格式错误"; return false; }
            std::memcpy(out.peer_public, raw.data(), 32);
            have_pub = true;
        } else if (lk == "presharedkey") {
            if (!wgc::base64_decode(val, raw) || raw.size() != 32) { err = "PresharedKey 格式错误"; return false; }
            std::memcpy(out.preshared, raw.data(), 32);
        } else if (lk == "address") {
            // take first IPv4 entry, e.g. "10.0.0.22/24, fd00::2/64"
            std::stringstream ss(val);
            std::string item;
            while (std::getline(ss, item, ',')) {
                item = trim(item);
                size_t slash = item.find('/');
                std::string ip = item.substr(0, slash);
                uint32_t v;
                if (parse_ipv4(ip, v)) { out.address = v; break; }
            }
        } else if (lk == "endpoint") {
            std::string h;
            uint16_t p = 51820;
            netc::split_host_port(val, h, p);
            out.endpoint_host = h;
            out.endpoint_port = p;
            have_ep = !h.empty();
        } else if (lk == "persistentkeepalive") {
            out.keepalive = std::max(0, std::atoi(val.c_str()));
        } else if (lk == "mtu") {
            int m = std::atoi(val.c_str());
            if (m >= 576 && m <= 1500) out.mtu = m;
        }
    }
    if (!have_priv) { err = "配置缺少 PrivateKey"; return false; }
    if (!have_pub) { err = "配置缺少 [Peer] PublicKey"; return false; }
    if (!have_ep) { err = "配置缺少 Endpoint"; return false; }
    if (out.address == 0) { err = "配置缺少 IPv4 Address"; return false; }
    if (out.keepalive == 0) out.keepalive = 25;
    return true;
}

bool load_config_file(const std::string& path, Config& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) { err = "无法打开 " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string name = path;
    size_t sl = name.find_last_of("/\\");
    if (sl != std::string::npos) name = name.substr(sl + 1);
    if (name.size() > 5 && name.substr(name.size() - 5) == ".conf") name = name.substr(0, name.size() - 5);
    out.name = name;
    return parse_config(ss.str(), out, err);
}

std::vector<std::string> list_configs(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 5 && n.substr(n.size() - 5) == ".conf") out.push_back(n.substr(0, n.size() - 5));
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

// ============================================================================ engine internals
struct Keypair {
    bool valid = false;
    uint32_t local_idx = 0;
    uint32_t remote_idx = 0;
    uint8_t send_key[32]{};
    uint8_t recv_key[32]{};
    uint64_t send_ctr = 0;
    uint64_t recv_max = 0;
    uint64_t recv_bitmap = 0;
    uint64_t created_us = 0;
};

struct Tcb {
    uint16_t sport = 0, dport = 0;
    uint32_t dst = 0;
    enum St { SYN_SENT, ESTABLISHED, CLOSED } st = SYN_SENT;
    uint32_t iss = 0, snd_una = 0, snd_nxt = 0, snd_max = 0, rcv_nxt = 0;
    uint32_t snd_wnd = 1460;
    uint16_t mss = 1360;
    std::vector<uint8_t> sbuf;          // bytes starting at snd_una (after SYN)
    std::vector<uint8_t> to_local;      // received bytes pending write to loopback socket
    std::map<uint32_t, std::vector<uint8_t>> ooo;
    uint64_t rto_deadline = 0;
    int rto_ms = 1000;
    int retries = 0;
    int dupacks = 0;
    bool rtt_timing = false;
    uint32_t rtt_seq = 0;
    uint64_t rtt_start = 0;
    double srtt_ms = 0;
    bool local_eof = false;
    bool fin_sent = false, fin_acked = false;
    bool peer_fin = false, local_shut = false;
    int lsock = -1;
    uint64_t created_us = 0;
    uint16_t last_wnd_adv = 0;
};

struct Forward {
    int listen_sock = -1;
    uint32_t dst = 0;
    uint16_t dport = 0;
    uint64_t created_us = 0;
};

struct Tunnel::Impl {
    Config cfg;
    uint8_t static_pub[32]{};
    uint8_t ss_static[32]{};        // DH(S_i, S_r) precomputed
    uint8_t ck0[32]{}, h0[32]{};    // initial chaining key / hash with responder static mixed in
    uint8_t mac1_key_peer[32]{};    // HASH(LABEL_MAC1 || S_r_pub)
    uint8_t mac1_key_self[32]{};    // HASH(LABEL_MAC1 || S_i_pub) (verify responses)
    uint8_t cookie_key[32]{};       // HASH(LABEL_COOKIE || S_r_pub)

    int usock = -1;
    sockaddr_storage ep{};
    wg_socklen_t ep_len = 0;
    uint64_t ep_resolved_us = 0;

    // handshake
    bool hs_pending = false;
    uint32_t hs_idx = 0;
    uint8_t hs_ck[32]{}, hs_h[32]{}, hs_eph[32]{};
    uint8_t last_mac1[16]{};
    uint64_t last_init_us = 0;
    uint8_t cookie[16]{};
    uint64_t cookie_us = 0;

    Keypair cur, prev;
    uint64_t last_sent_us = 0, last_recv_us = 0, last_hs_ok_us = 0, started_us = 0;

    std::vector<Forward> forwards;
    std::vector<std::unique_ptr<Tcb>> tcbs;
    uint16_t next_port = 41000;
    uint16_t ip_id = 1;

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;

    std::mutex fwd_mu;
    std::vector<Forward> pending_fwds;

    void rand_bytes(uint8_t* p, size_t n) { mbedtls_ctr_drbg_random(&drbg, p, n); }
    uint32_t rand32() { uint8_t b[4]; rand_bytes(b, 4); return rd32(b); }
};

Tunnel& Tunnel::instance() {
    static Tunnel t;
    return t;
}

Tunnel::~Tunnel() { stop(); }

void Tunnel::set_status(State st, const std::string& s) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (status_ != s) platform::log_line("[WG] " + s);
        status_ = s;
    }
    state_.store(st);
}

std::string Tunnel::status_text() const {
    std::lock_guard<std::mutex> lk(mu_);
    return status_;
}

std::string Tunnel::active_name() const {
    std::lock_guard<std::mutex> lk(mu_);
    return (state_.load() == State::OFF) ? std::string() : name_;
}

bool Tunnel::start(const Config& cfg) {
    stop();
    cfg_ = cfg;
    {
        std::lock_guard<std::mutex> lk(mu_);
        name_ = cfg.name;
    }
    rx_bytes_.store(0);
    tx_bytes_.store(0);
    stop_.store(false);
    set_status(State::CONNECTING, "WireGuard 正在连接 " + cfg.endpoint_host + ":" + std::to_string(cfg.endpoint_port) + " ...");
    thread_ = std::thread(&Tunnel::run, this);
    return true;
}

void Tunnel::stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    if (state_.load() != State::OFF) set_status(State::OFF, "WireGuard 已关闭");
}

uint16_t Tunnel::open_forward(uint32_t dst_ip, uint16_t dst_port, std::string& err) {
    if (state_.load() == State::OFF) {
        err = "WireGuard 未开启";
        return 0;
    }
    int ls = static_cast<int>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (ls < 0) { err = "WireGuard 本地转发 socket 失败"; return 0; }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7F000001u);
    a.sin_port = 0;
    if (bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(ls, 4) != 0) {
        WG_CLOSE(ls);
        err = "WireGuard 本地转发监听失败";
        return 0;
    }
    wg_socklen_t al = sizeof(a);
    getsockname(ls, reinterpret_cast<sockaddr*>(&a), &al);
    set_nb(ls);
    Forward f;
    f.listen_sock = ls;
    f.dst = dst_ip;
    f.dport = dst_port;
    f.created_us = platform::now_us();
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!impl_) {
            WG_CLOSE(ls);
            err = "WireGuard 未就绪";
            return 0;
        }
        std::lock_guard<std::mutex> lk2(impl_->fwd_mu);
        impl_->pending_fwds.push_back(f);
    }
    platform::log_line("[WG] forward 127.0.0.1:" + std::to_string(ntohs(a.sin_port)) + " -> " + ip_str(dst_ip) + ":" + std::to_string(dst_port));
    return ntohs(a.sin_port);
}

// ============================================================================ main loop
void Tunnel::run() {
    auto im = std::make_unique<Impl>();
    Impl& S = *im;
    S.cfg = cfg_;
    mbedtls_entropy_init(&S.entropy);
    mbedtls_ctr_drbg_init(&S.drbg);
    const char* pers = "OunceDeskWG";
    mbedtls_ctr_drbg_seed(&S.drbg, mbedtls_entropy_func, &S.entropy, reinterpret_cast<const uint8_t*>(pers), std::strlen(pers));

    // ---- static precomputation
    wgc::x25519_base(S.static_pub, S.cfg.private_key);
    wgc::x25519(S.ss_static, S.cfg.private_key, S.cfg.peer_public);
    wgc::blake2s(S.ck0, 32, reinterpret_cast<const uint8_t*>(CONSTRUCTION), std::strlen(CONSTRUCTION));
    uint8_t h_tmp[32];
    hash2(h_tmp, S.ck0, 32, reinterpret_cast<const uint8_t*>(IDENTIFIER), std::strlen(IDENTIFIER));
    hash2(S.h0, h_tmp, 32, S.cfg.peer_public, 32);
    hash2(S.mac1_key_peer, reinterpret_cast<const uint8_t*>(LABEL_MAC1), 8, S.cfg.peer_public, 32);
    hash2(S.mac1_key_self, reinterpret_cast<const uint8_t*>(LABEL_MAC1), 8, S.static_pub, 32);
    hash2(S.cookie_key, reinterpret_cast<const uint8_t*>(LABEL_COOKIE), 8, S.cfg.peer_public, 32);
    S.next_port = static_cast<uint16_t>(41000 + (S.rand32() % 20000));
    S.started_us = platform::now_us();

    {
        std::lock_guard<std::mutex> lk(mu_);
        impl_ = &S;
    }

    std::string ep_host = ep_override_host_.empty() ? S.cfg.endpoint_host : ep_override_host_;
    uint16_t ep_port = ep_override_port_ ? ep_override_port_ : S.cfg.endpoint_port;

    // ---- endpoint resolve + UDP socket
    auto resolve_endpoint = [&]() -> bool {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        std::string ps = std::to_string(ep_port);
        if (getaddrinfo(ep_host.c_str(), ps.c_str(), &hints, &res) != 0 || !res) return false;
        addrinfo* pick = nullptr;
        for (addrinfo* a = res; a; a = a->ai_next) if (a->ai_family == AF_INET) { pick = a; break; }
        if (!pick) pick = res;
        int fam = pick->ai_family;
        std::memcpy(&S.ep, pick->ai_addr, pick->ai_addrlen);
        S.ep_len = static_cast<wg_socklen_t>(pick->ai_addrlen);
        freeaddrinfo(res);
        if (S.usock >= 0) WG_CLOSE(S.usock);
        S.usock = static_cast<int>(socket(fam, SOCK_DGRAM, IPPROTO_UDP));
        if (S.usock < 0) return false;
        set_nb(S.usock);
        S.ep_resolved_us = platform::now_us();
        return true;
    };

    auto udp_send = [&](const uint8_t* d, size_t n) {
        if (S.usock < 0) return;
        int r = static_cast<int>(sendto(S.usock, reinterpret_cast<const char*>(d), static_cast<int>(n), 0,
                                        reinterpret_cast<sockaddr*>(&S.ep), S.ep_len));
        if (r > 0) {
            S.last_sent_us = platform::now_us();
            tx_bytes_.fetch_add(static_cast<uint64_t>(r));
        }
    };

    // ---- handshake initiation
    auto send_initiation = [&]() {
        uint8_t msg[148]{};
        msg[0] = 1;
        S.hs_idx = S.rand32();
        wr32(msg + 4, S.hs_idx);
        uint8_t ck[32], h[32], k[32], e_pub[32], dh[32];
        std::memcpy(ck, S.ck0, 32);
        std::memcpy(h, S.h0, 32);
        S.rand_bytes(S.hs_eph, 32);
        wgc::x25519_base(e_pub, S.hs_eph);
        wgc::kdf(ck, nullptr, nullptr, ck, e_pub, 32);
        std::memcpy(msg + 8, e_pub, 32);
        hash2(h, h, 32, e_pub, 32);
        wgc::x25519(dh, S.hs_eph, S.cfg.peer_public);
        wgc::kdf(ck, k, nullptr, ck, dh, 32);
        wgc::aead_encrypt(msg + 40, k, 0, S.static_pub, 32, h, 32);
        hash2(h, h, 32, msg + 40, 48);
        wgc::kdf(ck, k, nullptr, ck, S.ss_static, 32);
        // TAI64N timestamp
        uint8_t ts[12];
        auto now = std::chrono::system_clock::now().time_since_epoch();
        uint64_t secs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(now).count());
        uint32_t nanos = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() % 1000000000LL);
        uint64_t tai = 0x400000000000000aULL + secs;
        for (int i = 0; i < 8; ++i) ts[i] = uint8_t(tai >> (56 - 8 * i));
        be32w(ts + 8, nanos);
        wgc::aead_encrypt(msg + 88, k, 0, ts, 12, h, 32);
        hash2(h, h, 32, msg + 88, 28);
        wgc::blake2s(msg + 116, 16, msg, 116, S.mac1_key_peer, 32);
        std::memcpy(S.last_mac1, msg + 116, 16);
        if (S.cookie_us && platform::now_us() - S.cookie_us < 120ULL * 1000000ULL) {
            wgc::blake2s(msg + 132, 16, msg, 132, S.cookie, 16);
        }
        std::memcpy(S.hs_ck, ck, 32);
        std::memcpy(S.hs_h, h, 32);
        S.hs_pending = true;
        S.last_init_us = platform::now_us();
        udp_send(msg, sizeof(msg));
        wgc::secure_zero(k, 32);
        wgc::secure_zero(dh, 32);
    };

    // ---- transport send
    auto send_transport = [&](const uint8_t* pt, size_t len) -> bool {
        if (!S.cur.valid) return false;
        size_t padded = (len + 15) & ~size_t(15);
        std::vector<uint8_t> plain(padded, 0);
        if (len) std::memcpy(plain.data(), pt, len);
        std::vector<uint8_t> msg(16 + padded + 16);
        msg[0] = 4;
        wr32(msg.data() + 4, S.cur.remote_idx);
        uint64_t ctr = S.cur.send_ctr++;
        wgc::wr64le(msg.data() + 8, ctr);
        wgc::aead_encrypt(msg.data() + 16, S.cur.send_key, ctr, plain.data(), padded, nullptr, 0);
        udp_send(msg.data(), msg.size());
        return true;
    };
    auto send_keepalive = [&]() { send_transport(nullptr, 0); };

    // ---- TCP/IP output
    auto tcp_emit = [&](Tcb& t, uint32_t seq, const uint8_t* data, size_t len, uint8_t flags) {
        size_t opt = (flags & 0x02) ? 4 : 0;
        size_t tcp_len = 20 + opt + len;
        std::vector<uint8_t> pkt(20 + tcp_len, 0);
        uint8_t* ip = pkt.data();
        ip[0] = 0x45;
        be16w(ip + 2, static_cast<uint16_t>(pkt.size()));
        be16w(ip + 4, S.ip_id++);
        be16w(ip + 6, 0x4000);
        ip[8] = 64;
        ip[9] = 6;
        be32w(ip + 12, S.cfg.address);
        be32w(ip + 16, t.dst);
        be16w(ip + 10, inet_csum(ip, 20));
        uint8_t* tcp = ip + 20;
        be16w(tcp, t.sport);
        be16w(tcp + 2, t.dport);
        be32w(tcp + 4, seq);
        be32w(tcp + 8, (flags & 0x10) ? t.rcv_nxt : 0);
        tcp[12] = static_cast<uint8_t>(((20 + opt) / 4) << 4);
        tcp[13] = flags;
        size_t pending = t.to_local.size();
        uint16_t wnd = static_cast<uint16_t>(pending >= RCV_WINDOW ? 0 : RCV_WINDOW - pending);
        t.last_wnd_adv = wnd;
        be16w(tcp + 14, wnd);
        if (opt) {
            tcp[20] = 2; tcp[21] = 4;
            be16w(tcp + 22, static_cast<uint16_t>(S.cfg.mtu - 40 - 20));  // MSS
        }
        if (len) std::memcpy(tcp + 20 + opt, data, len);
        uint8_t pseudo[12];
        be32w(pseudo, S.cfg.address);
        be32w(pseudo + 4, t.dst);
        pseudo[8] = 0;
        pseudo[9] = 6;
        be16w(pseudo + 10, static_cast<uint16_t>(tcp_len));
        uint32_t sum = 0;
        for (int i = 0; i < 12; i += 2) sum += (uint32_t(pseudo[i]) << 8) | pseudo[i + 1];
        be16w(tcp + 16, inet_csum(tcp, tcp_len, sum));
        send_transport(pkt.data(), pkt.size());
    };

    auto tcp_abort = [&](Tcb& t, bool send_rst) {
        if (send_rst && t.st != Tcb::CLOSED) tcp_emit(t, t.snd_nxt, nullptr, 0, 0x04 | 0x10);
        t.st = Tcb::CLOSED;
        if (t.lsock >= 0) { WG_CLOSE(t.lsock); t.lsock = -1; }
    };

    auto tcp_output = [&](Tcb& t, uint64_t now) {
        if (t.st != Tcb::ESTABLISHED) return;
        uint32_t wnd = std::max<uint32_t>(t.snd_wnd, 0);
        while (true) {
            uint32_t inflight = t.snd_nxt - t.snd_una;
            size_t sent_off = t.fin_sent ? t.sbuf.size() : inflight;
            if (sent_off >= t.sbuf.size()) break;
            if (inflight >= wnd) break;
            size_t len = std::min<size_t>({static_cast<size_t>(t.mss), t.sbuf.size() - sent_off, static_cast<size_t>(wnd - inflight)});
            if (len == 0) break;
            tcp_emit(t, t.snd_nxt, t.sbuf.data() + sent_off, len, 0x18);
            t.snd_nxt += static_cast<uint32_t>(len);
            if (seq_gt(t.snd_nxt, t.snd_max)) t.snd_max = t.snd_nxt;
            if (!t.rtt_timing) { t.rtt_timing = true; t.rtt_seq = t.snd_nxt; t.rtt_start = now; }
            if (!t.rto_deadline) t.rto_deadline = now + static_cast<uint64_t>(t.rto_ms) * 1000ULL;
        }
        if (t.local_eof && !t.fin_sent && (t.snd_nxt - t.snd_una) == t.sbuf.size()) {
            tcp_emit(t, t.snd_nxt, nullptr, 0, 0x11);
            t.snd_nxt++;
            if (seq_gt(t.snd_nxt, t.snd_max)) t.snd_max = t.snd_nxt;
            t.fin_sent = true;
            if (!t.rto_deadline) t.rto_deadline = now + static_cast<uint64_t>(t.rto_ms) * 1000ULL;
        }
    };

    auto flush_to_local = [&](Tcb& t) {
        while (!t.to_local.empty() && t.lsock >= 0) {
            int n = static_cast<int>(send(t.lsock, reinterpret_cast<const char*>(t.to_local.data()),
                                          static_cast<int>(std::min<size_t>(t.to_local.size(), 65536)), 0));
            if (n > 0) {
                uint16_t before = t.last_wnd_adv;
                t.to_local.erase(t.to_local.begin(), t.to_local.begin() + n);
                // Window re-opened substantially -> send window update
                if (before < 16384 && t.to_local.size() + 16384 < RCV_WINDOW && t.st == Tcb::ESTABLISHED) {
                    tcp_emit(t, t.snd_nxt, nullptr, 0, 0x10);
                }
            } else {
                if (n < 0 && WG_EWOULDBLOCK) break;
                tcp_abort(t, true);
                return;
            }
        }
        if (t.to_local.empty() && t.peer_fin && !t.local_shut && t.lsock >= 0) {
            shutdown(t.lsock, WG_SHUT_WR);
            t.local_shut = true;
        }
    };

    auto tcp_input = [&](const uint8_t* ip, size_t iplen) {
        if (iplen < 20 || (ip[0] >> 4) != 4) return;
        size_t ihl = (ip[0] & 0x0F) * 4u;
        size_t total = be16r(ip + 2);
        if (total > iplen) total = iplen;
        if (ip[9] != 6 || total < ihl + 20) return;
        uint32_t src = be32r(ip + 12);
        const uint8_t* tcp = ip + ihl;
        size_t tcp_len = total - ihl;
        uint16_t sport = be16r(tcp), dport = be16r(tcp + 2);
        uint32_t seq = be32r(tcp + 4), ack = be32r(tcp + 8);
        size_t doff = (tcp[12] >> 4) * 4u;
        uint8_t flags = tcp[13];
        uint16_t win = be16r(tcp + 14);
        if (doff < 20 || doff > tcp_len) return;
        const uint8_t* data = tcp + doff;
        size_t dlen = tcp_len - doff;
        uint64_t now = platform::now_us();

        Tcb* tp = nullptr;
        for (auto& u : S.tcbs) if (u->st != Tcb::CLOSED && u->sport == dport && u->dport == sport && u->dst == src) { tp = u.get(); break; }
        if (!tp) return;
        Tcb& t = *tp;

        if (flags & 0x04) {  // RST
            platform::log_line("[WG] TCP RST from " + ip_str(src) + ":" + std::to_string(sport));
            tcp_abort(t, false);
            return;
        }
        if (t.st == Tcb::SYN_SENT) {
            if ((flags & 0x12) == 0x12 && ack == t.iss + 1) {
                // parse MSS option
                for (size_t o = 20; o + 1 < doff;) {
                    uint8_t kind = tcp[o];
                    if (kind == 0) break;
                    if (kind == 1) { ++o; continue; }
                    uint8_t l = tcp[o + 1];
                    if (l < 2 || o + l > doff) break;
                    if (kind == 2 && l == 4) t.mss = std::min<uint16_t>(be16r(tcp + o + 2), static_cast<uint16_t>(S.cfg.mtu - 60));
                    o += l;
                }
                t.rcv_nxt = seq + 1;
                t.snd_una = ack;
                t.snd_nxt = ack;
                t.snd_max = ack;
                t.snd_wnd = win;
                t.st = Tcb::ESTABLISHED;
                t.retries = 0;
                t.rto_deadline = 0;
                t.rto_ms = 1000;
                tcp_emit(t, t.snd_nxt, nullptr, 0, 0x10);
                platform::log_line("[WG] TCP connected " + ip_str(t.dst) + ":" + std::to_string(t.dport));
                tcp_output(t, now);
            }
            return;
        }
        if (t.st != Tcb::ESTABLISHED) return;

        // ---- ACK processing
        if (flags & 0x10) {
            if (seq_gt(ack, t.snd_una) && seq_le(ack, t.snd_max)) {
                uint32_t acked = ack - t.snd_una;
                size_t data_acked = std::min<size_t>(acked, t.sbuf.size());
                t.sbuf.erase(t.sbuf.begin(), t.sbuf.begin() + data_acked);
                if (acked > data_acked && t.fin_sent) t.fin_acked = true;
                t.snd_una = ack;
                if (seq_gt(ack, t.snd_nxt)) t.snd_nxt = ack;
                t.retries = 0;
                t.dupacks = 0;
                if (t.rtt_timing && seq_le(t.rtt_seq, ack)) {
                    double sample = static_cast<double>(now - t.rtt_start) / 1000.0;
                    t.srtt_ms = (t.srtt_ms == 0) ? sample : (0.875 * t.srtt_ms + 0.125 * sample);
                    t.rto_ms = std::max(300, std::min(8000, static_cast<int>(t.srtt_ms * 2 + 100)));
                    t.rtt_timing = false;
                }
                t.rto_deadline = (t.snd_una == t.snd_nxt) ? 0 : now + static_cast<uint64_t>(t.rto_ms) * 1000ULL;
            } else if (ack == t.snd_una && dlen == 0 && t.snd_nxt != t.snd_una && win == t.snd_wnd) {
                if (++t.dupacks == 3 && !t.sbuf.empty()) {
                    size_t len = std::min<size_t>(t.mss, t.sbuf.size());
                    tcp_emit(t, t.snd_una, t.sbuf.data(), len, 0x18);
                    t.rtt_timing = false;
                }
            }
            t.snd_wnd = win;
        }

        // ---- data
        bool need_ack = false;
        if (dlen > 0) {
            need_ack = true;
            if (seq == t.rcv_nxt) {
                t.to_local.insert(t.to_local.end(), data, data + dlen);
                t.rcv_nxt += static_cast<uint32_t>(dlen);
                // pull contiguous out-of-order segments
                while (!t.ooo.empty()) {
                    auto it = t.ooo.begin();
                    uint32_t s = it->first;
                    if (seq_gt(s, t.rcv_nxt)) break;
                    const auto& v = it->second;
                    uint32_t skip = t.rcv_nxt - s;
                    if (skip < v.size()) {
                        t.to_local.insert(t.to_local.end(), v.begin() + skip, v.end());
                        t.rcv_nxt += static_cast<uint32_t>(v.size() - skip);
                    }
                    t.ooo.erase(it);
                }
            } else if (seq_gt(seq, t.rcv_nxt)) {
                if (t.ooo.size() < 256 && seq_lt(seq, t.rcv_nxt + static_cast<uint32_t>(RCV_WINDOW * 4))) {
                    t.ooo[seq] = std::vector<uint8_t>(data, data + dlen);
                }
            } else if (seq_gt(seq + static_cast<uint32_t>(dlen), t.rcv_nxt)) {
                uint32_t skip = t.rcv_nxt - seq;
                t.to_local.insert(t.to_local.end(), data + skip, data + dlen);
                t.rcv_nxt += static_cast<uint32_t>(dlen - skip);
            }
        }
        if ((flags & 0x01) && !t.peer_fin && seq + static_cast<uint32_t>(dlen) == t.rcv_nxt) {
            t.rcv_nxt++;
            t.peer_fin = true;
            need_ack = true;
        }
        if (need_ack) tcp_emit(t, t.snd_nxt, nullptr, 0, 0x10);
        flush_to_local(t);
        tcp_output(t, now);
    };

    // ---- WG receive
    auto replay_ok = [](Keypair& kp, uint64_t ctr) {
        if (ctr > kp.recv_max) return true;
        uint64_t d = kp.recv_max - ctr;
        if (d >= 64) return false;
        return (kp.recv_bitmap & (1ULL << d)) == 0;
    };
    auto replay_mark = [](Keypair& kp, uint64_t ctr) {
        if (ctr > kp.recv_max) {
            uint64_t sh = ctr - kp.recv_max;
            kp.recv_bitmap = (sh >= 64) ? 0 : (kp.recv_bitmap << sh);
            kp.recv_bitmap |= 1;
            kp.recv_max = ctr;
        } else {
            kp.recv_bitmap |= 1ULL << (kp.recv_max - ctr);
        }
    };

    std::vector<uint8_t> plainbuf(65536);
    auto handle_udp = [&](const uint8_t* m, size_t n) {
        if (n < 4) return;
        uint8_t type = m[0];
        uint64_t now = platform::now_us();
        if (type == 2 && n == 92 && S.hs_pending) {
            if (rd32(m + 8) != S.hs_idx) return;
            uint8_t mac[16];
            wgc::blake2s(mac, 16, m, 60, S.mac1_key_self, 32);
            if (!wgc::ct_equal(mac, m + 60, 16)) return;
            uint8_t ck[32], h[32], dh[32], t[32], k[32];
            std::memcpy(ck, S.hs_ck, 32);
            std::memcpy(h, S.hs_h, 32);
            const uint8_t* e_r = m + 12;
            wgc::kdf(ck, nullptr, nullptr, ck, e_r, 32);
            hash2(h, h, 32, e_r, 32);
            wgc::x25519(dh, S.hs_eph, e_r);
            wgc::kdf(ck, nullptr, nullptr, ck, dh, 32);
            wgc::x25519(dh, S.cfg.private_key, e_r);
            wgc::kdf(ck, nullptr, nullptr, ck, dh, 32);
            wgc::kdf(ck, t, k, ck, S.cfg.preshared, 32);
            hash2(h, h, 32, t, 32);
            uint8_t empty[1];
            if (!wgc::aead_decrypt(empty, k, 0, m + 44, 16, h, 32)) {
                platform::log_line("[WG] handshake response decrypt failed (key mismatch?)");
                return;
            }
            Keypair kp;
            kp.valid = true;
            kp.local_idx = S.hs_idx;
            kp.remote_idx = rd32(m + 4);
            wgc::kdf(kp.send_key, kp.recv_key, nullptr, ck, nullptr, 0);
            kp.created_us = now;
            S.prev = S.cur;
            S.cur = kp;
            S.hs_pending = false;
            S.last_hs_ok_us = now;
            wgc::secure_zero(S.hs_eph, 32);
            send_keepalive();  // confirm keys to responder
            set_status(State::UP, "WireGuard 已连接 (" + S.cfg.name + ")  隧道 IP " + ip_str(S.cfg.address));
            return;
        }
        if (type == 3 && n == 64) {
            if (rd32(m + 4) != S.hs_idx) return;
            if (wgc::xaead_decrypt(S.cookie, S.cookie_key, m + 8, m + 32, 32, S.last_mac1, 16)) {
                S.cookie_us = now;
                platform::log_line("[WG] got cookie reply (server under load), retrying");
            }
            return;
        }
        if (type == 4 && n >= 32) {
            uint32_t idx = rd32(m + 4);
            Keypair* kp = nullptr;
            if (S.cur.valid && S.cur.local_idx == idx) kp = &S.cur;
            else if (S.prev.valid && S.prev.local_idx == idx) kp = &S.prev;
            if (!kp) return;
            uint64_t ctr = rd64(m + 8);
            if (!replay_ok(*kp, ctr)) return;
            size_t clen = n - 16;
            if (clen - 16 > plainbuf.size()) return;
            if (!wgc::aead_decrypt(plainbuf.data(), kp->recv_key, ctr, m + 16, clen, nullptr, 0)) return;
            replay_mark(*kp, ctr);
            S.last_recv_us = now;
            rx_bytes_.fetch_add(n);
            size_t plen = clen - 16;
            if (plen == 0) return;  // keepalive
            tcp_input(plainbuf.data(), plen);
        }
    };

    // ======================================================================== loop
    std::vector<uint8_t> rbuf(65536);
    uint64_t last_resolve_try = 0;
    bool resolved = false;

    while (!stop_.load()) {
        uint64_t now = platform::now_us();

        // (re)resolve endpoint (DDNS) when not yet up or every 5 min of failure
        if (!resolved || (!S.cur.valid && now - S.ep_resolved_us > 60ULL * 1000000ULL && now - last_resolve_try > 10ULL * 1000000ULL)) {
            last_resolve_try = now;
            if (resolve_endpoint()) {
                resolved = true;
            } else if (!resolved) {
                set_status(State::FAILED, "WireGuard: 无法解析 Endpoint " + ep_host + " (检查网络/DNS)");
                for (int i = 0; i < 50 && !stop_.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }

        // expire keys
        if (S.cur.valid && now - S.cur.created_us > REJECT_AFTER_US) S.cur.valid = false;
        if (S.prev.valid && now - S.prev.created_us > REJECT_AFTER_US) S.prev.valid = false;

        // handshake scheduling
        bool need_hs = !S.cur.valid || (now - S.cur.created_us > REKEY_AFTER_US);
        if (need_hs && (!S.hs_pending || now - S.last_init_us > REKEY_TIMEOUT_US)) {
            send_initiation();
        }
        if (!S.cur.valid) {
            uint64_t since = now - S.started_us;
            if (S.last_hs_ok_us == 0 && since > 20ULL * 1000000ULL && state_.load() != State::FAILED) {
                set_status(State::FAILED, "WireGuard 握手超时：请检查路由器 WireGuard 端口转发 / 公钥 / Endpoint");
            } else if (S.last_hs_ok_us != 0 && state_.load() == State::UP) {
                set_status(State::CONNECTING, "WireGuard 正在重新握手...");
            }
        }

        // keepalive
        if (S.cur.valid && S.cfg.keepalive > 0 && now - S.last_sent_us > static_cast<uint64_t>(S.cfg.keepalive) * 1000000ULL) {
            send_keepalive();
        }

        // adopt new forwards
        {
            std::lock_guard<std::mutex> lk(S.fwd_mu);
            for (auto& f : S.pending_fwds) S.forwards.push_back(f);
            S.pending_fwds.clear();
        }

        // ---- select
        fd_set rfds, wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        int maxfd = -1;
        auto addr_ = [&](int s, fd_set& set) { FD_SET(s, &set); if (s > maxfd) maxfd = s; };
        if (S.usock >= 0) addr_(S.usock, rfds);
        for (auto& f : S.forwards) addr_(f.listen_sock, rfds);
        for (auto& u : S.tcbs) {
            if (u->lsock < 0) continue;
            if (!u->local_eof && u->sbuf.size() < SNDBUF_MAX) addr_(u->lsock, rfds);
            if (!u->to_local.empty()) addr_(u->lsock, wfds);
        }
        // nearest timer
        uint64_t wait_us = 20000;
        for (auto& u : S.tcbs) {
            if (u->rto_deadline) {
                uint64_t d = (u->rto_deadline > now) ? (u->rto_deadline - now) : 0;
                wait_us = std::min(wait_us, d);
            }
        }
        timeval tv{0, static_cast<long>(std::max<uint64_t>(wait_us, 1000))};
        int sel = (maxfd >= 0) ? select(maxfd + 1, &rfds, &wfds, nullptr, &tv) : 0;
        if (maxfd < 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        now = platform::now_us();

        if (sel > 0) {
            // UDP
            if (S.usock >= 0 && FD_ISSET(S.usock, &rfds)) {
                for (int i = 0; i < 64; ++i) {
                    sockaddr_storage from{};
                    wg_socklen_t fl = sizeof(from);
                    int n = static_cast<int>(recvfrom(S.usock, reinterpret_cast<char*>(rbuf.data()), static_cast<int>(rbuf.size()), 0,
                                                      reinterpret_cast<sockaddr*>(&from), &fl));
                    if (n <= 0) break;
                    handle_udp(rbuf.data(), static_cast<size_t>(n));
                }
            }
            // accept forwards
            for (auto it = S.forwards.begin(); it != S.forwards.end();) {
                if (FD_ISSET(it->listen_sock, &rfds)) {
                    int cs = static_cast<int>(accept(it->listen_sock, nullptr, nullptr));
                    if (cs >= 0) {
                        set_nb(cs);
                        auto t = std::make_unique<Tcb>();
                        t->lsock = cs;
                        t->dst = it->dst;
                        t->dport = it->dport;
                        t->sport = S.next_port++;
                        if (S.next_port < 41000) S.next_port = 41000;
                        t->iss = S.rand32();
                        t->snd_una = t->snd_nxt = t->snd_max = t->iss + 1;
                        t->created_us = now;
                        t->rto_ms = 1000;
                        t->rto_deadline = now + 1000000ULL;
                        tcp_emit(*t, t->iss, nullptr, 0, 0x02);
                        S.tcbs.push_back(std::move(t));
                    }
                    WG_CLOSE(it->listen_sock);
                    it = S.forwards.erase(it);
                    continue;
                }
                ++it;
            }
            // local sockets
            for (auto& up : S.tcbs) {
                Tcb& t = *up;
                if (t.lsock < 0) continue;
                if (FD_ISSET(t.lsock, &wfds)) flush_to_local(t);
                if (t.lsock >= 0 && FD_ISSET(t.lsock, &rfds)) {
                    uint8_t buf[16384];
                    int n = static_cast<int>(recv(t.lsock, reinterpret_cast<char*>(buf), sizeof(buf), 0));
                    if (n > 0) {
                        t.sbuf.insert(t.sbuf.end(), buf, buf + n);
                        tcp_output(t, now);
                    } else if (n == 0) {
                        t.local_eof = true;
                        tcp_output(t, now);
                    } else if (!WG_EWOULDBLOCK) {
                        tcp_abort(t, true);
                    }
                }
            }
        }

        // expire unaccepted forwards
        for (auto it = S.forwards.begin(); it != S.forwards.end();) {
            if (now - it->created_us > 20ULL * 1000000ULL) { WG_CLOSE(it->listen_sock); it = S.forwards.erase(it); }
            else ++it;
        }

        // TCP timers
        for (auto& up : S.tcbs) {
            Tcb& t = *up;
            if (t.st == Tcb::CLOSED || !t.rto_deadline || now < t.rto_deadline) continue;
            if (++t.retries > 12) {
                platform::log_line("[WG] TCP timeout to " + ip_str(t.dst) + ":" + std::to_string(t.dport));
                tcp_abort(t, true);
                continue;
            }
            t.rtt_timing = false;
            t.rto_ms = std::min(t.rto_ms * 2, 10000);
            t.rto_deadline = now + static_cast<uint64_t>(t.rto_ms) * 1000ULL;
            if (t.st == Tcb::SYN_SENT) {
                tcp_emit(t, t.iss, nullptr, 0, 0x02);
            } else {
                // go-back-N
                t.snd_nxt = t.snd_una;
                if (t.fin_sent && !t.fin_acked) t.fin_sent = false;
                if (t.sbuf.empty() && !t.local_eof && t.snd_wnd == 0) {
                    tcp_emit(t, t.snd_una, nullptr, 0, 0x10);  // window probe
                }
                tcp_output(t, now);
            }
        }

        // cleanup
        for (auto it = S.tcbs.begin(); it != S.tcbs.end();) {
            Tcb& t = **it;
            bool done = (t.st == Tcb::CLOSED) ||
                        (t.fin_acked && t.peer_fin && t.to_local.empty()) ||
                        (t.st == Tcb::SYN_SENT && t.lsock < 0);
            if (done) {
                if (t.lsock >= 0) WG_CLOSE(t.lsock);
                it = S.tcbs.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ---- teardown
    for (auto& u : S.tcbs) {
        if (u->st == Tcb::ESTABLISHED) tcp_emit(*u, u->snd_nxt, nullptr, 0, 0x14);
        if (u->lsock >= 0) WG_CLOSE(u->lsock);
    }
    for (auto& f : S.forwards) WG_CLOSE(f.listen_sock);
    {
        std::lock_guard<std::mutex> lk(S.fwd_mu);
        for (auto& f : S.pending_fwds) WG_CLOSE(f.listen_sock);
        S.pending_fwds.clear();
    }
    if (S.usock >= 0) WG_CLOSE(S.usock);
    {
        std::lock_guard<std::mutex> lk(mu_);
        impl_ = nullptr;
    }
    wgc::secure_zero(S.cfg.private_key, 32);
    wgc::secure_zero(&S.cur, sizeof(S.cur));
    wgc::secure_zero(&S.prev, sizeof(S.prev));
    mbedtls_ctr_drbg_free(&S.drbg);
    mbedtls_entropy_free(&S.entropy);
}

// ============================================================================ connect hook
void install_connect_hook() {
    netc::tunnel_redirect() = [](const std::string& host, uint16_t port, uint16_t& out_port, std::string& err) -> bool {
        Tunnel& t = Tunnel::instance();
        if (t.state() == State::OFF) return false;
        if (host.find(':') != std::string::npos) return false;  // IPv6 target: not routed (tunnel is IPv4)
        uint32_t ip = 0;
        if (!parse_ipv4(host, ip)) {
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
                err = "WireGuard: 无法解析 " + host;
                return false;
            }
            ip = ntohl(reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr);
            freeaddrinfo(res);
        }
        // Wait briefly for the tunnel handshake so the first connect does not race it
        for (int i = 0; i < 60 && t.state() != State::UP && t.state() != State::OFF; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        out_port = t.open_forward(ip, port, err);
        return out_port != 0;
    };
}

}  // namespace wg
