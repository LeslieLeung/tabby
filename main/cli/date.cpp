#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"

namespace tabby {
namespace {

void helpDate(Cli& cli) {
    cli.appendLine("date - Show the current date and time");
    cli.appendLine("Usage: date");
    cli.appendLine("Sync first with 'time sync' if the clock has not been set.");
}

bool runDate(Cli& cli, const CliArgs&) {
    const std::string text = cli.time() && cli.config() ? cli.time()->formatted(cli.config()->system) : std::string();
    cli.appendLine(text.empty() ? "time not synced; connect Wi-Fi or run 'time sync'" : text);
    return true;
}

}  // namespace

void registerDateCommand(CliRegistry& registry) {
    static SimpleCommand date("date", "Show the current date and time", {}, runDate, helpDate);
    registry.add(&date);
}

}  // namespace tabby
