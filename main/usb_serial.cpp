#include "tabby/usb_serial.hpp"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_types_cdc.h"
#include "usb/vcp_ch34x.h"
#include "usb/vcp_cp210x.h"
#include "usb/vcp_ftdi.h"

#ifndef CDC_HOST_ANY_VID
#define CDC_HOST_ANY_VID 0
#endif
#ifndef CDC_HOST_ANY_PID
#define CDC_HOST_ANY_PID 0
#endif

namespace tabby {
namespace {
constexpr char kTag[] = "tabby_serial";
constexpr uint16_t kEspUsbJtagVid = 0x303A;
constexpr uint16_t kEspUsbJtagPid = 0x1001;
constexpr size_t kRxCap = 32 * 1024;
constexpr size_t kTxCap = 2048;
constexpr uint32_t kOpenAttemptMs = 80;
constexpr uint32_t kTxTimeoutMs = 100;

size_t ringUsed(size_t r, size_t w, size_t cap) { return (w + cap - r) % cap; }

size_t ringSpace(size_t r, size_t w, size_t cap) { return cap ? cap - 1 - ringUsed(r, w, cap) : 0; }

size_t ringPush(std::vector<uint8_t>& buf, size_t& w, size_t r, const uint8_t* data, size_t len) {
    const size_t cap = buf.size();
    size_t wrote = 0;
    while (wrote < len && ringSpace(r, w, cap) > 0) {
        buf[w] = data[wrote++];
        w = (w + 1) % cap;
    }
    return wrote;
}

size_t ringPop(std::vector<uint8_t>& buf, size_t& r, size_t w, uint8_t* data, size_t len) {
    const size_t cap = buf.size();
    size_t got = 0;
    while (got < len && ringUsed(r, w, cap) > 0) {
        data[got++] = buf[r];
        r = (r + 1) % cap;
    }
    return got;
}

bool aborted(const std::atomic<bool>* abort) {
    return abort != nullptr && abort->load(std::memory_order_acquire);
}

cdc_acm_dev_hdl_t asHandle(void* device) { return static_cast<cdc_acm_dev_hdl_t>(device); }

bool onRx(const uint8_t* data, size_t len, void* arg) {
    auto* self = static_cast<UsbSerial*>(arg);
    if (self == nullptr || data == nullptr || len == 0) return true;
    self->ingest(data, len);
    return true;
}

void onEvent(const cdc_acm_host_dev_event_data_t* event, void* arg) {
    auto* self = static_cast<UsbSerial*>(arg);
    if (self == nullptr || event == nullptr) return;
    switch (event->type) {
        case CDC_ACM_HOST_ERROR:
            ESP_LOGW(kTag, "CDC error %d", event->data.error);
            break;
        case CDC_ACM_HOST_DEVICE_DISCONNECTED:
            ESP_LOGI(kTag, "USB serial device disconnected");
            self->noteDisconnected();
            break;
        case CDC_ACM_HOST_SERIAL_STATE:
        case CDC_ACM_HOST_NETWORK_CONNECTION:
        default:
            break;
    }
}

}  // namespace

bool UsbSerial::begin() {
    if (tx_task_ != nullptr) return true;
    rx_.assign(kRxCap, 0);
    tx_.assign(kTxCap, 0);

    cdc_acm_host_driver_config_t driver_config = {};
    driver_config.driver_task_stack_size = 4096;
    driver_config.driver_task_priority = 6;
    driver_config.xCoreID = 0;
    driver_config.new_dev_cb = nullptr;
    esp_err_t err = cdc_acm_host_install(&driver_config);
    if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;
    if (err != ESP_OK) {
        last_error_ = std::string("CDC driver failed: ") + esp_err_to_name(err);
        ESP_LOGW(kTag, "%s", last_error_.c_str());
        return false;
    }

    TaskHandle_t handle = nullptr;
    if (xTaskCreatePinnedToCore(&UsbSerial::txEntry, "tabby_serial_tx", 4096, this, 4, &handle, 0) != pdPASS) {
        last_error_ = "USB serial task failed";
        return false;
    }
    tx_task_ = handle;
    last_error_.clear();
    ESP_LOGI(kTag, "USB CDC host ready on USB-A");
    return true;
}

void UsbSerial::txEntry(void* arg) { static_cast<UsbSerial*>(arg)->txLoop(); }

void UsbSerial::txLoop() {
    uint8_t buf[256];
    for (;;) {
        const size_t n = txPop(buf, sizeof(buf));
        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        auto handle = asHandle(device_);
        if (handle == nullptr || !live_.load(std::memory_order_acquire)) continue;
        const esp_err_t err = cdc_acm_host_data_tx_blocking(handle, buf, n, kTxTimeoutMs);
        if (err != ESP_OK) ESP_LOGW(kTag, "tx failed: %s", esp_err_to_name(err));
    }
}

void* UsbSerial::tryOpen(std::string& identity, std::string& error) {
    cdc_acm_host_device_config_t cfg = {};
    cfg.connection_timeout_ms = kOpenAttemptMs;
    cfg.out_buffer_size = 512;
    cfg.in_buffer_size = 512;
    cfg.event_cb = onEvent;
    cfg.data_cb = onRx;
    cfg.user_arg = this;

    cdc_acm_dev_hdl_t handle = nullptr;
    if (cdc_acm_host_open(CDC_HOST_ANY_VID, CDC_HOST_ANY_PID, 0, &cfg, &handle) == ESP_OK) {
        identity = "CDC";
        return handle;
    }
    if (cdc_acm_host_open(kEspUsbJtagVid, kEspUsbJtagPid, 0, &cfg, &handle) == ESP_OK) {
        identity = "USB Serial/JTAG";
        return handle;
    }
    if (cdc_acm_host_open(kEspUsbJtagVid, kEspUsbJtagPid, 1, &cfg, &handle) == ESP_OK) {
        identity = "USB Serial/JTAG";
        return handle;
    }
    if (ch34x_vcp_open(CH34X_PID_AUTO, 0, &cfg, &handle) == ESP_OK) {
        identity = "CH34x";
        return handle;
    }
    if (cp210x_vcp_open(CP210X_PID_AUTO, 0, &cfg, &handle) == ESP_OK) {
        identity = "CP210x";
        return handle;
    }
    if (ftdi_vcp_open(FTDI_PID_AUTO, 0, &cfg, &handle) == ESP_OK) {
        identity = "FTDI";
        return handle;
    }
    error = "no USB serial device on USB-A";
    return nullptr;
}

bool UsbSerial::configureLine(uint32_t baud) {
    auto handle = asHandle(device_);
    if (handle == nullptr) return false;
    cdc_acm_line_coding_t coding = {};
    coding.dwDTERate = baud;
    coding.bCharFormat = 0;
    coding.bParityType = 0;
    coding.bDataBits = 8;
    const esp_err_t line_err = cdc_acm_host_line_coding_set(handle, &coding);
    if (line_err != ESP_OK) {
        ESP_LOGI(kTag, "line coding %lu ignored: %s", static_cast<unsigned long>(baud), esp_err_to_name(line_err));
    }
    const esp_err_t ctrl_err = cdc_acm_host_set_control_line_state(handle, true, true);
    if (ctrl_err != ESP_OK) {
        ESP_LOGI(kTag, "DTR/RTS ignored: %s", esp_err_to_name(ctrl_err));
    }
    return true;
}

bool UsbSerial::connect(uint32_t baud, uint32_t timeout_ms, std::string& error, const std::atomic<bool>* abort) {
    const std::lock_guard<std::mutex> connect_lock(connect_mutex_);
    if (tx_task_ == nullptr && !begin()) {
        error = last_error_.empty() ? "USB serial is not initialized" : last_error_;
        return false;
    }

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        live_.store(false, std::memory_order_release);
        remote_eof_.store(false, std::memory_order_release);
        closeDevice();
        clearRings();
    }
    baud_.store(baud, std::memory_order_release);

    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    std::string identity;
    void* opened = nullptr;
    for (;;) {
        if (aborted(abort)) {
            error = "interrupted";
            last_error_ = error;
            return false;
        }
        opened = tryOpen(identity, error);
        if (opened != nullptr) break;
        if (esp_timer_get_time() >= deadline) {
            last_error_ = error;
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        closeDevice();
        device_ = opened;
        identity_ = identity;
        configureLine(baud);
        live_.store(true, std::memory_order_release);
        remote_eof_.store(false, std::memory_order_release);
    }
    last_error_.clear();
    ESP_LOGI(kTag, "opened %s baud=%lu", identity.c_str(), static_cast<unsigned long>(baud));
    return true;
}

void UsbSerial::closeDevice() {
    auto handle = asHandle(device_);
    device_ = nullptr;
    identity_.clear();
    if (handle != nullptr) cdc_acm_host_close(handle);
}

void UsbSerial::disconnect() {
    const std::lock_guard<std::mutex> lock(mutex_);
    live_.store(false, std::memory_order_release);
    remote_eof_.store(true, std::memory_order_release);
    closeDevice();
    clearRings();
}

bool UsbSerial::connected() const { return live_.load(std::memory_order_acquire); }

void UsbSerial::ingest(const uint8_t* data, size_t len) {
    if (!live_.load(std::memory_order_acquire) || data == nullptr || len == 0) return;
    rxPush(data, len);
}

void UsbSerial::noteDisconnected() {
    live_.store(false, std::memory_order_release);
    remote_eof_.store(true, std::memory_order_release);
}

int UsbSerial::read(char* buffer, size_t len) {
    if (buffer == nullptr || len == 0) return 0;
    const size_t n = rxPop(reinterpret_cast<uint8_t*>(buffer), len);
    if (n > 0) return static_cast<int>(n);
    if (remote_eof_.load(std::memory_order_acquire)) return -1;
    if (!live_.load(std::memory_order_acquire)) return -1;
    return 0;
}

bool UsbSerial::write(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) return false;
    if (!live_.load(std::memory_order_acquire)) return false;
    const size_t n = txPush(data, len);
    if (n < len) {
        ESP_LOGW(kTag, "tx ring full, dropped %u bytes", static_cast<unsigned>(len - n));
    }
    return n == len;
}

std::string UsbSerial::status() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (live_.load(std::memory_order_acquire)) {
        return identity_ + " " + std::to_string(baud_.load(std::memory_order_acquire));
    }
    return "disconnected";
}

void UsbSerial::clearRings() {
    const std::lock_guard<std::mutex> ring_lock(ring_mutex_);
    rx_r_ = rx_w_ = tx_r_ = tx_w_ = 0;
}

size_t UsbSerial::txPush(const uint8_t* data, size_t len) {
    const std::lock_guard<std::mutex> ring_lock(ring_mutex_);
    return ringPush(tx_, tx_w_, tx_r_, data, len);
}

size_t UsbSerial::txPop(uint8_t* data, size_t len) {
    const std::lock_guard<std::mutex> ring_lock(ring_mutex_);
    return ringPop(tx_, tx_r_, tx_w_, data, len);
}

size_t UsbSerial::rxPush(const uint8_t* data, size_t len) {
    const std::lock_guard<std::mutex> ring_lock(ring_mutex_);
    return ringPush(rx_, rx_w_, rx_r_, data, len);
}

size_t UsbSerial::rxPop(uint8_t* data, size_t len) {
    const std::lock_guard<std::mutex> ring_lock(ring_mutex_);
    return ringPop(rx_, rx_r_, rx_w_, data, len);
}

}  // namespace tabby
