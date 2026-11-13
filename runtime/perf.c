// CH_PERF=1: where the time goes, once a second on stderr. Counters are
// written without locks from the GPU thread and the guest's; close enough.
#include "af_debug.h"
#include "perf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int ch_perf = -1;
ch_perf_counters ch_pc;

uint64_t ch_perf_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t process_cpu_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#include <pthread.h>
__thread uint32_t ch_kcaller, ch_kcaller2;

// small per-second tallies keyed by two words, the top four printed
#define NTALLY 64
typedef struct { uint32_t a, b, n, timeouts; uint8_t type; uint64_t ns; } tally;
static tally waits[NTALLY], calls[NTALLY], wakes[NTALLY];
static pthread_mutex_t tally_m = PTHREAD_MUTEX_INITIALIZER;

static tally *find(tally *t, uint32_t a, uint32_t b)
{
    int free_i = -1;
    for (int i = 0; i < NTALLY; i++) {
        if (t[i].n && t[i].a == a && t[i].b == b) return &t[i];
        if (!t[i].n && free_i < 0) free_i = i;
    }
    if (free_i < 0) return NULL;
    t[free_i].a = a, t[free_i].b = b;
    return &t[free_i];
}

void ch_perf_wait(uint32_t obj, uint8_t type, uint64_t ns, int timed_out)
{
    if (ch_perf <= 0) return;
    pthread_mutex_lock(&tally_m);
    tally *w = find(waits, obj, ch_kcaller);
    if (w) w->type = type, w->n++, w->ns += ns, w->timeouts += timed_out;
    pthread_mutex_unlock(&tally_m);
}

void ch_perf_call(unsigned ord)
{
    if (ch_perf <= 0) return;
    pthread_mutex_lock(&tally_m);
    tally *t = find(calls, ord, ch_kcaller);
    if (t) t->type = 0, t->ns = ch_kcaller2;  // the outer caller (last seen)
    if (t) t->n++;
    pthread_mutex_unlock(&tally_m);
}

void ch_perf_wake(void)
{
    ch_pc.kwakes++;
    if (ch_perf <= 0) return;
    pthread_mutex_lock(&tally_m);
    tally *t = find(wakes, ch_kcaller, 0);
    if (t) t->n++;
    pthread_mutex_unlock(&tally_m);
}

// the four biggest by time (waits) or count (the others), then cleared
static tally *top(tally *t, int by_ns)
{
    tally *best = NULL;
    for (int i = 0; i < NTALLY; i++)
        if (t[i].n && (!best || (by_ns ? t[i].ns > best->ns : t[i].n > best->n))) best = &t[i];
    return best;
}

extern const char *ch_kernel_name(unsigned ord);

static void print_tallies(double s)
{
    tally *t;
    pthread_mutex_lock(&tally_m);
    fprintf(stderr, "chihiro perf: waits:");
    for (int k = 0; k < 6 && (t = top(waits, 1)); k++, t->n = 0)
        if (t->type == 0xff)
            fprintf(stderr, " [sleep %u us from %08x: %.0f/s, took %.2f ms each]", t->a, t->b, t->n / s,
                    t->ns / 1e6 / t->n);
        else
            fprintf(stderr, " [obj %08x type %u from %08x: %.0f/s, %.2f ms each, %.0f%% of the time, %u timeouts]", t->a,
                    t->type, t->b, t->n / s, t->ns / 1e6 / t->n, t->ns / 1e7 / s, t->timeouts);
    fprintf(stderr, "\nchihiro perf: kernel calls:");
    for (int k = 0; k < 4 && (t = top(calls, 0)); k++, t->n = 0)
        fprintf(stderr, " [%s from %08x (%08x): %.0f/s]", ch_kernel_name(t->a), t->b, (uint32_t)t->ns, t->n / s);
    fprintf(stderr, "\nchihiro perf: kernel wakes:");
    for (int k = 0; k < 4 && (t = top(wakes, 0)); k++, t->n = 0)
        fprintf(stderr, " [from %08x: %.0f/s]", t->a, t->n / s);
    fprintf(stderr, "\n");
    memset(waits, 0, sizeof waits), memset(calls, 0, sizeof calls), memset(wakes, 0, sizeof wakes);
    pthread_mutex_unlock(&tally_m);
}

void ch_perf_frame(void)
{
    static uint64_t start, cpu0;
    if (ch_perf < 0) ch_perf = af_debug_env("CH_PERF") && atoi(af_debug_env("CH_PERF")) > 0;
    if (!ch_perf) return;
    uint64_t now = ch_perf_ns();
    ch_pc.frames++;
    if (!start) {
        start = now, cpu0 = process_cpu_ns();
        memset(&ch_pc, 0, sizeof ch_pc);
        return;
    }
    uint64_t span = now - start;
    if (span < 1000000000ull) return;
    double s = span / 1e9, f = ch_pc.frames ? ch_pc.frames : 1;
    uint64_t cpu = process_cpu_ns();
    fprintf(stderr,
            "chihiro perf: %.1f fps | per frame: GPU thread %.1f ms (draws %.1f, flip %.1f) | %u draws, %u vertices,"
            " %.0f KB textures hashed, %u uploads, %u links | %u processor handoffs, %u kernel wakes | process CPU %.0f%%\n",
            ch_pc.frames / s, ch_pc.gpu_busy_ns / f / 1e6, ch_pc.draw_ns / f / 1e6, ch_pc.flip_ns / f / 1e6,
            (unsigned)(ch_pc.draws / f), (unsigned)(ch_pc.vertices / f), ch_pc.tex_hash_bytes / f / 1024,
            (unsigned)(ch_pc.tex_uploads / f + 0.5), ch_pc.prog_links, (unsigned)(ch_pc.gcpu_switches / s),
            (unsigned)(ch_pc.kwakes / s), (cpu - cpu0) / (double)span * 100);
    fprintf(stderr, "chihiro perf: %.1f vblanks/s, %.2f frames per vblank\n", ch_pc.vblanks / s,
            ch_pc.vblanks ? ch_pc.frames / (double)ch_pc.vblanks : 0);
    print_tallies(s);
    memset(&ch_pc, 0, sizeof ch_pc);
    start = now, cpu0 = cpu;
}
