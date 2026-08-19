#pragma once

#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#include <atomic>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace tabby {

struct SdCardInfo {
    bool mounted{false};
    bool busy{false};
    bool usb_drive{false};
    std::string card_name;
    std::string type;
    uint64_t card_bytes{0};
    uint64_t total_bytes{0};
    uint64_t free_bytes{0};
    std::string error;
};

class SdCard {
public:
    bool begin();
    bool mount();
    bool unmount();
    bool format();
    bool prepareForUsb();
    bool restoreFromUsb();
    sdmmc_card_t* rawCard();
    bool ready() const { return ready_.load(std::memory_order_acquire); }
    bool busy() const { return busy_.load(std::memory_order_acquire); }
    bool usbMode() const { return usb_mode_.load(std::memory_order_acquire); }
    SdCardInfo info();
    const std::string& lastError() const { return last_error_; }
    const std::string& cwd() const { return cwd_; }
    std::string virtualPath(const std::string& input) const;
    std::string fsPath(const std::string& virtual_path) const;

    std::string statusText();
    std::string dfText();
    bool cd(const std::string& input, std::string& message);
    bool list(const std::string& input, std::vector<std::string>& lines, std::string& error);
    bool cat(const std::string& input, std::vector<std::string>& lines, std::string& error);
    bool readFile(const std::string& input, std::string& text, std::string& error, size_t max_bytes,
                  bool* missing = nullptr);
    bool writeFile(const std::string& input, const std::string& text, std::string& error);
    bool writeText(const std::string& input_path, const std::string& text, bool append, std::string& message);
    bool mkdir(const std::string& input, std::string& message);
    bool rmdir(const std::string& input, std::string& message);
    bool removeFile(const std::string& input, std::string& message);
    bool chmod(const std::string& mode_text, const std::string& input, std::string& message);
    bool copyFile(const std::string& src_input, const std::string& dst_input, std::string& message);
    bool moveFile(const std::string& src_input, const std::string& dst_input, std::string& message);
    bool touch(const std::string& input, std::string& message);
    bool head(const std::string& input, size_t lines, std::vector<std::string>& out, std::string& error);
    bool tail(const std::string& input, size_t lines, std::vector<std::string>& out, std::string& error);
    bool exists(const std::string& virtual_path, bool* directory = nullptr);
    bool hasRead(const std::string& virtual_path);
    bool hasWrite(const std::string& virtual_path);
    bool hasExecute(const std::string& virtual_path);

private:
    struct ModeEntry {
        std::string path;
        uint16_t mode{0644};
    };
    struct LsOptions {
        bool long_format{false};
        bool all{false};
        bool human{false};
        std::string path;
    };

    bool ensureReady();
    bool ensurePower();
    void fillHostAndSlot(sdmmc_host_t& host, sdmmc_slot_config_t& slot);
    int mountLocked(bool format_if_mount_failed);
    bool unmountLocked();
    int initRawCardLocked();
    void deinitRawCardLocked();
    void fillInfoFromCard(SdCardInfo& result);
    void resetMountedState();
    void loadModes();
    void saveModes();
    uint16_t modeFor(const std::string& virtual_path, bool directory);
    void setMode(const std::string& virtual_path, uint16_t mode);
    bool parseLsOptions(const std::string& input, LsOptions& options, std::string& error) const;
    bool resolveCopyDest(const std::string& src, const std::string& dst_input, std::string& dest, std::string& message);
    static std::string formatBytes(uint64_t bytes);
    static std::string formatTime(time_t t);
    static std::string modeString(uint16_t mode, bool directory);
    static std::string basename(const std::string& path);
    static bool parseOctalMode(const std::string& text, uint16_t& mode);

    std::atomic<bool> ready_{false};
    std::atomic<bool> busy_{false};
    std::atomic<bool> usb_mode_{false};
    bool raw_card_owned_{false};
    bool modes_loaded_{false};
    std::string last_error_;
    std::string cwd_{"/"};
    std::vector<ModeEntry> modes_;
};

}  // namespace tabby
