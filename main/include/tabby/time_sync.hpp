#pragma once

#include "tabby/app_config.hpp"

#include <cstdint>
#include <mutex>
#include <string>

namespace tabby {

class TimeSync {
public:
    void configure(const SystemConfig& config);
    bool sync(uint32_t timeout_ms);
    bool detectTimezone(std::string& region, int16_t& utc_offset_minutes);
    bool synced() const;
    std::string formatted(const SystemConfig& config) const;
    std::string clockHm(const SystemConfig& config) const;

private:
    mutable std::mutex mutex_;
    bool initialized_{false};
    std::string ntp_server_{"pool.ntp.org"};
};

}  // namespace tabby
