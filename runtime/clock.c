// The core's one time source. Everything the game can see of time reads it:
// the kernel's interrupt and system time, KeTickCount and its timers, the
// TSC (guest_rdtsc), the NV2A's vblank, PTIMER and report times, and the
// APU's sample clock.
//
// Normal play: the host's clocks, as before.
//
// Render mode (the front end's "clock=frames" option, for video renders that
// run far slower than real time): time runs at the real rate, but stops at the
// end of each frame (1/60 s) until the front end asks for the next one
// (run_frame), and the game's threads are held meanwhile (kthread_freeze). The
// game, the vblank and the sound chip then see 1/60 s per frame however long
// the front end takes, so a frame's sound (800 samples at 48 kHz) comes out
// with its picture. Host-side poll intervals (the GPU thread's, run_frame's
// own) and the profiler stay on the host clock.
#include "xbox.h"
#include <pthread.h>
#include <time.h>

int clk_render;   // render mode, set at create for the whole run

static uint64_t host_mono(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static uint64_t host_real(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

// Render time is origin + vt, vt = min(limit, base_vt + host time since
// base_host). The front end moves limit a frame on; base is set again then, so
// vt carries on from where it stopped. Written under m, read without it
// through the sequence count (odd while being written).
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static uint64_t origin, real_origin;
static volatile uint64_t base_vt, base_host, limit;
static volatile uint32_t seq;
static uint64_t frames;     // frames the front end has asked for
static uint64_t done_at;    // the limit whose end-of-frame tick has run
static volatile int irq_live;   // the interrupt thread runs (it does the end-of-frame tick)

static uint64_t frame_end(uint64_t n)
{
    // rounded up, so the vblank and sample counts (time * 60, * 1500) reach
    // exactly n and 25 n at the end of frame n
    return (n * 1000000000ull + 59) / 60;
}

void clk_init(int render)
{
    pthread_mutex_lock(&m);
    clk_render = render;
    origin = host_mono();
    real_origin = host_real();
    __atomic_add_fetch(&seq, 1, __ATOMIC_SEQ_CST);
    base_vt = base_host = limit = 0;
    __atomic_add_fetch(&seq, 1, __ATOMIC_SEQ_CST);
    frames = done_at = 0;
    pthread_mutex_unlock(&m);
}

// render time since the origin
static uint64_t vt_now(void)
{
    for (;;) {
        uint32_t s = __atomic_load_n(&seq, __ATOMIC_ACQUIRE);
        if (s & 1) continue;
        uint64_t bv = base_vt, bh = base_host, lim = limit;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&seq, __ATOMIC_RELAXED) != s) continue;
        uint64_t h = host_mono();
        uint64_t v = bv + (h > bh ? h - bh : 0);
        return v < lim ? v : lim;
    }
}

uint64_t clk_mono_ns(void) { return clk_render ? origin + vt_now() : host_mono(); }
uint64_t clk_real_ns(void) { return clk_render ? real_origin + vt_now() : host_real(); }

// where a clock that counts from a moment should start: in render mode the
// origin, so 60ths of a second and the APU's frames fall on the frame ends
uint64_t clk_epoch(void) { return clk_render ? origin : host_mono(); }

// host nanoseconds until core time reaches `at` (clk_mono_ns time), at most
// `cap`: 0 once it has; while render time stands at a frame's end, `cap`
int64_t clk_host_wait(uint64_t at, uint64_t cap)
{
    uint64_t now = clk_mono_ns();
    if (now >= at) return 0;
    if (clk_render && now - origin >= limit) return (int64_t)cap;
    uint64_t left = at - now;
    return (int64_t)(left < cap ? left : cap);
}

// ---- render mode: the front end and the interrupt thread -------------------------------------------
// run_frame: one more frame of time
void clk_frame(void)
{
    pthread_mutex_lock(&m);
    uint64_t v = vt_now(), h = host_mono();
    frames++;
    __atomic_add_fetch(&seq, 1, __ATOMIC_SEQ_CST);
    base_vt = v;
    base_host = h;
    limit = frame_end(frames);
    __atomic_add_fetch(&seq, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&m);
}

// render time stands at the end of the frame asked for
int clk_at_limit(void) { return clk_render && vt_now() >= limit; }

// the interrupt thread, once it has run the end-of-frame tick (vblank, sound)
void clk_frame_ticked(void)
{
    pthread_mutex_lock(&m);
    done_at = limit;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&m);
}

void clk_irq_live(int live)
{
    pthread_mutex_lock(&m);
    irq_live = live;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&m);
}

// whether the end-of-frame tick for the current limit is still to run
int clk_tick_due(void)
{
    pthread_mutex_lock(&m);
    int due = clk_render && done_at != limit && vt_now() >= limit;
    pthread_mutex_unlock(&m);
    return due;
}

// run_frame: until the frame's time has run and its end-of-frame tick with it,
// or `stop` is set (the game stopping); `host_cap_ns` bounds the wait (the
// interrupt thread gone). 1 when the frame ran.
int clk_wait_frame(volatile int *stop, uint64_t host_cap_ns)
{
    uint64_t give_up = host_mono() + host_cap_ns;
    pthread_mutex_lock(&m);
    // without the interrupt thread (the game starting or restarting) the
    // frame has run once its time has
    while (done_at != limit && !*stop && !(!irq_live && vt_now() >= limit)) {
        uint64_t now = host_mono();
        if (now >= give_up) break;
        uint64_t left = give_up - now;
        if (left > 2000000) left = 2000000;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (long)left;
        if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
        pthread_cond_timedwait(&cv, &m, &ts);
    }
    int ran = done_at == limit || (!irq_live && vt_now() >= limit);
    pthread_mutex_unlock(&m);
    return ran;
}
