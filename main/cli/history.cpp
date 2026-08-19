#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"

namespace tabby {
namespace {

void helpHistory(Cli& cli) {
    cli.appendLine("history - Show recent CLI commands");
    cli.appendLine("Usage: history");
}

bool runHistory(Cli& cli, const CliArgs&) {
    const auto entries = cli.history();
    for (size_t i = 0; i < entries.size(); ++i) cli.appendLine(std::to_string(i + 1) + "  " + entries[i]);
    return true;
}

}  // namespace

void registerHistoryCommand(CliRegistry& registry) {
    static SimpleCommand history("history", "Show recent CLI commands", {}, runHistory, helpHistory);
    registry.add(&history);
}

}  // namespace tabby
