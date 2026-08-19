#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"

namespace tabby {
namespace {

void helpVi(Cli& cli) {
    cli.appendLine("vi (vim, edit) - Edit a file on the SD card");
    cli.appendLine("Usage:");
    cli.appendLine("  vi");
    cli.appendLine("  vi <path>");
    cli.appendLine("  vim <path>");
    cli.appendLine("  edit <path>");
    cli.appendLine("Disconnect SSH and stop Python before editing. Esc leaves the editor.");
}

bool runVi(Cli& cli, const CliArgs& args) {
    if (cli.ssh() && cli.ssh()->connected()) {
        cli.appendLine("vi: disconnect SSH first");
        return true;
    }
    if (cli.python() && cli.python()->running()) {
        cli.appendLine("vi: python is running");
        return true;
    }
    if (cli.editor() == nullptr) {
        cli.appendLine("vi: unavailable");
        return true;
    }
    if (cli.editor()->active()) {
        cli.appendLine("vi: already open");
        return true;
    }
    std::string path = args.rest;
    std::string error;
    if (!cli.editor()->open(cli.sd(), path, error)) cli.appendLine(error.empty() ? "vi: failed" : "vi: " + error);
    return true;
}

}  // namespace

void registerViCommand(CliRegistry& registry) {
    static SimpleCommand vi("vi", "Edit a file on the SD card", {"vim", "edit"}, runVi, helpVi);
    vi.setInterceptHelp(false);
    registry.add(&vi);
}

}  // namespace tabby
