#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"
#include "tabby/cli/util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace tabby {
namespace {

void helpSsh(Cli& cli) {
    cli.appendLine("ssh - SSH client");
    cli.appendLine("Usage:");
    cli.appendLine("  ssh list");
    cli.appendLine("  ssh connect [index]");
    cli.appendLine("  ssh [-p port] user@host");
    cli.appendLine("  ssh disconnect");
}

void helpSshList(Cli& cli) {
    cli.appendLine("ssh list - List saved SSH profiles");
    cli.appendLine("Usage: ssh list");
}

void helpSshConnect(Cli& cli) {
    cli.appendLine("ssh connect - Connect a saved profile");
    cli.appendLine("Usage: ssh connect");
    cli.appendLine("       ssh connect <index>");
}

void helpSshDisconnect(Cli& cli) {
    cli.appendLine("ssh disconnect - Close the current SSH session");
    cli.appendLine("Usage: ssh disconnect");
}

bool connectProfile(Cli& cli, size_t index) {
    if (cli.config() == nullptr || index >= cli.config()->ssh.size() || cli.ssh() == nullptr) {
        cli.appendLine("usage: ssh connect <index>");
        return true;
    }
    cli.config()->activeSsh = index;
    if (cli.settings()) cli.settings()->save(*cli.config());
    std::string error;
    cli.appendLine("Connecting SSH: " + cli.config()->ssh[index].user + "@" + cli.config()->ssh[index].host);
    if (!cli.ssh()->connect(cli.config()->ssh[index], error)) {
        cli.appendLine("SSH failed: " + error);
        return true;
    }
    cli.markSshSessionStart();
    cli.appendLine("SSH connected");
    return true;
}

bool connectDirect(Cli& cli, std::string rest) {
    rest = trimCopy(std::move(rest));
    SshProfile profile;
    profile.port = 22;
    profile.terminal = "xterm-256color";
    auto p = rest.find("-p ");
    if (p != std::string::npos) {
        std::string after = trimCopy(rest.substr(p + 3));
        const auto space = after.find(' ');
        const std::string port_text = space == std::string::npos ? after : after.substr(0, space);
        const int port = static_cast<int>(std::strtol(port_text.c_str(), nullptr, 10));
        if (port <= 0 || port > 65535) {
            cli.appendLine("Invalid SSH port");
            return true;
        }
        profile.port = static_cast<uint16_t>(port);
        rest = trimCopy(rest.substr(0, p) + (space == std::string::npos ? "" : after.substr(space)));
    }
    const auto at = rest.find('@');
    if (at == std::string::npos || at == 0) {
        cli.appendLine("usage: ssh [-p port] user@host");
        return true;
    }
    profile.user = rest.substr(0, at);
    profile.host = rest.substr(at + 1);
    const auto colon = profile.host.rfind(':');
    if (colon != std::string::npos && colon + 1 < profile.host.size()) {
        const std::string port_text = profile.host.substr(colon + 1);
        if (std::all_of(port_text.begin(), port_text.end(), [](unsigned char c) { return std::isdigit(c); })) {
            const int port = static_cast<int>(std::strtol(port_text.c_str(), nullptr, 10));
            if (port <= 0 || port > 65535) {
                cli.appendLine("Invalid SSH port");
                return true;
            }
            profile.port = static_cast<uint16_t>(port);
            profile.host = profile.host.substr(0, colon);
        }
    }
    profile.name = "direct " + profile.user + "@" + profile.host;
    cli.inheritCredentials(profile);
    std::string error;
    cli.appendLine("Connecting SSH: " + profile.user + "@" + profile.host);
    if (cli.ssh() == nullptr || !cli.ssh()->connect(profile, error)) {
        cli.appendLine("SSH failed: " + error);
        return true;
    }
    cli.markSshSessionStart();
    cli.appendLine("SSH connected");
    return true;
}

bool runSshList(Cli& cli, const CliArgs&) {
    if (cli.config()) {
        for (size_t i = 0; i < cli.config()->ssh.size(); ++i) {
            cli.appendLine(std::to_string(i) + "  " + cli.config()->ssh[i].name + "  " + cli.config()->ssh[i].user +
                           "@" + cli.config()->ssh[i].host);
        }
    }
    return true;
}

bool runSshConnect(Cli& cli, const CliArgs& args) {
    size_t index = cli.config() ? cli.config()->activeSsh : 0;
    if (!args.rest.empty() && !parseIndex(args.rest, index)) {
        cli.appendLine("usage: ssh connect <index>");
        return true;
    }
    return connectProfile(cli, index);
}

bool runSshDisconnect(Cli& cli, const CliArgs&) {
    if (cli.ssh()) cli.ssh()->disconnect();
    cli.appendLine("SSH disconnected");
    return true;
}

bool runSsh(Cli& cli, const CliArgs& args) {
    if (args.rest.empty()) {
        helpSsh(cli);
        return true;
    }
    if (args.rest.find('@') != std::string::npos) return connectDirect(cli, args.rest);
    cli.appendLine(std::string("ssh: unknown subcommand '") + args.tokens[1] + "'");
    helpSsh(cli);
    return true;
}

}  // namespace

void registerSshCommand(CliRegistry& registry) {
    static SimpleCommand list("list", "List saved SSH profiles", {}, runSshList, helpSshList);
    static SimpleCommand connect("connect", "Connect a saved profile", {}, runSshConnect, helpSshConnect);
    static SimpleCommand disconnect("disconnect", "Close the current SSH session", {}, runSshDisconnect,
                                    helpSshDisconnect);
    static SimpleCommand ssh("ssh", "SSH client", {}, runSsh, helpSsh);
    ssh.addSub(&list);
    ssh.addSub(&connect);
    ssh.addSub(&disconnect);
    registry.add(&ssh);
}

}  // namespace tabby
