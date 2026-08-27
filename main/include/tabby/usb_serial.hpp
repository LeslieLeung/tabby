#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace tabby {

class UsbSerial {
public:
    bool begin();
    bool connect(uint32_t baud, uint32_t timeout_ms, std::string& error, const std::atomic<bool>* abort = nullptr);
    void disconnect();
    bool connected() const;
    int read(char* buffer, size_t len);
    bool write(const uint8_t* data, size_t len);
    uint32_t baud() const { return baud_.load(std::memory_order_acquire); }
    std::string status() const;
    const std::string& lastError() const { return last_error_; }
    // USB host callbacks; not for application code.
    void ingest(const uint8_t* data, size_t len);
    void noteDisconnected();

private:
    static void txEntry(void* arg);
    void txLoop();
    void closeDevice();
    void clearRings();
    size_t txPush(const uint8_t* data, size_t len);
    size_t txPop(uint8_t* data, size_t len);
    size_t rxPush(const uint8_t* data, size_t len);
    size_t rxPop(uint8_t* data, size_t len);
    void* tryOpen(std::string& identity, std::string& error);
    bool configureLine(uint32_t baud);

    mutable std::mutex mutex_;
    std::mutex connect_mutex_;
    std::mutex ring_mutex_;
    void* device_{nullptr};
    void* tx_task_{nullptr};
    std::atomic<bool> live_{false};
    std::atomic<bool> remote_eof_{false};
    std::atomic<uint32_t> baud_{115200};
    std::vector<uint8_t> rx_;
    std::vector<uint8_t> tx_;
    size_t rx_r_{0};
    size_t rx_w_{0};
    size_t tx_r_{0};
    size_t tx_w_{0};
    std::string identity_;
    std::string last_error_;
};

}  // namespace tabby
