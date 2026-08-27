#include "ssh_page.hpp"
#include "wifi_page.hpp"

#include "tabby/app.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

namespace tabby {
namespace ssh_ui {
namespace {

constexpr uint32_t kScreenRgb = 0x0B1220;
constexpr uint32_t kCardRgb = 0x1B2838;
constexpr uint32_t kTextRgb = 0xE8EEF4;
constexpr uint32_t kMutedRgb = 0x8A97A8;
constexpr uint32_t kBorderRgb = 0x2A3A4E;
constexpr uint32_t kAccentRgb = 0x3D8BFF;
constexpr uint32_t kDangerRgb = 0xFF5C5C;
constexpr uint32_t kOnRgb = 0x4ADE80;
constexpr size_t kNameBytes = 32;
constexpr size_t kHostBytes = 64;
constexpr size_t kUserBytes = 32;
constexpr size_t kPasswordBytes = 64;
constexpr size_t kTerminalBytes = 32;
constexpr int kDefaultCols = 100;
constexpr int kDefaultRows = 32;

struct ConnectWork {
    SshProfile profile;
    size_t index{0};
    int columns{kDefaultCols};
    int rows{kDefaultRows};
};

struct ConnectResult {
    bool ok{false};
    size_t index{0};
    char error[128]{};
};

enum class Field : uint8_t { Name, Host, Port, User, Password, Terminal, Count };

constexpr const char* kPlaceholders[] = {
    "Name", "Host", "Port", "User", "Password", "Terminal type",
};
static_assert(sizeof(kPlaceholders) / sizeof(kPlaceholders[0]) == static_cast<size_t>(Field::Count));

App* g_app = nullptr;
lv_obj_t* g_page = nullptr;
lv_obj_t* g_status = nullptr;
lv_obj_t* g_actions = nullptr;
lv_obj_t* g_search = nullptr;
lv_obj_t* g_list = nullptr;
lv_obj_t* g_modal = nullptr;
lv_obj_t* g_fields[static_cast<size_t>(Field::Count)] = {};
lv_obj_t* g_focus = nullptr;
bool g_add_mode = false;
size_t g_edit_index = 0;
int g_pty_cols = kDefaultCols;
int g_pty_rows = kDefaultRows;
bool g_last_connected = false;
std::atomic<bool> g_connecting{false};
std::atomic<bool> g_connect_result_ready{false};
ConnectResult g_connect_result;

std::string asciiLower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

bool matchesFilter(const std::string& value, const std::string& filter) {
    if (filter.empty()) return true;
    return asciiLower(value).find(asciiLower(filter)) != std::string::npos;
}

const lv_font_t* font14() { return wifi_ui::font14(); }
const lv_font_t* font16() { return wifi_ui::font16(); }
const lv_font_t* font20() { return wifi_ui::font20(); }

void saveProfiles() {
    if (g_app == nullptr) return;
    g_app->settings.save(g_app->config);
}

std::string filterText() {
    if (g_search == nullptr) return {};
    const char* text = lv_textarea_get_text(g_search);
    return text ? text : "";
}

const char* fieldText(Field field) {
    lv_obj_t* ta = g_fields[static_cast<size_t>(field)];
    if (ta == nullptr) return "";
    const char* text = lv_textarea_get_text(ta);
    return text ? text : "";
}

uint16_t parsePort(const char* text) {
    if (text == nullptr || text[0] == '\0') return 22;
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || value < 1 || value > 65535) return 22;
    return static_cast<uint16_t>(value);
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

size_t fieldLimit(Field field) {
    switch (field) {
        case Field::Name: return kNameBytes;
        case Field::Host: return kHostBytes;
        case Field::Port: return 5;
        case Field::User: return kUserBytes;
        case Field::Password: return kPasswordBytes;
        case Field::Terminal: return kTerminalBytes;
        case Field::Count: break;
    }
    return kNameBytes;
}

Field fieldOf(lv_obj_t* ta) {
    for (size_t i = 0; i < static_cast<size_t>(Field::Count); ++i) {
        if (g_fields[i] == ta) return static_cast<Field>(i);
    }
    return Field::Name;
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

void limitInsert(lv_event_t* event) {
    lv_obj_t* ta = lv_event_get_target(event);
    const Field field = fieldOf(ta);
    const auto* incoming = static_cast<const char*>(lv_event_get_param(event));
    if (incoming == nullptr) return;
    if (field == Field::Port) {
        for (const char* p = incoming; *p != '\0'; ++p) {
            if (*p < '0' || *p > '9') {
                lv_textarea_set_insert_replace(ta, "");
                return;
            }
        }
    }
    limitBytes(event, fieldLimit(field));
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
    for (size_t i = 0; i < static_cast<size_t>(Field::Count); ++i) {
        if (g_focus == g_fields[i]) g_focus = g_search;
        g_fields[i] = nullptr;
    }
    if (g_modal != nullptr) {
        lv_obj_del(g_modal);
        g_modal = nullptr;
    }
    if (g_list != nullptr) lv_obj_clear_flag(g_list, LV_OBJ_FLAG_HIDDEN);
    if (g_search != nullptr) lv_obj_clear_flag(g_search, LV_OBJ_FLAG_HIDDEN);
    if (g_actions != nullptr) lv_obj_clear_flag(g_actions, LV_OBJ_FLAG_HIDDEN);
    g_add_mode = false;
}

void refreshList();
void refreshChrome();

void fillProfile(SshProfile& profile) {
    profile.name = fieldText(Field::Name);
    profile.host = fieldText(Field::Host);
    profile.port = parsePort(fieldText(Field::Port));
    profile.user = fieldText(Field::User);
    profile.password = fieldText(Field::Password);
    profile.terminal = fieldText(Field::Terminal);
    if (profile.terminal.empty()) profile.terminal = "xterm-256color";
    if (profile.name.empty()) profile.name = profile.user + "@" + profile.host;
}

bool submitModal() {
    if (g_app == nullptr) return false;
    SshProfile profile;
    fillProfile(profile);
    if (profile.host.empty() || profile.user.empty()) {
        if (g_status != nullptr) lv_label_set_text(g_status, "Host and user are required");
        return false;
    }
    if (g_add_mode) {
        g_app->config.ssh.push_back(profile);
        g_app->config.activeSsh = g_app->config.ssh.size() - 1;
        g_app->cli.appendLine("ssh: added " + profile.name);
    } else if (g_edit_index < g_app->config.ssh.size()) {
        g_app->config.ssh[g_edit_index] = std::move(profile);
        g_app->cli.appendLine("ssh: saved " + g_app->config.ssh[g_edit_index].name);
    } else {
        return false;
    }
    saveProfiles();
    closeModal();
    refresh();
    return true;
}

void openModal(bool add_mode, size_t index) {
    closeModal();
    g_add_mode = add_mode;
    g_edit_index = index;
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
    lv_obj_set_flex_flow(g_modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(g_modal, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(g_modal, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    lv_obj_t* card = lv_obj_create(g_modal);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(kCardRgb), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* heading = lv_label_create(card);
    lv_label_set_text(heading, add_mode ? "Add profile" : "Edit profile");
    lv_obj_set_style_text_font(heading, font20(), 0);
    lv_obj_set_style_text_color(heading, lv_color_hex(kTextRgb), 0);
    lv_label_set_long_mode(heading, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(heading, LV_PCT(100));

    for (size_t i = 0; i < static_cast<size_t>(Field::Count); ++i) {
        const auto field = static_cast<Field>(i);
        lv_obj_t* ta = lv_textarea_create(card);
        lv_obj_set_width(ta, LV_PCT(100));
        lv_obj_set_height(ta, 48);
        styleTextarea(ta);
        lv_textarea_set_placeholder_text(ta, kPlaceholders[i]);
        lv_textarea_set_password_mode(ta, field == Field::Password);
        lv_obj_add_event_cb(
            ta,
            [](lv_event_t* event) {
                if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
                    setFocus(lv_event_get_target(event));
                } else if (lv_event_get_code(event) == LV_EVENT_INSERT) {
                    limitInsert(event);
                }
            },
            LV_EVENT_ALL, nullptr);
        g_fields[i] = ta;
    }

    if (!add_mode && g_app != nullptr && index < g_app->config.ssh.size()) {
        const auto& profile = g_app->config.ssh[index];
        lv_textarea_set_text(g_fields[static_cast<size_t>(Field::Name)], profile.name.c_str());
        lv_textarea_set_text(g_fields[static_cast<size_t>(Field::Host)], profile.host.c_str());
        char port[8];
        std::snprintf(port, sizeof(port), "%u", profile.port);
        lv_textarea_set_text(g_fields[static_cast<size_t>(Field::Port)], port);
        lv_textarea_set_text(g_fields[static_cast<size_t>(Field::User)], profile.user.c_str());
        lv_textarea_set_text(g_fields[static_cast<size_t>(Field::Password)], profile.password.c_str());
        lv_textarea_set_text(g_fields[static_cast<size_t>(Field::Terminal)], profile.terminal.c_str());
        setFocus(g_fields[static_cast<size_t>(Field::Name)]);
    } else {
        lv_textarea_set_text(g_fields[static_cast<size_t>(Field::Port)], "22");
        lv_textarea_set_text(g_fields[static_cast<size_t>(Field::Terminal)], "xterm-256color");
        setFocus(g_fields[static_cast<size_t>(Field::Name)]);
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

    lv_obj_t* ok = makeActionBtn(actions, LV_SYMBOL_OK, "Save");
    lv_obj_set_style_bg_color(ok, lv_color_hex(kAccentRgb), 0);
    lv_obj_add_event_cb(ok, [](lv_event_t*) { submitModal(); }, LV_EVENT_CLICKED, nullptr);
}

void connectTask(void* parameter) {
    auto* work = static_cast<ConnectWork*>(parameter);
    ConnectResult result;
    result.index = work->index;
    std::string error;
    if (g_app != nullptr) result.ok = g_app->ssh.connect(work->profile, error, work->columns, work->rows);
    if (!result.ok) std::snprintf(result.error, sizeof(result.error), "%s", error.c_str());
    delete work;
    g_connect_result = result;
    g_connect_result_ready.store(true, std::memory_order_release);
    g_connecting.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

void consumeConnectResult() {
    if (!g_connect_result_ready.exchange(false, std::memory_order_acq_rel) || g_app == nullptr) return;
    const ConnectResult result = g_connect_result;
    if (!result.ok) {
        g_app->cli.appendLine(std::string("SSH failed: ") +
                              (result.error[0] == '\0' ? "connection failed" : result.error));
        ::tabby::ssh_ui::refresh();
        return;
    }
    g_app->cli.markSshSessionStart();
    g_app->cli.appendLine("SSH connected");
    g_app->screen = Screen::Terminal;
}

void connectProfile(size_t index) {
    if (g_app == nullptr || index >= g_app->config.ssh.size()) return;
    if (g_app->serial.connected()) {
        g_app->cli.appendLine("ssh: close USB serial first");
        return;
    }
    const auto profile = g_app->config.ssh[index];
    if (g_app->ssh.connected() && g_app->config.activeSsh == index) {
        g_app->screen = Screen::Terminal;
        return;
    }
    bool expected = false;
    if (!g_connecting.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    g_app->config.activeSsh = index;
    saveProfiles();
    if (g_status != nullptr) {
        char status[160];
        std::snprintf(status, sizeof(status), "Connecting  %s@%s", profile.user.c_str(), profile.host.c_str());
        lv_label_set_text(g_status, status);
    }
    g_app->cli.appendLine("Connecting SSH: " + profile.user + "@" + profile.host);
    auto* work = new (std::nothrow) ConnectWork{profile, index, g_pty_cols, g_pty_rows};
    if (work == nullptr || xTaskCreatePinnedToCore(connectTask, "tabby_ssh_connect", 16384, work, 3, nullptr, 0) != pdPASS) {
        delete work;
        g_connecting.store(false, std::memory_order_release);
        g_app->cli.appendLine("SSH failed: could not start connection task");
        ::tabby::ssh_ui::refresh();
        return;
    }
}

void onConnect(lv_event_t* event) {
    const size_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    connectProfile(index);
}

void onEdit(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    const size_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (g_app == nullptr || index >= g_app->config.ssh.size()) return;
    openModal(false, index);
}

void onDelete(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    if (g_app == nullptr) return;
    const size_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (index >= g_app->config.ssh.size()) return;
    const std::string name = g_app->config.ssh[index].name;
    const bool connected = g_app->ssh.connected() && g_app->config.activeSsh == index;
    g_app->config.ssh.erase(g_app->config.ssh.begin() + static_cast<std::ptrdiff_t>(index));
    if (g_app->config.activeSsh > index) {
        --g_app->config.activeSsh;
    } else if (g_app->config.activeSsh >= g_app->config.ssh.size()) {
        g_app->config.activeSsh = 0;
    }
    saveProfiles();
    if (connected) g_app->ssh.disconnect();
    g_app->cli.appendLine("ssh: deleted " + name);
    refresh();
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

lv_obj_t* addIconBtn(lv_obj_t* row, const char* symbol, uint32_t bg, uint32_t fg) {
    lv_obj_t* btn = lv_btn_create(row);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 40, 40);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(btn, 4);
    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_font(label, font16(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(fg), 0);
    lv_obj_center(label);
    return btn;
}

lv_obj_t* addProfileRow(lv_obj_t* list, const SshProfile& profile, size_t index, bool active) {
    lv_obj_t* row = lv_btn_create(list);
    styleCard(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 52);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(kAccentRgb), LV_STATE_PRESSED);

    lv_obj_t* icon_label = lv_label_create(row);
    lv_label_set_text(icon_label, active ? LV_SYMBOL_OK : LV_SYMBOL_DIRECTORY);
    lv_obj_set_style_text_font(icon_label, font16(), 0);
    lv_obj_set_style_text_color(icon_label, lv_color_hex(active ? kOnRgb : kTextRgb), 0);

    lv_obj_t* title_label = lv_label_create(row);
    lv_label_set_text(title_label, profile.name.c_str());
    lv_obj_set_style_text_font(title_label, font16(), 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(kTextRgb), 0);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_flex_grow(title_label, 1);

    char detail[96];
    std::snprintf(detail, sizeof(detail), "%s@%s:%u", profile.user.c_str(), profile.host.c_str(), profile.port);
    lv_obj_t* detail_label = lv_label_create(row);
    lv_label_set_text(detail_label, detail);
    lv_obj_set_style_text_font(detail_label, font14(), 0);
    lv_obj_set_style_text_color(detail_label, lv_color_hex(kMutedRgb), 0);
    lv_label_set_long_mode(detail_label, LV_LABEL_LONG_CLIP);

    lv_obj_add_event_cb(row, onConnect, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(index)));

    lv_obj_t* edit = addIconBtn(row, LV_SYMBOL_EDIT, 0x1C2A3C, kTextRgb);
    lv_obj_add_event_cb(edit, onEdit, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(index)));

    lv_obj_t* del = addIconBtn(row, LV_SYMBOL_TRASH, 0x2A1C24, kDangerRgb);
    lv_obj_add_event_cb(del, onDelete, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(index)));
    return row;
}

void addEmptyRow(lv_obj_t* list, const char* text) {
    lv_obj_t* row = lv_btn_create(list);
    styleCard(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 52);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* icon_label = lv_label_create(row);
    lv_label_set_text(icon_label, LV_SYMBOL_DIRECTORY);
    lv_obj_set_style_text_font(icon_label, font16(), 0);
    lv_obj_set_style_text_color(icon_label, lv_color_hex(kTextRgb), 0);

    lv_obj_t* title_label = lv_label_create(row);
    lv_label_set_text(title_label, text);
    lv_obj_set_style_text_font(title_label, font16(), 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(kTextRgb), 0);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_flex_grow(title_label, 1);
}

void refreshList() {
    if (g_app == nullptr || g_list == nullptr) return;
    lv_obj_clean(g_list);
    const std::string filter = filterText();

    addHeading(g_list, "Profiles");
    size_t shown = 0;
    for (size_t i = 0; i < g_app->config.ssh.size(); ++i) {
        const auto& profile = g_app->config.ssh[i];
        if (!matchesFilter(profile.name, filter) && !matchesFilter(profile.host, filter) &&
            !matchesFilter(profile.user, filter)) {
            continue;
        }
        ++shown;
        const bool active = g_app->ssh.connected() && g_app->config.activeSsh == i;
        addProfileRow(g_list, profile, i, active);
    }
    if (shown == 0) {
        addEmptyRow(g_list, filter.empty() ? "No saved profiles" : "No matching profiles");
    }
}

void refreshChrome() {
    if (g_app == nullptr || g_status == nullptr) return;
    g_last_connected = g_app->ssh.connected();
    if (g_last_connected && g_app->config.activeSsh < g_app->config.ssh.size()) {
        const auto& profile = g_app->config.ssh[g_app->config.activeSsh];
        char status[160];
        std::snprintf(status, sizeof(status), "Connected  %s  %s@%s:%u", profile.name.c_str(), profile.user.c_str(),
                      profile.host.c_str(), profile.port);
        lv_label_set_text(g_status, status);
    } else {
        lv_label_set_text(g_status, "Not connected");
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
    lv_label_set_text(g_status, "SSH");

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

    lv_obj_t* disconnect = makeActionBtn(actions, LV_SYMBOL_CLOSE, "Disconnect");
    lv_obj_add_event_cb(
        disconnect,
        [](lv_event_t*) {
            if (g_app == nullptr || !g_app->ssh.connected()) return;
            g_app->ssh.disconnect();
            g_app->cli.appendLine("SSH disconnected");
            refresh();
        },
        LV_EVENT_CLICKED, nullptr);

    lv_obj_t* add = makeActionBtn(actions, LV_SYMBOL_PLUS, "Add");
    lv_obj_add_event_cb(
        add,
        [](lv_event_t*) { openModal(true, 0); },
        LV_EVENT_CLICKED, nullptr);

    g_search = lv_textarea_create(g_page);
    lv_obj_set_width(g_search, LV_PCT(100));
    lv_obj_set_height(g_search, 48);
    styleTextarea(g_search);
    lv_textarea_set_placeholder_text(g_search, "Search profiles");
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
        [](lv_event_t* event) { limitBytes(event, kHostBytes); },
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
    consumeConnectResult();
    if (g_page == nullptr || lv_obj_has_flag(g_page, LV_OBJ_FLAG_HIDDEN) || g_app == nullptr) return;
    const bool connected = g_app->ssh.connected();
    if (connected != g_last_connected) refresh();
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
    lv_obj_t* ta = g_modal != nullptr ? (g_focus ? g_focus : g_fields[0]) : g_focus;
    if (ta == nullptr) return g_modal != nullptr;
    if (action.text == "\r" || action.text == "\n") {
        if (g_modal != nullptr) submitModal();
        return true;
    }
    if (action.text == "\t") {
        if (g_modal != nullptr) {
            size_t index = 0;
            for (; index < static_cast<size_t>(Field::Count); ++index) {
                if (g_fields[index] == g_focus) break;
            }
            if (index >= static_cast<size_t>(Field::Count)) index = 0;
            else index = (index + 1) % static_cast<size_t>(Field::Count);
            setFocus(g_fields[index]);
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

void setPtySize(int columns, int rows) {
    g_pty_cols = columns > 0 ? columns : kDefaultCols;
    g_pty_rows = rows > 0 ? rows : kDefaultRows;
}

}  // namespace ssh_ui
}  // namespace tabby
