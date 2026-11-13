// NV097, the 3D class: methods to state, vertices to draws. See pgraph.h.
// Runs on the GPU thread (nv2a.c), which owns the renderer.
#include "xbox.h"
#include "pgraph.h"
#include <math.h>
#include <string.h>

uint32_t nv2a_dma_base(uint32_t handle);   // nv2a.c: a DMA object's physical base

static pg_state S;
static int prim;                  // between SET_BEGIN_END(prim) and SET_BEGIN_END(END)
static pg_vertex *vtx;
static uint32_t nvtx, vtx_cap;
static uint32_t *inl;             // INLINE_ARRAY words
static uint32_t ninl, inl_cap;
static uint32_t prog_load, const_load;

static pg_vertex *new_vertex(void)
{
    if (nvtx == vtx_cap) {
        vtx_cap = vtx_cap ? vtx_cap * 2 : 1024;
        vtx = realloc(vtx, vtx_cap * sizeof *vtx);
    }
    return &vtx[nvtx++];
}

// ---- vertex attributes --------------------------------------------------------------------------
enum { T_UB_D3D = 0, T_S1 = 1, T_F = 2, T_UB_OGL = 4, T_S32K = 5, T_CMP = 6 };

static inline uint32_t fmt_of(int i) { return S.m[(M_VERTEX_DATA_ARRAY_FORMAT + 4 * i) / 4]; }

// bytes one attribute takes in memory, and (rounded to words) in an inline array
static uint32_t attr_bytes(uint32_t fmt)
{
    uint32_t size = fmt >> 4 & 0xf;
    switch (fmt & 0xf) {
    case T_UB_D3D: case T_UB_OGL: return size;
    case T_S1: case T_S32K: return size * 2;
    case T_F: return size * 4;
    case T_CMP: return 4;
    }
    return 0;
}

static void decode_attr(uint32_t fmt, const uint8_t *p, float *out)
{
    uint32_t size = fmt >> 4 & 0xf, i;
    out[0] = out[1] = out[2] = 0;
    out[3] = 1;
    switch (fmt & 0xf) {
    case T_UB_D3D:
        for (i = 0; i < size && i < 4; i++) out[i] = p[i] / 255.0f;
        if (size == 4) {   // D3DCOLOR: B, G, R, A in memory
            float t = out[0];
            out[0] = out[2];
            out[2] = t;
        }
        break;
    case T_UB_OGL:
        for (i = 0; i < size && i < 4; i++) out[i] = p[i] / 255.0f;
        break;
    case T_S1:
        for (i = 0; i < size && i < 4; i++) {
            int16_t s;
            memcpy(&s, p + 2 * i, 2);
            out[i] = (2.0f * s + 1.0f) / 65535.0f;
        }
        break;
    case T_S32K:
        for (i = 0; i < size && i < 4; i++) {
            int16_t s;
            memcpy(&s, p + 2 * i, 2);
            out[i] = s;
        }
        break;
    case T_F:
        for (i = 0; i < size && i < 4; i++) memcpy(&out[i], p + 4 * i, 4);
        break;
    case T_CMP: {   // 11:11:10 signed normalised
        uint32_t w;
        memcpy(&w, p, 4);
        out[0] = (int32_t)(w << 21) >> 21;
        out[1] = (int32_t)(w << 10) >> 21;
        out[2] = (int32_t)w >> 22;
        out[0] /= 1023.0f;
        out[1] /= 1023.0f;
        out[2] /= 511.0f;
        break;
    }
    }
}

// a vertex from the arrays in memory
static void fetch_array_vertex(uint32_t index)
{
    pg_vertex *v = new_vertex();
    memcpy(v->a, S.attr_const, sizeof v->a);
    for (int i = 0; i < PG_ATTRS; i++) {
        uint32_t fmt = fmt_of(i);
        if (!(fmt >> 4 & 0xf)) continue;
        uint32_t off = S.m[(M_VERTEX_DATA_ARRAY_OFFSET + 4 * i) / 4];
        uint32_t base = (off & 0x80000000u) ? S.dma_vertex_b : S.dma_vertex_a;
        uint32_t a = phys_of(base + (off & 0x7fffffffu) + index * (fmt >> 8));
        decode_attr(fmt, (const uint8_t *)M + a, v->a[i]);
    }
}

// the vertices of the inline array: each attribute rounded up to whole words
static void flush_inline(void)
{
    uint32_t words = 0;
    for (int i = 0; i < PG_ATTRS; i++) words += (attr_bytes(fmt_of(i)) + 3) / 4;
    if (!words) {
        ninl = 0;
        return;
    }
    for (uint32_t at = 0; at + words <= ninl; at += words) {
        pg_vertex *v = new_vertex();
        memcpy(v->a, S.attr_const, sizeof v->a);
        uint32_t w = at;
        for (int i = 0; i < PG_ATTRS; i++) {
            uint32_t fmt = fmt_of(i), n = (attr_bytes(fmt) + 3) / 4;
            if (!n) continue;
            decode_attr(fmt, (const uint8_t *)&inl[w], v->a[i]);
            w += n;
        }
    }
    ninl = 0;
}

static int imm_draw;   // this draw has immediate vertices: any attribute may differ between them

static void emit_immediate_vertex(void)
{
    if (prim == PRIM_END) return;
    imm_draw = 1;
    pg_vertex *v = new_vertex();
    memcpy(v->a, S.attr_const, sizeof v->a);
}

static void set_attr(int slot, int comp, float f, int ncomp)
{
    S.attr_const[slot][comp] = f;
    if (slot == 0 && comp == ncomp - 1) emit_immediate_vertex();
}

static void set_attr4(int slot, float x, float y, float z, float w)
{
    float *a = S.attr_const[slot];
    a[0] = x, a[1] = y, a[2] = z, a[3] = w;
    if (slot == 0) emit_immediate_vertex();
}

static void begin_end(uint32_t arg)
{
    if (arg != PRIM_END) {
        prim = arg;
        nvtx = ninl = 0;
        imm_draw = 0;
        return;
    }
    if (prim == PRIM_END) return;
    if (ninl) flush_inline();
    if (nvtx) {
        // the attributes from arrays differ between vertices; the others are
        // attr_const, the same in every vertex (unless set between vertices)
        uint32_t vary = 0;
        for (int i = 0; i < PG_ATTRS; i++)
            if (fmt_of(i) >> 4 & 0xf) vary |= 1u << i;
        S.draw_vary = imm_draw || !vary ? (1u << PG_ATTRS) - 1 : vary;
        gpu_draw(&S, prim, vtx, nvtx);
    }
    prim = PRIM_END;
    nvtx = 0;
}

// ---- textures ---------------------------------------------------------------------------------
static int fmt_linear(uint32_t c)
{
    return (c >= 0x10 && c <= 0x13) || c == 0x17 || (c >= 0x1b && c <= 0x20) || c == 0x24 || c == 0x25 ||
           (c >= 0x2e && c <= 0x31) || c == 0x35 || (c >= 0x3f && c <= 0x41);
}

int pg_texture_of(const pg_state *s, int stage, pg_texture *t)
{
    uint32_t b = 0x40 * stage;
    uint32_t ctl0 = pg_m(s, M_TEXTURE_CONTROL0 + b);
    if (!(ctl0 & 0x40000000u)) return 0;
    uint32_t fmt = pg_m(s, M_TEXTURE_FORMAT + b), off = pg_m(s, M_TEXTURE_OFFSET + b);
    memset(t, 0, sizeof *t);
    t->format = fmt;
    t->color = fmt >> 8 & 0xff;
    t->addr = phys_of(((fmt & 3) == 2 ? s->dma_b : s->dma_a) + off);
    t->cube = fmt >> 2 & 1;
    t->levels = fmt >> 16 & 0xf;
    if (!t->levels) t->levels = 1;
    t->compressed = t->color == 0x0c || t->color == 0x0e || t->color == 0x0f;
    t->linear = fmt_linear(t->color);
    t->swizzled = !t->linear && !t->compressed;
    uint32_t dim = fmt >> 4 & 0xf;
    if (t->linear) {
        uint32_t rect = pg_m(s, M_TEXTURE_IMAGE_RECT + b);
        t->width = rect >> 16;
        t->height = rect & 0xffff;
        t->depth = 1;
        t->pitch = pg_m(s, M_TEXTURE_CONTROL1 + b) >> 16;
        t->levels = 1;
    } else {
        t->width = 1u << (fmt >> 20 & 0xf);
        t->height = dim >= 2 ? 1u << (fmt >> 24 & 0xf) : 1;
        t->depth = dim == 3 ? 1u << (fmt >> 28 & 0xf) : 1;
    }
    if (t->color == 0x0b) {   // I8 with a palette
        uint32_t pal = pg_m(s, M_TEXTURE_PALETTE + b);
        t->palette = phys_of(((pal & 3) == 2 ? s->dma_b : s->dma_a) + (pal & ~0x3fu));
        t->palette_len = 256 >> (pal >> 2 & 3);
    }
    return t->width && t->height;
}

// ---- methods ------------------------------------------------------------------------------------
static float f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

// the fixed-function methods that write the transform constants (the matrices,
// texgen planes, eye position, fog plane, viewport): their constant row, or -1
static int xf_row(uint32_t method)
{
    if (method >= 0x0440 && method < 0x0480) return 0x04 + (method - 0x0440) / 16;   // projection
    if (method >= 0x0480 && method < 0x0580) return 0x08 + (method - 0x0480) / 64 * 8 + (method - 0x0480) / 16 % 4;
    if (method >= 0x0580 && method < 0x0680) return 0x0c + (method - 0x0580) / 64 * 8 + (method - 0x0580) / 16 % 4;
    if (method >= 0x0680 && method < 0x06c0) return 0x00 + (method - 0x0680) / 16;   // composite
    if (method >= 0x06c0 && method < 0x07c0) return 0x44 + (method - 0x06c0) / 64 * 8 + (method - 0x06c0) / 16 % 4;
    if (method >= 0x0840 && method < 0x0940) return 0x40 + (method - 0x0840) / 64 * 8 + (method - 0x0840) / 16 % 4;
    if (method >= 0x09d0 && method < 0x09e0) return 0x39;   // fog plane
    if (method >= 0x0a20 && method < 0x0a30) return 0x3b;   // viewport offset
    if (method >= 0x0a50 && method < 0x0a60) return 0x38;   // eye position
    if (method >= 0x0af0 && method < 0x0b00) return 0x3a;   // viewport scale
    return -1;
}

void pg_method(uint32_t method, uint32_t arg)
{
    S.m[(method & 0x1ffc) / 4] = arg;
    if (method >= 0x0440 && method < 0x0b00) {
        int row = xf_row(method);
        if (row >= 0) {
            S.constants[row][method / 4 % 4] = f32(arg);
            return;
        }
    }

    // vertex program and constants: a load pointer, then words written in turn
    if (method >= 0x0b00 && method < 0x0b80) {
        if (prog_load < PG_PROGRAM_SLOTS * 4) S.program[prog_load / 4][prog_load % 4] = arg;
        prog_load++;
        return;
    }
    if (method >= 0x0b80 && method < 0x0c00) {
        if (const_load < PG_CONSTANTS * 4) S.constants[const_load / 4][const_load % 4] = f32(arg);
        const_load++;
        return;
    }
    if (method == 0x1818) {   // INLINE_ARRAY
        if (ninl == inl_cap) {
            inl_cap = inl_cap ? inl_cap * 2 : 4096;
            inl = realloc(inl, inl_cap * 4);
        }
        inl[ninl++] = arg;
        return;
    }
    // immediate vertex data
    if (method >= 0x1880 && method < 0x1900) {   // SET_VERTEX_DATA2F_M
        int slot = (method - 0x1880) / 8, comp = (method - 0x1880) / 4 & 1;
        if (comp == 0) S.attr_const[slot][2] = 0, S.attr_const[slot][3] = 1;
        set_attr(slot, comp, f32(arg), 2);
        return;
    }
    if (method >= 0x1a00 && method < 0x1b00) {   // SET_VERTEX_DATA4F_M
        int slot = (method - 0x1a00) / 16, comp = (method - 0x1a00) / 4 & 3;
        set_attr(slot, comp, f32(arg), 4);
        return;
    }
    if (method >= 0x1900 && method < 0x1940) {   // SET_VERTEX_DATA2S
        set_attr4((method - 0x1900) / 4, (int16_t)arg, (int16_t)(arg >> 16), 0, 1);
        return;
    }
    if (method >= 0x1940 && method < 0x1980) {   // SET_VERTEX_DATA4UB
        set_attr4((method - 0x1940) / 4, (arg & 0xff) / 255.0f, (arg >> 8 & 0xff) / 255.0f,
                  (arg >> 16 & 0xff) / 255.0f, (arg >> 24) / 255.0f);
        return;
    }
    if (method >= 0x1980 && method < 0x1a00) {   // SET_VERTEX_DATA4S_M
        int slot = (method - 0x1980) / 8, half = (method - 0x1980) / 4 & 1;
        S.attr_const[slot][2 * half] = (int16_t)arg;
        S.attr_const[slot][2 * half + 1] = (int16_t)(arg >> 16);
        if (half && slot == 0) emit_immediate_vertex();
        return;
    }
    if (method >= 0x1500 && method < 0x1518) {   // SET_VERTEX3F
        int comp = (method - 0x1500) / 4;
        if (comp == 0) S.attr_const[0][3] = 1;
        set_attr(0, comp, f32(arg), 3);
        return;
    }
    if (method >= 0x1518 && method < 0x1528) {   // SET_VERTEX4F
        set_attr(0, (method - 0x1518) / 4, f32(arg), 4);
        return;
    }
    if (method >= 0x1530 && method < 0x153c) {   // SET_NORMAL3F
        S.attr_const[2][(method - 0x1530) / 4] = f32(arg);
        return;
    }
    if (method >= 0x1550 && method < 0x1560) {   // SET_DIFFUSE_COLOR4F
        S.attr_const[3][(method - 0x1550) / 4] = f32(arg);
        return;
    }
    if (method >= 0x1560 && method < 0x156c) {   // SET_DIFFUSE_COLOR3F
        S.attr_const[3][(method - 0x1560) / 4] = f32(arg);
        S.attr_const[3][3] = 1;
        return;
    }
    if (method == 0x156c || method == 0x158c) {   // SET_DIFFUSE/SPECULAR_COLOR4UB
        float *a = S.attr_const[method == 0x156c ? 3 : 4];
        a[0] = (arg & 0xff) / 255.0f, a[1] = (arg >> 8 & 0xff) / 255.0f;
        a[2] = (arg >> 16 & 0xff) / 255.0f, a[3] = (arg >> 24) / 255.0f;
        return;
    }
    if (method >= 0x1570 && method < 0x1580) {   // SET_SPECULAR_COLOR4F
        S.attr_const[4][(method - 0x1570) / 4] = f32(arg);
        return;
    }
    if (method >= 0x1580 && method < 0x158c) {   // SET_SPECULAR_COLOR3F
        S.attr_const[4][(method - 0x1580) / 4] = f32(arg);
        S.attr_const[4][3] = 1;
        return;
    }
    if (method >= 0x1590 && method < 0x1638) {   // SET_TEXCOORDn_2F/2S/4F/4S
        static const uint16_t base[4] = { 0x1590, 0x15b8, 0x15e0, 0x1608 };
        for (int t = 3; t >= 0; t--) {
            if (method < base[t]) continue;
            uint32_t o = method - base[t];
            float *a = S.attr_const[9 + t];
            if (o < 8) {   // 2F
                a[o / 4] = f32(arg);
                if (o == 0) a[2] = 0, a[3] = 1;
            } else if (o < 0x10) {   // 2S
                a[0] = (int16_t)arg, a[1] = (int16_t)(arg >> 16), a[2] = 0, a[3] = 1;
            } else if (o < 0x20) {   // 4F
                a[(o - 0x10) / 4] = f32(arg);
            } else {   // 4S
                int h = (o - 0x20) / 4 & 1;
                a[2 * h] = (int16_t)arg, a[2 * h + 1] = (int16_t)(arg >> 16);
            }
            break;
        }
        return;
    }
    if (method == 0x1698) {   // SET_FOG_COORD
        S.attr_const[5][0] = f32(arg);
        return;
    }
    if (method >= 0x169c && method < 0x16d0) {   // SET_WEIGHT1F..4F
        static const uint16_t wb[4] = { 0x169c, 0x16a0, 0x16b0, 0x16c0 };
        for (int k = 3; k >= 0; k--)
            if (method >= wb[k]) {
                S.attr_const[1][(method - wb[k]) / 4] = f32(arg);
                break;
            }
        return;
    }

    switch (method) {
    case 0x0184: S.dma_a = nv2a_dma_base(arg); return;
    case 0x0188: S.dma_b = nv2a_dma_base(arg); return;
    case 0x0194: S.dma_color = nv2a_dma_base(arg); return;
    case 0x0198: S.dma_zeta = nv2a_dma_base(arg); return;
    case 0x019c: S.dma_vertex_a = nv2a_dma_base(arg); return;
    case 0x01a0: S.dma_vertex_b = nv2a_dma_base(arg); return;
    case 0x012c:   // FLIP_INCREMENT_WRITE: this surface is a finished frame
        gpu_flip(&S);
        return;
    case 0x1d94:   // CLEAR_SURFACE
        gpu_clear(&S, arg);
        return;
    case 0x17fc:   // SET_BEGIN_END
        begin_end(arg);
        return;
    case 0x1800:   // ARRAY_ELEMENT16: two indices a word
        fetch_array_vertex(arg & 0xffff);
        fetch_array_vertex(arg >> 16);
        return;
    case 0x1808:   // ARRAY_ELEMENT32
        fetch_array_vertex(arg);
        return;
    case 0x1810: { // DRAW_ARRAYS: start, count - 1
        uint32_t start = arg & 0xffffff, count = (arg >> 24) + 1;
        for (uint32_t i = 0; i < count; i++) fetch_array_vertex(start + i);
        return;
    }
    case 0x1e9c:   // SET_TRANSFORM_PROGRAM_LOAD
        prog_load = arg * 4;
        return;
    case 0x1ea4:   // SET_TRANSFORM_CONSTANT_LOAD
        const_load = arg * 4;
        return;
    }
}

// back to power-on state (the renderer is gone with the GPU thread)
void pg_reset(void)
{
    memset(&S, 0, sizeof S);
    prim = 0;
    nvtx = ninl = 0;
    prog_load = const_load = 0;
}

void pg_init(void)
{
    for (int i = 0; i < PG_ATTRS; i++) S.attr_const[i][3] = 1;
    for (int i = 0; i < 4; i++) S.attr_const[3][i] = 1;
    S.m[M_COLOR_MASK / 4] = 0x01010101;
    S.m[M_DEPTH_MASK / 4] = 1;
    S.m[M_CLIP_MAX / 4] = f2u(16777215.0f);
    // the specular and spot falloff curves before anything sets them: (x + L) / (M x + N)
    // with L = 0, M = -0.494592, N = 1.494592
    static const float k[3] = { 0.0f, -0.494592f, 1.494592f };
    for (int i = 0; i < 3; i++) {
        S.m[(0x09e0 + 4 * i) / 4] = f2u(k[i]);
        for (int l = 0; l < 8; l++) S.m[(0x1040 + 0x80 * l + 4 * i) / 4] = f2u(k[i]);
    }
}
