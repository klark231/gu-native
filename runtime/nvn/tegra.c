// Tegra X1 block-linear layout. A GOB is 64 bytes x 8 rows; blocks stack GOBs vertically
// (block height) and in depth (block depth). Levels shrink their block height as they shrink.
#include "tegra.h"

#include <string.h>

static uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
static uint64_t align_up(uint64_t a, uint64_t b) { return (a + b - 1) / b * b; }

uint32_t tegra_default_block_height_log2(uint32_t height_in_blocks) {
    uint32_t gobs = div_up(height_in_blocks, 8);
    uint32_t log2 = 0;
    while ((1u << log2) < gobs && log2 < 4) log2++;
    return log2;
}

static uint32_t block_depth_for(uint32_t depth) {
    uint32_t log2 = 0;
    while ((1u << log2) < depth && log2 < 5) log2++;
    return log2;
}

void tegra_layout_init(TexLayout *t) {
    uint32_t hb = div_up(t->height, t->fmt->bh);
    t->block_height_log2 = tegra_default_block_height_log2(hb);
    t->block_depth_log2 = t->depth > 1 ? block_depth_for(t->depth) : 0;
}

// Block height shrinks with the level so a block never exceeds the level height.
static uint32_t level_block_height(uint32_t bh_log2, uint32_t hb) {
    while (bh_log2 > 0 && (8u << (bh_log2 - 1)) >= hb) bh_log2--;
    return bh_log2;
}

static uint32_t level_block_depth(uint32_t bd_log2, uint32_t d) {
    while (bd_log2 > 0 && (1u << (bd_log2 - 1)) >= d) bd_log2--;
    return bd_log2;
}

static uint64_t level_size(const TexLayout *t, uint32_t level, LevelLayout *out) {
    uint32_t w = t->width >> level, h = t->height >> level, d = t->depth >> level;
    if (!w) w = 1;
    if (!h) h = 1;
    if (!d) d = 1;
    uint32_t wb = div_up(w, t->fmt->bw), hb = div_up(h, t->fmt->bh);
    uint32_t bh = level_block_height(t->block_height_log2, hb);
    uint32_t bd = level_block_depth(t->block_depth_log2, d);
    uint32_t gob_rows = 8u << bh, gob_depth = 1u << bd;
    uint64_t width_gobs = div_up(wb * t->fmt->bpb, 64);
    uint64_t size = width_gobs * 512 * (1u << bh) * (1u << bd) * div_up(hb, gob_rows) * div_up(d, gob_depth);
    if (out) {
        out->w = w; out->h = h; out->d = d;
        out->wb = wb; out->hb = hb;
        out->block_height_log2 = bh;
        out->block_depth_log2 = bd;
    }
    return size;
}

LevelLayout tegra_level(const TexLayout *t, uint32_t level) {
    LevelLayout l = {0};
    uint64_t off = 0;
    for (uint32_t i = 0; i < level; i++) off += level_size(t, i, NULL);
    level_size(t, level, &l);
    l.offset = off;
    return l;
}

uint64_t tegra_layer_size(const TexLayout *t) {
    uint64_t size = 0;
    for (uint32_t i = 0; i < t->levels; i++) size += level_size(t, i, NULL);
    if (t->layers > 1) {
        // layers are aligned to the level-0 block size
        uint32_t bh = t->block_height_log2;
        size = align_up(size, 512ull << bh);
    }
    return size;
}

uint64_t tegra_total_size(const TexLayout *t) {
    return tegra_layer_size(t) * (t->layers ? t->layers : 1);
}

// Within a GOB, its 64x8 bytes are 16-byte sectors: row yi starts at ((yi >> 1) << 6) | ((yi & 1) << 4), and its
// bytes 0-15, 16-31, 32-47, 48-63 lie at +0, +32, +256, +288 from there. The GOB to the right is a whole block further
// on (the blocks of a row of blocks side by side, slices of blocks after all the rows). Where each 16 bytes used to cost
// the whole address sum and a call to memcpy, a GOB is now copied whole, its 8 rows of 64 bytes (four fixed 16-byte
// copies each) in a row: the GOB's 512 bytes are read together, and each linear row gets its 64 bytes in one go (the
// staging memory is write-combined: whole lines). The rows of a last, partial GOB row go one by one.
static inline uint32_t row_in_gob(uint32_t yi) { return ((yi >> 1) << 6) | ((yi & 1) << 4); }
static inline uint32_t sector(uint32_t xi) { return ((xi >> 5) << 8) | (((xi >> 4) & 1) << 5); }
static inline void copy16(uint8_t *d, const uint8_t *s) { memcpy(d, s, 16); }

// One GOB row: g its start in the GOB, l the linear row's 64 bytes (only `tail` of them at a row's end)
static inline void gob_row(uint8_t *g, uint8_t *l, bool to_linear) {
    if (to_linear) copy16(l, g), copy16(l + 16, g + 32), copy16(l + 32, g + 256), copy16(l + 48, g + 288);
    else copy16(g, l), copy16(g + 32, l + 16), copy16(g + 256, l + 32), copy16(g + 288, l + 48);
}
static inline void gob_row_tail(uint8_t *g, uint8_t *l, uint32_t tail, bool to_linear) {
    for (uint32_t x = 0; x < tail; x += 16) {
        uint32_t n = tail - x < 16 ? tail - x : 16;
        if (to_linear) memcpy(l + x, g + sector(x), n);
        else memcpy(g + sector(x), l + x, n);
    }
}

static inline __attribute__((always_inline)) void copy_surface(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb,
                                                               uint32_t d, uint32_t bpb, uint32_t bh_log2, uint32_t bd_log2,
                                                               bool to_linear) {
    uint64_t row_bytes = (uint64_t)wb * bpb;
    uint32_t width_gobs = (uint32_t)((row_bytes + 63) / 64), full = (uint32_t)(row_bytes / 64), tail = (uint32_t)(row_bytes & 63);
    uint32_t gob_rows = 8u << bh_log2, bh_mask = (1u << bh_log2) - 1, bd_mask = (1u << bd_log2) - 1;
    uint64_t blocks_rows = (hb + gob_rows - 1) / gob_rows;
    uint64_t block_size = 512ull << (bh_log2 + bd_log2), block_row = width_gobs * block_size;
    uint8_t *gobs = to_linear ? (uint8_t *)src : dst, *lin = to_linear ? dst : (uint8_t *)src;
    for (uint32_t z = 0; z < d; z++) {
        uint64_t zoff = (uint64_t)(z >> bd_log2) * blocks_rows * block_row + ((uint64_t)(z & bd_mask) << bh_log2) * 512;
        uint32_t y = 0;
        for (; y + 8 <= hb; y += 8) {  // whole GOB rows
            uint8_t *g = gobs + zoff + (y >> 3 >> bh_log2) * block_row + (uint64_t)((y >> 3) & bh_mask) * 512;
            uint8_t *l = lin + ((uint64_t)z * hb + y) * row_bytes;
            for (uint32_t gx = 0; gx < full; gx++, g += block_size, l += 64)
                for (uint32_t yi = 0; yi < 8; yi++) gob_row(g + row_in_gob(yi), l + yi * row_bytes, to_linear);
            if (tail)
                for (uint32_t yi = 0; yi < 8; yi++) gob_row_tail(g + row_in_gob(yi), l + yi * row_bytes, tail, to_linear);
        }
        for (; y < hb; y++) {  // the rows of a last, partial one
            uint8_t *g = gobs + zoff + (y >> 3 >> bh_log2) * block_row + (uint64_t)((y >> 3) & bh_mask) * 512 + row_in_gob(y & 7);
            uint8_t *l = lin + ((uint64_t)z * hb + y) * row_bytes;
            for (uint32_t gx = 0; gx < full; gx++, g += block_size, l += 64) gob_row(g, l, to_linear);
            if (tail) gob_row_tail(g, l, tail, to_linear);
        }
    }
}

void tegra_deswizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                     uint32_t bh_log2, uint32_t bd_log2) {
    copy_surface(src, dst, wb, hb, d, bpb, bh_log2, bd_log2, true);
}

void tegra_swizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                   uint32_t bh_log2, uint32_t bd_log2) {
    copy_surface(src, dst, wb, hb, d, bpb, bh_log2, bd_log2, false);
}
