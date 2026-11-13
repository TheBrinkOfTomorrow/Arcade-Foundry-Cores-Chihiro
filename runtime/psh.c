// The NV2A's pixel pipeline to GLSL: four texture stages, up to eight general
// register combiner stages and the final combiner.
//
// Combiner registers: 0 zero, 1 c0, 2 c1, 3 fog, 4 v0 (diffuse), 5 v1
// (specular), 8-11 t0-t3, 12 r0, 13 r1; the final combiner adds 14 (v1 +
// r0) and 15 (E * F). An input byte is the register (bits 0-3), alpha or
// not (bit 4) and a mapping (bits 5-7). A stage computes A*B (or A.B),
// C*D (or C.D) and their sum (or a mux on r0.a), scales and biases them,
// clamps to [-1, 1] and writes up to three registers; its RGB and alpha
// halves read the registers as they were before the stage.
//
// The generated body reads vD0 vD1 vT0-vT3 vFogF (the fog factor), the
// samplers tex0-tex3 with texScale[], c0[9] c1[9] (index 8 the final
// combiner's), fogColor and alphaRef, and writes fragColor.
#include "pgraph.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct { char *p; int left; } buf;
static void put(buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->p, b->left > 0 ? b->left : 0, fmt, ap);
    va_end(ap);
    if (n > 0 && n < b->left) b->p += n, b->left -= n;
    else b->left = 0;
}

void psh_key_of(const pg_state *s, psh_key *k, const uint8_t tex_kind[4])
{
    memset(k, 0, sizeof *k);
    k->combiner_control = pg_m(s, M_COMBINER_CONTROL);
    k->stage_program = pg_m(s, M_SHADER_STAGE_PROGRAM);
    k->other_input = pg_m(s, M_SHADER_OTHER_STAGE_INPUT);
    k->final0 = pg_m(s, M_COMBINER_SPECULAR_FOG_CW0);
    k->final1 = pg_m(s, M_COMBINER_SPECULAR_FOG_CW1);
    int n = k->combiner_control & 0xff;
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) {
        k->color_icw[i] = pg_m(s, M_COMBINER_COLOR_ICW + 4 * i);
        k->color_ocw[i] = pg_m(s, M_COMBINER_COLOR_OCW + 4 * i);
        k->alpha_icw[i] = pg_m(s, M_COMBINER_ALPHA_ICW + 4 * i);
        k->alpha_ocw[i] = pg_m(s, M_COMBINER_ALPHA_OCW + 4 * i);
    }
    memcpy(k->tex_kind, tex_kind, 4);
    k->alpha_test = pg_m(s, M_ALPHA_TEST_ENABLE) != 0;
    k->alpha_func = k->alpha_test ? pg_m(s, M_ALPHA_FUNC) & 7 : 0;
}

static const char *reg_name(int r, int stage, const psh_key *k)
{
    static char s[16];
    switch (r) {
    case 1:
        snprintf(s, sizeof s, "c0[%d]", stage == 8 || (k->combiner_control & 0x1000) ? stage : 0);
        return s;
    case 2:
        snprintf(s, sizeof s, "c1[%d]", stage == 8 || (k->combiner_control & 0x10000) ? stage : 0);
        return s;
    case 3: return "fog";
    case 4: return "v0";
    case 5: return "v1";
    case 8: return "t0";
    case 9: return "t1";
    case 10: return "t2";
    case 11: return "t3";
    case 12: return "r0";
    case 13: return "r1";
    case 14: return "v1r0";
    case 15: return "efprod";
    }
    return "zero";
}

// an input: the register's RGB, alpha or blue, through its mapping
static void input(char *out, uint32_t byte, int alpha_half, int stage, const psh_key *k)
{
    const char *r = reg_name(byte & 15, stage, k);
    int use_alpha = byte >> 4 & 1, map = byte >> 5 & 7;
    char x[48];
    if (alpha_half) snprintf(x, sizeof x, "%s.%c", r, use_alpha ? 'a' : 'b');
    else if (use_alpha) snprintf(x, sizeof x, "vec3(%s.a)", r);
    else snprintf(x, sizeof x, "%s.rgb", r);
    const char *t = alpha_half ? "float" : "vec3";
    if (stage == 8) {   // the final combiner: unsigned identity or invert only
        if (map & 1) sprintf(out, "(%s(1.0) - clamp(%s, 0.0, 1.0))", t, x);
        else sprintf(out, "clamp(%s, 0.0, 1.0)", x);
        return;
    }
    switch (map) {
    case 0: sprintf(out, "max(%s, 0.0)", x); break;
    case 1: sprintf(out, "(1.0 - clamp(%s, 0.0, 1.0))", x); break;
    case 2: sprintf(out, "(2.0 * max(%s, 0.0) - 1.0)", x); break;
    case 3: sprintf(out, "(1.0 - 2.0 * max(%s, 0.0))", x); break;
    case 4: sprintf(out, "(max(%s, 0.0) - 0.5)", x); break;
    case 5: sprintf(out, "(0.5 - max(%s, 0.0))", x); break;
    case 6: sprintf(out, "(%s)", x); break;
    default: sprintf(out, "(-(%s))", x); break;
    }
}

static const char *scale_fmt(uint32_t op)
{
    switch (op) {
    case 1: return "(%s - 0.5)";
    case 2: return "(%s * 2.0)";
    case 3: return "((%s - 0.5) * 2.0)";
    case 4: return "(%s * 4.0)";
    case 6: return "(%s * 0.5)";
    }
    return "(%s)";
}

static int writable(int r) { return r == 4 || r == 5 || (r >= 8 && r <= 13); }

// one half of a general combiner stage: computes into ab_, cd_, s_ (suffix c or a)
static void stage_half(buf *b, const psh_key *k, int i, int alpha)
{
    uint32_t icw = alpha ? k->alpha_icw[i] : k->color_icw[i], ocw = alpha ? k->alpha_ocw[i] : k->color_ocw[i];
    char a[96], bb[96], c[96], d[96], e[256];
    input(a, icw >> 24, alpha, i, k);
    input(bb, icw >> 16 & 0xff, alpha, i, k);
    input(c, icw >> 8 & 0xff, alpha, i, k);
    input(d, icw & 0xff, alpha, i, k);
    const char *t = alpha ? "float" : "vec3", sfx = alpha ? 'a' : 'c';
    uint32_t flags = ocw >> 12, op = flags >> 3 & 7;
    const char *sc = scale_fmt(op);
    if (!alpha && (flags & 2)) snprintf(e, sizeof e, "vec3(dot(%s, %s))", a, bb);
    else snprintf(e, sizeof e, "%s * %s", a, bb);
    put(b, "    %s ab%c = %s;\n", t, sfx, e);
    if (!alpha && (flags & 1)) snprintf(e, sizeof e, "vec3(dot(%s, %s))", c, d);
    else snprintf(e, sizeof e, "%s * %s", c, d);
    put(b, "    %s cd%c = %s;\n", t, sfx, e);
    if (flags & 4) {   // mux on r0.a: its top bit, or its lowest
        const char *cond = (k->combiner_control & 0x100) ? "r0.a >= 0.5" : "(int(r0.a * 255.0 + 0.5) & 1) == 1";
        put(b, "    %s s%c = (%s) ? cd%c : ab%c;\n", t, sfx, cond, sfx, sfx);
    } else {
        put(b, "    %s s%c = ab%c + cd%c;\n", t, sfx, sfx, sfx);
    }
    char x[32];
    snprintf(x, sizeof x, "ab%c", sfx);
    put(b, "    ab%c = clamp(", sfx), put(b, sc, x), put(b, ", -1.0, 1.0);\n");
    snprintf(x, sizeof x, "cd%c", sfx);
    put(b, "    cd%c = clamp(", sfx), put(b, sc, x), put(b, ", -1.0, 1.0);\n");
    snprintf(x, sizeof x, "s%c", sfx);
    put(b, "    s%c = clamp(", sfx), put(b, sc, x), put(b, ", -1.0, 1.0);\n");
}

static void stage_write(buf *b, const psh_key *k, int i, int alpha)
{
    uint32_t ocw = alpha ? k->alpha_ocw[i] : k->color_ocw[i];
    int cd = ocw & 15, ab = ocw >> 4 & 15, sum = ocw >> 8 & 15;
    char sfx = alpha ? 'a' : 'c';
    const char *sw = alpha ? "a" : "rgb";
    if (writable(ab)) put(b, "    %s.%s = ab%c;\n", reg_name(ab, i, k), sw, sfx);
    if (writable(cd)) put(b, "    %s.%s = cd%c;\n", reg_name(cd, i, k), sw, sfx);
    if (writable(sum)) put(b, "    %s.%s = s%c;\n", reg_name(sum, i, k), sw, sfx);
    if (!alpha) {   // blue to alpha
        if ((ocw >> 19 & 1) && writable(ab)) put(b, "    %s.a = abc.b;\n", reg_name(ab, i, k));
        if ((ocw >> 18 & 1) && writable(cd)) put(b, "    %s.a = cdc.b;\n", reg_name(cd, i, k));
    }
}

static void tex_stage(buf *b, const psh_key *k, int i)
{
    int mode = k->stage_program >> (5 * i) & 0x1f, kind = k->tex_kind[i];
    int prev = i == 0 ? 0 : i == 1 ? 0 : (k->other_input >> (16 + 4 * (i - 2)) & 3);
    if (mode == 0) return;
    if (!kind && mode != 4 && mode != 5) {
        put(b, "  t%d = vec4(0.0, 0.0, 0.0, 1.0);\n", i);
        return;
    }
    switch (mode) {
    case 1:   // 2D, projective
    case 6: case 7:   // bump-env-map: not yet, read as 2D
        if (kind == 3) put(b, "  t%d = texture(tex%d, vT%d.xyz);\n", i, i, i);
        else if (kind == 4) put(b, "  t%d = texture(tex%d, vT%d.xyz / vT%d.w);\n", i, i, i, i);
        else put(b, "  t%d = texture(tex%d, vT%d.xy / vT%d.w * texScale[%d]);\n", i, i, i, i, i);
        break;
    case 2:   // 3D
        if (kind == 4) put(b, "  t%d = texture(tex%d, vT%d.xyz / vT%d.w);\n", i, i, i, i);
        else put(b, "  t%d = texture(tex%d, vT%d.xy / vT%d.w * texScale[%d]);\n", i, i, i, i, i);
        break;
    case 3:   // cube map
        if (kind == 3) put(b, "  t%d = texture(tex%d, vT%d.xyz);\n", i, i, i);
        else put(b, "  t%d = texture(tex%d, vT%d.xy * texScale[%d]);\n", i, i, i, i);
        break;
    case 4:   // pass-through: the coordinates are the colour
        put(b, "  t%d = clamp(vT%d, 0.0, 1.0);\n", i, i);
        break;
    case 5:   // clip plane: a negative coordinate kills the pixel
        put(b, "  if (any(lessThan(vT%d, vec4(0.0)))) discard;\n  t%d = vec4(0.0);\n", i, i);
        break;
    case 15:  // dependent read from another stage's alpha and red
        put(b, "  t%d = texture(tex%d, t%d.ar);\n", i, i, prev);
        break;
    case 16:  // dependent read from green and blue
        put(b, "  t%d = texture(tex%d, t%d.gb);\n", i, i, prev);
        break;
    default:
        put(b, "  t%d = vec4(0.0); // texture mode %d not done\n", i, mode);
        break;
    }
}

int psh_translate(const psh_key *k, char *out, int size)
{
    buf b = { out, size };
    put(&b, "  vec4 zero = vec4(0.0);\n");
    put(&b, "  vec4 v0 = clamp(vD0, 0.0, 1.0), v1 = clamp(vD1, 0.0, 1.0);\n");
    put(&b, "  vec4 fog = vec4(fogColor.rgb, clamp(vFogF, 0.0, 1.0));\n");
    put(&b, "  vec4 t0 = vec4(0.0), t1 = vec4(0.0), t2 = vec4(0.0), t3 = vec4(0.0);\n");
    for (int i = 0; i < 4; i++) tex_stage(&b, k, i);
    put(&b, "  vec4 r0 = vec4(0.0, 0.0, 0.0, t0.a), r1 = vec4(0.0);\n");
    int n = k->combiner_control & 0xff;
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) {
        put(&b, "  {\n");
        stage_half(&b, k, i, 0);
        stage_half(&b, k, i, 1);
        stage_write(&b, k, i, 1);
        stage_write(&b, k, i, 0);
        put(&b, "  }\n");
    }
    if (k->final0 || k->final1) {
        uint32_t f1 = k->final1;
        char a[96], bb[96], c[96], d[96], e[96], f[96], g[96];
        input(e, f1 >> 24, 0, 8, k);
        input(f, f1 >> 16 & 0xff, 0, 8, k);
        put(&b, "  vec4 efprod = vec4(%s * %s, 0.0);\n", e, f);
        put(&b, "  vec4 v1r0 = vec4(%s.rgb + %s.rgb, 0.0);\n", (f1 & 0x40) ? "(1.0 - v1)" : "v1",
            (f1 & 0x20) ? "(1.0 - r0)" : "r0");
        if (f1 & 0x80) put(&b, "  v1r0 = clamp(v1r0, 0.0, 1.0);\n");
        input(a, k->final0 >> 24, 0, 8, k);
        input(bb, k->final0 >> 16 & 0xff, 0, 8, k);
        input(c, k->final0 >> 8 & 0xff, 0, 8, k);
        input(d, k->final0 & 0xff, 0, 8, k);
        input(g, f1 >> 8 & 0xff, 1, 8, k);
        put(&b, "  fragColor.rgb = %s + mix(%s, %s, %s);\n", d, c, bb, a);
        put(&b, "  fragColor.a = %s;\n", g);
    } else {
        put(&b, "  fragColor = r0;\n");
    }
    if (k->alpha_test) {
        static const char *const cmp[8] = { "false", "<", "==", "<=", ">", "!=", ">=", "true" };
        int fn = k->alpha_func;
        if (fn == 0) put(&b, "  discard;\n");
        else if (fn != 7) put(&b, "  if (!(floor(fragColor.a * 255.0 + 0.5) %s alphaRef)) discard;\n", cmp[fn]);
    }
    return b.left > 0;
}
