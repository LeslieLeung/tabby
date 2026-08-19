#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"

#include <cstdio>

namespace tabby {
namespace {

void helpBattery(Cli& cli) {
    cli.appendLine("battery - Show battery level and charging state");
    cli.appendLine("Usage: battery");
}

bool runBattery(Cli& cli, const CliArgs&) {
    if (cli.bsp() == nullptr) {
        cli.appendLine("battery: unavailable");
        return true;
    }
    if (!cli.bsp()->batteryPresent()) {
        cli.appendLine("battery: not present");
        return true;
    }
    char line[96];
    std::snprintf(line, sizeof(line), "%d%% %s", cli.bsp()->batteryPercent(),
                  cli.bsp()->batteryCharging() ? "charging" : "discharging");
    cli.appendLine(line);
    std::snprintf(line, sizeof(line), "%d mV  %d mA", cli.bsp()->batteryVoltageMv(), cli.bsp()->batteryCurrentMa());
    cli.appendLine(line);
    return true;
}

}  // namespace

void registerBatteryCommand(CliRegistry& registry) {
    static SimpleCommand battery("battery", "Show battery level and charging state", {}, runBattery, helpBattery);
    registry.add(&battery);
}

}  // namespace tabby
