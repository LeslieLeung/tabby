#include "appearance_page.hpp"
#include "wifi_page.hpp"

#include "tabby/app.hpp"

#include <cstddef>
#include <cstdint>

namespace tabby {
namespace appearance_ui {
namespace {

constexpr uint32_t kScreenRgb = 0x0B1220;
constexpr uint32_t kCardRgb = 0x1B2838;
constexpr uint32_t kTextRgb = 0xE8EEF4;
constexpr uint32_t kMutedRgb = 0x8A97A8;
constexpr uint32_t kBorderRgb = 0x2A3A4E;
constexpr uint32_t kAccentRgb = 0x3D8BFF;

struct FontOption {
    const char* label;
    uint8_t height;
};

constexpr FontOption kFontOptions[] = {
    {"Small  (20 px)", 20},
    {"Medium  (24 px)", 24},
    {"Large  (28 px)", 28},
    {"Extra large  (32 px)", 32},
    {"Huge  (36 px)", 36},
};
constexpr size_t kFontOptionCount = sizeof(kFontOptions) / sizeof(kFontOptions[0]);

App* g_app = nullptr;
lv_obj_t* g_page = nullptr;
lv_obj_t* g_current = nullptr;
lv_obj_t* g_brightness_value = nullptr;
lv_obj_t* g_brightness_slider = nullptr;
lv_obj_t* g_option_btns[kFontOptionCount] = {};
lv_obj_t* g_option_checks[kFontOptionCount] = {};
void (*g_apply_font)(uint8_t height) = nullptr;
bool g_updating_brightness = false;

bool optionSelected(size_t index, uint8_t current) {
    const uint8_t hi = kFontOptions[index].height;
    if (index == 0) return current <= hi;
    const uint8_t lo = kFontOptions[index - 1].height;
    if (index + 1 == kFontOptionCount) return current > lo;
    return current > lo && current <= hi;
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

const char* currentLabel(uint8_t height) {
    for (size_t i = 0; i < kFontOptionCount; ++i) {
        if (optionSelected(i, height)) return kFontOptions[i].label;
    }
    return kFontOptions[kFontOptionCount - 1].label;
}

void styleOption(lv_obj_t* btn, bool selected) {
    lv_obj_set_style_bg_color(btn, lv_color_hex(selected ? kAccentRgb : kScreenRgb), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(selected ? kAccentRgb : kBorderRgb), 0);
}

void onOptionClicked(lv_event_t* event) {
    if (g_apply_font == nullptr) return;
    const uint8_t height = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    g_apply_font(height);
}

uint8_t clampBrightness(int value) {
    if (value < DisplayConfig::kMinBrightness) return DisplayConfig::kMinBrightness;
    if (value > DisplayConfig::kMaxBrightness) return DisplayConfig::kMaxBrightness;
    return static_cast<uint8_t>(value);
}

int brightnessPercent(uint8_t value) {
    return (static_cast<int>(value) * 100 + DisplayConfig::kMaxBrightness / 2) / DisplayConfig::kMaxBrightness;
}

void updateBrightnessLabel(uint8_t value) {
    if (g_brightness_value == nullptr) return;
    lv_label_set_text_fmt(g_brightness_value, "%d%%", brightnessPercent(value));
}

void applyBrightness(uint8_t value, bool save) {
    if (g_app == nullptr) return;
    value = clampBrightness(value);
    g_app->bsp.setBrightness(value);
    updateBrightnessLabel(value);
    const bool changed = g_app->config.display.brightness != value;
    g_app->config.display.brightness = value;
    if (save && changed) g_app->settings.save(g_app->config);
}

void onBrightnessChanged(lv_event_t* event) {
    if (g_updating_brightness) return;
    const int32_t value = lv_slider_get_value(lv_event_get_target(event));
    applyBrightness(static_cast<uint8_t>(value), false);
}

void onBrightnessReleased(lv_event_t* event) {
    if (g_updating_brightness) return;
    const int32_t value = lv_slider_get_value(lv_event_get_target(event));
    applyBrightness(static_cast<uint8_t>(value), true);
}

}  // namespace

void create(App& app, lv_obj_t* pane, void (*apply_font)(uint8_t height)) {
    g_app = &app;
    g_apply_font = apply_font;

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

    lv_obj_t* brightness_card = lv_obj_create(g_page);
    styleCard(brightness_card);
    lv_obj_set_height(brightness_card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(brightness_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(brightness_card, 10, 0);

    lv_obj_t* brightness_header = lv_obj_create(brightness_card);
    lv_obj_set_width(brightness_header, LV_PCT(100));
    lv_obj_set_height(brightness_header, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(brightness_header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(brightness_header, 0, 0);
    lv_obj_set_style_pad_all(brightness_header, 0, 0);
    lv_obj_set_style_radius(brightness_header, 0, 0);
    lv_obj_set_flex_flow(brightness_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brightness_header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(brightness_header, LV_OBJ_FLAG_SCROLLABLE);

    makeLabel(brightness_header, "Brightness", wifi_ui::font16(), kTextRgb);
    g_brightness_value = makeLabel(brightness_header, "", wifi_ui::font16(), kMutedRgb);

    g_brightness_slider = lv_slider_create(brightness_card);
    lv_obj_set_width(g_brightness_slider, LV_PCT(100));
    lv_obj_set_height(g_brightness_slider, 22);
    lv_obj_set_ext_click_area(g_brightness_slider, 16);
    lv_slider_set_range(g_brightness_slider, DisplayConfig::kMinBrightness, DisplayConfig::kMaxBrightness);
    lv_obj_set_style_bg_color(g_brightness_slider, lv_color_hex(kScreenRgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_brightness_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g_brightness_slider, 11, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_brightness_slider, lv_color_hex(kAccentRgb), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_brightness_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_brightness_slider, 11, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_brightness_slider, lv_color_hex(kTextRgb), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(g_brightness_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g_brightness_slider, 8, LV_PART_KNOB);
    lv_obj_set_style_border_width(g_brightness_slider, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(g_brightness_slider, onBrightnessChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(g_brightness_slider, onBrightnessReleased, LV_EVENT_RELEASED, nullptr);

    lv_obj_t* font_card = lv_obj_create(g_page);
    styleCard(font_card);
    lv_obj_set_height(font_card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(font_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(font_card, 10, 0);

    makeLabel(font_card, "Terminal font size", wifi_ui::font16(), kTextRgb);
    g_current = makeLabel(font_card, "", wifi_ui::font14(), kMutedRgb);
    lv_obj_set_width(g_current, LV_PCT(100));
    lv_label_set_long_mode(g_current, LV_LABEL_LONG_DOT);

    for (size_t i = 0; i < kFontOptionCount; ++i) {
        lv_obj_t* btn = lv_btn_create(font_card);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, 46);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_pad_hor(btn, 14, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_ext_click_area(btn, 4);

        lv_obj_t* label = makeLabel(btn, kFontOptions[i].label, wifi_ui::font16(), kTextRgb);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(label, 1);

        lv_obj_t* check = makeLabel(btn, LV_SYMBOL_OK, wifi_ui::font16(), kTextRgb);
        g_option_btns[i] = btn;
        g_option_checks[i] = check;
        lv_obj_add_event_cb(btn, onOptionClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<uintptr_t>(kFontOptions[i].height)));
    }

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
    const uint8_t brightness = clampBrightness(g_app->config.display.brightness);
    updateBrightnessLabel(brightness);
    if (g_brightness_slider != nullptr && !lv_slider_is_dragged(g_brightness_slider)) {
        g_updating_brightness = true;
        lv_slider_set_value(g_brightness_slider, brightness, LV_ANIM_OFF);
        g_updating_brightness = false;
    }

    const uint8_t current = g_app->config.keyboard.terminalLineStep;
    if (g_current != nullptr) lv_label_set_text(g_current, currentLabel(current));
    for (size_t i = 0; i < kFontOptionCount; ++i) {
        const bool selected = optionSelected(i, current);
        if (g_option_btns[i] != nullptr) styleOption(g_option_btns[i], selected);
        if (g_option_checks[i] != nullptr) {
            lv_obj_set_style_text_opa(g_option_checks[i], selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
    }
}

}  // namespace appearance_ui
}  // namespace tabby
