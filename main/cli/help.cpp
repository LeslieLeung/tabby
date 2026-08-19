#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"

#include <cstdio>
#include <string>

namespace tabby {
namespace {

void helpHelp(Cli& cli) {
    cli.appendLine("help (?, man) - List commands and usage");
    cli.appendLine("Usage:");
    cli.appendLine("  help");
    cli.appendLine("  help <command>");
    cli.appendLine("  help <command> <subcommand>");
    cli.appendLine("  <command> help");
    cli.appendLine("  <command> --help");
}

void listCommands(Cli& cli) {
    cli.appendLine("Tabby CLI commands");
    cli.appendLine("Type '<command> help' or 'help <command>' for details.");
    cli.appendLine("");
    for (Command* cmd : cli.registry().commands()) {
        std::string names = cmd->name();
        for (const auto& alias : cmd->aliases()) {
            names += ", ";
            names += alias;
        }
        char line[200];
        std::snprintf(line, sizeof(line), "  %-24s %s", names.c_str(), cmd->summary());
        cli.appendLine(line);
    }
}

bool runHelp(Cli& cli, const CliArgs& args) {
    if (args.rest.empty()) {
        listCommands(cli);
        return true;
    }
    Command* cmd = cli.registry().find(args.tokens.size() >= 2 ? args.tokens[1] : args.rest);
    if (cmd == nullptr) {
        cli.appendLine(std::string("help: unknown command '") + (args.tokens.size() >= 2 ? args.tokens[1] : args.rest) +
                       "'");
        return true;
    }
    if (args.tokens.size() >= 3) {
        if (Command* sub = cmd->findSub(args.tokens[2])) {
            sub->help(cli);
            return true;
        }
        cli.appendLine(std::string("help: unknown subcommand '") + args.tokens[2] + "'");
    }
    cmd->help(cli);
    return true;
}

}  // namespace

void registerHelpCommand(CliRegistry& registry) {
    static SimpleCommand help("help", "List commands and usage", {"?", "man"}, runHelp, helpHelp);
    help.setInterceptHelp(true);
    registry.add(&help);
}

}  // namespace tabby
