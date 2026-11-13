// The NV2A's registers (0xFD000000, 16 MB). Guest accesses arrive from
// mmio.c. A register with no handler behaves as
// plain memory, which is right for most of the NV2A's setup registers and
// PRAMIN; the ones with side effects are handled here:
//   - interrupt status registers clear the bits written as 1;
//   - PMC_INTR_0 is the OR of the pending sources;
//   - PTIMER counts real time;
//   - writing the pushbuffer's DMA_PUT wakes the GPU thread, which runs the
//     pushbuffer up to it (moving DMA_GET as it goes) and owns the renderer.
// Vertical blank is the PCRTC interrupt, bus level 3, 60 times a second.
#define _GNU_SOURCE
#include "af_debug.h"
#include "perf.h"
#include "xbox.h"
#include "pgraph.h"
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#define REG(o) (NV2A_BASE + (o))
#define PMC_BOOT_0 0x000000
#define PMC_INTR_0 0x000100
#define PMC_INTR_EN_0 0x000140
#define PFIFO_INTR_0 0x002100
#define PFIFO_INTR_EN_0 0x002140
#define PTIMER_INTR_0 0x009100
#define PTIMER_TIME_0 0x009400
#define PTIMER_TIME_1 0x009410
#define PFB_CFG0 0x100200
#define PFB_CFG1 0x100204
#define PFB_WBC 0x100410
#define PGRAPH_INTR 0x400100
#define PGRAPH_INTR_EN 0x400140
#define PGRAPH_INTR_ERROR (1u << 20)
#define PGRAPH_NSOURCE 0x400108
#define PGRAPH_TRAPPED_ADDR 0x400704
#define PGRAPH_TRAPPED_DATA_LOW 0x400708
#define PGRAPH_STATUS 0x400700
#define PCRTC_INTR_0 0x600100
#define PCRTC_INTR_EN_0 0x600140
#define PCRTC_START 0x600800
#define USER_DMA_PUT 0x800040
#define USER_DMA_GET 0x800044
#define USER_REF 0x800048

#define GPU_LEVEL 3

volatile uint32_t nv2a_frames;
void (*gpu_present)(const uint32_t *px, int w, int h);
int gpu_offscreen;
static uint64_t t0_ns;   // the chip's time zero (clk_epoch: in render mode on a frame's end)
static volatile int nv2a_ready;   // the interrupt thread starts before nv2a_init
static uint64_t vblanks;
static uint32_t subch_object[8];
static uint32_t unknown_logged;
static pthread_mutex_t gpu_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gpu_kick = PTHREAD_COND_INITIALIZER;
static pthread_t gpu_thread;
static int gpu_started;
void pg_method(uint32_t method, uint32_t arg);
void pg_init(void);
void pg_reset(void);
static uint32_t call_return;   // the pushbuffer's call
// While the GPU runs a called pushbuffer, DMA_GET is outside the main one, and
// D3D (VC3 0x10c700, before it waits for a pushbuffer slot) reads where it
// will come back to from PFIFO_CACHE1_DMA_SUBROUTINE instead: the return
// address, bit 0 set while in the call. Its NOP-trap DPC copies the same from
// PGRAPH 0x401a88. Left at 0, D3D took the GPU for a whole ring behind and
// waited for a NOP the GPU had already run (a freeze for good).
#define PFIFO_CACHE1_DMA_SUBROUTINE 0x00324c
#define PGRAPH_DMA_SUBROUTINE 0x401a88
static void set_reg(uint32_t o, uint32_t v);
static void set_subroutine(uint32_t v)
{
    set_reg(PFIFO_CACHE1_DMA_SUBROUTINE, v);
    set_reg(PGRAPH_DMA_SUBROUTINE, v);
}
static pthread_mutex_t intr_m = PTHREAD_MUTEX_INITIALIZER;   // PGRAPH_INTR cleared by the ISR
static pthread_cond_t intr_cleared = PTHREAD_COND_INITIALIZER;
#define PGRAPH_FIFO 0x400720   // bit 0: PGRAPH takes methods (D3D's DPC turns it off while it handles a trap)
static int fifo_off;           // the guest turned PGRAPH_FIFO off (under intr_m)
static void *gpu_main(void *);
static FILE *pb_trace;   // CH_PBTRACE=file: every method, for study

static uint32_t reg(uint32_t o) { return reg_rd32(REG(o)); }
static void set_reg(uint32_t o, uint32_t v) { reg_wr32(REG(o), v); }

// the chip's time: vblanks, PTIMER and report times, from the core's clock
static uint64_t now_ns(void) { return clk_mono_ns() - t0_ns; }

void nv2a_init(void)
{
    // the chip and its memory as an Xbox reports them
    set_reg(PMC_BOOT_0, 0x02A000A2);
    set_reg(PFB_CFG0, 0x03070103);
    set_reg(PFB_CFG1, 0x11448000);
    t0_ns = clk_epoch();
    if (af_debug_env("CH_PBTRACE")) pb_trace = fopen(af_debug_env("CH_PBTRACE"), "w");
    pg_init();
    gpu_started = !pthread_create(&gpu_thread, NULL, gpu_main, NULL);
    nv2a_ready = 1;
}


// ---- interrupts -------------------------------------------------------------------------------
static uint32_t pmc_pending(void)
{
    uint32_t p = 0;
    if (reg(PFIFO_INTR_0) & reg(PFIFO_INTR_EN_0)) p |= 1u << 8;
    if (reg(PGRAPH_INTR) & reg(PGRAPH_INTR_EN)) p |= 1u << 12;
    if (reg(PCRTC_INTR_0) & reg(PCRTC_INTR_EN_0)) p |= 1u << 24;
    return p;
}

static void update_irq(void)
{
    if (pmc_pending() && (reg(PMC_INTR_EN_0) & 1)) ke_raise_interrupt(GPU_LEVEL);
}

// ---- the pushbuffer -------------------------------------------------------------------------------
// ---- objects -----------------------------------------------------------------------------------------
// PRAMIN (instance memory) is at 0x700000; RAMHT, the hash table from object
// handles to their instance addresses, is where PFIFO_RAMHT says. Looked up
// by a linear search, which finds the same entry as the hash.
#define PRAMIN 0x700000
static uint32_t ramht_find(uint32_t handle)
{
    uint32_t cfg = reg(0x002210);
    uint32_t base = (cfg >> 4 & 0x1f) << 12, size = 4096u << (cfg >> 16 & 3);
    for (uint32_t e = 0; e < size; e += 8)
        if (reg(PRAMIN + base + e) == handle) return (reg(PRAMIN + base + e + 4) & 0xffff) << 4;
    hle_log("nv2a: object 0x%x not in RAMHT\n", handle);
    return 0;
}

uint32_t nv2a_dma_base(uint32_t handle);
// a DMA object: its base (physical) and limit
static uint32_t dma_base(uint32_t handle, uint32_t *limit)
{
    uint32_t inst = ramht_find(handle);
    uint32_t w0 = reg(PRAMIN + inst), w1 = reg(PRAMIN + inst + 4), w2 = reg(PRAMIN + inst + 8);
    if (limit) *limit = w1;
    return (w2 & 0xfffff000u) | (w0 >> 20);
}

// ---- the 3D class (NV097) --------------------------------------------------------------------------
static uint32_t ctx_dma[16];    // methods 0x180-0x1bc: the object's DMA contexts
static uint32_t semaphore_offset;

uint32_t nv2a_dma_base(uint32_t handle) { return handle ? phys_of(dma_base(handle, NULL)) : 0; }

static void nv2a_method(int subch, uint32_t method, uint32_t arg)
{
    if (pb_trace) fprintf(pb_trace, "%d %04x %08x\n", subch, method, arg);
    if (method == 0) {
        subch_object[subch & 7] = arg;
        return;
    }
    if (method >= 0x180 && method < 0x1c0) {
        ctx_dma[(method - 0x180) / 4] = arg;
        if (subch == 0) pg_method(method, arg);
        return;
    }
    switch (method) {
    case 0x0050:   // NV_SET_REFERENCE (on any channel): the value USER_REF reads back
        set_reg(USER_REF, arg);
        return;
    case 0x012c:   // FLIP_INCREMENT_WRITE
        if (subch == 0) pg_method(method, arg);
        return;
    case 0x0130:   // NV097_FLIP_STALL
        nv2a_frames++;
        return;
    case 0x0100:   // NO_OPERATION: with an argument, a software method for the driver
        if (arg && hle_verbose > 0) {
            static uint32_t seen[16]; static int ns;
            int k; for (k = 0; k < ns && seen[k] != arg; k++) {}
            if (k == ns && ns < 16) { seen[ns++] = arg; hle_log("nv2a: software method NOP %x (subch %d)\n", arg, subch); }
        }
        if (arg && (reg(PGRAPH_INTR_EN) & PGRAPH_INTR_ERROR)) {
            // D3D patches this into the pushbuffer to be woken when the GPU
            // gets here (arg 5: "blocked thread may run"). Raise the
            // interrupt the way xemu does and stall until the ISR clears it.
            set_reg(PGRAPH_TRAPPED_ADDR, (uint32_t)(subch & 7) << 16 | (method & 0x1fff));
            set_reg(PGRAPH_TRAPPED_DATA_LOW, arg);
            set_reg(PGRAPH_NSOURCE, 1);   // NOTIFICATION
            set_reg(PGRAPH_INTR, reg(PGRAPH_INTR) | PGRAPH_INTR_ERROR);
            update_irq();
            // until the ISR clears it (nv2a_write signals intr_cleared)
            // and until its DPC lets PGRAPH take methods again (PGRAPH_FIFO):
            // the DPC clears the interrupt before it reads TRAPPED_DATA, so a
            // GPU going on at the clear could trap the next NOP over it, and
            // the lost "blocked thread may run" left D3D waiting for good
            pthread_mutex_lock(&intr_m);
            while (((reg(PGRAPH_INTR) & PGRAPH_INTR_ERROR) || fifo_off) && !guest_stopping) {
                struct timespec ts;   // a host poll, not game time (clock.c)
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 1000000;
                if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
                pthread_cond_timedwait(&intr_cleared, &intr_m, &ts);
            }
            pthread_mutex_unlock(&intr_m);
        }
        return;
    case 0x0110:   // WAIT_FOR_IDLE: the pushbuffer runs in order anyway
        return;
    case 0x1d6c:   // NV097_SET_SEMAPHORE_OFFSET
        semaphore_offset = arg;
        return;
    case 0x1d70: { // NV097_BACK_END_WRITE_SEMAPHORE_RELEASE
        uint32_t b = dma_base(ctx_dma[(0x1a4 - 0x180) / 4], NULL);
        reg_wr32(phys_of(b + semaphore_offset), arg);
        return;
    }
    case 0x17d0: { // NV097_GET_REPORT: a 16-byte report (time, count, done)
        uint32_t b = dma_base(ctx_dma[(0x1a8 - 0x180) / 4], NULL), at = phys_of(b + (arg & 0xffffff));
        uint64_t t = now_ns();
        reg_wr32(at, (uint32_t)t);
        reg_wr32(at + 4, (uint32_t)(t >> 32));
        reg_wr32(at + 8, 640 * 480);   // the pixels that passed: all of them
        reg_wr32(at + 12, 0);
        return;
    }
    default:
        if (subch == 0) {
            pg_method(method, arg);
            return;
        }
        if (unknown_logged < 64 && hle_verbose > 1) {
            unknown_logged++;
            hle_log("nv2a: subch %d obj 0x%x method 0x%04x = 0x%08x\n", subch, subch_object[subch & 7], method, arg);
        }
        return;
    }
}

// commands at physical addresses: D3D's pushbuffer DMA object starts at 0
uint32_t pb_ring[64][3]; unsigned pb_rn;   // the last commands run, for debugging hangs
#define ring pb_ring
#define rn pb_rn
static void run_pushbuffer(void)
{
    uint32_t get = reg(USER_DMA_GET), put = reg(USER_DMA_PUT);
    int budget = 1 << 22;
    while (get != put && budget-- > 0) {
        uint32_t cmd = reg_rd32(phys_of(get));
        ring[rn & 63][0] = get, ring[rn & 63][1] = cmd, ring[rn & 63][2] = put, rn++;
        get += 4;
        if ((cmd & 0xe0000003) == 0x20000000) {          // old jump
            get = cmd & 0x1ffffffc;
            continue;
        }
        if ((cmd & 3) == 1) {                             // jump
            if ((cmd & ~3u) >= RAM_SIZE && hle_verbose > 0) {
                static int jl; if (jl++ < 4) hle_log("nv2a: jump 0x%08x at 0x%08x\n", cmd, get - 4);
            }
            get = cmd & 0xfffffffc;
            continue;
        }
        if ((cmd & 3) == 2) {                             // call
            if ((cmd & ~3u) >= RAM_SIZE && hle_verbose > 0) {
                static int cl; if (cl++ < 4) hle_log("nv2a: call 0x%08x at 0x%08x\n", cmd, get - 4);
            }
            call_return = get;
            set_subroutine(call_return | 1);
            get = cmd & 0xfffffffc;
            continue;
        }
        if (cmd == 0x00020000) {                          // return
            get = call_return;
            set_subroutine(call_return);
            continue;
        }
        uint32_t count = (cmd >> 18) & 0x7ff, method = cmd & 0x1ffc;
        int subch = (cmd >> 13) & 7, ni = (cmd & 0xe0030003) == 0x40000000;
        if ((cmd & 0xe0030003) != 0 && !ni) {
            hle_log("nv2a: bad pushbuffer command 0x%08x at 0x%08x\n", cmd, get - 4);
            static int dumped;
            if (!dumped++ && af_debug_env("CH_PBDUMP")) {
                FILE *f = fopen(af_debug_env("CH_PBDUMP"), "w");
                for (unsigned k = rn - 64; f && k != rn; k++)
                    fprintf(f, "pb %08x: %08x (put %08x)\n", ring[k & 63][0], ring[k & 63][1], ring[k & 63][2]);
                for (uint32_t a = (ring[(rn - 2) & 63][0] & ~15u) - 64; f && a < (ring[(rn - 2) & 63][0] & ~15u) + 128; a += 16) {
                    fprintf(f, "%08x:", a);
                    for (int b = 0; b < 16; b++) fprintf(f, " %02x", M[phys_of(a + b)]);
                    fprintf(f, "  ");
                    for (int b = 0; b < 16; b++) { uint8_t ch = M[phys_of(a + b)]; fputc(ch >= 32 && ch < 127 ? ch : '.', f); }
                    fputc('\n', f);
                }
                if (f) fclose(f);
            }
            break;
        }
        for (uint32_t i = 0; i < count; i++) {
            nv2a_method(subch, ni ? method : method + 4 * i, reg_rd32(phys_of(get)));
            get += 4;
        }
        set_reg(USER_DMA_GET, get);
        put = reg(USER_DMA_PUT);
    }
    set_reg(USER_DMA_GET, get);
    if (budget <= 0) {
        static int logged;
        if (logged++ < 8)
            hle_log("nv2a: pushbuffer still running at 0x%08x (put 0x%08x, word 0x%08x 0x%08x)\n", get, put,
                    reg_rd32(phys_of(get)), reg_rd32(phys_of(get + 4)));
    }
    if (pb_trace) {
        fprintf(pb_trace, "-- kick %08x\n", put);
        fflush(pb_trace);
    }
}

// ---- registers -------------------------------------------------------------------------------------
static uint8_t touched[0x1000];   // 4 KB pages of the 16 MB, first access logged

static uint32_t nv2a_read(uint32_t o)
{
    switch (o) {
    case PMC_INTR_0:
        return pmc_pending();
    case PTIMER_TIME_0:
        return (uint32_t)(now_ns() << 5);   // bits 31:5 are the low nanoseconds
    case PTIMER_TIME_1:
        return (uint32_t)((now_ns() << 5) >> 32) & 0x1fffffff;
    case PGRAPH_STATUS:   // busy while there are commands to run
        return reg(USER_DMA_GET) != reg(USER_DMA_PUT);
    case 0x002400:   // PFIFO_RUNOUT_STATUS: empty
        return 0x10;
    case 0x003214:   // PFIFO_CACHE1_STATUS: empty once the commands have run
        return reg(USER_DMA_GET) == reg(USER_DMA_PUT) ? 0x10 : 0;
    case 0x003220:   // PFIFO_CACHE1_DMA_PUSH: not busy
        return reg(o) & ~0x10u;
    case 0x003240:   // PFIFO_CACHE1_DMA_PUT, _GET: the user channel's
        return reg(USER_DMA_PUT);
    case 0x003244:
        return reg(USER_DMA_GET);
    case 0x10020c:   // PFB_CSTATUS: 128 MB
        return RAM_SIZE;
    }
    return reg(o);
}

static void nv2a_write(uint32_t o, uint32_t v)
{
    switch (o) {
    case PGRAPH_FIFO:
        pthread_mutex_lock(&intr_m);
        set_reg(o, v);
        fifo_off = !(v & 1);
        pthread_cond_broadcast(&intr_cleared);
        pthread_mutex_unlock(&intr_m);
        return;
    case PGRAPH_INTR:
        pthread_mutex_lock(&intr_m);
        set_reg(o, reg(o) & ~v);   // write 1 to clear
        pthread_cond_broadcast(&intr_cleared);   // the GPU thread may be waiting for it
        pthread_mutex_unlock(&intr_m);
        return;
    case PFIFO_INTR_0:
    case PCRTC_INTR_0:
    case PTIMER_INTR_0:
        set_reg(o, reg(o) & ~v);   // write 1 to clear
        return;
    case PFB_WBC:
        set_reg(o, v & ~0x10000u);   // a cache flush is done at once
        return;
    case USER_DMA_PUT:
        pthread_mutex_lock(&gpu_lock);
        set_reg(o, v);
        pthread_cond_signal(&gpu_kick);
        pthread_mutex_unlock(&gpu_lock);
        return;
    }
    set_reg(o, v);
    if (o == PMC_INTR_EN_0 || o == PGRAPH_INTR_EN || o == PFIFO_INTR_EN_0 || o == PCRTC_INTR_EN_0) update_irq();
}

uint32_t nv2a_mmio_read(uint32_t o, int size)
{
    if (!touched[o >> 12] && hle_verbose > 1) {
        touched[o >> 12] = 1;
        hle_log("nv2a: first access to 0x%06x (read)\n", o);
    }
    uint32_t v = nv2a_read(o & ~3u);
    if (size == 4) return v;
    v >>= 8 * (o & 3);
    return size == 1 ? v & 0xff : v & 0xffff;
}

void nv2a_mmio_write(uint32_t o, uint32_t v, int size)
{
    if (!touched[o >> 12] && hle_verbose > 1) {
        touched[o >> 12] = 1;
        hle_log("nv2a: first access to 0x%06x (write 0x%x)\n", o, v);
    }
    if (size != 4) {
        uint32_t w = reg(o & ~3u), sh = 8 * (o & 3), m = (size == 1 ? 0xffu : 0xffffu) << sh;
        v = (w & ~m) | ((v << sh) & m);
    }
    if (pb_trace && (o < PRAMIN || o >= PRAMIN + 0x100000)) fprintf(pb_trace, "w %06x %08x\n", o, v);
    nv2a_write(o & ~3u, v);
}

void nv2a_poll(void)
{
    if (!nv2a_ready) return;
    // vertical blank: a 60th of a second has passed
    uint64_t due = now_ns() * 60 / 1000000000ull;
    if (due > vblanks) {
        vblanks = due;
        set_reg(PCRTC_INTR_0, reg(PCRTC_INTR_0) | 1);
        ch_pc.vblanks++;
    }
    // the interrupt line is level-triggered: a source still pending after
    // the ISR ran (one raised while it ran) interrupts again
    update_irq();
}

// the GPU thread: runs the pushbuffer whenever DMA_PUT moves
static void *gpu_main(void *arg)
{
    (void)arg;
    gpu_init();
    while (!guest_stopping) {
        pthread_mutex_lock(&gpu_lock);
        while (reg(USER_DMA_GET) == reg(USER_DMA_PUT) && !guest_stopping) {
            struct timespec ts;   // a host poll, not game time (clock.c)
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 16000000;
            if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
            pthread_cond_timedwait(&gpu_kick, &gpu_lock, &ts);
            if (reg(USER_DMA_GET) == reg(USER_DMA_PUT)) {
                pthread_mutex_unlock(&gpu_lock);
                gpu_poll_events();
                pthread_mutex_lock(&gpu_lock);
            }
        }
        pthread_mutex_unlock(&gpu_lock);
        if (!guest_stopping) {
            uint64_t t = ch_perf > 0 ? ch_perf_ns() : 0;
            run_pushbuffer();
            if (t) ch_pc.gpu_busy_ns += ch_perf_ns() - t;
        }
    }
    gpu_shutdown();
    return NULL;
}

// the GPU thread ends (guest_stopping is set) and the renderer with it
void nv2a_stop(void)
{
    pthread_mutex_lock(&gpu_lock);
    pthread_cond_broadcast(&gpu_kick);
    pthread_mutex_unlock(&gpu_lock);
    if (gpu_started) pthread_join(gpu_thread, NULL);
    gpu_started = 0;
}

// back to power-on state (the registers are in guest memory, mapped afresh)
void nv2a_reset(void)
{
    nv2a_ready = 0;
    nv2a_frames = 0;
    vblanks = 0;
    memset(subch_object, 0, sizeof subch_object);
    memset(ctx_dma, 0, sizeof ctx_dma);
    memset(touched, 0, sizeof touched);
    semaphore_offset = 0;
    call_return = 0;
    set_subroutine(0);
    fifo_off = 0;
    unknown_logged = 0;
    if (pb_trace) fclose(pb_trace);
    pb_trace = NULL;
    pg_reset();
}
