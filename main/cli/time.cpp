#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"

namespace tabby {
namespace {

void helpTime(Cli& cli) {
    cli.appendLine("time (ntp) - Clock and NTP sync");
    cli.appendLine("Usage:");
    cli.appendLine("  time sync");
    cli.appendLine("  ntp sync");
    cli.appendLine("  date");
}

void helpTimeSync(Cli& cli) {
    cli.appendLine("time sync - Synchronize the clock over NTP");
    cli.appendLine("Usage: time sync");
    cli.appendLine("       ntp sync");
}

bool runTimeSync(Cli& cli, const CliArgs&) {
    if (cli.time() && cli.config()) cli.time()->configure(cli.config()->system);
    cli.appendLine(cli.time() && cli.time()->sync(10000) && cli.config() ? cli.time()->formatted(cli.config()->system)
                                                                        : "time sync failed");
    return true;
}

}  // namespace

void registerTimeCommand(CliRegistry& registry) {
    static SimpleCommand sync("sync", "Synchronize the clock over NTP", {}, runTimeSync, helpTimeSync);
    static SimpleCommand time("time", "Clock and NTP sync", {"ntp"}, nullptr, helpTime);
    time.addSub(&sync);
    registry.add(&time);
}

}  // namespace tabby
