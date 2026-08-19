#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"
#include "tabby/cli/util.hpp"

#include "esp_timer.h"

#include <cstdint>

namespace tabby {
namespace {

void splitPythonArgs(const std::string& rest, std::string& path, std::string& args) {
    const std::string trimmed = trimCopy(rest);
    const auto split = trimmed.find(' ');
    if (split == std::string::npos) {
        path = trimmed;
        args.clear();
        return;
    }
    path = trimmed.substr(0, split);
    args = trimCopy(trimmed.substr(split + 1));
}

void helpPython(Cli& cli) {
    cli.appendLine("python - MicroPython REPL and scripts");
    cli.appendLine("Usage:");
    cli.appendLine("  python");
    cli.appendLine("  python <sd.py> [args...]");
    cli.appendLine("  python -V | --version");
    cli.appendLine("  python -c <statement>");
    cli.appendLine("  python --reset");
    cli.appendLine("In the REPL, type exit() to return to Tabby CLI.");
}

bool runPython(Cli& cli, const CliArgs& args) {
    if (args.rest.empty()) {
        cli.setPythonRepl(true);
        cli.appendLine("MicroPython REPL. Type exit() to return.");
        return true;
    }
    const std::string lower = lowerCopy(args.rest);
    if (lower == "-h") {
        helpPython(cli);
        return true;
    }
    if (lower == "-v" || lower == "--version") {
        auto out = [&cli](const std::string& line) { cli.appendLine(line); };
        if (cli.python() && !cli.python()->runLine("import sys; print(sys.version)", out)) {
            cli.appendLine("python: " + cli.python()->lastError());
        }
        return true;
    }
    if (lower == "--reset") {
        if (cli.python()) cli.python()->reset();
        cli.appendLine("python: state reset");
        return true;
    }
    auto out = [&cli](const std::string& line) { cli.appendLine(line); };
    if (startsWith(lower, "-c ")) {
        if (cli.python() && !cli.python()->runLine(skipFirstToken(args.rest), out)) {
            cli.appendLine("python: " + cli.python()->lastError());
        }
        return true;
    }
    if (cli.sd() == nullptr || !cli.sd()->begin()) {
        cli.appendLine(cli.sd() ? "sd: " + cli.sd()->lastError() : "sd unavailable");
        return true;
    }
    std::string path_arg;
    std::string py_args;
    splitPythonArgs(args.rest, path_arg, py_args);
    const std::string path = cli.sd()->virtualPath(path_arg);
    bool is_dir = false;
    if (!cli.sd()->exists(path, &is_dir) || is_dir) {
        cli.appendLine("python: no such file: " + path);
        return true;
    }
    if (!cli.sd()->hasRead(path)) {
        cli.appendLine("python: permission denied: " + path);
        return true;
    }
    cli.appendLine(py_args.empty() ? "python " + path : "python " + path + " " + py_args);
    const int64_t start = esp_timer_get_time();
    const bool ok = cli.python() && cli.python()->runFile(cli.sd()->fsPath(path), py_args, out);
    const uint32_t ms = static_cast<uint32_t>((esp_timer_get_time() - start) / 1000);
    cli.appendLine(ok ? "python: done in " + std::to_string(ms) + " ms"
                      : "python: failed: " + (cli.python() ? cli.python()->lastError() : std::string("unavailable")));
    return true;
}

}  // namespace

void registerPythonCommand(CliRegistry& registry) {
    static SimpleCommand python("python", "MicroPython REPL and scripts", {}, runPython, helpPython);
    python.setInterceptHelp(true);
    registry.add(&python);
}

}  // namespace tabby
