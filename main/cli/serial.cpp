#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"
#include "tabby/cli/util.hpp"

#include <cstdlib>

namespace tabby {
namespace {
constexpr uint32_t kDefaultBaud = 115200;
constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr uint32_t kMinBaud = 1200;
constexpr uint32_t kMaxBaud = 3000000;

void helpSerial(Cli& cli) {
    cli.appendLine("serial - USB-A serial monitor");
    cli.appendLine("Usage:");
    cli.appendLine("  serial");
    cli.appendLine("  serial <baud>");
    cli.appendLine("  serial off");
    cli.appendLine("Plug an ESP32 or USB-UART adapter into USB-A, then run serial.");
    cli.appendLine("Press Esc or Ctrl+C to leave the session. Baud is ignored for USB Serial/JTAG.");
}

bool parseBaud(const std::string& text, uint32_t& baud) {
    if (!isAllDigits(text)) return false;
    const unsigned long value = std::strtoul(text.c_str(), nullptr, 10);
    if (value < kMinBaud || value > kMaxBaud) return false;
    baud = static_cast<uint32_t>(value);
    return true;
}

bool runSerialOff(Cli& cli, const CliArgs&) {
    if (cli.serial() == nullptr) {
        cli.appendLine("serial: unavailable");
        return true;
    }
    cli.serial()->disconnect();
    cli.appendLine("USB serial closed");
    return true;
}

bool runSerial(Cli& cli, const CliArgs& args) {
    if (cli.serial() == nullptr) {
        cli.appendLine("serial: unavailable");
        return true;
    }
    const std::string rest = trimCopy(args.rest);
    if (rest == "off" || rest == "disconnect" || rest == "close") return runSerialOff(cli, args);

    uint32_t baud = cli.serial()->connected() ? cli.serial()->baud() : kDefaultBaud;
    if (!rest.empty() && !parseBaud(rest, baud)) {
        cli.appendLine("usage: serial [baud] | serial off");
        return true;
    }
    if (cli.ssh() && cli.ssh()->connected()) {
        cli.appendLine("serial: disconnect SSH first");
        return true;
    }
    if (cli.editor() && cli.editor()->active()) {
        cli.appendLine("serial: close vi first");
        return true;
    }
    if (cli.python() && cli.python()->running()) {
        cli.appendLine("serial: python is running");
        return true;
    }

    cli.appendLine("Waiting for USB serial device on USB-A...");
    std::string error;
    if (!cli.serial()->connect(baud, kConnectTimeoutMs, error, cli.interruptFlag())) {
        cli.appendLine("serial: " + (error.empty() ? std::string("failed") : error));
        return true;
    }
    cli.markSerialSessionStart();
    cli.appendLine("USB serial connected (" + cli.serial()->status() + ")");
    return true;
}

}  // namespace

void registerSerialCommand(CliRegistry& registry) {
    static SimpleCommand serial("serial", "USB-A serial monitor", {}, runSerial, helpSerial);
    serial.setInterceptHelp(true);
    registry.add(&serial);
}

}  // namespace tabby
