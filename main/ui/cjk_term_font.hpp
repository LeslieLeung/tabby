#pragma once

#include <cstdint>

namespace tabby {

// Terminal CJK glyphs. ASCII/box-drawing stay on Terminus; missing codepoints
// try an optional SD bitmap font, then the firmware efont already used by the
// settings UI. The SD file is not required for basic Simplified/Traditional
// Chinese.
class CjkTermFont {
public:
    // Load from microSD if a known font file is present. The UI loop should
    // keep wait=false so a background parse cannot stall LVGL.
    static bool tryLoadFromSd(bool wait = false);
    // True once after an SD face is published, so the UI can full-redraw.
    static bool takeNewlyLoaded();
    static bool ready();
    static const char* status();
    static void setProgressHook(void (*hook)(const char* text));

    // Scale a 1bpp source glyph into dst_w x dst_h. Returns true if painted.
    static bool drawGlyph(uint16_t* pixels, int stride, int width, int height, uint32_t cp, int x, int y,
                          int dst_w, int dst_h, uint16_t fg);
};

}  // namespace tabby
