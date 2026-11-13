// The NV2A's fixed-function vertex stage (transform, skinning, texgen,
// texture matrices, lighting, fog distance) as the body of a GLSL main(),
// written from the state the pushbuffer set. It runs in the same shader
// frame as a translated vertex program (gpu_gl.c vs_head/vs_tail), so it
// writes the same outputs: oPos in screen space, oD0/oD1, oT0-3, oFog.x.
//
// Matrices, texgen planes, the eye position and the viewport offset live
// where the hardware keeps them, in the transform constants (pgraph.c puts
// them there): see the XF_ slots below. The light and material values come
// in a separate uniform array, ff[], laid out by vsh_ff_uniforms().
#include "pgraph.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// transform constant rows the fixed-function methods write
enum {
    XF_CMAT = 0x00, XF_PMAT = 0x04, XF_MMAT = 0x08, XF_IMMAT = 0x0c,   // + 8 per matrix
    XF_EYEP = 0x38, XF_FOG = 0x39, XF_VPSCL = 0x3a, XF_VPOFF = 0x3b,
    XF_TGMAT = 0x40, XF_TMAT = 0x44,                                     // + 8 per stage
};

// ff[] layout
enum { FF_AMBIENT = 0, FF_EMISSION = 1, FF_SPECULAR = 2, FF_LIGHT = 4, FF_LIGHT_STRIDE = 10 };
enum { L_AMB, L_DIF, L_SPC, L_HALF, L_DIR, L_FALLOFF, L_SPOTDIR, L_POS, L_ATTEN };

static float fl(const pg_state *s, uint32_t method)
{
    uint32_t u = pg_m(s, method);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

void vsh_ff_uniforms(const pg_state *s, float ff[VSH_FF_UNIFORMS][4])
{
    memset(ff, 0, sizeof(float) * 4 * VSH_FF_UNIFORMS);
    for (int i = 0; i < 3; i++) {
        ff[FF_AMBIENT][i] = fl(s, 0x0a10 + 4 * i);    // SET_SCENE_AMBIENT_COLOR
        ff[FF_EMISSION][i] = fl(s, 0x03a8 + 4 * i);   // SET_MATERIAL_EMISSION
        ff[FF_SPECULAR][i] = fl(s, 0x09e0 + 4 * i);   // SET_SPECULAR_PARAMS
    }
    ff[FF_AMBIENT][3] = fl(s, 0x03b4);                // SET_MATERIAL_ALPHA
    for (int l = 0; l < 8; l++) {
        float (*L)[4] = &ff[FF_LIGHT + FF_LIGHT_STRIDE * l];
        uint32_t b = 0x1000 + 0x80 * l;
        for (int i = 0; i < 3; i++) {
            L[L_AMB][i] = fl(s, b + 0x00 + 4 * i);
            L[L_DIF][i] = fl(s, b + 0x0c + 4 * i);
            L[L_SPC][i] = fl(s, b + 0x18 + 4 * i);
            L[L_HALF][i] = fl(s, b + 0x28 + 4 * i);
            L[L_DIR][i] = fl(s, b + 0x34 + 4 * i);
            L[L_FALLOFF][i] = fl(s, b + 0x40 + 4 * i);
            L[L_POS][i] = fl(s, b + 0x5c + 4 * i);
            L[L_ATTEN][i] = fl(s, b + 0x68 + 4 * i);
        }
        for (int i = 0; i < 4; i++) L[L_SPOTDIR][i] = fl(s, b + 0x4c + 4 * i);
        L[L_AMB][3] = fl(s, b + 0x24);   // local range
    }
}

// the state the generated code depends on, for the program cache
void vsh_ff_key(const pg_state *s, uint32_t key[VSH_FF_KEY])
{
    static const uint16_t methods[] = {
        0x0294, 0x0298, 0x02a0, 0x02a4, 0x0314, 0x0328, 0x03a4, 0x03b8, 0x03bc, 0x09cc,
        0x03c0, 0x03c4, 0x03c8, 0x03cc, 0x03d0, 0x03d4, 0x03d8, 0x03dc,
        0x03e0, 0x03e4, 0x03e8, 0x03ec, 0x03f0, 0x03f4, 0x03f8, 0x03fc,
        0x0420, 0x0424, 0x0428, 0x042c,
    };
    memset(key, 0, 4 * VSH_FF_KEY);
    for (unsigned i = 0; i < sizeof methods / sizeof *methods && i < VSH_FF_KEY; i++) key[i] = pg_m(s, methods[i]);
}

typedef struct { char *p; int n, size; } out_t;

static void put(out_t *o, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(o->p + o->n, o->n < o->size ? o->size - o->n : 0, fmt, ap);
    va_end(ap);
    if (k > 0) o->n += k;
}

// out = in transformed by the matrix at constant row r (rows are dot products)
static void xform(out_t *o, const char *out, const char *in, int r, const char *sw)
{
    put(o, "%s(dot(%s, c[%d]), dot(%s, c[%d]), dot(%s, c[%d]), dot(%s, c[%d])).%s", out, in, r, in, r + 1, in, r + 2, in, r + 3, sw);
}

// skinning: tPosition from the model view matrices, tNormal from their inverses
static void skin(out_t *o, int mode, const char *type, const char *out, const char *in, int base, const char *sw)
{
    static const int count[7] = { 0, 2, 2, 3, 3, 4, 4 };
    int n = mode >= 0 && mode < 7 ? count[mode] : 0;
    int mix = mode == 1 || mode == 3 || mode == 5;   // the last weight is 1 - the others
    if (!n) {
        put(o, "  %s %s = ", type, out);
        xform(o, "vec4", in, base, sw);
        put(o, ";\n");
        return;
    }
    put(o, "  %s %s = %s(0.0);\n  { float wl = 1.0, wi;\n", type, out, type);
    for (int i = 0; i < n; i++) {
        if (mix && i == n - 1) put(o, "    wi = wl;\n");
        else put(o, "    wi = v1.%c; wl -= wi;\n", "xyzw"[i]);
        put(o, "    %s += ", out);
        xform(o, "vec4", in, base + 8 * i, sw);
        put(o, " * wi;\n");
    }
    put(o, "  }\n");
}

static const char *color_src(int src, const char *material)
{
    return src == 1 ? "v3" : src == 2 ? "v4" : material;
}

int vsh_ff_translate(const pg_state *s, char *buf, int size)
{
    out_t o = { buf, 0, size };
    uint32_t light_control = pg_m(s, 0x0294), color_material = pg_m(s, 0x0298);
    int lighting = pg_m(s, 0x0314) & 1, skin_mode = pg_m(s, 0x0328), normalize = pg_m(s, 0x03a4) & 1;
    int specular = pg_m(s, 0x03b8) & 1, lights = pg_m(s, 0x03bc), infinite_viewer = pg_m(s, 0x09cc) & 1;
    int local_eye = (light_control >> 16) & 1, separate_specular = light_control & 1;
    int material_specular_alpha = (light_control >> 17) & 1;
    int emission_src = color_material & 3, ambient_src = color_material >> 2 & 3;
    int diffuse_src = color_material >> 4 & 3, specular_src = color_material >> 6 & 3;

    skin(&o, skin_mode, "vec4", "tPosition", "v0", XF_MMAT, "xyzw");
    skin(&o, skin_mode, "vec3", "tNormal", "vec4(v2.xyz, 0.0)", XF_IMMAT, "xyz");
    if (normalize) put(&o, "  tNormal = normalize(tNormal);\n");

    // texture coordinates: generated or passed through, then the texture matrix
    for (int t = 0; t < 4; t++) {
        for (int j = 0; j < 4; j++) {
            char ch = "xyzw"[j];
            uint32_t g = pg_m(s, 0x03c0 + 0x10 * t + 4 * j);
            int plane = XF_TGMAT + 8 * t + j;
            switch (g) {
            case 0x2400:   // eye linear
                put(&o, "  oT%d.%c = dot(c[%d], tPosition);\n", t, ch, plane);
                break;
            case 0x2401:   // object linear
                put(&o, "  oT%d.%c = dot(c[%d], v0);\n", t, ch, plane);
                break;
            case 0x2402:   // sphere map (S, T)
            case 0x8512:   // reflection map (S, T, R)
                put(&o, "  { vec3 u = %s; vec3 r = reflect(u, tNormal);\n",
                    infinite_viewer ? "vec3(0.0, 0.0, 1.0)" : "normalize(tPosition.xyz)");
                if (g == 0x2402) put(&o, "    oT%d.%c = r.%c / (2.0 * length(r + vec3(0.0, 0.0, 1.0))) + 0.5; }\n", t, ch, ch);
                else put(&o, "    oT%d.%c = r.%c; }\n", t, ch, ch);
                break;
            case 0x8511:   // normal map
                put(&o, "  oT%d.%c = tNormal.%c;\n", t, ch, ch);
                break;
            default:
                put(&o, "  oT%d.%c = v%d.%c;\n", t, ch, 9 + t, ch);
                break;
            }
        }
        if (pg_m(s, 0x0420 + 4 * t) & 1) {
            put(&o, "  oT%d = ", t);
            char in[8];
            snprintf(in, sizeof in, "oT%d", t);
            xform(&o, "vec4", in, XF_TMAT + 8 * t, "xyzw");
            put(&o, ";\n");
        }
    }

    if (!lighting) {
        put(&o, "  oD0 = v3; oD1 = v4; oB0 = v7; oB1 = v8;\n");
    } else {
        const char *alpha = diffuse_src == 0 ? "ff[0].w" : diffuse_src == 2 ? "v4.a" : "v3.a";
        // the base colour: scene ambient (with the material's), plus emission
        if (emission_src == 0 || ambient_src == 0) {
            put(&o, "  oD0 = vec4(ff[%d].rgb, %s);\n", FF_AMBIENT, alpha);
            if (emission_src != 0) put(&o, "  oD0.rgb += %s.rgb * ff[%d].rgb;\n", color_src(emission_src, ""), FF_EMISSION);
            else if (ambient_src != 0) put(&o, "  oD0.rgb += %s.rgb * ff[%d].rgb;\n", color_src(ambient_src, ""), FF_EMISSION);
        } else {
            put(&o, "  oD0 = vec4(%s.rgb + %s.rgb * ff[%d].rgb, %s);\n", color_src(emission_src, ""),
                color_src(ambient_src, ""), FF_EMISSION, alpha);
        }
        put(&o, "  oD1 = vec4(0.0, 0.0, 0.0, v4.a);\n");
        if (local_eye) put(&o, "  vec3 VPeye = normalize(c[%d].xyz / c[%d].w - tPosition.xyz / tPosition.w);\n", XF_EYEP, XF_EYEP);
        for (int l = 0; l < 8; l++) {
            int type = lights >> (2 * l) & 3;
            if (!type) continue;
            int L = FF_LIGHT + FF_LIGHT_STRIDE * l;
            put(&o, "  { float att = 1.0; float nDotVP, nDotHV;\n");
            if (type == 1) {   // infinite
                put(&o, "    vec3 dir = normalize(ff[%d].xyz);\n    nDotVP = max(0.0, dot(tNormal, dir));\n", L + L_DIR);
                if (local_eye) put(&o, "    nDotHV = max(0.0, dot(tNormal, normalize(dir + VPeye)));\n");
                else put(&o, "    nDotHV = max(0.0, dot(tNormal, ff[%d].xyz));\n", L + L_HALF);
            } else {           // local or spot
                put(&o, "    vec3 VP = ff[%d].xyz - tPosition.xyz / tPosition.w;\n", L + L_POS);
                put(&o, "    float d = length(VP);\n");
                put(&o, "    if (d > ff[%d].w) att = 0.0;\n", L + L_AMB);
                put(&o, "    VP = normalize(VP);\n");
                put(&o, "    att /= ff[%d].x + ff[%d].y * d + ff[%d].z * d * d;\n", L + L_ATTEN, L + L_ATTEN, L + L_ATTEN);
                put(&o, "    nDotVP = max(0.0, dot(tNormal, VP));\n");
                put(&o, "    nDotHV = max(0.0, dot(tNormal, normalize(VP + %s)));\n", local_eye ? "VPeye" : "vec3(0.0)");
                if (type == 3) {   // spot: (s + L) / (M s + N), s from the spot direction
                    put(&o, "    float sp = dot(ff[%d].xyz, VP) + ff[%d].w;\n", L + L_SPOTDIR, L + L_SPOTDIR);
                    put(&o, "    if (sp <= 0.0) att = 0.0;\n"
                            "    else if (sp < 1.0) {\n"
                            "      vec3 k = ff[%d].xyz; float f = 0.0;\n"
                            "      float den = k.y * sp + k.z;\n"
                            "      if (sp > -k.x) f = den == 0.0 ? 1.0 : den > 0.0 ? clamp((sp + k.x) / den, 0.0, 1.0) : 0.0;\n"
                            "      att *= f;\n"
                            "    }\n", L + L_FALLOFF);
                }
            }
            // specular power, as the same rational form with SET_SPECULAR_PARAMS
            put(&o, "    float pf = 0.0;\n"
                    "    vec3 sk = ff[%d].xyz;\n"
                    "    if (nDotVP > 0.0 && nDotHV > -sk.x) {\n"
                    "      float den = sk.y * nDotHV + sk.z;\n"
                    "      pf = den == 0.0 ? 10000.0 : den > 0.0 ? clamp((nDotHV + sk.x) / den, 0.0, 10000.0) : 0.0;\n"
                    "    }\n", FF_SPECULAR);
            put(&o, "    vec3 la = ff[%d].rgb * att, ld = ff[%d].rgb * att * nDotVP, ls = ff[%d].rgb * att * pf;\n",
                L + L_AMB, L + L_DIF, L + L_SPC);
            static const char *const mul[3] = { "", "v3.rgb * ", "v4.rgb * " };
            put(&o, "    oD0.rgb += %sla + %sld;\n", mul[ambient_src % 3], mul[diffuse_src % 3]);
            put(&o, "    oD1.rgb += %sls;\n  }\n", mul[specular_src % 3]);
        }
        put(&o, "  oB0 = v7; oB1 = v8;\n");
    }
    if (!specular) {
        put(&o, "  oD1 = vec4(0.0, 0.0, 0.0, 1.0); oB1 = vec4(0.0, 0.0, 0.0, 1.0);\n");
    } else {
        if (!separate_specular) {
            if (lighting) put(&o, "  oD0.rgb += oD1.rgb; oB0.rgb += oB1.rgb;\n");
            put(&o, "  oD1 = v4; oB1 = v8;\n");
        }
        if (material_specular_alpha) put(&o, "  oD1.a = 1.0; oB1.a = 1.0;\n");
    }

    // fog distance, for the fog factor the shared tail computes from oFog.x
    if (pg_m(s, M_FOG_ENABLE)) {
        switch (pg_m(s, M_FOG_GEN_MODE)) {
        case 0: put(&o, "  oFog.x = clamp(v4.a, 0.0, 1.0);\n"); break;          // specular alpha
        case 1: put(&o, "  oFog.x = length(tPosition.xyz);\n"); break;          // radial
        case 2: put(&o, "  oFog.x = dot(c[%d].xyz, tPosition.xyz) + c[%d].w;\n", XF_FOG, XF_FOG); break;
        case 3: put(&o, "  oFog.x = abs(dot(c[%d].xyz, tPosition.xyz) + c[%d].w);\n", XF_FOG, XF_FOG); break;
        default: put(&o, "  oFog.x = v5.x;\n"); break;                          // fog coordinate
        }
    }

    // position: the composite matrix (it includes the model view when not
    // skinning; skinned, it takes the blended eye-space position), the
    // perspective divide (the shared tail takes screen x, y and depth, as a
    // vertex program leaves them), then the viewport offset
    put(&o, "  oPos = ");
    xform(&o, "vec4", skin_mode ? "tPosition" : "v0", XF_CMAT, "xyzw");
    put(&o, ";\n");
    put(&o, "  if (abs(oPos.w) < 1.0e-30) oPos.w = oPos.w < 0.0 ? -1.0e-30 : 1.0e-30;\n"
            "  if (isinf(oPos.w)) oPos.w = sign(oPos.w) * 1.0e30;\n"
            "  oPos.xyz /= oPos.w;\n"
            "  oPos.xy += c[%d].xy;\n", XF_VPOFF);
    return o.n < size ? 1 : 0;
}
