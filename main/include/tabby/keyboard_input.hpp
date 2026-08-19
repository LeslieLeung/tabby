#pragma once

#include "tabby/keyboard_mapper.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace tabby {

class KeyboardInput {
public:
    void configure(const KeyboardConfig& config);
    bool begin();
    void update();
    bool available() const;
    KeyAction read();
    std::string status() const;
    KeyboardMapper& mapper() { return mapper_; }
    void push(const KeyAction& action);
    void noteUsbConnected();
    void noteUsbDisconnected();
    bool pauseUsbHost();
    bool resumeUsbHost();

private:
    static constexpr size_t kQueueSize = 32;
    bool readI2c(uint8_t reg, uint8_t* data, size_t len);
    bool writeI2c(uint8_t reg, uint8_t value);
    bool ensureI2cBus();
    bool tryAttachTab5();
    void markTab5Disconnected();
    void pollTab5Keyboard();
    void beginUsbHost();
    bool stopUsbHostLocked();
    void closeUsbDevices();
    void pollUsbHost();
    void refreshStatus();
    bool startPollTask();

    KeyboardMapper mapper_;
    KeyAction queue_[kQueueSize]{};
    size_t head_{0};
    size_t tail_{0};
    std::atomic<uint32_t> events_{0};
    uint32_t last_probe_ms_{0};
    uint8_t i2c_fail_count_{0};
    mutable std::mutex status_mutex_;
    std::string status_{"not initialized"};
    std::atomic<bool> ready_{false};
    bool usb_started_{false};
    bool poll_task_started_{false};
};

}  // namespace tabby
