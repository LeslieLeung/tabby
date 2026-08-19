#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"

#include "esp_timer.h"

#include <cstdint>
#include <cstdio>

namespace tabby {
namespace {

void helpUptime(Cli& cli) {
    cli.appendLine("uptime - Show time since boot");
    cli.appendLine("Usage: uptime");
}

bool runUptime(Cli& cli, const CliArgs&) {
    const uint32_t seconds = static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
    char line[64];
    std::snprintf(line, sizeof(line), "up %uh %um %us", static_cast<unsigned>(seconds / 3600),
                  static_cast<unsigned>((seconds / 60) % 60), static_cast<unsigned>(seconds % 60));
    cli.appendLine(line);
    return true;
}

}  // namespace

void registerUptimeCommand(CliRegistry& registry) {
    static SimpleCommand uptime("uptime", "Show time since boot", {}, runUptime, helpUptime);
    registry.add(&uptime);
}

}  // namespace tabby
