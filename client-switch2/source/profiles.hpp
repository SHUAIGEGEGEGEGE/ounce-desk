#pragma once

#include "backend.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class ProfileManager {
public:
    ProfileManager() = default;
    ~ProfileManager();

    void load(const std::string& path);
    void save(const std::string& path) const;

    void add_or_update(const ConnectionProfile& profile, const std::string& path);
    void remove_saved(const ConnectionProfile& target, const std::string& path);

    // Accurate background LAN scanner:
    //  - TCP 3389 open -> Windows RDP (3389)
    //  - TCP 5900 open + RFB 003.889 banner (or RFB without 3389) -> macOS Screen Sharing (5900)
    // Windows PCs are NEVER misclassified into macOS!
    void start_lan_scan();
    void stop_lan_scan();
    bool is_scanning() const { return scanning_.load(); }
    int scan_progress_pct() const { return scan_pct_.load(); }

    // Separate filtered lists for crystal-clear Windows vs macOS UI sections
    std::vector<ConnectionProfile> windows_profiles() const;
    std::vector<ConnectionProfile> macos_profiles() const;
    std::vector<ConnectionProfile> all_profiles() const;

private:
    mutable std::mutex mu_;
    std::vector<ConnectionProfile> saved_;
    std::vector<ConnectionProfile> discovered_;

    std::atomic<bool> stop_scan_{false};
    std::atomic<bool> scanning_{false};
    std::atomic<int> scan_pct_{0};
    std::thread scan_thread_;

    void run_lan_scan();
    static bool probe_rfb_banner(uint32_t ip_net, std::string& out_banner);
};
