#include "tabby/time_sync.hpp"

#include "ArduinoJson.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

#include <M5Unified.h>

#include <ctime>
#include <cstdio>
#include <mutex>
#include <sys/time.h>

namespace tabby {
namespace {
constexpr char kTag[] = "tabby_time";
constexpr char kTimezoneUrl[] = "https://ipwho.is/";
constexpr size_t kMaxTimezoneResponse = 4096;
constexpr time_t kMinUnixTime = 1700000000;

void persistRtc(time_t unix_utc) {
    if (unix_utc < kMinUnixTime) return;
    if (!M5.Rtc.isEnabled()) {
        ESP_LOGW(kTag, "NTP synced, but RTC is not enabled");
        return;
    }
    struct tm utc {};
    gmtime_r(&unix_utc, &utc);
    M5.Rtc.setDateTime(&utc);
    ESP_LOGI(kTag, "RTC saved %04d-%02d-%02d %02d:%02d:%02d UTC", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
             utc.tm_hour, utc.tm_min, utc.tm_sec);
}

void onSntpSync(struct timeval* tv) {
    if (tv != nullptr) persistRtc(tv->tv_sec);
}

esp_err_t timezoneHttpEvent(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == nullptr || event->data_len <= 0 ||
        event->user_data == nullptr) {
        return ESP_OK;
    }
    auto* body = static_cast<std::string*>(event->user_data);
    const size_t incoming = static_cast<size_t>(event->data_len);
    if (body->size() + incoming > kMaxTimezoneResponse) return ESP_ERR_NO_MEM;
    body->append(static_cast<const char*>(event->data), incoming);
    return ESP_OK;
}
}

void TimeSync::configure(const SystemConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    ntp_server_ = config.ntpServer.empty() ? "pool.ntp.org" : config.ntpServer;
}

bool TimeSync::sync(uint32_t timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Reinitialize so "Sync now" always starts a fresh request instead of waiting
    // for lwIP's next hourly update.
    if (initialized_) {
        esp_netif_sntp_deinit();
        initialized_ = false;
    }
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        3, ESP_SNTP_SERVER_LIST(ntp_server_.c_str(), "time.nict.jp", "pool.ntp.org"));
    sntp.sync_cb = onSntpSync;
    if (esp_netif_sntp_init(&sntp) != ESP_OK) {
        ESP_LOGW(kTag, "SNTP init failed");
        return false;
    }
    initialized_ = true;
    const esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms ? timeout_ms : 10000));
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "SNTP wait failed: %s", esp_err_to_name(err));
        return false;
    }
    persistRtc(time(nullptr));
    return true;
}

bool TimeSync::detectTimezone(std::string& region, int16_t& utc_offset_minutes) {
    std::string body;
    body.reserve(1024);
    esp_http_client_config_t config = {};
    config.url = kTimezoneUrl;
    config.event_handler = timezoneHttpEvent;
    config.user_data = &body;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 7000;
    config.buffer_size = 1024;
    config.buffer_size_tx = 512;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGW(kTag, "Timezone HTTP client init failed");
        return false;
    }
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(kTag, "Timezone detection failed: %s, HTTP %d", esp_err_to_name(err), status);
        return false;
    }

    JsonDocument doc;
    const DeserializationError json_error = deserializeJson(doc, body);
    const bool success = doc["success"] | false;
    const char* detected_region = doc["timezone"]["id"] | "";
    const int offset_seconds = doc["timezone"]["offset"] | 1000000;
    if (json_error || !success || detected_region[0] == '\0' || offset_seconds % 60 != 0) {
        ESP_LOGW(kTag, "Timezone response was invalid");
        return false;
    }
    const int offset_minutes = offset_seconds / 60;
    if (offset_minutes < -12 * 60 || offset_minutes > 14 * 60) {
        ESP_LOGW(kTag, "Timezone offset out of range: %d", offset_minutes);
        return false;
    }
    region = detected_region;
    utc_offset_minutes = static_cast<int16_t>(offset_minutes);
    ESP_LOGI(kTag, "Timezone detected: %s (UTC%+d minutes)", region.c_str(), offset_minutes);
    return true;
}

bool TimeSync::synced() const {
    return time(nullptr) >= kMinUnixTime;
}

std::string TimeSync::formatted(const SystemConfig& config) const {
    time_t now = time(nullptr);
    if (now < kMinUnixTime) return {};
    now += static_cast<time_t>(config.utcOffsetMinutes) * 60;
    struct tm tm_local {};
    gmtime_r(&now, &tm_local);
    const int offset_hours = config.utcOffsetMinutes / 60;
    int offset_minutes = config.utcOffsetMinutes % 60;
    if (offset_minutes < 0) offset_minutes = -offset_minutes;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d UTC%+03d:%02d", tm_local.tm_year + 1900,
                  tm_local.tm_mon + 1, tm_local.tm_mday, tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec,
                  offset_hours, offset_minutes);
    return buffer;
}

std::string TimeSync::clockHm(const SystemConfig& config) const {
    time_t now = time(nullptr);
    if (now < kMinUnixTime) return "--:--";
    now += static_cast<time_t>(config.utcOffsetMinutes) * 60;
    struct tm tm_local {};
    gmtime_r(&now, &tm_local);
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", tm_local.tm_hour, tm_local.tm_min);
    return buffer;
}

}  // namespace tabby
