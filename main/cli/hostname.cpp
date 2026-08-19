#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"
#include "tabby/cli/util.hpp"

namespace tabby {
namespace {

void helpHostname(Cli& cli) {
    cli.appendLine("hostname - Show or set the device name");
    cli.appendLine("Usage: hostname");
    cli.appendLine("       hostname <name>");
}

bool runHostname(Cli& cli, const CliArgs& args) {
    if (cli.config() == nullptr) {
        cli.appendLine("tabby");
        return true;
    }
    if (args.rest.empty()) {
        cli.appendLine(cli.config()->system.deviceName);
        return true;
    }
    std::string name = trimCopy(args.rest);
    if (name.empty() || name.size() > 32 || name.find('\n') != std::string::npos) {
        cli.appendLine("hostname: name must be 1-32 characters");
        return true;
    }
    cli.config()->system.deviceName = name;
    if (cli.settings()) cli.settings()->save(*cli.config());
    cli.appendLine(name);
    return true;
}

}  // namespace

void registerHostnameCommand(CliRegistry& registry) {
    static SimpleCommand hostname("hostname", "Show or set the device name", {}, runHostname, helpHostname);
    hostname.setInterceptHelp(true);
    registry.add(&hostname);
}

}  // namespace tabby
