#include "tabby/app.hpp"

#include "boot_splash.hpp"
#include "cjk_term_font.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static constexpr char kTag[] = "tabby";

static void netTask(void* arg) {
    auto* app = static_cast<tabby::App*>(arg);
    tabby::CjkTermFont::setProgressHook(tabby::BootSplashSetStatus);
    tabby::BootSplashSetStatus("Starting Wi-Fi...");
    if (!app->wifi.begin()) {
        ESP_LOGW(kTag, "Wi-Fi: %s", app->wifi.lastError().c_str());
    }
    // SD is optional. Probe after Wi-Fi init so a missing card cannot disturb
    // C6 SDIO. Association itself waits until the splash is done.
    tabby::BootSplashSetStatus("Mounting SD card...");
    if (!app->sd.begin()) {
        ESP_LOGI(kTag, "SD card not present; /sd commands disabled");
        tabby::BootSplashSetStatus("No SD card");
    } else if (tabby::CjkTermFont::tryLoadFromSd(true)) {
        ESP_LOGI(kTag, "CJK font: %s", tabby::CjkTermFont::status());
        tabby::BootSplashSetStatus("Fonts loaded");
    } else {
        tabby::BootSplashSetStatus("Ready");
    }
    tabby::BootSplashFinish();

    if (app->wifi.ready()) {
        app->wifi.connectAny(app->config, 8000);
        if (app->wifi.connected()) app->time.sync(8000);
    }
    vTaskDelete(nullptr);
}

extern "C" void app_main(void) {
    static tabby::App app;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    if (!app.bsp.initialize()) {
        ESP_LOGE(kTag, "Board init failed");
        return;
    }
    if (!app.settings.begin() || !app.settings.load(app.config)) {
        ESP_LOGW(kTag, "Settings: %s", app.settings.lastError().c_str());
    }
    app.bsp.setBrightness(app.config.display.brightness);
    tabby::BootSplashShow(app.bsp);
    app.keyboard.configure(app.config.keyboard);
    app.keyboard.begin();
    app.usb_msc.attach(app.sd, app.keyboard);
    app.time.configure(app.config.system);
    app.python.begin();
    if (!app.ssh.begin()) ESP_LOGW(kTag, "SSH transmit task failed");
    if (!app.serial.begin()) ESP_LOGW(kTag, "USB serial: %s", app.serial.lastError().c_str());
    app.cli.attach(&app.config, &app.settings, &app.wifi, &app.ssh, &app.python, &app.terminal, &app.sd, &app.time,
                   &app.editor, &app.bsp, &app.keyboard, &app.usb_msc, &app.serial);

    if (xTaskCreatePinnedToCore(netTask, "tabby_net", 8192, &app, 3, nullptr, 0) != pdPASS) {
        // Never fall back to the app/UI path: connect and card probing can
        // block for seconds, and netTask deletes its caller on completion.
        ESP_LOGW(kTag, "Wi-Fi/SD bootstrap task failed; continuing without bootstrap");
        tabby::BootSplashFinish();
    }

    if (!tabby::UiStart(app)) {
        ESP_LOGE(kTag, "UI task failed");
        return;
    }
    ESP_LOGI(kTag, "Tabby started on %s", app.bsp.boardName());
}
