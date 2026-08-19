#include "wifi_page.hpp"
#include "cjk_font.hpp"

#include "tabby/app.hpp"

#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tabby {
namespace wifi_ui {
namespace {

constexpr uint32_t kScreenRgb = 0x0B1220;
constexpr uint32_t kCardRgb = 0x1B2838;
constexpr uint32_t kTextRgb = 0xE8EEF4;
constexpr uint32_t kMutedRgb = 0x8A97A8;
constexpr uint32_t kBorderRgb = 0x2A3A4E;
constexpr uint32_t kAccentRgb = 0x3D8BFF;
constexpr uint32_t kDangerRgb = 0xFF5C5C;
constexpr uint32_t kWifiOnRgb = 0x4ADE80;
constexpr size_t kSsidBytes = 32;
constexpr size_t kPasswordBytes = 63;

App* g_app = nullptr;
lv_font_t g_font14{};
lv_font_t g_font16{};
lv_font_t g_font20{};
lv_obj_t* g_page = nullptr;
lv_obj_t* g_status = nullptr;
lv_obj_t* g_toggle = nullptr;
lv_obj_t* g_scan = nullptr;
lv_obj_t* g_actions = nullptr;
lv_obj_t* g_search = nullptr;
lv_obj_t* g_list = nullptr;
lv_obj_t* g_modal = nullptr;
lv_obj_t* g_field_ssid = nullptr;
lv_obj_t* g_field_pass = nullptr;
lv_obj_t* g_focus = nullptr;
std::vector<ScannedAp> g_scan_copy;
std::string g_pending_ssid;
bool g_add_mode = false;
std::string g_last_status;
std::atomic<bool> g_toggle_working{false};
std::atomic<bool> g_toggle_result_ready{false};

std::string asciiLower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

bool matchesFilter(const std::string& ssid, const std::string& filter) {
    if (filter.empty()) return true;
    return asciiLower(ssid).find(asciiLower(filter)) != std::string::npos;
}

const char* authLabel(uint8_t auth) {
    switch (static_cast<wifi_auth_mode_t>(auth)) {
        case WIFI_AUTH_OPEN: return "Open";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK:
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA3_PSK:
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA3";
        default: return "Secured";
    }
}

int findProfile(const std::string& ssid) {
    if (g_app == nullptr) return -1;
    for (size_t i = 0; i < g_app->config.wifi.size(); ++i) {
        if (g_app->config.wifi[i].ssid == ssid) return static_cast<int>(i);
    }
    return -1;
}

size_t upsertProfile(const std::string& ssid, const std::string& password) {
    const int existing = findProfile(ssid);
    if (existing >= 0) {
        auto& profile = g_app->config.wifi[static_cast<size_t>(existing)];
        profile.password = password;
        if (profile.name.empty()) profile.name = ssid;
        return static_cast<size_t>(existing);
    }
    WifiProfile profile;
    profile.name = ssid;
    profile.ssid = ssid;
    profile.password = password;
    g_app->config.wifi.push_back(std::move(profile));
    return g_app->config.wifi.size() - 1;
}

void saveProfiles() {
    if (g_app == nullptr) return;
    g_app->settings.save(g_app->config);
}

std::string filterText() {
    if (g_search == nullptr) return {};
    const char* text = lv_textarea_get_text(g_search);
    return text ? text : "";
}

void styleCard(lv_obj_t* obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(kCardRgb), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 12, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_outline_width(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

void styleActionBtn(lv_obj_t* btn) {
    styleCard(btn);
    lv_obj_set_height(btn, 48);
    lv_obj_set_style_pad_hor(btn, 12, 0);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(btn, 6);
}

lv_obj_t* makeActionBtn(lv_obj_t* parent, const char* icon, const char* text) {
    lv_obj_t* btn = lv_btn_create(parent);
    styleActionBtn(btn);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(btn, 8, 0);

    lv_obj_t* icon_label = lv_label_create(btn);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_text_font(icon_label, font16(), 0);
    lv_obj_set_style_text_color(icon_label, lv_color_hex(kTextRgb), 0);

    lv_obj_t* text_label = lv_label_create(btn);
    lv_label_set_text(text_label, text);
    lv_obj_set_style_text_font(text_label, font16(), 0);
    lv_obj_set_style_text_color(text_label, lv_color_hex(kTextRgb), 0);
    return btn;
}

void styleTextarea(lv_obj_t* ta) {
    lv_obj_set_style_bg_color(ta, lv_color_hex(kCardRgb), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ta, 12, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(kBorderRgb), 0);
    lv_obj_set_style_text_font(ta, font16(), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(kTextRgb), 0);
    lv_obj_set_style_pad_hor(ta, 12, 0);
    lv_obj_set_style_pad_ver(ta, 10, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(kAccentRgb), LV_STATE_FOCUSED);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_cursor_click_pos(ta, true);
}

void limitBytes(lv_event_t* event, size_t max_bytes) {
    lv_obj_t* ta = lv_event_get_target(event);
    const auto* incoming = static_cast<const char*>(lv_event_get_param(event));
    if (ta == nullptr || incoming == nullptr) return;
    const char* current = lv_textarea_get_text(ta);
    const size_t have = current ? std::strlen(current) : 0;
    if (have + std::strlen(incoming) <= max_bytes) return;
    lv_textarea_set_insert_replace(ta, "");
}

void setFocus(lv_obj_t* ta) {
    if (g_focus != nullptr) lv_obj_clear_state(g_focus, LV_STATE_FOCUSED);
    g_focus = ta;
    if (g_focus != nullptr) {
        lv_obj_add_state(g_focus, LV_STATE_FOCUSED);
        lv_textarea_set_cursor_pos(g_focus, LV_TEXTAREA_CURSOR_LAST);
    }
}

void closeModal() {
    if (g_focus == g_field_ssid || g_focus == g_field_pass) g_focus = g_search;
    g_field_ssid = nullptr;
    g_field_pass = nullptr;
    if (g_modal != nullptr) {
        lv_obj_del(g_modal);
        g_modal = nullptr;
    }
    if (g_list != nullptr) lv_obj_clear_flag(g_list, LV_OBJ_FLAG_HIDDEN);
    if (g_search != nullptr) lv_obj_clear_flag(g_search, LV_OBJ_FLAG_HIDDEN);
    if (g_actions != nullptr) lv_obj_clear_flag(g_actions, LV_OBJ_FLAG_HIDDEN);
    g_pending_ssid.clear();
    g_add_mode = false;
}

void refreshList();
void refreshChrome();

void toggleWorkTask(void* parameter) {
    const bool enabled = reinterpret_cast<uintptr_t>(parameter) != 0;
    if (g_app != nullptr) g_app->wifi.setEnabled(enabled);
    g_toggle_result_ready.store(true, std::memory_order_release);
    g_toggle_working.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

void startToggleWork(bool enabled) {
    bool expected = false;
    if (g_app == nullptr || !g_toggle_working.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    if (g_status != nullptr) lv_label_set_text(g_status, enabled ? "Turning Wi-Fi on…" : "Turning Wi-Fi off…");
    if (g_toggle != nullptr) lv_obj_add_state(g_toggle, LV_STATE_DISABLED);
    if (xTaskCreatePinnedToCore(toggleWorkTask, "tabby_wifi_toggle", 6144,
                                reinterpret_cast<void*>(static_cast<uintptr_t>(enabled)), 3, nullptr, 0) != pdPASS) {
        g_toggle_working.store(false, std::memory_order_release);
        if (g_toggle != nullptr) lv_obj_clear_state(g_toggle, LV_STATE_DISABLED);
        if (g_status != nullptr) lv_label_set_text(g_status, "Could not start Wi-Fi operation");
    }
}

void connectProfile(size_t index) {
    if (g_app == nullptr || index >= g_app->config.wifi.size()) return;
    g_app->config.activeWifi = index;
    saveProfiles();
    g_app->wifi.startConnect(g_app->config.wifi[index]);
    g_app->cli.appendLine(std::string("wifi: ") + g_app->wifi.status());
    refreshChrome();
}

void connectSsid(const std::string& ssid, const std::string& password) {
    const size_t index = upsertProfile(ssid, password);
    connectProfile(index);
}

void submitModal() {
    if (g_app == nullptr) return;
    if (g_add_mode) {
        const char* ssid_text = g_field_ssid ? lv_textarea_get_text(g_field_ssid) : "";
        const char* pass_text = g_field_pass ? lv_textarea_get_text(g_field_pass) : "";
        std::string ssid = ssid_text ? ssid_text : "";
        if (ssid.empty()) return;
        connectSsid(ssid, pass_text ? pass_text : "");
        closeModal();
        refreshList();
        return;
    }
    const char* pass_text = g_field_pass ? lv_textarea_get_text(g_field_pass) : "";
    connectSsid(g_pending_ssid, pass_text ? pass_text : "");
    closeModal();
    refreshList();
}

void openModal(const char* title, bool add_mode, const std::string& ssid) {
    closeModal();
    g_add_mode = add_mode;
    g_pending_ssid = ssid;
    if (g_list != nullptr) lv_obj_add_flag(g_list, LV_OBJ_FLAG_HIDDEN);
    if (g_search != nullptr) lv_obj_add_flag(g_search, LV_OBJ_FLAG_HIDDEN);
    if (g_actions != nullptr) lv_obj_add_flag(g_actions, LV_OBJ_FLAG_HIDDEN);

    g_modal = lv_obj_create(g_page);
    lv_obj_remove_style_all(g_modal);
    lv_obj_set_size(g_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_modal, lv_color_hex(kScreenRgb), 0);
    lv_obj_set_style_bg_opa(g_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_modal, 0, 0);
    lv_obj_set_style_pad_all(g_modal, 8, 0);
    lv_obj_set_flex_grow(g_modal, 1);
    lv_obj_clear_flag(g_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = lv_obj_create(g_modal);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(kCardRgb), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* heading = lv_label_create(card);
    lv_label_set_text(heading, title);
    lv_obj_set_style_text_font(heading, font20(), 0);
    lv_obj_set_style_text_color(heading, lv_color_hex(kTextRgb), 0);
    lv_label_set_long_mode(heading, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(heading, LV_PCT(100));

    auto addField = [&](const char* placeholder, bool password) {
        lv_obj_t* ta = lv_textarea_create(card);
        lv_obj_set_width(ta, LV_PCT(100));
        lv_obj_set_height(ta, 48);
        styleTextarea(ta);
        lv_textarea_set_placeholder_text(ta, placeholder);
        lv_textarea_set_password_mode(ta, password);
        lv_obj_add_event_cb(
            ta,
            [](lv_event_t* event) {
                if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
                    setFocus(lv_event_get_target(event));
                } else if (lv_event_get_code(event) == LV_EVENT_INSERT) {
                    const bool pass = lv_event_get_target(event) == g_field_pass;
                    limitBytes(event, pass ? kPasswordBytes : kSsidBytes);
                }
            },
            LV_EVENT_ALL, nullptr);
        return ta;
    };

    if (add_mode) {
        g_field_ssid = addField("SSID", false);
        if (!ssid.empty()) lv_textarea_set_text(g_field_ssid, ssid.c_str());
        g_field_pass = addField("Password  (empty if open)", true);
        setFocus(g_field_ssid);
    } else {
        g_field_ssid = nullptr;
        g_field_pass = addField("Password", true);
        setFocus(g_field_pass);
    }

    lv_obj_t* actions = lv_obj_create(card);
    lv_obj_set_width(actions, LV_PCT(100));
    lv_obj_set_height(actions, 48);
    lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actions, 0, 0);
    lv_obj_set_style_pad_all(actions, 0, 0);
    lv_obj_set_style_pad_column(actions, 10, 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* cancel = makeActionBtn(actions, LV_SYMBOL_CLOSE, "Cancel");
    lv_obj_add_event_cb(cancel, [](lv_event_t*) { closeModal(); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* ok = makeActionBtn(actions, LV_SYMBOL_OK, add_mode ? "Save & connect" : "Connect");
    lv_obj_set_style_bg_color(ok, lv_color_hex(kAccentRgb), 0);
    lv_obj_add_event_cb(ok, [](lv_event_t*) { submitModal(); }, LV_EVENT_CLICKED, nullptr);
}

void promptForNetwork(const std::string& ssid, bool open_network, bool add_mode) {
    if (open_network && !add_mode) {
        connectSsid(ssid, "");
        refreshList();
        return;
    }
    char title[96];
    if (add_mode) {
        std::snprintf(title, sizeof(title), "Add network");
    } else {
        std::snprintf(title, sizeof(title), "Connect  %s", ssid.c_str());
    }
    openModal(title, add_mode, ssid);
}

void onSavedConnect(lv_event_t* event) {
    if (g_app == nullptr) return;
    const size_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (index >= g_app->config.wifi.size()) return;
    const auto& profile = g_app->config.wifi[index];
    if (profile.password.empty()) {
        const auto scan = std::find_if(g_scan_copy.begin(), g_scan_copy.end(),
                                       [&](const ScannedAp& ap) { return ap.ssid == profile.ssid; });
        if (scan != g_scan_copy.end() && !scan->open) {
            promptForNetwork(profile.ssid, false, false);
            return;
        }
    }
    connectProfile(index);
}

void onSavedDelete(lv_event_t* event) {
    if (g_app == nullptr) return;
    const size_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (index >= g_app->config.wifi.size()) return;
    const std::string ssid = g_app->config.wifi[index].ssid;
    const bool connected = g_app->wifi.connected() && g_app->wifi.ssid() == ssid;
    g_app->config.wifi.erase(g_app->config.wifi.begin() + static_cast<std::ptrdiff_t>(index));
    if (g_app->config.activeWifi > index) {
        --g_app->config.activeWifi;
    } else if (g_app->config.activeWifi >= g_app->config.wifi.size()) {
        g_app->config.activeWifi = 0;
    }
    saveProfiles();
    if (connected) g_app->wifi.disconnect();
    g_app->cli.appendLine("wifi: deleted " + ssid);
    refresh();
}

void onScanConnect(lv_event_t* event) {
    const size_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (index >= g_scan_copy.size()) return;
    const ScannedAp& ap = g_scan_copy[index];
    const int existing = findProfile(ap.ssid);
    if (existing >= 0 && (ap.open || !g_app->config.wifi[static_cast<size_t>(existing)].password.empty())) {
        connectProfile(static_cast<size_t>(existing));
        return;
    }
    promptForNetwork(ap.ssid, ap.open, false);
}

lv_obj_t* addHeading(lv_obj_t* list, const char* text) {
    lv_obj_t* heading = lv_label_create(list);
    lv_label_set_text(heading, text);
    lv_obj_set_style_text_font(heading, font14(), 0);
    lv_obj_set_style_text_color(heading, lv_color_hex(kMutedRgb), 0);
    lv_obj_set_style_pad_top(heading, 6, 0);
    lv_obj_set_width(heading, LV_PCT(100));
    return heading;
}

lv_obj_t* addNetworkRow(lv_obj_t* list, const char* icon, const char* title, const char* detail, bool active,
                        bool clickable) {
    lv_obj_t* row = lv_btn_create(list);
    styleCard(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 52);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(kAccentRgb), LV_STATE_PRESSED);
    if (!clickable) lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* icon_label = lv_label_create(row);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_text_font(icon_label, font16(), 0);
    lv_obj_set_style_text_color(icon_label, lv_color_hex(active ? kWifiOnRgb : kTextRgb), 0);

    lv_obj_t* title_label = lv_label_create(row);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, font16(), 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(kTextRgb), 0);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_flex_grow(title_label, 1);

    if (detail != nullptr && detail[0]) {
        lv_obj_t* detail_label = lv_label_create(row);
        lv_label_set_text(detail_label, detail);
        lv_obj_set_style_text_font(detail_label, font14(), 0);
        lv_obj_set_style_text_color(detail_label, lv_color_hex(kMutedRgb), 0);
        lv_label_set_long_mode(detail_label, LV_LABEL_LONG_CLIP);
    }
    return row;
}

lv_obj_t* addDeleteBtn(lv_obj_t* row, size_t index) {
    lv_obj_t* del = lv_btn_create(row);
    lv_obj_remove_style_all(del);
    lv_obj_set_size(del, 40, 40);
    lv_obj_set_style_bg_color(del, lv_color_hex(0x2A1C24), 0);
    lv_obj_set_style_bg_opa(del, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(del, 10, 0);
    lv_obj_clear_flag(del, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* trash = lv_label_create(del);
    lv_label_set_text(trash, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_font(trash, font16(), 0);
    lv_obj_set_style_text_color(trash, lv_color_hex(kDangerRgb), 0);
    lv_obj_center(trash);
    lv_obj_add_event_cb(del, onSavedDelete, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(index)));
    return del;
}

void refreshList() {
    if (g_app == nullptr || g_list == nullptr) return;
    lv_obj_clean(g_list);
    const std::string filter = filterText();
    const std::string current = g_app->wifi.ssid();

    addHeading(g_list, "Saved");
    size_t saved_shown = 0;
    for (size_t i = 0; i < g_app->config.wifi.size(); ++i) {
        const auto& profile = g_app->config.wifi[i];
        if (!matchesFilter(profile.ssid, filter) && !matchesFilter(profile.name, filter)) continue;
        ++saved_shown;
        const bool active = g_app->wifi.connected() && current == profile.ssid;
        lv_obj_t* row = addNetworkRow(g_list, active ? LV_SYMBOL_OK : LV_SYMBOL_WIFI, profile.ssid.c_str(),
                                      active ? "Connected" : nullptr, active, true);
        lv_obj_add_event_cb(row, onSavedConnect, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
        addDeleteBtn(row, i);
    }
    if (saved_shown == 0) {
        addNetworkRow(g_list, LV_SYMBOL_WIFI, filter.empty() ? "No saved networks" : "No matching saved networks",
                      nullptr, false, false);
    }

    g_scan_copy = g_app->wifi.scanResults();
    addHeading(g_list, g_app->wifi.scanning() ? "Nearby  (scanning...)" : "Nearby");
    size_t nearby_shown = 0;
    for (size_t i = 0; i < g_scan_copy.size(); ++i) {
        const auto& ap = g_scan_copy[i];
        if (!matchesFilter(ap.ssid, filter)) continue;
        if (findProfile(ap.ssid) >= 0) continue;
        ++nearby_shown;
        char detail[48];
        std::snprintf(detail, sizeof(detail), "%d dBm  %s", static_cast<int>(ap.rssi), authLabel(ap.auth));
        lv_obj_t* btn = addNetworkRow(g_list, LV_SYMBOL_WIFI, ap.ssid.c_str(), detail, false, true);
        lv_obj_add_event_cb(btn, onScanConnect, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
    }
    if (!g_app->wifi.scanning() && nearby_shown == 0) {
        const char* empty = g_scan_copy.empty() ? "Tap Scan to search for networks" : "No matching networks";
        addNetworkRow(g_list, LV_SYMBOL_REFRESH, empty, nullptr, false, false);
    }
}

void refreshChrome() {
    if (g_app == nullptr || g_status == nullptr) return;
    char status[160];
    if (g_app->wifi.connecting()) {
        std::snprintf(status, sizeof(status), "Connecting  %s", g_app->wifi.ssid().c_str());
    } else if (g_app->wifi.connected()) {
        std::snprintf(status, sizeof(status), "Connected  %s  %s", g_app->wifi.ssid().c_str(),
                      g_app->wifi.ip().c_str());
    } else if (!g_app->wifi.enabled()) {
        std::snprintf(status, sizeof(status), "Wi-Fi is off");
    } else if (g_app->wifi.scanning()) {
        std::snprintf(status, sizeof(status), "Scanning...");
    } else {
        std::snprintf(status, sizeof(status), "%s", g_app->wifi.status().c_str());
    }
    lv_label_set_text(g_status, status);
    g_last_status = g_app->wifi.status();

    if (g_toggle != nullptr) {
        lv_obj_t* icon = lv_obj_get_child(g_toggle, 0);
        lv_obj_t* label = lv_obj_get_child(g_toggle, 1);
        if (icon != nullptr) lv_label_set_text(icon, LV_SYMBOL_POWER);
        if (label != nullptr) lv_label_set_text(label, g_app->wifi.enabled() ? "Off" : "On");
    }
    if (g_scan != nullptr) {
        lv_obj_t* label = lv_obj_get_child(g_scan, 1);
        if (label != nullptr) lv_label_set_text(label, g_app->wifi.scanning() ? "Scanning" : "Scan");
    }
}

}  // namespace

void initFonts() {
    CjkFontInit();
    g_font14 = lv_font_montserrat_14;
    g_font16 = lv_font_montserrat_16;
    g_font20 = lv_font_montserrat_20;
    g_font14.fallback = CjkFont16();
    g_font16.fallback = CjkFont16();
    g_font20.fallback = CjkFont16();
}

const lv_font_t* font14() { return &g_font14; }
const lv_font_t* font16() { return &g_font16; }
const lv_font_t* font20() { return &g_font20; }

void create(App& app, lv_obj_t* pane) {
    g_app = &app;
    g_page = lv_obj_create(pane);
    lv_obj_set_width(g_page, LV_PCT(100));
    lv_obj_set_height(g_page, 0);
    lv_obj_set_flex_grow(g_page, 1);
    lv_obj_set_style_bg_opa(g_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_page, 0, 0);
    lv_obj_set_style_radius(g_page, 0, 0);
    lv_obj_set_style_pad_all(g_page, 12, 0);
    lv_obj_set_style_pad_row(g_page, 10, 0);
    lv_obj_set_flex_flow(g_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(g_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_page, LV_OBJ_FLAG_SCROLLABLE);

    g_status = lv_label_create(g_page);
    lv_obj_set_style_text_font(g_status, font16(), 0);
    lv_obj_set_style_text_color(g_status, lv_color_hex(kMutedRgb), 0);
    lv_label_set_long_mode(g_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_status, LV_PCT(100));
    lv_label_set_text(g_status, "Wi-Fi");

    lv_obj_t* actions = lv_obj_create(g_page);
    g_actions = actions;
    lv_obj_set_width(actions, LV_PCT(100));
    lv_obj_set_height(actions, 48);
    lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actions, 0, 0);
    lv_obj_set_style_pad_all(actions, 0, 0);
    lv_obj_set_style_pad_column(actions, 8, 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);

    g_toggle = makeActionBtn(actions, LV_SYMBOL_POWER, "Off");
    lv_obj_add_event_cb(
        g_toggle,
        [](lv_event_t*) {
            if (g_app == nullptr) return;
            startToggleWork(!g_app->wifi.enabled());
        },
        LV_EVENT_CLICKED, nullptr);

    g_scan = makeActionBtn(actions, LV_SYMBOL_REFRESH, "Scan");
    lv_obj_add_event_cb(
        g_scan,
        [](lv_event_t*) {
            if (g_app == nullptr) return;
            if (!g_app->wifi.enabled()) {
                g_app->cli.appendLine("wifi: turn Wi-Fi on first");
                refreshChrome();
                return;
            }
            if (!g_app->wifi.startScan()) {
                g_app->cli.appendLine(std::string("wifi: ") + g_app->wifi.status());
            }
            refresh();
        },
        LV_EVENT_CLICKED, nullptr);

    lv_obj_t* add = makeActionBtn(actions, LV_SYMBOL_PLUS, "Add");
    lv_obj_add_event_cb(
        add,
        [](lv_event_t*) {
            if (g_app == nullptr) return;
            promptForNetwork("", false, true);
        },
        LV_EVENT_CLICKED, nullptr);

    g_search = lv_textarea_create(g_page);
    lv_obj_set_width(g_search, LV_PCT(100));
    lv_obj_set_height(g_search, 48);
    styleTextarea(g_search);
    lv_textarea_set_placeholder_text(g_search, "Search networks");
    lv_obj_add_event_cb(
        g_search,
        [](lv_event_t*) { setFocus(g_search); },
        LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(
        g_search,
        [](lv_event_t*) { refreshList(); },
        LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(
        g_search,
        [](lv_event_t* event) { limitBytes(event, kSsidBytes); },
        LV_EVENT_INSERT, nullptr);

    g_list = lv_obj_create(g_page);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_width(g_list, LV_PCT(100));
    lv_obj_set_height(g_list, 0);
    lv_obj_set_flex_grow(g_list, 1);
    lv_obj_set_style_bg_color(g_list, lv_color_hex(kScreenRgb), 0);
    lv_obj_set_style_bg_opa(g_list, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_row(g_list, 6, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
}

void setVisible(bool visible) {
    if (g_page == nullptr) return;
    if (visible) {
        lv_obj_clear_flag(g_page, LV_OBJ_FLAG_HIDDEN);
        refresh();
    } else {
        closeModal();
        if (g_focus == g_search) g_focus = nullptr;
        lv_obj_add_flag(g_page, LV_OBJ_FLAG_HIDDEN);
    }
}

void refresh() {
    refreshChrome();
    refreshList();
}

void poll() {
    if (g_toggle_result_ready.exchange(false, std::memory_order_acq_rel) && g_app != nullptr) {
        g_app->cli.appendLine(g_app->wifi.status());
        if (g_toggle != nullptr) lv_obj_clear_state(g_toggle, LV_STATE_DISABLED);
        refreshChrome();
    }
    if (g_page == nullptr || lv_obj_has_flag(g_page, LV_OBJ_FLAG_HIDDEN) || g_app == nullptr) return;
    const bool scan_update = g_app->wifi.consumeScanUpdate();
    const std::string status = g_app->wifi.status();
    if (status != g_last_status) refreshChrome();
    if (scan_update) refreshList();
}

bool handleKey(const KeyAction& action) {
    if (g_page == nullptr || lv_obj_has_flag(g_page, LV_OBJ_FLAG_HIDDEN)) return false;
    if (action.type == KeyActionType::Menu) {
        if (g_modal != nullptr) {
            closeModal();
            return true;
        }
        return false;
    }
    if (action.type != KeyActionType::Text) return false;
    lv_obj_t* ta = g_modal != nullptr ? (g_focus ? g_focus : g_field_pass) : g_focus;
    if (ta == nullptr) return g_modal != nullptr;
    if (action.text == "\r" || action.text == "\n") {
        if (g_modal != nullptr) submitModal();
        return true;
    }
    if (action.text == "\t") {
        if (g_modal != nullptr && g_field_ssid != nullptr && g_field_pass != nullptr) {
            setFocus(g_focus == g_field_ssid ? g_field_pass : g_field_ssid);
        }
        return true;
    }
    if (action.text.size() == 1 && (action.text[0] == '\b' || action.text[0] == 0x7F)) {
        lv_textarea_del_char(ta);
        return true;
    }
    if (!action.text.empty() && action.text[0] == 0x1B) {
        if (action.text == "\x1B[C") lv_textarea_cursor_right(ta);
        else if (action.text == "\x1B[D") lv_textarea_cursor_left(ta);
        return true;
    }
    lv_textarea_add_text(ta, action.text.c_str());
    return true;
}

}  // namespace wifi_ui
}  // namespace tabby
