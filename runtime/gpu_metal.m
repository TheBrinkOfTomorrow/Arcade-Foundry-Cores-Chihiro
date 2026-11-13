// The renderer on Metal (macOS, iOS, tvOS): the same work as gpu_gl.c. Runs
// on the GPU thread. The shader translators (vsh.c, vsh_ff.c, psh.c) write
// GLSL bodies; a preamble of Metal definitions lets the Metal compiler take
// them as they are.
//
// Surfaces: every colour and depth surface the game draws to is a texture,
// found by its address, as in gpu_gl.c. Texture row 0 is the top (lowest
// address) of the surface; Metal's normalized y = +1 is row 0, so the vertex
// epilogue maps screen y = 0 to +1 (GL's maps it to -1), so Metal sees the
// screen as the NV2A does and the windings are the NV2A's own (GL's swap).
//
// One command buffer a frame; a render pass for each run of draws to the
// same surfaces. Uploads (textures, surfaces the CPU wrote) go through a blit
// pass in the same command buffer, so they land between the draws that come
// before and after them.
//
// The window is the front end's: it hands over its CAMetalLayer with
// gpu_metal_set_layer before the game starts (chihiro-run on a Mac makes an
// SDL window on the main thread, as Cocoa wants), and reads input itself.
// The core library (gpu_offscreen) has no window: each frame shown is drawn
// into a BGRA texture, copied to a buffer and handed to gpu_present.
#include "af_debug.h"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "perf.h"
#include "xbox.h"
#include "pgraph.h"
#include <math.h>
#include <string.h>

// ARC leaves autoreleased objects to a pool, and this thread has none of its own
extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *);
static void *frame_pool;

static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static id<MTLCommandBuffer> cmd;
static id<MTLRenderCommandEncoder> enc;
static CAMetalLayer *layer;
static dispatch_semaphore_t in_flight;
static uint32_t frame_no;
static const char *shot_dir;
static uint32_t shot_every;
static int ready;

void gpu_metal_set_layer(void *metal_layer) { layer = (__bridge CAMetalLayer *)metal_layer; }
uint32_t gpu_frame(void) { return frame_no; }

// ---- per-frame buffers for vertices and indices ---------------------------------------------------------------
#define RING 3
#define RING_BYTES (48u << 20)
static id<MTLBuffer> ring[RING];
static uint32_t ring_i, ring_used;

// room for n bytes in this frame's buffer; NULL when it is full
static id<MTLBuffer> ring_alloc(const void *data, uint32_t n, uint32_t *offset)
{
    uint32_t at = (ring_used + 255) & ~255u;
    if (at + n > RING_BYTES) return nil;
    memcpy((uint8_t *)ring[ring_i].contents + at, data, n);
    ring_used = at + n;
    *offset = at;
    return ring[ring_i];
}

static void end_pass(void)
{
    if (enc) [enc endEncoding];
    enc = nil;
}

static void need_cmd(void)
{
    if (cmd) return;
    if (!frame_pool) frame_pool = objc_autoreleasePoolPush();
    dispatch_semaphore_wait(in_flight, DISPATCH_TIME_FOREVER);
    ring_i = (ring_i + 1) % RING;
    ring_used = 0;
    cmd = [queue commandBuffer];
    dispatch_semaphore_t s = in_flight;
    [cmd addCompletedHandler:^(id<MTLCommandBuffer> b) {
        (void)b;
        dispatch_semaphore_signal(s);
    }];
}

// ---- surfaces ------------------------------------------------------------------------------------------
typedef struct surface {
    uint32_t addr, w, h, zeta, fmt;
    __unsafe_unretained id<MTLTexture> tex;   // owned through tex_own
    uint32_t used;
    uint64_t ram_hash;
    uint32_t ram_len, ram_checked;
} surface;
#define MAX_SURF 64
static surface surfs[MAX_SURF];
static NSMutableSet *owned;   // keeps the textures the C structs point at

static id<MTLTexture> tex_own(id<MTLTexture> t)
{
    if (t) [owned addObject:t];
    return t;
}

static void tex_drop(id<MTLTexture> t)
{
    if (t) [owned removeObject:t];
}

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
            if (s->w >= w && s->h >= h) {
                s->used = frame_no;
                return s;
            }
            tex_drop(s->tex);
            s->tex = nil;
            if (!free_s) free_s = s;
            continue;
        }
        if (!old || s->used < old->used) old = s;
    }
    if (!free_s) {
        tex_drop(old->tex);
        old->tex = nil;
        free_s = old;
    }
    surface *s = free_s;
    s->addr = addr, s->w = w, s->h = h, s->zeta = zeta, s->fmt = fmt, s->used = frame_no;
    s->ram_len = 0;
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:zeta ? MTLPixelFormatDepth32Float_Stencil8
                                                                       : MTLPixelFormatRGBA8Unorm
                                                           width:w
                                                          height:h
                                                       mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModePrivate;
    s->tex = tex_own([dev newTextureWithDescriptor:d]);
    if (hle_verbose > 0) hle_log("gpu: %s surface 0x%08x %ux%u format %u\n", zeta ? "depth" : "colour", addr, w, h, fmt);
    return s;
}

static surface *cur_color, *cur_zeta, *pass_color, *pass_zeta;
static uint32_t cur_w, cur_h;

// the state's surfaces, their size from the clip rectangle; a new render pass when they change
static int bind_target(const pg_state *st)
{
    uint32_t ch = pg_m(st, M_SURFACE_CLIP_H), cv = pg_m(st, M_SURFACE_CLIP_V), fmt = pg_m(st, M_SURFACE_FORMAT);
    uint32_t w = (ch & 0xffff) + (ch >> 16), h = (cv & 0xffff) + (cv >> 16);
    if ((fmt >> 8 & 0xf) == 2) {
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
    if (!cur_color && !cur_zeta) return 0;
    // a depth surface bigger than the colour one (or the other way) cannot share a pass
    if (cur_color && cur_zeta && (cur_zeta->w < cur_color->w || cur_zeta->h < cur_color->h)) cur_zeta = NULL;
    cur_w = w, cur_h = h;
    need_cmd();
    if (enc && pass_color == cur_color && pass_zeta == cur_zeta) return 1;
    end_pass();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    if (cur_color) {
        rp.colorAttachments[0].texture = cur_color->tex;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    if (cur_zeta) {
        rp.depthAttachment.texture = cur_zeta->tex;
        rp.depthAttachment.loadAction = MTLLoadActionLoad;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        rp.stencilAttachment.texture = cur_zeta->tex;
        rp.stencilAttachment.loadAction = MTLLoadActionLoad;
        rp.stencilAttachment.storeAction = MTLStoreActionStore;
    }
    if (!cur_color) {   // depth only: Metal wants the pass's size
        rp.renderTargetWidth = cur_zeta->w;
        rp.renderTargetHeight = cur_zeta->h;
    }
    enc = [cmd renderCommandEncoderWithDescriptor:rp];
    pass_color = cur_color, pass_zeta = cur_zeta;
    [enc setDepthClipMode:MTLDepthClipModeClamp];
    return 1;
}

// ---- uploads -------------------------------------------------------------------------------------------
static uint32_t *decode_buf;
static size_t decode_cap;

static void need_decode(size_t texels)
{
    if (texels > decode_cap) {
        decode_cap = texels;
        decode_buf = realloc(decode_buf, texels * 4);
    }
}

// copy RGBA8 texels into a texture through this frame's command buffer
static void upload(id<MTLTexture> t, uint32_t slice, uint32_t w, uint32_t h, uint32_t d, uint32_t row_texels,
                   const uint32_t *px)
{
    need_cmd();
    uint32_t bytes = row_texels * 4 * h * d, off;
    id<MTLBuffer> b = ring_alloc(px, bytes, &off);
    id<MTLBuffer> own = nil;
    if (!b) {   // too big for what is left of this frame's buffer
        own = [dev newBufferWithBytes:px length:bytes options:MTLResourceStorageModeShared];
        b = own, off = 0;
    }
    end_pass();
    id<MTLBlitCommandEncoder> bl = [cmd blitCommandEncoder];
    [bl copyFromBuffer:b
               sourceOffset:off
          sourceBytesPerRow:row_texels * 4
        sourceBytesPerImage:row_texels * 4 * h
                 sourceSize:MTLSizeMake(w, h, d)
                  toTexture:t
           destinationSlice:slice
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    [bl endEncoding];
}

static void make_mipmaps(id<MTLTexture> t)
{
    end_pass();
    id<MTLBlitCommandEncoder> bl = [cmd blitCommandEncoder];
    [bl generateMipmapsForTexture:t];
    [bl endEncoding];
}

// ---- textures ------------------------------------------------------------------------------------------
typedef struct gtex {
    uint32_t addr, format, w, h, pitch, palette;
    uint64_t hash;
    uint32_t checked, used;
    __unsafe_unretained id<MTLTexture> tex;
} gtex;
#define MAX_TEX 1024
static gtex texs[MAX_TEX];

static uint64_t hash_mem(const uint8_t *p, uint32_t n)
{
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
        tex_drop(old->tex);
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
            need_decode((size_t)t->width * t->height * t->depth * (t->cube ? 6 : 1));
            tex_decode(t, (const uint8_t *)M, decode_buf);
            if (fresh) {
                MTLTextureDescriptor *d = [MTLTextureDescriptor new];
                d.pixelFormat = MTLPixelFormatRGBA8Unorm;
                d.width = t->width, d.height = t->height;
                d.textureType = t->cube ? MTLTextureTypeCube : t->depth > 1 ? MTLTextureType3D : MTLTextureType2D;
                if (t->depth > 1) d.depth = t->depth;
                uint32_t big = t->width > t->height ? t->width : t->height;
                d.mipmapLevelCount = t->levels > 1 && !(t->depth > 1) ? (NSUInteger)floor(log2(big)) + 1 : 1;
                d.usage = MTLTextureUsageShaderRead;
                d.storageMode = MTLStorageModePrivate;
                g->tex = tex_own([dev newTextureWithDescriptor:d]);
            }
            if (t->cube) {
                for (int f = 0; f < 6; f++)
                    upload(g->tex, f, t->width, t->height, 1, t->width, decode_buf + (size_t)f * t->width * t->height);
            } else {
                upload(g->tex, 0, t->width, t->height, t->depth, t->width, decode_buf);
            }
            if (g->tex.mipmapLevelCount > 1) make_mipmaps(g->tex);
        }
    }
    g->used = frame_no;
    return g;
}

// The GPU's drawing never reaches RAM, so RAM under a surface changes only
// when the CPU writes it (Sofdec's movie frames): see gpu_gl.c.
static void surface_from_ram(surface *s, const pg_texture *t)
{
    if (s->ram_checked == frame_no) return;
    s->ram_checked = frame_no;
    uint32_t len = tex_bytes(t);
    if (t->addr + len > RAM_SIZE) len = RAM_SIZE - t->addr;
    uint64_t h = hash_mem((const uint8_t *)M + t->addr, len);
    if (s->ram_len != len) {
        s->ram_len = len, s->ram_hash = h;
        return;
    }
    if (h == s->ram_hash) return;
    s->ram_hash = h;
    need_decode((size_t)t->width * t->height);
    tex_decode(t, (const uint8_t *)M, decode_buf);
    uint32_t w = t->width < s->w ? t->width : s->w, h2 = t->height < s->h ? t->height : s->h;
    upload(s->tex, 0, w, h2, 1, t->width, decode_buf);
}

// ---- samplers ------------------------------------------------------------------------------------------
static NSMutableDictionary<NSNumber *, id<MTLSamplerState>> *samplers;

static MTLSamplerAddressMode wrap_of(uint32_t v)
{
    switch (v) {
    case 1: return MTLSamplerAddressModeRepeat;
    case 2: return MTLSamplerAddressModeMirrorRepeat;
    case 4: return MTLSamplerAddressModeClampToBorderColor;
    }
    return MTLSamplerAddressModeClampToEdge;
}

static id<MTLSamplerState> sampler_for(uint32_t minf, uint32_t magf, int levels, uint32_t addr, uint32_t border)
{
    // NV2A min filter: 1 nearest, 2 linear, 3-6 the four mipmapped ones
    int min_lin = minf == 2 || minf == 4 || minf == 6 || minf == 7;
    int mip = levels > 1 && minf >= 3 && minf <= 6 ? (minf == 5 || minf == 6 ? 2 : 1) : 0;
    int bc = (border >> 24) < 0x80 ? 0 : (border & 0xffffff) ? 2 : 1;   // transparent black, opaque black, white
    uint64_t key = (uint64_t)min_lin | (uint64_t)(magf == 1) << 1 | (uint64_t)mip << 2 | (uint64_t)(addr & 0xf0f0f) << 4 |
                   (uint64_t)bc << 32;
    NSNumber *k = @(key);
    id<MTLSamplerState> s = samplers[k];
    if (s) return s;
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.minFilter = min_lin ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.magFilter = magf == 1 ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    d.mipFilter = mip == 2 ? MTLSamplerMipFilterLinear : mip == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterNotMipmapped;
    d.sAddressMode = wrap_of(addr & 0xf);
    d.tAddressMode = wrap_of(addr >> 8 & 0xf);
    d.rAddressMode = wrap_of(addr >> 16 & 0xf);
#if TARGET_OS_OSX || (defined(__IPHONE_14_0) && __IPHONE_OS_VERSION_MAX_ALLOWED >= __IPHONE_14_0)
    if (@available(macOS 10.12, iOS 14.0, tvOS 14.0, *))
        d.borderColor = bc == 0 ? MTLSamplerBorderColorTransparentBlack
                        : bc == 1 ? MTLSamplerBorderColorOpaqueBlack : MTLSamplerBorderColorOpaqueWhite;
#endif
    s = [dev newSamplerStateWithDescriptor:d];
    samplers[k] = s;
    return s;
}

// stage i's texture and sampler, made ready (uploads may end the pass); its
// kind for the shader, and the scale for texel coordinates
static int prepare_texture(const pg_state *st, int i, float scale[2], id<MTLTexture> __strong *mt_out,
                           id<MTLSamplerState> __strong *smp_out)
{
    pg_texture t;
    scale[0] = scale[1] = 1;
    if (!pg_texture_of(st, i, &t)) return 0;
    surface *s = surface_find_addr(t.addr, 0);
    id<MTLTexture> mt;
    int kind;
    uint32_t tw, th;
    int levels;
    if (s && !t.cube && t.depth == 1) {   // drawn by the GPU: use the surface
        if (cur_color == s) return 0;     // reading the surface being drawn: not allowed here
        surface_from_ram(s, &t);
        mt = s->tex;
        tw = s->w, th = s->h;
        kind = t.linear ? 2 : 1;
        levels = 1;
    } else {
        gtex *g = texture_get(&t);
        mt = g->tex;
        tw = t.width, th = t.height;
        kind = t.cube ? 3 : t.depth > 1 ? 4 : t.linear ? 2 : 1;
        levels = (int)g->tex.mipmapLevelCount;
    }
    if (kind == 2) scale[0] = 1.0f / tw, scale[1] = 1.0f / th;
    uint32_t filt = pg_m(st, M_TEXTURE_FILTER + 0x40 * i), addr = pg_m(st, M_TEXTURE_ADDRESS + 0x40 * i);
    *smp_out = sampler_for(filt >> 16 & 0xff, filt >> 24 & 0xf, levels, addr, pg_m(st, M_TEXTURE_BORDER_COLOR + 0x40 * i));
    *mt_out = mt;
    return kind;
}

// ---- shaders -------------------------------------------------------------------------------------------
typedef struct prog_key {
    uint64_t vsh_hash;
    uint32_t ff[VSH_FF_KEY];
    psh_key psh;
    uint8_t fog_mode, flat, pad[2];
} prog_key;

typedef struct gprog {
    prog_key key;
    __unsafe_unretained id<MTLFunction> vs, fs;
    struct gprog *next;
} gprog;
#define PROG_BUCKETS 1024
static gprog *progs[PROG_BUCKETS];
static NSMutableArray *functions;   // keeps the functions the C structs point at
static char vsh_body[64 * 1024], psh_body[64 * 1024];

// GLSL's names for Metal's, enough for what the translators write
static const char msl_common[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "#define vec2 float2\n#define vec3 float3\n#define vec4 float4\n"
    "#define ivec4 int4\n#define bvec4 bool4\n"
    "#define inversesqrt rsqrt\n"
    "static inline bool4 lessThan(float4 a, float4 b) { return a < b; }\n"
    "static inline bool4 greaterThanEqual(float4 a, float4 b) { return a >= b; }\n";

#define PG_STR_(x) #x
#define PG_STR(x) PG_STR_(x)
static const char vs_head[] =
    "struct VU { float2 surfaceSize; float zmax; float pad; float4 fogParams; };\n"
    "struct V2F { float4 pos [[position]]; float4 vD0 %s; float4 vD1 %s;\n"
    "  float4 vT0; float4 vT1; float4 vT2; float4 vT3; float vFogF; float psize [[point_size]]; };\n"
    "static float vsh_rcc(float x) {\n"
    "  float r = 1.0 / x;\n"
    "  return r >= 0.0 ? clamp(r, 5.42101e-20, 1.884467e+19) : clamp(r, -1.884467e+19, -5.42101e-20);\n"
    "}\n"
    "static float4 vsh_exp(float x) { float f = floor(x); return float4(exp2(f), x - f, exp2(x), 1.0); }\n"
    "static float4 vsh_log(float x) {\n"
    "  x = abs(x);\n"
    "  if (x == 0.0) return float4(-INFINITY, 1.0, -INFINITY, 1.0);\n"
    "  float e = floor(log2(x));\n"
    "  return float4(e, x / exp2(e), log2(x), 1.0);\n"
    "}\n"
    "static float4 vsh_lit(float4 s) {\n"
    "  float p = clamp(s.w, -128.0, 128.0);\n"
    "  return float4(1.0, max(s.x, 0.0), s.x > 0.0 ? pow(max(s.y, 0.0), p) : 0.0, 1.0);\n"
    "}\n"
    "vertex V2F vmain(uint vid [[vertex_id]], const device float4 *vb [[buffer(0)]],\n"
    "                 constant float4 *c [[buffer(1)]], constant float4 *ff [[buffer(2)]],\n"
    "                 constant VU &u [[buffer(3)]]) {\n"
    "  V2F out_;\n"
    "  const device float4 *va = vb + vid * " PG_STR(PG_ATTRS) ";\n"
    "  float4 v0 = va[0], v1 = va[1], v2 = va[2], v3 = va[3], v4 = va[4], v5 = va[5], v6 = va[6], v7 = va[7];\n"
    "  float4 v8 = va[8], v9 = va[9], v10 = va[10], v11 = va[11], v12 = va[12], v13 = va[13], v14 = va[14], v15 = va[15];\n"
    "  float2 surfaceSize = u.surfaceSize; float zmax = u.zmax; float4 fogParams = u.fogParams;\n"
    "  float4 vD0, vD1, vT0, vT1, vT2, vT3; float vFogF;\n"
    "  float4 R0 = float4(0.0), R1 = float4(0.0), R2 = float4(0.0), R3 = float4(0.0), R4 = float4(0.0), R5 = float4(0.0);\n"
    "  float4 R6 = float4(0.0), R7 = float4(0.0), R8 = float4(0.0), R9 = float4(0.0), R10 = float4(0.0), R11 = float4(0.0);\n"
    "  float4 R13 = float4(0.0), R14 = float4(0.0), R15 = float4(0.0);\n"
    "  float4 oPos = float4(0.0, 0.0, 0.0, 1.0), oD0 = float4(0.0, 0.0, 0.0, 1.0), oD1 = float4(0.0, 0.0, 0.0, 1.0);\n"
    "  float4 oFog = float4(0.0, 0.0, 0.0, 1.0), oPts = float4(0.0), oB0 = float4(0.0, 0.0, 0.0, 1.0), oB1 = float4(0.0, 0.0, 0.0, 1.0);\n"
    "  float4 oT0 = float4(0.0, 0.0, 0.0, 1.0), oT1 = float4(0.0, 0.0, 0.0, 1.0), oT2 = float4(0.0, 0.0, 0.0, 1.0), oT3 = float4(0.0, 0.0, 0.0, 1.0);\n"
    "  float4 oUnused1, oUnused2, oUnused13, oUnused14, oUnused15;\n"
    "  int A0 = 0;\n";

// as gpu_gl.c's, but Metal's depth range is 0..1 and its y = +1 is row 0
static const char vs_tail[] =
    "  vD0 = clamp(oD0, 0.0, 1.0); vD1 = clamp(oD1, 0.0, 1.0);\n"
    "  vT0 = oT0; vT1 = oT1; vT2 = oT2; vT3 = oT3;\n"
    "  float w = oPos.w;\n"
    "  if (isinf(w)) w = sign(w) * 1.0e30;\n"
    "  if (abs(w) < 1.0e-30) w = 1.0e-30;\n"
    "  float2 xy = (2.0 * oPos.xy - surfaceSize) / surfaceSize;\n"
    "  float z = oPos.z / zmax;\n"
    "  out_.pos = float4(xy.x * w, -xy.y * w, z * w, w);\n"
    "  out_.vD0 = vD0; out_.vD1 = vD1; out_.vT0 = vT0; out_.vT1 = vT1; out_.vT2 = vT2; out_.vT3 = vT3;\n"
    "  out_.vFogF = vFogF; out_.psize = 1.0;\n"
    "  return out_;\n"
    "}\n";

static const char fs_head[] =
    "struct FU { float4 texScale[2]; float4 c0[9]; float4 c1[9]; float4 fogColor; float alphaRef; };\n"
    "fragment float4 fmain(V2F in_ [[stage_in]], constant FU &fu [[buffer(0)]]%@) {\n"
    "  float4 vD0 = in_.vD0, vD1 = in_.vD1, vT0 = in_.vT0, vT1 = in_.vT1, vT2 = in_.vT2, vT3 = in_.vT3;\n"
    "  float vFogF = in_.vFogF;\n"
    "  constant float4 *c0 = fu.c0; constant float4 *c1 = fu.c1;\n"
    "  float4 fogColor = fu.fogColor; float alphaRef = fu.alphaRef;\n"
    "  float2 texScale[4] = { fu.texScale[0].xy, fu.texScale[0].zw, fu.texScale[1].xy, fu.texScale[1].zw };\n"
    "  float4 fragColor;\n";

typedef struct FU {
    float tex_scale[2][4];
    float c0[9][4], c1[9][4];
    float fog_color[4];
    float alpha_ref, pad[3];
} FU;

typedef struct VU {
    float surface[2], zmax, pad;
    float fog[4];
} VU;

static id<MTLFunction> compile(NSString *src, NSString *name)
{
    NSError *err = nil;
    MTLCompileOptions *o = [MTLCompileOptions new];
    if (@available(macOS 15.0, iOS 18.0, tvOS 18.0, *)) o.mathMode = MTLMathModeSafe;   // the shaders test for infinities
    else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        o.fastMathEnabled = NO;
#pragma clang diagnostic pop
    }
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:o error:&err];
    if (!lib) {
        hle_log("gpu: shader did not compile:\n%s\n%s\n", err.localizedDescription.UTF8String, src.UTF8String);
        return nil;
    }
    return [lib newFunctionWithName:name];
}

static gprog *program_for(const pg_state *st, const uint8_t kinds[4])
{
    prog_key k;
    memset(&k, 0, sizeof k);
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
        case 0x2601: k.fog_mode = 1; break;
        case 0x800: k.fog_mode = 2; break;
        case 0x801: k.fog_mode = 3; break;
        case 0x802: k.fog_mode = 4; break;
        case 0x803: k.fog_mode = 5; break;
        case 0x804: k.fog_mode = 6; break;
        default: k.fog_mode = 1; break;
        }
    }
    k.flat = pg_m(st, M_SHADE_MODE) == 0x1d00;
    uint64_t h = hash_mem((const uint8_t *)&k, sizeof k);
    gprog **bucket = &progs[h % PROG_BUCKETS];
    for (gprog *p = *bucket; p; p = p->next)
        if (!memcmp(&p->key, &k, sizeof k)) return p->vs && p->fs ? p : NULL;

    gprog *p = calloc(1, sizeof *p);
    p->key = k;
    p->next = *bucket;
    *bucket = p;   // kept even when it fails, so it is not tried again each draw
    if (fixed ? !vsh_ff_translate(st, vsh_body, sizeof vsh_body)
              : !vsh_translate((const uint32_t(*)[4])st->program, start, vsh_body, sizeof vsh_body)) return NULL;
    psh_translate(&k.psh, psh_body, sizeof psh_body);
    const char *fl = k.flat ? "[[flat]]" : "";
    static const char *const fog[7] = {
        "  vFogF = 1.0;\n",
        "  vFogF = fogParams.x + oFog.x * fogParams.y - 1.0;\n",
        "  vFogF = fogParams.x + exp2(oFog.x * fogParams.y * 16.0) - 1.5;\n",
        "  vFogF = fogParams.x + exp2(-oFog.x * oFog.x * fogParams.y * fogParams.y * 32.0) - 1.5;\n",
        "  vFogF = abs(fogParams.x + exp2(oFog.x * fogParams.y * 16.0) - 1.5);\n",
        "  vFogF = abs(fogParams.x + exp2(-oFog.x * oFog.x * fogParams.y * fogParams.y * 32.0) - 1.5);\n",
        "  vFogF = abs(fogParams.x + oFog.x * fogParams.y - 1.0);\n",
    };
    NSMutableString *vs = [NSMutableString stringWithUTF8String:msl_common];
    [vs appendFormat:@(vs_head), fl, fl];
    [vs appendString:@(vsh_body)];
    [vs appendString:@(fog[k.fog_mode])];
    [vs appendString:@(vs_tail)];
    id<MTLFunction> vfn = compile(vs, @"vmain");

    // the fragment shader: the same V2F, the textures it samples, and
    // texture(texN, ...) as Metal's sample()
    static const char *const ttype[5] = { NULL, "texture2d<float>", "texture2d<float>", "texturecube<float>", "texture3d<float>" };
    NSMutableString *args = [NSMutableString string];
    for (int i = 0; i < 4; i++)
        if (kinds[i]) [args appendFormat:@", %s tex%d [[texture(%d)]], sampler tex%d_s [[sampler(%d)]]", ttype[kinds[i]], i, i, i, i];
    NSMutableString *fs = [NSMutableString stringWithUTF8String:msl_common];
    [fs appendFormat:@"struct V2F { float4 pos [[position]]; float4 vD0 %s; float4 vD1 %s;\n"
                      "  float4 vT0; float4 vT1; float4 vT2; float4 vT3; float vFogF; };\n", fl, fl];
    [fs appendFormat:@(fs_head), args];
    // after fmain's signature, which uses Metal's own [[texture(n)]]
    [fs appendString:@"#define texture(t, c) t.sample(t##_s, c)\n#define discard discard_fragment()\n"];
    [fs appendString:@(psh_body)];
    [fs appendString:@"  return fragColor;\n}\n"];
    id<MTLFunction> ffn = compile(fs, @"fmain");
    // held by the array before the unretained struct fields point at them
    if (vfn) [functions addObject:vfn];
    if (ffn) [functions addObject:ffn];
    p->vs = vfn, p->fs = ffn;
    if (hle_verbose > 1) hle_log("gpu: new program (%u vertex instructions)\n", end - start + 1);
    return p->vs && p->fs ? p : NULL;
}

// ---- pipelines and render state ---------------------------------------------------------------------------
static MTLBlendFactor blend_factor(uint32_t gl)
{
    switch (gl) {
    case 0: return MTLBlendFactorZero;
    case 1: return MTLBlendFactorOne;
    case 0x300: return MTLBlendFactorSourceColor;
    case 0x301: return MTLBlendFactorOneMinusSourceColor;
    case 0x302: return MTLBlendFactorSourceAlpha;
    case 0x303: return MTLBlendFactorOneMinusSourceAlpha;
    case 0x304: return MTLBlendFactorDestinationAlpha;
    case 0x305: return MTLBlendFactorOneMinusDestinationAlpha;
    case 0x306: return MTLBlendFactorDestinationColor;
    case 0x307: return MTLBlendFactorOneMinusDestinationColor;
    case 0x308: return MTLBlendFactorSourceAlphaSaturated;
    case 0x8001: return MTLBlendFactorBlendColor;
    case 0x8002: return MTLBlendFactorOneMinusBlendColor;
    case 0x8003: return MTLBlendFactorBlendAlpha;
    case 0x8004: return MTLBlendFactorOneMinusBlendAlpha;
    }
    return MTLBlendFactorOne;
}

static MTLBlendOperation blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800a: return MTLBlendOperationSubtract;          // GL_FUNC_SUBTRACT
    case 0x800b: case 0xf005: return MTLBlendOperationReverseSubtract;
    case 0x8007: return MTLBlendOperationMin;
    case 0x8008: return MTLBlendOperationMax;
    }
    return MTLBlendOperationAdd;
}

static MTLCompareFunction compare_of(uint32_t gl, MTLCompareFunction dflt)
{
    if (gl < 0x200 || gl > 0x207) return dflt;
    static const MTLCompareFunction f[8] = { MTLCompareFunctionNever, MTLCompareFunctionLess, MTLCompareFunctionEqual,
                                             MTLCompareFunctionLessEqual, MTLCompareFunctionGreater,
                                             MTLCompareFunctionNotEqual, MTLCompareFunctionGreaterEqual,
                                             MTLCompareFunctionAlways };
    return f[gl - 0x200];
}

static MTLStencilOperation stencil_op(uint32_t gl)
{
    switch (gl) {
    case 0: return MTLStencilOperationZero;
    case 0x1e01: return MTLStencilOperationReplace;
    case 0x1e02: return MTLStencilOperationIncrementClamp;
    case 0x1e03: return MTLStencilOperationDecrementClamp;
    case 0x150a: return MTLStencilOperationInvert;
    case 0x8507: return MTLStencilOperationIncrementWrap;
    case 0x8508: return MTLStencilOperationDecrementWrap;
    }
    return MTLStencilOperationKeep;
}

typedef struct pipe_key {
    void *vs, *fs;
    uint32_t blend, sf, df, eq, mask, color, zeta;
} pipe_key;
static NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *pipes;
static NSMutableDictionary<NSData *, id<MTLDepthStencilState>> *depth_states;

static id<MTLRenderPipelineState> pipeline(id<MTLFunction> vs, id<MTLFunction> fs, int blend, uint32_t sf, uint32_t df,
                                           uint32_t eq, uint32_t mask)
{
    pipe_key k = { (__bridge void *)vs, (__bridge void *)fs, (uint32_t)blend, blend ? sf : 0, blend ? df : 0,
                   blend ? eq : 0, mask, cur_color != NULL, cur_zeta != NULL };
    NSData *key = [NSData dataWithBytes:&k length:sizeof k];
    id<MTLRenderPipelineState> ps = pipes[key];
    if (ps) return ps;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vs;
    d.fragmentFunction = fs;
    if (cur_color) {
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[0];
        a.pixelFormat = MTLPixelFormatRGBA8Unorm;
        a.blendingEnabled = blend != 0;
        if (blend) {
            a.sourceRGBBlendFactor = a.sourceAlphaBlendFactor = blend_factor(sf);
            a.destinationRGBBlendFactor = a.destinationAlphaBlendFactor = blend_factor(df);
            a.rgbBlendOperation = a.alphaBlendOperation = blend_op(eq);
        }
        a.writeMask = mask;
    }
    if (cur_zeta) {
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    }
    NSError *err = nil;
    ps = [dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!ps) {
        hle_log("gpu: no pipeline: %s\n", err.localizedDescription.UTF8String);
        return nil;
    }
    pipes[key] = ps;
    return ps;
}

static id<MTLDepthStencilState> depth_state(int test, uint32_t func, int write, int stencil, uint32_t sfunc, uint32_t ref_mask,
                                            uint32_t fail, uint32_t zfail, uint32_t zpass, uint32_t wmask)
{
    uint32_t k[10] = { (uint32_t)test, func, (uint32_t)write, (uint32_t)stencil, sfunc, ref_mask, fail, zfail, zpass, wmask };
    if (!stencil) k[4] = k[5] = k[6] = k[7] = k[8] = k[9] = 0;
    NSData *key = [NSData dataWithBytes:k length:sizeof k];
    id<MTLDepthStencilState> ds = depth_states[key];
    if (ds) return ds;
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction = test ? compare_of(func, MTLCompareFunctionLess) : MTLCompareFunctionAlways;
    d.depthWriteEnabled = write != 0;
    if (stencil) {
        MTLStencilDescriptor *s = [MTLStencilDescriptor new];
        s.stencilCompareFunction = compare_of(sfunc, MTLCompareFunctionAlways);
        s.readMask = ref_mask;
        s.writeMask = wmask;
        s.stencilFailureOperation = stencil_op(fail);
        s.depthFailureOperation = stencil_op(zfail);
        s.depthStencilPassOperation = stencil_op(zpass);
        d.frontFaceStencil = s;
        d.backFaceStencil = s;
    }
    ds = [dev newDepthStencilStateWithDescriptor:d];
    depth_states[key] = ds;
    return ds;
}

static void argb(uint32_t v, float *o)
{
    o[0] = (v >> 16 & 0xff) / 255.0f, o[1] = (v >> 8 & 0xff) / 255.0f, o[2] = (v & 0xff) / 255.0f, o[3] = (v >> 24) / 255.0f;
}

static void set_scissor(int x0, int y0, int x1, int y1)
{
    uint32_t w = cur_color ? cur_color->w : cur_zeta->w, h = cur_color ? cur_color->h : cur_zeta->h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)w - 1) x1 = (int)w - 1;
    if (y1 > (int)h - 1) y1 = (int)h - 1;
    if (x1 < x0 || y1 < y0) x0 = y0 = 0, x1 = y1 = -1;   // nothing: a 0x0 rectangle
    MTLScissorRect r = { (NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0 + 1), (NSUInteger)(y1 - y0 + 1) };
    [enc setScissorRect:r];
}

static void apply_state(const pg_state *st, gprog *p)
{
    uint32_t cm = pg_m(st, M_COLOR_MASK);
    uint32_t mask = ((cm >> 16 & 0xff) ? MTLColorWriteMaskRed : 0) | ((cm >> 8 & 0xff) ? MTLColorWriteMaskGreen : 0) |
                    ((cm & 0xff) ? MTLColorWriteMaskBlue : 0) | ((cm >> 24) ? MTLColorWriteMaskAlpha : 0);
    uint32_t eq = pg_m(st, M_BLEND_EQUATION);
    id<MTLRenderPipelineState> ps = pipeline(p->vs, p->fs, pg_m(st, M_BLEND_ENABLE) != 0, pg_m(st, M_BLEND_SFACTOR),
                                             pg_m(st, M_BLEND_DFACTOR), eq, mask);
    if (ps) [enc setRenderPipelineState:ps];
    float bc[4];
    argb(pg_m(st, M_BLEND_COLOR), bc);
    [enc setBlendColorRed:bc[0] green:bc[1] blue:bc[2] alpha:bc[3]];
    int ztest = cur_zeta && pg_m(st, M_DEPTH_TEST_ENABLE);
    int stest = cur_zeta && pg_m(st, M_STENCIL_TEST_ENABLE);
    [enc setDepthStencilState:depth_state(ztest, pg_m(st, M_DEPTH_FUNC), cur_zeta && pg_m(st, M_DEPTH_MASK), stest,
                                          pg_m(st, M_STENCIL_FUNC), pg_m(st, M_STENCIL_FUNC_MASK) & 0xff,
                                          pg_m(st, M_STENCIL_OP_FAIL), pg_m(st, M_STENCIL_OP_ZFAIL),
                                          pg_m(st, M_STENCIL_OP_ZPASS), pg_m(st, M_STENCIL_MASK) & 0xff)];
    if (stest) [enc setStencilReferenceValue:pg_m(st, M_STENCIL_REF) & 0xff];
    if (pg_m(st, M_CULL_FACE_ENABLE)) {
        uint32_t cf = pg_m(st, M_CULL_FACE);
        [enc setCullMode:cf == 0x404 ? MTLCullModeFront : MTLCullModeBack];   // GL_FRONT; front-and-back: back only
        // not mirrored (see the top): the NV2A's winding as it is (0x900 = CW)
        [enc setFrontFacingWinding:pg_m(st, M_FRONT_FACE) == 0x900 ? MTLWindingClockwise : MTLWindingCounterClockwise];
    } else {
        [enc setCullMode:MTLCullModeNone];
    }
    if (pg_m(st, M_POLY_OFFSET_FILL_ENABLE))
        [enc setDepthBias:u2f(pg_m(st, M_POLYGON_OFFSET_BIAS)) / 16777215.0f
               slopeScale:u2f(pg_m(st, M_POLYGON_OFFSET_SCALE))
                    clamp:0];
    else
        [enc setDepthBias:0 slopeScale:0 clamp:0];
    [enc setTriangleFillMode:pg_m(st, M_FRONT_POLYGON_MODE) == 0x1b01 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
    uint32_t wh = pg_m(st, M_WINDOW_CLIP_H), wv = pg_m(st, M_WINDOW_CLIP_V);
    int x0 = wh & 0xfff, x1 = wh >> 16 & 0xfff, y0 = wv & 0xfff, y1 = wv >> 16 & 0xfff;
    if (x1 >= x0 && y1 >= y0 && (wh || wv)) set_scissor(x0, y0, x1, y1);
    else set_scissor(0, 0, 4095, 4095);
}

// ---- clears ---------------------------------------------------------------------------------------------
// A clear is a rectangle drawn with the clear colour and depth: a clear
// rectangle need not be the whole surface, and a pass is already open.
static id<MTLFunction> clear_vs, clear_fs;

static const char clear_src[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct CO { float4 pos [[position]]; };\n"
    "struct CU { float4 color; float depth; };\n"
    "vertex CO clear_v(uint vid [[vertex_id]], constant CU &u [[buffer(0)]]) {\n"
    "  float2 p = float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0);\n"
    "  CO o; o.pos = float4(p, u.depth, 1.0); return o;\n"
    "}\n"
    "fragment float4 clear_f(constant CU &u [[buffer(0)]]) { return u.color; }\n";

void gpu_clear(const pg_state *st, uint32_t flags)
{
    if (!ready || !bind_target(st)) return;
    uint32_t rh = pg_m(st, 0x1d98), rv = pg_m(st, 0x1d9c);
    int color = (flags & 0xf0) && cur_color, depth = (flags & 1) && cur_zeta, stencil = (flags & 2) && cur_zeta;
    if (!color && !depth && !stencil) return;
    struct { float color[4]; float depth, pad[3]; } u;
    argb(pg_m(st, 0x1d90), u.color);
    uint32_t z = pg_m(st, 0x1d8c), sref = 0;
    if (cur_zeta && cur_zeta->fmt == 1) u.depth = (z & 0xffff) / 65535.0f;
    else u.depth = (z >> 8) / 16777215.0f, sref = z & 0xff;
    uint32_t mask = color ? ((flags & 0x10) ? MTLColorWriteMaskRed : 0) | ((flags & 0x20) ? MTLColorWriteMaskGreen : 0) |
                                ((flags & 0x40) ? MTLColorWriteMaskBlue : 0) | ((flags & 0x80) ? MTLColorWriteMaskAlpha : 0)
                          : 0;
    id<MTLRenderPipelineState> ps = pipeline(clear_vs, clear_fs, 0, 0, 0, 0, mask);
    if (!ps) return;
    [enc setRenderPipelineState:ps];
    [enc setDepthStencilState:depth_state(depth, 0x207, depth, stencil, 0x207, 0xff, 0x1e01, 0x1e01, 0x1e01,
                                          stencil ? 0xff : 0)];
    if (stencil) [enc setStencilReferenceValue:sref];
    [enc setCullMode:MTLCullModeNone];
    [enc setDepthBias:0 slopeScale:0 clamp:0];
    [enc setTriangleFillMode:MTLTriangleFillModeFill];
    set_scissor(rh & 0xffff, rv & 0xffff, rh >> 16, rv >> 16);
    [enc setVertexBytes:&u length:sizeof u atIndex:0];
    [enc setFragmentBytes:&u length:sizeof u atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}

// ---- draws ---------------------------------------------------------------------------------------------
static uint32_t *idx;
static uint32_t idx_cap;

static void need_idx(uint32_t n)
{
    if (n > idx_cap) {
        idx_cap = n * 2;
        idx = realloc(idx, idx_cap * 4);
    }
}

void gpu_draw(const pg_state *st, int prim, const pg_vertex *v, uint32_t n)
{
    if (!ready || !n || !bind_target(st)) return;
    uint8_t kinds[4] = { 0 };
    FU fu;
    memset(&fu, 0, sizeof fu);
    id<MTLTexture> mts[4] = { nil, nil, nil, nil };
    id<MTLSamplerState> smps[4] = { nil, nil, nil, nil };
    for (int i = 0; i < 4; i++) {
        float scale[2] = { 1, 1 };
        int mode = pg_m(st, M_SHADER_STAGE_PROGRAM) >> (5 * i) & 0x1f;
        if (!(mode == 0 || mode == 4 || mode == 5)) kinds[i] = prepare_texture(st, i, scale, &mts[i], &smps[i]);
        fu.tex_scale[i / 2][(i & 1) * 2] = scale[0];
        fu.tex_scale[i / 2][(i & 1) * 2 + 1] = scale[1];
    }
    gprog *p = program_for(st, kinds);
    if (!p || !bind_target(st)) return;   // the pass again, if an upload ended it
    for (int i = 0; i < 4; i++)
        if (kinds[i]) {
            [enc setFragmentTexture:mts[i] atIndex:i];
            [enc setFragmentSamplerState:smps[i] atIndex:i];
        }
    apply_state(st, p);

    VU vu;
    vu.surface[0] = (float)cur_w, vu.surface[1] = (float)cur_h;
    float zmax = u2f(pg_m(st, M_CLIP_MAX));
    vu.zmax = zmax > 0 ? zmax : 16777215.0f;
    vu.pad = 0;
    vu.fog[0] = u2f(pg_m(st, M_FOG_PARAMS)), vu.fog[1] = u2f(pg_m(st, M_FOG_PARAMS + 4));
    vu.fog[2] = u2f(pg_m(st, M_FOG_PARAMS + 8)), vu.fog[3] = 0;
    [enc setVertexBytes:&st->constants[0][0] length:PG_CONSTANTS * 16 atIndex:1];
    float ff[VSH_FF_UNIFORMS][4];
    vsh_ff_uniforms(st, ff);
    [enc setVertexBytes:ff length:sizeof ff atIndex:2];
    [enc setVertexBytes:&vu length:sizeof vu atIndex:3];
    for (int i = 0; i < 8; i++) {
        argb(pg_m(st, M_COMBINER_FACTOR0 + 4 * i), fu.c0[i]);
        argb(pg_m(st, M_COMBINER_FACTOR1 + 4 * i), fu.c1[i]);
    }
    argb(pg_m(st, 0x1e20), fu.c0[8]);
    argb(pg_m(st, 0x1e24), fu.c1[8]);
    uint32_t f = pg_m(st, M_FOG_COLOR);
    fu.fog_color[0] = (f & 0xff) / 255.0f, fu.fog_color[1] = (f >> 8 & 0xff) / 255.0f;
    fu.fog_color[2] = (f >> 16 & 0xff) / 255.0f, fu.fog_color[3] = (f >> 24) / 255.0f;
    fu.alpha_ref = (float)(pg_m(st, M_ALPHA_REF) & 0xff);
    [enc setFragmentBytes:&fu length:sizeof fu atIndex:0];

    uint32_t off;
    id<MTLBuffer> vb = ring_alloc(v, n * (uint32_t)sizeof *v, &off);
    if (!vb) {
        hle_log("gpu: vertex buffer full this frame\n");
        return;
    }
    [enc setVertexBuffer:vb offset:off atIndex:0];

    MTLPrimitiveType mode = MTLPrimitiveTypeTriangle;
    uint32_t ni = 0;
    switch (prim) {
    case PRIM_POINTS: mode = MTLPrimitiveTypePoint; break;
    case PRIM_LINES: mode = MTLPrimitiveTypeLine; break;
    case PRIM_LINE_STRIP: mode = MTLPrimitiveTypeLineStrip; break;
    case PRIM_LINE_LOOP:
        mode = MTLPrimitiveTypeLineStrip;
        need_idx(n + 1);
        for (uint32_t i = 0; i < n; i++) idx[ni++] = i;
        idx[ni++] = 0;
        break;
    case PRIM_TRIANGLES: mode = MTLPrimitiveTypeTriangle; break;
    case PRIM_TRIANGLE_STRIP: mode = MTLPrimitiveTypeTriangleStrip; break;
    case PRIM_QUADS:
        need_idx(n / 4 * 6);
        for (uint32_t q = 0; q + 3 < n; q += 4) {
            idx[ni++] = q, idx[ni++] = q + 1, idx[ni++] = q + 2;
            idx[ni++] = q, idx[ni++] = q + 2, idx[ni++] = q + 3;
        }
        break;
    case PRIM_QUAD_STRIP:
        need_idx(n * 3);
        for (uint32_t q = 0; q + 3 < n; q += 2) {
            idx[ni++] = q, idx[ni++] = q + 1, idx[ni++] = q + 3;
            idx[ni++] = q, idx[ni++] = q + 3, idx[ni++] = q + 2;
        }
        break;
    default:   // a fan or a polygon: triangles around vertex 0
        need_idx(n * 3);
        for (uint32_t i = 1; i + 1 < n; i++) idx[ni++] = 0, idx[ni++] = i, idx[ni++] = i + 1;
        break;
    }
    if (ni) {
        uint32_t ioff;
        id<MTLBuffer> ib = ring_alloc(idx, ni * 4, &ioff);
        if (!ib) return;
        [enc drawIndexedPrimitives:mode indexCount:ni indexType:MTLIndexTypeUInt32 indexBuffer:ib indexBufferOffset:ioff];
    } else if (prim == PRIM_TRIANGLE_FAN || prim == PRIM_POLYGON || prim == PRIM_LINE_LOOP) {
        return;   // too few vertices for one
    } else {
        [enc drawPrimitives:mode vertexStart:0 vertexCount:n];
    }
}

// ---- frames -------------------------------------------------------------------------------------------
static id<MTLRenderPipelineState> show_pipe;
static id<MTLSamplerState> show_sampler;

static const char show_src[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct SO { float4 pos [[position]]; float2 uv; };\n"
    "vertex SO show_v(uint vid [[vertex_id]], constant float4 &r [[buffer(0)]]) {\n"
    "  float2 c = float2((vid & 1) ? 1.0 : 0.0, (vid & 2) ? 1.0 : 0.0);\n"
    "  SO o; o.pos = float4(mix(r.xy, r.zw, c), 0.0, 1.0); o.uv = float2(c.x, 1.0 - c.y); return o;\n"
    "}\n"
    "fragment float4 show_f(SO in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  return float4(t.sample(s, in.uv).rgb, 1.0);\n"
    "}\n";

static void screenshot(surface *s)
{
    // a shared copy of the surface, read when this frame's work is done
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:s->w
                                                                                 height:s->h
                                                                              mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> copy = [dev newTextureWithDescriptor:d];
    end_pass();
    id<MTLBlitCommandEncoder> bl = [cmd blitCommandEncoder];
    [bl copyFromTexture:s->tex toTexture:copy];
    [bl endEncoding];
    char path[512];
    snprintf(path, sizeof path, "%s/frame%05u.ppm", shot_dir, frame_no);
    NSString *where = @(path);
    uint32_t w = s->w, h = s->h;
    [cmd addCompletedHandler:^(id<MTLCommandBuffer> b) {
        (void)b;
        uint8_t *px = malloc((size_t)w * h * 4);
        [copy getBytes:px bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
        FILE *f = fopen(where.UTF8String, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", w, h);
            for (uint32_t i = 0; i < w * h; i++) fwrite(px + 4 * i, 1, 3, f);
            fclose(f);
        }
        free(px);
    }];
}

// the core library: the frame as 0xAARRGGBB words for gpu_present, once the GPU is done
static id<MTLTexture> out_tex;
static id<MTLBuffer> out_buf;

static void present_offscreen(surface *s)
{
    if (!out_tex || out_tex.width != s->w || out_tex.height != s->h) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                     width:s->w
                                                                                    height:s->h
                                                                                 mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget;
        d.storageMode = MTLStorageModePrivate;
        out_tex = [dev newTextureWithDescriptor:d];
        out_buf = [dev newBufferWithLength:(NSUInteger)s->w * s->h * 4 options:MTLResourceStorageModeShared];
    }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = out_tex;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cmd renderCommandEncoderWithDescriptor:rp];
    float r[4] = { -1, -1, 1, 1 };
    [e setRenderPipelineState:show_pipe];
    [e setVertexBytes:r length:sizeof r atIndex:0];
    [e setFragmentTexture:s->tex atIndex:0];
    [e setFragmentSamplerState:show_sampler atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [e endEncoding];
    id<MTLBlitCommandEncoder> bl = [cmd blitCommandEncoder];
    [bl copyFromTexture:out_tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(s->w, s->h, 1) toBuffer:out_buf destinationOffset:0
    destinationBytesPerRow:s->w * 4 destinationBytesPerImage:(NSUInteger)s->w * s->h * 4];
    [bl endEncoding];
}

void gpu_flip(const pg_state *st)
{
    ch_perf_frame();
    frame_no++;
    if (hle_verbose > 0 && frame_no % 60 == 0) hle_log("gpu: frame %u\n", frame_no);
    if (!ready) return;
    surface *s = surface_find_addr(phys_of(st->dma_color + pg_m(st, M_SURFACE_COLOR_OFFSET)), 0);
    need_cmd();
    end_pass();
    if (s && shot_dir && frame_no % shot_every == 0) screenshot(s);
    if (s && gpu_offscreen) present_offscreen(s);
    id<CAMetalDrawable> drawable = s && layer && !gpu_offscreen ? [layer nextDrawable] : nil;
    if (drawable) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = drawable.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cmd renderCommandEncoderWithDescriptor:rp];
        // keep the aspect: the picture's rectangle in normalized coordinates
        double dw = drawable.texture.width, dh = drawable.texture.height;
        double sx = 1, sy = 1;
        if (dw * s->h > dh * s->w) sx = (dh * s->w / s->h) / dw;
        else sy = (dw * s->h / s->w) / dh;
        float r[4] = { (float)-sx, (float)-sy, (float)sx, (float)sy };
        [e setRenderPipelineState:show_pipe];
        [e setVertexBytes:r length:sizeof r atIndex:0];
        [e setFragmentTexture:s->tex atIndex:0];
        [e setFragmentSamplerState:show_sampler atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e endEncoding];
        [cmd presentDrawable:drawable];
    }
    [cmd commit];
    if (s && gpu_offscreen) {
        [cmd waitUntilCompleted];
        uint32_t *px = out_buf.contents;
        for (size_t i = 0, n = (size_t)s->w * s->h; i < n; i++) px[i] |= 0xff000000u;
        if (gpu_present) gpu_present(px, (int)s->w, (int)s->h);
    }
    cmd = nil;
    pass_color = pass_zeta = NULL;
    if (frame_pool) objc_autoreleasePoolPop(frame_pool);
    frame_pool = NULL;
}

void gpu_flush_surfaces(void) {}

// ---- start ----------------------------------------------------------------------------------------------
int gpu_init(void)
{
    if (af_debug_env("CH_NOGPU")) return 0;
    @autoreleasepool {
        dev = MTLCreateSystemDefaultDevice();
        if (!dev) {
            hle_log("gpu: no Metal device: nothing will be drawn\n");
            return 0;
        }
        queue = [dev newCommandQueue];
        in_flight = dispatch_semaphore_create(RING);
        for (int i = 0; i < RING; i++) ring[i] = [dev newBufferWithLength:RING_BYTES options:MTLResourceStorageModeShared];
        owned = [NSMutableSet set];
        functions = [NSMutableArray array];
        samplers = [NSMutableDictionary dictionary];
        pipes = [NSMutableDictionary dictionary];
        depth_states = [NSMutableDictionary dictionary];
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@(clear_src) options:nil error:&err];
        clear_vs = [lib newFunctionWithName:@"clear_v"];
        clear_fs = [lib newFunctionWithName:@"clear_f"];
        [functions addObject:clear_vs];
        [functions addObject:clear_fs];
        if (layer || gpu_offscreen) {
            if (layer && !gpu_offscreen) {
                layer.device = dev;
                layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
                layer.framebufferOnly = YES;
            }
            id<MTLLibrary> slib = [dev newLibraryWithSource:@(show_src) options:nil error:&err];
            MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
            d.vertexFunction = [slib newFunctionWithName:@"show_v"];
            d.fragmentFunction = [slib newFunctionWithName:@"show_f"];
            d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            show_pipe = [dev newRenderPipelineStateWithDescriptor:d error:&err];
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
            show_sampler = [dev newSamplerStateWithDescriptor:sd];
        }
        hle_log("gpu: Metal on %s\n", dev.name.UTF8String);
    }
    shot_dir = af_debug_env("CH_SHOT");
    shot_every = af_debug_env("CH_SHOT_EVERY") ? atoi(af_debug_env("CH_SHOT_EVERY")) : 60;
    if (!shot_every) shot_every = 60;
    ready = 1;
    return 1;
}

void gpu_poll_events(void) {}   // the front end reads its window on its own thread

// the GPU thread is ending: the frame in progress is finished, then every
// object the renderer made is let go, for the next start to make afresh
void gpu_shutdown(void)
{
    if (!ready) return;
    // the frame's pool was pushed outside the block below: popped outside it too
    if (frame_pool) objc_autoreleasePoolPop(frame_pool);
    frame_pool = NULL;
    @autoreleasepool {
        if (cmd) {
            end_pass();
            [cmd commit];
            [cmd waitUntilCompleted];
            cmd = nil;
        } else {
            id<MTLCommandBuffer> last = [queue commandBuffer];
            [last commit];
            [last waitUntilCompleted];   // the queue's earlier frames are done too
        }
        for (int i = 0; i < PROG_BUCKETS; i++)
            while (progs[i]) {
                gprog *p = progs[i];
                progs[i] = p->next;
                free(p);
            }
        memset(surfs, 0, sizeof surfs);
        memset(texs, 0, sizeof texs);
        cur_color = cur_zeta = pass_color = pass_zeta = NULL;
        [owned removeAllObjects];
        owned = nil;
        [functions removeAllObjects];
        functions = nil;
        samplers = nil;
        pipes = nil;
        depth_states = nil;
        clear_vs = clear_fs = nil;
        show_pipe = nil;
        show_sampler = nil;
        out_tex = nil;
        out_buf = nil;
        for (int i = 0; i < RING; i++) ring[i] = nil;
        ring_i = ring_used = 0;
        in_flight = nil;
        queue = nil;
        dev = nil;
    }
    frame_no = 0;
    ready = 0;
}
