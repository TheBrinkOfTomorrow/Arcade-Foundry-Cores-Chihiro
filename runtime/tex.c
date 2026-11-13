// NV2A textures to RGBA8 (R in the lowest byte).
//
// Swizzled textures store texels in Morton order: the bits of x, y (and z)
// interleaved from the lowest, x first, until the smaller dimension runs
// out. Linear ("image") textures are rows at a pitch. DXT1/3/5 are 4x4
// blocks in rows. Only the top mip level is decoded; the renderer builds
// the rest.
#include "pgraph.h"
#include <string.h>
#include <stdlib.h>

static uint32_t bpp_of(uint32_t c)
{
    switch (c) {
    case 0x00: case 0x01: case 0x0b: case 0x13: case 0x19: case 0x1b: case 0x1f:
        return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11: case 0x17: case 0x1a: case 0x1c:
    case 0x1d: case 0x20: case 0x24: case 0x25: case 0x27: case 0x28: case 0x29: case 0x2c: case 0x30:
    case 0x31: case 0x32: case 0x35:
        return 2;
    }
    return 4;
}

static uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; }
static uint32_t x5(uint32_t v) { return v << 3 | v >> 2; }
static uint32_t x6(uint32_t v) { return v << 2 | v >> 4; }
static uint32_t x4(uint32_t v) { return v << 4 | v; }

static uint32_t texel(uint32_t c, const uint8_t *p, const uint32_t *pal)
{
    uint32_t v;
    switch (bpp_of(c)) {
    case 1: v = p[0]; break;
    case 2: v = p[0] | p[1] << 8; break;
    default: memcpy(&v, p, 4); break;
    }
    switch (c) {
    case 0x00: case 0x13: return rgba(v, v, v, 255);            // Y8
    case 0x01: case 0x1b: return rgba(v, v, v, v);              // AY8
    case 0x19: case 0x1f: return rgba(0, 0, 0, v);              // A8
    case 0x0b: return pal ? pal[v] : rgba(v, v, v, 255);        // I8 through the palette
    case 0x02: case 0x10:                                       // A1R5G5B5
        return rgba(x5(v >> 10 & 31), x5(v >> 5 & 31), x5(v & 31), (v >> 15) ? 255 : 0);
    case 0x03: case 0x1c:                                       // X1R5G5B5
        return rgba(x5(v >> 10 & 31), x5(v >> 5 & 31), x5(v & 31), 255);
    case 0x04: case 0x1d:                                       // A4R4G4B4
        return rgba(x4(v >> 8 & 15), x4(v >> 4 & 15), x4(v & 15), x4(v >> 12));
    case 0x05: case 0x11:                                       // R5G6B5
        return rgba(x5(v >> 11), x6(v >> 5 & 63), x5(v & 31), 255);
    case 0x27:                                                  // R6G5B5
        return rgba(x6(v >> 10), x5(v >> 5 & 31), x5(v & 31), 255);
    case 0x17: case 0x28:                                       // G8B8
        return rgba(v >> 8, v & 0xff, 0, 255);
    case 0x29:                                                  // R8B8
        return rgba(v >> 8, 0, v & 0xff, 255);
    case 0x1a: case 0x20:                                       // A8Y8
        return rgba(v & 0xff, v & 0xff, v & 0xff, v >> 8);
    case 0x2c: case 0x30: case 0x31: case 0x32: case 0x35:      // 16-bit depth or luminance
        return rgba(v >> 8, v >> 8, v >> 8, 255);
    case 0x06: case 0x12:                                       // A8R8G8B8
        return rgba(v >> 16 & 0xff, v >> 8 & 0xff, v & 0xff, v >> 24);
    case 0x07: case 0x1e:                                       // X8R8G8B8
        return rgba(v >> 16 & 0xff, v >> 8 & 0xff, v & 0xff, 255);
    case 0x3a: case 0x3f:                                       // A8B8G8R8
        return v;
    case 0x3b: case 0x40:                                       // B8G8R8A8
        return rgba(v >> 8 & 0xff, v >> 16 & 0xff, v >> 24, v & 0xff);
    case 0x3c: case 0x41:                                       // R8G8B8A8
        return rgba(v >> 24, v >> 16 & 0xff, v >> 8 & 0xff, v & 0xff);
    case 0x2e: case 0x2f:                                       // 24-bit depth
        return rgba(v >> 24, v >> 16 & 0xff, v >> 8 & 0xff, 255);
    }
    return v;
}

// Morton masks for a w x h x d texture
static void morton_masks(uint32_t w, uint32_t h, uint32_t d, uint32_t *mx, uint32_t *my, uint32_t *mz)
{
    uint32_t bit = 1, k = 0;
    *mx = *my = *mz = 0;
    while (bit < w || bit < h || bit < d) {
        if (bit < w) *mx |= 1u << k++;
        if (bit < h) *my |= 1u << k++;
        if (bit < d) *mz |= 1u << k++;
        bit <<= 1;
    }
}

static uint32_t deposit(uint32_t v, uint32_t mask)
{
    uint32_t r = 0;
    for (uint32_t b = 1; mask; b <<= 1) {
        uint32_t low = mask & -mask;
        if (v & b) r |= low;
        mask &= mask - 1;
    }
    return r;
}

static void dxt_block(uint32_t c, const uint8_t *p, uint32_t out[16])
{
    const uint8_t *col = c == 0x0c ? p : p + 8;
    uint32_t c0 = col[0] | col[1] << 8, c1 = col[2] | col[3] << 8;
    uint32_t r[4], g[4], b[4], a[4] = { 255, 255, 255, 255 };
    r[0] = x5(c0 >> 11), g[0] = x6(c0 >> 5 & 63), b[0] = x5(c0 & 31);
    r[1] = x5(c1 >> 11), g[1] = x6(c1 >> 5 & 63), b[1] = x5(c1 & 31);
    if (c0 > c1 || c != 0x0c) {
        r[2] = (2 * r[0] + r[1]) / 3, g[2] = (2 * g[0] + g[1]) / 3, b[2] = (2 * b[0] + b[1]) / 3;
        r[3] = (r[0] + 2 * r[1]) / 3, g[3] = (g[0] + 2 * g[1]) / 3, b[3] = (b[0] + 2 * b[1]) / 3;
    } else {
        r[2] = (r[0] + r[1]) / 2, g[2] = (g[0] + g[1]) / 2, b[2] = (b[0] + b[1]) / 2;
        r[3] = g[3] = b[3] = 0, a[3] = 0;
    }
    uint32_t idx = col[4] | col[5] << 8 | col[6] << 16 | (uint32_t)col[7] << 24;
    uint32_t alpha[16];
    if (c == 0x0e) {   // DXT3: four bits of alpha each
        for (int i = 0; i < 16; i++) alpha[i] = x4(p[i / 2] >> (4 * (i & 1)) & 15);
    } else if (c == 0x0f) {   // DXT5: two endpoints and three-bit indices
        uint32_t a0 = p[0], a1 = p[1], av[8];
        av[0] = a0, av[1] = a1;
        if (a0 > a1)
            for (int i = 1; i < 7; i++) av[i + 1] = ((7 - i) * a0 + i * a1) / 7;
        else {
            for (int i = 1; i < 5; i++) av[i + 1] = ((5 - i) * a0 + i * a1) / 5;
            av[6] = 0, av[7] = 255;
        }
        uint64_t bits = 0;
        for (int i = 0; i < 6; i++) bits |= (uint64_t)p[2 + i] << (8 * i);
        for (int i = 0; i < 16; i++) alpha[i] = av[bits >> (3 * i) & 7];
    }
    for (int i = 0; i < 16; i++) {
        uint32_t k = idx >> (2 * i) & 3;
        out[i] = rgba(r[k], g[k], b[k], c == 0x0c ? a[k] : alpha[i]);
    }
}

static uint32_t level_bytes(const pg_texture *t, uint32_t w, uint32_t h, uint32_t d)
{
    if (t->compressed) return ((w + 3) / 4) * ((h + 3) / 4) * (t->color == 0x0c ? 8 : 16) * d;
    return w * h * d * bpp_of(t->color);
}

uint32_t tex_bytes(const pg_texture *t)
{
    if (t->linear) return t->pitch * t->height;
    uint32_t total = 0, w = t->width, h = t->height, d = t->depth;
    for (uint32_t l = 0; l < t->levels; l++) {
        total += level_bytes(t, w, h, d);
        if (w > 1) w >>= 1;
        if (h > 1) h >>= 1;
        if (d > 1) d >>= 1;
    }
    if (t->cube) total = ((total + 127) & ~127u) * 6;
    return total;
}

void tex_decode(const pg_texture *t, const uint8_t *ram, uint32_t *out)
{
    uint32_t w = t->width, h = t->height, d = t->depth, bpp = bpp_of(t->color);
    uint32_t pal[256], *palp = NULL;
    if (t->color == 0x0b && t->palette) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = 0;
            if (i < t->palette_len) memcpy(&v, ram + ((t->palette + 4 * i) & 0x7ffffff), 4);
            pal[i] = rgba(v >> 16 & 0xff, v >> 8 & 0xff, v & 0xff, v >> 24);
        }
        palp = pal;
    }
    uint32_t faces = t->cube ? 6 : 1, face_stride = 0;
    if (t->cube) {
        pg_texture one = *t;
        one.cube = 0;
        face_stride = (tex_bytes(&one) + 127) & ~127u;
    }
    for (uint32_t f = 0; f < faces; f++) {
        const uint8_t *src = ram + ((t->addr + f * face_stride) & 0x7ffffff);
        uint32_t *dst = out + f * w * h * d;
        if (t->compressed) {
            uint32_t bs = t->color == 0x0c ? 8 : 16, blk[16];
            for (uint32_t by = 0; by < (h + 3) / 4; by++)
                for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
                    dxt_block(t->color, src + (by * ((w + 3) / 4) + bx) * bs, blk);
                    for (uint32_t i = 0; i < 16; i++) {
                        uint32_t x = bx * 4 + (i & 3), y = by * 4 + i / 4;
                        if (x < w && y < h) dst[y * w + x] = blk[i];
                    }
                }
        } else if (t->linear) {
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) dst[y * w + x] = texel(t->color, src + y * t->pitch + x * bpp, palp);
        } else {
            uint32_t mx, my, mz;
            morton_masks(w, h, d, &mx, &my, &mz);
            // swizzled offsets of every column, built once (deposit is per bit)
            static uint32_t *ox; static uint32_t ox_n;
            if (w > ox_n) { free(ox); ox = malloc(w * sizeof *ox); ox_n = w; }
            for (uint32_t x = 0; x < w; x++) ox[x] = deposit(x, mx) * bpp;
            for (uint32_t z = 0; z < d; z++) {
                uint32_t oz = deposit(z, mz);
                for (uint32_t y = 0; y < h; y++) {
                    const uint8_t *row = src + (deposit(y, my) | oz) * bpp;
                    uint32_t *o = dst + (z * h + y) * w;
                    for (uint32_t x = 0; x < w; x++) o[x] = texel(t->color, row + ox[x], palp);
                }
            }
        }
    }
}
