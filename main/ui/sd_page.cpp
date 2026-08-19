#include "sd_page.hpp"

#include "tabby/app.hpp"
#include "wifi_page.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace tabby {
namespace sd_ui {
namespace {

constexpr char kTag[] = "tabby_sd_ui";
constexpr uint32_t kCardRgb = 0x1B2838;
constexpr uint32_t kTextRgb = 0xE8EEF4;
constexpr uint32_t kMutedRgb = 0x8A97A8;
constexpr uint32_t kBorderRgb = 0x2A3A4E;
constexpr uint32_t kAccentRgb = 0x3D8BFF;
constexpr uint32_t kSuccessRgb = 0x4ADE80;
constexpr uint32_t kDangerRgb = 0xE05252;

enum class WorkKind : uint8_t { None, Mount, Unmount, Format, UsbStart, UsbStop };

struct WorkResult {
    WorkKind kind{WorkKind::None};
    bool ok{false};
    char error[96]{};
};

App* g_app = nullptr;
lv_obj_t* g_page = nullptr;
lv_obj_t* g_state = nullptr;
lv_obj_t* g_details = nullptr;
lv_obj_t* g_status = nullptr;
lv_obj_t* g_refresh_button = nullptr;
lv_obj_t* g_mount_button = nullptr;
lv_obj_t* g_unmount_button = nullptr;
lv_obj_t* g_format_button = nullptr;
lv_obj_t* g_usb_state = nullptr;
lv_obj_t* g_usb_details = nullptr;
lv_obj_t* g_usb_connect_button = nullptr;
lv_obj_t* g_usb_disconnect_button = nullptr;
lv_obj_t* g_confirm = nullptr;
lv_obj_t* g_progress = nullptr;
std::atomic<bool> g_working{false};
std::atomic<bool> g_result_ready{false};
WorkResult g_result;
WorkKind g_current_kind{WorkKind::None};
bool g_last_usb_host{false};

std::string formatBytes(uint64_t bytes) {
    char text[24];
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        std::snprintf(text, sizeof(text), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(text, sizeof(text), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return text;
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

lv_obj_t* makeButton(lv_obj_t* parent, const char* text, uint32_t color) {
    lv_obj_t* button = lv_btn_create(parent);
    lv_obj_set_height(button, 48);
    lv_obj_set_flex_grow(button, 1);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_t* label = makeLabel(button, text, wifi_ui::font16(), kTextRgb);
    lv_obj_center(label);
    return button;
}

void setStatus(const char* text, bool success = false) {
    if (g_status == nullptr) return;
    lv_label_set_text(g_status, text);
    lv_obj_set_style_text_color(g_status, lv_color_hex(success ? kSuccessRgb : kMutedRgb), 0);
}

void setButtonEnabled(lv_obj_t* button, bool enabled) {
    if (button == nullptr) return;
    if (enabled) lv_obj_clear_state(button, LV_STATE_DISABLED);
    else lv_obj_add_state(button, LV_STATE_DISABLED);
}

void closeProgress() {
    if (g_progress == nullptr) return;
    lv_msgbox_close(g_progress);
    g_progress = nullptr;
}

void showProgress(WorkKind kind) {
    const char* title = "SD Card";
    const char* message = "Working…";
    if (kind == WorkKind::Mount) message = "Checking and mounting the card…";
    else if (kind == WorkKind::Unmount) message = "Unmounting the card…";
    else if (kind == WorkKind::Format) message = "Formatting as FAT…\nDo not remove the card or power off.";
    else if (kind == WorkKind::UsbStart) message = "Switching USB to drive mode…";
    else if (kind == WorkKind::UsbStop) message = "Stopping USB drive mode…";
    g_progress = lv_msgbox_create(nullptr, title, message, nullptr, false);
    lv_obj_set_width(g_progress, 520);
    lv_obj_set_style_bg_color(g_progress, lv_color_hex(kCardRgb), 0);
    lv_obj_set_style_text_color(g_progress, lv_color_hex(kTextRgb), 0);
    lv_obj_set_style_text_font(g_progress, wifi_ui::font16(), 0);
    lv_obj_center(g_progress);
}

void workTask(void* parameter) {
    const WorkKind kind = static_cast<WorkKind>(reinterpret_cast<uintptr_t>(parameter));
    WorkResult result;
    result.kind = kind;
    if (g_app != nullptr) {
        if (kind == WorkKind::Mount) result.ok = g_app->sd.mount();
        else if (kind == WorkKind::Unmount) result.ok = g_app->sd.unmount();
        else if (kind == WorkKind::Format) result.ok = g_app->sd.format();
        else if (kind == WorkKind::UsbStart) result.ok = g_app->usb_msc.start();
        else if (kind == WorkKind::UsbStop) result.ok = g_app->usb_msc.stop();
        if (!result.ok) {
            const std::string& error =
                (kind == WorkKind::UsbStart || kind == WorkKind::UsbStop) ? g_app->usb_msc.lastError()
                                                                         : g_app->sd.lastError();
            std::snprintf(result.error, sizeof(result.error), "%s", error.c_str());
        }
    }
    g_result = result;
    g_result_ready.store(true, std::memory_order_release);
    g_working.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

bool startWork(WorkKind kind) {
    bool expected = false;
    if (g_app == nullptr || !g_working.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
    }
    showProgress(kind);
    g_current_kind = kind;
    if (xTaskCreatePinnedToCore(workTask, "tabby_sd_work", 8192,
                                reinterpret_cast<void*>(static_cast<uintptr_t>(kind)), 3, nullptr, 0) != pdPASS) {
        g_working.store(false, std::memory_order_release);
        g_current_kind = WorkKind::None;
        closeProgress();
        setStatus("Could not start SD card operation");
        return false;
    }
    refresh();
    return true;
}

void onDisconnectConfirm(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED || g_confirm == nullptr) return;
    const char* selected = lv_msgbox_get_active_btn_text(g_confirm);
    const bool confirmed = selected != nullptr && std::strcmp(selected, "Disconnect") == 0;
    lv_msgbox_close(g_confirm);
    g_confirm = nullptr;
    if (confirmed) startWork(WorkKind::UsbStop);
}

void askDisconnectConfirmation() {
    if (g_confirm != nullptr || g_working.load(std::memory_order_acquire)) return;
    if (g_app == nullptr || !g_app->usb_msc.hostAttached()) {
        startWork(WorkKind::UsbStop);
        return;
    }
    static const char* buttons[] = {"Cancel", "Disconnect", ""};
    g_confirm = lv_msgbox_create(nullptr, "Disconnect USB drive?",
                                 "Eject the drive on the computer first. Disconnecting while the computer is writing can corrupt the card.",
                                 buttons, false);
    lv_obj_set_width(g_confirm, 560);
    lv_obj_set_style_bg_color(g_confirm, lv_color_hex(kCardRgb), 0);
    lv_obj_set_style_text_color(g_confirm, lv_color_hex(kTextRgb), 0);
    lv_obj_set_style_text_font(g_confirm, wifi_ui::font16(), 0);
    lv_obj_add_event_cb(g_confirm, onDisconnectConfirm, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_center(g_confirm);
}

void onFormatConfirm(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED || g_confirm == nullptr) return;
    const char* selected = lv_msgbox_get_active_btn_text(g_confirm);
    const bool confirmed = selected != nullptr && std::strcmp(selected, "Format") == 0;
    lv_msgbox_close(g_confirm);
    g_confirm = nullptr;
    if (confirmed) startWork(WorkKind::Format);
}

void askFormatConfirmation() {
    if (g_confirm != nullptr || g_working.load(std::memory_order_acquire)) return;
    static const char* buttons[] = {"Cancel", "Format", ""};
    g_confirm = lv_msgbox_create(nullptr, "Format SD card?",
                                 "All files on this card will be permanently erased.\nThe card will be formatted as FAT.",
                                 buttons, false);
    lv_obj_set_width(g_confirm, 560);
    lv_obj_set_style_bg_color(g_confirm, lv_color_hex(kCardRgb), 0);
    lv_obj_set_style_text_color(g_confirm, lv_color_hex(kTextRgb), 0);
    lv_obj_set_style_text_font(g_confirm, wifi_ui::font16(), 0);
    lv_obj_add_event_cb(g_confirm, onFormatConfirm, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_center(g_confirm);
}

void consumeResult() {
    if (!g_result_ready.exchange(false, std::memory_order_acq_rel)) return;
    closeProgress();
    g_current_kind = WorkKind::None;
    g_last_usb_host = g_app != nullptr && g_app->usb_msc.hostAttached();
    refresh();
    if (g_result.ok) {
        if (g_result.kind == WorkKind::Mount) setStatus("SD card mounted", true);
        else if (g_result.kind == WorkKind::Unmount) setStatus("SD card unmounted", true);
        else if (g_result.kind == WorkKind::Format) setStatus("SD card formatted and mounted", true);
        else if (g_result.kind == WorkKind::UsbStart) setStatus("USB drive mode on. Use the USB-C port.", true);
        else if (g_result.kind == WorkKind::UsbStop) setStatus("USB drive mode off", true);
    } else {
        setStatus(g_result.error[0] == '\0' ? "SD card operation failed" : g_result.error);
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

    lv_obj_t* status_card = lv_obj_create(g_page);
    styleCard(status_card);
    lv_obj_set_height(status_card, 188);
    makeLabel(status_card, "microSD status", wifi_ui::font16(), kTextRgb);
    g_state = makeLabel(status_card, "Not mounted", wifi_ui::font20(), kMutedRgb);
    lv_obj_align(g_state, LV_ALIGN_TOP_LEFT, 0, 34);
    g_details = makeLabel(status_card, "Insert a card and tap Mount.", wifi_ui::font14(), kMutedRgb);
    lv_obj_set_width(g_details, LV_PCT(100));
    lv_label_set_long_mode(g_details, LV_LABEL_LONG_WRAP);
    lv_obj_align(g_details, LV_ALIGN_TOP_LEFT, 0, 76);

    lv_obj_t* actions = lv_obj_create(g_page);
    styleCard(actions);
    lv_obj_set_height(actions, 146);
    makeLabel(actions, "Card operations", wifi_ui::font16(), kTextRgb);
    lv_obj_t* row = lv_obj_create(actions);
    lv_obj_set_size(row, LV_PCT(100), 48);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    g_refresh_button = makeButton(row, LV_SYMBOL_REFRESH "  Refresh", kBorderRgb);
    g_mount_button = makeButton(row, LV_SYMBOL_SD_CARD "  Mount", kAccentRgb);
    g_unmount_button = makeButton(row, LV_SYMBOL_EJECT "  Unmount", kBorderRgb);
    lv_obj_add_event_cb(g_refresh_button, [](lv_event_t*) { refresh(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(g_mount_button, [](lv_event_t*) { startWork(WorkKind::Mount); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(g_unmount_button, [](lv_event_t*) { startWork(WorkKind::Unmount); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* usb = lv_obj_create(g_page);
    styleCard(usb);
    lv_obj_set_height(usb, 210);
    makeLabel(usb, "USB drive", wifi_ui::font16(), kTextRgb);
    g_usb_state = makeLabel(usb, "Off", wifi_ui::font20(), kMutedRgb);
    lv_obj_align(g_usb_state, LV_ALIGN_TOP_LEFT, 0, 34);
    g_usb_details = makeLabel(usb,
                             "Use USB-C to copy files. Serial console on USB-C pauses while this is on.",
                             wifi_ui::font14(), kMutedRgb);
    lv_obj_set_width(g_usb_details, LV_PCT(100));
    lv_label_set_long_mode(g_usb_details, LV_LABEL_LONG_WRAP);
    lv_obj_align(g_usb_details, LV_ALIGN_TOP_LEFT, 0, 76);
    lv_obj_t* usb_row = lv_obj_create(usb);
    lv_obj_set_size(usb_row, LV_PCT(100), 48);
    lv_obj_align(usb_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(usb_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usb_row, 0, 0);
    lv_obj_set_style_pad_all(usb_row, 0, 0);
    lv_obj_set_style_pad_column(usb_row, 10, 0);
    lv_obj_set_flex_flow(usb_row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(usb_row, LV_OBJ_FLAG_SCROLLABLE);
    g_usb_connect_button = makeButton(usb_row, LV_SYMBOL_USB "  Connect", kAccentRgb);
    g_usb_disconnect_button = makeButton(usb_row, LV_SYMBOL_EJECT "  Disconnect", kBorderRgb);
    lv_obj_add_event_cb(g_usb_connect_button, [](lv_event_t*) { startWork(WorkKind::UsbStart); }, LV_EVENT_CLICKED,
                        nullptr);
    lv_obj_add_event_cb(g_usb_disconnect_button, [](lv_event_t*) { askDisconnectConfirmation(); }, LV_EVENT_CLICKED,
                        nullptr);

    lv_obj_t* danger = lv_obj_create(g_page);
    styleCard(danger);
    lv_obj_set_height(danger, 136);
    makeLabel(danger, "Erase and format", wifi_ui::font16(), kTextRgb);
    lv_obj_t* warning = makeLabel(danger, "Formats the entire card as FAT. This cannot be undone.",
                                  wifi_ui::font14(), kMutedRgb);
    lv_obj_align(warning, LV_ALIGN_TOP_LEFT, 0, 30);
    g_format_button = lv_btn_create(danger);
    lv_obj_set_size(g_format_button, 190, 46);
    lv_obj_align(g_format_button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(g_format_button, lv_color_hex(kDangerRgb), 0);
    lv_obj_set_style_radius(g_format_button, 12, 0);
    lv_obj_set_style_shadow_width(g_format_button, 0, 0);
    lv_obj_t* format_label = makeLabel(g_format_button, LV_SYMBOL_TRASH "  Format card", wifi_ui::font16(), kTextRgb);
    lv_obj_center(format_label);
    lv_obj_add_event_cb(g_format_button, [](lv_event_t*) { askFormatConfirmation(); }, LV_EVENT_CLICKED, nullptr);

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
    if (g_app == nullptr || g_page == nullptr) return;
    const SdCardInfo info = g_app->sd.info();
    const bool usb = g_app->usb_msc.active();
    const bool usb_host = g_app->usb_msc.hostAttached();
    const bool working = g_working.load(std::memory_order_acquire) || info.busy;
    if (working) {
        lv_label_set_text(g_state, "Working…");
        lv_obj_set_style_text_color(g_state, lv_color_hex(kAccentRgb), 0);
        lv_label_set_text(g_details, "Do not remove the card or power off while an operation is running.");
    } else if (usb) {
        lv_label_set_text(g_state, "Reserved for USB");
        lv_obj_set_style_text_color(g_state, lv_color_hex(kAccentRgb), 0);
        if (info.card_bytes > 0) {
            lv_label_set_text_fmt(g_details, "Card: %s  ·  %s  ·  %s\nLocal /sd access is paused while USB drive mode is on.",
                                  info.card_name.empty() ? "Unknown" : info.card_name.c_str(), info.type.c_str(),
                                  formatBytes(info.card_bytes).c_str());
        } else {
            lv_label_set_text(g_details, "Local /sd access is paused while USB drive mode is on.");
        }
    } else if (info.mounted) {
        lv_label_set_text(g_state, "Mounted at /sd");
        lv_obj_set_style_text_color(g_state, lv_color_hex(kSuccessRgb), 0);
        const uint64_t used = info.total_bytes > info.free_bytes ? info.total_bytes - info.free_bytes : 0;
        lv_label_set_text_fmt(g_details, "Card: %s  ·  %s  ·  %s\nFAT volume: %s  ·  Used %s  ·  Free %s",
                              info.card_name.empty() ? "Unknown" : info.card_name.c_str(), info.type.c_str(),
                              formatBytes(info.card_bytes).c_str(), formatBytes(info.total_bytes).c_str(),
                              formatBytes(used).c_str(), formatBytes(info.free_bytes).c_str());
    } else {
        lv_label_set_text(g_state, "Not mounted");
        lv_obj_set_style_text_color(g_state, lv_color_hex(kMutedRgb), 0);
        if (!info.error.empty()) {
            lv_label_set_text_fmt(g_details, "%s\nInsert a card and tap Mount to probe again.", info.error.c_str());
        } else {
            lv_label_set_text(g_details, "Insert a card and tap Mount. Tab5 has no separate card-detect signal.");
        }
    }

    if (g_usb_state != nullptr) {
        if (working && (g_current_kind == WorkKind::UsbStart || g_current_kind == WorkKind::UsbStop)) {
            lv_label_set_text(g_usb_state, "Working…");
            lv_obj_set_style_text_color(g_usb_state, lv_color_hex(kAccentRgb), 0);
        } else if (usb && usb_host) {
            lv_label_set_text(g_usb_state, "Connected to computer");
            lv_obj_set_style_text_color(g_usb_state, lv_color_hex(kSuccessRgb), 0);
            lv_label_set_text(g_usb_details, "The card appears as a USB flash drive on USB-C. Eject it on the computer before disconnecting.");
        } else if (usb) {
            lv_label_set_text(g_usb_state, "Waiting for computer");
            lv_obj_set_style_text_color(g_usb_state, lv_color_hex(kAccentRgb), 0);
            lv_label_set_text(g_usb_details, "Plug USB-C into a computer. Serial console on this port pauses until you disconnect.");
        } else {
            lv_label_set_text(g_usb_state, "Off");
            lv_obj_set_style_text_color(g_usb_state, lv_color_hex(kMutedRgb), 0);
            lv_label_set_text(g_usb_details,
                             "Use USB-C to copy files. Serial console on USB-C pauses while this is on.");
        }
    }

    setButtonEnabled(g_refresh_button, !working);
    setButtonEnabled(g_mount_button, !working && !info.mounted && !usb);
    setButtonEnabled(g_unmount_button, !working && info.mounted && !usb);
    setButtonEnabled(g_format_button, !working && !usb);
    setButtonEnabled(g_usb_connect_button, !working && !usb);
    setButtonEnabled(g_usb_disconnect_button, !working && usb);
}

void poll() {
    consumeResult();
    if (g_app == nullptr || g_working.load(std::memory_order_acquire)) return;
    const bool usb_host = g_app->usb_msc.hostAttached();
    if (usb_host != g_last_usb_host) {
        g_last_usb_host = usb_host;
        refresh();
    }
}

bool handleKey(const KeyAction& action) {
    if (action.type != KeyActionType::Menu) return false;
    if (g_confirm != nullptr) {
        lv_msgbox_close(g_confirm);
        g_confirm = nullptr;
        return true;
    }
    return g_working.load(std::memory_order_acquire);
}

}  // namespace sd_ui
}  // namespace tabby
