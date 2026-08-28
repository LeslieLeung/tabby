#pragma once

#include "tabby/app_config.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace tabby {

class SshClient {
public:
    bool begin();
    bool connect(const SshProfile& profile, std::string& error, int columns = 0, int rows = 0);
    void disconnect();
    bool connected() const;
    int read(char* buffer, size_t len);
    bool write(const uint8_t* data, size_t len);
    bool resizePty(int columns, int rows);
    void setPtyHint(int columns, int rows);
    int ptyColumns() const { return pty_cols_.load(std::memory_order_acquire); }
    int ptyRows() const { return pty_rows_.load(std::memory_order_acquire); }

private:
    bool writePending(const uint8_t* data, size_t len, uint32_t generation);
    static void ioEntry(void* arg);
    void ioLoop();
    void clearRings();
    size_t txPush(const uint8_t* data, size_t len);
    size_t txPop(uint8_t* data, size_t len);
    size_t rxPush(const uint8_t* data, size_t len);
    size_t rxPop(uint8_t* data, size_t len);

    mutable std::mutex session_mutex_;
    std::mutex connect_mutex_;
    std::mutex ring_mutex_;
    std::atomic<uint32_t> generation_{0};
    std::atomic<bool> live_{false};
    std::atomic<bool> remote_eof_{false};
    void* session_{nullptr};
    void* channel_{nullptr};
    void* io_task_{nullptr};
    std::vector<uint8_t> rx_;
    std::vector<uint8_t> tx_;
    size_t rx_r_{0};
    size_t rx_w_{0};
    size_t tx_r_{0};
    size_t tx_w_{0};
    std::atomic<int> pty_cols_{80};
    std::atomic<int> pty_rows_{24};
};

}  // namespace tabby
