#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"

namespace tabby {
namespace {

void helpEcho(Cli& cli) {
    cli.appendLine("echo - Print text");
    cli.appendLine("Usage: echo <text>");
}

bool runEcho(Cli& cli, const CliArgs& args) {
    cli.appendLine(args.rest);
    return true;
}

}  // namespace

void registerEchoCommand(CliRegistry& registry) {
    static SimpleCommand echo("echo", "Print text", {}, runEcho, helpEcho);
    echo.setInterceptHelp(false);
    registry.add(&echo);
}

}  // namespace tabby
