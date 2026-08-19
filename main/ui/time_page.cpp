#include "time_page.hpp"

#include "tabby/app.hpp"
#include "wifi_page.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace tabby {
namespace time_ui {
namespace {

constexpr char kTag[] = "tabby_time_ui";
constexpr uint32_t kScreenRgb = 0x0B1220;
constexpr uint32_t kCardRgb = 0x1B2838;
constexpr uint32_t kTextRgb = 0xE8EEF4;
constexpr uint32_t kMutedRgb = 0x8A97A8;
constexpr uint32_t kBorderRgb = 0x2A3A4E;
constexpr uint32_t kAccentRgb = 0x3D8BFF;
constexpr uint32_t kSuccessRgb = 0x4ADE80;
constexpr int kMinOffsetMinutes = -12 * 60;
constexpr int kMaxOffsetMinutes = 14 * 60;
constexpr int kOffsetStepMinutes = 15;

enum class WorkKind : uint8_t { None, Sync, DetectAndSync };

struct WorkResult {
    WorkKind kind{WorkKind::None};
    bool zone_ok{false};
    bool sync_ok{false};
    int16_t offset_minutes{0};
    char region[64]{};
};

App* g_app = nullptr;
lv_obj_t* g_page = nullptr;
lv_obj_t* g_current = nullptr;
lv_obj_t* g_zone_name = nullptr;
lv_obj_t* g_auto_switch = nullptr;
lv_obj_t* g_zone_dropdown = nullptr;
lv_obj_t* g_sync_button = nullptr;
lv_obj_t* g_status = nullptr;
std::atomic<bool> g_working{false};
std::atomic<bool> g_result_ready{false};
WorkResult g_result;
bool g_auto_attempted = false;
int64_t g_last_clock_second = -1;
std::string g_offset_options;

std::string formatOffset(int minutes) {
    const char sign = minutes < 0 ? '-' : '+';
    const int absolute = minutes < 0 ? -minutes : minutes;
    char text[16];
    std::snprintf(text, sizeof(text), "UTC%c%02d:%02d", sign, absolute / 60, absolute % 60);
    return text;
}

void buildOffsetOptions() {
    if (!g_offset_options.empty()) return;
    for (int minutes = kMinOffsetMinutes; minutes <= kMaxOffsetMinutes; minutes += kOffsetStepMinutes) {
        if (!g_offset_options.empty()) g_offset_options.push_back('\n');
        g_offset_options += formatOffset(minutes);
    }
}

void styleCard(lv_obj_t* object) {
    lv_obj_set_width(object, LV_PCT(100));
    lv_obj_set_style_bg_color(object, lv_color_hex(kCardRgb), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 1, 0);
    lv_obj_set_style_border_color(object, lv_color_hex(kBorderRgb), 0);
    lv_obj_set_style_radius(object, 14, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 16, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t* makeLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

void setStatus(const char* text, bool success = false) {
    if (g_status == nullptr) return;
    lv_label_set_text(g_status, text);
    lv_obj_set_style_text_color(g_status, lv_color_hex(success ? kSuccessRgb : kMutedRgb), 0);
}

void refreshClock() {
    if (g_app == nullptr || g_current == nullptr) return;
    const std::string value = g_app->time.formatted(g_app->config.system);
    lv_label_set_text(g_current, value.empty() ? "Time not synchronized" : value.c_str());
}

void refreshZoneControls() {
    if (g_app == nullptr || g_zone_dropdown == nullptr) return;
    const auto& system = g_app->config.system;
    if (g_zone_name != nullptr) {
        const std::string offset = formatOffset(system.utcOffsetMinutes);
        if (system.region.empty() || system.region == offset) {
            lv_label_set_text(g_zone_name, offset.c_str());
        } else {
            lv_label_set_text_fmt(g_zone_name, "%s  ·  %s", system.region.c_str(), offset.c_str());
        }
    }
    if (g_auto_switch != nullptr) {
        if (system.timezoneAuto) lv_obj_add_state(g_auto_switch, LV_STATE_CHECKED);
        else lv_obj_clear_state(g_auto_switch, LV_STATE_CHECKED);
    }
    const int bounded = std::max(kMinOffsetMinutes, std::min(kMaxOffsetMinutes,
                                                             static_cast<int>(system.utcOffsetMinutes)));
    const int selected = (bounded - kMinOffsetMinutes + kOffsetStepMinutes / 2) / kOffsetStepMinutes;
    lv_dropdown_set_selected(g_zone_dropdown, static_cast<uint16_t>(selected));
    if (system.timezoneAuto) lv_obj_add_state(g_zone_dropdown, LV_STATE_DISABLED);
    else lv_obj_clear_state(g_zone_dropdown, LV_STATE_DISABLED);
}

void workTask(void* parameter) {
    const WorkKind kind = static_cast<WorkKind>(reinterpret_cast<uintptr_t>(parameter));
    WorkResult result;
    result.kind = kind;
    // Establish trusted wall-clock time before HTTPS certificate validation.
    // This matters on first boot, when the RTC may still be near the Unix epoch.
    if (g_app != nullptr) result.sync_ok = g_app->time.sync(10000);
    if (g_app != nullptr && kind == WorkKind::DetectAndSync) {
        std::string region;
        int16_t offset = 0;
        result.zone_ok = g_app->time.detectTimezone(region, offset);
        if (result.zone_ok) {
            result.offset_minutes = offset;
            std::snprintf(result.region, sizeof(result.region), "%s", region.c_str());
        }
    }
    g_result = result;
    g_result_ready.store(true, std::memory_order_release);
    g_working.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

bool startWork(WorkKind kind) {
    if (g_app == nullptr || !g_app->wifi.connected()) {
        setStatus("Connect Wi-Fi before synchronizing");
        return false;
    }
    bool expected = false;
    if (!g_working.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;
    setStatus(kind == WorkKind::DetectAndSync ? "Detecting time zone…" : "Synchronizing with NTP…");
    if (g_sync_button != nullptr) lv_obj_add_state(g_sync_button, LV_STATE_DISABLED);
    if (xTaskCreatePinnedToCore(workTask, "tabby_time_work", 12288,
                                reinterpret_cast<void*>(static_cast<uintptr_t>(kind)), 3, nullptr, 0) != pdPASS) {
        g_working.store(false, std::memory_order_release);
        if (g_sync_button != nullptr) lv_obj_clear_state(g_sync_button, LV_STATE_DISABLED);
        setStatus("Could not start time synchronization");
        return false;
    }
    return true;
}

void consumeResult() {
    if (!g_result_ready.exchange(false, std::memory_order_acq_rel) || g_app == nullptr) return;
    const WorkResult result = g_result;
    if (result.zone_ok) {
        g_app->config.system.region = result.region;
        g_app->config.system.utcOffsetMinutes = result.offset_minutes;
        if (!g_app->settings.save(g_app->config)) {
            ESP_LOGW(kTag, "Failed to persist detected time zone");
        }
    }
    if (g_sync_button != nullptr) lv_obj_clear_state(g_sync_button, LV_STATE_DISABLED);
    refreshZoneControls();
    refreshClock();

    if (result.kind == WorkKind::DetectAndSync) {
        if (result.zone_ok && result.sync_ok) setStatus("Time zone detected and time synchronized", true);
        else if (result.zone_ok) setStatus("Time zone detected; NTP sync failed");
        else if (result.sync_ok) setStatus("Time synchronized; zone detection failed");
        else setStatus("Time zone detection and NTP sync failed");
    } else if (result.sync_ok) {
        setStatus("Time synchronized", true);
    } else {
        setStatus("NTP synchronization failed");
    }
}

void onAutoChanged(lv_event_t*) {
    if (g_app == nullptr || g_auto_switch == nullptr) return;
    const bool enabled = lv_obj_has_state(g_auto_switch, LV_STATE_CHECKED);
    g_app->config.system.timezoneAuto = enabled;
    g_app->settings.save(g_app->config);
    g_auto_attempted = false;
    refreshZoneControls();
    setStatus(enabled ? "Automatic detection will use the public IP" : "Manual time zone selected");
}

void onZoneChanged(lv_event_t*) {
    if (g_app == nullptr || g_zone_dropdown == nullptr || g_app->config.system.timezoneAuto) return;
    const int minutes = kMinOffsetMinutes +
                        static_cast<int>(lv_dropdown_get_selected(g_zone_dropdown)) * kOffsetStepMinutes;
    g_app->config.system.utcOffsetMinutes = static_cast<int16_t>(minutes);
    g_app->config.system.region = formatOffset(minutes);
    g_app->settings.save(g_app->config);
    refreshZoneControls();
    refreshClock();
    setStatus("Time zone saved", true);
}

}  // namespace

void create(App& app, lv_obj_t* pane) {
    g_app = &app;
    buildOffsetOptions();

    g_page = lv_obj_create(pane);
    lv_obj_set_width(g_page, LV_PCT(100));
    lv_obj_set_height(g_page, 0);
    lv_obj_set_flex_grow(g_page, 1);
    lv_obj_set_style_bg_opa(g_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_page, 0, 0);
    lv_obj_set_style_radius(g_page, 0, 0);
    lv_obj_set_style_pad_all(g_page, 16, 0);
    lv_obj_set_style_pad_row(g_page, 12, 0);
    lv_obj_set_flex_flow(g_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(g_page, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(g_page, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* clock_card = lv_obj_create(g_page);
    styleCard(clock_card);
    lv_obj_set_height(clock_card, 94);
    makeLabel(clock_card, "Current local time", wifi_ui::font14(), kMutedRgb);
    g_current = makeLabel(clock_card, "Time not synchronized", wifi_ui::font20(), kTextRgb);
    lv_obj_align(g_current, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t* zone_card = lv_obj_create(g_page);
    styleCard(zone_card);
    lv_obj_set_height(zone_card, 186);
    makeLabel(zone_card, "Time zone", wifi_ui::font16(), kTextRgb);
    g_zone_name = makeLabel(zone_card, "UTC+00:00", wifi_ui::font14(), kMutedRgb);
    lv_obj_align(g_zone_name, LV_ALIGN_TOP_LEFT, 0, 30);
    lv_label_set_long_mode(g_zone_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_zone_name, LV_PCT(100));

    lv_obj_t* auto_label = makeLabel(zone_card, "Detect automatically", wifi_ui::font16(), kTextRgb);
    lv_obj_align(auto_label, LV_ALIGN_LEFT_MID, 0, 10);
    g_auto_switch = lv_switch_create(zone_card);
    lv_obj_set_size(g_auto_switch, 54, 28);
    lv_obj_align(g_auto_switch, LV_ALIGN_RIGHT_MID, 0, 10);
    lv_obj_set_style_bg_color(g_auto_switch, lv_color_hex(kBorderRgb), LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_auto_switch, lv_color_hex(kAccentRgb),
                              static_cast<lv_style_selector_t>(LV_PART_INDICATOR) |
                                  static_cast<lv_style_selector_t>(LV_STATE_CHECKED));
    lv_obj_add_event_cb(g_auto_switch, onAutoChanged, LV_EVENT_VALUE_CHANGED, nullptr);

    g_zone_dropdown = lv_dropdown_create(zone_card);
    lv_obj_set_size(g_zone_dropdown, LV_PCT(100), 46);
    lv_obj_align(g_zone_dropdown, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_dropdown_set_options(g_zone_dropdown, g_offset_options.c_str());
    lv_obj_set_style_bg_color(g_zone_dropdown, lv_color_hex(kScreenRgb), 0);
    lv_obj_set_style_bg_opa(g_zone_dropdown, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_zone_dropdown, lv_color_hex(kBorderRgb), 0);
    lv_obj_set_style_border_width(g_zone_dropdown, 1, 0);
    lv_obj_set_style_radius(g_zone_dropdown, 10, 0);
    lv_obj_set_style_text_font(g_zone_dropdown, wifi_ui::font16(), 0);
    lv_obj_set_style_text_color(g_zone_dropdown, lv_color_hex(kTextRgb), 0);
    lv_obj_add_event_cb(g_zone_dropdown, onZoneChanged, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* ntp_card = lv_obj_create(g_page);
    styleCard(ntp_card);
    lv_obj_set_height(ntp_card, 148);
    makeLabel(ntp_card, "Network time (NTP)", wifi_ui::font16(), kTextRgb);
    lv_obj_t* server = makeLabel(ntp_card, app.config.system.ntpServer.c_str(), wifi_ui::font14(), kMutedRgb);
    lv_obj_align(server, LV_ALIGN_TOP_LEFT, 0, 30);
    lv_label_set_long_mode(server, LV_LABEL_LONG_DOT);
    lv_obj_set_width(server, LV_PCT(55));

    g_sync_button = lv_btn_create(ntp_card);
    lv_obj_set_size(g_sync_button, 180, 48);
    lv_obj_align(g_sync_button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(g_sync_button, lv_color_hex(kAccentRgb), 0);
    lv_obj_set_style_radius(g_sync_button, 12, 0);
    lv_obj_set_style_shadow_width(g_sync_button, 0, 0);
    lv_obj_t* sync_label = makeLabel(g_sync_button, LV_SYMBOL_REFRESH "  Sync now", wifi_ui::font16(), kTextRgb);
    lv_obj_center(sync_label);
    lv_obj_add_event_cb(g_sync_button, [](lv_event_t*) { startWork(WorkKind::Sync); }, LV_EVENT_CLICKED, nullptr);

    g_status = makeLabel(g_page, "Ready", wifi_ui::font14(), kMutedRgb);
    lv_obj_set_width(g_status, LV_PCT(100));
    lv_label_set_long_mode(g_status, LV_LABEL_LONG_WRAP);
    refresh();
}

void setVisible(bool visible) {
    if (g_page == nullptr) return;
    if (visible) {
        lv_obj_clear_flag(g_page, LV_OBJ_FLAG_HIDDEN);
        refresh();
    } else {
        lv_obj_add_flag(g_page, LV_OBJ_FLAG_HIDDEN);
    }
}

void refresh() {
    refreshClock();
    refreshZoneControls();
}

void poll() {
    if (g_app == nullptr || g_page == nullptr) return;
    consumeResult();

    if (g_app->config.system.timezoneAuto && !g_auto_attempted && !g_working.load(std::memory_order_acquire) &&
        g_app->wifi.connected()) {
        g_auto_attempted = true;
        startWork(WorkKind::DetectAndSync);
    }

    const int64_t second = esp_timer_get_time() / 1000000;
    if (second != g_last_clock_second) {
        g_last_clock_second = second;
        refreshClock();
    }
}

}  // namespace time_ui
}  // namespace tabby
