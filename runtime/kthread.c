// Threads, IRQL, dispatcher objects, waits, timers, DPCs and interrupts.
//
// Guest threads are host threads. The Xbox has one processor, and kernel
// code relies on that: raising IRQL to DISPATCH_LEVEL keeps other threads and
// DPCs out. Here that is a recursive "processor lock" taken while a thread's
// IRQL is at DISPATCH_LEVEL or above; DPCs and interrupt routines run under
// it on the interrupt thread.
//
// Dispatcher objects keep the kernel's layouts in guest memory (the game and
// its libraries read SignalState directly). Waiting is a loop under one lock
// on one condition variable, broadcast on every signal.
#define _GNU_SOURCE
#include "perf.h"
#include "xbox.h"
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <stdlib.h>

// object types (DISPATCHER_HEADER.Type)
enum { EventNotification = 0, EventSynchronization = 1, Mutant = 2, Semaphore = 5, Thread = 6,
       TimerNotification = 8, TimerSynchronization = 9 };
#define HDR_TYPE(o) rd8(o)
#define HDR_SIGNAL(o) (o + 4)

// ---- locks ------------------------------------------------------------------------------------
static pthread_mutex_t kmutex = PTHREAD_MUTEX_INITIALIZER;   // object state and waits
static pthread_cond_t kcv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t plock;                               // the processor (IRQL >= DISPATCH_LEVEL)

void kobj_note_set(uint32_t obj);

static __thread uint32_t self_thread;    // ETHREAD of this host thread
static __thread uint32_t self_kpcr;
static __thread int plock_depth;

#define DISPATCH_LEVEL 2

static uint8_t *irql_ptr(void) { return M + self_kpcr + 0x24; }

uint8_t irql_current(void) { return self_kpcr ? *irql_ptr() : 0; }

uint8_t irql_raise(uint8_t to)
{
    uint8_t old = irql_current();
    if (to >= DISPATCH_LEVEL && old < DISPATCH_LEVEL) {
        pthread_mutex_lock(&plock);
        plock_depth++;
    }
    if (self_kpcr) *irql_ptr() = to;
    return old;
}

void irql_lower(uint8_t to)
{
    uint8_t old = irql_current();
    if (self_kpcr) *irql_ptr() = to;
    if (to < DISPATCH_LEVEL && old >= DISPATCH_LEVEL && plock_depth) {
        plock_depth--;
        pthread_mutex_unlock(&plock);
    }
}

void cpu_lock(void) { pthread_mutex_lock(&plock); }
void cpu_unlock(void) { pthread_mutex_unlock(&plock); }

// ---- a game's own "nobody else runs now" lock ------------------------------------------------
// Code written for the Xbox's one processor may keep other threads out by
// raising its own priority (CRI's middleware in HotD3 does: its lock makes
// the caller time-critical). Here the other threads run on other cores, so
// such a lock is patched (games/<set>/patches.txt) to the I/O ports
// PRIO_LOCK_PORT / PRIO_UNLOCK_PORT, which take this recursive mutex. Like
// the raised priority it is given up while its holder waits, as a waiting
// thread lets the others run.
static pthread_mutex_t prio_lock;
static __thread int prio_depth;

void prio_enter(void)
{
    pthread_mutex_lock(&prio_lock);
    prio_depth++;
}

void prio_leave(void)
{
    if (!prio_depth) return;
    prio_depth--;
    pthread_mutex_unlock(&prio_lock);
}

int prio_release_all(void)
{
    int n = prio_depth;
    while (prio_depth) prio_leave();
    return n;
}

void prio_retake(int n)
{
    while (n-- > 0) prio_enter();
}

// ---- one processor (games with GAME_ONE_CORE) ---------------------------------------------------
// The Xbox has one processor, and some game code counts on it: CRI's
// middleware in HotD3 hands work between its threads in ways that only
// hold when they never run at the same moment (its movies froze within
// minutes with the game's threads on several cores). For such a game only
// one guest thread runs guest code at a time: it holds "the processor" and
// gives it up when it waits, suspends itself, yields or ends, and at a
// loop's back edge (GUEST_LOOP in generated code) once its slice is over and
// another thread wants it, or at once when the interrupt thread wants it for
// an interrupt routine or DPC. Threads take turns in the order they asked.
// Not while the holder is at DISPATCH_LEVEL or in a game's priority lock:
// those keep other threads out on the Xbox too. The renderer, sound and
// interrupt threads' own host work runs alongside. CH_ALLCORES=1 turns it
// off. (It replaced pinning to one host core, which macOS cannot do and
// which slowed the renderer down with the game.)
int guest_one_core;
volatile int gcpu_want;
static pthread_mutex_t gcpu_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gcpu_cv = PTHREAD_COND_INITIALIZER;
static int gcpu_busy, gcpu_irq_waiting;
static uint32_t gcpu_next, gcpu_serving;   // turns, in the order asked
static volatile uint32_t gcpu_ticks;       // milliseconds, from the interrupt thread
static uint32_t gcpu_slice_start;
static __thread int gcpu_held;
#define GCPU_SLICE_MS 2

// while stopping it stays set, so a thread spinning in game code (VC3's idle
// loop has no kernel call) reaches gcpu_yield and leaves
static volatile int frozen;   // render mode: the guest's threads held (kthread_freeze)
static void gcpu_want_update(void) { gcpu_want = guest_stopping || frozen || gcpu_irq_waiting || gcpu_next != gcpu_serving; }

static void gcpu_take(void)
{
    if (!guest_one_core || gcpu_held) return;
    pthread_mutex_lock(&gcpu_m);
    uint32_t turn = gcpu_next++;
    gcpu_want_update();
    // stopping: every thread runs on to its next check of guest_stopping
    while ((gcpu_busy || gcpu_irq_waiting || turn != gcpu_serving) && !guest_stopping)
        pthread_cond_wait(&gcpu_cv, &gcpu_m);
    gcpu_serving++;
    gcpu_busy = 1;
    ch_pc.gcpu_switches++;
    gcpu_slice_start = gcpu_ticks;
    gcpu_want_update();
    pthread_mutex_unlock(&gcpu_m);
    gcpu_held = 1;
}

static void gcpu_give(void)
{
    if (!gcpu_held) return;
    gcpu_held = 0;
    pthread_mutex_lock(&gcpu_m);
    gcpu_busy = 0;
    pthread_cond_broadcast(&gcpu_cv);
    pthread_mutex_unlock(&gcpu_m);
}

// the interrupt thread: next in line, ahead of every waiting thread
static void gcpu_take_irq(void)
{
    if (!guest_one_core) return;
    pthread_mutex_lock(&gcpu_m);
    gcpu_irq_waiting = 1;
    gcpu_want_update();
    while (gcpu_busy && !guest_stopping) pthread_cond_wait(&gcpu_cv, &gcpu_m);
    gcpu_busy = 1;
    gcpu_irq_waiting = 0;
    gcpu_want_update();
    pthread_mutex_unlock(&gcpu_m);
    gcpu_held = 1;
}

// ---- render mode: the guest held while time stands still ------------------------------------------
// While render time stands at a frame's end (clock.c) the game's threads stop
// at their next loop back edge (gcpu_want sends it here) or kernel call, and go
// on at the next run_frame: the game then sees time as in play, in 60ths of a
// second, however long the front end takes. Left running, its threads raced on
// with time stopped: Crazy Taxi's CRI movie (Sofdec) froze 1 run in 3 with the
// front end taking 40 ms a frame. The interrupt thread is not held (it runs the
// end-of-frame vblank), nor a thread at DISPATCH_LEVEL or in a game's priority
// lock (others may wait on it).
static pthread_mutex_t freeze_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t freeze_cv = PTHREAD_COND_INITIALIZER;
static __thread int is_irq_thread;

void kthread_freeze(int on)
{
    pthread_mutex_lock(&freeze_m);
    frozen = on;
    if (!on) pthread_cond_broadcast(&freeze_cv);
    pthread_mutex_unlock(&freeze_m);
    pthread_mutex_lock(&gcpu_m);
    gcpu_want_update();
    pthread_mutex_unlock(&gcpu_m);
}

static void freeze_point(void)
{
    if (!frozen || is_irq_thread || plock_depth || prio_depth) return;
    int held = gcpu_held;
    if (held) gcpu_give();   // the interrupt thread may need the processor meanwhile
    pthread_mutex_lock(&freeze_m);
    while (frozen && !guest_stopping) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 10000000;
        if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
        pthread_cond_timedwait(&freeze_cv, &freeze_m, &ts);
    }
    pthread_mutex_unlock(&freeze_m);
    if (held) gcpu_take();
    if (guest_stopping) guest_exit_thread();
}

void kthread_freeze_point(void) { freeze_point(); }

// at a back edge (GUEST_LOOP), when someone wants the processor
// (kthread_stop_all sets gcpu_want, so every loop of every thread ends here
// and the thread leaves)
void gcpu_yield(void)
{
    if (guest_stopping) guest_exit_thread();
    freeze_point();
    if (!gcpu_held || plock_depth || prio_depth) return;
    if (!gcpu_irq_waiting && gcpu_ticks - gcpu_slice_start < GCPU_SLICE_MS) return;
    gcpu_give();
    gcpu_take();
}

// a thread that yields (NtYieldExecution, a zero delay): the next in line runs first
static void gcpu_yield_now(void)
{
    if (!gcpu_held || plock_depth || prio_depth || !gcpu_want) {
        if (!gcpu_held) sched_yield();
        return;
    }
    gcpu_give();
    gcpu_take();
}

void kthread_yield(void) { gcpu_yield_now(); }

// a host sleep inside a kernel call (NtPulseEvent): others may run meanwhile
void kthread_host_sleep(unsigned us)
{
    int give = gcpu_held && !plock_depth && !prio_depth;
    if (give) gcpu_give();
    usleep(us);
    if (give) gcpu_take();
}

// around a wait: give up the locks and the processor, then take them back
// (the processor first)
static int locks_release(int *prio)
{
    int held = plock_depth;
    for (int i = 0; i < held; i++) pthread_mutex_unlock(&plock);
    *prio = prio_depth;
    for (int i = 0; i < *prio; i++) pthread_mutex_unlock(&prio_lock);
    gcpu_give();
    return held;
}

static void locks_retake(int held, int prio)
{
    gcpu_take();
    for (int i = 0; i < prio; i++) pthread_mutex_lock(&prio_lock);
    for (int i = 0; i < held; i++) pthread_mutex_lock(&plock);
}

// ---- threads -------------------------------------------------------------------------------------
// ETHREAD as the kernel lays it out, the parts games read:
//   +0x00 KTHREAD.Header (Type 6)   +0x28 KTHREAD.TlsData
//   +0x130 UniqueThread             +0x134 StartAddress
// and each thread has its own KPCR (fs): +0x00 NtTib.ExceptionList,
// +0x04 StackBase, +0x08 StackLimit, +0x18 Self, +0x1C SelfPcr, +0x20 Prcb,
// +0x24 Irql, +0x28 Prcb.CurrentThread
#define ETHREAD_SIZE 0x200
#define KPCR_SIZE 0x300

typedef struct hthread {
    CPU *cpu;
    uint32_t ethread, start, ctx1, ctx2, system_routine, stack_top;
    int suspended;
    // A thread that suspends itself until another resumes it (CRI's file
    // server thread in HotD3, woken each frame): on the Xbox's one processor
    // it always suspends before the other thread runs, here the resume can
    // come first and would be lost. Such a resume is kept for the thread's
    // next suspension of itself, which then returns at once.
    int resume_early;
    pthread_cond_t resume;
    pthread_t pt;
    int started;          // pt is a thread to join
} hthread;

#define MAX_THREADS 256
static hthread threads[MAX_THREADS];
static int nthreads;

static uint32_t make_kpcr(uint32_t ethread, uint32_t stack_top, uint32_t stack_size)
{
    uint32_t k = hostdata_alloc(KPCR_SIZE);
    wr32(k + 0x00, 0xffffffff);      // no exception handler yet
    wr32(k + 0x04, stack_top);
    wr32(k + 0x08, stack_top - stack_size);
    wr32(k + 0x18, k);
    wr32(k + 0x1C, k);
    wr32(k + 0x20, k + 0x28);
    wr8(k + 0x24, 0);
    wr32(k + 0x28, ethread);
    return k;
}

static uint32_t make_ethread(uint32_t tls_size)
{
    uint32_t t = hostdata_alloc(ETHREAD_SIZE);
    wr8(t, Thread);
    wr8(t + 2, ETHREAD_SIZE / 4);
    wr32(t + 8, t + 8);              // empty wait list
    wr32(t + 12, t + 8);
    // TLS: the XBE's template, then zero fill; TlsData points past a slot
    // for the index (the SDK's code reads [TlsData + index*4] on Windows but
    // on the Xbox reads TlsData directly)
    uint32_t start = 0, end = 0, zero = 0;
    if (xbe.tls) {
        start = rd32(xbe.tls), end = rd32(xbe.tls + 4), zero = rd32(xbe.tls + 16);
    }
    uint32_t size = end - start + zero;
    if (tls_size > size) size = tls_size;
    if (size) {
        uint32_t p = hostdata_alloc((size + 15) & ~15u);
        if (end > start) memcpy(M + p, M + start, end - start);
        wr32(t + 0x28, p);
    }
    return t;
}

uint32_t kthread_current(void) { return self_thread; }

static void bind_thread(CPU *c, uint32_t ethread, uint32_t stack_top, uint32_t stack_size)
{
    self_thread = ethread;
    self_kpcr = make_kpcr(ethread, stack_top, stack_size);
    c->fs_base = self_kpcr;
    cpu_set_current(c);
}

void kthread_init(CPU *main)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&plock, &a);
    pthread_mutex_init(&prio_lock, &a);
    uint32_t top = stack_alloc(0x100000);
    main->r[ESP] = top;
    uint32_t t = make_ethread(0);
    wr32(t + 0x130, 1);
    wr32(t + 0x134, xbe.entry);
    threads[0].cpu = main;
    threads[0].ethread = t;
    pthread_cond_init(&threads[0].resume, NULL);
    nthreads = 1;
    bind_thread(main, t, top, 0x100000);
    gcpu_take();
}

// the entry point returned: the main thread runs no more guest code
void kthread_main_done(void) { gcpu_give(); }

static void *thread_main(void *p)
{
    hthread *h = p;
    bind_thread(h->cpu, h->ethread, h->stack_top, 0x10000);
    pthread_mutex_lock(&kmutex);
    while (h->suspended && !guest_stopping) pthread_cond_wait(&h->resume, &kmutex);
    pthread_mutex_unlock(&kmutex);
    if (guest_stopping) guest_exit_thread();
    gcpu_take();
    if (h->system_routine) {
        uint32_t args[2] = {h->start, h->ctx1};
        guest_call(h->cpu, h->system_routine, 2, args);
    } else {
        uint32_t args[2] = {h->ctx1, h->ctx2};
        guest_call(h->cpu, h->start, 2, args);
    }
    kthread_exit(0);
}

uint32_t kthread_create(uint32_t start, uint32_t ctx1, uint32_t ctx2, uint32_t stack, int suspended,
                        uint32_t system_routine)
{
    pthread_mutex_lock(&kmutex);
    if (nthreads == MAX_THREADS) host_fatal("too many threads");
    hthread *h = &threads[nthreads++];
    pthread_mutex_unlock(&kmutex);
    if (stack < 0x10000) stack = 0x10000;
    h->cpu = cpu_new();
    h->stack_top = stack_alloc(stack + 0x10000);
    h->cpu->r[ESP] = h->stack_top;
    h->ethread = make_ethread(0);
    h->start = start, h->ctx1 = ctx1, h->ctx2 = ctx2, h->system_routine = system_routine;
    h->suspended = suspended;
    wr32(h->ethread + 0x130, (uint32_t)(h - threads) + 1);
    wr32(h->ethread + 0x134, start);
    pthread_cond_init(&h->resume, NULL);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 << 20);    // host stack: generated code nests deep
    h->started = !pthread_create(&h->pt, &attr, thread_main, h);
    pthread_attr_destroy(&attr);
    if (hle_verbose) hle_log("thread %d: start 0x%x ctx 0x%x%s\n", (int)(h - threads) + 1, start, ctx1,
                             suspended ? " (suspended)" : "");
    return h->ethread;
}

static hthread *thread_of(uint32_t ethread)
{
    for (int i = 0; i < nthreads; i++)
        if (threads[i].ethread == ethread) return &threads[i];
    return NULL;
}

int kthread_resume(uint32_t ethread)
{
    pthread_mutex_lock(&kmutex);
    hthread *h = thread_of(ethread);
    int was = h ? h->suspended : 0;
    if (h && h->suspended) {
        h->suspended--;
        if (!h->suspended) pthread_cond_signal(&h->resume);
    } else if (h)
        h->resume_early = 1;

    pthread_mutex_unlock(&kmutex);
    return was;
}

// A suspended thread stops at its next kernel call (a thread suspending
// itself stops at once): host threads cannot be stopped between two guest
// instructions, and the game's code only relies on the thread not running on
static __thread int my_index = -1;

int kthread_suspend(uint32_t ethread)
{
    pthread_mutex_lock(&kmutex);
    hthread *h = thread_of(ethread);
    int was = h ? h->suspended : 0;
    if (h && ethread == self_thread && h->resume_early) {
        // resumed before it got here: see resume_early
        h->resume_early = 0;
        pthread_mutex_unlock(&kmutex);
        return 0;
    }
    if (h) h->suspended++;
    pthread_mutex_unlock(&kmutex);
    if (h && ethread == self_thread) kthread_check_suspend();
    return was;
}

void kthread_check_suspend(void)
{
    if (my_index < 0) {
        hthread *h = thread_of(self_thread);
        if (!h) return;
        my_index = (int)(h - threads);
    }
    hthread *h = &threads[my_index];
    if (!h->suspended) return;
    // a thread holding the processor lock would stop every other thread with it
    int prio, depth = locks_release(&prio);
    pthread_mutex_lock(&kmutex);
    while (h->suspended && !guest_stopping) pthread_cond_wait(&h->resume, &kmutex);
    pthread_mutex_unlock(&kmutex);
    locks_retake(depth, prio);
    if (guest_stopping) guest_exit_thread();
}

void kthread_exit(uint32_t status)
{
    while (plock_depth) {
        plock_depth--;
        pthread_mutex_unlock(&plock);
    }
    while (prio_depth) prio_leave();
    gcpu_give();
    pthread_mutex_lock(&kmutex);
    if (self_thread) {
        wr32(HDR_SIGNAL(self_thread), 1);
        kobj_note_set(self_thread);
        wr32(self_thread + 0x120, status);
    }
    ch_perf_wake(), pthread_cond_broadcast(&kcv);
    pthread_mutex_unlock(&kmutex);
    pthread_exit(NULL);
}

// ---- time ---------------------------------------------------------------------------------------
// both from the core's clock (clock.c): the host's, or render time
static uint64_t boot_ns;

uint64_t ke_interrupt_time(void)
{
    return (clk_mono_ns() - boot_ns) / 100;
}

uint64_t ke_system_time(void)
{
    return clk_real_ns() / 100 + 116444736000000000ull;
}

// ---- waits --------------------------------------------------------------------------------------
// A notification object (event, timer, thread) releases every thread waiting on it at the moment
// it is set, as on the real kernel. Here a waiter only looks once it runs again, and by then another
// thread may have reset the object: D3D's BlockUntilVerticalBlank clears the vblank event before it
// waits, so a second thread lost that vblank and waited for the next (VC3 drew every other one).
// Each set is counted; a waiter whose object was set since it began goes.
#define NSETS 1024
static struct { uint32_t obj, n; } nsets[NSETS];

static int notification(uint32_t o)
{
    uint8_t t = HDR_TYPE(o);
    return t == EventNotification || t == TimerNotification || t == Thread;
}

// under kmutex
static uint32_t *nset_slot(uint32_t o, int add)
{
    uint32_t h = (o * 2654435761u) >> 22;
    for (int i = 0; i < NSETS; i++, h = (h + 1) & (NSETS - 1)) {
        if (nsets[h].obj == o) return &nsets[h].n;
        if (!nsets[h].obj) {
            if (!add) return NULL;
            nsets[h].obj = o;
            return &nsets[h].n;
        }
    }
    return NULL;
}

static uint32_t nset_count(uint32_t o)
{
    uint32_t *p = nset_slot(o, 0);
    return p ? *p : 0;
}

// under kmutex, after setting a notification object
void kobj_note_set(uint32_t o)
{
    if (!notification(o)) return;
    uint32_t *p = nset_slot(o, 1);
    if (p) (*p)++;
}

// under kmutex: can this thread take the object now?
static int ready(uint32_t o)
{
    int32_t s = (int32_t)rd32(HDR_SIGNAL(o));
    if (HDR_TYPE(o) == Mutant) return s > 0 || rd32(o + 24) == self_thread;
    return s > 0;
}

// under kmutex: what taking it does
static void take(uint32_t o)
{
    switch (HDR_TYPE(o)) {
    case EventSynchronization:
    case TimerSynchronization:
        wr32(HDR_SIGNAL(o), 0);
        break;
    case Semaphore:
        wr32(HDR_SIGNAL(o), rd32(HDR_SIGNAL(o)) - 1);
        break;
    case Mutant:
        wr32(HDR_SIGNAL(o), rd32(HDR_SIGNAL(o)) - 1);
        wr32(o + 24, self_thread);
        break;
    }
}

// render mode, under kmutex: wait for a signal or for core time to reach
// due_ns; 1 when it has. Core time stands still at a frame's end, so the host
// wait is short and the time is looked at again.
static int wait_core_time(uint64_t due_ns)
{
    int64_t left = clk_host_wait(due_ns, 2000000);
    if (left <= 0) return 1;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)left;
    if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
    pthread_cond_timedwait(&kcv, &kmutex, &ts);
    return clk_mono_ns() >= due_ns;
}

uint32_t ke_wait(int count, const uint32_t *objects, int wait_all, int64_t *timeout, int alertable)
{
    struct timespec deadline;
    uint64_t due_ns = 0;   // render mode: the deadline in core time
    int timed = timeout != NULL;
    int64_t asked = timeout ? *timeout : 0;   // for CH_PERF
    if (timed) {
        int64_t t = *timeout;
        uint64_t rel;
        if (t < 0) rel = (uint64_t)-t;
        else {
            uint64_t now = ke_system_time();
            rel = (uint64_t)t > now ? (uint64_t)t - now : 0;
        }
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += rel / 10000000ull;
        deadline.tv_nsec += (rel % 10000000ull) * 100;
        if (deadline.tv_nsec >= 1000000000) deadline.tv_sec++, deadline.tv_nsec -= 1000000000;
        due_ns = clk_mono_ns() + rel * 100;
    }
    // a thread at DISPATCH_LEVEL that waits gives up the processor meanwhile
    int prio, held = locks_release(&prio);
    pthread_mutex_lock(&kmutex);
    uint32_t result = STATUS_TIMEOUT;
    uint64_t wait_t0 = ch_perf > 0 ? ch_perf_ns() : 0;
    int blocked = 0;
    if (count > 64) count = 64;
    uint32_t sets[64];
    for (int i = 0; i < count; i++) sets[i] = notification(objects[i]) ? nset_count(objects[i]) : 0;
#define FIRED(i) (ready(objects[i]) || (notification(objects[i]) && nset_count(objects[i]) != sets[i]))
    for (;;) {
        if (guest_stopping) {
            pthread_mutex_unlock(&kmutex);
            guest_exit_thread();
        }
        if (wait_all) {
            int all = 1;
            for (int i = 0; i < count; i++) all &= FIRED(i);
            if (all) {
                for (int i = 0; i < count; i++) take(objects[i]);
                result = STATUS_SUCCESS;
                break;
            }
        } else {
            int hit = -1;
            for (int i = 0; i < count && hit < 0; i++)
                if (FIRED(i)) hit = i;
            if (hit >= 0) {
                take(objects[hit]);
                result = STATUS_SUCCESS + hit;
                break;
            }
        }
        if (timed) {
            if (timeout && *timeout == 0) break;
            if (clk_render ? wait_core_time(due_ns) : pthread_cond_timedwait(&kcv, &kmutex, &deadline) != 0) {
                // one last look after the timeout
                timed = 2;
                timeout = NULL;
                int any = 0;
                for (int i = 0; i < count; i++) any |= FIRED(i);
                if (!any || wait_all) break;
            }
        } else pthread_cond_wait(&kcv, &kmutex);
        blocked = 1;
        if (timed == 2) {
            result = STATUS_TIMEOUT;
            break;
        }
    }
#undef FIRED
    pthread_mutex_unlock(&kmutex);
    if (wait_t0 && count == 0)   // a sleep: what was asked (in us) and how long it took
        ch_perf_wait((uint32_t)((asked < 0 ? -asked : 0) / 10), 0xff, ch_perf_ns() - wait_t0, 0);
    else if (wait_t0 && blocked && count > 0)
        ch_perf_wait(objects[0], rd8(objects[0]), ch_perf_ns() - wait_t0, result == STATUS_TIMEOUT);
    locks_retake(held, prio);
    return result;
}

void ke_signal(uint32_t obj)
{
    (void)obj;
    pthread_mutex_lock(&kmutex);
    ch_perf_wake(), pthread_cond_broadcast(&kcv);
    pthread_mutex_unlock(&kmutex);
}

void ke_set_event(uint32_t ev)
{
    pthread_mutex_lock(&kmutex);
    wr32(HDR_SIGNAL(ev), 1);
    kobj_note_set(ev);
    ch_perf_wake(), pthread_cond_broadcast(&kcv);
    pthread_mutex_unlock(&kmutex);
}

// object state changes from kernel.c go through these so they are atomic with waits
void kobj_lock(void) { pthread_mutex_lock(&kmutex); }
// for a change that frees nothing a thread could be waiting on
void kobj_unlock(void) { pthread_mutex_unlock(&kmutex); }
void kobj_unlock_signal(void)
{
    ch_perf_wake(), pthread_cond_broadcast(&kcv);
    pthread_mutex_unlock(&kmutex);
}

// ---- DPCs and timers -----------------------------------------------------------------------------
// KDPC: +0 Type, +2 Inserted, +12 DeferredRoutine, +16 DeferredContext,
// +20 SystemArgument1, +24 SystemArgument2
#define MAX_DPC 256
static uint32_t dpc_queue[MAX_DPC];
static int dpc_count;
static pthread_mutex_t dpc_lock = PTHREAD_MUTEX_INITIALIZER;

void ke_insert_dpc(uint32_t dpc, uint32_t a1, uint32_t a2)
{
    pthread_mutex_lock(&dpc_lock);
    if (!rd8(dpc + 2) && dpc_count < MAX_DPC) {
        wr8(dpc + 2, 1);
        wr32(dpc + 20, a1);
        wr32(dpc + 24, a2);
        dpc_queue[dpc_count++] = dpc;
    }
    pthread_mutex_unlock(&dpc_lock);
}

int ke_remove_dpc(uint32_t dpc)
{
    int found = 0;
    pthread_mutex_lock(&dpc_lock);
    for (int i = 0; i < dpc_count; i++)
        if (dpc_queue[i] == dpc) {
            memmove(dpc_queue + i, dpc_queue + i + 1, (dpc_count - i - 1) * 4);
            dpc_count--;
            found = 1;
            break;
        }
    wr8(dpc + 2, 0);
    pthread_mutex_unlock(&dpc_lock);
    return found;
}

// KTIMER: +0 header, +16 DueTime, +24 TimerListEntry, +32 Dpc, +36 Period
typedef struct { uint32_t timer, dpc; uint64_t due; uint32_t period_ms; } htimer;
#define MAX_TIMERS 128
static htimer timers[MAX_TIMERS];
static int ntimers;

void ke_set_timer(uint32_t timer, int64_t due, int32_t period_ms, uint32_t dpc)
{
    uint64_t now = ke_interrupt_time();
    uint64_t at = due < 0 ? now + (uint64_t)-due : (uint64_t)due > ke_system_time() ? now + (uint64_t)due - ke_system_time() : now;
    pthread_mutex_lock(&kmutex);
    int i;
    for (i = 0; i < ntimers && timers[i].timer != timer; i++)
        ;
    if (i == ntimers) {
        if (ntimers == MAX_TIMERS) host_fatal("too many timers");
        ntimers++;
    }
    timers[i] = (htimer){timer, dpc, at, (uint32_t)period_ms};
    wr32(HDR_SIGNAL(timer), 0);
    wr8(timer + 3, 1);           // Inserted
    wr32(timer + 32, dpc);
    wr32(timer + 36, period_ms);
    pthread_mutex_unlock(&kmutex);
}

int ke_cancel_timer(uint32_t timer)
{
    // On the Xbox's one processor a timer's DPC has run before any thread
    // could cancel it. Here the interrupt thread runs alongside, so take the
    // processor (no DPC is mid-run) and drop a DPC still queued for this
    // timer: the KTIMER and KDPC are often on the canceller's stack (the USB
    // library's 10 ms request timeout), and a late DPC would run on whatever
    // the stack holds by then.
    uint8_t irql = irql_raise(DISPATCH_LEVEL);
    int found = 0;
    pthread_mutex_lock(&kmutex);
    for (int i = 0; i < ntimers; i++)
        if (timers[i].timer == timer) {
            timers[i] = timers[--ntimers];
            found = 1;
            break;
        }
    wr8(timer + 3, 0);
    uint32_t dpc = rd32(timer + 32);
    pthread_mutex_unlock(&kmutex);
    if (dpc) {
        pthread_mutex_lock(&dpc_lock);
        for (int i = 0; i < dpc_count; i++)
            if (dpc_queue[i] == dpc) {
                memmove(dpc_queue + i, dpc_queue + i + 1, (dpc_count - i - 1) * 4);
                dpc_count--;
                wr8(dpc + 2, 0);
                break;
            }
        pthread_mutex_unlock(&dpc_lock);
    }
    irql_lower(irql);
    return found;
}

static void run_dpcs(CPU *c)
{
    for (;;) {
        pthread_mutex_lock(&dpc_lock);
        if (!dpc_count) {
            pthread_mutex_unlock(&dpc_lock);
            return;
        }
        uint32_t d = dpc_queue[0];
        memmove(dpc_queue, dpc_queue + 1, (--dpc_count) * 4);
        wr8(d + 2, 0);
        pthread_mutex_unlock(&dpc_lock);
        uint32_t args[4] = {d, rd32(d + 16), rd32(d + 20), rd32(d + 24)};
        guest_call(c, rd32(d + 12), 4, args);
    }
}

// ---- interrupts ---------------------------------------------------------------------------------
// KINTERRUPT: +0 ServiceRoutine, +4 ServiceContext, +8 BusInterruptLevel,
// +12 Irql, +16 Connected
#define MAX_VECTORS 64
static uint32_t vectors[MAX_VECTORS];
static volatile uint32_t pending_irq;

void ke_connect_interrupt(uint32_t vector, uint32_t kint)
{
    if (vector < MAX_VECTORS) vectors[vector] = kint;
}

// the interrupt thread sleeps until its next millisecond tick or until an
// interrupt is raised, whichever comes first: an interrupt (the GPU's, which
// the game's frame loop waits on) runs at once instead of up to a tick later
static pthread_mutex_t irq_wake_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t irq_wake_cv;
static int irq_wake_ready;

void ke_raise_interrupt(uint32_t vector)
{
    __atomic_or_fetch(&pending_irq, 1u << (vector & 31), __ATOMIC_SEQ_CST);
    pthread_mutex_lock(&irq_wake_m);
    if (irq_wake_ready) pthread_cond_signal(&irq_wake_cv);
    pthread_mutex_unlock(&irq_wake_m);
}

static uint64_t mono_ns(void) { return clk_mono_ns(); }

// the host's clock, for the one-processor time slices: host scheduling, not
// game time (in render mode game time stands still between frames)
static uint64_t host_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

void kthread_clock_kick(void)
{
    pthread_mutex_lock(&irq_wake_m);
    if (irq_wake_ready) pthread_cond_signal(&irq_wake_cv);
    pthread_mutex_unlock(&irq_wake_m);
}

// render mode: until core time reaches the next tick, an interrupt comes in or
// the frame's end needs its tick, for at most a host millisecond (while time
// stands at a frame's end the loop still runs each millisecond, for the DPCs
// the game queues). Every tick runs, none skipped: core time only moves at the
// real rate or stands still. 1 when the tick is due.
static int irq_sleep_render(uint64_t *next_tick, uint64_t host_start)
{
    pthread_mutex_lock(&irq_wake_m);
    if (!guest_stopping && !__atomic_load_n(&pending_irq, __ATOMIC_SEQ_CST) && mono_ns() < *next_tick &&
        !clk_tick_due()) {
        int64_t left = clk_host_wait(*next_tick, 1000000);
#ifdef __APPLE__
        struct timespec ts = { 0, (long)left };
        if (left > 0) pthread_cond_timedwait_relative_np(&irq_wake_cv, &irq_wake_m, &ts);
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_nsec += (long)left;
        if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
        if (left > 0) pthread_cond_timedwait(&irq_wake_cv, &irq_wake_m, &ts);
#endif
    }
    pthread_mutex_unlock(&irq_wake_m);
    gcpu_ticks = (uint32_t)(host_ms() - host_start);
    if (mono_ns() < *next_tick) return 0;
    *next_tick += 1000000;
    return 1;
}

// until the next tick (1 ms after the last) or an interrupt; 1 when the tick is due
static int irq_sleep(uint64_t *next_tick)
{
    pthread_mutex_lock(&irq_wake_m);
    uint64_t t;
    while (!guest_stopping && !__atomic_load_n(&pending_irq, __ATOMIC_SEQ_CST) && (t = mono_ns()) < *next_tick) {
#ifdef __APPLE__
        // no monotonic condition clock here: an absolute monotonic time read
        // as wall-clock time is long past, and the wait returned at once (the
        // thread spun on the clock, most of a core on the Mac). The clock is
        // read once: a second read past the tick made left wrap to ~585
        // years, and the interrupt thread slept until something signalled it
        // (no vblanks, timers or DPCs: Crazy Taxi froze at boot ~1 in 30)
        uint64_t left = *next_tick - t;
        struct timespec ts = { (time_t)(left / 1000000000ull), (long)(left % 1000000000ull) };
        pthread_cond_timedwait_relative_np(&irq_wake_cv, &irq_wake_m, &ts);
#else
        struct timespec ts = { (time_t)(*next_tick / 1000000000ull), (long)(*next_tick % 1000000000ull) };
        pthread_cond_timedwait(&irq_wake_cv, &irq_wake_m, &ts);
#endif
    }
    pthread_mutex_unlock(&irq_wake_m);
    uint64_t now = mono_ns();
    if (now < *next_tick) return 0;
    *next_tick += 1000000;
    if (*next_tick < now) *next_tick = now + 1000000;   // fell behind: don't catch up in a burst
    return 1;
}

void ohci_frame(void);
static CPU *irq_cpu;

// the interrupt thread: the clock, timers, interrupts and DPCs
static void *interrupt_main(void *p)
{
    (void)p;
    irq_cpu = cpu_new();
    irq_cpu->r[ESP] = stack_alloc(0x40000);
    uint32_t t = make_ethread(0);
    bind_thread(irq_cpu, t, irq_cpu->r[ESP], 0x40000);
    pthread_mutex_lock(&irq_wake_m);
    if (!irq_wake_ready) {
        pthread_condattr_t ca;
        pthread_condattr_init(&ca);
#ifndef __APPLE__
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
#endif
        pthread_cond_init(&irq_wake_cv, &ca);
        pthread_condattr_destroy(&ca);
        irq_wake_ready = 1;
    }
    pthread_mutex_unlock(&irq_wake_m);
    uint64_t next_tick = mono_ns() + 1000000, host_start = host_ms();
    is_irq_thread = 1;
    clk_irq_live(1);
    while (!guest_stopping) {
        if (clk_render ? irq_sleep_render(&next_tick, host_start) : irq_sleep(&next_tick)) {
            kernel_tick();
            nv2a_poll();
            ohci_frame();
            apu_tick();
            if (!clk_render) gcpu_ticks++;
        }
        // render mode: time stands at the frame's end; its vblank and the
        // sound up to it come now, so the frame's sound is all there (800
        // samples) when run_frame returns
        int frame_end = clk_render && clk_tick_due();
        if (frame_end) {
            kernel_tick();
            nv2a_poll();
            apu_tick();
        }
        uint64_t now = ke_interrupt_time();
        // with one processor (GAME_ONE_CORE): it, before the lock below,
        // when an interrupt routine or a DPC (game code) is to run
        int guest_work = 0;
        if (guest_one_core) {
            guest_work = pending_irq != 0;
            pthread_mutex_lock(&dpc_lock);
            guest_work |= dpc_count != 0;
            pthread_mutex_unlock(&dpc_lock);
            pthread_mutex_lock(&kmutex);
            for (int i = 0; i < ntimers && !guest_work; i++) guest_work = timers[i].due <= now && timers[i].dpc;
            pthread_mutex_unlock(&kmutex);
            if (guest_work) gcpu_take_irq();
        }
        // the processor from here on: timers, their DPCs, interrupts and the
        // DPC queue run as one block, so a thread cancelling a timer (which
        // takes the processor too) never sees a fired timer whose DPC is
        // not yet queued, or one whose DPC is running
        pthread_mutex_lock(&plock);
        plock_depth++;
        // timers
        uint32_t fire_dpc[MAX_TIMERS];
        uint32_t fire_arg[MAX_TIMERS][2];
        int nfire = 0;
        pthread_mutex_lock(&kmutex);
        int signalled = 0;
        for (int i = 0; i < ntimers; i++) {
            if (timers[i].due > now) continue;
            wr32(HDR_SIGNAL(timers[i].timer), 1);
            kobj_note_set(timers[i].timer);
            signalled = 1;
            if (timers[i].dpc) {
                fire_dpc[nfire] = timers[i].dpc;
                fire_arg[nfire][0] = (uint32_t)now;
                fire_arg[nfire][1] = (uint32_t)(now >> 32);
                nfire++;
            }
            if (timers[i].period_ms) timers[i].due += timers[i].period_ms * 10000ull;
            else {
                wr8(timers[i].timer + 3, 0);
                timers[i--] = timers[--ntimers];
            }
        }
        if (signalled) ch_perf_wake(), pthread_cond_broadcast(&kcv);
        pthread_mutex_unlock(&kmutex);
        for (int i = 0; i < nfire; i++) ke_insert_dpc(fire_dpc[i], fire_arg[i][0], fire_arg[i][1]);
        // interrupts
        // (without the processor, what came in since the look above waits a tick)
        int may_run = !guest_one_core || guest_work;
        uint32_t irq = may_run ? __atomic_exchange_n(&pending_irq, 0, __ATOMIC_SEQ_CST) : 0;
        for (int v = 0; v < 32; v++) {
            if (!(irq >> v & 1) || !vectors[v]) continue;
            uint32_t k = vectors[v];
            uint8_t old = *irql_ptr();
            *irql_ptr() = rd8(k + 12) ? rd8(k + 12) : 0x1a;
            uint32_t args[2] = {k, rd32(k + 4)};
            uint32_t handled = guest_call(irq_cpu, rd32(k), 2, args);
            if (hle_verbose > 2) hle_log("isr %d: 0x%x -> %u\n", v, rd32(k), handled & 0xff);
            *irql_ptr() = old;
        }
        *irql_ptr() = DISPATCH_LEVEL;
        if (may_run) run_dpcs(irq_cpu);
        *irql_ptr() = 0;
        plock_depth--;
        pthread_mutex_unlock(&plock);
        if (guest_work) gcpu_give();
        if (frame_end) {
            kthread_freeze(1);   // until run_frame asks for the next frame
            clk_frame_ticked();
        }
    }
    clk_irq_live(0);
    return NULL;
}

static pthread_t irq_thread;
static int irq_started;

void kobj_start(void)
{
    boot_ns = clk_mono_ns();
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 << 20);
    irq_started = !pthread_create(&irq_thread, &attr, interrupt_main, NULL);
    pthread_attr_destroy(&attr);
}

// ---- stopping and starting again (the core library: runtime/core_api.c) ---------------------------
// With guest_stopping set, every guest thread leaves at its next wait,
// kernel call, suspension or loop back edge (gcpu_want sends each back edge
// to gcpu_yield). Wake every wait, then join every thread but the main
// one, which the caller of run_game joins.
void kthread_stop_all(void)
{
    guest_stopping = 1;
    gcpu_want = 1;
    for (int pass = 0; pass < 2; pass++) {
        pthread_mutex_lock(&kmutex);
        ch_perf_wake(), pthread_cond_broadcast(&kcv);
        for (int i = 0; i < nthreads; i++) pthread_cond_broadcast(&threads[i].resume);
        pthread_mutex_unlock(&kmutex);
        pthread_mutex_lock(&gcpu_m);
        pthread_cond_broadcast(&gcpu_cv);
        pthread_mutex_unlock(&gcpu_m);
        if (pass) break;
        // threads started while stopping exit as they start (thread_main)
        pthread_mutex_lock(&kmutex);
        int n = nthreads;
        pthread_mutex_unlock(&kmutex);
        for (int i = 1; i < n; i++) {
            if (threads[i].started) pthread_join(threads[i].pt, NULL);
            threads[i].started = 0;
        }
    }
    if (irq_started) pthread_join(irq_thread, NULL);
    irq_started = 0;
}

// back to power-on state, every thread gone
void kthread_reset(void)
{
    for (int i = 0; i < nthreads; i++) {
        pthread_cond_destroy(&threads[i].resume);
        free(threads[i].cpu);
    }
    memset(threads, 0, sizeof threads);
    nthreads = 0;
    free(irq_cpu);
    irq_cpu = NULL;
    ntimers = 0;
    dpc_count = 0;
    memset(vectors, 0, sizeof vectors);
    pending_irq = 0;
    gcpu_want = 0;
    frozen = 0;
    gcpu_busy = gcpu_irq_waiting = 0;
    gcpu_next = gcpu_serving = 0;
    gcpu_ticks = 0;
    gcpu_slice_start = 0;
    pthread_mutex_destroy(&plock);
    pthread_mutex_destroy(&prio_lock);
}

void kobj_poll(void) {}
