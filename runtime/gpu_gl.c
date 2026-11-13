// The renderer on OpenGL 3.3 (Linux; Apple gets Metal). Runs on the GPU
// thread, which owns the context.
//
// Surfaces: every colour and depth surface the game draws to is a texture
// here, found by its address; a texture whose address is a surface's is
// that surface. Surface row 0 (the top, lowest address) is texture row 0:
// the vertex epilogue maps screen y = 0 to NDC -1, so nothing is flipped
// until the frame is shown.
//
// Textures in RAM are decoded to RGBA8 and kept, checked against a hash of
// their memory once a frame.
//
// Built with CH_CORE (the core library) there is no window: the context is
// EGL's without a surface (Mesa), and each frame shown is read back and
// handed to gpu_present.
#define GL_GLEXT_PROTOTYPES 1
#include "af_debug.h"
#include "perf.h"
#include "xbox.h"
#include "pgraph.h"
#include <GL/glcorearb.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef CH_CORE
#include <EGL/egl.h>
#include <EGL/eglext.h>
static EGLDisplay dpy = EGL_NO_DISPLAY;
static EGLContext ctx = EGL_NO_CONTEXT;
static uint32_t *present_px;
static size_t present_cap;
#else
#include <SDL2/SDL.h>
static SDL_Window *win;
static SDL_GLContext ctx;
#endif
#ifndef CH_CORE
static int win_w = 640, win_h = 480;
#endif
static GLuint fbo, vao, vbo, ibo, blit_fbo;
static uint32_t frame_no;
static const char *shot_dir;
static uint32_t shot_every;

// ---- surfaces ---------------------------------------------------------------------------------------
typedef struct surface {
    uint32_t addr, w, h, zeta, fmt;
    GLuint tex;
    uint32_t used;
    uint64_t ram_hash;            // of its memory as last seen, to catch the CPU writing it
    uint32_t ram_len, ram_checked;
} surface;
#define MAX_SURF 64
static surface surfs[MAX_SURF];

static surface *surface_find_addr(uint32_t addr, int zeta)
{
    surface *best = NULL;
    for (int i = 0; i < MAX_SURF; i++)
        if (surfs[i].tex && surfs[i].addr == addr && surfs[i].zeta == zeta && (!best || surfs[i].used > best->used))
            best = &surfs[i];
    return best;
}

static surface *surface_get(uint32_t addr, uint32_t w, uint32_t h, int zeta, uint32_t fmt)
{
    surface *free_s = NULL, *old = NULL;
    for (int i = 0; i < MAX_SURF; i++) {
        surface *s = &surfs[i];
        if (!s->tex) {
            if (!free_s) free_s = s;
            continue;
        }
        if (s->addr == addr && s->zeta == zeta) {
            if (s->w >= w && s->h >= h) {   // the same, or bigger: a smaller target over the same memory
                s->used = frame_no;
                return s;
            }
            // the same memory at another size: replace it
            glDeleteTextures(1, &s->tex);
            s->tex = 0;
            if (!free_s) free_s = s;
            continue;
        }
        if (!old || s->used < old->used) old = s;
    }
    if (!free_s) {
        glDeleteTextures(1, &old->tex);
        old->tex = 0;
        free_s = old;
    }
    surface *s = free_s;
    s->addr = addr, s->w = w, s->h = h, s->zeta = zeta, s->fmt = fmt, s->used = frame_no;
    s->ram_len = 0;
    glGenTextures(1, &s->tex);
    glBindTexture(GL_TEXTURE_2D, s->tex);
    if (zeta)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (hle_verbose > 0) hle_log("gpu: %s surface 0x%08x %ux%u format %u\n", zeta ? "depth" : "colour", addr, w, h, fmt);
    return s;
}

static surface *cur_color, *cur_zeta;

// bind the state's surfaces; their size from the clip rectangle
static int bind_target(const pg_state *st)
{
    uint32_t ch = pg_m(st, M_SURFACE_CLIP_H), cv = pg_m(st, M_SURFACE_CLIP_V), fmt = pg_m(st, M_SURFACE_FORMAT);
    uint32_t w = (ch & 0xffff) + (ch >> 16), h = (cv & 0xffff) + (cv >> 16);
    if ((fmt >> 8 & 0xf) == 2) {   // swizzled: the size is in the format
        w = 1u << (fmt >> 16 & 0xff);
        h = 1u << (fmt >> 24 & 0xff);
    }
    if (!w || !h || w > 4096 || h > 4096) return 0;
    uint32_t aa = fmt >> 12 & 0xf;
    if (aa == 1) w *= 2;
    else if (aa == 2) w *= 2, h *= 2;
    uint32_t cfmt = fmt & 0xf, zfmt = fmt >> 4 & 0xf;
    uint32_t pitch = pg_m(st, M_SURFACE_PITCH);
    cur_color = cfmt && (pitch & 0xffff) ? surface_get(phys_of(st->dma_color + pg_m(st, M_SURFACE_COLOR_OFFSET)), w, h, 0, cfmt) : NULL;
    cur_zeta = zfmt && (pitch >> 16) ? surface_get(phys_of(st->dma_zeta + pg_m(st, M_SURFACE_ZETA_OFFSET)), w, h, 1, zfmt) : NULL;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cur_color ? cur_color->tex : 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, cur_zeta ? cur_zeta->tex : 0, 0);
    glDrawBuffer(cur_color ? GL_COLOR_ATTACHMENT0 : GL_NONE);
    glViewport(0, 0, w, h);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

// ---- textures ----------------------------------------------------------------------------------------
typedef struct gtex {
    uint32_t addr, format, w, h, pitch, palette;
    uint64_t hash;
    uint32_t checked, used;
    GLuint tex;
    GLenum target;
} gtex;
#define MAX_TEX 1024
static gtex texs[MAX_TEX];
static uint32_t *decode_buf;
static size_t decode_cap;

static uint64_t hash_mem(const uint8_t *p, uint32_t n)
{
    ch_pc.tex_hash_bytes += n;
    uint64_t h = 0xcbf29ce484222325ull;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

static gtex *texture_get(const pg_texture *t)
{
    gtex *g = NULL, *old = NULL;
    for (int i = 0; i < MAX_TEX; i++) {
        gtex *x = &texs[i];
        if (!x->tex) {
            if (!g) g = x;
            continue;
        }
        if (x->addr == t->addr && x->format == t->format && x->w == t->width && x->h == t->height &&
            x->pitch == t->pitch && x->palette == t->palette) {
            g = x;
            break;
        }
        if (!old || x->used < old->used) old = x;
    }
    if (!g) {
        glDeleteTextures(1, &old->tex);
        memset(old, 0, sizeof *old);
        g = old;
    }
    uint32_t bytes = tex_bytes(t);
    if (t->addr + bytes > RAM_SIZE) bytes = RAM_SIZE - t->addr;
    int fresh = !g->tex;
    if (fresh || g->checked != frame_no) {
        uint64_t h = hash_mem((const uint8_t *)M + t->addr, bytes);
        if (t->palette) h ^= hash_mem((const uint8_t *)M + t->palette, 1024) * 31;
        g->checked = frame_no;
        if (fresh || h != g->hash) {
            g->hash = h;
            g->addr = t->addr, g->format = t->format, g->w = t->width, g->h = t->height;
            g->pitch = t->pitch, g->palette = t->palette;
            size_t n = (size_t)t->width * t->height * t->depth * (t->cube ? 6 : 1);
            if (n > decode_cap) {
                decode_cap = n;
                decode_buf = realloc(decode_buf, n * 4);
            }
            tex_decode(t, (const uint8_t *)M, decode_buf);
            ch_pc.tex_uploads++;
            if (fresh) glGenTextures(1, &g->tex);
            g->target = t->cube ? GL_TEXTURE_CUBE_MAP : t->depth > 1 ? GL_TEXTURE_3D : GL_TEXTURE_2D;
            glBindTexture(g->target, g->tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            if (t->cube) {
                for (int f = 0; f < 6; f++)
                    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, t->width, t->height, 0, GL_RGBA,
                                 GL_UNSIGNED_BYTE, decode_buf + (size_t)f * t->width * t->height);
            } else if (t->depth > 1) {
                glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, t->width, t->height, t->depth, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                             decode_buf);
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t->width, t->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, decode_buf);
            }
            if (t->levels > 1) glGenerateMipmap(g->target);
        }
    }
    g->used = frame_no;
    return g;
}

static GLenum wrap_of(uint32_t v)
{
    switch (v) {
    case 1: return GL_REPEAT;
    case 2: return GL_MIRRORED_REPEAT;
    case 4: return GL_CLAMP_TO_BORDER;
    }
    return GL_CLAMP_TO_EDGE;
}

// The GPU's drawing stays in the surface's texture and never reaches RAM, so
// RAM under a surface changes only when the CPU writes it: Sofdec decodes
// movie frames straight into a texture that D3D once cleared as a target.
// Checked once a frame when the surface is read as a texture; when its
// memory has changed, the memory replaces the surface.
static void surface_from_ram(surface *s, const pg_texture *t)
{
    if (s->ram_checked == frame_no) return;
    s->ram_checked = frame_no;
    uint32_t len = tex_bytes(t);
    if (t->addr + len > RAM_SIZE) len = RAM_SIZE - t->addr;
    uint64_t h = hash_mem((const uint8_t *)M + t->addr, len);
    if (s->ram_len != len) {   // first look at this memory: what the GPU drew stands
        s->ram_len = len, s->ram_hash = h;
        return;
    }
    if (h == s->ram_hash) return;
    s->ram_hash = h;
    size_t n = (size_t)t->width * t->height;
    if (n > decode_cap) {
        decode_cap = n;
        decode_buf = realloc(decode_buf, n * 4);
    }
    tex_decode(t, (const uint8_t *)M, decode_buf);
    uint32_t w = t->width < s->w ? t->width : s->w, h2 = t->height < s->h ? t->height : s->h;
    glPixelStorei(GL_UNPACK_ROW_LENGTH, t->width);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h2, GL_RGBA, GL_UNSIGNED_BYTE, decode_buf);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

// bind stage i's texture; its kind for the shader, and the scale for texel coordinates
static int bind_texture(const pg_state *st, int i, float scale[2])
{
    pg_texture t;
    scale[0] = scale[1] = 1;
    if (!pg_texture_of(st, i, &t)) return 0;
    glActiveTexture(GL_TEXTURE0 + i);
    surface *s = surface_find_addr(t.addr, 0);
    GLenum target;
    int kind;
    uint32_t tw, th;
    int levels = t.levels;
    if (s && !t.cube && t.depth == 1) {   // drawn by the GPU: use the surface
        if (cur_color == s) return 0;     // reading the surface being drawn: not allowed here
        glBindTexture(GL_TEXTURE_2D, s->tex);
        surface_from_ram(s, &t);
        target = GL_TEXTURE_2D;
        tw = s->w, th = s->h;
        kind = t.linear ? 2 : 1;
        levels = 1;
    } else {
        gtex *g = texture_get(&t);
        glBindTexture(g->target, g->tex);
        target = g->target;
        tw = t.width, th = t.height;
        kind = t.cube ? 3 : t.depth > 1 ? 4 : t.linear ? 2 : 1;
    }
    if (kind == 2) scale[0] = 1.0f / tw, scale[1] = 1.0f / th;
    uint32_t filt = pg_m(st, M_TEXTURE_FILTER + 0x40 * i), addr = pg_m(st, M_TEXTURE_ADDRESS + 0x40 * i);
    uint32_t minf = filt >> 16 & 0xff, magf = filt >> 24 & 0xf;
    static const GLenum mins[8] = { GL_NEAREST, GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST,
                                    GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR };
    GLenum mn = mins[minf & 7];
    if (levels <= 1) mn = (mn == GL_NEAREST || mn == GL_NEAREST_MIPMAP_NEAREST || mn == GL_NEAREST_MIPMAP_LINEAR) ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, mn);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, magf == 1 ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, wrap_of(addr & 0xf));
    glTexParameteri(target, GL_TEXTURE_WRAP_T, wrap_of(addr >> 8 & 0xf));
    if (target != GL_TEXTURE_2D) glTexParameteri(target, GL_TEXTURE_WRAP_R, wrap_of(addr >> 16 & 0xf));
    uint32_t bc = pg_m(st, M_TEXTURE_BORDER_COLOR + 0x40 * i);
    float border[4] = { (bc >> 16 & 0xff) / 255.0f, (bc >> 8 & 0xff) / 255.0f, (bc & 0xff) / 255.0f, (bc >> 24) / 255.0f };
    glTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, border);
    return kind;
}

// ---- shaders -------------------------------------------------------------------------------------------
typedef struct prog_key {
    uint64_t vsh_hash;
    uint32_t ff[VSH_FF_KEY];   // fixed-function mode: the state its code depends on
    psh_key psh;
    uint8_t fog_mode, flat, pad[2];
} prog_key;

typedef struct gprog {
    prog_key key;
    GLuint id;
    GLint u_c, u_ff, u_surface, u_zmax, u_fog, u_c0, u_c1, u_fogcolor, u_alpharef, u_texscale;
    struct gprog *next;
} gprog;
#define PROG_BUCKETS 1024
static gprog *progs[PROG_BUCKETS];
static char vsh_body[64 * 1024], psh_body[64 * 1024];

#define PG_STR_(x) #x
#define PG_STR(x) PG_STR_(x)
static const char vs_head[] =
    "#version 330 core\n"
    "layout(location = 0) in vec4 v0; layout(location = 1) in vec4 v1; layout(location = 2) in vec4 v2;\n"
    "layout(location = 3) in vec4 v3; layout(location = 4) in vec4 v4; layout(location = 5) in vec4 v5;\n"
    "layout(location = 6) in vec4 v6; layout(location = 7) in vec4 v7; layout(location = 8) in vec4 v8;\n"
    "layout(location = 9) in vec4 v9; layout(location = 10) in vec4 v10; layout(location = 11) in vec4 v11;\n"
    "layout(location = 12) in vec4 v12; layout(location = 13) in vec4 v13; layout(location = 14) in vec4 v14;\n"
    "layout(location = 15) in vec4 v15;\n"
    "uniform vec4 c[192];\n"
    "uniform vec2 surfaceSize;\n"
    "uniform float zmax;\n"
    "uniform vec4 fogParams;\n"
    "uniform vec4 ff[" PG_STR(VSH_FF_UNIFORMS) "];\n"
    "%s out vec4 vD0; %s out vec4 vD1;\n"
    "out vec4 vT0; out vec4 vT1; out vec4 vT2; out vec4 vT3; out float vFogF;\n"
    "float vsh_rcc(float x) {\n"
    "  float r = 1.0 / x;\n"
    "  return r >= 0.0 ? clamp(r, 5.42101e-20, 1.884467e+19) : clamp(r, -1.884467e+19, -5.42101e-20);\n"
    "}\n"
    "vec4 vsh_exp(float x) { float f = floor(x); return vec4(exp2(f), x - f, exp2(x), 1.0); }\n"
    "vec4 vsh_log(float x) {\n"
    "  x = abs(x);\n"
    "  if (x == 0.0) return vec4(-1.0 / 0.0, 1.0, -1.0 / 0.0, 1.0);\n"
    "  float e = floor(log2(x));\n"
    "  return vec4(e, x / exp2(e), log2(x), 1.0);\n"
    "}\n"
    "vec4 vsh_lit(vec4 s) {\n"
    "  float p = clamp(s.w, -128.0, 128.0);\n"
    "  return vec4(1.0, max(s.x, 0.0), s.x > 0.0 ? pow(max(s.y, 0.0), p) : 0.0, 1.0);\n"
    "}\n"
    "void main() {\n"
    "  vec4 R0 = vec4(0.0), R1 = vec4(0.0), R2 = vec4(0.0), R3 = vec4(0.0), R4 = vec4(0.0), R5 = vec4(0.0);\n"
    "  vec4 R6 = vec4(0.0), R7 = vec4(0.0), R8 = vec4(0.0), R9 = vec4(0.0), R10 = vec4(0.0), R11 = vec4(0.0);\n"
    "  vec4 R13 = vec4(0.0), R14 = vec4(0.0), R15 = vec4(0.0);\n"
    "  vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0), oD0 = vec4(0.0, 0.0, 0.0, 1.0), oD1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "  vec4 oFog = vec4(0.0, 0.0, 0.0, 1.0), oPts = vec4(0.0), oB0 = vec4(0.0, 0.0, 0.0, 1.0), oB1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "  vec4 oT0 = vec4(0.0, 0.0, 0.0, 1.0), oT1 = vec4(0.0, 0.0, 0.0, 1.0), oT2 = vec4(0.0, 0.0, 0.0, 1.0), oT3 = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "  vec4 oUnused1, oUnused2, oUnused13, oUnused14, oUnused15;\n"
    "  int A0 = 0;\n";

static const char vs_tail[] =
    "  vD0 = clamp(oD0, 0.0, 1.0); vD1 = clamp(oD1, 0.0, 1.0);\n"
    "  vT0 = oT0; vT1 = oT1; vT2 = oT2; vT3 = oT3;\n"
    "  float w = oPos.w;\n"
    "  if (isinf(w)) w = sign(w) * 1.0e30;\n"
    "  if (abs(w) < 1.0e-30) w = 1.0e-30;\n"
    "  vec2 xy = (2.0 * oPos.xy - surfaceSize) / surfaceSize;\n"
    "  float z = oPos.z / zmax;\n"
    "  gl_Position = vec4(xy * w, (2.0 * z - 1.0) * w, w);\n"
    "}\n";

static const char fs_head[] =
    "#version 330 core\n"
    "%s in vec4 vD0; %s in vec4 vD1;\n"
    "in vec4 vT0; in vec4 vT1; in vec4 vT2; in vec4 vT3; in float vFogF;\n"
    "uniform vec2 texScale[4];\n"
    "uniform vec4 c0[9]; uniform vec4 c1[9];\n"
    "uniform vec4 fogColor;\n"
    "uniform float alphaRef;\n"
    "out vec4 fragColor;\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        hle_log("gpu: shader did not compile:\n%s\n%s\n", log, src);
    }
    return s;
}

static gprog *program_for(const pg_state *st, const uint8_t kinds[4])
{
    prog_key k;
    memset(&k, 0, sizeof k);
    // the vertex program: its words from the start slot to the final one
    int fixed = (pg_m(st, M_TRANSFORM_EXECUTION_MODE) & 3) != 2;
    uint32_t start = pg_m(st, 0x1ea0);
    if (start >= PG_PROGRAM_SLOTS) start = 0;
    uint32_t end = start;
    if (fixed) {
        vsh_ff_key(st, k.ff);
        k.vsh_hash = 1;
    } else {
        while (end < PG_PROGRAM_SLOTS - 1 && !(st->program[end][3] & 1)) end++;
        k.vsh_hash = hash_mem((const uint8_t *)st->program[start], (end - start + 1) * 16) ^ start ^ 2;
    }
    psh_key_of(st, &k.psh, kinds);
    if (pg_m(st, M_FOG_ENABLE)) {
        switch (pg_m(st, M_FOG_MODE)) {
        case 0x2601: k.fog_mode = 1; break;   // linear
        case 0x800: k.fog_mode = 2; break;    // exp
        case 0x801: k.fog_mode = 3; break;    // exp2
        case 0x802: k.fog_mode = 4; break;    // exp, abs
        case 0x803: k.fog_mode = 5; break;    // exp2, abs
        case 0x804: k.fog_mode = 6; break;    // linear, abs
        default: k.fog_mode = 1; break;
        }
    }
    k.flat = pg_m(st, M_SHADE_MODE) == 0x1d00;
    uint64_t h = hash_mem((const uint8_t *)&k, sizeof k);
    gprog **bucket = &progs[h % PROG_BUCKETS];
    for (gprog *p = *bucket; p; p = p->next)
        if (!memcmp(&p->key, &k, sizeof k)) return p;

    if (fixed ? !vsh_ff_translate(st, vsh_body, sizeof vsh_body)
              : !vsh_translate((const uint32_t(*)[4])st->program, start, vsh_body, sizeof vsh_body)) return NULL;
    psh_translate(&k.psh, psh_body, sizeof psh_body);
    const char *fl = k.flat ? "flat" : "";
    static char src[160 * 1024];
    int n = snprintf(src, sizeof src, vs_head, fl, fl);
    n += snprintf(src + n, sizeof src - n, "%s", vsh_body);
    static const char *const fog[7] = {
        "  vFogF = 1.0;\n",
        "  vFogF = fogParams.x + oFog.x * fogParams.y - 1.0;\n",
        "  vFogF = fogParams.x + exp2(oFog.x * fogParams.y * 16.0) - 1.5;\n",
        "  vFogF = fogParams.x + exp2(-oFog.x * oFog.x * fogParams.y * fogParams.y * 32.0) - 1.5;\n",
        "  vFogF = abs(fogParams.x + exp2(oFog.x * fogParams.y * 16.0) - 1.5);\n",
        "  vFogF = abs(fogParams.x + exp2(-oFog.x * oFog.x * fogParams.y * fogParams.y * 32.0) - 1.5);\n",
        "  vFogF = abs(fogParams.x + oFog.x * fogParams.y - 1.0);\n",
    };
    n += snprintf(src + n, sizeof src - n, "%s%s", fog[k.fog_mode], vs_tail);
    GLuint vs = compile(GL_VERTEX_SHADER, src);

    n = snprintf(src, sizeof src, fs_head, fl, fl);
    static const char *const samp[5] = { NULL, "sampler2D", "sampler2D", "samplerCube", "sampler3D" };
    for (int i = 0; i < 4; i++)
        if (kinds[i]) n += snprintf(src + n, sizeof src - n, "uniform %s tex%d;\n", samp[kinds[i]], i);
    n += snprintf(src + n, sizeof src - n, "void main() {\n%s}\n", psh_body);
    GLuint fs = compile(GL_FRAGMENT_SHADER, src);

    gprog *p = calloc(1, sizeof *p);
    p->key = k;
    p->id = glCreateProgram();
    glAttachShader(p->id, vs);
    glAttachShader(p->id, fs);
    glLinkProgram(p->id);
    ch_pc.prog_links++;
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok;
    glGetProgramiv(p->id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p->id, sizeof log, NULL, log);
        hle_log("gpu: program did not link: %s\n", log);
    }
    p->u_c = glGetUniformLocation(p->id, "c");
    p->u_ff = glGetUniformLocation(p->id, "ff");
    p->u_surface = glGetUniformLocation(p->id, "surfaceSize");
    p->u_zmax = glGetUniformLocation(p->id, "zmax");
    p->u_fog = glGetUniformLocation(p->id, "fogParams");
    p->u_c0 = glGetUniformLocation(p->id, "c0");
    p->u_c1 = glGetUniformLocation(p->id, "c1");
    p->u_fogcolor = glGetUniformLocation(p->id, "fogColor");
    p->u_alpharef = glGetUniformLocation(p->id, "alphaRef");
    p->u_texscale = glGetUniformLocation(p->id, "texScale");
    glUseProgram(p->id);
    for (int i = 0; i < 4; i++) {
        char nm[8];
        snprintf(nm, sizeof nm, "tex%d", i);
        GLint l = glGetUniformLocation(p->id, nm);
        if (l >= 0) glUniform1i(l, i);
    }
    p->next = *bucket;
    *bucket = p;
    if (hle_verbose > 1) hle_log("gpu: new program (%u vertex instructions)\n", end - start + 1);
    return p;
}

static void argb(uint32_t v, float *o)
{
    o[0] = (v >> 16 & 0xff) / 255.0f, o[1] = (v >> 8 & 0xff) / 255.0f, o[2] = (v & 0xff) / 255.0f, o[3] = (v >> 24) / 255.0f;
}

// ---- render state ------------------------------------------------------------------------------------
static void apply_state(const pg_state *st)
{
    if (pg_m(st, M_BLEND_ENABLE)) {
        glEnable(GL_BLEND);
        glBlendFunc(pg_m(st, M_BLEND_SFACTOR), pg_m(st, M_BLEND_DFACTOR));
        uint32_t eq = pg_m(st, M_BLEND_EQUATION);
        if (eq == 0xf005) eq = GL_FUNC_REVERSE_SUBTRACT;
        else if (eq == 0xf006 || !eq) eq = GL_FUNC_ADD;
        glBlendEquation(eq);
        float bc[4];
        argb(pg_m(st, M_BLEND_COLOR), bc);
        glBlendColor(bc[0], bc[1], bc[2], bc[3]);
    } else {
        glDisable(GL_BLEND);
    }
    if (cur_zeta && pg_m(st, M_DEPTH_TEST_ENABLE)) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(pg_m(st, M_DEPTH_FUNC) ? pg_m(st, M_DEPTH_FUNC) : GL_LESS);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(pg_m(st, M_DEPTH_MASK) ? GL_TRUE : GL_FALSE);
    if (cur_zeta && pg_m(st, M_STENCIL_TEST_ENABLE)) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(pg_m(st, M_STENCIL_FUNC) ? pg_m(st, M_STENCIL_FUNC) : GL_ALWAYS, pg_m(st, M_STENCIL_REF),
                      pg_m(st, M_STENCIL_FUNC_MASK));
        uint32_t f = pg_m(st, M_STENCIL_OP_FAIL), zf = pg_m(st, M_STENCIL_OP_ZFAIL), zp = pg_m(st, M_STENCIL_OP_ZPASS);
        glStencilOp(f ? f : GL_KEEP, zf ? zf : GL_KEEP, zp ? zp : GL_KEEP);
        glStencilMask(pg_m(st, M_STENCIL_MASK));
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    if (pg_m(st, M_CULL_FACE_ENABLE)) {
        glEnable(GL_CULL_FACE);
        uint32_t cf = pg_m(st, M_CULL_FACE);
        glCullFace(cf == GL_FRONT || cf == GL_FRONT_AND_BACK ? cf : GL_BACK);
        // the NV2A's winding is on the screen (y down); the vertex epilogue puts
        // screen y = 0 at GL's window y = 0 (bottom), so GL sees the triangle
        // mirrored and the windings swap. (HotD3's level geometry, drawn with
        // the front faces culled, vanished without this.)
        glFrontFace(pg_m(st, M_FRONT_FACE) == GL_CW ? GL_CCW : GL_CW);
    } else {
        glDisable(GL_CULL_FACE);
    }
    uint32_t cm = pg_m(st, M_COLOR_MASK);
    glColorMask((cm >> 16 & 0xff) != 0, (cm >> 8 & 0xff) != 0, (cm & 0xff) != 0, (cm >> 24) != 0);
    if (pg_m(st, M_POLY_OFFSET_FILL_ENABLE)) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(u2f(pg_m(st, M_POLYGON_OFFSET_SCALE)), u2f(pg_m(st, M_POLYGON_OFFSET_BIAS)));
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    glPolygonMode(GL_FRONT_AND_BACK, pg_m(st, M_FRONT_POLYGON_MODE) == GL_LINE ? GL_LINE
                                     : pg_m(st, M_FRONT_POLYGON_MODE) == GL_POINT ? GL_POINT : GL_FILL);
    // the window clip: inclusive bounds
    uint32_t wh = pg_m(st, M_WINDOW_CLIP_H), wv = pg_m(st, M_WINDOW_CLIP_V);
    int x0 = wh & 0xfff, x1 = wh >> 16 & 0xfff, y0 = wv & 0xfff, y1 = wv >> 16 & 0xfff;
    if (x1 >= x0 && y1 >= y0 && (wh || wv)) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
}

// ---- the interface ------------------------------------------------------------------------------------
#ifdef CH_CORE
// an OpenGL 3.3 core context with no surface: everything is drawn to textures
static int open_context(void)
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_display) dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
        hle_log("gpu: no EGL display: nothing will be drawn\n");
        return 0;
    }
    EGLint attrs[] = {EGL_SURFACE_TYPE, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    EGLint cattrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                       EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    if (eglBindAPI(EGL_OPENGL_API) && eglChooseConfig(dpy, attrs, &cfg, 1, &n) && n)
        ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cattrs);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        hle_log("gpu: no OpenGL 3.3 from EGL: nothing will be drawn\n");
        if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
        ctx = EGL_NO_CONTEXT;
        eglTerminate(dpy);
        dpy = EGL_NO_DISPLAY;
        return 0;
    }
    return 1;
}
#else
static int open_context(void)
{
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        hle_log("gpu: no video (%s): nothing will be drawn\n", SDL_GetError());
        return 0;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    win = SDL_CreateWindow("Chihiro", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h,
                           SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!win || !(ctx = SDL_GL_CreateContext(win))) {
        hle_log("gpu: no OpenGL 3.3 (%s): nothing will be drawn\n", SDL_GetError());
        return 0;
    }
    SDL_GL_SetSwapInterval(0);
    return 1;
}
#endif

int gpu_init(void)
{
    if (af_debug_env("CH_NOGPU") || !open_context()) return 0;
    hle_log("gpu: %s\n", (const char *)glGetString(GL_RENDERER));
    glGenFramebuffers(1, &fbo);
    glGenFramebuffers(1, &blit_fbo);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    for (int i = 0; i < PG_ATTRS; i++) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, sizeof(pg_vertex), (void *)(uintptr_t)(16 * i));
    }
    glEnable(GL_DEPTH_CLAMP);
    shot_dir = af_debug_env("CH_SHOT");
    shot_every = af_debug_env("CH_SHOT_EVERY") ? atoi(af_debug_env("CH_SHOT_EVERY")) : 60;
    if (!shot_every) shot_every = 60;
    return 1;
}

void gpu_clear(const pg_state *st, uint32_t flags)
{
    if (!ctx || !bind_target(st)) return;
    uint32_t rh = pg_m(st, 0x1d98), rv = pg_m(st, 0x1d9c);
    int x0 = rh & 0xffff, x1 = rh >> 16, y0 = rv & 0xffff, y1 = rv >> 16;
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    GLbitfield bits = 0;
    if ((flags & 0xf0) && cur_color) {
        float c[4];
        argb(pg_m(st, 0x1d90), c);
        glColorMask((flags & 0x10) != 0, (flags & 0x20) != 0, (flags & 0x40) != 0, (flags & 0x80) != 0);
        glClearColor(c[0], c[1], c[2], c[3]);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if ((flags & 3) && cur_zeta) {
        uint32_t z = pg_m(st, 0x1d8c);
        if (cur_zeta->fmt == 1) {   // Z16
            glClearDepth((z & 0xffff) / 65535.0);
        } else {
            glClearDepth((z >> 8) / 16777215.0);
            glClearStencil(z & 0xff);
        }
        if (flags & 1) {
            glDepthMask(GL_TRUE);
            bits |= GL_DEPTH_BUFFER_BIT;
        }
        if (flags & 2) {
            glStencilMask(0xff);
            bits |= GL_STENCIL_BUFFER_BIT;
        }
    }
    if (bits) glClear(bits);
}

static uint32_t *idx;
static uint32_t idx_cap;

static void need_idx(uint32_t n)
{
    if (n > idx_cap) {
        idx_cap = n * 2;
        idx = realloc(idx, idx_cap * 4);
    }
}

static void gpu_draw_gl(const pg_state *st, int prim, const pg_vertex *v, uint32_t n);

// Only the attributes that differ between a draw's vertices go to the GPU,
// packed (a vertex is 16 attributes of 4 floats, 256 bytes, and most draws
// use 3 or 4); the others are the same in every vertex, set once as the
// attribute's current value.
static float *packed;
static size_t packed_cap;
static uint32_t enabled_attrs = (1u << PG_ATTRS) - 1;

static void upload_vertices(uint32_t vary, const pg_vertex *v, uint32_t n)
{
    int k = 0, slot[PG_ATTRS];
    for (int i = 0; i < PG_ATTRS; i++)
        if (vary >> i & 1) slot[k++] = i;
    if (k == PG_ATTRS) {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n * sizeof *v, v, GL_STREAM_DRAW);
    } else {
        size_t need = (size_t)n * k * 4;
        if (need > packed_cap) {
            packed_cap = need * 2;
            packed = realloc(packed, packed_cap * sizeof *packed);
        }
        float *o = packed;
        for (uint32_t j = 0; j < n; j++)
            for (int a = 0; a < k; a++, o += 4) memcpy(o, v[j].a[slot[a]], 16);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(need * sizeof *packed), packed, GL_STREAM_DRAW);
    }
    int stride = 16 * k;
    for (int i = 0, a = 0; i < PG_ATTRS; i++) {
        if (vary >> i & 1) {
            if (!(enabled_attrs >> i & 1)) glEnableVertexAttribArray(i);
            glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, stride, (void *)(uintptr_t)(16 * a++));
        } else {
            if (enabled_attrs >> i & 1) glDisableVertexAttribArray(i);
            glVertexAttrib4fv(i, v[0].a[i]);
        }
    }
    enabled_attrs = vary;
}
void gpu_draw(const pg_state *st, int prim, const pg_vertex *v, uint32_t n)
{
    uint64_t t = ch_perf > 0 ? ch_perf_ns() : 0;
    gpu_draw_gl(st, prim, v, n);
    if (t) {
        ch_pc.draw_ns += ch_perf_ns() - t;
        ch_pc.draws++;
        ch_pc.vertices += n;
    }
}

static void gpu_draw_gl(const pg_state *st, int prim, const pg_vertex *v, uint32_t n)
{
    if (!ctx || !bind_target(st)) return;
    uint8_t kinds[4] = { 0 };
    float scale[4][2];
    for (int i = 0; i < 4; i++) {
        int mode = pg_m(st, M_SHADER_STAGE_PROGRAM) >> (5 * i) & 0x1f;
        if (mode == 0 || mode == 4 || mode == 5) {
            scale[i][0] = scale[i][1] = 1;
            continue;
        }
        kinds[i] = bind_texture(st, i, scale[i]);
    }
    gprog *p = program_for(st, kinds);
    if (!p) return;
    glUseProgram(p->id);
    glUniform4fv(p->u_c, PG_CONSTANTS, &st->constants[0][0]);
    if (p->u_ff >= 0) {
        float ff[VSH_FF_UNIFORMS][4];
        vsh_ff_uniforms(st, ff);
        glUniform4fv(p->u_ff, VSH_FF_UNIFORMS, &ff[0][0]);
    }
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    glUniform2f(p->u_surface, (float)vp[2], (float)vp[3]);
    float zmax = u2f(pg_m(st, M_CLIP_MAX));
    if (!(zmax > 0)) zmax = 16777215.0f;
    glUniform1f(p->u_zmax, zmax);
    glUniform4f(p->u_fog, u2f(pg_m(st, M_FOG_PARAMS)), u2f(pg_m(st, M_FOG_PARAMS + 4)), u2f(pg_m(st, M_FOG_PARAMS + 8)), 0);
    float c0[9][4], c1[9][4];
    for (int i = 0; i < 8; i++) {
        argb(pg_m(st, M_COMBINER_FACTOR0 + 4 * i), c0[i]);
        argb(pg_m(st, M_COMBINER_FACTOR1 + 4 * i), c1[i]);
    }
    argb(pg_m(st, 0x1e20), c0[8]);   // SET_SPECULAR_FOG_FACTOR
    argb(pg_m(st, 0x1e24), c1[8]);
    glUniform4fv(p->u_c0, 9, &c0[0][0]);
    glUniform4fv(p->u_c1, 9, &c1[0][0]);
    // SET_FOG_COLOR is R, G, B, A from the low byte
    float fc[4];
    uint32_t f = pg_m(st, M_FOG_COLOR);
    fc[0] = (f & 0xff) / 255.0f, fc[1] = (f >> 8 & 0xff) / 255.0f, fc[2] = (f >> 16 & 0xff) / 255.0f, fc[3] = (f >> 24) / 255.0f;
    glUniform4fv(p->u_fogcolor, 1, fc);
    glUniform1f(p->u_alpharef, (float)(pg_m(st, M_ALPHA_REF) & 0xff));
    glUniform2fv(p->u_texscale, 4, &scale[0][0]);
    apply_state(st);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    upload_vertices(st->draw_vary, v, n);
    GLenum mode;
    uint32_t ni = 0;
    switch (prim) {
    case PRIM_POINTS: mode = GL_POINTS; break;
    case PRIM_LINES: mode = GL_LINES; break;
    case PRIM_LINE_LOOP: mode = GL_LINE_LOOP; break;
    case PRIM_LINE_STRIP: mode = GL_LINE_STRIP; break;
    case PRIM_TRIANGLES: mode = GL_TRIANGLES; break;
    case PRIM_TRIANGLE_STRIP: mode = GL_TRIANGLE_STRIP; break;
    case PRIM_TRIANGLE_FAN: mode = GL_TRIANGLE_FAN; break;
    case PRIM_QUADS:
        mode = GL_TRIANGLES;
        need_idx(n / 4 * 6);
        for (uint32_t q = 0; q + 3 < n; q += 4) {
            idx[ni++] = q, idx[ni++] = q + 1, idx[ni++] = q + 2;
            idx[ni++] = q, idx[ni++] = q + 2, idx[ni++] = q + 3;
        }
        break;
    case PRIM_QUAD_STRIP:
        mode = GL_TRIANGLES;
        need_idx(n * 3);
        for (uint32_t q = 0; q + 3 < n; q += 2) {
            idx[ni++] = q, idx[ni++] = q + 1, idx[ni++] = q + 3;
            idx[ni++] = q, idx[ni++] = q + 3, idx[ni++] = q + 2;
        }
        break;
    default:   // polygon: a fan
        mode = GL_TRIANGLE_FAN;
        break;
    }
    if (ni) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, ni * 4, idx, GL_STREAM_DRAW);
        glDrawElements(mode, ni, GL_UNSIGNED_INT, 0);
    } else {
        glDrawArrays(mode, 0, n);
    }
}

static void screenshot(surface *s)
{
    char path[512];
    snprintf(path, sizeof path, "%s/frame%05u.ppm", shot_dir, frame_no);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint8_t *px = malloc((size_t)s->w * s->h * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, blit_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, s->w, s->h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    fprintf(f, "P6\n%u %u\n255\n", s->w, s->h);
    for (uint32_t i = 0; i < s->w * s->h; i++) fwrite(px + 4 * i, 1, 3, f);   // row 0 is the top already
    fclose(f);
    free(px);
}

static void gpu_flip_gl(const pg_state *st);
void gpu_flip(const pg_state *st)
{
    uint64_t t = ch_perf > 0 ? ch_perf_ns() : 0;
    gpu_flip_gl(st);
    if (t) ch_pc.flip_ns += ch_perf_ns() - t;
    ch_perf_frame();
}

static void gpu_flip_gl(const pg_state *st)
{
    frame_no++;
    if (hle_verbose > 0 && frame_no % 60 == 0) hle_log("gpu: frame %u\n", frame_no);
    if (!ctx) return;
    surface *s = surface_find_addr(phys_of(st->dma_color + pg_m(st, M_SURFACE_COLOR_OFFSET)), 0);
    if (!s) return;
    if (shot_dir && frame_no % shot_every == 0) screenshot(s);
#ifdef CH_CORE
    // the picture for the front end: row 0 is the top already
    size_t n = (size_t)s->w * s->h;
    if (present_cap < n) {
        free(present_px);
        present_px = malloc(n * 4);
        present_cap = n;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, blit_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, s->w, s->h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, present_px);
    for (size_t i = 0; i < n; i++) present_px[i] |= 0xff000000u;
    if (gpu_present) gpu_present(present_px, (int)s->w, (int)s->h);
#else
    SDL_GetWindowSize(win, &win_w, &win_h);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, blit_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    // keep the aspect; texture row 0 is the top of the picture
    int dw = win_w, dh = win_h;
    if ((int64_t)dw * s->h > (int64_t)dh * s->w) dw = dh * s->w / s->h;
    else dh = dw * s->h / s->w;
    int dx = (win_w - dw) / 2, dy = (win_h - dh) / 2;
    glBlitFramebuffer(0, 0, s->w, s->h, dx, dy + dh, dx + dw, dy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    SDL_GL_SwapWindow(win);
    gpu_poll_events();
#endif
}

void gpu_flush_surfaces(void) {}

void input_frame(uint32_t frame, int win_w, int win_h);   // input_sdl.c

#ifdef CH_CORE
void gpu_poll_events(void) {}   // the front end has the controls
#else
void gpu_poll_events(void)
{
    if (!ctx) return;
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_QUIT) exit(0);
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    input_frame(frame_no, w, h);
}
#endif

// the GPU thread is ending: every object goes with the context, and the
// caches that named them are emptied for the next start
void gpu_shutdown(void)
{
    for (int i = 0; i < PROG_BUCKETS; i++)
        while (progs[i]) {
            gprog *p = progs[i];
            progs[i] = p->next;
            free(p);
        }
    memset(surfs, 0, sizeof surfs);
    memset(texs, 0, sizeof texs);
    cur_color = cur_zeta = NULL;
    fbo = vao = vbo = ibo = blit_fbo = 0;
    frame_no = 0;
#ifdef CH_CORE
    if (ctx != EGL_NO_CONTEXT) {
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(dpy, ctx);
        eglTerminate(dpy);
    }
    ctx = EGL_NO_CONTEXT;
    dpy = EGL_NO_DISPLAY;
#else
    if (ctx) SDL_GL_DeleteContext(ctx);
    if (win) SDL_DestroyWindow(win);
    ctx = NULL;
    win = NULL;
#endif
}
