#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace tabby {
namespace {

void helpReboot(Cli& cli) {
    cli.appendLine("reboot (restart) - Reboot the device");
    cli.appendLine("Usage: reboot");
}

bool runReboot(Cli& cli, const CliArgs&) {
    cli.appendLine("rebooting...");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return true;
}

}  // namespace

void registerRebootCommand(CliRegistry& registry) {
    static SimpleCommand reboot("reboot", "Reboot the device", {"restart"}, runReboot, helpReboot);
    registry.add(&reboot);
}

}  // namespace tabby
