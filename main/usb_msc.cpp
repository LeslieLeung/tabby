#include "tabby/usb_msc.hpp"

#include "tabby/sd_card.hpp"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "hal/usb_wrap_ll.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

#include <string>

namespace tabby {
namespace {

constexpr char kTag[] = "tabby_usb_msc";
// Tab5 USB-C is USB1P1_0 (GPIO 24/25). Default mux puts Serial/JTAG there
// and USB-OTG FS on GPIO 26/27 (I2S on this board).
constexpr gpio_num_t kUsbCDm = GPIO_NUM_24;
constexpr gpio_num_t kUsbCDp = GPIO_NUM_25;
UsbMsc* g_msc = nullptr;

void mapUsbCToOtgDevice() {
    usb_wrap_ll_phy_select(&USB_WRAP, 0);
    gpio_set_drive_capability(kUsbCDm, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(kUsbCDp, GPIO_DRIVE_CAP_3);
}

void restoreUsbCSerialJtag() {
    usb_serial_jtag_ll_phy_select(0);
}

void onDeviceEvent(tinyusb_event_t* event, void* arg) {
    auto* self = static_cast<UsbMsc*>(arg != nullptr ? arg : g_msc);
    if (self == nullptr || event == nullptr) return;
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        self->noteHostAttached(true);
        ESP_LOGI(kTag, "USB host attached");
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        self->noteHostAttached(false);
        ESP_LOGI(kTag, "USB host detached");
    }
}

void onStorageEvent(tinyusb_msc_storage_handle_t, tinyusb_msc_event_t* event, void*) {
    if (event == nullptr) return;
    ESP_LOGI(kTag, "MSC storage event %d mount=%d", static_cast<int>(event->id),
             static_cast<int>(event->mount_point));
}

}  // namespace

void UsbMsc::noteHostAttached(bool attached) {
    host_attached_.store(attached, std::memory_order_release);
}

void UsbMsc::attach(SdCard& sd, KeyboardInput&) {
    sd_ = &sd;
    g_msc = this;
}

bool UsbMsc::startTinyusb() {
    sdmmc_card_t* card = sd_->rawCard();
    if (card == nullptr) {
        last_error_ = "no SD card";
        return false;
    }

    tinyusb_msc_driver_config_t drv_cfg = {};
    drv_cfg.user_flags.auto_mount_off = 1;
    drv_cfg.callback = onStorageEvent;
    drv_cfg.callback_arg = this;
    esp_err_t err = tinyusb_msc_install_driver(&drv_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        last_error_ = std::string("MSC driver failed: ") + esp_err_to_name(err);
        return false;
    }

    tinyusb_msc_storage_config_t storage_cfg = {};
    storage_cfg.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
    storage_cfg.medium.card = card;
    storage_cfg.fat_fs.base_path = nullptr;
    storage_cfg.fat_fs.config.max_files = 4;
    storage_cfg.fat_fs.do_not_format = true;
    storage_cfg.fat_fs.format_flags = 0;

    tinyusb_msc_storage_handle_t handle = nullptr;
    err = tinyusb_msc_new_storage_sdmmc(&storage_cfg, &handle);
    if (err != ESP_OK) {
        tinyusb_msc_uninstall_driver();
        last_error_ = std::string("MSC storage failed: ") + esp_err_to_name(err);
        return false;
    }
    storage_ = handle;

    mapUsbCToOtgDevice();
    tinyusb_config_t tusb_cfg = TINYUSB_CONFIG_FULL_SPEED(onDeviceEvent, this);
    err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        restoreUsbCSerialJtag();
        if (handle != nullptr) tinyusb_msc_delete_storage(handle);
        storage_ = nullptr;
        tinyusb_msc_uninstall_driver();
        last_error_ = std::string("USB device failed: ") + esp_err_to_name(err);
        return false;
    }
    ESP_LOGI(kTag, "USB MSC on USB-C (OTG FS, PHY 0)");
    return true;
}

void UsbMsc::stopTinyusb() {
    host_attached_.store(false, std::memory_order_release);
    if (storage_ != nullptr) {
        tinyusb_msc_delete_storage(static_cast<tinyusb_msc_storage_handle_t>(storage_));
        storage_ = nullptr;
    }
    tinyusb_msc_uninstall_driver();
    tinyusb_driver_uninstall();
    restoreUsbCSerialJtag();
    vTaskDelay(pdMS_TO_TICKS(100));
}

bool UsbMsc::start() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (active_.load(std::memory_order_acquire)) return true;
    if (sd_ == nullptr) {
        last_error_ = "USB drive is not initialized";
        return false;
    }
    if (!sd_->prepareForUsb()) {
        last_error_ = sd_->lastError();
        return false;
    }
    if (!startTinyusb()) {
        sd_->restoreFromUsb();
        return false;
    }
    last_error_.clear();
    active_.store(true, std::memory_order_release);
    ESP_LOGI(kTag, "USB drive mode on");
    return true;
}

bool UsbMsc::stop() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!active_.load(std::memory_order_acquire)) return true;
    stopTinyusb();
    if (sd_ != nullptr) sd_->restoreFromUsb();
    active_.store(false, std::memory_order_release);
    last_error_.clear();
    ESP_LOGI(kTag, "USB drive mode off");
    return true;
}

}  // namespace tabby
