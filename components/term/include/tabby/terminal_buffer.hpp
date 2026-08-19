#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

class TerminalBuffer {
public:
    explicit TerminalBuffer(size_t maxLines = 2000);

    void setViewport(size_t columns, size_t rows);
    void clear();
    void append(const char* data, size_t len);
    void append(const std::string& text);
    void inputEcho(char c);
    void backspace();
    void scroll(int delta);
    void scrollToBottom();
    std::string lineAt(size_t viewportRow) const;
    size_t inputViewportRow() const;
    size_t viewportRows() const;
    size_t totalRows() const;
    size_t scrollOffset() const;
    bool atBottom() const;
    uint32_t revision() const;

private:
    void pushLine(std::string line);
    void appendChar(char c);
    void trim();

    size_t max_lines_;
    size_t columns_{100};
    size_t rows_{32};
    size_t scroll_offset_{0};
    uint8_t escape_state_{0};
    mutable std::recursive_mutex mutex_;
    uint32_t revision_{0};
    std::deque<std::string> lines_;
    std::string current_;
};
