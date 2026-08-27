#include "tabby/app.hpp"
#include "tabby/python_gfx.hpp"
#include "tabby/utf8.hpp"
#include "appearance_page.hpp"
#include "boot_splash.hpp"
#include "cjk_term_font.hpp"
#include "sd_page.hpp"
#include "ssh_page.hpp"
#include "system_page.hpp"
#include "terminal_view.hpp"
#include "time_page.hpp"
#include "wifi_page.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace tabby {
namespace {

constexpr char kTag[] = "tabby_ui";
constexpr int kHeaderH = 56;
constexpr int kTitleH = 52;
constexpr int kNavW = 240;
constexpr int kHeaderPad = 8;
// Two internal-RAM buffers let LVGL render the next stripe while PPA is still
// reading the previous one. Taller is better than it looks: LVGL re-walks the
// widget tree and rebuilds every rounded-corner mask once per stripe, so
// halving the stripe count halves that fixed per-object cost. The PSRAM
// fallbacks trade latency for capacity.
constexpr int kDrawRowsInternal = 64;
constexpr int kDrawRowsInternalMid = 48;
constexpr int kDrawRowsInternalMin = 32;
constexpr int kDrawRowsPsram = 64;
constexpr int kDrawRowsSingle = 96;
// RGB565 PPA transfers are most reliable when the input and output blocks are
// aligned to complete DMA macro blocks. This avoids the ESP32-P4 DIG-734
// trailing-unit erratum without rejecting small dirty regions or peeling them
// into CPU-rendered slivers. The display dimensions divide both values exactly.
constexpr int kPpaRoundX = 64;
constexpr int kPpaRoundY = 16;
// The UI loop also drains the keyboard, SSH socket, and USB serial, so it
// stays well below the 16 ms refresh period instead of pacing frames itself.
constexpr uint32_t kLoopDelayMs = 4;
constexpr uint32_t kBoardPollMs = 8;
constexpr uint32_t kTouchReadMs = 8;
// Settings pages only refresh status text, which does not need to keep up with
// the render loop and costs a lock plus a string copy per visible page.
constexpr uint32_t kPagePollMs = 32;
// How long LVGL blocks per attempt while a flush is in flight, and how long it
// keeps trying before it assumes the completion was lost.
constexpr uint32_t kFlushPollMs = 2;
constexpr uint32_t kFlushTimeoutMs = 100;
// One lost completion already means the PPA engine is stalled: the driver can
// neither abort nor recycle the transaction, so its pool slot and the engine
// lock are gone for good. Waiting for more timeouts cannot work either - once
// both pool slots have drained, every submit fails fast on the full pool, the
// flush goes synchronous, and no further timeout ever fires; a threshold
// above the pool size would leave the dead accelerator enabled forever, with
// every blit logging two driver errors over UART.
constexpr uint32_t kFlushTimeoutsBeforeCpu = 1;
// Diagnostic sampling window for the render/flush split.
constexpr uint32_t kHeartbeatMs = 3000;
constexpr int kTabH = 52;
constexpr uint32_t kNavAnimMs = 140;
constexpr uint32_t kHeaderRgb = 0x121A28;
constexpr uint32_t kAccentRgb = 0x3D8BFF;
constexpr uint32_t kScreenRgb = 0x0B1220;
constexpr uint32_t kCardRgb = 0x1B2838;
constexpr uint32_t kTextRgb = 0xE8EEF4;
constexpr uint32_t kMutedRgb = 0x8A97A8;
constexpr uint32_t kBorderRgb = 0x2A3A4E;
constexpr uint32_t kWifiOnRgb = 0x4ADE80;
constexpr uint32_t kLowBattRgb = 0xFF5C5C;
constexpr uint32_t kChargeRgb = 0x7DD3FC;

enum class SettingsPage : uint8_t { Wifi, Ssh, Time, Storage, Appearance, System };

struct SettingsTab {
    SettingsPage page;
    const char* icon;
    const char* label;
};

constexpr SettingsTab kSettingsTabs[] = {
    {SettingsPage::Wifi, LV_SYMBOL_WIFI, "Wi-Fi"},
    {SettingsPage::Ssh, LV_SYMBOL_DIRECTORY, "SSH"},
    {SettingsPage::Time, LV_SYMBOL_LOOP, "Time"},
    {SettingsPage::Storage, LV_SYMBOL_SD_CARD, "SD Card"},
    {SettingsPage::Appearance, LV_SYMBOL_EYE_OPEN, "Appearance"},
    {SettingsPage::System, LV_SYMBOL_SETTINGS, "System"},
};
constexpr size_t kSettingsTabCount = sizeof(kSettingsTabs) / sizeof(kSettingsTabs[0]);

App* g_app = nullptr;
lv_disp_draw_buf_t g_draw_buffer{};
lv_disp_drv_t g_display_driver{};
lv_indev_drv_t g_touch_driver{};
lv_color_t* g_pixels = nullptr;
lv_color_t* g_pixels_alt = nullptr;
SemaphoreHandle_t g_flush_done = nullptr;
uint32_t g_flush_wait_ms = 0;
uint32_t g_flush_timeouts = 0;

// Where a frame goes, sampled by the heartbeat. Splitting LVGL's rendering from
// the blit is the only way to tell whether the widget tree or the accelerator
// is the limit. The flush accumulator is also written from the PPA interrupt.
uint32_t g_perf_refreshes = 0;
uint32_t g_perf_blits = 0;
uint32_t g_perf_blits_sync = 0;
uint32_t g_perf_px = 0;
int64_t g_perf_handler_us = 0;
volatile int64_t g_perf_flush_us = 0;
int64_t g_perf_flush_start = 0;
lv_obj_t* g_header = nullptr;
lv_obj_t* g_settings_btn = nullptr;
lv_obj_t* g_time = nullptr;
lv_obj_t* g_wifi = nullptr;
lv_obj_t* g_battery = nullptr;
lv_obj_t* g_settings = nullptr;
lv_obj_t* g_settings_nav = nullptr;
lv_obj_t* g_settings_tabs[kSettingsTabCount] = {};
lv_obj_t* g_settings_pill = nullptr;
lv_obj_t* g_settings_right = nullptr;
lv_obj_t* g_settings_pane = nullptr;
lv_obj_t* g_settings_title = nullptr;
TerminalView g_terminal;
std::string g_command;
size_t g_cursor = 0;
size_t g_history_index = 0;
std::string g_history_draft;
bool g_dirty = true;
bool g_ssh_full_redraw = false;
bool g_python_painted = false;
uint32_t g_last_blink = 0;
uint32_t g_last_status = 0;
bool g_cursor_on = true;
SettingsPage g_settings_page = SettingsPage::Wifi;
std::string g_status_time_text;
std::string g_status_wifi_text;
std::string g_status_battery_text;
uint32_t g_status_wifi_color = UINT32_MAX;
uint32_t g_status_battery_color = UINT32_MAX;
int8_t g_status_settings_open = -1;

void rebuildSettings();
void fillSettingsContent();
void selectSettingsPage(SettingsPage page, bool animate);
void showScreen(Screen screen);
void applyTerminalFont(uint8_t height);

void roundFlushArea(lv_disp_drv_t* driver, lv_area_t* area) {
    // Once the accelerator has fallen back permanently, keep native LVGL dirty
    // areas: the CPU path benefits more from moving fewer pixels than it does
    // from macro-block alignment.
    if (g_app == nullptr || !g_app->bsp.asyncFlushSupported()) return;
    area->x1 = static_cast<lv_coord_t>((area->x1 / kPpaRoundX) * kPpaRoundX);
    area->y1 = static_cast<lv_coord_t>((area->y1 / kPpaRoundY) * kPpaRoundY);
    area->x2 = std::min<lv_coord_t>(
        driver->hor_res - 1, static_cast<lv_coord_t>(((area->x2 + kPpaRoundX) / kPpaRoundX) * kPpaRoundX - 1));
    area->y2 = std::min<lv_coord_t>(
        driver->ver_res - 1, static_cast<lv_coord_t>(((area->y2 + kPpaRoundY) / kPpaRoundY) * kPpaRoundY - 1));
}

void flush(lv_disp_drv_t* driver, const lv_area_t* area, lv_color_t* colors) {
    const int width = area->x2 - area->x1 + 1;
    const int height = area->y2 - area->y1 + 1;
    ++g_perf_blits;
    g_perf_px += static_cast<uint32_t>(width) * static_cast<uint32_t>(height);
    g_perf_flush_start = esp_timer_get_time();
    if (g_app != nullptr &&
        g_app->bsp.displayFlushAsync(area->x1, area->y1, width, height,
                                     reinterpret_cast<const uint16_t*>(colors),
                                     lv_disp_flush_is_last(driver))) {
        // The PPA interrupt reports completion once it has read the buffer.
        return;
    }
    ++g_perf_blits_sync;
    g_perf_flush_us += esp_timer_get_time() - g_perf_flush_start;
    lv_disp_flush_ready(driver);
}

bool onFlushDone(void*) {
    g_perf_flush_us += esp_timer_get_time() - g_perf_flush_start;
    lv_disp_flush_ready(&g_display_driver);
    BaseType_t woken = pdFALSE;
    if (g_flush_done != nullptr) xSemaphoreGiveFromISR(g_flush_done, &woken);
    return woken == pdTRUE;
}

void monitorRefresh(lv_disp_drv_t*, uint32_t, uint32_t) { ++g_perf_refreshes; }

void waitForFlush(lv_disp_drv_t* driver) {
    // LVGL spins here while the previous stripe is still in flight, so a
    // completion that never arrives would hang the whole UI task. Give up after
    // kFlushTimeoutMs and release the buffer ourselves; a torn frame beats a
    // frozen screen, and the counter surfaces the problem in the heartbeat.
    if (g_flush_done == nullptr) {
        taskYIELD();
        return;
    }
    if (xSemaphoreTake(g_flush_done, pdMS_TO_TICKS(kFlushPollMs)) == pdTRUE) {
        g_flush_wait_ms = 0;
        return;
    }
    g_flush_wait_ms += kFlushPollMs;
    if (g_flush_wait_ms < kFlushTimeoutMs) return;
    g_flush_wait_ms = 0;
    ++g_flush_timeouts;
    if (g_flush_timeouts == kFlushTimeoutsBeforeCpu && g_app != nullptr) {
        ESP_LOGE(kTag, "PPA stopped reporting flushes; falling back to CPU blits");
        g_app->bsp.disableAcceleration();
    }
    lv_disp_flush_ready(driver);
}

void touchRead(lv_indev_drv_t*, lv_indev_data_t* data) {
    if (g_app == nullptr) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }
    const TouchPoint point = g_app->bsp.touch();
    data->point.x = point.x;
    data->point.y = point.y;
    data->state = point.pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

const char* batterySymbol(int percent, bool charging) {
    if (charging) return LV_SYMBOL_CHARGE;
    if (percent >= 90) return LV_SYMBOL_BATTERY_FULL;
    if (percent >= 65) return LV_SYMBOL_BATTERY_3;
    if (percent >= 40) return LV_SYMBOL_BATTERY_2;
    if (percent >= 15) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

void setLabelTextIfChanged(lv_obj_t* label, std::string& cached, const std::string& text) {
    if (label == nullptr || cached == text) return;
    cached = text;
    lv_label_set_text(label, cached.c_str());
}

void setTextColorIfChanged(lv_obj_t* object, uint32_t& cached, uint32_t color) {
    if (object == nullptr || cached == color) return;
    cached = color;
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
}

void refreshStatusBar() {
    if (g_app == nullptr || g_time == nullptr) return;

    setLabelTextIfChanged(g_time, g_status_time_text, g_app->time.clockHm(g_app->config.system));

    if (g_app->wifi.connected()) {
        setTextColorIfChanged(g_wifi, g_status_wifi_color, kWifiOnRgb);
        setLabelTextIfChanged(g_wifi, g_status_wifi_text, LV_SYMBOL_WIFI);
    } else if (!g_app->wifi.enabled()) {
        setTextColorIfChanged(g_wifi, g_status_wifi_color, kMutedRgb);
        setLabelTextIfChanged(g_wifi, g_status_wifi_text, LV_SYMBOL_WIFI " Off");
    } else {
        setTextColorIfChanged(g_wifi, g_status_wifi_color, kMutedRgb);
        setLabelTextIfChanged(g_wifi, g_status_wifi_text, LV_SYMBOL_WIFI " --");
    }

    if (!g_app->bsp.batteryPresent()) {
        setTextColorIfChanged(g_battery, g_status_battery_color, kMutedRgb);
        setLabelTextIfChanged(g_battery, g_status_battery_text, LV_SYMBOL_BATTERY_EMPTY " n/a");
    } else {
        const int percent = g_app->bsp.batteryPercent();
        const bool charging = g_app->bsp.batteryCharging();
        uint32_t color = kTextRgb;
        if (charging) color = kChargeRgb;
        else if (percent <= 15) color = kLowBattRgb;
        setTextColorIfChanged(g_battery, g_status_battery_color, color);
        char text[32];
        std::snprintf(text, sizeof(text), "%s %d%%", batterySymbol(percent, charging), percent);
        setLabelTextIfChanged(g_battery, g_status_battery_text, text);
    }

    if (g_settings_btn != nullptr) {
        const bool open = g_app->screen == Screen::Settings;
        if (g_status_settings_open != static_cast<int8_t>(open)) {
            g_status_settings_open = static_cast<int8_t>(open);
            lv_obj_set_style_bg_color(g_settings_btn, lv_color_hex(open ? kAccentRgb : kCardRgb), 0);
        }
    }
}

lv_obj_t* makeIconButton(lv_obj_t* parent, const char* symbol) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 44, 44);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kCardRgb), 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(btn, 8);
    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_font(label, wifi_ui::font20(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(kTextRgb), 0);
    lv_obj_center(label);
    return btn;
}

void animMoveY(void* var, int32_t value) {
    lv_obj_set_y(static_cast<lv_obj_t*>(var), value);
}

lv_coord_t selectedTabY() {
    for (size_t i = 0; i < kSettingsTabCount; ++i) {
        if (g_settings_tabs[i] != nullptr && kSettingsTabs[i].page == g_settings_page) {
            return lv_obj_get_y(g_settings_tabs[i]);
        }
    }
    return 0;
}

void moveNavPill(bool animate) {
    if (g_settings_pill == nullptr || g_settings_nav == nullptr) return;
    lv_obj_update_layout(g_settings_nav);
    const lv_coord_t to_y = selectedTabY();
    const lv_coord_t from_y = lv_obj_get_y(g_settings_pill);
    lv_anim_del(g_settings_pill, nullptr);
    if (!animate || from_y == to_y) {
        lv_obj_set_y(g_settings_pill, to_y);
        return;
    }
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, g_settings_pill);
    lv_anim_set_values(&anim, from_y, to_y);
    lv_anim_set_time(&anim, kNavAnimMs);
    lv_anim_set_exec_cb(&anim, animMoveY);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);
}

void updateNavHighlight() {
    for (size_t i = 0; i < kSettingsTabCount; ++i) {
        lv_obj_t* tab = g_settings_tabs[i];
        if (tab == nullptr) continue;
        const bool on = kSettingsTabs[i].page == g_settings_page;
        const uint32_t color = on ? kTextRgb : kMutedRgb;
        lv_obj_t* icon = lv_obj_get_child(tab, 0);
        lv_obj_t* label = lv_obj_get_child(tab, 1);
        if (icon != nullptr) lv_obj_set_style_text_color(icon, lv_color_hex(color), 0);
        if (label != nullptr) lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    }
}

lv_obj_t* makeNavTab(lv_obj_t* parent, const char* icon, const char* text) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_size(btn, LV_PCT(100), kTabH);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_hor(btn, 14, 0);
    lv_obj_set_style_pad_ver(btn, 0, 0);
    lv_obj_set_style_pad_gap(btn, 12, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kHeaderRgb), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(btn, 4);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kCardRgb), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_t* icon_label = lv_label_create(btn);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_text_font(icon_label, wifi_ui::font16(), 0);
    lv_obj_set_style_text_color(icon_label, lv_color_hex(kMutedRgb), 0);

    lv_obj_t* text_label = lv_label_create(btn);
    lv_label_set_text(text_label, text);
    lv_obj_set_style_text_font(text_label, wifi_ui::font16(), 0);
    lv_obj_set_style_text_color(text_label, lv_color_hex(kMutedRgb), 0);
    return btn;
}

void openSettings() {
    g_app->screen = Screen::Settings;
    g_dirty = true;
}

void closeSettings() {
    g_app->screen = Screen::Terminal;
    g_dirty = true;
}

void settingsBack() {
    closeSettings();
}

void applyTerminalFont(uint8_t height) {
    const char* name = "mono36";
    if (height <= 20) name = "mono20";
    else if (height <= 24) name = "mono24";
    else if (height <= 28) name = "mono28";
    else if (height <= 32) name = "mono32";
    g_app->config.keyboard.terminalFont = name;
    g_app->config.keyboard.terminalLineStep = height;
    g_app->settings.save(g_app->config);
    g_terminal.configureFont(height);
    g_app->terminal.setViewport(static_cast<size_t>(g_terminal.columns()),
                                static_cast<size_t>(g_terminal.rows()));
    g_app->vt.resize(static_cast<size_t>(g_terminal.columns()), static_cast<size_t>(g_terminal.rows()));
    g_app->ssh.setPtyHint(g_terminal.columns(), g_terminal.rows());
    if (g_app->ssh.connected()) {
        g_app->ssh.resizePty(g_terminal.columns(), g_terminal.rows());
    }
    if (g_app->editor.active()) g_app->editor.requestRedraw();
    ssh_ui::setPtySize(g_terminal.columns(), g_terminal.rows());
    g_dirty = true;
    appearance_ui::refresh();
}

void fillSettingsContent() {
    if (g_app == nullptr || g_settings_title == nullptr) return;
    wifi_ui::setVisible(false);
    ssh_ui::setVisible(false);
    time_ui::setVisible(false);
    sd_ui::setVisible(false);
    appearance_ui::setVisible(false);
    system_ui::setVisible(false);

    switch (g_settings_page) {
        case SettingsPage::Wifi: {
            lv_label_set_text(g_settings_title, "Wi-Fi");
            wifi_ui::setVisible(true);
            break;
        }

        case SettingsPage::Ssh: {
            lv_label_set_text(g_settings_title, "SSH");
            ssh_ui::setVisible(true);
            break;
        }

        case SettingsPage::Time: {
            lv_label_set_text(g_settings_title, "Time");
            time_ui::setVisible(true);
            break;
        }

        case SettingsPage::Storage: {
            lv_label_set_text(g_settings_title, "SD Card");
            sd_ui::setVisible(true);
            break;
        }

        case SettingsPage::Appearance: {
            lv_label_set_text(g_settings_title, "Appearance");
            appearance_ui::setVisible(true);
            break;
        }

        case SettingsPage::System: {
            lv_label_set_text(g_settings_title, "System");
            system_ui::setVisible(true);
            break;
        }
    }
}

void rebuildSettings() {
    updateNavHighlight();
    moveNavPill(false);
    fillSettingsContent();
}

void selectSettingsPage(SettingsPage page, bool animate) {
    if (g_settings_title == nullptr) {
        g_settings_page = page;
        return;
    }
    if (page == g_settings_page) return;
    g_settings_page = page;
    updateNavHighlight();
    fillSettingsContent();
    moveNavPill(animate);
}

void showScreen(Screen screen) {
    if (screen == Screen::Settings) {
        lv_obj_add_flag(g_terminal.object(), LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_settings, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_settings);
        rebuildSettings();
    } else {
        lv_anim_del(g_settings_pill, nullptr);
        lv_obj_add_flag(g_settings, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_terminal.object(), LV_OBJ_FLAG_HIDDEN);
        g_dirty = true;
    }
    refreshStatusBar();
}

void resetHistoryNavigation() {
    g_history_index = g_app->cli.history().size();
    g_history_draft.clear();
}

int pageLines() { return std::max(1, g_terminal.rows() - 1); }

bool serialLive() { return g_app != nullptr && g_app->serial.connected(); }

bool sshLive() { return g_app != nullptr && g_app->ssh.connected(); }

bool vtLive() { return sshLive() || serialLive(); }

void closeSerialSession(const char* reason) {
    if (g_app == nullptr) return;
    g_app->serial.disconnect();
    if (reason != nullptr && reason[0] != '\0') g_app->cli.appendLine(reason);
    g_dirty = true;
}

int resolveScrollDelta(int value) {
    if (value >= kTerminalScrollPage) return pageLines();
    if (value <= -kTerminalScrollPage) return -pageLines();
    return value;
}

void applyTerminalScroll(int delta) {
    if (g_app == nullptr || g_app->screen != Screen::Terminal) return;
    if (pythonGfx().active() || g_app->editor.active()) return;
    delta = resolveScrollDelta(delta);
    if (delta == 0) return;
    if (vtLive()) g_app->vt.scrollback(delta);
    else g_app->terminal.scroll(delta);
    g_dirty = true;
}

struct TerminalDrag {
    bool active{false};
    lv_coord_t last_y{0};
    int leftover{0};
} g_term_drag;

void onTerminalPointer(lv_event_t* event) {
    if (g_app == nullptr || g_app->screen != Screen::Terminal) return;
    if (pythonGfx().active() || g_app->editor.active()) return;

    const lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t* indev = lv_indev_get_act();
    if (indev == nullptr) return;
    lv_point_t point{};
    lv_indev_get_point(indev, &point);

    if (code == LV_EVENT_PRESSED) {
        g_term_drag.active = true;
        g_term_drag.last_y = point.y;
        g_term_drag.leftover = 0;
        return;
    }
    if (code == LV_EVENT_PRESSING && g_term_drag.active) {
        g_term_drag.leftover += point.y - g_term_drag.last_y;
        g_term_drag.last_y = point.y;
        const int cell = std::max(1, g_terminal.cellHeight());
        const int lines = g_term_drag.leftover / cell;
        if (lines != 0) {
            g_term_drag.leftover -= lines * cell;
            applyTerminalScroll(lines);
        }
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        g_term_drag.active = false;
        g_term_drag.leftover = 0;
    }
}

void navigateCommandHistory(bool older) {
    const auto history = g_app->cli.history();
    if (history.empty()) return;

    g_history_index = std::min(g_history_index, history.size());
    if (older) {
        if (g_history_index == 0) return;
        if (g_history_index == history.size()) g_history_draft = g_command;
        --g_history_index;
        g_command = history[g_history_index];
    } else {
        if (g_history_index >= history.size()) return;
        ++g_history_index;
        g_command = g_history_index == history.size() ? g_history_draft : history[g_history_index];
    }
    g_cursor = g_command.size();
}

void handleAction(const KeyAction& action) {
    if (BootSplashVisible()) return;
    if (action.type == KeyActionType::Menu) {
        if (g_app->screen == Screen::Settings && wifi_ui::handleKey(action)) return;
        if (g_app->screen == Screen::Settings && ssh_ui::handleKey(action)) return;
        if (g_app->screen == Screen::Settings && sd_ui::handleKey(action)) return;
        if (g_app->screen == Screen::Settings) {
            settingsBack();
            return;
        }
        if (serialLive()) {
            closeSerialSession("USB serial closed");
            return;
        }
        if (g_app->ssh.connected() || g_app->editor.active()) {
            handleAction({KeyActionType::Text, "\x1B", 0});
            return;
        }
        openSettings();
        return;
    }
    if (action.type == KeyActionType::Scroll) {
        if (g_app->editor.active()) {
            handleAction({KeyActionType::Text, action.value > 0 ? "\x1B[5~" : "\x1B[6~", 0});
            return;
        }
        applyTerminalScroll(action.value);
        return;
    }
    if (g_app->screen == Screen::Settings && wifi_ui::handleKey(action)) return;
    if (g_app->screen == Screen::Settings && ssh_ui::handleKey(action)) return;
    if (g_app->screen == Screen::Settings && sd_ui::handleKey(action)) return;
    if (action.type != KeyActionType::Text || g_app->screen != Screen::Terminal) return;
    if (g_app->editor.active()) {
        if (!g_app->editor.handleKey(action.text)) {
            g_app->vt.reset();
            g_app->vt.resize(static_cast<size_t>(g_terminal.columns()), static_cast<size_t>(g_terminal.rows()));
            if (!g_app->editor.quitMessage().empty()) g_app->cli.appendLine(g_app->editor.quitMessage());
        }
        g_dirty = true;
        return;
    }
    const bool ctrl_c = action.text.find('\x03') != std::string::npos;
    if (g_app->python.running() && !action.text.empty()) {
        const char c = action.text[0];
        if (c == 0x03 || c == 'q' || c == 'Q') {
            g_app->python.requestInterrupt();
            return;
        }
    }
    if (serialLive()) {
        if (action.text == "\x1B" || ctrl_c) {
            closeSerialSession("USB serial closed");
            return;
        }
        g_app->vt.scrollbackToBottom();
        std::string payload = action.text;
        for (char& c : payload) {
            if (c == '\n') c = '\r';
        }
        if (!g_app->serial.write(reinterpret_cast<const uint8_t*>(payload.data()), payload.size())) {
            ESP_LOGW(kTag, "serial write dropped %u bytes", static_cast<unsigned>(payload.size()));
        }
        g_app->vt.markCursorDirty();
        g_dirty = true;
        return;
    }
    if (g_app->ssh.connected()) {
        g_app->vt.scrollbackToBottom();
        std::string payload = action.text;
        for (char& c : payload) {
            if (c == '\n') c = '\r';
        }
        if (!g_app->ssh.write(reinterpret_cast<const uint8_t*>(payload.data()), payload.size())) {
            ESP_LOGW(kTag, "ssh write dropped %u bytes", static_cast<unsigned>(payload.size()));
        }
        g_app->vt.markCursorDirty();
        g_dirty = true;
        return;
    }
    if (ctrl_c) {
        g_app->terminal.scrollToBottom();
        if (g_app->cli.busy()) {
            g_app->cli.requestInterrupt();
        } else {
            g_app->cli.appendLine(std::string(g_app->cli.prompt()) + g_command + "^C");
            if (g_app->cli.pythonRepl()) g_app->cli.appendLine("KeyboardInterrupt");
            g_command.clear();
            g_cursor = 0;
            resetHistoryNavigation();
        }
        g_dirty = true;
        return;
    }
    if (action.text == "\x1B") {
        openSettings();
        return;
    }
    if (action.text == "\x1B[5~" || action.text == "\x1B[6~") {
        applyTerminalScroll(action.text == "\x1B[5~" ? kTerminalScrollPage : -kTerminalScrollPage);
        return;
    }
    if (g_app->cli.busy()) return;
    g_app->terminal.scrollToBottom();
    if (action.text == "\x1B[A" || action.text == "\x1B[B") {
        navigateCommandHistory(action.text == "\x1B[A");
        g_dirty = true;
        return;
    }
    if (action.text == "\x1B[C") {
        if (g_cursor < g_command.size()) {
            size_t next = g_cursor;
            utf8Next(g_command, next);
            g_cursor = next;
        }
        g_dirty = true;
        return;
    }
    if (action.text == "\x1B[D") {
        if (g_cursor > 0) g_cursor = utf8Prev(g_command, g_cursor);
        g_dirty = true;
        return;
    }
    if (action.text == "\x1B[H" || action.text == "\x1B[1~") {
        g_cursor = 0;
        g_dirty = true;
        return;
    }
    if (action.text == "\x1B[F" || action.text == "\x1B[4~") {
        g_cursor = g_command.size();
        g_dirty = true;
        return;
    }
    if (action.text.size() >= 2 && action.text[0] == '\x1B') return;
    for (char c : action.text) {
        if (c == '\r' || c == '\n') {
            g_app->cli.execute(g_command);
            g_command.clear();
            g_cursor = 0;
            resetHistoryNavigation();
        } else if (c == 0x7F || c == '\b') {
            if (g_cursor > 0 && !g_command.empty()) {
                const size_t prev = utf8Prev(g_command, g_cursor);
                g_command.erase(prev, g_cursor - prev);
                g_cursor = prev;
            }
        } else if (std::isprint(static_cast<unsigned char>(c)) || c == '\t' ||
                   static_cast<unsigned char>(c) >= 0x80) {
            g_command.insert(g_cursor, 1, c);
            ++g_cursor;
        }
    }
    g_dirty = true;
}

void createStatusBar(lv_obj_t* screen, int width) {
    g_header = lv_obj_create(screen);
    lv_obj_set_size(g_header, width, kHeaderH);
    lv_obj_set_pos(g_header, 0, 0);
    lv_obj_set_style_bg_color(g_header, lv_color_hex(kHeaderRgb), 0);
    lv_obj_set_style_bg_opa(g_header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_header, 1, 0);
    lv_obj_set_style_border_side(g_header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(g_header, lv_color_hex(kBorderRgb), 0);
    lv_obj_set_style_radius(g_header, 0, 0);
    lv_obj_set_style_pad_all(g_header, 0, 0);
    lv_obj_clear_flag(g_header, LV_OBJ_FLAG_SCROLLABLE);

    g_settings_btn = makeIconButton(g_header, LV_SYMBOL_SETTINGS);
    lv_obj_align(g_settings_btn, LV_ALIGN_LEFT_MID, kHeaderPad, 0);
    lv_obj_add_event_cb(
        g_settings_btn,
        [](lv_event_t*) {
            if (g_app == nullptr) return;
            if (g_app->screen == Screen::Settings) closeSettings();
            else openSettings();
        },
        LV_EVENT_CLICKED, nullptr);

    g_time = lv_label_create(g_header);
    lv_obj_set_style_text_font(g_time, wifi_ui::font20(), 0);
    lv_obj_set_style_text_color(g_time, lv_color_hex(kTextRgb), 0);
    lv_label_set_text(g_time, "--:--");
    lv_obj_align(g_time, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t* right = lv_obj_create(g_header);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, -kHeaderPad, 0);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 0, 0);
    lv_obj_set_style_pad_gap(right, 16, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    g_wifi = lv_label_create(right);
    lv_obj_set_style_text_font(g_wifi, wifi_ui::font16(), 0);
    lv_obj_set_style_text_color(g_wifi, lv_color_hex(kMutedRgb), 0);
    lv_label_set_text(g_wifi, LV_SYMBOL_WIFI " --");

    g_battery = lv_label_create(right);
    lv_obj_set_style_text_font(g_battery, wifi_ui::font16(), 0);
    lv_obj_set_style_text_color(g_battery, lv_color_hex(kTextRgb), 0);
    lv_label_set_text(g_battery, LV_SYMBOL_BATTERY_FULL " --%");
}

void createSettings(lv_obj_t* screen, int width, int body_y, int body_h) {
    g_settings = lv_obj_create(screen);
    lv_obj_set_pos(g_settings, 0, body_y);
    lv_obj_set_size(g_settings, width, body_h);
    lv_obj_set_style_bg_color(g_settings, lv_color_hex(kScreenRgb), 0);
    lv_obj_set_style_bg_opa(g_settings, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_settings, 0, 0);
    lv_obj_set_style_radius(g_settings, 0, 0);
    lv_obj_set_style_pad_all(g_settings, 0, 0);
    lv_obj_set_style_pad_gap(g_settings, 0, 0);
    lv_obj_set_flex_flow(g_settings, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_settings, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(g_settings, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_settings, LV_OBJ_FLAG_HIDDEN);

    g_settings_nav = lv_obj_create(g_settings);
    lv_obj_set_size(g_settings_nav, kNavW, LV_PCT(100));
    lv_obj_set_style_bg_color(g_settings_nav, lv_color_hex(kHeaderRgb), 0);
    lv_obj_set_style_bg_opa(g_settings_nav, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_settings_nav, 1, 0);
    lv_obj_set_style_border_side(g_settings_nav, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_color(g_settings_nav, lv_color_hex(kBorderRgb), 0);
    lv_obj_set_style_radius(g_settings_nav, 0, 0);
    lv_obj_set_style_pad_all(g_settings_nav, 12, 0);
    lv_obj_set_style_pad_row(g_settings_nav, 8, 0);
    lv_obj_set_flex_flow(g_settings_nav, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(g_settings_nav, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(g_settings_nav, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* heading = lv_label_create(g_settings_nav);
    lv_obj_set_style_text_font(heading, wifi_ui::font20(), 0);
    lv_obj_set_style_text_color(heading, lv_color_hex(kTextRgb), 0);
    lv_label_set_text(heading, "Settings");
    lv_obj_set_style_pad_bottom(heading, 8, 0);
    lv_obj_set_style_pad_left(heading, 4, 0);

    g_settings_pill = lv_obj_create(g_settings_nav);
    lv_obj_add_flag(g_settings_pill, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(g_settings_pill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g_settings_pill, LV_PCT(100), kTabH);
    lv_obj_set_style_bg_color(g_settings_pill, lv_color_hex(kAccentRgb), 0);
    lv_obj_set_style_bg_opa(g_settings_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_settings_pill, 12, 0);
    lv_obj_set_style_border_width(g_settings_pill, 0, 0);
    lv_obj_set_style_pad_all(g_settings_pill, 0, 0);
    lv_obj_set_x(g_settings_pill, 0);

    for (size_t i = 0; i < kSettingsTabCount; ++i) {
        g_settings_tabs[i] = makeNavTab(g_settings_nav, kSettingsTabs[i].icon, kSettingsTabs[i].label);
        lv_obj_add_event_cb(
            g_settings_tabs[i],
            [](lv_event_t* event) {
                const auto page = static_cast<SettingsPage>(
                    reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
                selectSettingsPage(page, true);
            },
            LV_EVENT_PRESSED, reinterpret_cast<void*>(static_cast<uintptr_t>(kSettingsTabs[i].page)));
    }

    g_settings_right = lv_obj_create(g_settings);
    lv_obj_set_width(g_settings_right, 0);
    lv_obj_set_flex_grow(g_settings_right, 1);
    lv_obj_set_height(g_settings_right, LV_PCT(100));
    lv_obj_set_style_bg_color(g_settings_right, lv_color_hex(kScreenRgb), 0);
    lv_obj_set_style_bg_opa(g_settings_right, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_settings_right, 0, 0);
    lv_obj_set_style_radius(g_settings_right, 0, 0);
    lv_obj_set_style_pad_all(g_settings_right, 0, 0);
    // No clip_corner here: the radius is 0, so it would clip nothing while still
    // putting a mask in front of every child draw in the pane.
    lv_obj_clear_flag(g_settings_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g_settings_right, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    g_settings_pane = lv_obj_create(g_settings_right);
    lv_obj_set_pos(g_settings_pane, 0, 0);
    lv_obj_set_size(g_settings_pane, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(g_settings_pane, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_settings_pane, 0, 0);
    lv_obj_set_style_radius(g_settings_pane, 0, 0);
    lv_obj_set_style_pad_all(g_settings_pane, 0, 0);
    lv_obj_set_flex_flow(g_settings_pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(g_settings_pane, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title_bar = lv_obj_create(g_settings_pane);
    lv_obj_set_size(title_bar, LV_PCT(100), kTitleH);
    lv_obj_set_style_bg_opa(title_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_bar, 1, 0);
    lv_obj_set_style_border_side(title_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(title_bar, lv_color_hex(kBorderRgb), 0);
    lv_obj_set_style_radius(title_bar, 0, 0);
    lv_obj_set_style_pad_hor(title_bar, 20, 0);
    lv_obj_set_style_pad_ver(title_bar, 0, 0);
    lv_obj_clear_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);

    g_settings_title = lv_label_create(title_bar);
    lv_obj_set_style_text_font(g_settings_title, wifi_ui::font20(), 0);
    lv_obj_set_style_text_color(g_settings_title, lv_color_hex(kTextRgb), 0);
    lv_label_set_text(g_settings_title, "Wi-Fi");
    lv_obj_align(g_settings_title, LV_ALIGN_LEFT_MID, 0, 0);

    wifi_ui::create(*g_app, g_settings_pane);
    ssh_ui::create(*g_app, g_settings_pane);
    time_ui::create(*g_app, g_settings_pane);
    sd_ui::create(*g_app, g_settings_pane);
    appearance_ui::create(*g_app, g_settings_pane, applyTerminalFont);
    system_ui::create(*g_app, g_settings_pane);
    updateNavHighlight();
    moveNavPill(false);
}

void createWidgets(int width, int height) {
    wifi_ui::initFonts();
    lv_obj_t* screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(kScreenRgb), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    createStatusBar(screen, width);

    const int body_y = kHeaderH;
    const int body_h = height - kHeaderH;
    g_terminal.create(screen, 0, body_y, width, body_h);
    g_terminal.configureFont(g_app->config.keyboard.terminalLineStep);
    if (lv_obj_t* canvas = g_terminal.object()) {
        lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(canvas, LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(canvas, onTerminalPointer, LV_EVENT_PRESSED, nullptr);
        lv_obj_add_event_cb(canvas, onTerminalPointer, LV_EVENT_PRESSING, nullptr);
        lv_obj_add_event_cb(canvas, onTerminalPointer, LV_EVENT_RELEASED, nullptr);
        lv_obj_add_event_cb(canvas, onTerminalPointer, LV_EVENT_PRESS_LOST, nullptr);
    }
    ssh_ui::setPtySize(g_terminal.columns(), g_terminal.rows());
    g_app->ssh.setPtyHint(g_terminal.columns(), g_terminal.rows());
    g_app->terminal.setViewport(static_cast<size_t>(g_terminal.columns()),
                                static_cast<size_t>(g_terminal.rows()));
    g_app->vt.resize(static_cast<size_t>(g_terminal.columns()), static_cast<size_t>(g_terminal.rows()));
    pythonGfx().begin(g_terminal.pixels(), g_terminal.pixelWidth(), g_terminal.pixelHeight(),
                      g_terminal.pixelWidth());
    pythonGfx().setAbortPoll([]() { return g_app && g_app->python.interruptRequested(); });
    createSettings(screen, width, body_y, body_h);
    refreshStatusBar();
}

// Rendering into internal RAM avoids the PSRAM round trip and stops the draw
// buffer from evicting everything else out of L2 cache, so it is tried first.
int allocateDrawBuffers(int width) {
    struct Attempt {
        int rows;
        uint32_t first_caps;
        uint32_t second_caps;
        bool paired;
    };
    const Attempt attempts[] = {
        {kDrawRowsInternal, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, true},
        {kDrawRowsInternalMid, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, true},
        // If only one 64-row block fits internally, retain it and place the
        // other in PSRAM. This uses the same amount of scarce SRAM as two
        // 32-row buffers while halving LVGL's stripe/object traversal count.
        {kDrawRowsInternal, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, true},
        {kDrawRowsInternalMin, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, true},
        {kDrawRowsPsram, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, true},
        {kDrawRowsSingle, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, 0, false},
        {kDrawRowsInternalMin, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, 0, false},
    };
    for (const Attempt& attempt : attempts) {
        const size_t bytes = static_cast<size_t>(width) * attempt.rows * sizeof(lv_color_t);
        g_pixels = static_cast<lv_color_t*>(heap_caps_malloc(bytes, attempt.first_caps));
        if (g_pixels == nullptr) continue;
        if (!attempt.paired) return attempt.rows;
        g_pixels_alt = static_cast<lv_color_t*>(heap_caps_malloc(bytes, attempt.second_caps));
        if (g_pixels_alt != nullptr) return attempt.rows;
        heap_caps_free(g_pixels);
        g_pixels = nullptr;
    }
    return 0;
}

void uiTask(void*) {
    g_app->bsp.update();
    lv_init();
    const int width = g_app->bsp.displayWidth();
    const int height = g_app->bsp.displayHeight();
    const int draw_rows = allocateDrawBuffers(width);
    if (draw_rows == 0) {
        ESP_LOGE(kTag, "LVGL draw buffer alloc failed");
        vTaskDelete(nullptr);
        return;
    }
    const char* first_memory = esp_ptr_internal(g_pixels) ? "internal" : "psram";
    const char* second_memory = g_pixels_alt == nullptr ? nullptr : (esp_ptr_internal(g_pixels_alt) ? "internal" : "psram");
    if (second_memory != nullptr) {
        ESP_LOGI(kTag, "LVGL draw buffers: 2 x %d rows, %s/%s", draw_rows, first_memory, second_memory);
    } else {
        ESP_LOGI(kTag, "LVGL draw buffer: 1 x %d rows, %s", draw_rows, first_memory);
    }

    lv_disp_draw_buf_init(&g_draw_buffer, g_pixels, g_pixels_alt, width * draw_rows);
    lv_disp_drv_init(&g_display_driver);
    g_display_driver.hor_res = width;
    g_display_driver.ver_res = height;
    g_display_driver.flush_cb = flush;
    g_display_driver.rounder_cb = roundFlushArea;
    g_display_driver.monitor_cb = monitorRefresh;
    g_display_driver.draw_buf = &g_draw_buffer;
    if (g_pixels_alt != nullptr && g_app->bsp.asyncFlushSupported()) {
        g_flush_done = xSemaphoreCreateBinary();
        if (g_flush_done != nullptr) {
            g_display_driver.wait_cb = waitForFlush;
            g_app->bsp.setFlushDoneHandler(onFlushDone, nullptr);
        }
    }
    lv_disp_drv_register(&g_display_driver);

    lv_indev_drv_init(&g_touch_driver);
    g_touch_driver.type = LV_INDEV_TYPE_POINTER;
    g_touch_driver.read_cb = touchRead;
    lv_indev_t* touch = lv_indev_drv_register(&g_touch_driver);
    if (touch != nullptr && g_touch_driver.read_timer != nullptr) {
        lv_timer_set_period(g_touch_driver.read_timer, kTouchReadMs);
    }

    createWidgets(width, height);
    BootSplashCreateUi();
    g_app->cli.appendLine("Tabby IDF + LVGL");
    g_app->cli.appendLine(g_app->keyboard.status());
    g_app->cli.appendLine("type 'help' for commands");
    g_dirty = true;

    uint32_t last_tick = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    uint32_t last_board_poll = last_tick;
    uint32_t last_page_poll = last_tick;
    uint32_t terminal_revision = g_app->terminal.revision();
    bool last_cli_busy = g_app->cli.busy();
    Screen last_screen = g_app->screen;
    uint32_t last_beat = last_tick;
    uint32_t beat_loops = 0;
    for (;;) {
        const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
        lv_tick_inc(now - last_tick);
        last_tick = now;

        ++beat_loops;
        if (now - last_beat >= kHeartbeatMs) {
            const uint32_t elapsed = now - last_beat;
            const uint32_t refreshes = g_perf_refreshes;
            if (refreshes > 0) {
                // Per frame rather than per second, so the numbers stay
                // comparable whether or not anything is being redrawn.
                ESP_LOGI(kTag,
                         "perf: %u fps | handler %u us | flush %u us | %u blits (%u sync) | %u kpx | %u loops | %u timeouts",
                         static_cast<unsigned>(refreshes * 1000 / elapsed),
                         static_cast<unsigned>(g_perf_handler_us / refreshes),
                         static_cast<unsigned>(g_perf_flush_us / refreshes),
                         static_cast<unsigned>(g_perf_blits / refreshes),
                         static_cast<unsigned>(g_perf_blits_sync / refreshes),
                         static_cast<unsigned>(g_perf_px / refreshes / 1000),
                         static_cast<unsigned>(beat_loops),
                         static_cast<unsigned>(g_flush_timeouts));
            }
            g_perf_refreshes = 0;
            g_perf_blits = 0;
            g_perf_blits_sync = 0;
            g_perf_px = 0;
            g_perf_handler_us = 0;
            g_perf_flush_us = 0;
            last_beat = now;
            beat_loops = 0;
        }

        // Touch and power come over I2C, so they are sampled at roughly twice
        // the LVGL input rate rather than on every pass of the loop.
        if (now - last_board_poll >= kBoardPollMs) {
            last_board_poll = now;
            g_app->bsp.update();
        }
        for (int handled = 0; handled < 32 && g_app->keyboard.available(); ++handled) {
            handleAction(g_app->keyboard.read());
        }

        if (BootSplashPoll()) {
            g_terminal.markFullRedraw();
            if (vtLive() || g_app->editor.active()) g_app->vt.markAllDirty();
            g_ssh_full_redraw = true;
            g_dirty = true;
        }

        if (now - g_last_blink > 500) {
            g_last_blink = now;
            g_cursor_on = !g_cursor_on;
            if (vtLive() || g_app->editor.active()) g_app->vt.markCursorDirty();
            g_dirty = true;
        }
        if (now - g_last_status > 1000) {
            g_last_status = now;
            refreshStatusBar();
        }
        if (now - last_page_poll >= kPagePollMs) {
            last_page_poll = now;
            wifi_ui::poll();
            ssh_ui::poll();
            time_ui::poll();
            sd_ui::poll();
            system_ui::poll();
            CjkTermFont::tryLoadFromSd();
            if (CjkTermFont::takeNewlyLoaded()) {
                g_terminal.markFullRedraw();
                if (vtLive() || g_app->editor.active()) g_app->vt.markAllDirty();
                g_ssh_full_redraw = true;
                g_dirty = true;
            }
        }
        const uint32_t revision = g_app->terminal.revision();
        if (revision != terminal_revision) {
            terminal_revision = revision;
            g_dirty = true;
        }
        const bool cli_busy = g_app->cli.busy();
        if (cli_busy != last_cli_busy) {
            last_cli_busy = cli_busy;
            g_dirty = true;
        }
        if (last_screen != g_app->screen) {
            showScreen(g_app->screen);
            last_screen = g_app->screen;
        }
        if (g_app->cli.takeSshSessionStart() && g_app->ssh.connected()) {
            g_app->vt.reset();
            g_app->vt.resize(static_cast<size_t>(g_terminal.columns()), static_cast<size_t>(g_terminal.rows()));
            g_app->vt.markAllDirty();
            g_app->ssh.resizePty(g_terminal.columns(), g_terminal.rows());
            g_command.clear();
            g_cursor = 0;
            resetHistoryNavigation();
            g_ssh_full_redraw = true;
            g_dirty = true;
        }
        if (g_app->cli.takeSerialSessionStart() && g_app->serial.connected()) {
            g_app->vt.reset();
            g_app->vt.resize(static_cast<size_t>(g_terminal.columns()), static_cast<size_t>(g_terminal.rows()));
            g_app->vt.markAllDirty();
            g_command.clear();
            g_cursor = 0;
            resetHistoryNavigation();
            g_ssh_full_redraw = true;
            g_dirty = true;
        }
        if (g_app->editor.takeStart()) {
            g_app->vt.reset();
            g_app->vt.resize(static_cast<size_t>(g_terminal.columns()), static_cast<size_t>(g_terminal.rows()));
            g_app->editor.requestRedraw();
            g_ssh_full_redraw = true;
            g_dirty = true;
        }
        if (g_app->ssh.connected() && !g_app->editor.active()) {
            char buffer[512];
            size_t consumed = 0;
            constexpr size_t kMaxVtBytes = 2048;
            for (int i = 0; i < 8 && consumed < kMaxVtBytes; ++i) {
                const size_t want = std::min(sizeof(buffer), kMaxVtBytes - consumed);
                const int n = g_app->ssh.read(buffer, want);
                if (n > 0) {
                    g_app->vt.write(buffer, static_cast<size_t>(n));
                    consumed += static_cast<size_t>(n);
                    g_dirty = true;
                    continue;
                }
                if (n < 0) {
                    g_app->ssh.disconnect();
                    g_app->cli.appendLine("SSH disconnected by remote");
                    g_dirty = true;
                }
                break;
            }
        } else if (g_app->serial.connected() && !g_app->editor.active()) {
            char buffer[512];
            size_t consumed = 0;
            constexpr size_t kMaxVtBytes = 2048;
            for (int i = 0; i < 8 && consumed < kMaxVtBytes; ++i) {
                const size_t want = std::min(sizeof(buffer), kMaxVtBytes - consumed);
                const int n = g_app->serial.read(buffer, want);
                if (n > 0) {
                    g_app->vt.write(buffer, static_cast<size_t>(n));
                    consumed += static_cast<size_t>(n);
                    g_dirty = true;
                    continue;
                }
                if (n < 0) {
                    closeSerialSession("USB serial disconnected");
                }
                break;
            }
        } else if (g_app->ssh.connected() && g_app->editor.active()) {
            char buffer[512];
            for (int i = 0; i < 4; ++i) {
                const int n = g_app->ssh.read(buffer, sizeof(buffer));
                if (n <= 0) break;
            }
        }
        if (pythonGfx().takePresentRequest() && g_app->screen == Screen::Terminal) {
            pythonGfx().lockFrame();
            g_app->bsp.displayFlush(0, kHeaderH, g_terminal.pixelWidth(), g_terminal.pixelHeight(),
                                    g_terminal.pixels());
            pythonGfx().unlockFrame();
            g_python_painted = true;
        }
        if (g_dirty && g_app->screen == Screen::Terminal && !BootSplashVisible()) {
            pythonGfx().lockFrame();
            if (!pythonGfx().active()) {
                if (g_python_painted) {
                    // MicroPython drew straight into the canvas buffer, so
                    // neither the row cache nor the VT dirty flags describe
                    // what is on screen any more.
                    g_terminal.markFullRedraw();
                    g_ssh_full_redraw = true;
                    g_python_painted = false;
                }
                if (g_app->editor.active()) {
                    g_app->editor.paint(g_app->vt);
                    g_terminal.renderVt(g_app->vt, g_cursor_on, g_ssh_full_redraw);
                    g_ssh_full_redraw = false;
                } else if (vtLive()) {
                    g_terminal.renderVt(g_app->vt, g_cursor_on, g_ssh_full_redraw);
                    g_ssh_full_redraw = false;
                } else {
                    const std::string prompt = g_app->cli.prompt();
                    g_terminal.render(g_app->terminal, g_command, g_cursor, g_cursor_on && !cli_busy, prompt.c_str(),
                                      !cli_busy);
                }
                g_dirty = false;
            }
            pythonGfx().unlockFrame();
        }
        const int64_t handler_start = esp_timer_get_time();
        lv_timer_handler();
        g_perf_handler_us += esp_timer_get_time() - handler_start;
        // Sleeping a whole refresh period here would stretch every frame to
        // "render time + 16 ms" and halve the effective animation rate.
        vTaskDelay(pdMS_TO_TICKS(kLoopDelayMs));
    }
}

}  // namespace

bool UiStart(App& app) {
    g_app = &app;
    return xTaskCreatePinnedToCore(uiTask, "tabby_ui", 32768, nullptr, 4, nullptr, 1) == pdPASS;
}

}  // namespace tabby
