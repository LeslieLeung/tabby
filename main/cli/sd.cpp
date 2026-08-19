#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"
#include "tabby/cli/util.hpp"

namespace tabby {
namespace {

bool requireSd(Cli& cli) {
    if (cli.sd() == nullptr) {
        cli.appendLine("sd unavailable");
        return false;
    }
    return true;
}

void helpSd(Cli& cli) {
    cli.appendLine("sd - SD card filesystem");
    cli.appendLine("Usage:");
    cli.appendLine("  sd status");
    cli.appendLine("  sd df | df");
    cli.appendLine("  pwd | cd [path]");
    cli.appendLine("  ls [-lah] [path]");
    cli.appendLine("  cat | head | tail [-n N] <path>");
    cli.appendLine("  mkdir <path> | rmdir <path>");
    cli.appendLine("  rm <path> | cp <src> <dst> | mv <src> <dst> | touch <path>");
    cli.appendLine("  sd write <path> <text>");
    cli.appendLine("  sd append <path> <text>");
    cli.appendLine("  chmod <mode> <path>");
    cli.appendLine("  sd mount | unmount | usb [on|off]");
}

void helpSdStatus(Cli& cli) {
    cli.appendLine("sd status - Show card mount state and capacity");
    cli.appendLine("Usage: sd");
    cli.appendLine("       sd status");
}

void helpSdDf(Cli& cli) {
    cli.appendLine("df - Show SD card space");
    cli.appendLine("Usage: df");
    cli.appendLine("       sd df");
}

void helpSdPwd(Cli& cli) {
    cli.appendLine("pwd - Print the SD working directory");
    cli.appendLine("Usage: pwd");
    cli.appendLine("       sd pwd");
}

void helpSdCd(Cli& cli) {
    cli.appendLine("cd - Change the SD working directory");
    cli.appendLine("Usage: cd");
    cli.appendLine("       cd <path>");
    cli.appendLine("       sd cd <path>");
}

void helpSdLs(Cli& cli) {
    cli.appendLine("ls (dir) - List an SD directory");
    cli.appendLine("Usage: ls [-lah] [path]");
    cli.appendLine("       sd ls [-lah] [path]");
    cli.appendLine("Options: -l long  -a all  -h human-readable sizes");
}

void helpSdCat(Cli& cli) {
    cli.appendLine("cat - Print an SD file");
    cli.appendLine("Usage: cat <path>");
    cli.appendLine("       sd cat <path>");
}

void helpSdMkdir(Cli& cli) {
    cli.appendLine("mkdir - Create an SD directory");
    cli.appendLine("Usage: mkdir <path>");
    cli.appendLine("       sd mkdir <path>");
}

void helpSdRmdir(Cli& cli) {
    cli.appendLine("rmdir - Remove an empty SD directory");
    cli.appendLine("Usage: rmdir <path>");
    cli.appendLine("       sd rmdir <path>");
}

void helpSdRm(Cli& cli) {
    cli.appendLine("sd rm (rm) - Remove an SD file");
    cli.appendLine("Usage: rm <path>");
    cli.appendLine("       sd rm <path>");
}

void helpSdWrite(Cli& cli) {
    cli.appendLine("sd write - Overwrite a text file on the SD card");
    cli.appendLine("Usage: sd write <path> <text>");
}

void helpSdAppend(Cli& cli) {
    cli.appendLine("sd append - Append text to an SD file");
    cli.appendLine("Usage: sd append <path> <text>");
}

void helpSdChmod(Cli& cli) {
    cli.appendLine("chmod - Set SD file permissions");
    cli.appendLine("Usage: chmod <mode> <path>");
    cli.appendLine("       sd chmod <mode> <path>");
}

bool runSdStatus(Cli& cli, const CliArgs&) {
    if (!requireSd(cli)) return true;
    cli.appendText(cli.sd()->statusText());
    return true;
}

bool runSdDf(Cli& cli, const CliArgs&) {
    if (!requireSd(cli)) return true;
    cli.appendText(cli.sd()->dfText());
    return true;
}

bool runSdPwd(Cli& cli, const CliArgs&) {
    if (!requireSd(cli)) return true;
    cli.appendLine(cli.sd()->cwd());
    return true;
}

bool runSdCd(Cli& cli, const CliArgs& args) {
    if (!requireSd(cli)) return true;
    std::string message;
    cli.sd()->cd(args.rest.empty() ? "/" : args.rest, message);
    cli.appendLine(message);
    return true;
}

bool runSdLs(Cli& cli, const CliArgs& args) {
    if (!requireSd(cli)) return true;
    std::vector<std::string> lines;
    std::string error;
    if (!cli.sd()->list(args.rest, lines, error)) {
        cli.appendLine(error);
    } else {
        for (const auto& line : lines) cli.appendLine(line);
    }
    return true;
}

bool runSdCat(Cli& cli, const CliArgs& args) {
    if (!requireSd(cli)) return true;
    if (args.rest.empty()) {
        cli.appendLine("usage: cat <path>");
        return true;
    }
    std::vector<std::string> lines;
    std::string error;
    if (!cli.sd()->cat(args.rest, lines, error)) {
        cli.appendLine(error);
    } else {
        for (const auto& line : lines) cli.appendLine(line);
    }
    return true;
}

bool runSdMkdir(Cli& cli, const CliArgs& args) {
    if (!requireSd(cli)) return true;
    if (args.rest.empty()) {
        cli.appendLine("usage: mkdir <path>");
        return true;
    }
    std::string message;
    cli.sd()->mkdir(args.rest, message);
    cli.appendLine(message);
    return true;
}

bool runSdRmdir(Cli& cli, const CliArgs& args) {
    if (!requireSd(cli)) return true;
    if (args.rest.empty()) {
        cli.appendLine("usage: rmdir <path>");
        return true;
    }
    std::string message;
    cli.sd()->rmdir(args.rest, message);
    cli.appendLine(message);
    return true;
}

bool runSdRm(Cli& cli, const CliArgs& args) {
    if (!requireSd(cli)) return true;
    if (args.rest.empty()) {
        cli.appendLine("usage: rm <path>");
        return true;
    }
    std::string message;
    cli.sd()->removeFile(args.rest, message);
    cli.appendLine(message);
    return true;
}

bool runSdWriteOrAppend(Cli& cli, const CliArgs& args, bool append) {
    if (!requireSd(cli)) return true;
    const auto split = args.rest.find(' ');
    if (args.rest.empty() || split == std::string::npos) {
        cli.appendLine(std::string("usage: ") + (append ? "sd append" : "sd write") + " <path> <text>");
        return true;
    }
    std::string message;
    cli.sd()->writeText(args.rest.substr(0, split), args.rest.substr(split + 1), append, message);
    cli.appendLine(message);
    return true;
}

bool runSdWrite(Cli& cli, const CliArgs& args) { return runSdWriteOrAppend(cli, args, false); }
bool runSdAppend(Cli& cli, const CliArgs& args) { return runSdWriteOrAppend(cli, args, true); }

bool runSdChmod(Cli& cli, const CliArgs& args) {
    if (!requireSd(cli)) return true;
    const std::string rest = trimCopy(args.rest);
    const auto split = rest.find(' ');
    if (split == std::string::npos) {
        cli.appendLine("chmod: usage: chmod <mode> <path>");
        return true;
    }
    std::string message;
    cli.sd()->chmod(rest.substr(0, split), rest.substr(split + 1), message);
    cli.appendLine(message);
    return true;
}

void helpSdCp(Cli& cli) {
    cli.appendLine("cp - Copy an SD file");
    cli.appendLine("Usage: cp <src> <dst>");
}

void helpSdMv(Cli& cli) {
    cli.appendLine("mv - Rename or move an SD file");
    cli.appendLine("Usage: mv <src> <dst>");
}

void helpSdTouch(Cli& cli) {
    cli.appendLine("touch - Create an empty SD file or update it");
    cli.appendLine("Usage: touch <path>");
}

void helpSdHead(Cli& cli) {
    cli.appendLine("head - Show the first lines of an SD file");
    cli.appendLine("Usage: head [-n N] <path>");
}

void helpSdTail(Cli& cli) {
    cli.appendLine("tail - Show the last lines of an SD file");
    cli.appendLine("Usage: tail [-n N] <path>");
}

void helpSdMount(Cli& cli) {
    cli.appendLine("sd mount - Mount the SD card");
    cli.appendLine("Usage: sd mount");
}

void helpSdUnmount(Cli& cli) {
    cli.appendLine("sd unmount - Unmount the SD card");
    cli.appendLine("Usage: sd unmount");
}

void helpSdUsb(Cli& cli) {
    cli.appendLine("sd usb - USB flash-drive mode");
    cli.appendLine("Usage: sd usb");
    cli.appendLine("       sd usb on");
    cli.appendLine("       sd usb off");
}

bool parseCountPath(Cli& cli, const CliArgs& args, size_t& count, std::string& path, const char* usage) {
    count = 10;
    const auto tokens = tokenize(args.rest);
    if (tokens.empty()) {
        cli.appendLine(usage);
        return false;
    }
    if (tokens[0] == "-n") {
        if (tokens.size() < 3 || !parseIndex(tokens[1], count) || count == 0) {
            cli.appendLine(usage);
            return false;
        }
        path = skipFirstToken(skipFirstToken(args.rest));
        return true;
    }
    path = args.rest;
    return true;
}

bool runSdCp(Cli& cli, const CliArgs& args) {
    if (!requireSd(cli)) return true;
    std::string src;
    std::string dst;
    if (!splitTwoArgs(args.rest, src, dst)) {
        cli.appendLine("usage: cp <src> <dst>");
        return true;
    }
    std::string message;
    cli.sd()->copyFile(src, dst, message);
    cli.appendLine(message);
    return true;
}

bool runSdMv(Cli& cli, const CliArgs& args) {
    if (!requireSd(cli)) return true;
    std::string src;
    std::string dst;
    if (!splitTwoArgs(args.rest, src, dst)) {
        cli.appendLine("usage: mv <src> <dst>");
        return true;
    }
    std::string message;
    cli.sd()->moveFile(src, dst, message);
    cli.appendLine(message);
    return true;
}

bool runSdTouch(Cli& cli, const CliArgs& args) {
    if (!requireSd(cli)) return true;
    if (args.rest.empty()) {
        cli.appendLine("usage: touch <path>");
        return true;
    }
    std::string message;
    cli.sd()->touch(args.rest, message);
    cli.appendLine(message);
    return true;
}

bool runSdHead(Cli& cli, const CliArgs& args) {
    if (!requireSd(cli)) return true;
    size_t count = 10;
    std::string path;
    if (!parseCountPath(cli, args, count, path, "usage: head [-n N] <path>")) return true;
    std::vector<std::string> lines;
    std::string error;
    if (!cli.sd()->head(path, count, lines, error)) cli.appendLine(error);
    else
        for (const auto& line : lines) cli.appendLine(line);
    return true;
}

bool runSdTail(Cli& cli, const CliArgs& args) {
    if (!requireSd(cli)) return true;
    size_t count = 10;
    std::string path;
    if (!parseCountPath(cli, args, count, path, "usage: tail [-n N] <path>")) return true;
    std::vector<std::string> lines;
    std::string error;
    if (!cli.sd()->tail(path, count, lines, error)) cli.appendLine(error);
    else
        for (const auto& line : lines) cli.appendLine(line);
    return true;
}

bool runSdMount(Cli& cli, const CliArgs&) {
    if (cli.sd() == nullptr) {
        cli.appendLine("sd unavailable");
        return true;
    }
    if (cli.sd()->usbMode()) {
        cli.appendLine("sd: disconnect USB drive mode first");
        return true;
    }
    cli.appendLine(cli.sd()->mount() ? "sd mounted" : "sd: " + cli.sd()->lastError());
    return true;
}

bool runSdUnmount(Cli& cli, const CliArgs&) {
    if (cli.sd() == nullptr) {
        cli.appendLine("sd unavailable");
        return true;
    }
    if (cli.sd()->usbMode()) {
        cli.appendLine("sd: disconnect USB drive mode first");
        return true;
    }
    cli.appendLine(cli.sd()->unmount() ? "sd unmounted" : "sd: " + cli.sd()->lastError());
    return true;
}

bool runSdUsb(Cli& cli, const CliArgs& args) {
    if (cli.usb() == nullptr || cli.sd() == nullptr) {
        cli.appendLine("usb: unavailable");
        return true;
    }
    const std::string action = lowerCopy(args.rest);
    if (action.empty()) {
        if (cli.usb()->active() && cli.usb()->hostAttached()) cli.appendLine("usb: connected to computer");
        else if (cli.usb()->active()) cli.appendLine("usb: waiting for computer");
        else cli.appendLine("usb: off");
        return true;
    }
    if (action == "on") {
        cli.appendLine("Starting USB drive mode...");
        cli.appendLine(cli.usb()->start() ? "usb: on" : "usb: " + cli.usb()->lastError());
        return true;
    }
    if (action == "off") {
        cli.appendLine(cli.usb()->stop() ? "usb: off" : "usb: " + cli.usb()->lastError());
        return true;
    }
    cli.appendLine("usage: sd usb [on|off]");
    return true;
}

bool runSd(Cli& cli, const CliArgs& args) {
    if (args.rest.empty()) return runSdStatus(cli, args);
    cli.appendLine(std::string("sd: unknown subcommand '") + args.tokens[1] + "'");
    helpSd(cli);
    return true;
}

}  // namespace

void registerSdCommand(CliRegistry& registry) {
    static SimpleCommand status("status", "Show card mount state and capacity", {}, runSdStatus, helpSdStatus);
    static SimpleCommand df("df", "Show SD card space", {}, runSdDf, helpSdDf);
    static SimpleCommand pwd("pwd", "Print the SD working directory", {}, runSdPwd, helpSdPwd);
    static SimpleCommand cd("cd", "Change the SD working directory", {}, runSdCd, helpSdCd);
    static SimpleCommand ls("ls", "List an SD directory", {"dir"}, runSdLs, helpSdLs);
    static SimpleCommand cat("cat", "Print an SD file", {}, runSdCat, helpSdCat);
    static SimpleCommand head("head", "Show the first lines of an SD file", {}, runSdHead, helpSdHead);
    static SimpleCommand tail("tail", "Show the last lines of an SD file", {}, runSdTail, helpSdTail);
    static SimpleCommand mkdir("mkdir", "Create an SD directory", {}, runSdMkdir, helpSdMkdir);
    static SimpleCommand rmdir("rmdir", "Remove an empty SD directory", {}, runSdRmdir, helpSdRmdir);
    static SimpleCommand rm("rm", "Remove an SD file", {}, runSdRm, helpSdRm);
    static SimpleCommand cp("cp", "Copy an SD file", {}, runSdCp, helpSdCp);
    static SimpleCommand mv("mv", "Rename or move an SD file", {}, runSdMv, helpSdMv);
    static SimpleCommand touch("touch", "Create an empty SD file", {}, runSdTouch, helpSdTouch);
    static SimpleCommand write("write", "Overwrite a text file on the SD card", {}, runSdWrite, helpSdWrite);
    static SimpleCommand append("append", "Append text to an SD file", {}, runSdAppend, helpSdAppend);
    static SimpleCommand chmod("chmod", "Set SD file permissions", {}, runSdChmod, helpSdChmod);
    static SimpleCommand mount("mount", "Mount the SD card", {}, runSdMount, helpSdMount);
    static SimpleCommand unmount("unmount", "Unmount the SD card", {}, runSdUnmount, helpSdUnmount);
    static SimpleCommand usb("usb", "USB flash-drive mode", {}, runSdUsb, helpSdUsb);
    static SimpleCommand sd("sd", "SD card filesystem", {}, runSd, helpSd);

    sd.addSub(&status);
    sd.addSub(&df);
    sd.addSub(&pwd);
    sd.addSub(&cd);
    sd.addSub(&ls);
    sd.addSub(&cat);
    sd.addSub(&head);
    sd.addSub(&tail);
    sd.addSub(&mkdir);
    sd.addSub(&rmdir);
    sd.addSub(&rm);
    sd.addSub(&cp);
    sd.addSub(&mv);
    sd.addSub(&touch);
    sd.addSub(&write);
    sd.addSub(&append);
    sd.addSub(&chmod);
    sd.addSub(&mount);
    sd.addSub(&unmount);
    sd.addSub(&usb);

    ls.setInterceptHelp(true);
    df.setInterceptHelp(true);
    pwd.setInterceptHelp(true);
    rm.setInterceptHelp(true);
    cp.setInterceptHelp(true);
    mv.setInterceptHelp(true);
    touch.setInterceptHelp(true);
    head.setInterceptHelp(true);
    tail.setInterceptHelp(true);

    registry.add(&sd);
    registry.add(&df);
    registry.add(&pwd);
    registry.add(&cd);
    registry.add(&ls);
    registry.add(&cat);
    registry.add(&head);
    registry.add(&tail);
    registry.add(&mkdir);
    registry.add(&rmdir);
    registry.add(&rm);
    registry.add(&cp);
    registry.add(&mv);
    registry.add(&touch);
    registry.add(&chmod);
}

}  // namespace tabby
