#include "tabby/sd_card.hpp"

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstddef>
#include <dirent.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace tabby {
namespace {

constexpr char kTag[] = "tabby_sd";
constexpr char kMount[] = "/sd";
constexpr int kSdLdoChannel = 4;
constexpr gpio_num_t kClk = GPIO_NUM_43;
constexpr gpio_num_t kCmd = GPIO_NUM_44;
constexpr gpio_num_t kD0 = GPIO_NUM_39;
constexpr gpio_num_t kD1 = GPIO_NUM_40;
constexpr gpio_num_t kD2 = GPIO_NUM_41;
constexpr gpio_num_t kD3 = GPIO_NUM_42;
constexpr char kPermsFile[] = "/.tab5perms";

sdmmc_card_t* g_card = nullptr;
sd_pwr_ctrl_handle_t g_power = nullptr;
std::mutex g_probe_mutex;

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir == "/") return "/" + name;
    return dir + "/" + name;
}

bool isMissingCard(esp_err_t err) {
    return err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_NOT_FOUND ||
           err == ESP_ERR_NOT_SUPPORTED;
}

}  // namespace

bool SdCard::begin() { return mount(); }

void SdCard::resetMountedState() {
    ready_.store(false, std::memory_order_release);
    g_card = nullptr;
    raw_card_owned_ = false;
    cwd_ = "/";
    modes_loaded_ = false;
    modes_.clear();
}

bool SdCard::ensurePower() {
    if (g_power != nullptr) return true;
    const sd_pwr_ctrl_ldo_config_t power_cfg = {
        .ldo_chan_id = kSdLdoChannel,
    };
    const esp_err_t power_err = sd_pwr_ctrl_new_on_chip_ldo(&power_cfg, &g_power);
    if (power_err != ESP_OK) {
        last_error_ = std::string("SD power init failed: ") + esp_err_to_name(power_err);
        ESP_LOGE(kTag, "%s", last_error_.c_str());
        return false;
    }
    return true;
}

void SdCard::fillHostAndSlot(sdmmc_host_t& host, sdmmc_slot_config_t& slot) {
    // Tab5 wires its microSD slot to SDMMC slot 0. Slot 1 is used by the
    // ESP32-C6 Wi-Fi coprocessor, so keeping the two slots separate is required.
    host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = g_power;

    slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = kClk;
    slot.cmd = kCmd;
    slot.d0 = kD0;
    slot.d1 = kD1;
    slot.d2 = kD2;
    slot.d3 = kD3;
}

void SdCard::fillInfoFromCard(SdCardInfo& result) {
    if (g_card == nullptr) return;
    result.card_name = g_card->cid.name;
    result.type = g_card->is_mmc ? "MMC" : (g_card->csd.capacity <= 2097152 ? "SDSC" : "SDHC/SDXC");
    result.card_bytes = static_cast<uint64_t>(g_card->csd.capacity) * g_card->csd.sector_size;
}

int SdCard::mountLocked(bool format_if_mount_failed) {
    if (ready_.load(std::memory_order_acquire)) return ESP_OK;
    if (usb_mode_.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    if (!ensurePower()) return ESP_FAIL;

    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    fillHostAndSlot(host, slot);

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = format_if_mount_failed;
    mount_cfg.max_files = 8;
    mount_cfg.allocation_unit_size = 16 * 1024;

    const esp_err_t err = esp_vfs_fat_sdmmc_mount(kMount, &host, &slot, &mount_cfg, &g_card);
    if (err != ESP_OK) {
        resetMountedState();
        if (isMissingCard(err)) {
            last_error_ = "no SD card";
            ESP_LOGI(kTag, "No microSD card (%s), continuing without /sd", esp_err_to_name(err));
        } else if (err == ESP_FAIL) {
            last_error_ = "card found but FAT mount failed";
            ESP_LOGW(kTag, "microSD detected but FAT mount failed; exFAT and unformatted cards are unsupported");
        } else {
            last_error_ = std::string("SD mount failed: ") + esp_err_to_name(err);
            ESP_LOGW(kTag, "%s", last_error_.c_str());
        }
        return err;
    }
    raw_card_owned_ = false;
    ready_.store(true, std::memory_order_release);
    last_error_.clear();
    ESP_LOGI(kTag, "mounted at %s", kMount);
    return ESP_OK;
}

bool SdCard::unmountLocked() {
    if (!ready_.load(std::memory_order_acquire) || g_card == nullptr) {
        resetMountedState();
        last_error_.clear();
        return true;
    }
    const esp_err_t err = esp_vfs_fat_sdcard_unmount(kMount, g_card);
    if (err == ESP_OK) {
        resetMountedState();
        last_error_.clear();
        ESP_LOGI(kTag, "unmounted from %s", kMount);
        return true;
    }
    last_error_ = std::string("SD unmount failed: ") + esp_err_to_name(err);
    ESP_LOGW(kTag, "%s", last_error_.c_str());
    return false;
}

int SdCard::initRawCardLocked() {
    if (g_card != nullptr && raw_card_owned_) return ESP_OK;
    if (!ensurePower()) return ESP_FAIL;

    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    fillHostAndSlot(host, slot);

    auto* card = static_cast<sdmmc_card_t*>(std::calloc(1, sizeof(sdmmc_card_t)));
    if (card == nullptr) {
        last_error_ = "SD card alloc failed";
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = host.init();
    if (err != ESP_OK) {
        std::free(card);
        last_error_ = std::string("SD host init failed: ") + esp_err_to_name(err);
        return err;
    }
    err = sdmmc_host_init_slot(host.slot, &slot);
    if (err != ESP_OK) {
        if (host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) host.deinit_p(host.slot);
        else host.deinit();
        std::free(card);
        last_error_ = std::string("SD slot init failed: ") + esp_err_to_name(err);
        return err;
    }
    err = sdmmc_card_init(&host, card);
    if (err != ESP_OK) {
        if (host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) host.deinit_p(host.slot);
        else host.deinit();
        std::free(card);
        if (isMissingCard(err)) last_error_ = "no SD card";
        else last_error_ = std::string("SD card init failed: ") + esp_err_to_name(err);
        return err;
    }
    g_card = card;
    raw_card_owned_ = true;
    last_error_.clear();
    ESP_LOGI(kTag, "raw SD card ready for USB");
    return ESP_OK;
}

void SdCard::deinitRawCardLocked() {
    if (g_card == nullptr || !raw_card_owned_) {
        g_card = nullptr;
        raw_card_owned_ = false;
        return;
    }
    if (g_card->host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) g_card->host.deinit_p(g_card->host.slot);
    else if (g_card->host.deinit) g_card->host.deinit();
    std::free(g_card);
    g_card = nullptr;
    raw_card_owned_ = false;
}

bool SdCard::mount() {
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;
    const std::lock_guard<std::mutex> lock(g_probe_mutex);
    if (usb_mode_.load(std::memory_order_acquire)) {
        last_error_ = "SD card is in USB drive mode";
        busy_.store(false, std::memory_order_release);
        return false;
    }
    const bool ok = mountLocked(false) == ESP_OK;
    busy_.store(false, std::memory_order_release);
    return ok;
}

bool SdCard::unmount() {
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;
    const std::lock_guard<std::mutex> lock(g_probe_mutex);
    if (usb_mode_.load(std::memory_order_acquire)) {
        last_error_ = "SD card is in USB drive mode";
        busy_.store(false, std::memory_order_release);
        return false;
    }
    const bool ok = unmountLocked();
    busy_.store(false, std::memory_order_release);
    return ok;
}

bool SdCard::format() {
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;
    const std::lock_guard<std::mutex> lock(g_probe_mutex);
    if (usb_mode_.load(std::memory_order_acquire)) {
        last_error_ = "SD card is in USB drive mode";
        busy_.store(false, std::memory_order_release);
        return false;
    }

    bool formatted_during_mount = false;
    esp_err_t err = ESP_OK;
    if (!ready_.load(std::memory_order_acquire)) {
        err = mountLocked(false);
        if (err == ESP_FAIL) {
            // A card was initialized but did not contain a mountable FAT volume.
            // The mount helper can create the partition/filesystem and mount it.
            err = mountLocked(true);
            formatted_during_mount = err == ESP_OK;
        }
    }
    if (err == ESP_OK && !formatted_during_mount) {
        err = esp_vfs_fat_sdcard_format(kMount, g_card);
    }

    if (err == ESP_OK) {
        cwd_ = "/";
        modes_loaded_ = false;
        modes_.clear();
        last_error_.clear();
        ESP_LOGI(kTag, "formatted and mounted at %s", kMount);
    } else {
        uint64_t total = 0;
        uint64_t free_bytes = 0;
        if (esp_vfs_fat_info(kMount, &total, &free_bytes) != ESP_OK) resetMountedState();
        last_error_ = std::string("SD format failed: ") + esp_err_to_name(err);
        ESP_LOGE(kTag, "%s", last_error_.c_str());
    }
    busy_.store(false, std::memory_order_release);
    return err == ESP_OK;
}

bool SdCard::prepareForUsb() {
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        last_error_ = "SD card is busy";
        return false;
    }
    const std::lock_guard<std::mutex> lock(g_probe_mutex);
    if (usb_mode_.load(std::memory_order_acquire) && g_card != nullptr && raw_card_owned_) {
        busy_.store(false, std::memory_order_release);
        return true;
    }
    usb_mode_.store(true, std::memory_order_release);
    if (ready_.load(std::memory_order_acquire) && !unmountLocked()) {
        usb_mode_.store(false, std::memory_order_release);
        busy_.store(false, std::memory_order_release);
        return false;
    }
    const int err = initRawCardLocked();
    if (err != ESP_OK) {
        usb_mode_.store(false, std::memory_order_release);
        busy_.store(false, std::memory_order_release);
        return false;
    }
    busy_.store(false, std::memory_order_release);
    return true;
}

bool SdCard::restoreFromUsb() {
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        last_error_ = "SD card is busy";
        return false;
    }
    const std::lock_guard<std::mutex> lock(g_probe_mutex);
    deinitRawCardLocked();
    usb_mode_.store(false, std::memory_order_release);
    const bool ok = mountLocked(false) == ESP_OK;
    busy_.store(false, std::memory_order_release);
    return ok;
}

sdmmc_card_t* SdCard::rawCard() { return g_card; }

bool SdCard::ensureReady() {
    if (usb_mode_.load(std::memory_order_acquire)) {
        last_error_ = "SD card is in USB drive mode";
        return false;
    }
    if (busy_.load(std::memory_order_acquire)) {
        last_error_ = "SD card is busy";
        return false;
    }
    if (ready_.load(std::memory_order_acquire)) return true;
    return mount();
}

SdCardInfo SdCard::info() {
    SdCardInfo result;
    result.busy = busy_.load(std::memory_order_acquire);
    result.usb_drive = usb_mode_.load(std::memory_order_acquire);
    if (result.busy) return result;
    const std::lock_guard<std::mutex> lock(g_probe_mutex);
    result.usb_drive = usb_mode_.load(std::memory_order_acquire);
    result.mounted = ready_.load(std::memory_order_acquire) && g_card != nullptr;
    result.error = last_error_;
    if (g_card == nullptr) return result;
    fillInfoFromCard(result);
    if (result.mounted) esp_vfs_fat_info(kMount, &result.total_bytes, &result.free_bytes);
    return result;
}

std::string SdCard::virtualPath(const std::string& input) const {
    std::string path = input;
    auto trim = [](std::string& text) {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.erase(text.begin());
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    };
    trim(path);
    if (path.empty()) path = cwd_;
    else if (path[0] != '/') {
        path = cwd_;
        if (path.back() != '/') path += '/';
        path += input;
        trim(path);
    }
    while (path.find("//") != std::string::npos) {
        auto pos = path.find("//");
        path.replace(pos, 2, "/");
    }
    if (path.empty() || path[0] != '/') path = "/" + path;
    if (path.size() > 1 && path.back() == '/') path.pop_back();

    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/') {
            if (!cur.empty()) {
                parts.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(path[i]);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    std::vector<std::string> out;
    for (const auto& part : parts) {
        if (part == ".") continue;
        if (part == "..") {
            if (!out.empty()) out.pop_back();
            continue;
        }
        out.push_back(part);
    }
    if (out.empty()) return "/";
    std::string result;
    for (const auto& part : out) {
        result += '/';
        result += part;
    }
    return result;
}

std::string SdCard::fsPath(const std::string& virtual_path) const {
    if (virtual_path == "/") return kMount;
    return std::string(kMount) + virtual_path;
}

std::string SdCard::formatBytes(uint64_t bytes) {
    char buf[24];
    if (bytes < 1024) {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    } else if (bytes < 1024ULL * 1024ULL) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return buf;
}

std::string SdCard::formatTime(time_t t) {
    if (t <= 0) return "Jan  1  1980";
    struct tm tm_value {};
    localtime_r(&t, &tm_value);
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buf[16];
    const int mon = tm_value.tm_mon < 0 ? 0 : (tm_value.tm_mon > 11 ? 11 : tm_value.tm_mon);
    std::snprintf(buf, sizeof(buf), "%s %2d %02d:%02d", months[mon], tm_value.tm_mday, tm_value.tm_hour,
                  tm_value.tm_min);
    return buf;
}

std::string SdCard::modeString(uint16_t mode, bool directory) {
    std::string out = directory ? "d" : "-";
    const uint16_t bits[] = {0400, 0200, 0100, 0040, 0020, 0010, 0004, 0002, 0001};
    const char chars[] = {'r', 'w', 'x', 'r', 'w', 'x', 'r', 'w', 'x'};
    for (size_t i = 0; i < 9; ++i) out += (mode & bits[i]) ? chars[i] : '-';
    return out;
}

std::string SdCard::basename(const std::string& path) {
    const auto pos = path.find_last_of('/');
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

bool SdCard::parseOctalMode(const std::string& text, uint16_t& mode) {
    if (text.empty() || text.size() > 4) return false;
    uint16_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '7') return false;
        value = static_cast<uint16_t>((value << 3) + (c - '0'));
    }
    mode = value & 0777;
    return true;
}

void SdCard::loadModes() {
    if (modes_loaded_ || !ready_) return;
    modes_loaded_ = true;
    modes_.clear();
    FILE* file = std::fopen(fsPath(kPermsFile).c_str(), "r");
    if (file == nullptr) return;
    char line[256];
    while (std::fgets(line, sizeof(line), file)) {
        std::string text = line;
        if (!text.empty() && text.back() == '\n') text.pop_back();
        if (!text.empty() && text.back() == '\r') text.pop_back();
        const auto split = text.find(' ');
        if (split == std::string::npos) continue;
        uint16_t mode = 0;
        if (!parseOctalMode(text.substr(0, split), mode)) continue;
        std::string path = text.substr(split + 1);
        if (!path.empty()) modes_.push_back({path, mode});
    }
    std::fclose(file);
}

void SdCard::saveModes() {
    if (!ready_) return;
    FILE* file = std::fopen(fsPath(kPermsFile).c_str(), "w");
    if (file == nullptr) return;
    for (const auto& entry : modes_) {
        std::fprintf(file, "%03o %s\n", entry.mode & 0777, entry.path.c_str());
    }
    std::fclose(file);
}

uint16_t SdCard::modeFor(const std::string& virtual_path, bool directory) {
    loadModes();
    for (const auto& entry : modes_) {
        if (entry.path == virtual_path) return entry.mode;
    }
    return directory ? 0755 : 0644;
}

void SdCard::setMode(const std::string& virtual_path, uint16_t mode) {
    loadModes();
    for (auto& entry : modes_) {
        if (entry.path == virtual_path) {
            entry.mode = mode;
            saveModes();
            return;
        }
    }
    modes_.push_back({virtual_path, mode});
    saveModes();
}

bool SdCard::exists(const std::string& virtual_path, bool* directory) {
    struct stat st {};
    if (stat(fsPath(virtual_path).c_str(), &st) != 0) return false;
    if (directory) *directory = S_ISDIR(st.st_mode);
    return true;
}

bool SdCard::hasRead(const std::string& virtual_path) {
    if (!ensureReady()) return false;
    bool directory = false;
    exists(virtual_path, &directory);
    return (modeFor(virtual_path, directory) & 0400) != 0;
}

bool SdCard::hasWrite(const std::string& virtual_path) {
    if (!ensureReady()) return false;
    bool directory = false;
    exists(virtual_path, &directory);
    return (modeFor(virtual_path, directory) & 0200) != 0;
}

bool SdCard::hasExecute(const std::string& virtual_path) {
    if (!ensureReady()) return false;
    return (modeFor(virtual_path, true) & 0100) != 0;
}

std::string SdCard::statusText() {
    if (!ensureReady()) return "sd=not present (optional)";
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    esp_vfs_fat_info(kMount, &total, &free_bytes);
    const uint64_t used = total > free_bytes ? total - free_bytes : 0;
    const char* type = "SD";
    if (g_card) {
        if (g_card->is_mmc) type = "MMC";
        else if (g_card->csd.capacity <= 2097152) type = "SDSC";
        else type = "SDHC";
    }
    char line[192];
    std::snprintf(line, sizeof(line), "sd=ready type=%s card=%s size=%s used=%s avail=%s", type,
                  formatBytes(g_card ? static_cast<uint64_t>(g_card->csd.capacity) * g_card->csd.sector_size : total)
                      .c_str(),
                  formatBytes(total).c_str(), formatBytes(used).c_str(), formatBytes(free_bytes).c_str());
    return std::string(line) + "\ncwd=" + cwd_;
}

std::string SdCard::dfText() {
    if (!ensureReady()) return "sd: " + last_error_;
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    esp_vfs_fat_info(kMount, &total, &free_bytes);
    const uint64_t used = total > free_bytes ? total - free_bytes : 0;
    const uint32_t use_pct = total ? static_cast<uint32_t>((used * 100ULL + total - 1) / total) : 0;
    char line[160];
    std::snprintf(line, sizeof(line), "Filesystem      Size  Used Avail Use%% Mounted on\nmicroSD         %s  %s  %s  %u%% /sd",
                  formatBytes(total).c_str(), formatBytes(used).c_str(), formatBytes(free_bytes).c_str(),
                  static_cast<unsigned>(use_pct));
    return line;
}

bool SdCard::cd(const std::string& input, std::string& message) {
    if (!ensureReady()) {
        message = "sd: " + last_error_;
        return false;
    }
    const std::string path = virtualPath(input);
    bool directory = false;
    if (!exists(path, &directory) || !directory) {
        message = "sd cd: not a directory: " + path;
        return false;
    }
    if (!hasExecute(path)) {
        message = "sd cd: permission denied: " + path;
        return false;
    }
    cwd_ = path;
    message = cwd_;
    return true;
}

bool SdCard::parseLsOptions(const std::string& input, LsOptions& options, std::string& error) const {
    options = {};
    std::string rest = input;
    auto next = [&](std::string& token) {
        while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.erase(rest.begin());
        if (rest.empty()) return false;
        const auto split = rest.find(' ');
        token = split == std::string::npos ? rest : rest.substr(0, split);
        rest = split == std::string::npos ? std::string() : rest.substr(split + 1);
        return true;
    };
    std::string token;
    while (next(token)) {
        if (token == "--") {
            while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.erase(rest.begin());
            if (!rest.empty()) {
                if (!options.path.empty()) {
                    error = "ls: multiple paths are not supported";
                    return false;
                }
                options.path = rest;
            }
            break;
        }
        if (token.size() > 1 && token[0] == '-') {
            for (size_t i = 1; i < token.size(); ++i) {
                const char opt = token[i];
                if (opt == 'l') options.long_format = true;
                else if (opt == 'a') options.all = true;
                else if (opt == 'h') options.human = true;
                else {
                    error = std::string("ls: unsupported option -- ") + opt;
                    return false;
                }
            }
            continue;
        }
        if (!options.path.empty()) {
            error = "ls: multiple paths are not supported";
            return false;
        }
        options.path = token;
    }
    return true;
}

bool SdCard::list(const std::string& input, std::vector<std::string>& lines, std::string& error) {
    lines.clear();
    if (!ensureReady()) {
        error = "sd: " + last_error_;
        return false;
    }
    LsOptions options;
    if (!parseLsOptions(input, options, error)) return false;
    const std::string path = virtualPath(options.path);
    bool directory = false;
    if (!exists(path, &directory)) {
        error = "sd ls: cannot open " + path;
        return false;
    }
    if (directory && !hasExecute(path)) {
        error = "ls: cannot open directory '" + path + "': Permission denied";
        return false;
    }
    auto display = [&](const std::string& name, const std::string& item_path, bool is_dir, uint64_t size, time_t mtime) {
        if (!options.long_format) return name + (is_dir ? "/" : "");
        char size_text[16];
        if (options.human) {
            std::snprintf(size_text, sizeof(size_text), "%8s", formatBytes(is_dir ? 0 : size).c_str());
        } else {
            std::snprintf(size_text, sizeof(size_text), "%8llu", static_cast<unsigned long long>(is_dir ? 0 : size));
        }
        return modeString(modeFor(item_path, is_dir), is_dir) + " 1 tabby tabby " + size_text + " " + formatTime(mtime) +
               " " + name + (is_dir ? "/" : "");
    };

    if (!directory) {
        struct stat st {};
        stat(fsPath(path).c_str(), &st);
        lines.push_back(display(basename(path), path, false, static_cast<uint64_t>(st.st_size), st.st_mtime));
        return true;
    }

    DIR* dir = opendir(fsPath(path).c_str());
    if (dir == nullptr) {
        error = "sd ls: cannot open " + path;
        return false;
    }
    std::vector<std::string> names;
    while (dirent* ent = readdir(dir)) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        if (!options.all && !name.empty() && name[0] == '.') continue;
        names.push_back(name);
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        const std::string item = joinPath(path, name);
        struct stat st {};
        stat(fsPath(item).c_str(), &st);
        lines.push_back(display(name, item, S_ISDIR(st.st_mode), static_cast<uint64_t>(st.st_size), st.st_mtime));
    }
    return true;
}

bool SdCard::cat(const std::string& input, std::vector<std::string>& lines, std::string& error) {
    lines.clear();
    if (!ensureReady()) {
        error = "sd: " + last_error_;
        return false;
    }
    const std::string path = virtualPath(input);
    bool directory = false;
    if (!exists(path, &directory) || directory) {
        error = "cat: " + path + ": No such file";
        return false;
    }
    if (!hasRead(path)) {
        error = "cat: permission denied: " + path;
        return false;
    }
    FILE* file = std::fopen(fsPath(path).c_str(), "r");
    if (file == nullptr) {
        error = "cat: cannot open " + path;
        return false;
    }
    char buf[256];
    std::string current;
    while (std::fgets(buf, sizeof(buf), file)) {
        current += buf;
        if (!current.empty() && current.back() == '\n') {
            current.pop_back();
            if (!current.empty() && current.back() == '\r') current.pop_back();
            lines.push_back(current);
            current.clear();
        }
        if (lines.size() > 400) {
            lines.push_back("cat: truncated");
            break;
        }
    }
    if (!current.empty()) lines.push_back(current);
    std::fclose(file);
    return true;
}

bool SdCard::readFile(const std::string& input, std::string& text, std::string& error, size_t max_bytes,
                      bool* missing) {
    text.clear();
    if (missing) *missing = false;
    if (!ensureReady()) {
        error = last_error_;
        return false;
    }
    const std::string path = virtualPath(input);
    bool directory = false;
    if (!exists(path, &directory)) {
        if (missing) *missing = true;
        error = "No such file";
        return false;
    }
    if (directory) {
        error = "Is a directory";
        return false;
    }
    if (!hasRead(path)) {
        error = "Permission denied";
        return false;
    }
    struct stat st {};
    if (stat(fsPath(path).c_str(), &st) != 0) {
        error = "Cannot stat " + path;
        return false;
    }
    const int64_t bytes64 = static_cast<int64_t>(st.st_size);
    if (bytes64 < 0) {
        error = "Invalid size";
        return false;
    }
    const size_t bytes = static_cast<size_t>(bytes64);
    if (max_bytes > 0 && bytes > max_bytes) {
        error = "File too large (max " + std::to_string(max_bytes) + " bytes)";
        return false;
    }
    FILE* file = std::fopen(fsPath(path).c_str(), "rb");
    if (file == nullptr) {
        error = "Cannot open " + path;
        return false;
    }
    text.resize(bytes);
    const size_t n = bytes == 0 ? 0 : std::fread(text.data(), 1, bytes, file);
    std::fclose(file);
    if (n != bytes) {
        text.clear();
        error = "Read failed: " + path;
        return false;
    }
    return true;
}

bool SdCard::writeFile(const std::string& input, const std::string& text, std::string& error) {
    if (!ensureReady()) {
        error = last_error_;
        return false;
    }
    const std::string path = virtualPath(input);
    if (exists(path) && !hasWrite(path)) {
        error = "Permission denied";
        return false;
    }
    FILE* file = std::fopen(fsPath(path).c_str(), "wb");
    if (file == nullptr) {
        error = "Cannot open " + path;
        return false;
    }
    const size_t n = text.empty() ? 0 : std::fwrite(text.data(), 1, text.size(), file);
    const int err = std::fclose(file);
    if (n != text.size() || err != 0) {
        error = "Write failed: " + path;
        return false;
    }
    return true;
}

bool SdCard::writeText(const std::string& input_path, const std::string& text, bool append, std::string& message) {
    if (!ensureReady()) {
        message = "sd: " + last_error_;
        return false;
    }
    const std::string path = virtualPath(input_path);
    if (exists(path) && !hasWrite(path)) {
        message = std::string(append ? "sd append" : "sd write") + ": permission denied: " + path;
        return false;
    }
    FILE* file = std::fopen(fsPath(path).c_str(), append ? "a" : "w");
    if (file == nullptr) {
        message = "sd write: cannot open " + path;
        return false;
    }
    std::fprintf(file, "%s\n", text.c_str());
    std::fclose(file);
    message = std::string(append ? "appended " : "wrote ") + path;
    return true;
}

bool SdCard::mkdir(const std::string& input, std::string& message) {
    if (!ensureReady()) {
        message = "sd: " + last_error_;
        return false;
    }
    const std::string path = virtualPath(input);
    if (::mkdir(fsPath(path).c_str(), 0755) != 0) {
        message = "mkdir failed: " + path;
        return false;
    }
    message = "created " + path;
    return true;
}

bool SdCard::rmdir(const std::string& input, std::string& message) {
    if (!ensureReady()) {
        message = "sd: " + last_error_;
        return false;
    }
    const std::string path = virtualPath(input);
    if (::rmdir(fsPath(path).c_str()) != 0) {
        message = "rmdir failed: " + path;
        return false;
    }
    message = "removed " + path;
    return true;
}

bool SdCard::removeFile(const std::string& input, std::string& message) {
    if (!ensureReady()) {
        message = "sd: " + last_error_;
        return false;
    }
    const std::string path = virtualPath(input);
    if (exists(path) && !hasWrite(path)) {
        message = "sd rm: permission denied: " + path;
        return false;
    }
    if (unlink(fsPath(path).c_str()) != 0) {
        message = "sd rm failed: " + path;
        return false;
    }
    message = "removed " + path;
    return true;
}

bool SdCard::chmod(const std::string& mode_text, const std::string& input, std::string& message) {
    if (!ensureReady()) {
        message = "sd: " + last_error_;
        return false;
    }
    const std::string path = virtualPath(input);
    if (!exists(path)) {
        message = "chmod: cannot access '" + path + "'";
        return false;
    }
    uint16_t mode = 0;
    if (!parseOctalMode(mode_text, mode)) {
        message = "chmod: invalid mode";
        return false;
    }
    setMode(path, mode);
    message = "mode " + mode_text + " " + path;
    return true;
}

bool SdCard::resolveCopyDest(const std::string& src, const std::string& dst_input, std::string& dest, std::string& message) {
    dest = virtualPath(dst_input);
    bool dest_dir = false;
    if (exists(dest, &dest_dir) && dest_dir) {
        dest = virtualPath(dest + "/" + basename(src));
    }
    if (dest == src) {
        message = "source and destination are the same: " + src;
        return false;
    }
    return true;
}

bool SdCard::copyFile(const std::string& src_input, const std::string& dst_input, std::string& message) {
    if (!ensureReady()) {
        message = "sd: " + last_error_;
        return false;
    }
    const std::string src = virtualPath(src_input);
    bool src_dir = false;
    if (!exists(src, &src_dir)) {
        message = "cp: no such file: " + src;
        return false;
    }
    if (src_dir) {
        message = "cp: skipping directory: " + src;
        return false;
    }
    if (!hasRead(src)) {
        message = "cp: permission denied: " + src;
        return false;
    }
    std::string dest;
    if (!resolveCopyDest(src, dst_input, dest, message)) {
        message = "cp: " + message;
        return false;
    }
    if (exists(dest) && !hasWrite(dest)) {
        message = "cp: permission denied: " + dest;
        return false;
    }
    FILE* in = std::fopen(fsPath(src).c_str(), "rb");
    if (in == nullptr) {
        message = "cp: cannot open " + src;
        return false;
    }
    FILE* out = std::fopen(fsPath(dest).c_str(), "wb");
    if (out == nullptr) {
        std::fclose(in);
        message = "cp: cannot create " + dest;
        return false;
    }
    char buf[4096];
    bool ok = true;
    while (ok) {
        const size_t n = std::fread(buf, 1, sizeof(buf), in);
        if (n == 0) {
            ok = std::feof(in) != 0;
            break;
        }
        if (std::fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    std::fclose(in);
    if (std::fclose(out) != 0) ok = false;
    if (!ok) {
        message = "cp: write failed: " + dest;
        return false;
    }
    message = src + " -> " + dest;
    return true;
}

bool SdCard::moveFile(const std::string& src_input, const std::string& dst_input, std::string& message) {
    if (!ensureReady()) {
        message = "sd: " + last_error_;
        return false;
    }
    const std::string src = virtualPath(src_input);
    bool src_dir = false;
    if (!exists(src, &src_dir)) {
        message = "mv: no such file: " + src;
        return false;
    }
    if (src_dir) {
        message = "mv: skipping directory: " + src;
        return false;
    }
    if (!hasRead(src) || !hasWrite(src)) {
        message = "mv: permission denied: " + src;
        return false;
    }
    std::string dest;
    if (!resolveCopyDest(src, dst_input, dest, message)) {
        message = "mv: " + message;
        return false;
    }
    if (exists(dest) && !hasWrite(dest)) {
        message = "mv: permission denied: " + dest;
        return false;
    }
    if (rename(fsPath(src).c_str(), fsPath(dest).c_str()) == 0) {
        message = src + " -> " + dest;
        return true;
    }
    if (!copyFile(src, dest, message)) {
        if (message.find("cp:") == 0) message.replace(0, 3, "mv");
        return false;
    }
    std::string removed;
    if (!removeFile(src, removed)) {
        message = "mv: copied but could not remove " + src;
        return false;
    }
    message = src + " -> " + dest;
    return true;
}

bool SdCard::touch(const std::string& input, std::string& message) {
    if (!ensureReady()) {
        message = "sd: " + last_error_;
        return false;
    }
    const std::string path = virtualPath(input);
    bool directory = false;
    if (exists(path, &directory) && directory) {
        message = "touch: is a directory: " + path;
        return false;
    }
    if (exists(path) && !hasWrite(path)) {
        message = "touch: permission denied: " + path;
        return false;
    }
    FILE* file = std::fopen(fsPath(path).c_str(), exists(path) ? "ab" : "wb");
    if (file == nullptr) {
        message = "touch: cannot open " + path;
        return false;
    }
    std::fclose(file);
    message = path;
    return true;
}

bool SdCard::head(const std::string& input, size_t line_count, std::vector<std::string>& out, std::string& error) {
    out.clear();
    if (!ensureReady()) {
        error = "sd: " + last_error_;
        return false;
    }
    const std::string path = virtualPath(input);
    bool directory = false;
    if (!exists(path, &directory) || directory) {
        error = "head: no such file: " + path;
        return false;
    }
    if (!hasRead(path)) {
        error = "head: permission denied: " + path;
        return false;
    }
    FILE* file = std::fopen(fsPath(path).c_str(), "r");
    if (file == nullptr) {
        error = "head: cannot open " + path;
        return false;
    }
    const size_t limit = line_count == 0 ? 10 : line_count;
    char buf[256];
    std::string current;
    while (out.size() < limit && std::fgets(buf, sizeof(buf), file)) {
        current += buf;
        if (!current.empty() && current.back() == '\n') {
            current.pop_back();
            if (!current.empty() && current.back() == '\r') current.pop_back();
            out.push_back(current);
            current.clear();
        }
    }
    if (out.size() < limit && !current.empty()) out.push_back(current);
    std::fclose(file);
    return true;
}

bool SdCard::tail(const std::string& input, size_t line_count, std::vector<std::string>& out, std::string& error) {
    out.clear();
    if (!ensureReady()) {
        error = "sd: " + last_error_;
        return false;
    }
    const std::string path = virtualPath(input);
    bool directory = false;
    if (!exists(path, &directory) || directory) {
        error = "tail: no such file: " + path;
        return false;
    }
    if (!hasRead(path)) {
        error = "tail: permission denied: " + path;
        return false;
    }
    FILE* file = std::fopen(fsPath(path).c_str(), "r");
    if (file == nullptr) {
        error = "tail: cannot open " + path;
        return false;
    }
    const size_t limit = line_count == 0 ? 10 : line_count;
    std::vector<std::string> lines;
    char buf[256];
    std::string current;
    while (std::fgets(buf, sizeof(buf), file)) {
        current += buf;
        if (!current.empty() && current.back() == '\n') {
            current.pop_back();
            if (!current.empty() && current.back() == '\r') current.pop_back();
            lines.push_back(current);
            current.clear();
            if (lines.size() > 800) lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(lines.size() - 800));
        }
    }
    if (!current.empty()) lines.push_back(current);
    std::fclose(file);
    const size_t start = lines.size() > limit ? lines.size() - limit : 0;
    out.assign(lines.begin() + static_cast<std::ptrdiff_t>(start), lines.end());
    return true;
}

}  // namespace tabby
