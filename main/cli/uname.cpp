#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"

namespace tabby {
namespace {

void helpUname(Cli& cli) {
    cli.appendLine("uname - Show system identity");
    cli.appendLine("Usage: uname");
    cli.appendLine("       uname -a");
}

bool runUname(Cli& cli, const CliArgs&) {
    cli.appendLine("Tabby CLI tabby 0.1.0 esp32p4 idf");
    return true;
}

}  // namespace

void registerUnameCommand(CliRegistry& registry) {
    static SimpleCommand uname("uname", "Show system identity", {}, runUname, helpUname);
    registry.add(&uname);
}

}  // namespace tabby
