#include "terminal_view.hpp"

#include "cjk_term_font.hpp"
#include "tabby/utf8.hpp"

#include "esp_heap_caps.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace tabby {
namespace {

constexpr uint16_t kBg = 0x10A2;  // dark blue-gray
constexpr uint16_t kFg = 0xE73C;  // light gray
constexpr uint16_t kCursor = 0x07E0;
constexpr int kScrollbarW = 4;

}  // namespace

bool TerminalView::create(lv_obj_t* parent, int x, int y, int width, int height) {
    width_ = width;
    height_ = height;
    pixels_ = static_cast<uint16_t*>(
        heap_caps_malloc(static_cast<size_t>(width) * height * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pixels_ == nullptr) {
        pixels_ = static_cast<uint16_t*>(
            heap_caps_malloc(static_cast<size_t>(width) * height * sizeof(uint16_t), MALLOC_CAP_8BIT));
    }
    if (pixels_ == nullptr) return false;
    std::fill(pixels_, pixels_ + (static_cast<size_t>(width) * height), kBg);

    canvas_ = lv_canvas_create(parent);
    lv_obj_set_pos(canvas_, x, y);
    lv_canvas_set_buffer(canvas_, pixels_, width, height, LV_IMG_CF_TRUE_COLOR);
    configureFont(28);
    return true;
}

void TerminalView::configureFont(uint8_t line_step) {
    font_ = &TerminusBitmap::fontForHeight(line_step);
    cell_w_ = font_->targetW;
    cell_h_ = font_->targetH;
    columns_ = std::max(10, width_ / cell_w_);
    rows_ = std::max(5, height_ / cell_h_);
    full_redraw_ = true;
    // A smaller glyph grid can leave pixels from the previous font in the
    // unused right/bottom margins. Clear them once; normal VT renders remain
    // cell-local after this configuration change.
    if (pixels_ != nullptr) {
        std::fill(pixels_, pixels_ + (static_cast<size_t>(width_) * height_), kBg);
        invalidate();
    }
}

void TerminalView::fillCell(int col, int row, uint16_t color) {
    const int x0 = col * cell_w_;
    const int y0 = row * cell_h_;
    for (int y = 0; y < cell_h_ && y0 + y < height_; ++y) {
        uint16_t* line = pixels_ + (y0 + y) * width_ + x0;
        const int count = std::min(cell_w_, width_ - x0);
        for (int x = 0; x < count; ++x) line[x] = color;
    }
}

void TerminalView::clearRow(int row) {
    const int y0 = row * cell_h_;
    const int count = std::min(cell_h_, height_ - y0);
    // Preserve the scrollbar. Fonts whose final cell overlaps it repaint it
    // explicitly below; the common fonts leave a margin and avoid a tall,
    // narrow LVGL invalidation on every changed row.
    const int clear_width = std::max(0, width_ - kScrollbarW);
    for (int y = 0; y < count; ++y) {
        uint16_t* line = pixels_ + static_cast<size_t>(y0 + y) * width_;
        std::fill(line, line + clear_width, kBg);
    }
}

void TerminalView::drawCursor(int col, int row, uint16_t color) {
    const int x0 = col * cell_w_;
    const int y0 = row * cell_h_ + std::max(0, cell_h_ - 2);
    if (x0 < 0 || y0 < 0 || x0 >= width_ || y0 >= height_) return;
    const int cursor_h = std::min(2, height_ - y0);
    const int cursor_w = std::min(cell_w_, width_ - x0);
    for (int y = 0; y < cursor_h; ++y) {
        uint16_t* line = pixels_ + (y0 + y) * width_ + x0;
        std::fill(line, line + cursor_w, color);
    }
}

void TerminalView::drawTofu(int x, int y, int w, int h, uint16_t color) {
    if (pixels_ == nullptr || w < 3 || h < 3) return;
    const int x0 = std::max(0, x + 1);
    const int y0 = std::max(0, y + 1);
    const int x1 = std::min(width_ - 1, x + w - 2);
    const int y1 = std::min(height_ - 1, y + h - 2);
    if (x1 < x0 || y1 < y0) return;
    for (int px = x0; px <= x1; ++px) {
        pixels_[y0 * width_ + px] = color;
        pixels_[y1 * width_ + px] = color;
    }
    for (int py = y0; py <= y1; ++py) {
        pixels_[py * width_ + x0] = color;
        pixels_[py * width_ + x1] = color;
    }
}

void TerminalView::drawCodepoint(int col, int row, uint32_t cp, int cells, uint16_t fg) {
    const int x = col * cell_w_;
    const int y = row * cell_h_;
    if (font_ != nullptr && TerminusBitmap::drawGlyph(pixels_, width_, width_, height_, *font_, cp, x, y, fg)) {
        return;
    }
    const int dst_w = std::max(1, cells) * cell_w_;
    if (CjkTermFont::drawGlyph(pixels_, width_, width_, height_, cp, x, y, dst_w, cell_h_, fg)) return;
    if (cells > 1) drawTofu(x, y, dst_w, cell_h_, fg);
}

void TerminalView::drawText(int col, int row, const std::string& text, uint16_t fg, uint16_t bg) {
    size_t i = 0;
    int xcol = col;
    while (i < text.size() && xcol < columns_) {
        const uint32_t cp = utf8Next(text, i);
        const int width = static_cast<int>(utf8Width(cp));
        fillCell(xcol, row, bg);
        if (width > 1 && xcol + 1 < columns_) fillCell(xcol + 1, row, bg);
        const int cells = std::min(width, columns_ - xcol);
        drawCodepoint(xcol, row, cp, std::max(1, cells), fg);
        xcol += width;
    }
}

uint16_t TerminalView::terminalColor(uint32_t color, bool bold) const {
    static const uint16_t kAnsi[16] = {
        0x0000, 0xA800, 0x0540, 0xAAA0, 0x0015, 0xA815, 0x0555, 0xAD55,
        0x52AA, 0xFAAA, 0x57EA, 0xFFEA, 0x52BF, 0xFABF, 0x57FF, 0xFFFF,
    };
    if (color & 0x01000000UL) return static_cast<uint16_t>(color & 0xFFFF);
    if (color < 16) {
        uint8_t index = static_cast<uint8_t>(color & 0x0F);
        if (bold && index < 8) index += 8;
        return kAnsi[index];
    }
    if (color >= 16 && color <= 231) {
        const uint32_t c = color - 16;
        const uint8_t r = static_cast<uint8_t>(c / 36);
        const uint8_t g = static_cast<uint8_t>((c / 6) % 6);
        const uint8_t b = static_cast<uint8_t>(c % 6);
        auto level = [](uint8_t v) -> uint8_t { return v == 0 ? 0 : static_cast<uint8_t>(55 + v * 40); };
        return static_cast<uint16_t>(((level(r) & 0xF8) << 8) | ((level(g) & 0xFC) << 3) | (level(b) >> 3));
    }
    if (color >= 232 && color <= 255) {
        const uint8_t level = static_cast<uint8_t>(8 + (color - 232) * 10);
        return static_cast<uint16_t>(((level & 0xF8) << 8) | ((level & 0xFC) << 3) | (level >> 3));
    }
    return kFg;
}

bool TerminalView::drawScrollbar(size_t offset, size_t max_offset, size_t visible, size_t total, bool force) {
    constexpr uint16_t kTrack = 0x2124;
    constexpr uint16_t kThumb = 0x7BEF;
    if (pixels_ == nullptr || width_ < kScrollbarW) return false;

    const bool track_visible = total > visible && max_offset != 0;
    int thumb_h = 0;
    int thumb_y = 0;
    if (track_visible) {
        thumb_h = std::min(
            height_, std::max(cell_h_, static_cast<int>((static_cast<size_t>(height_) * visible) / total)));
        const int travel = std::max(0, height_ - thumb_h);
        thumb_y = static_cast<int>((static_cast<size_t>(travel) * (max_offset - offset)) / max_offset);
    }
    if (!force && track_visible == scrollbar_track_visible_ && thumb_h == scrollbar_thumb_h_ &&
        thumb_y == scrollbar_thumb_y_) {
        return false;
    }

    scrollbar_track_visible_ = track_visible;
    scrollbar_thumb_h_ = thumb_h;
    scrollbar_thumb_y_ = thumb_y;
    const int x0 = width_ - kScrollbarW;
    for (int y = 0; y < height_; ++y) {
        uint16_t* line = pixels_ + y * width_ + x0;
        std::fill(line, line + kScrollbarW, track_visible ? kTrack : kBg);
    }
    for (int y = 0; y < thumb_h && thumb_y + y < height_; ++y) {
        uint16_t* line = pixels_ + (thumb_y + y) * width_ + x0;
        std::fill(line, line + kScrollbarW, kThumb);
    }
    return true;
}

void TerminalView::invalidate() {
    if (canvas_) lv_obj_invalidate(canvas_);
}

void TerminalView::invalidateCells(int first_col, int first_row, int last_col, int last_row) {
    if (canvas_ == nullptr || first_col > last_col || first_row > last_row) return;

    lv_area_t object_area{};
    lv_obj_get_coords(canvas_, &object_area);
    lv_area_t dirty_area{};
    dirty_area.x1 = object_area.x1 + first_col * cell_w_;
    dirty_area.y1 = object_area.y1 + first_row * cell_h_;
    dirty_area.x2 = std::min<lv_coord_t>(object_area.x2, object_area.x1 + (last_col + 1) * cell_w_ - 1);
    dirty_area.y2 = std::min<lv_coord_t>(object_area.y2, object_area.y1 + (last_row + 1) * cell_h_ - 1);
    lv_obj_invalidate_area(canvas_, &dirty_area);
}

void TerminalView::invalidateScrollbar() {
    if (canvas_ == nullptr) return;
    lv_area_t object_area{};
    lv_obj_get_coords(canvas_, &object_area);
    lv_area_t bar{};
    bar.x1 = std::max<lv_coord_t>(object_area.x1, object_area.x2 - 3);
    bar.y1 = object_area.y1;
    bar.x2 = object_area.x2;
    bar.y2 = object_area.y2;
    lv_obj_invalidate_area(canvas_, &bar);
}

void TerminalView::render(const TerminalBuffer& buffer, const std::string& command_line, size_t cursor,
                          bool show_cursor, const char* prompt, bool show_input) {
    if (pixels_ == nullptr) return;
    const char* prompt_text = prompt ? prompt : "[tabby] ";
    const size_t input_row = buffer.inputViewportRow();

    if (row_shadow_.size() != static_cast<size_t>(rows_)) {
        row_shadow_.assign(static_cast<size_t>(rows_), std::string());
        full_redraw_ = true;
    }
    const bool full = full_redraw_;
    if (full) std::fill(pixels_, pixels_ + (static_cast<size_t>(width_) * height_), kBg);

    const size_t prompt_cols = utf8DisplayCols(prompt_text);
    const size_t command_cols = utf8DisplayCols(command_line, cursor);
    const int cursor_col =
        show_cursor && show_input
            ? static_cast<int>(std::min(prompt_cols + command_cols, static_cast<size_t>(std::max(0, columns_ - 1))))
            : -1;
    const int cursor_row = show_cursor && show_input ? static_cast<int>(input_row) : -1;
    const bool cursor_moved = cursor_col != cursor_col_ || cursor_row != cursor_row_;

    int first_row = rows_;
    int last_row = -1;
    for (int row = 0; row < rows_; ++row) {
        std::string line = buffer.lineAt(static_cast<size_t>(row));
        if (show_input && static_cast<size_t>(row) == input_row) {
            if (line.find(prompt_text) == std::string::npos) line += prompt_text;
            line += command_line;
        }
        // The cursor is painted inside its row, so a blink dirties both the row
        // it left and the row it moved to.
        const bool cursor_dirty = cursor_moved && (row == cursor_row || row == cursor_row_);
        if (!full && !cursor_dirty && line == row_shadow_[static_cast<size_t>(row)]) continue;

        if (!full) clearRow(row);
        drawText(0, row, line, kFg, kBg);
        row_shadow_[static_cast<size_t>(row)] = std::move(line);
        first_row = std::min(first_row, row);
        last_row = std::max(last_row, row);
    }
    if (cursor_row >= 0) drawCursor(cursor_col, cursor_row, kCursor);
    cursor_col_ = cursor_col;
    cursor_row_ = cursor_row;

    const size_t visible = static_cast<size_t>(std::max(1, rows_));
    const size_t total = buffer.totalRows();
    const size_t max_offset = total > visible ? total - visible : 0;
    const size_t offset = buffer.scrollOffset();
    // The common font widths stop before the bar, so unchanged scrollbar
    // geometry does not need a separate tall invalidation.
    const bool cells_overlap_bar = columns_ * cell_w_ > width_ - kScrollbarW;
    const bool bar_dirty = drawScrollbar(offset, max_offset, visible, total,
                                         full || (last_row >= 0 && cells_overlap_bar));

    if (full) {
        full_redraw_ = false;
        invalidate();
        return;
    }
    if (last_row >= 0) invalidateCells(0, first_row, columns_ - 1, last_row);
    if (bar_dirty) invalidateScrollbar();
}

void TerminalView::markFullRedraw() { full_redraw_ = true; }

void TerminalView::renderVt(TerminalEmulator& vt, bool show_cursor, bool full) {
    if (pixels_ == nullptr) return;
    // The emulator paints cells the CLI row cache knows nothing about, so the
    // next render() has to start from a clean slate.
    full_redraw_ = true;
    full = full || vt.fullRedrawPending();
    const size_t cols = std::min(vt.columns(), static_cast<size_t>(columns_));
    const size_t rows = std::min(vt.rows(), static_cast<size_t>(rows_));
    if (full) {
        std::fill(pixels_, pixels_ + (static_cast<size_t>(width_) * height_), kBg);
        vt.markAllDirty();
    }
    int first_col = static_cast<int>(cols);
    int first_row = static_cast<int>(rows);
    int last_col = -1;
    int last_row = -1;
    for (size_t row = 0; row < rows; ++row) {
        for (size_t col = 0; col < cols; ++col) {
            const auto& cell = vt.displayCell(col, row);
            if (cell.continuation) continue;
            const int cells = cell.wide && col + 1 < cols ? 2 : 1;
            if (!full && !cell.dirty &&
                !(cells == 2 && vt.displayCell(col + 1, row).dirty)) {
                continue;
            }

            uint32_t fg = cell.fg;
            uint32_t bg = cell.bg;
            if (cell.inverse) std::swap(fg, bg);
            const uint16_t fg16 = terminalColor(fg, cell.bold);
            const uint16_t bg16 = terminalColor(bg, false);
            fillCell(static_cast<int>(col), static_cast<int>(row), bg16);
            if (cells == 2) fillCell(static_cast<int>(col) + 1, static_cast<int>(row), bg16);
            const uint32_t cp = cell.cp ? cell.cp : ' ';
            drawCodepoint(static_cast<int>(col), static_cast<int>(row), cp, cells, fg16);
            first_col = std::min(first_col, static_cast<int>(col));
            first_row = std::min(first_row, static_cast<int>(row));
            last_col = std::max(last_col, static_cast<int>(col) + cells - 1);
            last_row = std::max(last_row, static_cast<int>(row));
        }
    }
    if (show_cursor && vt.cursorVisible() && vt.scrollbackOffset() == 0 && cols > 0 && rows > 0) {
        const int cursor_col = static_cast<int>(std::min(vt.cursorColumn(), cols - 1));
        const int cursor_row = static_cast<int>(std::min(vt.cursorRow(), rows - 1));
        drawCursor(cursor_col, cursor_row, kCursor);
        first_col = std::min(first_col, cursor_col);
        first_row = std::min(first_row, cursor_row);
        last_col = std::max(last_col, cursor_col);
        last_row = std::max(last_row, cursor_row);
    }
    const size_t visible = rows ? rows : 1;
    const size_t history = vt.scrollbackRows();
    const bool cells_overlap_bar = last_col >= 0 && (last_col + 1) * cell_w_ > width_ - kScrollbarW;
    const bool bar_dirty = drawScrollbar(vt.scrollbackOffset(), history, visible, history + visible,
                                         full || cells_overlap_bar);
    vt.clearDirty();
    if (full) invalidate();
    else {
        invalidateCells(first_col, first_row, last_col, last_row);
        if (bar_dirty) invalidateScrollbar();
    }
}

}  // namespace tabby
