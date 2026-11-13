// CH_PERF=1: once a second on stderr, where a frame's time goes (perf.c).
#pragma once
#include <stdint.h>

typedef struct {
    uint64_t gpu_busy_ns;     // the GPU thread running the pushbuffer
    uint64_t draw_ns;         // in gpu_draw (state, uniforms, vertex upload, the GL call)
    uint64_t flip_ns;         // in gpu_flip (the picture read back and handed over)
    uint64_t tex_hash_bytes;  // texture memory hashed to see if it changed
    uint32_t draws, vertices, tex_uploads, prog_links, frames;
    uint32_t gcpu_switches;   // one-processor handoffs (GAME_ONE_CORE)
    uint32_t kwakes;          // kernel object signals (every waiting thread wakes)
    uint32_t vblanks;         // vertical blank interrupts raised
} ch_perf_counters;

extern int ch_perf;
extern ch_perf_counters ch_pc;
uint64_t ch_perf_ns(void);
void ch_perf_frame(void);   // from gpu_flip: prints once a second
// a kernel wait that blocked: its first object, type byte, how long, timed out
void ch_perf_wait(uint32_t obj, uint8_t type, uint64_t ns, int timed_out);
// the guest return address of the kernel call this thread is in (0 outside one)
extern __thread uint32_t ch_kcaller;
// and its caller's, when the caller keeps an ebp frame (XAPI wrappers do)
extern __thread uint32_t ch_kcaller2;
// a kernel call (by ordinal) and a kernel object signal, counted by ch_kcaller
void ch_perf_call(unsigned ord);
void ch_perf_wake(void);
