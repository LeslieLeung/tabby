#include "tabby/keyboard_mapper.hpp"

#include <cctype>

namespace tabby {
namespace {
constexpr uint8_t kHidEnter = 0x28;
constexpr uint8_t kHidEscape = 0x29;
constexpr uint8_t kHidBackspace = 0x2A;
constexpr uint8_t kHidTab = 0x2B;
constexpr uint8_t kHidSpace = 0x2C;
constexpr uint8_t kHidPageUp = 0x4B;
constexpr uint8_t kHidPageDown = 0x4E;
constexpr uint8_t kHidUp = 0x52;
constexpr uint8_t kHidDown = 0x51;
constexpr uint8_t kHidLeft = 0x50;
constexpr uint8_t kHidRight = 0x4F;
}  // namespace

void KeyboardMapper::configure(const KeyboardConfig& config) { config_ = config; }

KeyAction KeyboardMapper::mapChar(char c) const {
    if (c == 0x1B) return {KeyActionType::Menu, "", 0};
    if (c == 0x11) return {KeyActionType::Scroll, "", kTerminalScrollStep};
    if (c == 0x12) return {KeyActionType::Scroll, "", -kTerminalScrollStep};
    return {KeyActionType::Text, std::string(1, translatePrintable(c)), 0};
}

KeyAction KeyboardMapper::mapHid(uint8_t modifier, uint8_t keycode) const {
    const bool shift = (modifier & 0x22) != 0;
    const bool ctrl = (modifier & 0x11) != 0;
    if (keycode == kHidPageUp) {
        if (shift || ctrl) return {KeyActionType::Scroll, "", kTerminalScrollPage};
        return {KeyActionType::Text, "\x1B[5~", 0};
    }
    if (keycode == kHidPageDown) {
        if (shift || ctrl) return {KeyActionType::Scroll, "", -kTerminalScrollPage};
        return {KeyActionType::Text, "\x1B[6~", 0};
    }
    if (keycode == kHidUp) {
        if (ctrl) return {KeyActionType::Scroll, "", kTerminalScrollStep};
        if (shift) return {KeyActionType::Scroll, "", 1};
        return {KeyActionType::Text, "\x1B[A", 0};
    }
    if (keycode == kHidDown) {
        if (ctrl) return {KeyActionType::Scroll, "", -kTerminalScrollStep};
        if (shift) return {KeyActionType::Scroll, "", -1};
        return {KeyActionType::Text, "\x1B[B", 0};
    }
    if (keycode == kHidEnter) return {KeyActionType::Text, "\r", 0};
    if (keycode == kHidEscape) return {KeyActionType::Menu, "", 0};
    if (keycode == kHidBackspace) return {KeyActionType::Text, std::string(1, static_cast<char>(0x7F)), 0};
    if (keycode == kHidTab) return {KeyActionType::Text, "\t", 0};
    if (keycode == kHidSpace) return {KeyActionType::Text, " ", 0};
    if (keycode == kHidRight) return {KeyActionType::Text, "\x1B[C", 0};
    if (keycode == kHidLeft) return {KeyActionType::Text, "\x1B[D", 0};

    if (keycode >= 0x04 && keycode <= 0x1D) {
        char c = static_cast<char>('a' + keycode - 0x04);
        if (shift) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (ctrl) c = static_cast<char>((std::tolower(static_cast<unsigned char>(c)) - 'a') + 1);
        return {KeyActionType::Text, std::string(1, c), 0};
    }

    char c = translateHidPrintable(keycode, shift);
    if (c) {
        if (ctrl && c == '[') return {KeyActionType::Text, std::string(1, static_cast<char>(0x1B)), 0};
        return {KeyActionType::Text, std::string(1, c), 0};
    }
    return {};
}

char KeyboardMapper::translatePrintable(char c) const {
    if (config_.layout == "jp") {
        if (c == '@') return '"';
        if (c == '"') return '@';
    }
    return c;
}

char KeyboardMapper::translateHidPrintable(uint8_t keycode, bool shift) const {
    static constexpr char kJpShifted[] = "!\"#$%&'() =~`{}+*<>?";
    const bool jp = config_.layout == "jp";
    if (keycode >= 0x1E && keycode <= 0x27) {
        static constexpr char kUsShifted[] = "!@#$%^&*()";
        char c = shift ? (jp ? kJpShifted[keycode - 0x1E] : kUsShifted[keycode - 0x1E])
                       : static_cast<char>('1' + keycode - 0x1E);
        return keycode == 0x27 && !shift ? '0' : c;
    }
    if (jp) {
        switch (keycode) {
            case 0x2D: return shift ? '=' : '-';
            case 0x2E: return shift ? '~' : '^';
            case 0x2F: return shift ? '`' : '@';
            case 0x30: return shift ? '{' : '[';
            case 0x31:
            case 0x32: return shift ? '}' : ']';
            case 0x33: return shift ? '+' : ';';
            case 0x34: return shift ? '*' : ':';
            case 0x35: return shift ? '~' : '`';
            case 0x36: return shift ? '<' : ',';
            case 0x37: return shift ? '>' : '.';
            case 0x38: return shift ? '?' : '/';
            case 0x87:
            case 0x89: return '\\';
            default: return 0;
        }
    }
    switch (keycode) {
        case 0x2D: return shift ? '_' : '-';
        case 0x2E: return shift ? '+' : '=';
        case 0x2F: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31:
        case 0x32: return shift ? '|' : '\\';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
        default: return 0;
    }
}

}  // namespace tabby
