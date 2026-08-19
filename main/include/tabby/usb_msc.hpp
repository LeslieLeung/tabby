#pragma once

#include <atomic>
#include <mutex>
#include <string>

namespace tabby {

class KeyboardInput;
class SdCard;

class UsbMsc {
public:
    void attach(SdCard& sd, KeyboardInput& keyboard);
    bool start();
    bool stop();
    bool active() const { return active_.load(std::memory_order_acquire); }
    bool hostAttached() const { return host_attached_.load(std::memory_order_acquire); }
    const std::string& lastError() const { return last_error_; }
    void noteHostAttached(bool attached);

private:
    bool startTinyusb();
    void stopTinyusb();

    SdCard* sd_{nullptr};
    std::mutex mutex_;
    std::atomic<bool> active_{false};
    std::atomic<bool> host_attached_{false};
    void* storage_{nullptr};
    std::string last_error_;
};

}  // namespace tabby
