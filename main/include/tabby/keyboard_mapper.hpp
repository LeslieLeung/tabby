#pragma once

#include "tabby/app_config.hpp"

#include <cstdint>
#include <string>

namespace tabby {

constexpr int kTerminalScrollStep = 5;
constexpr int kTerminalScrollPage = 10000;

enum class KeyActionType : uint8_t {
    None,
    Text,
    Control,
    Scroll,
    ConnectNext,
    ConnectPrevious,
    Menu,
};

struct KeyAction {
    KeyActionType type{KeyActionType::None};
    std::string text;
    int value{0};
};

class KeyboardMapper {
public:
    void configure(const KeyboardConfig& config);
    KeyAction mapChar(char c) const;
    KeyAction mapHid(uint8_t modifier, uint8_t keycode) const;

private:
    KeyboardConfig config_;
    char translatePrintable(char c) const;
    char translateHidPrintable(uint8_t keycode, bool shift) const;
};

}  // namespace tabby
