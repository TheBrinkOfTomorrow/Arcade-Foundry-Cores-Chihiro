// The NV2A's 3D engine (class NV097): the state the pushbuffer sets, and the
// interface to the host renderer that draws with it.
//
// pgraph.c keeps every method's last value in m[] (indexed by method / 4),
// the way PGRAPH keeps its registers, plus the state that is not one value
// per method: the vertex program, its constants, the vertex being built.
// It turns begin/end and the vertex methods into lists of vertices with all
// sixteen attributes as floats, and calls the renderer (gpu_*.c) to clear,
// draw and show.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PG_ATTRS 16
#define PG_PROGRAM_SLOTS 136
#define PG_CONSTANTS 192

// methods (NV097_*) the renderer reads back from m[]
#define M_SURFACE_CLIP_H 0x0200
#define M_SURFACE_CLIP_V 0x0204
#define M_SURFACE_FORMAT 0x0208
#define M_SURFACE_PITCH 0x020c
#define M_SURFACE_COLOR_OFFSET 0x0210
#define M_SURFACE_ZETA_OFFSET 0x0214
#define M_COMBINER_ALPHA_ICW 0x0260
#define M_COMBINER_SPECULAR_FOG_CW0 0x0288
#define M_COMBINER_SPECULAR_FOG_CW1 0x028c
#define M_CONTROL0 0x0290
#define M_FOG_MODE 0x029c
#define M_FOG_GEN_MODE 0x02a0
#define M_FOG_ENABLE 0x02a4
#define M_FOG_COLOR 0x02a8
#define M_WINDOW_CLIP_H 0x02c0
#define M_WINDOW_CLIP_V 0x02e0
#define M_ALPHA_TEST_ENABLE 0x0300
#define M_BLEND_ENABLE 0x0304
#define M_CULL_FACE_ENABLE 0x0308
#define M_DEPTH_TEST_ENABLE 0x030c
#define M_STENCIL_TEST_ENABLE 0x032c
#define M_POLY_OFFSET_FILL_ENABLE 0x0338
#define M_ALPHA_FUNC 0x033c
#define M_ALPHA_REF 0x0340
#define M_BLEND_SFACTOR 0x0344
#define M_BLEND_DFACTOR 0x0348
#define M_BLEND_COLOR 0x034c
#define M_BLEND_EQUATION 0x0350
#define M_DEPTH_FUNC 0x0354
#define M_COLOR_MASK 0x0358
#define M_DEPTH_MASK 0x035c
#define M_STENCIL_MASK 0x0360
#define M_STENCIL_FUNC 0x0364
#define M_STENCIL_REF 0x0368
#define M_STENCIL_FUNC_MASK 0x036c
#define M_STENCIL_OP_FAIL 0x0370
#define M_STENCIL_OP_ZFAIL 0x0374
#define M_STENCIL_OP_ZPASS 0x0378
#define M_SHADE_MODE 0x037c
#define M_POLYGON_OFFSET_SCALE 0x0384
#define M_POLYGON_OFFSET_BIAS 0x0388
#define M_FRONT_POLYGON_MODE 0x038c
#define M_CLIP_MIN 0x0394
#define M_CLIP_MAX 0x0398
#define M_CULL_FACE 0x039c
#define M_FRONT_FACE 0x03a0
#define M_FOG_PARAMS 0x09c0
#define M_COMBINER_FACTOR0 0x0a60
#define M_COMBINER_FACTOR1 0x0a80
#define M_COMBINER_ALPHA_OCW 0x0aa0
#define M_COMBINER_COLOR_ICW 0x0ac0
#define M_VERTEX_DATA_ARRAY_OFFSET 0x1720
#define M_VERTEX_DATA_ARRAY_FORMAT 0x1760
#define M_TEXTURE_OFFSET 0x1b00   // + 0x40 per stage
#define M_TEXTURE_FORMAT 0x1b04
#define M_TEXTURE_ADDRESS 0x1b08
#define M_TEXTURE_CONTROL0 0x1b0c
#define M_TEXTURE_CONTROL1 0x1b10
#define M_TEXTURE_FILTER 0x1b14
#define M_TEXTURE_IMAGE_RECT 0x1b1c
#define M_TEXTURE_PALETTE 0x1b20
#define M_TEXTURE_BORDER_COLOR 0x1b24
#define M_COMBINER_COLOR_OCW 0x1e40
#define M_COMBINER_CONTROL 0x1e60
#define M_SHADER_STAGE_PROGRAM 0x1e70
#define M_SHADER_OTHER_STAGE_INPUT 0x1e78
#define M_TRANSFORM_EXECUTION_MODE 0x1e94

// primitives (SET_BEGIN_END)
enum { PRIM_END, PRIM_POINTS, PRIM_LINES, PRIM_LINE_LOOP, PRIM_LINE_STRIP, PRIM_TRIANGLES,
       PRIM_TRIANGLE_STRIP, PRIM_TRIANGLE_FAN, PRIM_QUADS, PRIM_QUAD_STRIP, PRIM_POLYGON };

typedef struct pg_vertex {
    float a[PG_ATTRS][4];
} pg_vertex;

// a texture as the renderer needs it, read from the state of one stage
typedef struct pg_texture {
    uint32_t addr;            // physical
    uint32_t format;          // SET_TEXTURE_FORMAT
    uint32_t color;           // its colour format
    uint32_t width, height, depth, levels, pitch;
    int swizzled, compressed, cube, linear;   // linear: texel coordinates (rectangle)
    uint32_t palette;         // physical, for I8
    uint32_t palette_len;
} pg_texture;

typedef struct pg_state {
    uint32_t m[0x2000 / 4];                  // every method's last value
    uint32_t program[PG_PROGRAM_SLOTS][4];   // vertex program
    float constants[PG_CONSTANTS][4];        // its constants
    float attr_const[PG_ATTRS][4];           // attribute values when an attribute is not in an array
    uint32_t dma_a, dma_b, dma_color, dma_zeta, dma_vertex_a, dma_vertex_b;   // physical bases
    uint32_t draw_vary;   // for gpu_draw: the attributes that differ between its vertices (bit i: attribute i)
} pg_state;

static inline uint32_t pg_m(const pg_state *s, uint32_t method) { return s->m[(method & 0x1ffc) / 4]; }

// read a stage's texture state (pgraph.c)
int pg_texture_of(const pg_state *s, int stage, pg_texture *t);

// ---- the renderer --------------------------------------------------------------------------------
int gpu_init(void);                       // on the GPU thread: 0 if no renderer
void gpu_clear(const pg_state *s, uint32_t flags);   // CLEAR_SURFACE
void gpu_draw(const pg_state *s, int prim, const pg_vertex *v, uint32_t n);
void gpu_flip(const pg_state *s);         // the surface just drawn is the next frame
void gpu_flush_surfaces(void);            // write rendered surfaces back to RAM
void gpu_poll_events(void);
void gpu_shutdown(void);                  // on the GPU thread as it ends: everything the renderer made is freed
// the core library's front end (runtime/core_api.c): each frame shown, as
// 0xAARRGGBB words, top row first; NULL in chihiro-run
extern void (*gpu_present)(const uint32_t *px, int w, int h);
extern int gpu_offscreen;                 // no window: frames go to gpu_present

// ---- shader translators -------------------------------------------------------------------------
// vsh.c: an NV2A vertex program (from slot start to its final instruction)
// as the body of a GLSL main(); returns its length in instructions, 0 if none
int vsh_translate(const uint32_t (*program)[4], int start, char *out, int size);
// vsh_ff.c: the fixed-function vertex stage as the same kind of body (1 if it fit),
// the state it depends on, and the light and material values it reads as ff[]
#define VSH_FF_UNIFORMS 84
#define VSH_FF_KEY 32
int vsh_ff_translate(const pg_state *s, char *out, int size);
void vsh_ff_key(const pg_state *s, uint32_t key[VSH_FF_KEY]);
void vsh_ff_uniforms(const pg_state *s, float ff[VSH_FF_UNIFORMS][4]);
// psh.c: the register combiners and texture stages as a GLSL fragment shader body
typedef struct psh_key {
    uint32_t combiner_control, stage_program, other_input, final0, final1;
    uint32_t color_icw[8], color_ocw[8], alpha_icw[8], alpha_ocw[8];
    uint8_t tex_kind[4];      // 0 none, 1 2D, 2 rectangle (texel coordinates), 3 cube, 4 3D
    uint8_t alpha_test, alpha_func, fog, fog_mode;
} psh_key;
void psh_key_of(const pg_state *s, psh_key *k, const uint8_t tex_kind[4]);
int psh_translate(const psh_key *k, char *out, int size);

// tex.c: a texture decoded to RGBA8 (R in the lowest byte), width*height*depth*(6 if cube) texels
void tex_decode(const pg_texture *t, const uint8_t *ram, uint32_t *out);
uint32_t tex_bytes(const pg_texture *t);  // bytes the texture takes in RAM (level 0, all faces)

#ifdef __cplusplus
}
#endif
