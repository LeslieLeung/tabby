#include "tabby/terminal_buffer.hpp"
#include "tabby/utf8.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

namespace {
size_t displayCells(const std::string& text)
{
    return utf8DisplayCols(text);
}

size_t wrapByteIndex(const std::string& text, size_t columns)
{
    size_t cells = 0;
    size_t lastGood = 0;
    size_t i = 0;
    while (i < text.length()) {
        const size_t start = i;
        const uint32_t cp = utf8Next(text, i);
        const size_t nextCells = cells + utf8Width(cp);
        if (nextCells > columns && lastGood > 0) {
            return lastGood;
        }
        cells = nextCells;
        lastGood = i;
        if (i == start) break;
    }
    return text.length();
}
}

TerminalBuffer::TerminalBuffer(size_t maxLines) : max_lines_(maxLines)
{
}

void TerminalBuffer::setViewport(size_t columns, size_t rows)
{
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    columns_ = std::max<size_t>(1, columns);
    rows_ = std::max<size_t>(1, rows);
    trim();
    ++revision_;
}

void TerminalBuffer::clear()
{
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    lines_.clear();
    current_ = "";
    scroll_offset_ = 0;
    escape_state_ = 0;
    ++revision_;
}

void TerminalBuffer::append(const char* data, size_t len)
{
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (size_t i = 0; i < len; ++i) {
        appendChar(data[i]);
    }
    if (len > 0) ++revision_;
}

void TerminalBuffer::append(const std::string& text)
{
    append(text.data(), text.length());
}

void TerminalBuffer::inputEcho(char c)
{
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    appendChar(c);
    ++revision_;
}

void TerminalBuffer::backspace()
{
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (current_.length()) {
        current_.erase(utf8Prev(current_, current_.size()));
        ++revision_;
    }
}

void TerminalBuffer::scroll(int delta)
{
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    const size_t total = lines_.size() + 1;
    const size_t maxOffset = total > rows_ ? total - rows_ : 0;
    int next = static_cast<int>(scroll_offset_) + delta;
    if (next < 0) {
        next = 0;
    }
    if (next > static_cast<int>(maxOffset)) {
        next = static_cast<int>(maxOffset);
    }
    if (scroll_offset_ == static_cast<size_t>(next)) return;
    scroll_offset_ = static_cast<size_t>(next);
    ++revision_;
}

void TerminalBuffer::scrollToBottom()
{
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (scroll_offset_ == 0) return;
    scroll_offset_ = 0;
    ++revision_;
}

std::string TerminalBuffer::lineAt(size_t viewportRow) const
{
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    const size_t total = lines_.size() + 1;
    if (viewportRow >= rows_) {
        return {};
    }

    const size_t maxOffset = total > rows_ ? total - rows_ : 0;
    const size_t offset = std::min(scroll_offset_, maxOffset);
    const size_t firstIndex = total > rows_ ? total - rows_ - offset : 0;
    const size_t index = firstIndex + viewportRow;
    if (index < lines_.size()) {
        return lines_[index];
    }
    if (index == lines_.size()) {
        return current_;
    }
    return {};
}

size_t TerminalBuffer::inputViewportRow() const
{
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (scroll_offset_ != 0 || rows_ == 0) {
        return rows_;
    }
    const size_t total = lines_.size() + 1;
    return total <= rows_ ? total - 1 : rows_ - 1;
}

bool TerminalBuffer::atBottom() const
{
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return scroll_offset_ == 0;
}

size_t TerminalBuffer::viewportRows() const
{
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return rows_;
}

size_t TerminalBuffer::totalRows() const
{
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return lines_.size() + 1;
}

size_t TerminalBuffer::scrollOffset() const
{
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return scroll_offset_;
}

uint32_t TerminalBuffer::revision() const
{
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return revision_;
}

void TerminalBuffer::pushLine(std::string line)
{
    lines_.push_back(std::move(line));
    if (scroll_offset_ > 0) {
        ++scroll_offset_;
    }
    trim();
}

void TerminalBuffer::appendChar(char c)
{
    uint8_t uc = static_cast<uint8_t>(c);
    if (escape_state_ == 1) {
        if (c == '[') {
            escape_state_ = 2;
        } else if (c == ']') {
            escape_state_ = 3;
        } else {
            escape_state_ = 0;
        }
        return;
    }
    if (escape_state_ == 2) {
        if (uc >= 0x40 && uc <= 0x7E) {
            escape_state_ = 0;
        }
        return;
    }
    if (escape_state_ == 3) {
        if (c == '\a') {
            escape_state_ = 0;
        } else if (c == 0x1B) {
            escape_state_ = 1;
        }
        return;
    }
    if (c == 0x1B) {
        escape_state_ = 1;
        return;
    }
    if (c == '\r') {
        return;
    }
    if (c == '\n') {
        pushLine(current_);
        current_ = "";
        return;
    }
    if (c == '\b' || c == 0x7F) {
        backspace();
        return;
    }
    if (c == '\t') {
        current_ += "    ";
    } else if (std::isprint(static_cast<unsigned char>(c)) || uc >= 0x80) {
        current_ += c;
    }
    while (displayCells(current_) > columns_) {
        size_t index = wrapByteIndex(current_, columns_);
        pushLine(current_.substr(0, index));
        current_ = current_.substr(index);
    }
}

void TerminalBuffer::trim()
{
    while (lines_.size() > max_lines_) {
        lines_.pop_front();
    }
    const size_t total = lines_.size() + 1;
    const size_t maxOffset = total > rows_ ? total - rows_ : 0;
    if (scroll_offset_ > maxOffset) {
        scroll_offset_ = maxOffset;
    }
}
