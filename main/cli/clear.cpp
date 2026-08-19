#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"

namespace tabby {
namespace {

void helpClear(Cli& cli) {
    cli.appendLine("clear (cls, reset) - Clear the terminal");
    cli.appendLine("Usage: clear");
}

bool runClear(Cli& cli, const CliArgs&) {
    if (cli.terminal()) cli.terminal()->clear();
    return true;
}

}  // namespace

void registerClearCommand(CliRegistry& registry) {
    static SimpleCommand clear("clear", "Clear the terminal", {"cls", "reset"}, runClear, helpClear);
    registry.add(&clear);
}

}  // namespace tabby
