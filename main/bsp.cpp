#include "tabby/bsp.hpp"

#include <M5Unified.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"

#if SOC_PPA_SUPPORTED
#include "driver/ppa.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lgfx/v1/platforms/esp32p4/Panel_DSI.hpp"
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace tabby {
namespace {
constexpr char kTag[] = "tabby_bsp";
constexpr size_t kRgb565Bytes = sizeof(uint16_t);
constexpr size_t kPpaAlignment = 64;

// The board drives a single DSI panel, so the completion hook lives at file
// scope instead of being threaded through the PPA interrupt's user data.
BoardBsp::FlushDoneHandler g_flush_done_handler = nullptr;
void* g_flush_done_user = nullptr;

#if SOC_PPA_SUPPORTED
// Synchronous transfers share the client and reach the same completion
// callback. Reporting one of those to LVGL would release a draw buffer the
// accelerator is still reading, so the tags route each completion to the
// waiter that actually issued the transfer.
constexpr uintptr_t kAsyncFlushTag = 0xA5F1u;
constexpr uintptr_t kSyncFlushTag = 0xA5F2u;

// Wakes blitPpa's bounded wait for a synchronous transfer. The driver's own
// blocking mode waits forever, which turns a stalled engine into a frozen UI
// task, so synchronous flushes are submitted non-blocking instead.
SemaphoreHandle_t g_ppa_sync_done = nullptr;
// Given when the last in-flight transfer completes, so a CPU blit can wait
// without polling the engine.
SemaphoreHandle_t g_ppa_idle = nullptr;
std::atomic<int> g_ppa_inflight{0};

void notePpaSubmitted() { g_ppa_inflight.fetch_add(1, std::memory_order_relaxed); }

void notePpaSubmitFailed() {
    if (g_ppa_inflight.fetch_sub(1, std::memory_order_acq_rel) == 1 && g_ppa_idle != nullptr) {
        xSemaphoreGive(g_ppa_idle);
    }
}

bool onPpaTransDone(ppa_client_handle_t, ppa_event_data_t*, void* user) {
    BaseType_t woken = pdFALSE;
    if (g_ppa_inflight.fetch_sub(1, std::memory_order_acq_rel) == 1 && g_ppa_idle != nullptr) {
        xSemaphoreGiveFromISR(g_ppa_idle, &woken);
    }
    const uintptr_t tag = reinterpret_cast<uintptr_t>(user);
    if (tag == kSyncFlushTag) {
        if (g_ppa_sync_done != nullptr) xSemaphoreGiveFromISR(g_ppa_sync_done, &woken);
        return woken == pdTRUE;
    }
    if (tag != kAsyncFlushTag) return woken == pdTRUE;
    return (g_flush_done_handler != nullptr && g_flush_done_handler(g_flush_done_user)) || woken == pdTRUE;
}

// Silicon defect DIG-734: when the SRM engine splits an operation across
// transfer units and the trailing unit carries fewer pixels than the 2D-DMA
// FIFO, the engine stalls with no completion interrupt and no way to abort, so
// the transaction descriptor is lost for good. ESP-IDF (v5.4.4 included)
// programs the bypass_mb_order workaround, but its trigger condition derives
// the trailing unit from the input block geometry and ignores rotation_angle.
// A 90-degree rotation writes the output in swapped orientation, and stalls
// were still observed on areas the driver's formula accepts, so both
// orientations are screened here before an area goes near the accelerator.
constexpr int kPpaWidthUnit = 64;   // 32 only for RGB888/ARGB8888 output
constexpr int kPpaRowUnit = 16;     // 32 from chip revision 3.0 onwards
constexpr uint32_t kPpaFifoBits = 12 * 128;
constexpr uint32_t kPpaOutDepthBits = 16;  // RGB565 output
// How long a foreground flush waits for the completion interrupt before it
// declares the engine stalled. Even a full-frame rotate finishes in a few
// milliseconds, so anything near this limit is already dead.
constexpr uint32_t kPpaSyncTimeoutMs = 100;

bool ppaTrailingUnitSafe(uint32_t width, uint32_t height) {
    // A block small enough to stay within a single transfer unit never splits.
    if (width <= kPpaWidthUnit && height <= kPpaRowUnit) return true;
    const uint32_t w_left = (width % kPpaWidthUnit) == 0 ? kPpaWidthUnit : width % kPpaWidthUnit;
    const uint32_t h_left = (height % kPpaRowUnit) == 0 ? kPpaRowUnit : height % kPpaRowUnit;
    return w_left * h_left * kPpaOutDepthBits >= kPpaFifoBits;
}

bool ppaBlockSafe(int width, int height) {
    const uint32_t w = static_cast<uint32_t>(width);
    const uint32_t h = static_cast<uint32_t>(height);
    return ppaTrailingUnitSafe(w, h) && ppaTrailingUnitSafe(h, w);
}

// Rows the accelerator will take from the top of the area. Trimming to a whole
// number of macro blocks maximises h_left, which is what rescues heights that
// leave an awkward remainder.
int ppaSafeRows(int width, int height) {
    if (ppaBlockSafe(width, height)) return height;
    const int whole = (height / kPpaRowUnit) * kPpaRowUnit;
    if (whole > 0 && ppaBlockSafe(width, whole)) return whole;
    return 0;
}
#endif
}  // namespace

bool BoardBsp::initialize() {
    auto config = M5.config();
    config.internal_imu = false;
    config.internal_rtc = true;
    config.internal_mic = false;
    config.internal_spk = false;
    config.clear_display = true;
    config.output_power = true;
    M5.begin(config);

    if (M5.getBoard() != m5::board_t::board_M5Tab5) {
        ESP_LOGW(kTag, "Board is not M5Tab5 (detected=%d); continuing anyway", static_cast<int>(M5.getBoard()));
    }

    // Native panel is 720x1280 portrait. Rotation 3 is landscape (1280x720),
    // matching the original Tab5 SSH client default.
    M5.Display.setRotation(3);

    // LVGL 16-bit draw buffers hold native RGB565. M5GFX's uint16_t pushImage
    // otherwise treats the source as byte-swapped RGB565.
    M5.Display.setSwapBytes(true);
    M5.Display.setBrightness(128);
    // A framebuffer panel normally writes its cache back on every endWrite().
    // LVGL can flush several areas per refresh, so defer that work until the
    // final area. The PPA path writes the native framebuffer directly.
    M5.Display.setAutoDisplay(false);
    ready_ = true;
    ppa_ready_ = initializePpa();
    if (M5.Rtc.isEnabled()) {
        m5::rtc_datetime_t dt;
        if (M5.Rtc.getDateTime(&dt)) {
            ESP_LOGI(kTag, "RTC UTC %04d-%02d-%02d %02d:%02d:%02d%s", dt.date.year, dt.date.month, dt.date.date,
                     dt.time.hours, dt.time.minutes, dt.time.seconds, M5.Rtc.getVoltLow() ? " (voltage low)" : "");
        } else {
            ESP_LOGW(kTag, "RTC enabled, but datetime is invalid until NTP sync");
        }
    } else {
        ESP_LOGW(kTag, "Internal RTC was not found");
    }
    ESP_LOGI(kTag, "Display %dx%d, PPA rotation %s", displayWidth(), displayHeight(),
             ppa_ready_ ? (ppa_async_ ? "enabled (async)" : "enabled (blocking)") : "disabled");
    return true;
}

void BoardBsp::update() {
    if (!ready_) return;
    M5.update();
    const int64_t now = esp_timer_get_time();
    if (now - batt_poll_us_ < 100000) return;
    batt_poll_us_ = now;
    sampleBattery();
}

int BoardBsp::displayWidth() const { return M5.Display.width(); }
int BoardBsp::displayHeight() const { return M5.Display.height(); }

void BoardBsp::setBrightness(uint8_t brightness) {
    if (ready_) M5.Display.setBrightness(brightness);
}

int BoardBsp::batteryPercent() const {
    const int level = M5.Power.getBatteryLevel();
    return level < 0 ? 0 : (level > 100 ? 100 : level);
}

bool BoardBsp::batteryCharging() const {
    // CHG_STAT blips while the IP2326 retries on an empty connector, so the
    // shunt current is the only reliable "charging a pack" signal.
    return batt_present_ && batteryCurrentMa() >= 15;
}

int BoardBsp::batteryVoltageMv() const { return M5.Power.getBatteryVoltage(); }

int BoardBsp::batteryCurrentMa() const { return M5.Power.getBatteryCurrent(); }

bool BoardBsp::batteryPresent() const { return batt_present_; }

void BoardBsp::sampleBattery() {
    const int current_ma = M5.Power.getBatteryCurrent();
    if (current_ma >= 25 || current_ma <= -25) {
        batt_present_ = true;
        batt_stable_windows_ = 2;
        batt_window_us_ = 0;
        return;
    }

    const bool chg_stat = M5.Power.isCharging() == m5::Power_Class::is_charging;
    // STAT high with no pack current is the charger hunting on USB-only power.
    if (chg_stat && current_ma > -10 && current_ma < 15) {
        batt_present_ = false;
        batt_stable_windows_ = 0;
        batt_window_us_ = 0;
        return;
    }

    const int voltage_mv = M5.Power.getBatteryVoltage();
    if (voltage_mv < 6000) {
        batt_present_ = false;
        batt_stable_windows_ = 0;
        batt_window_us_ = 0;
        return;
    }

    if (batt_window_us_ == 0) {
        batt_window_us_ = batt_poll_us_;
        batt_min_mv_ = voltage_mv;
        batt_max_mv_ = voltage_mv;
        return;
    }
    if (voltage_mv < batt_min_mv_) batt_min_mv_ = voltage_mv;
    if (voltage_mv > batt_max_mv_) batt_max_mv_ = voltage_mv;
    if (batt_poll_us_ - batt_window_us_ < 1000000) return;

    const int swing = batt_max_mv_ - batt_min_mv_;
    batt_window_us_ = batt_poll_us_;
    batt_min_mv_ = voltage_mv;
    batt_max_mv_ = voltage_mv;
    // An installed pack holds the rail; an empty charge path swings volts.
    if (swing >= 400) {
        batt_present_ = false;
        batt_stable_windows_ = 0;
        return;
    }
    if (batt_stable_windows_ < 2) ++batt_stable_windows_;
    if (batt_stable_windows_ >= 2) batt_present_ = true;
}

void BoardBsp::setFlushDoneHandler(FlushDoneHandler handler, void* user) {
    g_flush_done_handler = handler;
    g_flush_done_user = user;
}

bool BoardBsp::displayFlush(int x, int y, int width, int height, const uint16_t* pixels, bool last) {
    if (!ready_ || pixels == nullptr || width <= 0 || height <= 0) return false;
    if (ppa_ready_ && blitArea(x, y, width, height, pixels, false) != BlitResult::Failed) return true;
    blitCpu(x, y, width, height, pixels, width, 0, 0, last);
    return true;
}

bool BoardBsp::displayFlushAsync(int x, int y, int width, int height, const uint16_t* pixels, bool last) {
    if (!ready_ || pixels == nullptr || width <= 0 || height <= 0) return false;
    // Without a handler nothing would ever observe the completion.
    if (ppa_async_ && g_flush_done_handler != nullptr) {
        const BlitResult result = blitArea(x, y, width, height, pixels, true);
        if (result == BlitResult::InFlight) return true;
        if (result == BlitResult::Done) return false;
    }
    // One PPA attempt per flush: whatever made the accelerator reject this
    // area (unsafe geometry, exhausted transaction pool) rejects a retry the
    // same way, and every rejected submit logs a driver error line over UART,
    // which costs far more time than the blit itself.
    blitCpu(x, y, width, height, pixels, width, 0, 0, last);
    return false;
}

BoardBsp::BlitResult BoardBsp::blitArea(int x, int y, int width, int height, const uint16_t* pixels, bool async) {
#if SOC_PPA_SUPPORTED
    if (x < 0 || y < 0 || x + width > logical_width_ || y + height > logical_height_) {
        ESP_LOGE(kTag, "Invalid display flush area (%d,%d %dx%d)", x, y, width, height);
        return BlitResult::Failed;
    }

    // A width that leaves only a few pixels past a multiple of kPpaWidthUnit can
    // never clear the geometry rule, no matter how the rows are cut, because
    // w_left stays tiny. Peeling that remainder off as its own column restores
    // a full-width unit for everything else.
    int block_w = width;
    int sliver_w = 0;
    const int width_rem = width % kPpaWidthUnit;
    if (width_rem != 0 && !ppaBlockSafe(width_rem, kPpaRowUnit) && width > kPpaWidthUnit) {
        block_w = width - width_rem;
        sliver_w = width_rem;
    }

    const int block_h = ppaSafeRows(block_w, height);
    if (block_h <= 0) return BlitResult::Failed;
    const int rem_h = height - block_h;
    const bool whole = sliver_w == 0 && rem_h == 0;
    // A mixed blit has to wait for this PPA transfer: the driver's output
    // invalidate covers a full-width native window, and the CPU cache
    // writeback expands to 128-byte lines that overlap neighbouring pixels
    // the accelerator is still DMA-writing. Doing the CPU part first used to
    // stomp those pixels with the previous frame, which showed up as
    // leftover ghosting on large updates.
    const bool use_async = async && whole;
    if (!blitPpa(x, y, block_w, block_h, pixels, width, use_async)) return BlitResult::Failed;
    if (!whole) {
        if (sliver_w > 0) blitCpu(x + block_w, y, sliver_w, height, pixels, width, block_w, 0, false);
        if (rem_h > 0) blitCpu(x, y + block_h, block_w, rem_h, pixels, width, 0, block_h, false);
        return BlitResult::Done;
    }
    return use_async ? BlitResult::InFlight : BlitResult::Done;
#else
    (void)x, (void)y, (void)width, (void)height, (void)pixels, (void)async;
    return BlitResult::Failed;
#endif
}

bool BoardBsp::blitPpa(int x, int y, int width, int height, const uint16_t* pixels, int stride, bool async) {
#if SOC_PPA_SUPPORTED
    // The source is the caller's buffer rather than a full-frame shadow: PPA
    // accepts an unaligned input anywhere in memory and writes back its cache
    // window itself, so only the pixels LVGL actually redrew get moved.
    ppa_srm_oper_config_t config{};
    config.in.buffer = pixels;
    config.in.pic_w = static_cast<uint32_t>(stride);
    config.in.pic_h = static_cast<uint32_t>(height);
    config.in.block_w = static_cast<uint32_t>(width);
    config.in.block_h = static_cast<uint32_t>(height);
    config.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

    config.out.buffer = native_framebuffer_;
    config.out.buffer_size = native_bytes_;
    config.out.pic_w = static_cast<uint32_t>(native_width_);
    config.out.pic_h = static_cast<uint32_t>(native_height_);
    // Rotation 3 in M5GFX maps logical (x,y) to native
    // (y, logical_width - 1 - x), i.e. 90 degrees counter-clockwise.
    config.out.block_offset_x = static_cast<uint32_t>(y);
    config.out.block_offset_y = static_cast<uint32_t>(logical_width_ - (x + width));
    config.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    config.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;
    config.scale_x = 1.0f;
    config.scale_y = 1.0f;
    // Always non-blocking: the driver's blocking mode waits on the completion
    // forever, and a stalled transfer never completes (the driver cannot abort
    // it), so a synchronous caller waits below with a deadline instead.
    config.mode = PPA_TRANS_MODE_NON_BLOCKING;
    config.user_data = reinterpret_cast<void*>(async ? kAsyncFlushTag : kSyncFlushTag);

    if (!async) xSemaphoreTake(g_ppa_sync_done, 0);  // drop any stale completion token
    notePpaSubmitted();
    const esp_err_t err = ppa_do_scale_rotate_mirror(static_cast<ppa_client_handle_t>(ppa_client_), &config);
    if (err == ESP_OK) {
        if (async) return true;
        if (xSemaphoreTake(g_ppa_sync_done, pdMS_TO_TICKS(kPpaSyncTimeoutMs)) == pdTRUE) return true;
        // The transaction slot this transfer occupied is lost for good, and
        // the engine will not start anything queued behind it. The caller
        // repaints the area with the CPU, which also covers the case where
        // the transfer limps in later: it would rewrite identical pixels.
        ESP_LOGE(kTag, "PPA transfer stalled; disabling acceleration");
        disablePpa();
        return false;
    }
    notePpaSubmitFailed();

    // A full transaction pool reports ESP_FAIL; with a stalled engine that
    // state is permanent, so waitForFlush's timeout handling decides when to
    // give up on the accelerator. Only a rejected configuration takes it out
    // of service here.
    if (err == ESP_FAIL) {
        ESP_LOGD(kTag, "PPA busy; falling back to CPU for this area");
        return false;
    }
    ESP_LOGE(kTag, "PPA display rotation failed: %s; using CPU fallback", esp_err_to_name(err));
    disablePpa();
    return false;
#else
    (void)x, (void)y, (void)width, (void)height, (void)pixels, (void)stride, (void)async;
    return false;
#endif
}

void BoardBsp::blitCpu(int x, int y, int width, int height, const uint16_t* pixels, int stride, int offset_x,
                       int offset_y, bool last) {
    waitForPpaIdle();
    M5.Display.startWrite();
    if (stride == width && offset_x == 0 && offset_y == 0) {
        M5.Display.pushImage(x, y, width, height, pixels);
    } else {
        // A sliver is not contiguous, so it goes one row at a time.
        for (int row = 0; row < height; ++row) {
            const uint16_t* src = pixels + static_cast<size_t>(offset_y + row) * stride + offset_x;
            M5.Display.pushImage(x, y + row, width, 1, src);
        }
    }
    M5.Display.endWrite();
    // A PPA transfer invalidates a tall window of the framebuffer before it
    // writes, which would discard these pixels while they still only live in
    // the cache. Mixing both paths in one frame therefore has to push here.
    if (last || ppa_ready_) M5.Display.display();
}

void BoardBsp::waitForPpaIdle() {
#if SOC_PPA_SUPPORTED
    if (!ppa_ready_ || g_ppa_inflight.load(std::memory_order_acquire) <= 0) return;
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(kPpaSyncTimeoutMs);
    while (g_ppa_inflight.load(std::memory_order_acquire) > 0) {
        const TickType_t elapsed = xTaskGetTickCount() - start;
        if (!ppa_ready_ || elapsed >= timeout) {
            ESP_LOGE(kTag, "PPA still busy before CPU blit; disabling acceleration");
            disablePpa();
            return;
        }
        if (g_ppa_idle != nullptr) xSemaphoreTake(g_ppa_idle, timeout - elapsed);
    }
#endif
}

void BoardBsp::disablePpa() {
    // The client is deliberately left registered: unregistering it while an
    // asynchronous transfer is still queued would either fail or drop the
    // completion the caller is waiting on.
    ppa_ready_ = false;
    ppa_async_ = false;
}

bool BoardBsp::initializePpa() {
#if SOC_PPA_SUPPORTED
    if (M5.getBoard() != m5::board_t::board_M5Tab5) return false;

    logical_width_ = displayWidth();
    logical_height_ = displayHeight();

    auto* panel = M5.Display.getPanel();
    if (panel == nullptr) {
        ESP_LOGW(kTag, "Display panel is unavailable; PPA disabled");
        return false;
    }
    const auto& panel_config = panel->config();
    native_width_ = panel_config.panel_width;
    native_height_ = panel_config.panel_height;
    if (logical_width_ != native_height_ || logical_height_ != native_width_) {
        ESP_LOGW(kTag, "PPA rotation requires 90-degree geometry, logical=%dx%d native=%dx%d", logical_width_,
                 logical_height_, native_width_, native_height_);
        return false;
    }

    // Every M5Tab5 display panel derives from Panel_DSI. Its common detail
    // exposes the framebuffer while keeping the esp_lcd handle encapsulated.
    auto* dsi_panel = static_cast<lgfx::Panel_DSI*>(panel);
    native_framebuffer_ = static_cast<uint16_t*>(dsi_panel->config_detail().buffer);
    const size_t native_bytes = static_cast<size_t>(native_width_) * native_height_ * kRgb565Bytes;
    if (native_framebuffer_ == nullptr ||
        (reinterpret_cast<uintptr_t>(native_framebuffer_) & (kPpaAlignment - 1)) != 0 ||
        (native_bytes & (kPpaAlignment - 1)) != 0) {
        ESP_LOGW(kTag, "DSI framebuffer is not PPA aligned");
        native_framebuffer_ = nullptr;
        return false;
    }
    native_bytes_ = static_cast<uint32_t>(native_bytes);

    if (g_ppa_sync_done == nullptr) g_ppa_sync_done = xSemaphoreCreateBinary();
    if (g_ppa_idle == nullptr) g_ppa_idle = xSemaphoreCreateBinary();
    if (g_ppa_sync_done == nullptr || g_ppa_idle == nullptr) {
        ESP_LOGW(kTag, "PPA sync semaphore alloc failed; PPA disabled");
        return false;
    }

    ppa_client_config_t client_config{};
    client_config.oper_type = PPA_OPERATION_SRM;
    // One transfer can still be in flight while LVGL renders into its other
    // draw buffer, and a synchronous flush may queue behind it.
    client_config.max_pending_trans_num = 2;
    ppa_client_handle_t client = nullptr;
    const esp_err_t err = ppa_register_client(&client_config, &client);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "PPA client registration failed: %s", esp_err_to_name(err));
        return false;
    }

    // Both the asynchronous and the synchronous paths observe completion
    // through this callback, so without it the accelerator is unusable.
    ppa_event_callbacks_t callbacks{};
    callbacks.on_trans_done = onPpaTransDone;
    const esp_err_t cb_err = ppa_client_register_event_callbacks(client, &callbacks);
    if (cb_err != ESP_OK) {
        ESP_LOGW(kTag, "PPA completion callback unavailable: %s; PPA disabled", esp_err_to_name(cb_err));
        ppa_unregister_client(client);
        return false;
    }
    ppa_client_ = client;
    ppa_async_ = true;
    return true;
#else
    return false;
#endif
}

TouchPoint BoardBsp::touch() const {
    TouchPoint point;
    if (M5.Touch.getCount() == 0) return point;
    const auto& detail = M5.Touch.getDetail(0);
    point.pressed = true;
    point.x = detail.x;
    point.y = detail.y;
    return point;
}

const char* BoardBsp::boardName() const {
    return M5.getBoard() == m5::board_t::board_M5Tab5 ? "M5Tab5" : "unknown";
}

}  // namespace tabby
