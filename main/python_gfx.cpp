#include "tabby/python_gfx.hpp"

#include "tabby/terminus_bitmap.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace tabby {
namespace {

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

uint16_t gfxColor(uint32_t rgb) {
    return rgb565(static_cast<uint8_t>((rgb >> 16) & 0xFF), static_cast<uint8_t>((rgb >> 8) & 0xFF),
                  static_cast<uint8_t>(rgb & 0xFF));
}

int gfxToInt(std::string& rest, int fallback = 0) {
    while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.erase(rest.begin());
    if (rest.empty()) return fallback;
    char* end = nullptr;
    const long value = std::strtol(rest.c_str(), &end, 0);
    if (end == rest.c_str()) return fallback;
    rest.erase(0, static_cast<size_t>(end - rest.c_str()));
    return static_cast<int>(value);
}

std::string gfxTakeToken(std::string& rest) {
    while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.erase(rest.begin());
    const auto split = rest.find(' ');
    std::string token = split == std::string::npos ? rest : rest.substr(0, split);
    rest = split == std::string::npos ? std::string() : rest.substr(split + 1);
    return token;
}

bool gfxBitAt(const std::string& bits, int index) {
    if (index < 0 || static_cast<size_t>(index) >= bits.size()) return false;
    const char c = bits[static_cast<size_t>(index)];
    return c == '1' || c == '#' || c == 'x' || c == 'X';
}

PythonGfx g_gfx;

}  // namespace

PythonGfx& pythonGfx() { return g_gfx; }

void PythonGfx::begin(uint16_t* pixels, int width, int height, int stride) {
    pixels_ = pixels;
    width_ = width;
    height_ = height;
    stride_ = stride > 0 ? stride : width;
}

void PythonGfx::drawPixel(int x, int y, uint16_t color) {
    if (pixels_ == nullptr || x < 0 || y < 0 || x >= width_ || y >= height_) return;
    pixels_[y * stride_ + x] = color;
}

void PythonGfx::fillRect(int x, int y, int w, int h, uint16_t color) {
    if (pixels_ == nullptr || w <= 0 || h <= 0) return;
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(width_, x + w);
    const int y1 = std::min(height_, y + h);
    for (int yy = y0; yy < y1; ++yy) {
        uint16_t* line = pixels_ + yy * stride_;
        for (int xx = x0; xx < x1; ++xx) line[xx] = color;
    }
}

void PythonGfx::drawLine(int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = std::abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        drawPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void PythonGfx::drawCircle(int x, int y, int r, uint16_t color, bool fill) {
    if (r < 0) return;
    if (fill) {
        for (int dy = -r; dy <= r; ++dy) {
            const int span = static_cast<int>(std::sqrt(static_cast<double>(r * r - dy * dy)));
            fillRect(x - span, y + dy, span * 2 + 1, 1, color);
        }
        return;
    }
    int xx = r;
    int yy = 0;
    int err = 0;
    while (xx >= yy) {
        drawPixel(x + xx, y + yy, color);
        drawPixel(x + yy, y + xx, color);
        drawPixel(x - yy, y + xx, color);
        drawPixel(x - xx, y + yy, color);
        drawPixel(x - xx, y - yy, color);
        drawPixel(x - yy, y - xx, color);
        drawPixel(x + yy, y - xx, color);
        drawPixel(x + xx, y - yy, color);
        if (err <= 0) {
            ++yy;
            err += 2 * yy + 1;
        }
        if (err > 0) {
            --xx;
            err -= 2 * xx + 1;
        }
    }
}

void PythonGfx::drawText(int x, int y, const std::string& text, uint16_t color) {
    if (pixels_ == nullptr) return;
    const TerminusBitmap::Font& font = TerminusBitmap::fontForHeight(16);
    int cx = x;
    for (unsigned char c : text) {
        TerminusBitmap::drawGlyph(pixels_, stride_, width_, height_, font, c, cx, y, color);
        cx += font.targetW;
    }
}

bool PythonGfx::command(const char* command) {
    if (command == nullptr || pixels_ == nullptr) return true;
    const std::lock_guard<std::mutex> lock(frame_mutex_);
    std::string rest = command;
    std::string op = gfxTakeToken(rest);
    std::transform(op.begin(), op.end(), op.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    active_.store(true, std::memory_order_release);
    const int canvas_w = width_;
    const int canvas_h = height_;

    if (op == "clear") {
        fillRect(0, 0, canvas_w, canvas_h, gfxColor(static_cast<uint32_t>(gfxToInt(rest, 0))));
        return true;
    }
    if (op == "present") {
        present_requested_.store(true, std::memory_order_release);
        if (abort_fn_ && abort_fn_()) return false;
        return true;
    }
    if (op == "px") {
        const int x = gfxToInt(rest);
        const int y = gfxToInt(rest);
        const uint16_t color = gfxColor(static_cast<uint32_t>(gfxToInt(rest, 0xffffff)));
        drawPixel(x, y, color);
        return true;
    }
    if (op == "line") {
        const int x0 = gfxToInt(rest);
        const int y0 = gfxToInt(rest);
        const int x1 = gfxToInt(rest);
        const int y1 = gfxToInt(rest);
        drawLine(x0, y0, x1, y1, gfxColor(static_cast<uint32_t>(gfxToInt(rest, 0xffffff))));
        return true;
    }
    if (op == "rect") {
        const int x = gfxToInt(rest);
        const int y = gfxToInt(rest);
        const int w = gfxToInt(rest);
        const int h = gfxToInt(rest);
        const uint16_t color = gfxColor(static_cast<uint32_t>(gfxToInt(rest, 0xffffff)));
        const bool fill = gfxToInt(rest, 1) != 0;
        if (fill) fillRect(x, y, w, h, color);
        else {
            fillRect(x, y, w, 1, color);
            fillRect(x, y + h - 1, w, 1, color);
            fillRect(x, y, 1, h, color);
            fillRect(x + w - 1, y, 1, h, color);
        }
        return true;
    }
    if (op == "circle") {
        const int x = gfxToInt(rest);
        const int y = gfxToInt(rest);
        const int r = gfxToInt(rest);
        const uint16_t color = gfxColor(static_cast<uint32_t>(gfxToInt(rest, 0xffffff)));
        drawCircle(x, y, r, color, gfxToInt(rest, 1) != 0);
        return true;
    }
    if (op == "mono") {
        const int x0 = gfxToInt(rest);
        const int y0 = gfxToInt(rest);
        const int cols = gfxToInt(rest);
        const int rows = gfxToInt(rest);
        const int cell = gfxToInt(rest);
        const uint16_t fg = gfxColor(static_cast<uint32_t>(gfxToInt(rest, 0xffffff)));
        const uint16_t bg = gfxColor(static_cast<uint32_t>(gfxToInt(rest, 0)));
        while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.erase(rest.begin());
        if (cols <= 0 || rows <= 0 || cell <= 0 || cols > 128 || rows > 128) return true;
        for (int y = 0; y < rows; ++y) {
            for (int x = 0; x < cols; ++x) {
                const bool on = gfxBitAt(rest, y * cols + x);
                const int pw = std::max(1, cell - 2);
                const int ph = std::max(1, cell - 2);
                fillRect(x0 + x * cell + 1, y0 + y * cell + 1, pw, ph, on ? fg : bg);
            }
        }
        return true;
    }
    if (op == "text") {
        const int x = gfxToInt(rest);
        const int y = gfxToInt(rest);
        const uint16_t color = gfxColor(static_cast<uint32_t>(gfxToInt(rest, 0xffffff)));
        while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.erase(rest.begin());
        drawText(x, y, rest, color);
        return true;
    }
    return true;
}

}  // namespace tabby

extern "C" int tab5_python_gfx_width() { return tabby::pythonGfx().width(); }
extern "C" int tab5_python_gfx_height() { return tabby::pythonGfx().height(); }
extern "C" bool tab5_python_gfx_command(const char* command) { return tabby::pythonGfx().command(command); }
