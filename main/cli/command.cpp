#include "tabby/cli/command.hpp"

#include "tabby/cli.hpp"
#include "tabby/cli/util.hpp"

#include <cstdio>

namespace tabby {
namespace {

const std::vector<std::string>& emptyAliases() {
    static const std::vector<std::string> empty;
    return empty;
}

}  // namespace

const std::vector<std::string>& Command::aliases() const { return emptyAliases(); }

bool Command::interceptHelp() const { return !subs_.empty(); }

void Command::addSub(Command* command) {
    if (command == nullptr) return;
    for (Command* existing : subs_) {
        if (existing == command) return;
    }
    subs_.push_back(command);
}

Command* Command::findSub(const std::string& name) const {
    const std::string key = lowerCopy(name);
    for (Command* sub : subs_) {
        if (lowerCopy(sub->name()) == key) return sub;
        for (const auto& alias : sub->aliases()) {
            if (lowerCopy(alias) == key) return sub;
        }
    }
    return nullptr;
}

void Command::help(Cli& cli) const {
    std::string title = name();
    if (!aliases().empty()) {
        title += " (";
        for (size_t i = 0; i < aliases().size(); ++i) {
            if (i > 0) title += ", ";
            title += aliases()[i];
        }
        title += ")";
    }
    cli.appendLine(title + " - " + summary());
    if (subs_.empty()) return;
    cli.appendLine("Subcommands:");
    for (Command* sub : subs_) {
        std::string label = sub->name();
        for (const auto& alias : sub->aliases()) {
            label += ", ";
            label += alias;
        }
        char line[192];
        std::snprintf(line, sizeof(line), "  %-22s %s", label.c_str(), sub->summary());
        cli.appendLine(line);
    }
    cli.appendLine(std::string("Try '") + name() + " <subcommand> help' for details.");
}

bool Command::run(Cli& cli, const CliArgs& args) {
    if (args.rest.empty()) {
        help(cli);
        return true;
    }
    const std::string unknown = args.tokens.size() >= 2 ? args.tokens[1] : args.rest;
    cli.appendLine(std::string(name()) + ": unknown subcommand '" + unknown + "'");
    help(cli);
    return true;
}

SimpleCommand::SimpleCommand(const char* name, const char* summary, std::initializer_list<const char*> aliases, RunFn run,
                             HelpFn help)
    : name_(name != nullptr ? name : ""), summary_(summary != nullptr ? summary : ""), run_(run), help_(help) {
    aliases_.reserve(aliases.size());
    for (const char* alias : aliases) {
        if (alias != nullptr && alias[0] != '\0') aliases_.emplace_back(alias);
    }
}

bool SimpleCommand::interceptHelp() const {
    if (intercept_help_override_) return intercept_help_;
    return Command::interceptHelp();
}

void SimpleCommand::setInterceptHelp(bool value) {
    intercept_help_override_ = true;
    intercept_help_ = value;
}

void SimpleCommand::help(Cli& cli) const {
    if (help_ != nullptr) {
        help_(cli);
        return;
    }
    Command::help(cli);
}

bool SimpleCommand::run(Cli& cli, const CliArgs& args) {
    if (run_ != nullptr) return run_(cli, args);
    return Command::run(cli, args);
}

void CliRegistry::add(Command* command) {
    if (command == nullptr) return;
    commands_.push_back(command);
    auto add_name = [&](const std::string& name) { index_.push_back({lowerCopy(name), command}); };
    add_name(command->name());
    for (const auto& alias : command->aliases()) add_name(alias);
}

Command* CliRegistry::find(const std::string& name) const {
    const std::string key = lowerCopy(name);
    for (const auto& entry : index_) {
        if (entry.first == key) return entry.second;
    }
    return nullptr;
}

bool CliRegistry::isHelpToken(const std::string& token) {
    const std::string lower = lowerCopy(token);
    return lower == "help" || lower == "--help";
}

CliArgs CliRegistry::parse(const std::string& line) {
    CliArgs args;
    args.raw = trimCopy(line);
    args.tokens = tokenize(args.raw);
    if (!args.tokens.empty()) {
        args.name = args.tokens[0];
        args.rest = skipFirstToken(args.raw);
    }
    return args;
}

CliArgs CliRegistry::shift(const CliArgs& args) {
    CliArgs out;
    out.raw = args.raw;
    if (args.tokens.size() < 2) return out;
    out.name = args.tokens[1];
    out.tokens.assign(args.tokens.begin() + 1, args.tokens.end());
    out.rest = skipFirstToken(args.rest);
    return out;
}

bool CliRegistry::dispatch(Cli& cli, const std::string& line) const {
    const CliArgs args = parse(line);
    if (args.name.empty()) return true;
    Command* cmd = find(args.name);
    if (cmd == nullptr) return false;

    if (cmd->interceptHelp() && args.tokens.size() >= 2 && isHelpToken(args.tokens[1])) {
        if (args.tokens.size() >= 3) {
            if (Command* sub = cmd->findSub(args.tokens[2])) {
                sub->help(cli);
                return true;
            }
        }
        cmd->help(cli);
        return true;
    }

    if (args.tokens.size() >= 2) {
        if (Command* sub = cmd->findSub(args.tokens[1])) {
            const CliArgs sub_args = shift(args);
            if (sub->interceptHelp() && sub_args.tokens.size() >= 2 && isHelpToken(sub_args.tokens[1])) {
                sub->help(cli);
                return true;
            }
            return sub->run(cli, sub_args);
        }
    }

    return cmd->run(cli, args);
}

}  // namespace tabby
