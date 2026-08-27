#include "cjk_term_font.hpp"

#include "cjk_font.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace tabby {
namespace {

constexpr char kTag[] = "tabby_cjk";
constexpr uint32_t kMaxGlyphs = 80000;
constexpr uint8_t kMaxSrc = 32;
constexpr size_t kEfontCache = 256;
constexpr char kBinMagic[4] = {'C', 'J', 'K', '1'};

const char* kBinPaths[] = {
    "/sd/fonts/cjk16.bin",
    "/sd/fonts/cjk.bin",
    "/sd/tabby/fonts/cjk16.bin",
};
const char* kHexPaths[] = {
    "/sd/fonts/unifont.hex",
    "/sd/unifont.hex",
};

struct __attribute__((packed)) BinHeader {
    char magic[4];
    uint16_t width;
    uint16_t height;
    uint32_t count;
    uint16_t row_bytes;
    uint16_t reserved;
};

struct Face {
    uint8_t src_w{16};
    uint8_t src_h{16};
    uint8_t row_bytes{2};
    uint32_t count{0};
    const uint16_t* codepoints{nullptr};
    const uint8_t* bitmaps{nullptr};
    char source[48]{};
};

struct EfontCacheEntry {
    uint32_t cp{0};
    uint8_t bits[32]{};
    bool ok{false};
};

std::mutex g_load_mutex;
std::atomic<const Face*> g_sd_face{nullptr};
std::atomic<bool> g_newly_loaded{false};
Face g_face{};
void* g_blob{nullptr};
char g_status[80] = "firmware efont";
EfontCacheEntry g_efont_cache[kEfontCache];
int64_t g_last_try_us{0};
void (*g_progress)(const char*) = nullptr;

void noteProgress(const char* text) {
    if (g_progress != nullptr && text != nullptr) g_progress(text);
}

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool parseHexByte(const char* p, uint8_t& out) {
    const int hi = hexNibble(p[0]);
    const int lo = hexNibble(p[1]);
    if (hi < 0 || lo < 0) return false;
    out = static_cast<uint8_t>((hi << 4) | lo);
    return true;
}

int findGlyph(const Face& face, uint32_t cp) {
    if (cp > 0xFFFF || face.count == 0 || face.codepoints == nullptr) return -1;
    int lo = 0;
    int hi = static_cast<int>(face.count) - 1;
    const uint16_t key = static_cast<uint16_t>(cp);
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const uint16_t value = face.codepoints[mid];
        if (value == key) return mid;
        if (value < key) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

void fillRect(uint16_t* pixels, int stride, int width, int height, int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0 || pixels == nullptr) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > width) x1 = width;
    if (y1 > height) y1 = height;
    for (int row = y0; row < y1; ++row) {
        uint16_t* line = pixels + row * stride + x0;
        for (int col = x0; col < x1; ++col) *line++ = color;
    }
}

void blitBitmap(uint16_t* pixels, int stride, int width, int height, const uint8_t* bitmap, uint8_t src_w,
                uint8_t src_h, uint8_t row_bytes, int x, int y, int dst_w, int dst_h, uint16_t fg) {
    if (bitmap == nullptr || src_w == 0 || src_h == 0 || dst_w <= 0 || dst_h <= 0) return;
    const int x_scale = dst_w == src_w ? 1 : 0;
    const int y_scale = dst_h == src_h ? 1 : 0;
    for (uint8_t sy = 0; sy < src_h; ++sy) {
        const int dy0 = y_scale ? y + sy : y + (sy * dst_h) / src_h;
        int dy1 = y_scale ? dy0 + 1 : y + ((sy + 1) * dst_h) / src_h;
        if (dy1 <= dy0) dy1 = dy0 + 1;
        for (uint8_t sx = 0; sx < src_w; ++sx) {
            const uint8_t row = bitmap[sy * row_bytes + sx / 8];
            if ((row & static_cast<uint8_t>(0x80 >> (sx % 8))) == 0) continue;
            const int dx0 = x_scale ? x + sx : x + (sx * dst_w) / src_w;
            int dx1 = x_scale ? dx0 + 1 : x + ((sx + 1) * dst_w) / src_w;
            if (dx1 <= dx0) dx1 = dx0 + 1;
            fillRect(pixels, stride, width, height, dx0, dy0, dx1 - dx0, dy1 - dy0, fg);
        }
    }
}

void publishFace(void* blob, uint8_t src_w, uint8_t src_h, uint8_t row_bytes, uint32_t count,
                 uint16_t* codepoints, uint8_t* bitmaps, const char* source) {
    void* old = g_blob;
    g_blob = blob;
    g_face.src_w = src_w;
    g_face.src_h = src_h;
    g_face.row_bytes = row_bytes;
    g_face.count = count;
    g_face.codepoints = codepoints;
    g_face.bitmaps = bitmaps;
    std::snprintf(g_face.source, sizeof(g_face.source), "%s", source);
    std::snprintf(g_status, sizeof(g_status), "sd %s n=%u", source, static_cast<unsigned>(count));
    g_sd_face.store(&g_face, std::memory_order_release);
    g_newly_loaded.store(true, std::memory_order_release);
    if (old != nullptr && old != blob) heap_caps_free(old);
}

bool loadBin(const char* path) {
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;
    noteProgress("Loading fonts...");
    BinHeader header{};
    if (std::fread(&header, 1, sizeof(header), file) != sizeof(header)) {
        std::fclose(file);
        return false;
    }
    if (std::memcmp(header.magic, kBinMagic, 4) != 0 || header.width == 0 || header.height == 0 ||
        header.width > kMaxSrc || header.height > kMaxSrc || header.count == 0 || header.count > kMaxGlyphs ||
        header.row_bytes == 0 || header.row_bytes > ((kMaxSrc + 7) / 8) ||
        header.row_bytes < static_cast<uint16_t>((header.width + 7) / 8)) {
        std::fclose(file);
        ESP_LOGW(kTag, "reject %s: bad header", path);
        return false;
    }
    const size_t glyph_bytes = static_cast<size_t>(header.height) * header.row_bytes;
    const size_t index_bytes = static_cast<size_t>(header.count) * sizeof(uint16_t);
    const size_t bitmap_bytes = static_cast<size_t>(header.count) * glyph_bytes;
    uint8_t* blob = static_cast<uint8_t*>(
        heap_caps_malloc(index_bytes + bitmap_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (blob == nullptr) {
        blob = static_cast<uint8_t*>(heap_caps_malloc(index_bytes + bitmap_bytes, MALLOC_CAP_8BIT));
    }
    if (blob == nullptr) {
        std::fclose(file);
        ESP_LOGE(kTag, "OOM loading %s (%u glyphs)", path, static_cast<unsigned>(header.count));
        return false;
    }
    if (std::fread(blob, 1, index_bytes + bitmap_bytes, file) != index_bytes + bitmap_bytes) {
        heap_caps_free(blob);
        std::fclose(file);
        ESP_LOGW(kTag, "short read %s", path);
        return false;
    }
    std::fclose(file);
    const char* name = path;
    if (std::strncmp(path, "/sd", 3) == 0) name = path + 3;
    publishFace(blob, static_cast<uint8_t>(header.width), static_cast<uint8_t>(header.height),
                static_cast<uint8_t>(header.row_bytes), header.count, reinterpret_cast<uint16_t*>(blob),
                blob + index_bytes, name);
    ESP_LOGI(kTag, "loaded %s (%u glyphs %ux%u)", path, static_cast<unsigned>(header.count), header.width,
             header.height);
    return true;
}

bool parseUnifontLine(const char* line, uint16_t& cp, uint8_t bitmap[32]) {
    if (line == nullptr || line[0] == '#' || line[0] == '\0') return false;
    uint32_t value = 0;
    size_t i = 0;
    for (; i < 6 && hexNibble(line[i]) >= 0; ++i) {
        value = (value << 4) | static_cast<uint32_t>(hexNibble(line[i]));
    }
    if (i < 4 || line[i] != ':' || value > 0xFFFF) return false;
    ++i;
    size_t hex_len = 0;
    while (hexNibble(line[i + hex_len]) >= 0) ++hex_len;
    if (hex_len != 64) return false;
    for (size_t b = 0; b < 32; ++b) {
        if (!parseHexByte(line + i + b * 2, bitmap[b])) return false;
    }
    cp = static_cast<uint16_t>(value);
    return true;
}

bool loadHex(const char* path) {
    FILE* file = std::fopen(path, "r");
    if (file == nullptr) return false;
    noteProgress("Indexing fonts...");
    char line[96];
    uint32_t count = 0;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        uint16_t cp = 0;
        uint8_t bits[32];
        if (parseUnifontLine(line, cp, bits)) ++count;
    }
    if (count == 0 || count > kMaxGlyphs) {
        std::fclose(file);
        ESP_LOGW(kTag, "no 16x16 glyphs in %s", path);
        return false;
    }
    const size_t index_bytes = static_cast<size_t>(count) * sizeof(uint16_t);
    const size_t bitmap_bytes = static_cast<size_t>(count) * 32;
    uint8_t* blob = static_cast<uint8_t*>(
        heap_caps_malloc(index_bytes + bitmap_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (blob == nullptr) {
        blob = static_cast<uint8_t*>(heap_caps_malloc(index_bytes + bitmap_bytes, MALLOC_CAP_8BIT));
    }
    if (blob == nullptr) {
        std::fclose(file);
        ESP_LOGE(kTag, "OOM parsing %s (%u glyphs)", path, static_cast<unsigned>(count));
        return false;
    }
    std::rewind(file);
    noteProgress("Loading fonts...");
    auto* codepoints = reinterpret_cast<uint16_t*>(blob);
    uint8_t* bitmaps = blob + index_bytes;
    uint32_t filled = 0;
    while (filled < count && std::fgets(line, sizeof(line), file) != nullptr) {
        uint16_t cp = 0;
        if (!parseUnifontLine(line, cp, bitmaps + static_cast<size_t>(filled) * 32)) continue;
        codepoints[filled] = cp;
        ++filled;
    }
    std::fclose(file);
    if (filled == 0) {
        heap_caps_free(blob);
        return false;
    }
    const char* name = path;
    if (std::strncmp(path, "/sd", 3) == 0) name = path + 3;
    publishFace(blob, 16, 16, 2, filled, codepoints, bitmaps, name);
    ESP_LOGI(kTag, "parsed %s (%u glyphs)", path, static_cast<unsigned>(filled));
    return true;
}

const uint8_t* efontBits(uint32_t cp) {
    EfontCacheEntry& slot = g_efont_cache[cp & (kEfontCache - 1)];
    if (slot.ok && slot.cp == cp) return slot.bits;
    uint8_t bits[32];
    if (!CjkFontGlyph(cp, bits)) return nullptr;
    slot.cp = cp;
    std::memcpy(slot.bits, bits, 32);
    slot.ok = true;
    return slot.bits;
}

}  // namespace

bool CjkTermFont::tryLoadFromSd(bool wait) {
    if (g_sd_face.load(std::memory_order_acquire) != nullptr) return false;
    std::unique_lock<std::mutex> lock(g_load_mutex, std::defer_lock);
    if (wait) lock.lock();
    else if (!lock.try_lock()) return false;
    if (g_sd_face.load(std::memory_order_relaxed) != nullptr) return false;
    const int64_t now = esp_timer_get_time();
    if (g_last_try_us != 0 && now - g_last_try_us < 3000000) return false;
    g_last_try_us = now;
    for (const char* path : kBinPaths) {
        if (loadBin(path)) return true;
    }
    for (const char* path : kHexPaths) {
        if (loadHex(path)) return true;
    }
    return false;
}

bool CjkTermFont::takeNewlyLoaded() {
    return g_newly_loaded.exchange(false, std::memory_order_acq_rel);
}

bool CjkTermFont::ready() { return g_sd_face.load(std::memory_order_acquire) != nullptr; }

const char* CjkTermFont::status() { return g_status; }

void CjkTermFont::setProgressHook(void (*hook)(const char* text)) { g_progress = hook; }

bool CjkTermFont::drawGlyph(uint16_t* pixels, int stride, int width, int height, uint32_t cp, int x, int y,
                            int dst_w, int dst_h, uint16_t fg) {
    const Face* face = g_sd_face.load(std::memory_order_acquire);
    if (face != nullptr) {
        const int index = findGlyph(*face, cp);
        if (index >= 0) {
            const size_t glyph_bytes = static_cast<size_t>(face->src_h) * face->row_bytes;
            blitBitmap(pixels, stride, width, height, face->bitmaps + static_cast<size_t>(index) * glyph_bytes,
                       face->src_w, face->src_h, face->row_bytes, x, y, dst_w, dst_h, fg);
            return true;
        }
    }
    const uint8_t* bits = efontBits(cp);
    if (bits == nullptr) return false;
    blitBitmap(pixels, stride, width, height, bits, 16, 16, 2, x, y, dst_w, dst_h, fg);
    return true;
}

}  // namespace tabby
