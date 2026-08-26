#include "tabby/terminal_emulator.hpp"
#include "tabby/utf8.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr uint32_t DirectColorFlag = 0x01000000UL;
constexpr size_t kMaxCsiBytes = 96;
constexpr size_t kMaxOscBytes = 1024;
constexpr size_t kMaxCsiParams = 16;

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

uint32_t directColor(uint8_t r, uint8_t g, uint8_t b)
{
    return DirectColorFlag | rgb565(r, g, b);
}

TerminalEmulator::Cell BlankCell(uint32_t fg = 7, uint32_t bg = 0)
{
    TerminalEmulator::Cell cell;
    cell.cp = ' ';
    cell.fg = fg;
    cell.bg = bg;
    cell.bold = false;
    cell.inverse = false;
    cell.wide = false;
    cell.continuation = false;
    cell.dirty = true;
    return cell;
}

void copyGrid(const std::vector<TerminalEmulator::Cell>& src, size_t src_cols, size_t src_rows,
              std::vector<TerminalEmulator::Cell>& dst, size_t dst_cols, size_t dst_rows)
{
    if (src.empty() || src_cols == 0 || src_rows == 0 || dst_cols == 0 || dst_rows == 0) return;
    const size_t copy_cols = std::min(src_cols, dst_cols);
    const size_t copy_rows = std::min(src_rows, dst_rows);
    for (size_t row = 0; row < copy_rows; ++row) {
        if ((row + 1) * src_cols > src.size() || (row + 1) * dst_cols > dst.size()) break;
        std::memcpy(&dst[row * dst_cols], &src[row * src_cols], copy_cols * sizeof(TerminalEmulator::Cell));
    }
}
}

void TerminalEmulator::resize(size_t columns, size_t rows)
{
    columns = std::max<size_t>(10, columns);
    rows = std::max<size_t>(5, rows);
    if (columns == cols_ && rows == rows_ && !main_.empty() && !alt_.empty()) {
        return;
    }
    const size_t old_cols = cols_;
    const size_t old_rows = rows_;
    std::vector<Cell> old_main = std::move(main_);
    std::vector<Cell> old_alt = std::move(alt_);
    const bool had = !old_main.empty() && old_cols > 0 && old_rows > 0;

    cols_ = columns;
    rows_ = rows;
    main_.assign(cols_ * rows_, BlankCell());
    alt_.assign(cols_ * rows_, BlankCell());
    if (had) {
        copyGrid(old_main, old_cols, old_rows, main_, cols_, rows_);
        copyGrid(old_alt, old_cols, old_rows, alt_, cols_, rows_);
    }
    if (cols_) cursor_col_ = std::min(cursor_col_, cols_ - 1);
    if (rows_) cursor_row_ = std::min(cursor_row_, rows_ - 1);
    if (cols_) saved_col_ = std::min(saved_col_, cols_ - 1);
    if (rows_) saved_row_ = std::min(saved_row_, rows_ - 1);
    if (scroll_top_ >= rows_ || regionBottom() >= rows_ || scroll_top_ > regionBottom()) {
        scroll_top_ = 0;
        scroll_bottom_ = rows_ ? rows_ - 1 : 0;
    }
    wrap_pending_ = false;
    if (old_cols != cols_) clearScrollback();
    markAllDirty();
}

void TerminalEmulator::reset()
{
    clear();
    clearScrollback();
    alternate_ = false;
    cursor_visible_ = true;
    state_ = State::Ground;
    csi_ = "";
    osc_ = "";
    utf8_ = "";
    utf8_expected_ = 0;
    osc_bytes_ = 0;
    resetAttrs();
}

void TerminalEmulator::clear()
{
    clearCells();
    cursor_col_ = 0;
    cursor_row_ = 0;
    scroll_top_ = 0;
    scroll_bottom_ = rows_ ? rows_ - 1 : 0;
    wrap_pending_ = false;
}

void TerminalEmulator::write(const char* data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        processByte(static_cast<uint8_t>(data[i]));
    }
}

void TerminalEmulator::putStatusLine(const std::string& text)
{
    if (!rows_) return;
    scrollUp(0, rows_ - 1, 1);
    setCursor(rows_ - 1, 0);
    resetAttrs();
    write("[tabby] ", 8);
    write(text);
}

void TerminalEmulator::putRow(size_t row, const std::string& text, uint32_t fg, uint32_t bg, bool inverse)
{
    if (row >= rows_ || cols_ == 0) return;
    auto& buf = alternate_ ? alt_ : main_;
    if ((row + 1) * cols_ > buf.size()) return;
    size_t col = 0;
    size_t i = 0;
    while (i < text.size() && col < cols_) {
        const uint32_t cp = utf8Next(text, i);
        const bool wide = utf8IsWide(cp) && col + 1 < cols_;
        Cell cell = BlankCell(fg, bg);
        cell.cp = cp ? cp : ' ';
        cell.inverse = inverse;
        cell.wide = wide;
        cell.dirty = true;
        buf[row * cols_ + col] = cell;
        if (wide) {
            Cell cont = BlankCell(fg, bg);
            cont.inverse = inverse;
            cont.continuation = true;
            cont.dirty = true;
            buf[row * cols_ + col + 1] = cont;
            col += 2;
        } else {
            ++col;
        }
    }
    while (col < cols_) {
        Cell cell = BlankCell(fg, bg);
        cell.inverse = inverse;
        buf[row * cols_ + col] = cell;
        ++col;
    }
}

size_t TerminalEmulator::regionBottom() const
{
    if (rows_ == 0) return 0;
    return scroll_bottom_ < rows_ ? scroll_bottom_ : rows_ - 1;
}

const TerminalEmulator::Cell& TerminalEmulator::cell(size_t col, size_t row) const
{
    static Cell blank;
    const auto& buf = alternate_ ? alt_ : main_;
    if (col >= cols_ || row >= rows_ || buf.empty()) {
        return blank;
    }
    return buf[row * cols_ + col];
}

const TerminalEmulator::Cell& TerminalEmulator::displayCell(size_t col, size_t row) const
{
    static Cell blank;
    if (col >= cols_ || row >= rows_ || alternate_ || scrollback_offset_ == 0) {
        return cell(col, row);
    }

    const size_t historyRows = scrollbackRows();
    const size_t totalRows = historyRows + rows_;
    if (totalRows <= rows_) {
        return cell(col, row);
    }

    const size_t maxOffset = totalRows - rows_;
    const size_t offset = std::min(scrollback_offset_, maxOffset);
    const size_t firstRow = totalRows - rows_ - offset;
    const size_t displayRow = firstRow + row;
    if (displayRow < historyRows) {
        if (cols_ == 0 || scrollback_.empty()) return blank;
        const size_t max_rows = scrollback_.size() / cols_;
        if (max_rows == 0 || displayRow >= scrollback_count_) return blank;
        const size_t oldest = (scrollback_head_ + max_rows - scrollback_count_) % max_rows;
        const size_t hist_row = (oldest + displayRow) % max_rows;
        const size_t index = hist_row * cols_ + col;
        return index < scrollback_.size() ? scrollback_[index] : blank;
    }
    return cell(col, displayRow - historyRows);
}

void TerminalEmulator::scrollback(int delta)
{
    if (alternate_ || delta == 0) {
        return;
    }
    const size_t maxOffset = scrollbackRows();
    int next = static_cast<int>(scrollback_offset_) + delta;
    if (next < 0) {
        next = 0;
    }
    if (next > static_cast<int>(maxOffset)) {
        next = static_cast<int>(maxOffset);
    }
    if (scrollback_offset_ != static_cast<size_t>(next)) {
        scrollback_offset_ = static_cast<size_t>(next);
        markAllDirty();
    }
}

void TerminalEmulator::scrollbackToBottom()
{
    if (scrollback_offset_) {
        scrollback_offset_ = 0;
        markAllDirty();
    }
}

size_t TerminalEmulator::scrollbackRows() const
{
    return scrollback_count_;
}

void TerminalEmulator::markAllDirty()
{
    full_redraw_pending_ = true;
}

void TerminalEmulator::clearDirty()
{
    // Only the active grid was inspected by the renderer. Keeping the other
    // grid untouched preserves its pending changes and halves the normal
    // dirty-clear traffic.
    auto& buffer = alternate_ ? alt_ : main_;
    for (auto& cell : buffer) cell.dirty = false;
    full_redraw_pending_ = false;
}

void TerminalEmulator::markCursorDirty()
{
    if (cursor_col_ >= cols_ || cursor_row_ >= rows_) return;
    auto& buffer = alternate_ ? alt_ : main_;
    const size_t index = cursor_row_ * cols_ + cursor_col_;
    if (index < buffer.size()) buffer[index].dirty = true;
}

void TerminalEmulator::resetAttrs()
{
    fg_ = 7;
    bg_ = 0;
    bold_ = false;
    inverse_ = false;
}

void TerminalEmulator::useAlternate(bool enabled)
{
    if (alternate_ == enabled) {
        return;
    }
    if (enabled) {
        saved_col_ = cursor_col_;
        saved_row_ = cursor_row_;
    }
    alternate_ = enabled;
    scrollbackToBottom();
    if (enabled) {
        cursor_col_ = 0;
        cursor_row_ = 0;
    } else {
        cursor_col_ = std::min(saved_col_, cols_ ? cols_ - 1 : 0);
        cursor_row_ = std::min(saved_row_, rows_ ? rows_ - 1 : 0);
    }
    wrap_pending_ = false;
    if (enabled) {
        for (auto& cell : alt_) cell = BlankCell();
    }
    markAllDirty();
}

void TerminalEmulator::newline()
{
    markCursorDirty();
    wrap_pending_ = false;
    size_t bottom = regionBottom();
    if (cursor_row_ == bottom) {
        scrollUp(scroll_top_, bottom, 1);
    } else if (cursor_row_ + 1 < rows_) {
        ++cursor_row_;
    } else if (rows_) {
        scrollUp(0, rows_ - 1, 1);
    }
    markCursorDirty();
}

void TerminalEmulator::carriageReturn()
{
    markCursorDirty();
    cursor_col_ = 0;
    wrap_pending_ = false;
    markCursorDirty();
}

void TerminalEmulator::backspace()
{
    markCursorDirty();
    if (cursor_col_ > 0) {
        --cursor_col_;
        wrap_pending_ = false;
    }
    markCursorDirty();
}

void TerminalEmulator::tab()
{
    markCursorDirty();
    size_t next = ((cursor_col_ / 8) + 1) * 8;
    if (next >= cols_) {
        next = cols_ - 1;
    }
    cursor_col_ = next;
    wrap_pending_ = false;
    markCursorDirty();
}

void TerminalEmulator::putGlyph(const std::string& glyph, bool wide)
{
    if (wrap_pending_) {
        carriageReturn();
        newline();
    }
    if (wide && cursor_col_ + 1 >= cols_) {
        carriageReturn();
        newline();
    }
    Cell& cell = mutableCell(cursor_col_, cursor_row_);

    if (cell.continuation && cursor_col_ > 0) {
        Cell& prev = mutableCell(cursor_col_ - 1, cursor_row_);
        prev = BlankCell(fg_, bg_);
        markDirtyWithNeighbors(cursor_col_ - 1, cursor_row_);
    } else if (cell.wide && cursor_col_ + 1 < cols_) {
        Cell& next = mutableCell(cursor_col_ + 1, cursor_row_);
        next = BlankCell(fg_, bg_);
        markDirtyWithNeighbors(cursor_col_ + 1, cursor_row_);
    }

    size_t i = 0;
    cell.cp = glyph.length() ? utf8Next(glyph, i) : ' ';
    cell.fg = fg_;
    cell.bg = bg_;
    cell.bold = bold_;
    cell.inverse = inverse_;
    cell.wide = wide && cursor_col_ + 1 < cols_;
    cell.continuation = false;
    markDirtyWithNeighbors(cursor_col_, cursor_row_);

    if (cell.wide) {
        Cell& next = mutableCell(cursor_col_ + 1, cursor_row_);
        next = BlankCell(fg_, bg_);
        next.inverse = inverse_;
        next.continuation = true;
        markDirtyWithNeighbors(cursor_col_ + 1, cursor_row_);
    }

    size_t advance = cell.wide ? 2 : 1;
    if (cursor_col_ + advance >= cols_) {
        cursor_col_ = cols_ - 1;
        wrap_pending_ = true;
    } else {
        cursor_col_ += advance;
    }
}

void TerminalEmulator::scrollUp(size_t top, size_t bottom, size_t count)
{
    if (top >= rows_ || bottom >= rows_ || top > bottom || count == 0) return;
    auto& buf = alternate_ ? alt_ : main_;
    count = std::min(count, bottom - top + 1);
    const bool fullNormalScroll = !alternate_ && top == 0 && bottom == rows_ - 1;
    const bool viewingHistory = scrollback_offset_ > 0;
    if (fullNormalScroll) {
        for (size_t row = 0; row < count; ++row) {
            pushScrollbackRow(row);
        }
        if (viewingHistory) {
            scrollback_offset_ = std::min(scrollback_offset_ + count, scrollbackRows());
        }
    }
    const size_t live_rows = bottom - top + 1;
    if (count < live_rows) {
        Cell* dest = &buf[top * cols_];
        const Cell* src = &buf[(top + count) * cols_];
        std::memmove(dest, src, (live_rows - count) * cols_ * sizeof(Cell));
    }
    for (size_t row = top; row + count <= bottom; ++row) {
        for (size_t col = 0; col < cols_; ++col) buf[row * cols_ + col].dirty = true;
    }
    for (size_t row = bottom - count + 1; row <= bottom; ++row) {
        clearRow(row, 0, cols_ - 1);
    }
}

void TerminalEmulator::clearRow(size_t row, size_t fromCol, size_t toCol)
{
    if (row >= rows_ || fromCol >= cols_) return;
    toCol = std::min(toCol, cols_ - 1);
    for (size_t col = fromCol; col <= toCol; ++col) {
        mutableCell(col, row) = BlankCell(fg_, bg_);
        markDirtyWithNeighbors(col, row);
    }
}

void TerminalEmulator::clearCells()
{
    auto& buf = alternate_ ? alt_ : main_;
    for (auto& cell : buf) {
        cell = BlankCell();
    }
}

void TerminalEmulator::clearScrollback()
{
    scrollback_.clear();
    scrollback_head_ = 0;
    scrollback_count_ = 0;
    scrollback_offset_ = 0;
}

void TerminalEmulator::pushScrollbackRow(size_t row)
{
    if (row >= rows_ || cols_ == 0 || max_scrollback_rows_ == 0) {
        return;
    }
    const auto& buf = main_;
    const size_t first = row * cols_;
    if (first + cols_ > buf.size()) {
        return;
    }
    const size_t need = max_scrollback_rows_ * cols_;
    if (scrollback_.size() != need) {
        scrollback_.assign(need, BlankCell());
        scrollback_head_ = 0;
        scrollback_count_ = 0;
    }
    Cell* dest = &scrollback_[scrollback_head_ * cols_];
    std::memcpy(dest, &buf[first], cols_ * sizeof(Cell));
    scrollback_head_ = (scrollback_head_ + 1) % max_scrollback_rows_;
    if (scrollback_count_ < max_scrollback_rows_) {
        ++scrollback_count_;
    } else if (scrollback_offset_ > 0) {
        --scrollback_offset_;
    }
}

void TerminalEmulator::scrollDown(size_t top, size_t bottom, size_t count)
{
    if (top >= rows_ || bottom >= rows_ || top > bottom || count == 0) return;
    auto& buf = alternate_ ? alt_ : main_;
    count = std::min(count, bottom - top + 1);
    const size_t live_rows = bottom - top + 1;
    if (count < live_rows) {
        Cell* dest = &buf[(top + count) * cols_];
        const Cell* src = &buf[top * cols_];
        std::memmove(dest, src, (live_rows - count) * cols_ * sizeof(Cell));
    }
    for (size_t row = top + count; row <= bottom; ++row) {
        for (size_t col = 0; col < cols_; ++col) buf[row * cols_ + col].dirty = true;
    }
    for (size_t row = top; row < top + count; ++row) {
        clearRow(row, 0, cols_ - 1);
    }
}

void TerminalEmulator::insertCells(size_t count)
{
    if (cursor_row_ >= rows_ || cursor_col_ >= cols_ || count == 0) return;
    count = std::min(count, cols_ - cursor_col_);
    for (size_t col = cols_; col-- > cursor_col_ + count;) {
        mutableCell(col, cursor_row_) = mutableCell(col - count, cursor_row_);
        markDirtyWithNeighbors(col, cursor_row_);
    }
    for (size_t col = cursor_col_; col < cursor_col_ + count; ++col) {
        mutableCell(col, cursor_row_) = BlankCell(fg_, bg_);
        markDirtyWithNeighbors(col, cursor_row_);
    }
}

void TerminalEmulator::deleteCells(size_t count)
{
    if (cursor_row_ >= rows_ || cursor_col_ >= cols_ || count == 0) return;
    count = std::min(count, cols_ - cursor_col_);
    for (size_t col = cursor_col_; col + count < cols_; ++col) {
        mutableCell(col, cursor_row_) = mutableCell(col + count, cursor_row_);
        markDirtyWithNeighbors(col, cursor_row_);
    }
    for (size_t col = cols_ - count; col < cols_; ++col) {
        mutableCell(col, cursor_row_) = BlankCell(fg_, bg_);
        markDirtyWithNeighbors(col, cursor_row_);
    }
}

void TerminalEmulator::insertLines(size_t count)
{
    size_t bottom = regionBottom();
    scrollDown(cursor_row_, bottom, count);
}

void TerminalEmulator::deleteLines(size_t count)
{
    size_t bottom = regionBottom();
    scrollUp(cursor_row_, bottom, count);
}

void TerminalEmulator::processByte(uint8_t c)
{
    if (state_ == State::Utf8) {
        if (utf8IsContinuation(c)) {
            utf8_ += static_cast<char>(c);
            if (utf8_.length() >= utf8_expected_) {
                size_t i = 0;
                const uint32_t cp = utf8Next(utf8_, i);
                putGlyph(utf8_, utf8IsWide(cp));
                utf8_ = "";
                utf8_expected_ = 0;
                state_ = State::Ground;
            }
        } else {
            utf8_ = "";
            utf8_expected_ = 0;
            state_ = State::Ground;
            processByte(c);
        }
        return;
    }
    if (state_ == State::Escape) {
        processEscape(c);
        return;
    }
    if (state_ == State::Charset) {
        state_ = State::Ground;
        return;
    }
    if (state_ == State::Csi) {
        processCsi(c);
        return;
    }
    if (state_ == State::Osc) {
        if (c == '\a') {
            osc_bytes_ = 0;
            state_ = State::Ground;
        } else if (c == 0x1B) {
            osc_bytes_ = 0;
            state_ = State::Escape;
        } else if (++osc_bytes_ > kMaxOscBytes) {
            osc_bytes_ = 0;
            state_ = State::Ground;
        }
        return;
    }

    if (c == 0x1B) {
        state_ = State::Escape;
    } else if (c == '\r') {
        carriageReturn();
    } else if (c == '\n') {
        // PTYs normally translate LF to CRLF (ONLCR), but small shells and
        // network appliances often emit a bare LF. Treat it as a visual line
        // break so subsequent output starts in column zero instead of forming
        // a staircase across the display.
        carriageReturn();
        newline();
    } else if (c == '\b' || c == 0x7F) {
        backspace();
    } else if (c == '\t') {
        tab();
    } else if (c >= 0x20 && c < 0x80) {
        putGlyph(std::string(1, static_cast<char>(c)), false);
    } else if (c >= 0x80) {
        utf8_.assign(1, static_cast<char>(c));
        utf8_expected_ = utf8ByteLen(c);
        if (utf8_expected_ <= 1) {
            size_t i = 0;
            const uint32_t cp = utf8Next(utf8_, i);
            putGlyph(utf8_, utf8IsWide(cp));
            utf8_ = "";
        } else {
            state_ = State::Utf8;
        }
    }
}

void TerminalEmulator::processEscape(uint8_t c)
{
    if (c == '[') {
        csi_ = "";
        params_.clear();
        state_ = State::Csi;
    } else if (c == ']') {
        osc_ = "";
        osc_bytes_ = 0;
        state_ = State::Osc;
    } else if (c == 'c') {
        reset();
    } else if (c == '7') {
        saved_col_ = cursor_col_;
        saved_row_ = cursor_row_;
        state_ = State::Ground;
    } else if (c == '8') {
        setCursor(saved_row_, saved_col_);
        state_ = State::Ground;
    } else if (c == '(' || c == ')' || c == '*' || c == '+') {
        state_ = State::Charset;
    } else {
        state_ = State::Ground;
    }
}

void TerminalEmulator::processCsi(uint8_t c)
{
    if (c >= 0x40 && c <= 0x7E) {
        parseParams();
        executeCsi(static_cast<char>(c));
        csi_.clear();
        state_ = State::Ground;
    } else if (csi_.size() >= kMaxCsiBytes) {
        csi_.clear();
        state_ = State::Ground;
    } else {
        csi_ += static_cast<char>(c);
    }
}

void TerminalEmulator::executeCsi(char command)
{
    switch (command) {
        case 'A':
            setCursor(cursor_row_ > static_cast<size_t>(param(0, 1)) ? cursor_row_ - param(0, 1) : 0, cursor_col_);
            break;
        case 'B':
            setCursor(std::min(rows_ - 1, cursor_row_ + static_cast<size_t>(param(0, 1))), cursor_col_);
            break;
        case 'C':
            setCursor(cursor_row_, std::min(cols_ - 1, cursor_col_ + static_cast<size_t>(param(0, 1))));
            break;
        case 'D':
            setCursor(cursor_row_, cursor_col_ > static_cast<size_t>(param(0, 1)) ? cursor_col_ - param(0, 1) : 0);
            break;
        case 'G':
            setCursor(cursor_row_, static_cast<size_t>(std::max(1, param(0, 1)) - 1));
            break;
        case '`':
            setCursor(cursor_row_, static_cast<size_t>(std::max(1, param(0, 1)) - 1));
            break;
        case 'H':
        case 'f':
            setCursor(static_cast<size_t>(std::max(1, param(0, 1)) - 1),
                      static_cast<size_t>(std::max(1, param(1, 1)) - 1));
            break;
        case 'd':
            setCursor(static_cast<size_t>(std::max(1, param(0, 1)) - 1), cursor_col_);
            break;
        case '@':
            insertCells(static_cast<size_t>(param(0, 1)));
            break;
        case 'P':
            deleteCells(static_cast<size_t>(param(0, 1)));
            break;
        case 'X':
            clearRow(cursor_row_, cursor_col_,
                     std::min(cols_ - 1, cursor_col_ + static_cast<size_t>(param(0, 1)) - 1));
            break;
        case 'L':
            insertLines(static_cast<size_t>(param(0, 1)));
            break;
        case 'M':
            deleteLines(static_cast<size_t>(param(0, 1)));
            break;
        case 'S':
            scrollUp(scroll_top_, regionBottom(),
                     static_cast<size_t>(param(0, 1)));
            break;
        case 'T':
            scrollDown(scroll_top_, regionBottom(),
                       static_cast<size_t>(param(0, 1)));
            break;
        case 'J': {
            int mode = param(0, 0);
            if (mode == 2) {
                clearCells();
            } else if (mode == 3) {
                clearScrollback();
                clearCells();
            } else if (mode == 0) {
                clearRow(cursor_row_, cursor_col_, cols_ - 1);
                for (size_t row = cursor_row_ + 1; row < rows_; ++row) clearRow(row, 0, cols_ - 1);
            } else if (mode == 1) {
                for (size_t row = 0; row < cursor_row_; ++row) clearRow(row, 0, cols_ - 1);
                clearRow(cursor_row_, 0, cursor_col_);
            }
            break;
        }
        case 'K': {
            int mode = param(0, 0);
            if (mode == 0) clearRow(cursor_row_, cursor_col_, cols_ - 1);
            else if (mode == 1) clearRow(cursor_row_, 0, cursor_col_);
            else if (mode == 2) clearRow(cursor_row_, 0, cols_ - 1);
            break;
        }
        case 'm':
            if (params_.empty()) params_.push_back(0);
            for (size_t i = 0; i < params_.size(); ++i) {
                int p = params_[i];
                if (p == 0) resetAttrs();
                else if (p == 1) bold_ = true;
                else if (p == 7) inverse_ = true;
                else if (p == 22) bold_ = false;
                else if (p == 27) inverse_ = false;
                else if (p >= 30 && p <= 37) fg_ = static_cast<uint32_t>(p - 30);
                else if (p == 39) fg_ = 7;
                else if (p >= 40 && p <= 47) bg_ = static_cast<uint32_t>(p - 40);
                else if (p == 49) bg_ = 0;
                else if (p >= 90 && p <= 97) fg_ = static_cast<uint32_t>(p - 90 + 8);
                else if (p >= 100 && p <= 107) bg_ = static_cast<uint32_t>(p - 100 + 8);
                else if ((p == 38 || p == 48) && i + 2 < params_.size() && params_[i + 1] == 5) {
                    uint32_t color = static_cast<uint32_t>(std::max(0, std::min(255, params_[i + 2])));
                    if (p == 38) fg_ = color;
                    else bg_ = color;
                    i += 2;
                } else if ((p == 38 || p == 48) && i + 4 < params_.size() && params_[i + 1] == 2) {
                    uint8_t r = static_cast<uint8_t>(std::max(0, std::min(255, params_[i + 2])));
                    uint8_t g = static_cast<uint8_t>(std::max(0, std::min(255, params_[i + 3])));
                    uint8_t b = static_cast<uint8_t>(std::max(0, std::min(255, params_[i + 4])));
                    uint32_t color = directColor(r, g, b);
                    if (p == 38) fg_ = color;
                    else bg_ = color;
                    i += 4;
                }
            }
            break;
        case 'r':
            scroll_top_ = static_cast<size_t>(std::max(1, param(0, 1)) - 1);
            scroll_bottom_ = static_cast<size_t>(std::max(1, param(1, static_cast<int>(rows_))) - 1);
            if (scroll_top_ >= rows_ || scroll_bottom_ >= rows_ || scroll_top_ > scroll_bottom_) {
                scroll_top_ = 0;
                scroll_bottom_ = rows_ ? rows_ - 1 : 0;
            }
            setCursor(0, 0);
            break;
        case 'h':
            if (!csi_.empty() && csi_[0] == '?') {
                for (int p : params_) {
                    if (p == 25 && !cursor_visible_) {
                        markCursorDirty();
                        cursor_visible_ = true;
                    }
                    if (p == 1049 || p == 47 || p == 1047) useAlternate(true);
                }
            }
            break;
        case 'l':
            if (!csi_.empty() && csi_[0] == '?') {
                for (int p : params_) {
                    if (p == 25 && cursor_visible_) {
                        markCursorDirty();
                        cursor_visible_ = false;
                    }
                    if (p == 1049 || p == 47 || p == 1047) useAlternate(false);
                }
            }
            break;
        case 's':
            saved_col_ = cursor_col_;
            saved_row_ = cursor_row_;
            break;
        case 'u':
            setCursor(saved_row_, saved_col_);
            break;
        default:
            break;
    }
    wrap_pending_ = false;
}

int TerminalEmulator::param(size_t index, int fallback) const
{
    if (index >= params_.size() || params_[index] <= 0) {
        return fallback;
    }
    return params_[index];
}

void TerminalEmulator::parseParams()
{
    params_.clear();
    std::string s = csi_;
    auto erase_all = [](std::string& text, char ch) {
        text.erase(std::remove(text.begin(), text.end(), ch), text.end());
    };
    erase_all(s, '?');
    erase_all(s, '>');
    size_t start = 0;
    while (start <= s.length()) {
        auto semi = s.find(';', start);
        size_t end = semi == std::string::npos ? s.size() : semi;
        std::string part = s.substr(start, end - start);
        params_.push_back(part.length() ? atoi(part.c_str()) : 0);
        if (semi == std::string::npos) break;
        if (params_.size() >= kMaxCsiParams) break;
        start = static_cast<size_t>(semi) + 1;
    }
}

void TerminalEmulator::setCursor(size_t row, size_t col)
{
    markCursorDirty();
    cursor_row_ = std::min(row, rows_ ? rows_ - 1 : 0);
    cursor_col_ = std::min(col, cols_ ? cols_ - 1 : 0);
    wrap_pending_ = false;
    markCursorDirty();
}

void TerminalEmulator::markDirty(size_t col, size_t row)
{
    mutableCell(col, row).dirty = true;
}

void TerminalEmulator::markDirtyWithNeighbors(size_t col, size_t row)
{
    if (row >= rows_ || col >= cols_) {
        return;
    }
    mutableCell(col, row).dirty = true;
    if (col > 0) {
        mutableCell(col - 1, row).dirty = true;
    }
    if (col + 1 < cols_) {
        mutableCell(col + 1, row).dirty = true;
    }
}

TerminalEmulator::Cell& TerminalEmulator::mutableCell(size_t col, size_t row)
{
    auto& buf = alternate_ ? alt_ : main_;
    return buf[row * cols_ + col];
}
