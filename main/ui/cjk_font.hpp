#pragma once

#include "lvgl.h"

#include <cstdint>

namespace tabby {

void CjkFontInit();
lv_font_t* CjkFont16();
// 16x16 1bpp (32 bytes, MSB-first, row-major). Used by the terminal blitter
// so SSH/serial CJK can reuse the firmware efont without a second decoder.
bool CjkFontGlyph(uint32_t letter, uint8_t bitmap[32]);

}  // namespace tabby
