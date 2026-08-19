#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"

namespace tabby {
namespace {

void helpWhoami(Cli& cli) {
    cli.appendLine("whoami - Show the device name");
    cli.appendLine("Usage: whoami");
}

bool runWhoami(Cli& cli, const CliArgs&) {
    cli.appendLine(cli.config() ? cli.config()->system.deviceName : "tabby");
    return true;
}

}  // namespace

void registerWhoamiCommand(CliRegistry& registry) {
    static SimpleCommand whoami("whoami", "Show the device name", {}, runWhoami, helpWhoami);
    registry.add(&whoami);
}

}  // namespace tabby
