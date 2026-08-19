#pragma once

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace tabby {

class Cli;

struct CliArgs {
    std::string raw;
    std::string name;
    std::string rest;
    std::vector<std::string> tokens;
};

class Command {
public:
    virtual ~Command() = default;

    virtual const char* name() const = 0;
    virtual const char* summary() const = 0;
    virtual const std::vector<std::string>& aliases() const;
    virtual bool interceptHelp() const;
    virtual void help(Cli& cli) const;
    virtual bool run(Cli& cli, const CliArgs& args);

    void addSub(Command* command);
    Command* findSub(const std::string& name) const;
    const std::vector<Command*>& subs() const { return subs_; }

private:
    std::vector<Command*> subs_;
};

class SimpleCommand : public Command {
public:
    using RunFn = bool (*)(Cli&, const CliArgs&);
    using HelpFn = void (*)(Cli&);

    SimpleCommand(const char* name, const char* summary, std::initializer_list<const char*> aliases, RunFn run = nullptr,
                  HelpFn help = nullptr);

    const char* name() const override { return name_; }
    const char* summary() const override { return summary_; }
    const std::vector<std::string>& aliases() const override { return aliases_; }
    bool interceptHelp() const override;
    void help(Cli& cli) const override;
    bool run(Cli& cli, const CliArgs& args) override;

    void setInterceptHelp(bool value);

private:
    const char* name_{""};
    const char* summary_{""};
    std::vector<std::string> aliases_;
    RunFn run_{nullptr};
    HelpFn help_{nullptr};
    bool intercept_help_override_{false};
    bool intercept_help_{false};
};

class CliRegistry {
public:
    void add(Command* command);
    Command* find(const std::string& name) const;
    const std::vector<Command*>& commands() const { return commands_; }
    bool dispatch(Cli& cli, const std::string& line) const;

    static CliArgs parse(const std::string& line);
    static CliArgs shift(const CliArgs& args);
    static bool isHelpToken(const std::string& token);

private:
    std::vector<Command*> commands_;
    std::vector<std::pair<std::string, Command*>> index_;
};

void registerBuiltinCommands(CliRegistry& registry);

}  // namespace tabby
