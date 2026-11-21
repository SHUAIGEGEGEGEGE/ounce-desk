#pragma once

#include "backend.hpp"

#include <string>
#include <vector>

class ProfileManager {
public:
    ProfileManager() = default;
    ~ProfileManager();

    void load(const std::string& path);
    void save(const std::string& path) const;

    void add_or_update(const ConnectionProfile& profile, const std::string& path);
    void remove_at(size_t idx, const std::string& path);

    // Bonjour mDNS (_rfb._tcp.local) scanner to auto-discover Macs with Screen Sharing enabled
    bool start_mdns();
    void stop_mdns();
    void send_mdns_query();
    void poll_mdns();

    // Returns saved profiles + newly discovered Bonjour Macs
    std::vector<ConnectionProfile> all_profiles() const;
    const std::vector<ConnectionProfile>& saved_profiles() const { return saved_; }

private:
    std::vector<ConnectionProfile> saved_;
    std::vector<ConnectionProfile> mdns_macs_;
    int mdns_sock_ = -1;
    uint64_t last_query_us_ = 0;
};
