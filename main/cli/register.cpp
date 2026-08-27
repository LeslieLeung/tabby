#include "tabby/cli/command.hpp"

namespace tabby {

void registerHelpCommand(CliRegistry& registry);
void registerClearCommand(CliRegistry& registry);
void registerStatusCommand(CliRegistry& registry);
void registerHistoryCommand(CliRegistry& registry);
void registerEchoCommand(CliRegistry& registry);
void registerWhoamiCommand(CliRegistry& registry);
void registerHostnameCommand(CliRegistry& registry);
void registerUnameCommand(CliRegistry& registry);
void registerUptimeCommand(CliRegistry& registry);
void registerDateCommand(CliRegistry& registry);
void registerTimeCommand(CliRegistry& registry);
void registerBatteryCommand(CliRegistry& registry);
void registerBrightnessCommand(CliRegistry& registry);
void registerRebootCommand(CliRegistry& registry);
void registerMemCommand(CliRegistry& registry);
void registerKeyboardCommand(CliRegistry& registry);
void registerWifiCommand(CliRegistry& registry);
void registerIpCommand(CliRegistry& registry);
void registerPingCommand(CliRegistry& registry);
void registerSshCommand(CliRegistry& registry);
void registerSerialCommand(CliRegistry& registry);
void registerScpCommand(CliRegistry& registry);
void registerPythonCommand(CliRegistry& registry);
void registerViCommand(CliRegistry& registry);
void registerSdCommand(CliRegistry& registry);
void registerWgetCommand(CliRegistry& registry);

void registerBuiltinCommands(CliRegistry& registry) {
    registerHelpCommand(registry);
    registerClearCommand(registry);
    registerStatusCommand(registry);
    registerHistoryCommand(registry);
    registerEchoCommand(registry);
    registerWhoamiCommand(registry);
    registerHostnameCommand(registry);
    registerUnameCommand(registry);
    registerUptimeCommand(registry);
    registerDateCommand(registry);
    registerTimeCommand(registry);
    registerBatteryCommand(registry);
    registerBrightnessCommand(registry);
    registerRebootCommand(registry);
    registerMemCommand(registry);
    registerKeyboardCommand(registry);
    registerWifiCommand(registry);
    registerIpCommand(registry);
    registerPingCommand(registry);
    registerSshCommand(registry);
    registerSerialCommand(registry);
    registerScpCommand(registry);
    registerPythonCommand(registry);
    registerViCommand(registry);
    registerSdCommand(registry);
    registerWgetCommand(registry);
}

}  // namespace tabby
