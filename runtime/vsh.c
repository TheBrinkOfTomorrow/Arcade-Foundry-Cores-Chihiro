// NV2A vertex programs to GLSL.
//
// An instruction is four words (the first unused) holding a MAC operation
// (MOV MUL ADD MAD DP3 DPH DP4 DST MIN MAX SLT SGE ARL) and an ILU
// operation (MOV RCP RCC RSQ EXP LOG LIT) that run together on the same
// inputs: A, B, C, each a temporary R0-R12, an input v0-v15 or a constant
// c[0-191] (optionally indexed by A0), with a swizzle and a negation. The
// MAC writes a temporary, the ILU writes a temporary (R1 when both run),
// and one of them can also write an output. R12 is oPos.
//
// The body generated here reads its inputs from v0..v15, the constants from
// c[], and writes R0-R11 and the outputs oPos oD0 oD1 oFog oPts oB0 oB1
// oT0-oT3; the renderer declares those and turns oPos from the screen
// coordinates the program leaves into clip coordinates.
#include "pgraph.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct { int word, bit, len; } field;
static const field F_ILU = { 1, 25, 3 }, F_MAC = { 1, 21, 4 }, F_CONST = { 1, 13, 8 }, F_V = { 1, 9, 4 },
                   F_A_NEG = { 1, 8, 1 }, F_A_SWZ = { 1, 0, 8 }, F_A_R = { 2, 28, 4 }, F_A_MUX = { 2, 26, 2 },
                   F_B_NEG = { 2, 25, 1 }, F_B_SWZ = { 2, 17, 8 }, F_B_R = { 2, 13, 4 }, F_B_MUX = { 2, 11, 2 },
                   F_C_NEG = { 2, 10, 1 }, F_C_SWZ = { 2, 2, 8 }, F_C_RH = { 2, 0, 2 }, F_C_RL = { 3, 30, 2 },
                   F_C_MUX = { 3, 28, 2 }, F_MAC_MASK = { 3, 24, 4 }, F_OUT_R = { 3, 20, 4 },
                   F_ILU_MASK = { 3, 16, 4 }, F_O_MASK = { 3, 12, 4 }, F_ORB = { 3, 11, 1 },
                   F_O_ADDR = { 3, 3, 8 }, F_O_MUX = { 3, 2, 1 }, F_A0X = { 3, 1, 1 }, F_FINAL = { 3, 0, 1 };

static uint32_t get(const uint32_t *t, field f) { return t[f.word] >> f.bit & ((1u << f.len) - 1); }

enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4, MAC_DST, MAC_MIN, MAC_MAX,
       MAC_SLT, MAC_SGE, MAC_ARL };
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };

static const char *const out_name[16] = { "oPos", "oUnused1", "oUnused2", "oD0", "oD1", "oFog", "oPts", "oB0",
                                          "oB1", "oT0", "oT1", "oT2", "oT3", "oUnused13", "oUnused14", "oUnused15" };

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

static const char *rname(uint32_t r)
{
    static const char *const n[16] = { "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9", "R10", "R11",
                                       "oPos", "R13", "R14", "R15" };
    return n[r & 15];
}

static int creg(uint32_t c) { return (int)((c >> 5 & 7) - 3) * 32 + (int)(c & 31) + 96; }

// mask bits: x is 8, w is 1
static void mask_str(uint32_t m, char *s)
{
    for (int i = 0; i < 4; i++)
        if (m & (8 >> i)) *s++ = "xyzw"[i];
    *s = 0;
}

static void source(const uint32_t *t, int which, int scalar, char *s)
{
    static const field *mux[3] = { &F_A_MUX, &F_B_MUX, &F_C_MUX }, *neg[3] = { &F_A_NEG, &F_B_NEG, &F_C_NEG },
                       *swz[3] = { &F_A_SWZ, &F_B_SWZ, &F_C_SWZ };
    int r = which == 0 ? (int)get(t, F_A_R) : which == 1 ? (int)get(t, F_B_R) : (int)(get(t, F_C_RH) << 2 | get(t, F_C_RL));
    char reg[40];
    switch (get(t, *mux[which])) {
    case 1:
        snprintf(reg, sizeof reg, "%s", rname(r));
        break;
    case 2:
        snprintf(reg, sizeof reg, "v%u", get(t, F_V));
        break;
    case 3:
        if (get(t, F_A0X)) snprintf(reg, sizeof reg, "c[clamp(A0 + %d, 0, 191)]", creg(get(t, F_CONST)));
        else snprintf(reg, sizeof reg, "c[%d]", creg(get(t, F_CONST)));
        break;
    default:
        snprintf(reg, sizeof reg, "vec4(0.0)");
        break;
    }
    uint32_t sw = get(t, *swz[which]);
    char sx[5];
    for (int i = 0; i < 4; i++) sx[i] = "xyzw"[scalar ? sw >> 6 & 3 : sw >> (6 - 2 * i) & 3];
    sx[4] = 0;
    sprintf(s, "%s%s.%s", get(t, *neg[which]) ? "-" : "", reg, sx);
}

int vsh_translate(const uint32_t (*program)[4], int start, char *out, int size)
{
    buf b = { out, size };
    int n = 0;
    out[0] = 0;
    for (int slot = start; slot < PG_PROGRAM_SLOTS; slot++) {
        const uint32_t *t = program[slot];
        uint32_t mac = get(t, F_MAC), ilu = get(t, F_ILU);
        n++;
        if (mac != MAC_NOP || ilu != ILU_NOP) {
            char a[64], bb[64], c[64], cs[64];
            source(t, 0, 0, a);
            source(t, 1, 0, bb);
            source(t, 2, 0, c);
            source(t, 2, 1, cs);   // RCP RCC RSQ EXP LOG take C's first component
            put(&b, "  {\n");
            if (mac != MAC_NOP) {
                switch (mac) {
                case MAC_MOV: case MAC_ARL: put(&b, "    vec4 m = %s;\n", a); break;
                case MAC_MUL: put(&b, "    vec4 m = %s * %s;\n", a, bb); break;
                case MAC_ADD: put(&b, "    vec4 m = %s + %s;\n", a, c); break;
                case MAC_MAD: put(&b, "    vec4 m = %s * %s + %s;\n", a, bb, c); break;
                case MAC_DP3: put(&b, "    vec4 m = vec4(dot((%s).xyz, (%s).xyz));\n", a, bb); break;
                case MAC_DPH: put(&b, "    vec4 m = vec4(dot((%s).xyz, (%s).xyz) + (%s).w);\n", a, bb, bb); break;
                case MAC_DP4: put(&b, "    vec4 m = vec4(dot(%s, %s));\n", a, bb); break;
                case MAC_DST: put(&b, "    vec4 m = vec4(1.0, (%s).y * (%s).y, (%s).z, (%s).w);\n", a, bb, a, bb); break;
                case MAC_MIN: put(&b, "    vec4 m = min(%s, %s);\n", a, bb); break;
                case MAC_MAX: put(&b, "    vec4 m = max(%s, %s);\n", a, bb); break;
                case MAC_SLT: put(&b, "    vec4 m = vec4(lessThan(%s, %s));\n", a, bb); break;
                case MAC_SGE: put(&b, "    vec4 m = vec4(greaterThanEqual(%s, %s));\n", a, bb); break;
                default: put(&b, "    vec4 m = vec4(0.0);\n"); break;
                }
            }
            if (ilu != ILU_NOP) {
                switch (ilu) {
                case ILU_MOV: put(&b, "    vec4 i = %s;\n", c); break;
                case ILU_RCP: put(&b, "    vec4 i = vec4(1.0 / (%s).x);\n", cs); break;
                case ILU_RCC: put(&b, "    vec4 i = vec4(vsh_rcc((%s).x));\n", cs); break;
                case ILU_RSQ: put(&b, "    vec4 i = vec4(inversesqrt(abs((%s).x)));\n", cs); break;
                case ILU_EXP: put(&b, "    vec4 i = vsh_exp((%s).x);\n", cs); break;
                case ILU_LOG: put(&b, "    vec4 i = vsh_log((%s).x);\n", cs); break;
                case ILU_LIT: put(&b, "    vec4 i = vsh_lit(%s);\n", c); break;
                }
            }
            char mk[5];
            int paired = mac != MAC_NOP && ilu != ILU_NOP;
            uint32_t out_r = get(t, F_OUT_R);
            // temporaries
            if (mac == MAC_ARL) {
                put(&b, "    A0 = int(floor(m.x + 0.001));\n");
            } else if (mac != MAC_NOP && get(t, F_MAC_MASK) && !(paired && out_r == 1)) {
                mask_str(get(t, F_MAC_MASK), mk);
                put(&b, "    %s.%s = m.%s;\n", rname(out_r), mk, mk);
            }
            if (ilu != ILU_NOP && get(t, F_ILU_MASK)) {
                uint32_t r = paired ? 1 : out_r;
                mask_str(get(t, F_ILU_MASK), mk);
                put(&b, "    %s.%s = i.%s;\n", rname(r), mk, mk);
            }
            // the output
            uint32_t omask = get(t, F_O_MASK);
            int from_ilu = get(t, F_O_MUX) == 1;
            if (omask && (from_ilu ? ilu != ILU_NOP : mac != MAC_NOP && mac != MAC_ARL)) {
                const char *src = from_ilu ? "i" : "m";
                if (get(t, F_ORB) == 0) {
                    put(&b, "    // writes c[%d]: not supported\n", creg(get(t, F_O_ADDR)));
                } else {
                    uint32_t o = get(t, F_O_ADDR) & 15;
                    if (o == 5) {
                        put(&b, "    oFog.x = %s.x;\n", src);
                    } else {
                        mask_str(omask, mk);
                        put(&b, "    %s.%s = %s.%s;\n", out_name[o], mk, src, mk);
                    }
                }
            }
            put(&b, "  }\n");
        }
        if (get(t, F_FINAL)) break;
    }
    return b.left > 0 ? n : 0;
}
