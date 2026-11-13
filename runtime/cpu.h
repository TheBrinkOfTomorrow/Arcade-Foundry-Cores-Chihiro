// Guest CPU state and the primitives generated code is written against
// (shared with the Lindbergh cores' recompiler).
// Guest memory is one 4 GB window at M; a guest address is an offset into it.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union {
    float f[4];
    double d[2];
    uint8_t b[16];
    int8_t sb[16];
    uint16_t w[8];
    int16_t sw[8];
    uint32_t u[4];
    int32_t i[4];
    uint64_t q[2];
    int64_t sq[2];
} xmm_t;

typedef union {
    float f[2];
    uint8_t b[8];
    uint16_t w[4];
    int16_t sw[4];
    uint32_t u[2];
    int32_t i[2];
    int8_t sb[8];
    uint64_t q;
} mmx_t;

typedef struct CPU {
    // eax ecx edx ebx esp ebp esi edi, in encoding order
    uint32_t r[8];
    uint32_t df;
    uint32_t eflags_id;   // EFLAGS.ID: software toggles it to detect cpuid
    // x87: physical registers, top-of-stack index, control and status words
    double st[8];
    uint32_t ftop;
    uint16_t fcw, fsw;
    xmm_t xmm[8];
    mmx_t mm[8];
    uint32_t mxcsr;
    // set by a guest function that returns through a different path than call
    uint32_t thread_index;
    uint32_t fs_base;     // fs: the kernel's per-processor region (KPCR)
} CPU;

extern uint8_t *M;

#define EAX 0
#define ECX 1
#define EDX 2
#define EBX 3
#define ESP 4
#define EBP 5
#define ESI 6
#define EDI 7

// Hardware registers: everything from 0xFD000000 up (NV2A, APU, USB, the
// rest of the MCPX) is a device. An access there goes to the device's
// handler; the compare costs a predictable branch on every other access.
#define MMIO_BASE 0xFD000000u
uint32_t mmio_read(uint32_t a, int size);
void mmio_write(uint32_t a, uint32_t v, int size);
#define IS_MMIO(a) __builtin_expect((a) >= MMIO_BASE, 0)

static inline uint8_t rd8(uint32_t a) { if (IS_MMIO(a)) return (uint8_t)mmio_read(a, 1); return M[a]; }
static inline uint16_t rd16(uint32_t a) { if (IS_MMIO(a)) return (uint16_t)mmio_read(a, 2); uint16_t v; memcpy(&v, M + a, 2); return v; }
static inline uint32_t rd32(uint32_t a) { if (IS_MMIO(a)) return mmio_read(a, 4); uint32_t v; memcpy(&v, M + a, 4); return v; }
static inline uint64_t rd64(uint32_t a)
{
    if (IS_MMIO(a)) return mmio_read(a, 4) | (uint64_t)mmio_read(a + 4, 4) << 32;
    uint64_t v; memcpy(&v, M + a, 8); return v;
}
static inline float rdf32(uint32_t a) { uint32_t u = rd32(a); float v; memcpy(&v, &u, 4); return v; }
static inline double rdf64(uint32_t a) { uint64_t u = rd64(a); double v; memcpy(&v, &u, 8); return v; }
static inline void wr8(uint32_t a, uint8_t v) { if (IS_MMIO(a)) { mmio_write(a, v, 1); return; } M[a] = v; }
static inline void wr16(uint32_t a, uint16_t v) { if (IS_MMIO(a)) { mmio_write(a, v, 2); return; } memcpy(M + a, &v, 2); }
static inline void wr32(uint32_t a, uint32_t v) { if (IS_MMIO(a)) { mmio_write(a, v, 4); return; } memcpy(M + a, &v, 4); }
static inline void wr64(uint32_t a, uint64_t v)
{
    if (IS_MMIO(a)) { mmio_write(a, (uint32_t)v, 4); mmio_write(a + 4, (uint32_t)(v >> 32), 4); return; }
    memcpy(M + a, &v, 8);
}
static inline void wrf32(uint32_t a, float v) { uint32_t u; memcpy(&u, &v, 4); wr32(a, u); }
static inline void wrf64(uint32_t a, double v) { uint64_t u; memcpy(&u, &v, 8); wr64(a, u); }
static inline void rd128(xmm_t *x, uint32_t a)
{
    if (IS_MMIO(a)) { uint32_t w[4]; for (int i = 0; i < 4; i++) w[i] = mmio_read(a + 4 * i, 4); memcpy(x, w, 16); return; }
    memcpy(x, M + a, 16);
}
static inline void wr128(uint32_t a, const xmm_t *x)
{
    if (IS_MMIO(a)) { uint32_t w[4]; memcpy(w, x, 16); for (int i = 0; i < 4; i++) mmio_write(a + 4 * i, w[i], 4); return; }
    memcpy(M + a, x, 16);
}
// the devices' own view of their registers: plain memory, no handler
static inline uint32_t reg_rd32(uint32_t a) { uint32_t v; memcpy(&v, M + a, 4); return v; }
static inline void reg_wr32(uint32_t a, uint32_t v) { memcpy(M + a, &v, 4); }

static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

static inline uint32_t parity8(uint32_t v) { return !__builtin_parity(v & 0xff); }

// 80-bit extended values in memory (fld/fstp tbyte) are converted to double.
double x87_load80(uint32_t a);
void x87_store80(uint32_t a, double v);
void x87_fnsave(CPU *c, uint32_t a);
void x87_frstor(CPU *c, uint32_t a);
void x87_fxam(CPU *c);

#define ST(i) c->st[(c->ftop + (i)) & 7]
static inline void fpush(CPU *c, double v) { c->ftop = (c->ftop - 1) & 7; c->st[c->ftop] = v; }
static inline void fpop(CPU *c) { c->ftop = (c->ftop + 1) & 7; }
static inline uint16_t fstatus(CPU *c) { return (uint16_t)((c->fsw & ~0x3800) | ((c->ftop & 7) << 11)); }
// C0/C2/C3 from a compare of a and b; unordered sets all three
static inline void fcompare(CPU *c, double a, double b) {
    c->fsw &= ~0x4500;
    if (a != a || b != b) c->fsw |= 0x4500;
    else if (a < b) c->fsw |= 0x0100;
    else if (a == b) c->fsw |= 0x4000;
}
// integer conversion with the rounding mode in the control word
int64_t x87_round(CPU *c, double v);
int64_t sse_round(CPU *c, double v);
static inline int32_t sse_cvt32(CPU *c, double v) {
    if (!(v >= -2147483648.0 && v < 2147483648.0)) return (int32_t)0x80000000;
    return (int32_t)sse_round(c, v);
}
static inline int32_t sse_cvtt32(double v) {
    if (!(v >= -2147483648.0 && v < 2147483648.0)) return (int32_t)0x80000000;
    return (int32_t)v;
}
// fist/fistp: out of range is the integer indefinite. For 64 bits only the
// double is checked: -(int64_t)2^63 is undefined, and clang on arm64 made the
// check always fail, so every fistp m64 stored the indefinite (HotD4's CRI
// sound turned all its send levels to 0 that way).
static inline int64_t x87_int(CPU *c, double v, int bits) {
    if (bits == 64) {
        if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0)) return INT64_MIN;
        return x87_round(c, v);
    }
    int64_t lim = bits == 16 ? 32768 : 2147483648LL;
    if (!(v >= -(double)lim && v < (double)lim)) return bits == 16 ? -32768 : (int64_t)(int32_t)0x80000000;
    int64_t r = x87_round(c, v);
    if (r < -lim || r >= lim) return bits == 16 ? -32768 : (int64_t)(int32_t)0x80000000;
    return r;
}

// SSE compare predicates (cmpXXss/sd, imm 0-7)
static inline int sse_cmp(double a, double b, int p) {
    switch (p & 7) {
    case 0: return a == b;
    case 1: return a < b;
    case 2: return a <= b;
    case 3: return a != a || b != b;
    case 4: return !(a == b);
    case 5: return !(a < b);
    case 6: return !(a <= b);
    default: return !(a != a || b != b);
    }
}
static inline float sse_minf(float a, float b) { return a < b ? a : b; }
static inline float sse_maxf(float a, float b) { return a > b ? a : b; }
static inline double sse_mind(double a, double b) { return a < b ? a : b; }
static inline double sse_maxd(double a, double b) { return a > b ? a : b; }

static inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

// Calls between generated code, the dispatcher and the host-side libraries.
typedef void (*guest_fn)(CPU *c);
guest_fn guest_lookup(uint32_t addr);
void guest_dispatch(CPU *c, uint32_t addr);   // call addr; return address already pushed
uint32_t guest_call(CPU *c, uint32_t addr, int nargs, const uint32_t *args);  // host -> guest
void guest_unimpl(CPU *c, uint32_t addr, const char *what);
void guest_port_out(CPU *c, uint16_t port, uint32_t value, int size);
uint32_t guest_port_in(CPU *c, uint16_t port, int size);
void guest_cpuid(CPU *c, uint32_t *a, uint32_t *b, uint32_t *cc, uint32_t *d);
uint64_t guest_rdtsc(CPU *c);
void guest_debug_service(CPU *c, uint32_t service, uint32_t a, uint32_t b);
void guest_bad_return(CPU *c, uint32_t from, uint32_t expected, uint32_t got);

// A loop's back edge in generated code: another guest thread waiting for
// the processor may run here (kthread.c, games with GAME_ONE_CORE).
extern volatile int gcpu_want;
void gcpu_yield(void);
#define GUEST_LOOP()                                                         \
    do {                                                                     \
        __asm__ volatile("" ::: "memory");                                   \
        if (__builtin_expect(gcpu_want, 0)) gcpu_yield();                    \
    } while (0)

#ifdef __cplusplus
}
#endif
