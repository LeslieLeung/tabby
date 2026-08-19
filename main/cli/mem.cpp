#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"

#include "esp_heap_caps.h"

#include <cstdio>

namespace tabby {
namespace {

void helpMem(Cli& cli) {
    cli.appendLine("free (mem) - Show heap memory");
    cli.appendLine("Usage: free");
    cli.appendLine("       mem");
    cli.appendLine("SD card space is 'df'.");
}

bool runMem(Cli& cli, const CliArgs&) {
    multi_heap_info_t intern {};
    multi_heap_info_t spiram {};
    heap_caps_get_info(&intern, MALLOC_CAP_INTERNAL);
    heap_caps_get_info(&spiram, MALLOC_CAP_SPIRAM);
    const size_t min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    char line[96];
    std::snprintf(line, sizeof(line), "internal %u free / %u used", static_cast<unsigned>(intern.total_free_bytes),
                  static_cast<unsigned>(intern.total_allocated_bytes));
    cli.appendLine(line);
    std::snprintf(line, sizeof(line), "spiram   %u free / %u used", static_cast<unsigned>(spiram.total_free_bytes),
                  static_cast<unsigned>(spiram.total_allocated_bytes));
    cli.appendLine(line);
    std::snprintf(line, sizeof(line), "min free %u", static_cast<unsigned>(min_free));
    cli.appendLine(line);
    return true;
}

}  // namespace

void registerMemCommand(CliRegistry& registry) {
    static SimpleCommand mem("free", "Show heap memory", {"mem"}, runMem, helpMem);
    registry.add(&mem);
}

}  // namespace tabby
