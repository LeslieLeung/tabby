#pragma once

#include "tabby/app_config.hpp"

#include <atomic>
#include <string>
#include <vector>

namespace tabby {

struct ScannedAp {
    std::string ssid;
    int8_t rssi{0};
    uint8_t auth{0};
    bool open{false};
};

class WifiStation {
public:
    bool begin();
    bool connect(const WifiProfile& profile, uint32_t timeout_ms);
    bool startConnect(const WifiProfile& profile);
    bool connectAny(const AppConfig& config, uint32_t timeout_ms);
    bool startScan();
    bool waitScan(uint32_t timeout_ms);
    void disconnect();
    void setEnabled(bool enabled);
    bool ready() const { return ready_.load(std::memory_order_acquire); }
    bool enabled() const { return enabled_.load(std::memory_order_acquire); }
    bool connected() const {
        return ready_.load(std::memory_order_acquire) && connected_.load(std::memory_order_acquire);
    }
    bool connecting() const;
    bool scanning() const;
    bool consumeScanUpdate();
    std::vector<ScannedAp> scanResults() const;
    std::string ip() const;
    std::string ssid() const;
    std::string status() const;
    std::string lastError() const;
    std::string activeName() const;

private:
    friend void onWifiEvent(void* arg, const char* base, int32_t id, void* data);
    void handleEvent(const char* base, int32_t id, void* data);
    void markDisconnected();
    void markConnected(const std::string& ssid, const char* ip);
    void collectScanResults();
    bool applyProfile(const WifiProfile& profile);

    std::atomic<bool> ready_{false};
    std::atomic<bool> enabled_{true};
    std::atomic<bool> connected_{false};
    bool connecting_{false};
    bool scanning_{false};
    bool scan_dirty_{false};
    char ip_[16]{"0.0.0.0"};
    std::string ssid_;
    std::string status_{"Wi-Fi idle"};
    std::string last_error_;
    std::string active_name_;
    std::vector<ScannedAp> scan_results_;
};

}  // namespace tabby
