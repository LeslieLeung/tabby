#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"

namespace tabby {
namespace {

void helpKeyboard(Cli& cli) {
    cli.appendLine("keyboard - Show keyboard connection status");
    cli.appendLine("Usage: keyboard");
}

bool runKeyboard(Cli& cli, const CliArgs&) {
    cli.appendLine(cli.keyboard() ? cli.keyboard()->status() : "keyboard: unavailable");
    return true;
}

}  // namespace

void registerKeyboardCommand(CliRegistry& registry) {
    static SimpleCommand keyboard("keyboard", "Show keyboard connection status", {}, runKeyboard, helpKeyboard);
    registry.add(&keyboard);
}

}  // namespace tabby
