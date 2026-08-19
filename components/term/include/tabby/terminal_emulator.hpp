#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class TerminalEmulator {
public:
    struct Cell {
        uint32_t cp{' '};
        uint32_t fg{7};
        uint32_t bg{0};
        bool bold{false};
        bool inverse{false};
        bool wide{false};
        bool continuation{false};
        bool dirty{true};
    };

    void resize(size_t columns, size_t rows);
    void reset();
    void clear();
    void write(const char* data, size_t len);
    void write(const std::string& text) { write(text.c_str(), text.size()); }
    void putStatusLine(const std::string& text);
    void setAlternateScreen(bool enabled) { useAlternate(enabled); }
    void moveCursor(size_t row, size_t col) { setCursor(row, col); }
    void putRow(size_t row, const std::string& text, uint32_t fg = 7, uint32_t bg = 0, bool inverse = false);

    size_t columns() const { return cols_; }
    size_t rows() const { return rows_; }
    size_t cursorColumn() const { return cursor_col_; }
    size_t cursorRow() const { return cursor_row_; }
    bool cursorVisible() const { return cursor_visible_; }
    bool alternateScreen() const { return alternate_; }
    const Cell& cell(size_t col, size_t row) const;
    const Cell& displayCell(size_t col, size_t row) const;
    void scrollback(int delta);
    void scrollbackToBottom();
    size_t scrollbackOffset() const { return scrollback_offset_; }
    size_t scrollbackRows() const;
    void markAllDirty();
    void markCursorDirty();
    void clearDirty();

private:
    enum class State : uint8_t { Ground, Escape, Charset, Csi, Osc, Utf8 };

    void resetAttrs();
    void useAlternate(bool enabled);
    void newline();
    void carriageReturn();
    void backspace();
    void tab();
    void putGlyph(const std::string& glyph, bool wide);
    void scrollUp(size_t top, size_t bottom, size_t count);
    void clearRow(size_t row, size_t fromCol, size_t toCol);
    void clearCells();
    void clearScrollback();
    void pushScrollbackRow(size_t row);
    void insertCells(size_t count);
    void deleteCells(size_t count);
    void insertLines(size_t count);
    void deleteLines(size_t count);
    void scrollDown(size_t top, size_t bottom, size_t count);
    void processByte(uint8_t c);
    void processEscape(uint8_t c);
    void processCsi(uint8_t c);
    void executeCsi(char command);
    int param(size_t index, int fallback) const;
    void parseParams();
    void setCursor(size_t row, size_t col);
    void markDirty(size_t col, size_t row);
    void markDirtyWithNeighbors(size_t col, size_t row);
    Cell& mutableCell(size_t col, size_t row);
    size_t regionBottom() const;

    size_t cols_{80};
    size_t rows_{24};
    size_t cursor_col_{0};
    size_t cursor_row_{0};
    size_t saved_col_{0};
    size_t saved_row_{0};
    uint32_t fg_{7};
    uint32_t bg_{0};
    bool bold_{false};
    bool inverse_{false};
    bool wrap_pending_{false};
    bool alternate_{false};
    bool cursor_visible_{true};
    size_t scroll_top_{0};
    size_t scroll_bottom_{static_cast<size_t>(-1)};
    size_t osc_bytes_{0};
    State state_{State::Ground};
    std::string csi_;
    std::string osc_;
    std::string utf8_;
    uint8_t utf8_expected_{0};
    std::vector<int> params_;
    std::vector<Cell> main_;
    std::vector<Cell> alt_;
    std::vector<Cell> scrollback_;
    size_t scrollback_offset_{0};
    size_t scrollback_head_{0};
    size_t scrollback_count_{0};
    size_t max_scrollback_rows_{800};
};
