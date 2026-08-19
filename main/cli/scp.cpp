#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"
#include "tabby/cli/util.hpp"

#include <cstdlib>

namespace tabby {
namespace {

void helpScp(Cli& cli) {
    cli.appendLine("scp - Copy files over SSH");
    cli.appendLine("Usage:");
    cli.appendLine("  scp get <remote> <sd-local> [profile-index]");
    cli.appendLine("  scp put <sd-local> <remote> [profile-index]");
    cli.appendLine("  scp get user@host:/remote <sd-local> [password]");
    cli.appendLine("  scp put <sd-local> user@host:/remote [password]");
}

void helpScpGet(Cli& cli) {
    cli.appendLine("scp get - Download a remote file onto the SD card");
    cli.appendLine("Usage:");
    cli.appendLine("  scp get <remote> <sd-local> [profile-index]");
    cli.appendLine("  scp get user@host:/remote <sd-local> [password]");
}

void helpScpPut(Cli& cli) {
    cli.appendLine("scp put - Upload an SD file to a remote host");
    cli.appendLine("Usage:");
    cli.appendLine("  scp put <sd-local> <remote> [profile-index]");
    cli.appendLine("  scp put <sd-local> user@host:/remote [password]");
}

bool parseDirectScp(const std::string& endpoint, SshProfile& profile, std::string& remote_path) {
    const auto at = endpoint.find('@');
    const auto colon = endpoint.find(':', at == std::string::npos ? 0 : at + 1);
    if (at == std::string::npos || at == 0 || colon == std::string::npos || colon <= at + 1 ||
        colon + 1 >= endpoint.size()) {
        return false;
    }
    profile = {};
    profile.user = endpoint.substr(0, at);
    profile.host = endpoint.substr(at + 1, colon - at - 1);
    profile.port = 22;
    profile.terminal = "xterm-256color";
    remote_path = endpoint.substr(colon + 1);
    profile.name = "direct " + profile.user + "@" + profile.host;
    return !profile.user.empty() && !profile.host.empty() && !remote_path.empty();
}

bool splitTwo(const std::string& rest_in, std::string& first, std::string& second, std::string& trailing) {
    std::string rest = trimCopy(rest_in);
    auto split = rest.find(' ');
    if (split == std::string::npos) return false;
    first = rest.substr(0, split);
    rest = trimCopy(rest.substr(split + 1));
    split = rest.find(' ');
    if (split == std::string::npos) {
        second = rest;
        trailing.clear();
    } else {
        second = rest.substr(0, split);
        trailing = trimCopy(rest.substr(split + 1));
    }
    return !first.empty() && !second.empty();
}

bool runScpTransfer(Cli& cli, const CliArgs& args, bool is_get) {
    if (cli.sd() == nullptr || !cli.sd()->ready()) {
        cli.appendLine(cli.sd() ? "sd: " + cli.sd()->lastError() : "sd unavailable");
        if (cli.sd()) cli.sd()->begin();
        if (cli.sd() == nullptr || !cli.sd()->ready()) return true;
    }
    std::string first;
    std::string second;
    std::string trailing;
    if (!splitTwo(args.rest, first, second, trailing)) {
        cli.appendLine(std::string("usage: scp ") + (is_get ? "get <remote> <sd-local>" : "put <sd-local> <remote>"));
        return true;
    }
    SshProfile profile;
    std::string remote_path;
    std::string local_path;
    const bool direct = is_get ? parseDirectScp(first, profile, remote_path) : parseDirectScp(second, profile, remote_path);
    if (direct) {
        if (!trailing.empty()) profile.password = trailing;
        cli.inheritCredentials(profile);
        local_path = is_get ? second : first;
    } else {
        if (cli.config() == nullptr || cli.config()->ssh.empty()) {
            cli.appendLine("scp: no SSH profiles");
            return true;
        }
        size_t index = cli.config()->activeSsh;
        if (isAllDigits(trailing)) index = static_cast<size_t>(std::strtoul(trailing.c_str(), nullptr, 10));
        if (index >= cli.config()->ssh.size()) {
            cli.appendLine("scp: invalid profile index");
            return true;
        }
        profile = cli.config()->ssh[index];
        remote_path = is_get ? first : second;
        local_path = is_get ? second : first;
    }
    std::string error;
    cli.appendLine(std::string("scp ") + (is_get ? "get " : "put ") + profile.user + "@" + profile.host);
    if (cli.ssh() == nullptr) {
        cli.appendLine("scp: ssh unavailable");
        return true;
    }
    const bool ok =
        is_get ? cli.ssh()->scpDownload(profile, remote_path, cli.sd()->fsPath(cli.sd()->virtualPath(local_path)), error,
                                        cli.interruptFlag())
               : cli.ssh()->scpUpload(profile, cli.sd()->fsPath(cli.sd()->virtualPath(local_path)), remote_path, error,
                                      cli.interruptFlag());
    cli.appendLine(ok ? "scp: done" : "scp: failed: " + error);
    return true;
}

bool runScpGet(Cli& cli, const CliArgs& args) { return runScpTransfer(cli, args, true); }
bool runScpPut(Cli& cli, const CliArgs& args) { return runScpTransfer(cli, args, false); }

}  // namespace

void registerScpCommand(CliRegistry& registry) {
    static SimpleCommand get("get", "Download a remote file onto the SD card", {}, runScpGet, helpScpGet);
    static SimpleCommand put("put", "Upload an SD file to a remote host", {}, runScpPut, helpScpPut);
    static SimpleCommand scp("scp", "Copy files over SSH", {}, nullptr, helpScp);
    scp.addSub(&get);
    scp.addSub(&put);
    registry.add(&scp);
}

}  // namespace tabby
