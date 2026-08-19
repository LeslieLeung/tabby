#include "system_page.hpp"
#include "wifi_page.hpp"

#include "tabby/app.hpp"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_timer.h"

#include <cstdint>

namespace tabby {
namespace system_ui {
namespace {

constexpr uint32_t kCardRgb = 0x1B2838;
constexpr uint32_t kTextRgb = 0xE8EEF4;
constexpr uint32_t kMutedRgb = 0x8A97A8;
constexpr uint32_t kBorderRgb = 0x2A3A4E;
constexpr uint32_t kChargeRgb = 0x7DD3FC;
constexpr uint32_t kLowBattRgb = 0xFF5C5C;

App* g_app = nullptr;
lv_obj_t* g_page = nullptr;
lv_obj_t* g_name = nullptr;
lv_obj_t* g_board = nullptr;
lv_obj_t* g_chip = nullptr;
lv_obj_t* g_idf = nullptr;
lv_obj_t* g_flash = nullptr;
lv_obj_t* g_batt_level = nullptr;
lv_obj_t* g_batt_status = nullptr;
lv_obj_t* g_batt_voltage = nullptr;
lv_obj_t* g_batt_current = nullptr;
int64_t g_last_battery_second = -1;

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

lv_obj_t* addInfoRow(lv_obj_t* parent, const char* title) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 36);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title_label = makeLabel(row, title, wifi_ui::font16(), kMutedRgb);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* value = makeLabel(row, "", wifi_ui::font16(), kTextRgb);
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_obj_set_width(value, LV_PCT(62));
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    return value;
}

const char* chipModel(esp_chip_model_t model) {
    if (model == CHIP_ESP32P4) return "ESP32-P4";
    return "Unknown";
}

void refreshBattery() {
    if (g_app == nullptr) return;

    if (!g_app->bsp.batteryPresent()) {
        if (g_batt_level != nullptr) {
            lv_obj_set_style_text_color(g_batt_level, lv_color_hex(kMutedRgb), 0);
            lv_label_set_text(g_batt_level, "n/a");
        }
        if (g_batt_status != nullptr) lv_label_set_text(g_batt_status, "Not present");
        if (g_batt_voltage != nullptr) lv_label_set_text(g_batt_voltage, "n/a");
        if (g_batt_current != nullptr) lv_label_set_text(g_batt_current, "n/a");
        return;
    }

    const int percent = g_app->bsp.batteryPercent();
    const bool charging = g_app->bsp.batteryCharging();
    const int voltage_mv = g_app->bsp.batteryVoltageMv();
    const int current_ma = g_app->bsp.batteryCurrentMa();

    if (g_batt_level != nullptr) {
        uint32_t color = kTextRgb;
        if (charging) color = kChargeRgb;
        else if (percent <= 15) color = kLowBattRgb;
        lv_obj_set_style_text_color(g_batt_level, lv_color_hex(color), 0);
        lv_label_set_text_fmt(g_batt_level, "%d%%", percent);
    }
    if (g_batt_status != nullptr) {
        const char* status = "Idle";
        if (charging) status = "Charging";
        else if (current_ma <= -15) status = "Discharging";
        lv_label_set_text(g_batt_status, status);
    }
    if (g_batt_voltage != nullptr) {
        if (voltage_mv > 0) {
            lv_label_set_text_fmt(g_batt_voltage, "%d.%02d V", voltage_mv / 1000, (voltage_mv % 1000) / 10);
        } else {
            lv_label_set_text(g_batt_voltage, "n/a");
        }
    }
    if (g_batt_current != nullptr) {
        lv_label_set_text_fmt(g_batt_current, "%+d mA", current_ma);
    }
}

}  // namespace

void create(App& app, lv_obj_t* pane) {
    g_app = &app;

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

    lv_obj_t* device_card = lv_obj_create(g_page);
    styleCard(device_card);
    lv_obj_set_height(device_card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(device_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(device_card, 4, 0);

    makeLabel(device_card, "Device", wifi_ui::font16(), kTextRgb);
    g_name = addInfoRow(device_card, "Name");
    g_board = addInfoRow(device_card, "Board");
    g_chip = addInfoRow(device_card, "Chip");
    g_idf = addInfoRow(device_card, "IDF");
    g_flash = addInfoRow(device_card, "Flash");

    lv_obj_t* battery_card = lv_obj_create(g_page);
    styleCard(battery_card);
    lv_obj_set_height(battery_card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(battery_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(battery_card, 4, 0);

    makeLabel(battery_card, "Battery", wifi_ui::font16(), kTextRgb);
    g_batt_level = addInfoRow(battery_card, "Level");
    g_batt_status = addInfoRow(battery_card, "Status");
    g_batt_voltage = addInfoRow(battery_card, "Voltage");
    g_batt_current = addInfoRow(battery_card, "Current");
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
    if (g_app == nullptr) return;
    if (g_name != nullptr) {
        const char* name = g_app->config.system.deviceName.empty() ? "tabby" : g_app->config.system.deviceName.c_str();
        lv_label_set_text(g_name, name);
    }
    if (g_board != nullptr) lv_label_set_text(g_board, g_app->bsp.boardName());

    esp_chip_info_t info{};
    esp_chip_info(&info);
    if (g_chip != nullptr) {
        lv_label_set_text_fmt(g_chip, "%s  rev%u.%02u  %u cores", chipModel(info.model),
                              static_cast<unsigned>(info.revision / 100),
                              static_cast<unsigned>(info.revision % 100), static_cast<unsigned>(info.cores));
    }
    if (g_idf != nullptr) lv_label_set_text(g_idf, esp_get_idf_version());

    uint32_t flash_bytes = 0;
    if (g_flash != nullptr) {
        if (esp_flash_get_size(nullptr, &flash_bytes) == ESP_OK && flash_bytes > 0) {
            lv_label_set_text_fmt(g_flash, "%u MB", static_cast<unsigned>(flash_bytes / (1024 * 1024)));
        } else {
            lv_label_set_text(g_flash, "n/a");
        }
    }
    refreshBattery();
}

void poll() {
    if (g_app == nullptr || g_page == nullptr) return;
    if (lv_obj_has_flag(g_page, LV_OBJ_FLAG_HIDDEN)) return;

    const int64_t second = esp_timer_get_time() / 1000000;
    if (second == g_last_battery_second) return;
    g_last_battery_second = second;
    refreshBattery();
}

}  // namespace system_ui
}  // namespace tabby
