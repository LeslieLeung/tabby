#include "cjk_font.hpp"

#include "lgfx/Fonts/efont/lgfx_efont_cn.h"
#include "lgfx/Fonts/efont/lgfx_efont_tw.h"

#include <cstring>

namespace tabby {
namespace {

constexpr size_t kBitmapBytes = 128;

struct U8g2Face {
    const uint8_t* data;
    lv_font_t font{};
};

struct GlyphCache {
    const uint8_t* face{nullptr};
    uint32_t letter{0};
    bool ok{false};
    lv_font_glyph_dsc_t dsc{};
    uint8_t bitmap[kBitmapBytes]{};
};

U8g2Face g_cn{};
U8g2Face g_tw{};
GlyphCache g_cache;

struct BitReader {
    const uint8_t* ptr;
    uint8_t bit_pos;

    explicit BitReader(const uint8_t* p) : ptr(p), bit_pos(0) {}

    uint8_t u(uint8_t cnt) {
        uint8_t val = static_cast<uint8_t>(*ptr >> bit_pos);
        uint8_t next = static_cast<uint8_t>(bit_pos + cnt);
        if (next >= 8) {
            next = static_cast<uint8_t>(next - 8);
            val = static_cast<uint8_t>(val | (*++ptr << (8 - bit_pos)));
        }
        bit_pos = next;
        return static_cast<uint8_t>(val & ((1U << cnt) - 1));
    }

    int8_t s(uint8_t cnt) { return static_cast<int8_t>(u(cnt) - (1 << (cnt - 1))); }
};

uint8_t u8(const uint8_t* p, size_t i) { return p[i]; }
uint16_t u16(const uint8_t* p, size_t i) {
    return static_cast<uint16_t>((p[i] << 8) | p[i + 1]);
}

const uint8_t* findGlyph(const uint8_t* font, uint16_t encoding) {
    const uint8_t* p = font + 23;
    if (encoding <= 255) {
        if (encoding >= 'a') p += u16(font, 19);
        else if (encoding >= 'A') p += u16(font, 17);
        for (; u8(p, 1); p += u8(p, 1)) {
            if (u8(p, 0) == encoding) return p + 2;
        }
        return nullptr;
    }

    p += u16(font, 21);
    const uint8_t* lut = p;
    uint16_t last = 0;
    do {
        p += u16(lut, 0);
        last = u16(lut, 2);
        lut += 4;
    } while (last < encoding);

    for (;;) {
        const uint16_t code = u16(p, 0);
        if (code == 0) return nullptr;
        if (code == encoding) return p + 3;
        p += u8(p, 2);
    }
}

void setBit(uint8_t* bitmap, uint32_t bit) {
    bitmap[bit >> 3] |= static_cast<uint8_t>(0x80 >> (bit & 7));
}

bool decodeGlyph(const uint8_t* font, uint32_t letter, lv_font_glyph_dsc_t* dsc, uint8_t* bitmap) {
    if (letter == 0 || letter > 0xFFFF) return false;
    const uint8_t* glyph = findGlyph(font, static_cast<uint16_t>(letter));
    if (glyph == nullptr) return false;

    BitReader bits(glyph);
    const uint8_t wbits = u8(font, 4);
    const uint8_t hbits = u8(font, 5);
    const uint8_t xbits = u8(font, 6);
    const uint8_t ybits = u8(font, 7);
    const uint8_t dxbits = u8(font, 8);
    const uint8_t bp0 = u8(font, 2);
    const uint8_t bp1 = u8(font, 3);

    const uint32_t w = bits.u(wbits);
    const uint32_t h = bits.u(hbits);
    const int8_t xoff = bits.s(xbits);
    const int8_t yoff = bits.s(ybits);
    const int8_t adv = bits.s(dxbits);
    if (w > 32 || h > 32) return false;

    std::memset(dsc, 0, sizeof(*dsc));
    int16_t advance = adv;
    if (advance <= 0) advance = static_cast<int16_t>(xoff + static_cast<int16_t>(w));
    if (advance < 1) advance = 1;
    dsc->adv_w = static_cast<uint16_t>(advance);
    dsc->box_w = static_cast<uint16_t>(w);
    dsc->box_h = static_cast<uint16_t>(h);
    dsc->ofs_x = xoff;
    dsc->ofs_y = yoff;
    dsc->bpp = 1;

    const uint32_t bits_total = w * h;
    const size_t bytes = static_cast<size_t>((bits_total + 7) / 8);
    if (bytes > kBitmapBytes) return false;
    std::memset(bitmap, 0, bytes);
    if (w == 0 || h == 0) return true;

    uint32_t lx = 0;
    uint32_t ly = 0;
    do {
        uint32_t run[2] = {bits.u(bp0), bits.u(bp1)};
        bool ink = false;
        do {
            uint32_t length = run[ink];
            while (length) {
                const uint32_t chunk = length > w - lx ? w - lx : length;
                length -= chunk;
                if (ink) {
                    const uint32_t base = ly * w + lx;
                    for (uint32_t x = 0; x < chunk; ++x) setBit(bitmap, base + x);
                }
                lx += chunk;
                if (lx == w) {
                    lx = 0;
                    ++ly;
                }
            }
            ink = !ink;
        } while (ink || bits.u(1) != 0);
    } while (ly < h);
    return true;
}

bool ensureCache(const uint8_t* font, uint32_t letter) {
    if (g_cache.ok && g_cache.face == font && g_cache.letter == letter) return true;
    g_cache.ok = decodeGlyph(font, letter, &g_cache.dsc, g_cache.bitmap);
    g_cache.face = font;
    g_cache.letter = letter;
    return g_cache.ok;
}

bool glyphDsc(const lv_font_t* font, lv_font_glyph_dsc_t* dsc, uint32_t letter, uint32_t) {
    const auto* face = static_cast<const U8g2Face*>(font->dsc);
    if (face == nullptr || dsc == nullptr) return false;
    if (!ensureCache(face->data, letter)) return false;
    *dsc = g_cache.dsc;
    return true;
}

const uint8_t* glyphBitmap(const lv_font_t* font, uint32_t letter) {
    const auto* face = static_cast<const U8g2Face*>(font->dsc);
    if (face == nullptr) return nullptr;
    if (!ensureCache(face->data, letter)) return nullptr;
    return g_cache.bitmap;
}

void initFace(U8g2Face* face, const uint8_t* data, lv_coord_t line_height, lv_coord_t baseline) {
    face->data = data;
    face->font.get_glyph_dsc = glyphDsc;
    face->font.get_glyph_bitmap = glyphBitmap;
    face->font.line_height = line_height;
    face->font.base_line = baseline;
    face->font.subpx = LV_FONT_SUBPX_NONE;
    face->font.underline_position = -1;
    face->font.underline_thickness = 1;
    face->font.dsc = face;
    face->font.fallback = nullptr;
}

}  // namespace

void CjkFontInit() {
    initFace(&g_cn, lgfx_efont_cn_16, 18, 3);
    initFace(&g_tw, lgfx_efont_tw_16, 18, 3);
    g_cn.font.fallback = &g_tw.font;
}

lv_font_t* CjkFont16() { return &g_cn.font; }

bool CjkFontGlyph(uint32_t letter, uint8_t bitmap[32]) {
    if (bitmap == nullptr || letter == 0 || letter > 0xFFFF) return false;
    lv_font_glyph_dsc_t dsc{};
    uint8_t raw[kBitmapBytes];
    bool ok = false;
    if (g_cn.data != nullptr) ok = decodeGlyph(g_cn.data, letter, &dsc, raw);
    if (!ok && g_tw.data != nullptr) ok = decodeGlyph(g_tw.data, letter, &dsc, raw);
    if (!ok) return false;
    std::memset(bitmap, 0, 32);
    if (dsc.box_w == 0 || dsc.box_h == 0) return true;
    const uint32_t src_w = dsc.box_w;
    const uint32_t src_h = dsc.box_h;
    for (int y = 0; y < 16; ++y) {
        const uint32_t sy = static_cast<uint32_t>(y) * src_h / 16;
        for (int x = 0; x < 16; ++x) {
            const uint32_t sx = static_cast<uint32_t>(x) * src_w / 16;
            const uint32_t bit = sy * src_w + sx;
            if ((raw[bit >> 3] & static_cast<uint8_t>(0x80 >> (bit & 7))) == 0) continue;
            const uint32_t out = static_cast<uint32_t>(y * 16 + x);
            bitmap[out >> 3] |= static_cast<uint8_t>(0x80 >> (out & 7));
        }
    }
    return true;
}

}  // namespace tabby
