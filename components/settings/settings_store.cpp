#include "tabby/settings_store.hpp"

#include "ArduinoJson.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstdint>
#include <new>
#include <string>

namespace tabby {
namespace {

constexpr char kTag[] = "tabby_settings";
constexpr char kPartition[] = "storage";
constexpr char kMount[] = "/littlefs";
constexpr char kConfigPath[] = "/littlefs/profiles.json";

extern "C" {
extern const uint8_t profiles_json_start[] asm("_binary_profiles_json_start");
extern const uint8_t profiles_json_end[] asm("_binary_profiles_json_end");
}

std::string embeddedDefault() {
    return {reinterpret_cast<const char*>(profiles_json_start),
            static_cast<size_t>(profiles_json_end - profiles_json_start)};
}

bool parseConfig(const std::string& json, AppConfig& config, std::string& error) {
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, json);
    if (err) {
        error = std::string("JSON parse failed: ") + err.c_str();
        return false;
    }

    config = AppConfig{};
    for (JsonObject item : doc["wifi"].as<JsonArray>()) {
        WifiProfile profile;
        profile.name = item["name"] | "";
        profile.ssid = item["ssid"] | "";
        profile.password = item["password"] | "";
        if (!profile.ssid.empty()) {
            if (profile.name.empty()) profile.name = profile.ssid;
            config.wifi.push_back(std::move(profile));
        }
    }
    for (JsonObject item : doc["ssh"].as<JsonArray>()) {
        SshProfile profile;
        profile.name = item["name"] | "";
        profile.host = item["host"] | "";
        profile.port = item["port"] | 22;
        profile.user = item["user"] | "";
        profile.password = item["password"] | "";
        profile.terminal = item["terminal"] | "xterm-256color";
        if (!profile.name.empty() && !profile.host.empty() && !profile.user.empty()) {
            config.ssh.push_back(std::move(profile));
        }
    }

    // BLE fields from older profiles.json are ignored on purpose.
    config.keyboard.layout = doc["keyboard"]["layout"] | "us";
    config.keyboard.terminalFont = doc["keyboard"]["terminalFont"] | "mono28";
    config.keyboard.terminalLineStep = doc["keyboard"]["terminalLineStep"] | 28;
    if (config.keyboard.terminalLineStep < 20) config.keyboard.terminalLineStep = 20;
    config.keyboard.swapCtrlCaps = doc["keyboard"]["swapCtrlCaps"] | false;
    config.system.deviceName = doc["system"]["deviceName"] | "tabby";
    config.system.region = doc["system"]["region"] | "UTC";
    config.system.utcOffsetMinutes = doc["system"]["utcOffsetMinutes"] | 0;
    config.system.timezoneAuto = doc["system"]["timezoneAuto"] | false;
    config.system.ntpServer = doc["system"]["ntpServer"] | "pool.ntp.org";
    int brightness = doc["display"]["brightness"] | DisplayConfig::kDefaultBrightness;
    if (brightness < DisplayConfig::kMinBrightness) brightness = DisplayConfig::kMinBrightness;
    if (brightness > DisplayConfig::kMaxBrightness) brightness = DisplayConfig::kMaxBrightness;
    config.display.brightness = static_cast<uint8_t>(brightness);
    config.activeWifi = doc["activeWifi"] | 0;
    config.activeSsh = doc["activeSsh"] | 0;
    if (config.activeWifi >= config.wifi.size()) config.activeWifi = 0;
    if (config.activeSsh >= config.ssh.size()) config.activeSsh = 0;
    return true;
}

std::string serializeConfig(const AppConfig& config) {
    JsonDocument doc;
    JsonArray wifi = doc["wifi"].to<JsonArray>();
    for (const auto& profile : config.wifi) {
        JsonObject item = wifi.add<JsonObject>();
        item["name"] = profile.name;
        item["ssid"] = profile.ssid;
        item["password"] = profile.password;
    }
    JsonArray ssh = doc["ssh"].to<JsonArray>();
    for (const auto& profile : config.ssh) {
        JsonObject item = ssh.add<JsonObject>();
        item["name"] = profile.name;
        item["host"] = profile.host;
        item["port"] = profile.port;
        item["user"] = profile.user;
        item["password"] = profile.password;
        item["terminal"] = profile.terminal;
    }
    doc["keyboard"]["layout"] = config.keyboard.layout;
    doc["keyboard"]["terminalFont"] = config.keyboard.terminalFont;
    doc["keyboard"]["terminalLineStep"] = config.keyboard.terminalLineStep;
    doc["keyboard"]["swapCtrlCaps"] = config.keyboard.swapCtrlCaps;
    doc["system"]["deviceName"] = config.system.deviceName;
    doc["system"]["region"] = config.system.region;
    doc["system"]["utcOffsetMinutes"] = config.system.utcOffsetMinutes;
    doc["system"]["timezoneAuto"] = config.system.timezoneAuto;
    doc["system"]["ntpServer"] = config.system.ntpServer;
    doc["display"]["brightness"] = config.display.brightness;
    doc["activeWifi"] = config.activeWifi;
    doc["activeSsh"] = config.activeSsh;

    std::string out;
    serializeJsonPretty(doc, out);
    return out;
}

bool partitionLooksBlank(const char* label) {
    const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (part == nullptr) return false;
    uint8_t buf[32];
    if (esp_partition_read(part, 0, buf, sizeof(buf)) != ESP_OK) return false;
    for (uint8_t b : buf) {
        if (b != 0xFF) return false;
    }
    return true;
}

}  // namespace

bool SettingsStore::begin() {
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = kMount;
    conf.partition_label = kPartition;
    conf.format_if_mount_failed = false;
    conf.dont_mount = false;
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK && partitionLooksBlank(kPartition)) {
        ESP_LOGW(kTag, "LittleFS partition is empty; formatting once");
        if (esp_littlefs_format(kPartition) == ESP_OK) {
            err = esp_vfs_littlefs_register(&conf);
        }
    }
    if (err != ESP_OK) {
        setError("LittleFS mount failed");
        ESP_LOGE(kTag, "LittleFS mount failed: %s (not formatting a populated partition)", esp_err_to_name(err));
        return false;
    }
    if (!startWriter()) ESP_LOGW(kTag, "%s", lastError().c_str());
    return true;
}

bool SettingsStore::writeFile(const std::string& json) {
    FILE* file = fopen(kConfigPath, "w");
    if (file == nullptr) {
        setError("profiles.json open for write failed");
        return false;
    }
    const size_t written = fwrite(json.data(), 1, json.size(), file);
    fclose(file);
    if (written != json.size()) {
        setError("profiles.json write failed");
        return false;
    }
    setError({});
    return true;
}

bool SettingsStore::load(AppConfig& config) {
    FILE* file = fopen(kConfigPath, "r");
    std::string json;
    if (file == nullptr) {
        ESP_LOGW(kTag, "profiles.json missing; installing embedded default");
        json = embeddedDefault();
        if (!writeFile(json)) return false;
    } else {
        char buffer[512];
        while (size_t n = fread(buffer, 1, sizeof(buffer), file)) {
            json.append(buffer, n);
        }
        fclose(file);
    }
    std::string error;
    const bool ok = parseConfig(json, config, error);
    setError(error);
    return ok;
}

bool SettingsStore::save(const AppConfig& config) {
    if (!startWriter()) return false;
    auto* json = new (std::nothrow) std::string(serializeConfig(config));
    if (json == nullptr) {
        setError("profiles.json save allocation failed");
        return false;
    }
    auto queue = static_cast<QueueHandle_t>(save_queue_);
    if (xQueueSend(queue, &json, 0) == pdTRUE) return true;

    // Keep the newest snapshot when several UI changes arrive faster than
    // flash can persist them. Intermediate snapshots are not observable.
    std::string* stale = nullptr;
    if (xQueueReceive(queue, &stale, 0) == pdTRUE) delete stale;
    if (xQueueSend(queue, &json, 0) == pdTRUE) return true;
    delete json;
    setError("profiles.json save queue full");
    return false;
}

std::string SettingsStore::lastError() const {
    const std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

void SettingsStore::setError(const std::string& error) {
    const std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = error;
}

bool SettingsStore::startWriter() {
    const std::lock_guard<std::mutex> lock(writer_mutex_);
    if (save_queue_ != nullptr) return true;
    QueueHandle_t queue = xQueueCreate(2, sizeof(std::string*));
    if (queue == nullptr) {
        setError("profiles.json writer queue failed");
        return false;
    }
    save_queue_ = queue;
    if (xTaskCreatePinnedToCore(writerEntry, "tabby_settings", 8192, this, 2, nullptr, 0) != pdPASS) {
        vQueueDelete(queue);
        save_queue_ = nullptr;
        setError("profiles.json writer task failed");
        return false;
    }
    return true;
}

void SettingsStore::writerEntry(void* context) {
    static_cast<SettingsStore*>(context)->writerLoop();
}

void SettingsStore::writerLoop() {
    auto queue = static_cast<QueueHandle_t>(save_queue_);
    for (;;) {
        std::string* json = nullptr;
        if (xQueueReceive(queue, &json, portMAX_DELAY) != pdTRUE || json == nullptr) continue;
        if (!writeFile(*json)) ESP_LOGW(kTag, "%s", lastError().c_str());
        delete json;
    }
}

}  // namespace tabby
