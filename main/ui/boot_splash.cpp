#include "boot_splash.hpp"

#include "tabby/bsp.hpp"
#include "tabby/terminus_bitmap.hpp"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace tabby {
namespace {

constexpr uint32_t kMinSplashMs = 800;
constexpr uint32_t kBgRgb = 0x0B1220;
constexpr uint32_t kTextRgb = 0xE8EEF4;
constexpr uint32_t kMutedRgb = 0x8A97A8;
constexpr uint32_t kAccentRgb = 0x3D8BFF;
constexpr int kFillRows = 16;

uint16_t rgb565(uint32_t rgb) {
    const uint8_t r = static_cast<uint8_t>((rgb >> 16) & 0xFF);
    const uint8_t g = static_cast<uint8_t>((rgb >> 8) & 0xFF);
    const uint8_t b = static_cast<uint8_t>(rgb & 0xFF);
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

BoardBsp* g_bsp = nullptr;
std::mutex g_mutex;
char g_status[72] = "Starting...";
char g_painted[72] = "";
std::atomic<bool> g_finished{false};
std::atomic<bool> g_visible{false};
std::atomic<bool> g_direct{true};
int64_t g_shown_us{0};
int g_status_y{0};
int g_status_h{0};
lv_obj_t* g_overlay = nullptr;
lv_obj_t* g_status_label = nullptr;

int asciiWidth(const TerminusBitmap::Font& font, const char* text) {
    int width = 0;
    for (const char* p = text; p != nullptr && *p != '\0'; ++p) width += font.targetW;
    return width;
}

void drawAscii(uint16_t* pixels, int stride, int width, int height, const TerminusBitmap::Font& font,
               const char* text, int x, int y, uint16_t fg) {
    int cx = x;
    for (const char* p = text; p != nullptr && *p != '\0'; ++p) {
        TerminusBitmap::drawGlyph(pixels, stride, width, height, font, static_cast<uint8_t>(*p), cx, y, fg);
        cx += font.targetW;
    }
}

bool canDismissLocked(int64_t now_us) {
    if (!g_finished.load(std::memory_order_acquire)) return false;
    return (now_us - g_shown_us) >= static_cast<int64_t>(kMinSplashMs) * 1000;
}

void fillScreen(BoardBsp& bsp, uint16_t color) {
    const int width = bsp.displayWidth();
    const int height = bsp.displayHeight();
    if (width <= 0 || height <= 0) return;
    const size_t bytes = static_cast<size_t>(width) * kFillRows * sizeof(uint16_t);
    auto* band = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (band == nullptr) band = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
    if (band == nullptr) return;
    std::fill(band, band + static_cast<size_t>(width) * kFillRows, color);
    for (int y = 0; y < height; y += kFillRows) {
        const int rows = std::min(kFillRows, height - y);
        bsp.displayFlush(0, y, width, rows, band, y + rows >= height);
    }
    heap_caps_free(band);
}

void blitLine(BoardBsp& bsp, const TerminusBitmap::Font& font, const char* text, int y, uint16_t fg, uint16_t bg) {
    const int width = bsp.displayWidth();
    const int line_h = font.targetH;
    if (width <= 0 || line_h <= 0) return;
    auto* row = static_cast<uint16_t*>(
        heap_caps_malloc(static_cast<size_t>(width) * line_h * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (row == nullptr) {
        row = static_cast<uint16_t*>(
            heap_caps_malloc(static_cast<size_t>(width) * line_h * sizeof(uint16_t), MALLOC_CAP_8BIT));
    }
    if (row == nullptr) return;
    std::fill(row, row + static_cast<size_t>(width) * line_h, bg);
    const int text_w = asciiWidth(font, text);
    const int x = std::max(0, (width - text_w) / 2);
    drawAscii(row, width, width, line_h, font, text, x, 0, fg);
    bsp.displayFlush(0, y, width, line_h, row, true);
    heap_caps_free(row);
}

void blitBar(BoardBsp& bsp, int y, int bar_w, int bar_h, uint16_t color, uint16_t bg) {
    const int width = bsp.displayWidth();
    if (width <= 0 || bar_h <= 0) return;
    auto* row = static_cast<uint16_t*>(
        heap_caps_malloc(static_cast<size_t>(width) * bar_h * sizeof(uint16_t), MALLOC_CAP_8BIT));
    if (row == nullptr) return;
    std::fill(row, row + static_cast<size_t>(width) * bar_h, bg);
    const int x0 = std::max(0, (width - bar_w) / 2);
    const int x1 = std::min(width, x0 + bar_w);
    for (int yy = 0; yy < bar_h; ++yy) {
        uint16_t* line = row + yy * width;
        for (int x = x0; x < x1; ++x) line[x] = color;
    }
    bsp.displayFlush(0, y, width, bar_h, row, true);
    heap_caps_free(row);
}

void paintDirect(BoardBsp& bsp, bool full) {
    const uint16_t bg = rgb565(kBgRgb);
    const uint16_t fg = rgb565(kTextRgb);
    const uint16_t muted = rgb565(kMutedRgb);
    const uint16_t accent = rgb565(kAccentRgb);
    const int height = bsp.displayHeight();
    const auto& title_font = TerminusBitmap::fontForHeight(36);
    const auto& body_font = TerminusBitmap::fontForHeight(20);
    const int block_h = title_font.targetH + 12 + 3 + 16 + body_font.targetH + 20 + body_font.targetH;
    const int title_y = std::max(0, (height - block_h) / 2);
    const int bar_y = title_y + title_font.targetH + 12;
    const int sub_y = bar_y + 3 + 16;
    g_status_y = sub_y + body_font.targetH + 20;
    g_status_h = body_font.targetH;

    if (full) {
        fillScreen(bsp, bg);
        blitLine(bsp, title_font, "Tabby", title_y, fg, bg);
        blitBar(bsp, bar_y, 96, 3, accent, bg);
        blitLine(bsp, body_font, "SSH terminal", sub_y, muted, bg);
    }
    blitLine(bsp, body_font, g_status, g_status_y, muted, bg);
    std::snprintf(g_painted, sizeof(g_painted), "%s", g_status);
}

void paintDirectStatus(BoardBsp& bsp) {
    if (g_status_h <= 0) {
        paintDirect(bsp, true);
        return;
    }
    const auto& body_font = TerminusBitmap::fontForHeight(20);
    blitLine(bsp, body_font, g_status, g_status_y, rgb565(kMutedRgb), rgb565(kBgRgb));
    std::snprintf(g_painted, sizeof(g_painted), "%s", g_status);
}

void deleteOverlay() {
    if (g_overlay == nullptr) return;
    lv_obj_del(g_overlay);
    g_overlay = nullptr;
    g_status_label = nullptr;
    g_visible.store(false, std::memory_order_release);
}

}  // namespace

void BootSplashShow(BoardBsp& bsp) {
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_bsp = &bsp;
    g_shown_us = esp_timer_get_time();
    g_finished.store(false, std::memory_order_release);
    g_direct.store(true, std::memory_order_release);
    g_visible.store(true, std::memory_order_release);
    std::snprintf(g_status, sizeof(g_status), "%s", "Starting...");
    paintDirect(bsp, true);
}

void BootSplashSetStatus(const char* text) {
    if (text == nullptr || text[0] == '\0') return;
    const std::lock_guard<std::mutex> lock(g_mutex);
    std::snprintf(g_status, sizeof(g_status), "%s", text);
    if (g_direct.load(std::memory_order_relaxed) && g_bsp != nullptr) {
        if (std::strcmp(g_painted, g_status) != 0) paintDirectStatus(*g_bsp);
    }
}

void BootSplashFinish() { g_finished.store(true, std::memory_order_release); }

void BootSplashCreateUi() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_direct.store(false, std::memory_order_release);
    if (canDismissLocked(esp_timer_get_time())) {
        g_visible.store(false, std::memory_order_release);
        return;
    }

    g_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_overlay);
    lv_obj_set_size(g_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_overlay, lv_color_hex(kBgRgb), 0);
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(g_overlay);
    lv_label_set_text(title, "Tabby");
    lv_obj_set_style_text_color(title, lv_color_hex(kTextRgb), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -36);

    lv_obj_t* bar = lv_obj_create(g_overlay);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 96, 3);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kAccentRgb), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, -8);

    lv_obj_t* sub = lv_label_create(g_overlay);
    lv_label_set_text(sub, "SSH terminal");
    lv_obj_set_style_text_color(sub, lv_color_hex(kMutedRgb), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 12);

    g_status_label = lv_label_create(g_overlay);
    lv_label_set_text(g_status_label, g_status);
    lv_obj_set_style_text_color(g_status_label, lv_color_hex(kMutedRgb), 0);
    lv_obj_set_style_text_font(g_status_label, &lv_font_montserrat_16, 0);
    lv_obj_align(g_status_label, LV_ALIGN_CENTER, 0, 48);
    g_visible.store(true, std::memory_order_release);
}

bool BootSplashVisible() { return g_visible.load(std::memory_order_acquire); }

bool BootSplashPoll() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    if (g_overlay == nullptr) return false;
    if (g_status_label != nullptr) {
        const char* shown = lv_label_get_text(g_status_label);
        if (shown != nullptr && std::strcmp(shown, g_status) != 0) lv_label_set_text(g_status_label, g_status);
    }
    if (!canDismissLocked(esp_timer_get_time())) return false;
    deleteOverlay();
    return true;
}

}  // namespace tabby
