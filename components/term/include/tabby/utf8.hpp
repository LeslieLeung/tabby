#pragma once

#include <cstdint>
#include <string>

inline bool utf8IsContinuation(uint8_t c) {
    return (c & 0xC0) == 0x80;
}

inline uint8_t utf8ByteLen(uint8_t c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

inline bool utf8IsWide(uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) ||
           cp == 0x2329 || cp == 0x232A ||
           (cp >= 0x2E80 && cp <= 0xA4CF) ||
           (cp >= 0xAC00 && cp <= 0xD7A3) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE10 && cp <= 0xFE19) ||
           (cp >= 0xFE30 && cp <= 0xFE6F) ||
           (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6) ||
           (cp >= 0x1F300 && cp <= 0x1FAFF);
}

inline uint32_t utf8Next(const std::string& text, size_t& i) {
    if (i >= text.size()) return 0;
    const uint8_t c0 = static_cast<uint8_t>(text[i]);
    const uint8_t n = utf8ByteLen(c0);
    if (n == 1 || i + n > text.size()) {
        ++i;
        return c0;
    }
    uint32_t cp = 0;
    if (n == 2) {
        cp = ((c0 & 0x1F) << 6) | (static_cast<uint8_t>(text[i + 1]) & 0x3F);
    } else if (n == 3) {
        cp = ((c0 & 0x0F) << 12) | ((static_cast<uint8_t>(text[i + 1]) & 0x3F) << 6) |
             (static_cast<uint8_t>(text[i + 2]) & 0x3F);
    } else {
        cp = ((c0 & 0x07) << 18) | ((static_cast<uint8_t>(text[i + 1]) & 0x3F) << 12) |
             ((static_cast<uint8_t>(text[i + 2]) & 0x3F) << 6) | (static_cast<uint8_t>(text[i + 3]) & 0x3F);
    }
    i += n;
    return cp;
}

inline size_t utf8Prev(const std::string& text, size_t pos) {
    if (pos == 0 || text.empty()) return 0;
    if (pos > text.size()) pos = text.size();
    --pos;
    while (pos > 0 && utf8IsContinuation(static_cast<uint8_t>(text[pos]))) --pos;
    return pos;
}

inline size_t utf8Width(uint32_t cp) {
    return utf8IsWide(cp) ? 2 : 1;
}

inline size_t utf8DisplayCols(const std::string& text, size_t byte_end = static_cast<size_t>(-1)) {
    if (byte_end > text.size()) byte_end = text.size();
    size_t cols = 0;
    size_t i = 0;
    while (i < byte_end) {
        cols += utf8Width(utf8Next(text, i));
    }
    return cols;
}
