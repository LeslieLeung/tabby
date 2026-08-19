#pragma once

#include <cstdint>

namespace tabby {

struct TouchPoint {
    bool pressed{false};
    int16_t x{0};
    int16_t y{0};
};

class BoardBsp {
public:
    // Runs in the PPA interrupt once a flush has finished reading its source
    // pixels. Returns true when it woke a higher priority task.
    using FlushDoneHandler = bool (*)(void* user);

    bool initialize();
    void update();
    int displayWidth() const;
    int displayHeight() const;
    int batteryPercent() const;
    bool batteryCharging() const;
    int batteryVoltageMv() const;
    int batteryCurrentMa() const;
    bool batteryPresent() const;
    // Returns once the pixels have been handed to the panel.
    bool displayFlush(int x, int y, int width, int height, const uint16_t* pixels, bool last = true);
    // Returns true when the flush is still in flight and the registered handler
    // will report its completion. A false return means it already finished.
    bool displayFlushAsync(int x, int y, int width, int height, const uint16_t* pixels, bool last);
    void setFlushDoneHandler(FlushDoneHandler handler, void* user);
    bool asyncFlushSupported() const { return ppa_async_; }
    // Drops back to CPU blits for good. The accelerator cannot recover from a
    // stalled transfer on its own, so a caller that stops seeing completions
    // has to take it out of service.
    void disableAcceleration() { disablePpa(); }
    void setBrightness(uint8_t brightness);
    TouchPoint touch() const;
    const char* boardName() const;

private:
    // Failed: caller should CPU-blit the whole area.
    // Done: pixels are already in the framebuffer.
    // InFlight: the registered handler will report completion.
    enum class BlitResult : uint8_t { Failed, Done, InFlight };

    bool initializePpa();
    // Splits the flushed area into the largest block the accelerator will
    // accept, leaving at most a narrow sliver for the CPU.
    BlitResult blitArea(int x, int y, int width, int height, const uint16_t* pixels, bool async);
    bool blitPpa(int x, int y, int width, int height, const uint16_t* pixels, int stride, bool async);
    // `stride` is the row pitch of the source area, which is wider than `width`
    // whenever this is painting a sliver left over from the split.
    void blitCpu(int x, int y, int width, int height, const uint16_t* pixels, int stride, int offset_x,
                 int offset_y, bool last);
    // CPU cache writebacks expand to whole cache lines, so they must not run
    // while the accelerator is still DMA-writing neighbouring pixels.
    void waitForPpaIdle();
    void disablePpa();
    void sampleBattery();

    bool ready_{false};
    bool batt_present_{false};
    uint8_t batt_stable_windows_{0};
    int batt_min_mv_{0};
    int batt_max_mv_{0};
    int64_t batt_poll_us_{0};
    int64_t batt_window_us_{0};
    bool ppa_ready_{false};
    bool ppa_async_{false};
    void* ppa_client_{nullptr};
    uint16_t* native_framebuffer_{nullptr};
    uint32_t native_bytes_{0};
    int logical_width_{0};
    int logical_height_{0};
    int native_width_{0};
    int native_height_{0};
};

}  // namespace tabby
