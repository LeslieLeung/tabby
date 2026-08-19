#include "tabby/wifi_station.hpp"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

extern "C" {
#include "esp_hosted_transport_config.h"
}
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

// Hosted's own constructor applies Kconfig pins. Tab5 is not the P4-EV board
// (CLK18/CMD19/D0-14/RST54). Set SDIO2 before that constructor runs.
extern "C" void __attribute__((constructor(101))) tabby_hosted_sdio_pins(void) {
    struct esp_hosted_sdio_config cfg = INIT_DEFAULT_HOST_SDIO_CONFIG();
    cfg.pin_clk.pin = 12;
    cfg.pin_cmd.pin = 13;
    cfg.pin_d0.pin = 11;
    cfg.pin_d1.pin = 10;
    cfg.pin_d2.pin = 9;
    cfg.pin_d3.pin = 8;
    cfg.pin_reset.pin = 15;
    if (esp_hosted_sdio_set_config(&cfg) != ESP_TRANSPORT_OK) {
        // Hosted constructor already ran; Kconfig pins from sdkconfig.defaults apply.
    }
}

namespace tabby {
namespace {

constexpr char kTag[] = "tabby_wifi";
constexpr EventBits_t kConnected = 1U << 0;
constexpr EventBits_t kFailed = 1U << 1;
constexpr size_t kSsidBytes = 32;
constexpr size_t kPasswordBytes = 64;
constexpr uint16_t kMaxScanAps = 64;

EventGroupHandle_t g_events = nullptr;
std::atomic<int> g_retry{0};
std::mutex g_mutex;

void fillFixed(uint8_t* dest, size_t dest_len, const std::string& src, size_t copy_max) {
    std::memset(dest, 0, dest_len);
    const size_t n = std::min(src.size(), std::min(dest_len, copy_max));
    if (n > 0) std::memcpy(dest, src.data(), n);
}

std::string ssidFromBytes(const uint8_t* ssid, size_t max_len) {
    size_t n = 0;
    while (n < max_len && ssid[n] != 0) ++n;
    return {reinterpret_cast<const char*>(ssid), n};
}

bool isOpenAuth(wifi_auth_mode_t auth) {
    return auth == WIFI_AUTH_OPEN;
}

}  // namespace

void onWifiEvent(void* arg, const char* base, int32_t id, void* data) {
    if (arg != nullptr) static_cast<WifiStation*>(arg)->handleEvent(base, id, data);
}

void WifiStation::markDisconnected() {
    connected_ = false;
    ssid_.clear();
    std::snprintf(ip_, sizeof(ip_), "0.0.0.0");
}

void WifiStation::markConnected(const std::string& ssid, const char* ip) {
    connected_ = true;
    connecting_ = false;
    ssid_ = ssid;
    std::snprintf(ip_, sizeof(ip_), "%s", ip && ip[0] ? ip : "0.0.0.0");
}

bool WifiStation::connecting() const {
    std::lock_guard<std::mutex> lock(g_mutex);
    return connecting_;
}

bool WifiStation::scanning() const {
    std::lock_guard<std::mutex> lock(g_mutex);
    return scanning_;
}

bool WifiStation::consumeScanUpdate() {
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool dirty = scan_dirty_;
    scan_dirty_ = false;
    return dirty;
}

std::vector<ScannedAp> WifiStation::scanResults() const {
    std::lock_guard<std::mutex> lock(g_mutex);
    return scan_results_;
}

std::string WifiStation::ssid() const {
    std::lock_guard<std::mutex> lock(g_mutex);
    return ssid_;
}

std::string WifiStation::ip() const {
    std::lock_guard<std::mutex> lock(g_mutex);
    return ip_;
}

std::string WifiStation::status() const {
    std::lock_guard<std::mutex> lock(g_mutex);
    return status_;
}

std::string WifiStation::lastError() const {
    std::lock_guard<std::mutex> lock(g_mutex);
    return last_error_;
}

std::string WifiStation::activeName() const {
    std::lock_guard<std::mutex> lock(g_mutex);
    return active_name_;
}

void WifiStation::collectScanResults() {
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > kMaxScanAps) count = kMaxScanAps;
    std::vector<wifi_ap_record_t> records(count);
    if (count > 0) {
        uint16_t got = count;
        if (esp_wifi_scan_get_ap_records(&got, records.data()) != ESP_OK) {
            std::lock_guard<std::mutex> lock(g_mutex);
            scanning_ = false;
            scan_dirty_ = true;
            last_error_ = "Wi-Fi scan read failed";
            status_ = last_error_;
            return;
        }
        count = got;
        records.resize(count);
    }

    std::vector<ScannedAp> results;
    results.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        ScannedAp ap;
        ap.ssid = ssidFromBytes(records[i].ssid, kSsidBytes);
        if (ap.ssid.empty()) continue;
        ap.rssi = records[i].rssi;
        ap.auth = static_cast<uint8_t>(records[i].authmode);
        ap.open = isOpenAuth(records[i].authmode);
        auto existing = std::find_if(results.begin(), results.end(),
                                     [&](const ScannedAp& other) { return other.ssid == ap.ssid; });
        if (existing == results.end()) {
            results.push_back(std::move(ap));
        } else if (ap.rssi > existing->rssi) {
            *existing = std::move(ap);
        }
    }
    std::sort(results.begin(), results.end(),
              [](const ScannedAp& a, const ScannedAp& b) { return a.rssi > b.rssi; });

    std::lock_guard<std::mutex> lock(g_mutex);
    scan_results_ = std::move(results);
    scanning_ = false;
    scan_dirty_ = true;
    status_ = std::string("Found ") + std::to_string(scan_results_.size()) + " networks";
    last_error_.clear();
}

void WifiStation::handleEvent(const char* base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        collectScanResults();
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED && data != nullptr) {
        auto* event = static_cast<wifi_event_sta_connected_t*>(data);
        const size_t n = event->ssid_len > kSsidBytes ? kSsidBytes : event->ssid_len;
        std::lock_guard<std::mutex> lock(g_mutex);
        ssid_.assign(reinterpret_cast<const char*>(event->ssid), n);
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        bool retry = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            markDisconnected();
            if (!scanning_ && enabled_ && g_retry < 8) {
                ++g_retry;
                retry = true;
            } else if (!scanning_) {
                connecting_ = false;
                last_error_ = "Wi-Fi connect failed";
                status_ = last_error_;
                if (g_events) xEventGroupSetBits(g_events, kFailed);
            }
        }
        if (retry) esp_wifi_connect();
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        g_retry = 0;
        char ip[16] = "0.0.0.0";
        if (data != nullptr) {
            auto* event = static_cast<ip_event_got_ip_t*>(data);
            std::snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        markConnected(ssid_, ip);
        status_ = std::string("Connected ") + ssid_;
        last_error_.clear();
        if (g_events) xEventGroupSetBits(g_events, kConnected);
    }
}

bool WifiStation::begin() {
    if (g_events == nullptr) {
        g_events = xEventGroupCreate();
    }
    auto fail = [&](const char* what, esp_err_t err) {
        const char* error = esp_err_to_name(err);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            last_error_ = error;
            status_ = "Wi-Fi init failed";
        }
        ESP_LOGW(kTag, "%s: %s", what, error);
        return false;
    };

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return fail("esp_netif_init", err);
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return fail("esp_event_loop_create_default", err);
    if (esp_netif_create_default_wifi_sta() == nullptr) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            last_error_ = "netif create failed";
            status_ = "Wi-Fi init failed";
        }
        ESP_LOGW(kTag, "netif create failed");
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return fail("esp_wifi_init", err);
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &onWifiEvent, this, nullptr);
    if (err != ESP_OK) return fail("wifi event register", err);
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &onWifiEvent, this, nullptr);
    if (err != ESP_OK) return fail("ip event register", err);
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return fail("esp_wifi_set_mode", err);
    err = esp_wifi_start();
    if (err != ESP_OK) return fail("esp_wifi_start", err);

    wifi_country_t country = {};
    std::memcpy(country.cc, "CN", 2);
    country.schan = 1;
    country.nchan = 13;
    country.policy = WIFI_COUNTRY_POLICY_AUTO;
    err = esp_wifi_set_country(&country);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "esp_wifi_set_country: %s", esp_err_to_name(err));
    }

    ready_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        status_ = "Wi-Fi started";
    }
    return true;
}

bool WifiStation::applyProfile(const WifiProfile& profile) {
    if (!ready_) {
        std::lock_guard<std::mutex> lock(g_mutex);
        last_error_ = "Wi-Fi not ready";
        return false;
    }
    if (!enabled_) {
        std::lock_guard<std::mutex> lock(g_mutex);
        last_error_ = "Wi-Fi disabled";
        return false;
    }
    wifi_config_t cfg = {};
    fillFixed(cfg.sta.ssid, sizeof(cfg.sta.ssid), profile.ssid, kSsidBytes);
    fillFixed(cfg.sta.password, sizeof(cfg.sta.password), profile.password, kPasswordBytes - 1);
    cfg.sta.threshold.authmode = profile.password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    g_retry = 0;
    if (g_events) xEventGroupClearBits(g_events, kConnected | kFailed);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ssid_ = profile.ssid;
        connecting_ = true;
        connected_ = false;
        active_name_ = profile.name.empty() ? profile.ssid : profile.name;
        status_ = std::string("Connecting ") + active_name_;
        last_error_.clear();
    }
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        std::lock_guard<std::mutex> lock(g_mutex);
        connecting_ = false;
        last_error_ = esp_err_to_name(err);
        status_ = last_error_;
        return false;
    }
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        std::lock_guard<std::mutex> lock(g_mutex);
        connecting_ = false;
        last_error_ = esp_err_to_name(err);
        status_ = last_error_;
        return false;
    }
    return true;
}

bool WifiStation::startConnect(const WifiProfile& profile) {
    return applyProfile(profile);
}

bool WifiStation::connect(const WifiProfile& profile, uint32_t timeout_ms) {
    if (!applyProfile(profile)) return false;
    const EventBits_t bits =
        xEventGroupWaitBits(g_events, kConnected | kFailed, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & kConnected) {
        std::lock_guard<std::mutex> lock(g_mutex);
        active_name_ = profile.name.empty() ? profile.ssid : profile.name;
        status_ = std::string("Connected ") + active_name_;
        last_error_.clear();
        return true;
    }
    // Stop the 8-retry STA path so the next profile (or the UI) cannot race GOT_IP.
    disconnect();
    std::lock_guard<std::mutex> lock(g_mutex);
    last_error_ = "No Wi-Fi profile connected";
    status_ = last_error_;
    return false;
}

bool WifiStation::connectAny(const AppConfig& config, uint32_t timeout_ms) {
    for (const auto& profile : config.wifi) {
        if (connect(profile, timeout_ms)) return true;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    last_error_ = "No Wi-Fi profile connected";
    status_ = last_error_;
    return false;
}

bool WifiStation::startScan() {
    if (!ready_) {
        std::lock_guard<std::mutex> lock(g_mutex);
        last_error_ = "Wi-Fi not ready";
        return false;
    }
    if (!enabled_) {
        std::lock_guard<std::mutex> lock(g_mutex);
        last_error_ = "Wi-Fi disabled";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (scanning_) {
            last_error_ = "Wi-Fi scan running";
            return false;
        }
        scanning_ = true;
        scan_results_.clear();
        scan_dirty_ = true;
        status_ = "Scanning...";
        last_error_.clear();
    }
    wifi_scan_config_t cfg = {};
    cfg.ssid = nullptr;
    cfg.bssid = nullptr;
    cfg.channel = 0;
    cfg.show_hidden = true;
    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    const esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        std::lock_guard<std::mutex> lock(g_mutex);
        scanning_ = false;
        last_error_ = esp_err_to_name(err);
        status_ = last_error_;
        scan_dirty_ = true;
        return false;
    }
    return true;
}

bool WifiStation::waitScan(uint32_t timeout_ms) {
    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(timeout_ms ? timeout_ms : 10000);
    while (scanning()) {
        if ((xTaskGetTickCount() - start) > limit) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return true;
}

void WifiStation::disconnect() {
    g_retry = 8;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        markDisconnected();
        connecting_ = false;
        active_name_.clear();
        status_ = "Wi-Fi disconnected";
    }
    if (ready_) esp_wifi_disconnect();
}

void WifiStation::setEnabled(bool enabled) {
    enabled_.store(enabled, std::memory_order_release);
    if (!enabled) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            scanning_ = false;
            connecting_ = false;
        }
        disconnect();
        if (ready_) esp_wifi_stop();
        std::lock_guard<std::mutex> lock(g_mutex);
        status_ = "Wi-Fi off";
    } else if (ready_) {
        esp_wifi_start();
        std::lock_guard<std::mutex> lock(g_mutex);
        status_ = "Wi-Fi on";
    }
}

}  // namespace tabby
