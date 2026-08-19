#include "tabby/keyboard_input.hpp"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/usb_host.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace tabby {
namespace {

constexpr char kTag[] = "tabby_kbd";
constexpr uint8_t kAddr = 0x6D;
constexpr uint8_t kRegEventNum = 0x02;
constexpr uint8_t kRegMode = 0x10;
constexpr uint8_t kRegCharLength = 0x40;
constexpr uint8_t kRegCharEvent = 0x50;
constexpr uint8_t kRegFirmware = 0xFE;
constexpr uint8_t kModeCharacter = 2;
constexpr gpio_num_t kSda = GPIO_NUM_0;
constexpr gpio_num_t kScl = GPIO_NUM_1;
constexpr int kI2cTimeoutMs = 30;
constexpr uint32_t kProbeIntervalMs = 500;
constexpr uint8_t kI2cFailLimit = 3;

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL); }

i2c_master_bus_handle_t g_bus = nullptr;
i2c_master_dev_handle_t g_dev = nullptr;
KeyboardInput* g_input = nullptr;
std::mutex g_queue_mutex;
QueueHandle_t g_hid_events = nullptr;
uint8_t g_prev_keys[HID_KEYBOARD_KEY_MAX]{};
std::atomic<int> g_usb_keyboards{0};
std::atomic<bool> g_usb_host_stop{false};
std::mutex g_usb_host_mutex;
std::vector<hid_host_device_handle_t> g_hid_handles;
TaskHandle_t g_usb_lib_task = nullptr;
TaskHandle_t g_usb_lib_waiter = nullptr;

struct HidEvent {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t event;
};

bool keyFound(const uint8_t* src, uint8_t key, unsigned length) {
    for (unsigned i = 0; i < length; ++i) {
        if (src[i] == key) return true;
    }
    return false;
}

void hidKeyboardReport(const uint8_t* data, int length) {
    if (g_input == nullptr || length < static_cast<int>(sizeof(hid_keyboard_input_report_boot_t))) return;
    const auto* report = reinterpret_cast<const hid_keyboard_input_report_boot_t*>(data);
    for (int i = 0; i < HID_KEYBOARD_KEY_MAX; ++i) {
        const uint8_t key = report->key[i];
        if (key > HID_KEY_ERROR_UNDEFINED && !keyFound(g_prev_keys, key, HID_KEYBOARD_KEY_MAX)) {
            g_input->push(g_input->mapper().mapHid(report->modifier.val, key));
        }
    }
    std::memcpy(g_prev_keys, report->key, HID_KEYBOARD_KEY_MAX);
}

void hidInterfaceCallback(hid_host_device_handle_t handle, const hid_host_interface_event_t event, void*) {
    hid_host_dev_params_t params{};
    if (hid_host_device_get_params(handle, &params) != ESP_OK) return;
    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        uint8_t data[64]{};
        size_t length = 0;
        if (hid_host_device_get_raw_input_report_data(handle, data, sizeof(data), &length) == ESP_OK &&
            params.sub_class == HID_SUBCLASS_BOOT_INTERFACE && params.proto == HID_PROTOCOL_KEYBOARD) {
            hidKeyboardReport(data, static_cast<int>(length));
        }
    } else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        hid_host_device_close(handle);
        {
            const std::lock_guard<std::mutex> lock(g_usb_host_mutex);
            g_hid_handles.erase(std::remove(g_hid_handles.begin(), g_hid_handles.end(), handle), g_hid_handles.end());
        }
        if (params.proto == HID_PROTOCOL_KEYBOARD && g_usb_keyboards > 0) --g_usb_keyboards;
        std::memset(g_prev_keys, 0, sizeof(g_prev_keys));
        if (g_input) g_input->noteUsbDisconnected();
        ESP_LOGI(kTag, "USB HID keyboard disconnected");
    }
}

void hidDeviceCallback(hid_host_device_handle_t handle, const hid_host_driver_event_t event, void*) {
    if (g_hid_events == nullptr) return;
    HidEvent msg{handle, event};
    xQueueSend(g_hid_events, &msg, 0);
}

void usbLibTask(void* arg) {
    auto waiter = static_cast<TaskHandle_t>(arg);
    usb_host_config_t host_config = {};
    host_config.skip_phy_setup = false;
    host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
    const esp_err_t err = usb_host_install(&host_config);
    if (waiter) xTaskNotifyGive(waiter);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "usb_host_install failed");
        g_usb_lib_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            const esp_err_t free_err = usb_host_device_free_all();
            if (g_usb_host_stop.load(std::memory_order_acquire) &&
                (free_err == ESP_OK || (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE))) {
                break;
            }
        }
        if (g_usb_host_stop.load(std::memory_order_acquire) && (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)) {
            break;
        }
    }
    usb_host_uninstall();
    g_usb_lib_task = nullptr;
    if (g_usb_lib_waiter) xTaskNotifyGive(g_usb_lib_waiter);
    vTaskDelete(nullptr);
}

void keyboardPollTask(void* arg) {
    auto* input = static_cast<KeyboardInput*>(arg);
    for (;;) {
        input->update();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

std::string toLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

KeyAction mapNamedKey(KeyboardMapper& mapper, const char* chars, uint8_t modifier) {
    std::string token = toLower(chars);
    for (char& c : token) {
        if (c == '_' || c == '-') c = ' ';
    }
    const bool shift = (modifier & 0x22) != 0;
    const bool ctrl = (modifier & 0x11) != 0;
    if (token == "backspace" || token == "bs") return mapper.mapChar(static_cast<char>(0x7F));
    if (token == "enter" || token == "return") return mapper.mapChar('\r');
    if (token == "tab") return mapper.mapChar('\t');
    if (token == "esc" || token == "escape") return mapper.mapChar(static_cast<char>(0x1B));
    if (token == "space") return mapper.mapChar(' ');
    if (token == "up" || token == "up arrow") {
        if (ctrl) return {KeyActionType::Scroll, "", kTerminalScrollStep};
        if (shift) return {KeyActionType::Scroll, "", 1};
        return {KeyActionType::Text, "\x1B[A", 0};
    }
    if (token == "down" || token == "down arrow") {
        if (ctrl) return {KeyActionType::Scroll, "", -kTerminalScrollStep};
        if (shift) return {KeyActionType::Scroll, "", -1};
        return {KeyActionType::Text, "\x1B[B", 0};
    }
    if (token == "right" || token == "right arrow") return {KeyActionType::Text, "\x1B[C", 0};
    if (token == "left" || token == "left arrow") return {KeyActionType::Text, "\x1B[D", 0};
    if (token == "home") return {KeyActionType::Text, "\x1B[H", 0};
    if (token == "end") return {KeyActionType::Text, "\x1B[F", 0};
    if (token == "delete" || token == "del") return {KeyActionType::Text, "\x1B[3~", 0};
    if (token == "page up" || token == "pgup") {
        if (shift || ctrl) return {KeyActionType::Scroll, "", kTerminalScrollPage};
        return {KeyActionType::Text, "\x1B[5~", 0};
    }
    if (token == "page down" || token == "pgdn") {
        if (shift || ctrl) return {KeyActionType::Scroll, "", -kTerminalScrollPage};
        return {KeyActionType::Text, "\x1B[6~", 0};
    }
    return {};
}

KeyAction mapCtrlCharacter(char c) {
    if (c >= 'a' && c <= 'z') return {KeyActionType::Text, std::string(1, static_cast<char>(c - 'a' + 1)), 0};
    if (c >= 'A' && c <= 'Z') return {KeyActionType::Text, std::string(1, static_cast<char>(c - 'A' + 1)), 0};
    return {};
}

}  // namespace

void KeyboardInput::configure(const KeyboardConfig& config) { mapper_.configure(config); }

std::string KeyboardInput::status() const {
    const std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

bool KeyboardInput::readI2c(uint8_t reg, uint8_t* data, size_t len) {
    if (g_dev == nullptr) return false;
    return i2c_master_transmit_receive(g_dev, &reg, 1, data, len, kI2cTimeoutMs) == ESP_OK;
}

bool KeyboardInput::writeI2c(uint8_t reg, uint8_t value) {
    if (g_dev == nullptr) return false;
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(g_dev, buf, sizeof(buf), kI2cTimeoutMs) == ESP_OK;
}

void KeyboardInput::refreshStatus() {
    const std::lock_guard<std::mutex> lock(status_mutex_);
    const bool ready = ready_.load(std::memory_order_acquire);
    const int usb_keyboards = g_usb_keyboards.load(std::memory_order_acquire);
    if (ready && usb_keyboards > 0) {
        status_ = "Tab5 keyboard ready; USB keyboard connected";
    } else if (ready) {
        char text[48];
        std::snprintf(text, sizeof(text), "Tab5 keyboard ready events=%lu",
                      static_cast<unsigned long>(events_.load(std::memory_order_acquire)));
        status_ = text;
    } else if (usb_keyboards > 0) {
        status_ = "USB keyboard connected";
    } else if (g_bus == nullptr) {
        status_ = "Tab5 keyboard I2C bus failed; USB-serial fallback active";
    } else {
        status_ = "Tab5 keyboard waiting (hotplug)";
    }
}

void KeyboardInput::noteUsbConnected() { refreshStatus(); }

void KeyboardInput::noteUsbDisconnected() { refreshStatus(); }

bool KeyboardInput::ensureI2cBus() {
    if (g_bus == nullptr) {
        i2c_master_bus_config_t bus_cfg = {};
        // I2C_NUM_1 is already taken by M5GFX (touch on GPIO 31/32).
        bus_cfg.i2c_port = I2C_NUM_0;
        bus_cfg.sda_io_num = kSda;
        bus_cfg.scl_io_num = kScl;
        bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_cfg.glitch_ignore_cnt = 7;
        bus_cfg.flags.enable_internal_pullup = true;
        if (i2c_new_master_bus(&bus_cfg, &g_bus) != ESP_OK) {
            g_bus = nullptr;
            return false;
        }
    }
    if (g_dev == nullptr) {
        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = kAddr;
        dev_cfg.scl_speed_hz = 400000;
        if (i2c_master_bus_add_device(g_bus, &dev_cfg, &g_dev) != ESP_OK) {
            g_dev = nullptr;
            return false;
        }
    }
    return true;
}

bool KeyboardInput::tryAttachTab5() {
    last_probe_ms_ = nowMs();
    if (!ensureI2cBus()) return false;
    if (i2c_master_probe(g_bus, kAddr, kI2cTimeoutMs) != ESP_OK) return false;
    uint8_t fw = 0;
    if (!writeI2c(kRegMode, kModeCharacter) || !readI2c(kRegFirmware, &fw, 1)) return false;
    ready_.store(true, std::memory_order_release);
    i2c_fail_count_ = 0;
    char text[48];
    std::snprintf(text, sizeof(text), "Tab5 keyboard ready fw=0x%02x", fw);
    {
        const std::lock_guard<std::mutex> lock(status_mutex_);
        status_ = text;
    }
    ESP_LOGI(kTag, "%s", text);
    return true;
}

void KeyboardInput::markTab5Disconnected() {
    if (!ready_.exchange(false, std::memory_order_acq_rel)) return;
    i2c_fail_count_ = 0;
    last_probe_ms_ = nowMs();
    ESP_LOGI(kTag, "Tab5 keyboard disconnected");
    refreshStatus();
}

bool KeyboardInput::begin() {
    g_input = this;
    beginUsbHost();
    if (!ensureI2cBus()) {
        {
            const std::lock_guard<std::mutex> lock(status_mutex_);
            status_ = "Tab5 keyboard I2C bus failed; USB-serial fallback active";
        }
        ESP_LOGW(kTag, "Tab5 keyboard I2C bus failed; USB-serial fallback active");
        startPollTask();
        return false;
    }
    const bool attached = tryAttachTab5();
    if (!attached) refreshStatus();
    const std::string current_status = status();
    ESP_LOGI(kTag, "%s", current_status.c_str());
    startPollTask();
    return attached;
}

bool KeyboardInput::startPollTask() {
    if (poll_task_started_) return true;
    if (xTaskCreatePinnedToCore(keyboardPollTask, "tabby_keyboard", 6144, this, 3, nullptr, 0) != pdPASS) {
        return false;
    }
    poll_task_started_ = true;
    return true;
}

void KeyboardInput::push(const KeyAction& action) {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    const size_t next = (head_ + 1) % kQueueSize;
    if (next == tail_) tail_ = (tail_ + 1) % kQueueSize;
    queue_[head_] = action;
    head_ = next;
}

void KeyboardInput::beginUsbHost() {
    if (usb_started_) return;
    if (g_hid_events == nullptr) {
        g_hid_events = xQueueCreate(8, sizeof(HidEvent));
    }
    g_usb_host_stop.store(false, std::memory_order_release);
    const TaskHandle_t waiter = xTaskGetCurrentTaskHandle();
    if (xTaskCreatePinnedToCore(usbLibTask, "usb_lib", 4096, waiter, 2, &g_usb_lib_task, 0) != pdPASS) {
        const std::lock_guard<std::mutex> lock(status_mutex_);
        status_ = "USB host task failed";
        g_usb_lib_task = nullptr;
        return;
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    hid_host_driver_config_t hid_cfg = {};
    hid_cfg.create_background_task = true;
    hid_cfg.task_priority = 5;
    hid_cfg.stack_size = 4096;
    hid_cfg.core_id = 0;
    hid_cfg.callback = hidDeviceCallback;
    hid_cfg.callback_arg = nullptr;
    if (hid_host_install(&hid_cfg) != ESP_OK) {
        ESP_LOGW(kTag, "hid_host_install failed");
        return;
    }
    usb_started_ = true;
    ESP_LOGI(kTag, "USB HID host started");
}

void KeyboardInput::closeUsbDevices() {
    std::vector<hid_host_device_handle_t> handles;
    {
        const std::lock_guard<std::mutex> lock(g_usb_host_mutex);
        handles.swap(g_hid_handles);
    }
    for (const auto handle : handles) {
        hid_host_device_close(handle);
    }
    g_usb_keyboards.store(0, std::memory_order_release);
    std::memset(g_prev_keys, 0, sizeof(g_prev_keys));
}

bool KeyboardInput::stopUsbHostLocked() {
    if (!usb_started_ && g_usb_lib_task == nullptr) return true;
    usb_started_ = false;
    closeUsbDevices();
    if (g_hid_events != nullptr) {
        HidEvent event{};
        while (xQueueReceive(g_hid_events, &event, 0) == pdTRUE) {
            hid_host_device_close(event.handle);
        }
    }
    esp_err_t hid_err = ESP_FAIL;
    for (int attempt = 0; attempt < 10; ++attempt) {
        hid_err = hid_host_uninstall();
        if (hid_err == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (hid_err != ESP_OK) {
        ESP_LOGW(kTag, "hid_host_uninstall failed: %s", esp_err_to_name(hid_err));
        return false;
    }
    if (g_usb_lib_task == nullptr) return true;
    g_usb_lib_waiter = xTaskGetCurrentTaskHandle();
    g_usb_host_stop.store(true, std::memory_order_release);
    const uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));
    g_usb_lib_waiter = nullptr;
    if (notified == 0 && g_usb_lib_task != nullptr) {
        ESP_LOGW(kTag, "USB host task did not stop");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(kTag, "USB HID host stopped");
    return true;
}

bool KeyboardInput::pauseUsbHost() {
    const bool ok = stopUsbHostLocked();
    refreshStatus();
    return ok;
}

bool KeyboardInput::resumeUsbHost() {
    beginUsbHost();
    refreshStatus();
    return usb_started_;
}

void KeyboardInput::pollUsbHost() {
    if (!usb_started_ || g_hid_events == nullptr) return;
    HidEvent event{};
    while (usb_started_ && xQueueReceive(g_hid_events, &event, 0) == pdTRUE) {
        if (event.event != HID_HOST_DRIVER_EVENT_CONNECTED) continue;
        if (!usb_started_) {
            hid_host_device_close(event.handle);
            continue;
        }
        hid_host_dev_params_t params{};
        if (hid_host_device_get_params(event.handle, &params) != ESP_OK) continue;
        hid_host_device_config_t dev_cfg = {};
        dev_cfg.callback = hidInterfaceCallback;
        dev_cfg.callback_arg = nullptr;
        if (hid_host_device_open(event.handle, &dev_cfg) != ESP_OK) continue;
        {
            const std::lock_guard<std::mutex> lock(g_usb_host_mutex);
            g_hid_handles.push_back(event.handle);
        }
        if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
            hid_class_request_set_protocol(event.handle, HID_REPORT_PROTOCOL_BOOT);
            if (params.proto == HID_PROTOCOL_KEYBOARD) {
                hid_class_request_set_idle(event.handle, 0, 0);
                ++g_usb_keyboards;
                noteUsbConnected();
                ESP_LOGI(kTag, "USB keyboard connected");
            }
        }
        hid_host_device_start(event.handle);
    }
}

void KeyboardInput::pollTab5Keyboard() {
    if (!ready_.load(std::memory_order_acquire)) {
        if (nowMs() - last_probe_ms_ < kProbeIntervalMs) return;
        tryAttachTab5();
        return;
    }
    uint8_t count = 0;
    if (!readI2c(kRegEventNum, &count, 1) || count == 0xFF) {
        if (++i2c_fail_count_ >= kI2cFailLimit) markTab5Disconnected();
        return;
    }
    if (count == 0) {
        i2c_fail_count_ = 0;
        return;
    }
    for (uint8_t n = 0; n < count && n < 32; ++n) {
        uint8_t length = 0;
        if (!readI2c(kRegCharLength, &length, 1)) {
            if (++i2c_fail_count_ >= kI2cFailLimit) markTab5Disconnected();
            break;
        }
        // 0x40 reports the total event size: one modifier byte followed by
        // zero to nine character bytes. The keyboard's 0x50 register window
        // must be read using that exact size; asking for all ten bytes for a
        // two-byte key event can NACK and leaves the event queued forever.
        if (length == 0) break;
        if (length > 10) {
            ESP_LOGW(kTag, "invalid character event length: %u", static_cast<unsigned>(length));
            break;
        }
        uint8_t payload[10]{};
        if (!readI2c(kRegCharEvent, payload, length)) {
            if (++i2c_fail_count_ >= kI2cFailLimit) markTab5Disconnected();
            break;
        }
        i2c_fail_count_ = 0;
        const uint8_t modifier = payload[0];
        const uint8_t nchars = static_cast<uint8_t>(length - 1);
        char chars[10]{};
        std::memcpy(chars, payload + 1, nchars);
        events_.fetch_add(1, std::memory_order_acq_rel);
        KeyAction named = mapNamedKey(mapper_, chars, modifier);
        if (named.type != KeyActionType::None) {
            push(named);
        } else {
            bool utf8 = false;
            for (uint8_t i = 0; i < nchars; ++i) {
                if (static_cast<unsigned char>(chars[i]) >= 0x80) {
                    utf8 = true;
                    break;
                }
            }
            if (utf8) {
                push({KeyActionType::Text, std::string(chars, nchars), 0});
            } else {
                for (uint8_t i = 0; i < nchars; ++i) {
                    const bool ctrl = (modifier & 0x11) != 0;
                    KeyAction action = ctrl ? mapCtrlCharacter(chars[i]) : KeyAction{};
                    push(action.type != KeyActionType::None ? action : mapper_.mapChar(chars[i]));
                }
            }
        }
    }
}

void KeyboardInput::update() {
    pollUsbHost();
    pollTab5Keyboard();
}

bool KeyboardInput::available() const {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    return head_ != tail_;
}

KeyAction KeyboardInput::read() {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    if (head_ == tail_) return {};
    KeyAction action = queue_[tail_];
    tail_ = (tail_ + 1) % kQueueSize;
    return action;
}

}  // namespace tabby
