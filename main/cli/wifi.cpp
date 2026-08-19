#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"
#include "tabby/cli/util.hpp"

#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>

namespace tabby {
namespace {

const char* authName(uint8_t auth) {
    switch (auth) {
        case WIFI_AUTH_OPEN:
            return "open";
        case WIFI_AUTH_WEP:
            return "wep";
        case WIFI_AUTH_WPA_PSK:
            return "wpa";
        case WIFI_AUTH_WPA2_PSK:
            return "wpa2";
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "wpa/wpa2";
        case WIFI_AUTH_WPA2_ENTERPRISE:
            return "wpa2-ent";
        case WIFI_AUTH_WPA3_PSK:
            return "wpa3";
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "wpa2/wpa3";
        default:
            return "sec";
    }
}

void helpWifi(Cli& cli) {
    cli.appendLine("wifi - Wi-Fi station");
    cli.appendLine("Usage:");
    cli.appendLine("  wifi");
    cli.appendLine("  wifi status");
    cli.appendLine("  wifi list");
    cli.appendLine("  wifi on | off | disconnect");
    cli.appendLine("  wifi scan");
    cli.appendLine("  wifi connect <index>");
    cli.appendLine("  wifi connect <ssid> [password]");
}

void helpWifiStatus(Cli& cli) {
    cli.appendLine("wifi status - Show connection state, IP and SSID");
    cli.appendLine("Usage: wifi status");
}

void helpWifiList(Cli& cli) {
    cli.appendLine("wifi list - List saved Wi-Fi profiles");
    cli.appendLine("Usage: wifi list");
}

void helpWifiOn(Cli& cli) {
    cli.appendLine("wifi on - Enable Wi-Fi");
    cli.appendLine("Usage: wifi on");
}

void helpWifiOff(Cli& cli) {
    cli.appendLine("wifi off - Disable Wi-Fi");
    cli.appendLine("Usage: wifi off");
}

void helpWifiDisconnect(Cli& cli) {
    cli.appendLine("wifi disconnect - Drop the current association");
    cli.appendLine("Usage: wifi disconnect");
}

void helpWifiScan(Cli& cli) {
    cli.appendLine("wifi scan - Scan and list nearby networks");
    cli.appendLine("Usage: wifi scan");
}

void helpWifiConnect(Cli& cli) {
    cli.appendLine("wifi connect - Connect a saved profile or SSID");
    cli.appendLine("Usage: wifi connect <index>");
    cli.appendLine("       wifi connect <ssid> [password]");
}

bool runWifiStatus(Cli& cli, const CliArgs&) {
    if (cli.wifi()) {
        cli.appendLine(cli.wifi()->status());
        cli.appendLine(std::string("ip=") + cli.wifi()->ip() + " ssid=" + cli.wifi()->ssid());
    }
    return true;
}

bool runWifiList(Cli& cli, const CliArgs&) {
    if (cli.config()) {
        for (size_t i = 0; i < cli.config()->wifi.size(); ++i) {
            const char mark = cli.config()->activeWifi == i ? '*' : ' ';
            char line[160];
            std::snprintf(line, sizeof(line), "%c %u  %s  %s", mark, static_cast<unsigned>(i),
                          cli.config()->wifi[i].name.c_str(), cli.config()->wifi[i].ssid.c_str());
            cli.appendLine(line);
        }
    }
    return true;
}

bool runWifiOn(Cli& cli, const CliArgs&) {
    if (cli.wifi()) cli.wifi()->setEnabled(true);
    cli.appendLine("Wi-Fi on");
    return true;
}

bool runWifiOff(Cli& cli, const CliArgs&) {
    if (cli.wifi()) cli.wifi()->setEnabled(false);
    cli.appendLine("Wi-Fi off");
    return true;
}

bool runWifiDisconnect(Cli& cli, const CliArgs&) {
    if (cli.wifi()) cli.wifi()->disconnect();
    cli.appendLine("Wi-Fi disconnected");
    return true;
}

bool runWifiScan(Cli& cli, const CliArgs&) {
    if (!cli.wifi()) {
        cli.appendLine("Wi-Fi unavailable");
        return true;
    }
    if (!cli.wifi()->startScan() && !cli.wifi()->scanning()) {
        cli.appendLine(cli.wifi()->status());
        return true;
    }
    cli.appendLine("Scanning...");
    const int64_t start = esp_timer_get_time();
    while (cli.wifi()->scanning()) {
        if (cli.interrupted()) {
            cli.appendLine("^C");
            return true;
        }
        if ((esp_timer_get_time() - start) > 10000000) {
            cli.appendLine("wifi scan timed out");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    const auto results = cli.wifi()->scanResults();
    if (results.empty()) {
        cli.appendLine("No networks found");
        return true;
    }
    cli.appendLine(std::string("Found ") + std::to_string(results.size()) + " networks");
    for (const auto& ap : results) {
        char line[160];
        std::snprintf(line, sizeof(line), "  %4d dBm  %-8s  %s", static_cast<int>(ap.rssi), authName(ap.auth),
                      ap.ssid.c_str());
        cli.appendLine(line);
    }
    return true;
}

bool runWifiConnect(Cli& cli, const CliArgs& args) {
    if (!cli.config() || !cli.wifi()) {
        cli.appendLine("usage: wifi connect <index|ssid>");
        return true;
    }
    size_t index = 0;
    if (parseIndex(args.rest, index)) {
        if (index >= cli.config()->wifi.size()) {
            cli.appendLine("usage: wifi connect <index>");
            return true;
        }
        cli.config()->activeWifi = index;
        if (cli.settings()) cli.settings()->save(*cli.config());
        cli.appendLine(cli.wifi()->connect(cli.config()->wifi[index], 8000) ? "Wi-Fi connected" : "Wi-Fi connect failed");
        return true;
    }
    std::string ssid;
    std::string password;
    const auto tokens = tokenize(args.rest);
    if (tokens.empty()) {
        cli.appendLine("usage: wifi connect <index|ssid> [password]");
        return true;
    }
    ssid = tokens[0];
    if (tokens.size() >= 2) password = skipFirstToken(args.rest);
    for (size_t i = 0; i < cli.config()->wifi.size(); ++i) {
        const auto& profile = cli.config()->wifi[i];
        if (profile.ssid == ssid || profile.name == ssid) {
            WifiProfile use = profile;
            if (!password.empty()) use.password = password;
            cli.config()->activeWifi = i;
            if (cli.settings()) cli.settings()->save(*cli.config());
            cli.appendLine(cli.wifi()->connect(use, 8000) ? "Wi-Fi connected" : "Wi-Fi connect failed");
            return true;
        }
    }
    WifiProfile guest;
    guest.name = ssid;
    guest.ssid = ssid;
    guest.password = password;
    cli.appendLine(cli.wifi()->connect(guest, 8000) ? "Wi-Fi connected" : "Wi-Fi connect failed");
    return true;
}

}  // namespace

void registerWifiCommand(CliRegistry& registry) {
    static SimpleCommand status("status", "Show connection state, IP and SSID", {}, runWifiStatus, helpWifiStatus);
    static SimpleCommand list("list", "List saved Wi-Fi profiles", {}, runWifiList, helpWifiList);
    static SimpleCommand on("on", "Enable Wi-Fi", {}, runWifiOn, helpWifiOn);
    static SimpleCommand off("off", "Disable Wi-Fi", {}, runWifiOff, helpWifiOff);
    static SimpleCommand disconnect("disconnect", "Drop the current association", {}, runWifiDisconnect,
                                    helpWifiDisconnect);
    static SimpleCommand scan("scan", "Scan and list nearby networks", {}, runWifiScan, helpWifiScan);
    static SimpleCommand connect("connect", "Connect a saved profile or SSID", {}, runWifiConnect, helpWifiConnect);
    static SimpleCommand wifi("wifi", "Wi-Fi station", {}, runWifiStatus, helpWifi);
    wifi.addSub(&status);
    wifi.addSub(&list);
    wifi.addSub(&on);
    wifi.addSub(&off);
    wifi.addSub(&disconnect);
    wifi.addSub(&scan);
    wifi.addSub(&connect);
    registry.add(&wifi);
}

}  // namespace tabby
