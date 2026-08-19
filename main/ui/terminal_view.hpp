#pragma once

#include "tabby/terminal_buffer.hpp"
#include "tabby/terminal_emulator.hpp"
#include "tabby/terminus_bitmap.hpp"

#include "lvgl.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tabby {

class TerminalView {
public:
    bool create(lv_obj_t* parent, int x, int y, int width, int height);
    void configureFont(uint8_t line_step);
    void render(const TerminalBuffer& buffer, const std::string& command_line, size_t cursor, bool show_cursor,
                const char* prompt);
    void renderVt(TerminalEmulator& vt, bool show_cursor, bool full = false);
    // Discards the row cache so the next render() repaints everything. Needed
    // whenever something else has drawn into the pixel buffer.
    void markFullRedraw();
    void invalidate();
    int columns() const { return columns_; }
    int rows() const { return rows_; }
    int cellHeight() const { return cell_h_; }
    int pixelWidth() const { return width_; }
    int pixelHeight() const { return height_; }
    uint16_t* pixels() { return pixels_; }
    lv_obj_t* object() const { return canvas_; }

private:
    void fillCell(int col, int row, uint16_t color);
    void clearRow(int row);
    void drawCursor(int col, int row, uint16_t color);
    void drawText(int col, int row, const std::string& text, uint16_t fg, uint16_t bg);
    void drawScrollbar(size_t offset, size_t max_offset, size_t visible, size_t total);
    void invalidateCells(int first_col, int first_row, int last_col, int last_row);
    void invalidateScrollbar();
    uint16_t terminalColor(uint32_t color, bool bold) const;

    lv_obj_t* canvas_{nullptr};
    uint16_t* pixels_{nullptr};
    int width_{0};
    int height_{0};
    int cell_w_{8};
    int cell_h_{16};
    int columns_{80};
    int rows_{24};
    const TerminusBitmap::Font* font_{nullptr};
    // Text last painted per row, so render() can repaint only what moved
    // instead of the whole screen on every keystroke and cursor blink.
    std::vector<std::string> row_shadow_;
    int cursor_col_{-1};
    int cursor_row_{-1};
    size_t scroll_offset_{0};
    size_t scroll_total_{0};
    bool full_redraw_{true};
};

}  // namespace tabby
