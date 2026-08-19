#pragma once

#include <cstdint>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace tabby {

class PythonGfx {
public:
    using AbortFn = std::function<bool()>;
    void begin(uint16_t* pixels, int width, int height, int stride);
    void setAbortPoll(AbortFn fn) { abort_fn_ = std::move(fn); }
    int width() const { return width_; }
    int height() const { return height_; }
    bool command(const char* command);
    bool active() const { return active_.load(std::memory_order_acquire); }
    void setActive(bool active) { active_.store(active, std::memory_order_release); }
    bool takePresentRequest() { return present_requested_.exchange(false, std::memory_order_acq_rel); }
    void lockFrame() { frame_mutex_.lock(); }
    void unlockFrame() { frame_mutex_.unlock(); }

private:
    void fillRect(int x, int y, int w, int h, uint16_t color);
    void drawPixel(int x, int y, uint16_t color);
    void drawLine(int x0, int y0, int x1, int y1, uint16_t color);
    void drawCircle(int x, int y, int r, uint16_t color, bool fill);
    void drawText(int x, int y, const std::string& text, uint16_t color);

    uint16_t* pixels_{nullptr};
    int width_{0};
    int height_{0};
    int stride_{0};
    std::atomic<bool> active_{false};
    std::atomic<bool> present_requested_{false};
    std::mutex frame_mutex_;
    AbortFn abort_fn_;
};

PythonGfx& pythonGfx();

}  // namespace tabby

extern "C" int tab5_python_gfx_width();
extern "C" int tab5_python_gfx_height();
extern "C" bool tab5_python_gfx_command(const char* command);
