#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"
#include "tabby/cli/util.hpp"

#include <cstdio>
#include <cstdlib>

namespace tabby {
namespace {

int brightnessPercent(uint8_t value) {
    return (static_cast<int>(value) * 100 + DisplayConfig::kMaxBrightness / 2) / DisplayConfig::kMaxBrightness;
}

uint8_t brightnessFromPercent(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    const int span = DisplayConfig::kMaxBrightness - DisplayConfig::kMinBrightness;
    const uint8_t value =
        static_cast<uint8_t>(DisplayConfig::kMinBrightness + (span * percent + 50) / 100);
    return value;
}

void helpBrightness(Cli& cli) {
    cli.appendLine("brightness - Show or set display brightness");
    cli.appendLine("Usage: brightness");
    cli.appendLine("       brightness <0-100>");
}

bool runBrightness(Cli& cli, const CliArgs& args) {
    if (cli.config() == nullptr || cli.bsp() == nullptr) {
        cli.appendLine("brightness: unavailable");
        return true;
    }
    if (args.rest.empty()) {
        char line[32];
        std::snprintf(line, sizeof(line), "%d%%", brightnessPercent(cli.config()->display.brightness));
        cli.appendLine(line);
        return true;
    }
    if (!isAllDigits(trimCopy(args.rest))) {
        cli.appendLine("usage: brightness <0-100>");
        return true;
    }
    const int percent = static_cast<int>(std::strtol(args.rest.c_str(), nullptr, 10));
    if (percent < 0 || percent > 100) {
        cli.appendLine("usage: brightness <0-100>");
        return true;
    }
    const uint8_t value = brightnessFromPercent(percent);
    cli.bsp()->setBrightness(value);
    cli.config()->display.brightness = value;
    if (cli.settings()) cli.settings()->save(*cli.config());
    char line[32];
    std::snprintf(line, sizeof(line), "%d%%", brightnessPercent(value));
    cli.appendLine(line);
    return true;
}

}  // namespace

void registerBrightnessCommand(CliRegistry& registry) {
    static SimpleCommand brightness("brightness", "Show or set display brightness", {}, runBrightness, helpBrightness);
    brightness.setInterceptHelp(true);
    registry.add(&brightness);
}

}  // namespace tabby
