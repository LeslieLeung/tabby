#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"

#include "esp_heap_caps.h"

#include <cstdio>

namespace tabby {
namespace {

void helpStatus(Cli& cli) {
    cli.appendLine("status - Show device, Wi-Fi, SSH, SD and power status");
    cli.appendLine("Usage: status");
}

bool runStatus(Cli& cli, const CliArgs&) {
    char line[192];
    std::snprintf(line, sizeof(line), "screen=Tabby CLI wifi=%s", cli.wifi() ? cli.wifi()->status().c_str() : "n/a");
    cli.appendLine(line);
    std::snprintf(line, sizeof(line), "ip=%s ssid=%s", cli.wifi() ? cli.wifi()->ip().c_str() : "0.0.0.0",
                  cli.wifi() ? cli.wifi()->ssid().c_str() : "");
    cli.appendLine(line);
    std::snprintf(line, sizeof(line), "ssh=%s", cli.ssh() && cli.ssh()->connected() ? "connected" : "disconnected");
    cli.appendLine(line);
    if (cli.sd()) {
        if (cli.sd()->usbMode()) cli.appendLine("sd=usb drive");
        else cli.appendLine(cli.sd()->ready() ? "sd=ready cwd=" + cli.sd()->cwd() : "sd=not present");
    }
    if (cli.usb()) {
        if (cli.usb()->active() && cli.usb()->hostAttached()) cli.appendLine("usb=connected to computer");
        else if (cli.usb()->active()) cli.appendLine("usb=waiting for computer");
        else cli.appendLine("usb=off");
    }
    if (cli.keyboard()) cli.appendLine("keyboard=" + cli.keyboard()->status());
    if (cli.bsp()) {
        if (!cli.bsp()->batteryPresent()) {
            cli.appendLine("battery=not present");
        } else {
            std::snprintf(line, sizeof(line), "battery=%d%% %s %dmV", cli.bsp()->batteryPercent(),
                          cli.bsp()->batteryCharging() ? "charging" : "discharging",
                          cli.bsp()->batteryVoltageMv());
            cli.appendLine(line);
        }
        if (cli.config()) {
            const unsigned pct =
                (static_cast<unsigned>(cli.config()->display.brightness) * 100 + DisplayConfig::kMaxBrightness / 2) /
                DisplayConfig::kMaxBrightness;
            std::snprintf(line, sizeof(line), "brightness=%u%%", pct);
            cli.appendLine(line);
        }
    }
    if (cli.config()) {
        std::snprintf(line, sizeof(line), "device=%s region=%s", cli.config()->system.deviceName.c_str(),
                      cli.config()->system.region.c_str());
        cli.appendLine(line);
        std::snprintf(line, sizeof(line), "keymap=%s activeWifi=%u activeSsh=%u", cli.config()->keyboard.layout.c_str(),
                      static_cast<unsigned>(cli.config()->activeWifi), static_cast<unsigned>(cli.config()->activeSsh));
        cli.appendLine(line);
    }
    const size_t intern = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    std::snprintf(line, sizeof(line), "heap intern=%u spiram=%u", static_cast<unsigned>(intern),
                  static_cast<unsigned>(spiram));
    cli.appendLine(line);
    return true;
}

}  // namespace

void registerStatusCommand(CliRegistry& registry) {
    static SimpleCommand status("status", "Show device, Wi-Fi, SSH, SD and power status", {}, runStatus, helpStatus);
    registry.add(&status);
}

}  // namespace tabby
