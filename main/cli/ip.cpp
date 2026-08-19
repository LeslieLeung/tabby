#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"
#include "tabby/cli/util.hpp"

namespace tabby {
namespace {

void printAddr(Cli& cli) {
    cli.appendLine("wlan0:");
    cli.appendLine(std::string("  inet ") + (cli.wifi() ? cli.wifi()->ip() : "0.0.0.0"));
    cli.appendLine(std::string("  ssid ") + (cli.wifi() ? cli.wifi()->ssid() : ""));
}

void helpIp(Cli& cli) {
    cli.appendLine("ip (ifconfig) - Show network addresses");
    cli.appendLine("Usage:");
    cli.appendLine("  ip addr");
    cli.appendLine("  ip a");
    cli.appendLine("  ifconfig");
}

void helpIpAddr(Cli& cli) {
    cli.appendLine("ip addr - Show Wi-Fi IP and SSID");
    cli.appendLine("Usage: ip addr");
    cli.appendLine("       ip a");
    cli.appendLine("       ifconfig");
}

bool runIpAddr(Cli& cli, const CliArgs&) {
    printAddr(cli);
    return true;
}

bool runIp(Cli& cli, const CliArgs& args) {
    if (lowerCopy(args.name) == "ifconfig") {
        printAddr(cli);
        return true;
    }
    if (args.rest.empty()) {
        helpIp(cli);
        return true;
    }
    cli.appendLine(std::string("ip: unknown subcommand '") + args.tokens[1] + "'");
    helpIp(cli);
    return true;
}

}  // namespace

void registerIpCommand(CliRegistry& registry) {
    static SimpleCommand addr("addr", "Show Wi-Fi IP and SSID", {"a"}, runIpAddr, helpIpAddr);
    static SimpleCommand ip("ip", "Show network addresses", {"ifconfig"}, runIp, helpIp);
    ip.addSub(&addr);
    registry.add(&ip);
}

}  // namespace tabby
