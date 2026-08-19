#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tabby {

struct WifiProfile {
    std::string name;
    std::string ssid;
    std::string password;
};

struct SshProfile {
    std::string name;
    std::string host;
    uint16_t port{22};
    std::string user;
    std::string password;
    std::string terminal{"xterm-256color"};
};

struct KeyboardConfig {
    std::string layout{"us"};
    std::string terminalFont{"mono28"};
    uint8_t terminalLineStep{28};
    bool swapCtrlCaps{false};
};

struct SystemConfig {
    std::string deviceName{"tabby"};
    std::string region{"UTC"};
    int16_t utcOffsetMinutes{0};
    bool timezoneAuto{false};
    std::string ntpServer{"pool.ntp.org"};
};

struct DisplayConfig {
    static constexpr uint8_t kMinBrightness = 16;
    static constexpr uint8_t kMaxBrightness = 255;
    static constexpr uint8_t kDefaultBrightness = 128;
    uint8_t brightness{kDefaultBrightness};
};

struct AppConfig {
    std::vector<WifiProfile> wifi;
    std::vector<SshProfile> ssh;
    KeyboardConfig keyboard;
    SystemConfig system;
    DisplayConfig display;
    size_t activeWifi{0};
    size_t activeSsh{0};
};

}  // namespace tabby
