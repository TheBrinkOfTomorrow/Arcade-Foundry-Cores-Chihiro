// The Xbox kernel's exports, by ordinal: what the game's thunk table points at.
// Structures keep the kernel's layouts; handles, memory and threads map onto
// kthread.c and the physical page allocator in core.c.
#define _GNU_SOURCE
#include "xbox.h"
#include "kernel_names.h"
#include "perf.h"
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void kobj_lock(void);
void kobj_unlock_signal(void);
void kobj_unlock(void);
int ke_remove_dpc(uint32_t dpc);
int kthread_resume(uint32_t ethread);
int kthread_suspend(uint32_t ethread);
void kobj_start(void);
void kobj_note_set(uint32_t obj);

typedef uint32_t (*kfn)(CPU *c);
static kfn kimpl[380];
static uint32_t kdata[380];

#define KMAGIC_BASE (MAGIC_BASE + 0x08000000u)
#define A(i) ARG(i)

// ---- data exports --------------------------------------------------------------------------
static uint32_t ke_tick_count;    // KeTickCount, milliseconds since boot

void kernel_tick(void)
{
    if (ke_tick_count) wr32(ke_tick_count, (uint32_t)(ke_interrupt_time() / 10000));
}

static void data_exports(void)
{
    kdata[16] = hostdata_alloc(64);                  // ExEventObjectType
    kdata[259] = hostdata_alloc(64);                 // PsThreadObjectType
    kdata[40] = hostdata_alloc(4);                   // HalDiskCachePartitionCount
    wr32(kdata[40], 3);
    ke_tick_count = kdata[156] = hostdata_alloc(4);  // KeTickCount
    kdata[164] = hostdata_alloc(4);                  // LaunchDataPage: none
    kdata[322] = hostdata_alloc(16);                 // XboxHardwareInfo
    wr32(kdata[322], 0x00000000);
    wr8(kdata[322] + 4, 0xD2);                        // GPU revision (NV2A A2)
    wr8(kdata[322] + 5, 0xD4);                        // MCP revision
    kdata[324] = hostdata_alloc(8);                  // XboxKrnlVersion 1.0.5838.1
    wr16(kdata[324], 1);
    wr16(kdata[324] + 2, 0);
    wr16(kdata[324] + 4, 5838);
    wr16(kdata[324] + 6, 1);
    kdata[356] = hostdata_alloc(4);                  // HalBootSMCVideoMode
    kdata[357] = hostdata_alloc(64);                 // IdexChannelObject
    kdata[71] = hostdata_alloc(64);                  // IoFileObjectType
    kdata[355] = hostdata_alloc(256);                // XePublicKeyData (unused)
}

// ---- pool -----------------------------------------------------------------------------------------
// ExAllocatePool: size classes over pages from the physical allocator.
// Every block has a 16-byte header: size, magic.
#define POOL_MAGIC 0x6c6f6f50u
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t pool_free[64];     // classes of 16 bytes up to 1 KB
static uint32_t pool_cur, pool_end;

static uint32_t pool_alloc(uint32_t n)
{
    uint32_t total = (n + 16 + 15) & ~15u;
    uint32_t blk = 0;
    pthread_mutex_lock(&pool_lock);
    if (total <= 64 * 16) {
        int k = total / 16 - 1;
        if (pool_free[k]) {
            blk = pool_free[k];
            pool_free[k] = rd32(blk + 8);
        } else {
            if (pool_cur + total > pool_end) {
                uint32_t pa = phys_alloc(0x10000, 0x1000, 0, 0);
                if (pa == ~0u) host_fatal("out of memory for the kernel pool");
                pool_cur = RAM_CONTIG | pa, pool_end = pool_cur + 0x10000;
            }
            blk = pool_cur;
            pool_cur += total;
        }
    } else {
        uint32_t pa = phys_alloc(total, 0x1000, 0, 0);
        if (pa != ~0u) blk = RAM_CONTIG | pa;
    }
    pthread_mutex_unlock(&pool_lock);
    if (!blk) return 0;
    wr32(blk, total);
    wr32(blk + 4, POOL_MAGIC);
    return blk + 16;
}

static void pool_release(uint32_t p)
{
    if (!p) return;
    uint32_t blk = p - 16, total = rd32(blk);
    if (rd32(blk + 4) != POOL_MAGIC) {
        hle_log("ExFreePool: bad pointer 0x%08x\n", p);
        return;
    }
    wr32(blk + 4, 0);
    pthread_mutex_lock(&pool_lock);
    if (total <= 64 * 16) {
        int k = total / 16 - 1;
        wr32(blk + 8, pool_free[k]);
        pool_free[k] = blk;
    } else phys_free(phys_of(blk));
    pthread_mutex_unlock(&pool_lock);
}

static uint32_t k_ExAllocatePool(CPU *c) { return pool_alloc(A(0)); }
static uint32_t k_ExAllocatePoolWithTag(CPU *c) { return pool_alloc(A(0)); }
static uint32_t k_ExFreePool(CPU *c) { pool_release(A(0)); return 0; }
static uint32_t k_ExQueryPoolBlockSize(CPU *c) { return rd32(A(0) - 16) - 16; }

static uint32_t k_ExQueryNonVolatileSetting(CPU *c)
{
    // index, *type, value, length, *result length: every setting reads as zero
    uint32_t idx = A(0), type = A(1), val = A(2), len = A(3), rl = A(4);
    if (type) wr32(type, 4);
    if (val && len) memset(M + val, 0, len);
    if (rl) wr32(rl, len < 4 ? len : 4);
    if (hle_verbose) hle_log("ExQueryNonVolatileSetting(0x%x)\n", idx);
    return STATUS_SUCCESS;
}

// ---- debug, AV, HAL ------------------------------------------------------------------------------
static uint32_t k_DbgPrint(CPU *c)
{
    // cdecl, printf style: the format alone is enough to see what is going on
    char buf[256];
    snprintf(buf, sizeof buf, "%s", G(A(0)));
    hle_log("DbgPrint: %s%s", buf, strchr(buf, '\n') ? "" : "\n");
    return 0;
}

static uint32_t k_AvGetSavedDataAddress(CPU *c) { return 0; }
static uint32_t k_AvSetSavedDataAddress(CPU *c) { return 0; }
static uint32_t k_AvSendTVEncoderOption(CPU *c)
{
    if (A(3)) wr32(A(3), 0);
    return 0;
}
uint32_t av_display_mode[6];
static uint32_t k_AvSetDisplayMode(CPU *c)
{
    for (int i = 0; i < 6; i++) av_display_mode[i] = A(i);
    hle_log("AvSetDisplayMode(step %u, mode 0x%x, format 0x%x, pitch %u, fb 0x%x)\n", A(1), A(2), A(3), A(4), A(5));
    return STATUS_SUCCESS;
}

static uint32_t k_HalGetInterruptVector(CPU *c)
{
    // the vector is the bus level + 0x30; IRQL descends from 26 with the level
    uint32_t level = A(0);
    if (A(1)) wr8(A(1), (uint8_t)(0x1a - level));
    return level + 0x30;
}

static uint32_t k_HalReadWritePCISpace(CPU *c)
{
    uint32_t bus = A(0), slot = A(1), reg = A(2), buf = A(3), len = A(4), write = A(5);
    if (!write) memset(M + buf, 0, len);
    if (hle_verbose) hle_log("HalReadWritePCISpace(%s bus %u slot 0x%x reg 0x%x len %u)\n", write ? "write" : "read", bus,
                             slot, reg, len);
    return 0;
}

static uint32_t k_HalRegisterShutdownNotification(CPU *c) { return 0; }
// A Chihiro's test button reboots (routine 2) into the system menu, whose
// test menu launches the game's own (kernel_launch_test_mode); leaving
// that reboots again and the game starts as usual. The front end restarts
// the game the same way: into its test menu from a normal run, normally
// from a test-mode run (front_end_reboot; without one the run ends).
void (*front_end_reboot)(int into_test);

static uint32_t k_HalReturnToFirmware(CPU *c)
{
    hle_log("HalReturnToFirmware(%u): the game asked to reboot\n", A(0));
    if (front_end_reboot && (A(0) == 1 || A(0) == 2)) front_end_reboot(!launch_test_mode);
    host_fatal("the game rebooted");
}
static uint32_t k_HalInitiateShutdown(CPU *c) { host_fatal("the game shut down"); }
static uint32_t k_KeBugCheck(CPU *c)
{
    char why[64];
    snprintf(why, sizeof why, "KeBugCheck(0x%x)", A(0));
    host_fatal(why);
}

// ---- IRQL ---------------------------------------------------------------------------------------
static uint32_t k_KeRaiseIrqlToDpcLevel(CPU *c) { return irql_raise(2); }
static uint32_t k_KfRaiseIrql(CPU *c) { return irql_raise((uint8_t)c->r[ECX]); }
static uint32_t k_KfLowerIrql(CPU *c) { irql_lower((uint8_t)c->r[ECX]); return 0; }

// ---- dispatcher objects -----------------------------------------------------------------------------
static void init_header(uint32_t o, int type, int size, int32_t signal)
{
    wr8(o, (uint8_t)type);
    wr8(o + 1, 0);
    wr8(o + 2, (uint8_t)size);
    wr8(o + 3, 0);
    wr32(o + 4, (uint32_t)signal);
    wr32(o + 8, o + 8);
    wr32(o + 12, o + 8);
}

static uint32_t k_KeInitializeEvent(CPU *c) { init_header(A(0), A(1), 4, (int32_t)(A(2) & 0xff)); return 0; }
static uint32_t k_KeInitializeSemaphore(CPU *c)
{
    init_header(A(0), 5, 5, (int32_t)A(1));
    wr32(A(0) + 16, A(2));
    return 0;
}
static uint32_t k_KeInitializeTimerEx(CPU *c)
{
    uint32_t t = A(0);
    init_header(t, 8 + (A(1) & 1), 10, 0);
    wr64(t + 16, 0);
    wr32(t + 32, 0);
    wr32(t + 36, 0);
    return 0;
}

static uint32_t k_KeSetEvent(CPU *c)
{
    uint32_t ev = A(0);
    kobj_lock();
    uint32_t old = rd32(ev + 4);
    wr32(ev + 4, 1);
    kobj_note_set(ev);
    kobj_unlock_signal();
    return old;
}

static uint32_t k_KeReleaseSemaphore(CPU *c)
{
    uint32_t s = A(0), adj = A(2);
    kobj_lock();
    uint32_t old = rd32(s + 4);
    wr32(s + 4, old + adj);
    kobj_unlock_signal();
    return old;
}

static uint32_t k_KeWaitForSingleObject(CPU *c)
{
    uint32_t o = A(0), t = A(4);
    int64_t to;
    if (t) to = (int64_t)rd64(t);
    return ke_wait(1, &o, 0, t ? &to : NULL, A(3) & 0xff);
}

static uint32_t k_KeWaitForMultipleObjects(CPU *c)
{
    uint32_t n = A(0), arr = A(1), type = A(2), t = A(6);
    uint32_t objs[64];
    if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; i++) objs[i] = rd32(arr + 4 * i);
    int64_t to;
    if (t) to = (int64_t)rd64(t);
    return ke_wait((int)n, objs, type == 0, t ? &to : NULL, A(5) & 0xff);
}

static uint32_t k_KeDelayExecutionThread(CPU *c)
{
    uint32_t t = A(2);
    int64_t to = t ? (int64_t)rd64(t) : 0;
    if (to == 0) {
        kthread_yield();
        return STATUS_SUCCESS;
    }
    ke_wait(0, NULL, 0, &to, 0);
    return STATUS_SUCCESS;
}

static uint32_t k_KeStallExecutionProcessor(CPU *c)
{
    uint32_t us = A(0);
    if (us > 50) usleep(us);
    return 0;
}

// ---- DPCs, timers, interrupts ------------------------------------------------------------------------
static uint32_t k_KeInitializeDpc(CPU *c)
{
    uint32_t d = A(0);
    wr16(d, 0x13);
    wr8(d + 2, 0);
    wr32(d + 12, A(1));
    wr32(d + 16, A(2));
    return 0;
}

static uint32_t k_KeInsertQueueDpc(CPU *c)
{
    uint32_t d = A(0);
    int was = rd8(d + 2);
    ke_insert_dpc(d, A(1), A(2));
    return !was;
}

static uint32_t k_KeRemoveQueueDpc(CPU *c) { return ke_remove_dpc(A(0)); }

static uint32_t k_KeSetTimer(CPU *c)
{
    uint32_t t = A(0);
    int was = rd8(t + 3);
    ke_set_timer(t, (int64_t)(((uint64_t)A(2) << 32) | A(1)), 0, A(3));
    return was;
}

static uint32_t k_KeSetTimerEx(CPU *c)
{
    uint32_t t = A(0);
    int was = rd8(t + 3);
    ke_set_timer(t, (int64_t)(((uint64_t)A(2) << 32) | A(1)), (int32_t)A(3), A(4));
    return was;
}

static uint32_t k_KeCancelTimer(CPU *c) { return ke_cancel_timer(A(0)); }

static uint32_t k_KeInitializeInterrupt(CPU *c)
{
    uint32_t k = A(0);
    memset(M + k, 0, 0x70);
    wr32(k, A(1));
    wr32(k + 4, A(2));
    wr32(k + 8, A(3) - 0x30);       // bus level
    if (hle_verbose > 1) hle_log("interrupt %u initialised: routine 0x%x\n", A(3) - 0x30, A(1));
    wr8(k + 12, (uint8_t)A(4));
    wr32(k + 24, A(5));
    return 0;
}

static uint32_t k_KeConnectInterrupt(CPU *c)
{
    uint32_t k = A(0);
    wr8(k + 16, 1);
    ke_connect_interrupt(rd32(k + 8), k);
    hle_log("interrupt %u connected: routine 0x%x\n", rd32(k + 8), rd32(k));
    return 1;
}

static uint32_t k_KeDisconnectInterrupt(CPU *c)
{
    uint32_t k = A(0);
    wr8(k + 16, 0);
    ke_connect_interrupt(rd32(k + 8), 0);
    return 1;
}

static uint32_t k_KeSynchronizeExecution(CPU *c)
{
    uint32_t k = A(0), routine = A(1), ctx = A(2);
    uint8_t old = irql_raise(rd8(k + 12) ? rd8(k + 12) : 0x1a);
    uint32_t r = guest_call(c, routine, 1, &ctx);
    irql_lower(old);
    return r & 0xff;
}

// ---- threads -----------------------------------------------------------------------------------------
static uint32_t k_PsCreateSystemThreadEx(CPU *c)
{
    uint32_t ph = A(0), stack = A(2), pid = A(4), start = A(5), ctx = A(6), susp = A(7) & 0xff, sysr = A(9);
    uint32_t t = kthread_create(start, ctx, 0, stack, susp, sysr);
    uint32_t h = ob_handle_new(t, OB_THREAD);
    if (ph) wr32(ph, h);
    if (pid) wr32(pid, rd32(t + 0x130));
    return STATUS_SUCCESS;
}

static uint32_t k_PsTerminateSystemThread(CPU *c) { kthread_exit(A(0)); }

static uint32_t k_KeQueryBasePriorityThread(CPU *c) { return 0; }
static uint32_t k_KeSetBasePriorityThread(CPU *c) { return 0; }
static uint32_t k_KeSetDisableBoostThread(CPU *c) { return 0; }
static uint32_t k_NtYieldExecution(CPU *c) { kthread_yield(); return STATUS_SUCCESS; }

static uint32_t thread_of_handle(uint32_t h)
{
    if (h == 0xfffffffe) return kthread_current();
    int kind;
    uint32_t o = ob_object(h, &kind);
    return kind == OB_THREAD ? o : 0;
}

static uint32_t k_NtResumeThread(CPU *c)
{
    int was = kthread_resume(thread_of_handle(A(0)));
    if (A(1)) wr32(A(1), was);
    return STATUS_SUCCESS;
}

static uint32_t k_NtSuspendThread(CPU *c)
{
    int was = kthread_suspend(thread_of_handle(A(0)));
    if (A(1)) wr32(A(1), was);
    return STATUS_SUCCESS;
}

// ---- time ----------------------------------------------------------------------------------------------
static uint32_t k_KeQueryInterruptTime(CPU *c)
{
    uint64_t t = ke_interrupt_time();
    c->r[EDX] = (uint32_t)(t >> 32);
    return (uint32_t)t;
}

static uint32_t k_KeQuerySystemTime(CPU *c)
{
    wr64(A(0), ke_system_time());
    return 0;
}

static uint32_t k_KeSaveFloatingPointState(CPU *c) { return STATUS_SUCCESS; }
static uint32_t k_KeRestoreFloatingPointState(CPU *c) { return STATUS_SUCCESS; }

// ---- memory --------------------------------------------------------------------------------------------
static uint32_t gpu_instance;   // the GPU's instance memory, claimed by D3D

static uint32_t k_MmAllocateContiguousMemoryEx(CPU *c)
{
    uint32_t size = A(0), lo = A(1), hi = A(2), align = A(3);
    uint32_t pa = phys_alloc(size, align, lo, hi == 0xffffffff ? 0 : hi);
    if (pa == ~0u) {
        hle_log("MmAllocateContiguousMemoryEx(0x%x): out of memory (0x%x free)\n", size, phys_free_bytes());
        return 0;
    }
    return RAM_CONTIG | pa;
}

static uint32_t k_MmAllocateContiguousMemory(CPU *c)
{
    uint32_t pa = phys_alloc(A(0), 0x1000, 0, 0);
    return pa == ~0u ? 0 : RAM_CONTIG | pa;
}

static uint32_t k_MmFreeContiguousMemory(CPU *c) { phys_free(phys_of(A(0))); return 0; }
static uint32_t k_MmGetPhysicalAddress(CPU *c) { return bus_of(A(0)); }
static uint32_t k_MmPersistContiguousMemory(CPU *c) { return 0; }
static uint32_t k_MmLockUnlockBufferPages(CPU *c) { return 0; }
static uint32_t k_MmLockUnlockPhysicalPage(CPU *c) { return 0; }
static uint32_t k_MmQueryAddressProtect(CPU *c) { return 0x04; }
static uint32_t k_MmSetAddressProtect(CPU *c) { return 0; }
static uint32_t k_MmQueryAllocationSize(CPU *c) { return phys_size(phys_of(A(0))); }

static uint32_t k_MmQueryStatistics(CPU *c)
{
    uint32_t s = A(0);
    if (rd32(s) != 36) return STATUS_INVALID_PARAMETER;
    memset(M + s + 4, 0, 32);
    wr32(s + 4, RAM_SIZE / 4096);
    wr32(s + 8, phys_free_bytes() / 4096);
    return STATUS_SUCCESS;
}

static uint32_t k_MmClaimGpuInstanceMemory(CPU *c)
{
    // The GPU's instance memory sits at the top of the first 64 MB (where a
    // retail Xbox has it); D3D gets back the address just past it and how
    // much was claimed.
    uint32_t want = A(0), pad = A(1);
    if (pad) wr32(pad, 0);
    if (!gpu_instance) gpu_instance = 0x04000000u;
    hle_log("MmClaimGpuInstanceMemory(0x%x)\n", want);
    return RAM_CONTIG | gpu_instance;
}

#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_RELEASE 0x8000u

static uint32_t k_NtAllocateVirtualMemory(CPU *c)
{
    uint32_t pbase = A(0), psize = A(2), type = A(3);
    uint32_t base = rd32(pbase), size = rd32(psize);
    size = (size + 0xfff) & ~0xfffu;
    if (base) {
        // committing inside an earlier reservation, or asking for a place
        if (phys_size(phys_of(base & ~0xfffu)) || (type & MEM_COMMIT && !(type & MEM_RESERVE))) {
            wr32(pbase, base & ~0xfffu);
            wr32(psize, size);
            return STATUS_SUCCESS;
        }
        uint32_t pa = phys_alloc(size, 0x1000, base & ~0xfffu, (base & ~0xfffu) + 0x1000);
        if (pa == ~0u) return STATUS_NO_MEMORY;
        memset(M + pa, 0, size);
        wr32(pbase, pa);
        wr32(psize, size);
        return STATUS_SUCCESS;
    }
    uint32_t pa = phys_alloc(size, 0x10000, 0, 0);
    if (pa == ~0u) {
        hle_log("NtAllocateVirtualMemory(0x%x): out of memory (0x%x free)\n", size, phys_free_bytes());
        return STATUS_NO_MEMORY;
    }
    memset(M + pa, 0, size);
    wr32(pbase, pa);
    wr32(psize, size);
    return STATUS_SUCCESS;
}

static uint32_t k_NtFreeVirtualMemory(CPU *c)
{
    uint32_t base = rd32(A(0));
    if (A(2) & MEM_RELEASE) phys_free(phys_of(base));
    return STATUS_SUCCESS;
}

static uint32_t k_NtQueryVirtualMemory(CPU *c)
{
    uint32_t base = A(0) & ~0xfffu, mbi = A(1);
    uint32_t pa = phys_of(base);
    // the block holding base: walk down to its first page
    uint32_t start = pa, size = 0;
    for (uint32_t p = pa;; p -= 0x1000) {
        uint32_t s = phys_size(p);
        if (s && p + s > pa) {
            start = p, size = s;
            break;
        }
        if (!p || pa - p > 0x4000000) break;
    }
    wr32(mbi, base);
    wr32(mbi + 4, size ? (base & ~(RAM_SIZE - 1)) | start : base);
    wr32(mbi + 8, 4);
    wr32(mbi + 12, size ? start + size - pa : 0x1000);
    wr32(mbi + 16, size ? MEM_COMMIT : 0x10000);
    wr32(mbi + 20, size ? 4 : 1);
    wr32(mbi + 24, 0x20000);
    return STATUS_SUCCESS;
}

// ---- Nt objects ----------------------------------------------------------------------------------------
static uint32_t k_NtCreateEvent(CPU *c)
{
    uint32_t ev = hostdata_alloc(16);
    init_header(ev, A(2) & 1, 4, (int32_t)(A(3) & 0xff));
    wr32(A(0), ob_handle_new(ev, OB_EVENT));
    return STATUS_SUCCESS;
}

static uint32_t k_NtCreateSemaphore(CPU *c)
{
    uint32_t s = hostdata_alloc(20);
    init_header(s, 5, 5, (int32_t)A(2));
    wr32(s + 16, A(3));
    wr32(A(0), ob_handle_new(s, OB_SEMAPHORE));
    return STATUS_SUCCESS;
}

static uint32_t object_of(uint32_t h)
{
    int kind;
    if (h == 0xfffffffe) return kthread_current();
    return ob_object(h, &kind);
}

static uint32_t k_NtSetEvent(CPU *c)
{
    uint32_t ev = object_of(A(0));
    if (!ev) return STATUS_INVALID_HANDLE;
    kobj_lock();
    if (A(1)) wr32(A(1), rd32(ev + 4));
    wr32(ev + 4, 1);
    kobj_note_set(ev);
    kobj_unlock_signal();
    return STATUS_SUCCESS;
}

static uint32_t k_NtClearEvent(CPU *c)
{
    uint32_t ev = object_of(A(0));
    if (!ev) return STATUS_INVALID_HANDLE;
    kobj_lock();
    wr32(ev + 4, 0);
    kobj_unlock_signal();
    return STATUS_SUCCESS;
}

static uint32_t k_NtPulseEvent(CPU *c)
{
    // wake whoever waits now, then leave the event clear
    uint32_t ev = object_of(A(0));
    if (!ev) return STATUS_INVALID_HANDLE;
    kobj_lock();
    if (A(1)) wr32(A(1), rd32(ev + 4));
    wr32(ev + 4, 1);
    kobj_note_set(ev);
    kobj_unlock_signal();
    kthread_host_sleep(100);
    kobj_lock();
    wr32(ev + 4, 0);
    kobj_unlock_signal();
    return STATUS_SUCCESS;
}

static uint32_t k_NtReleaseSemaphore(CPU *c)
{
    uint32_t s = object_of(A(0));
    if (!s) return STATUS_INVALID_HANDLE;
    kobj_lock();
    uint32_t old = rd32(s + 4);
    if (old + A(1) > rd32(s + 16)) {
        kobj_unlock_signal();
        return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    }
    wr32(s + 4, old + A(1));
    if (A(2)) wr32(A(2), old);
    kobj_unlock_signal();
    return STATUS_SUCCESS;
}

static uint32_t nt_wait(uint32_t h, uint32_t alertable, uint32_t t)
{
    uint32_t o = object_of(h);
    if (!o) return STATUS_INVALID_HANDLE;
    int64_t to;
    if (t) to = (int64_t)rd64(t);
    return ke_wait(1, &o, 0, t ? &to : NULL, alertable & 0xff);
}

static uint32_t k_NtWaitForSingleObject(CPU *c) { return nt_wait(A(0), A(1), A(2)); }
static uint32_t k_NtWaitForSingleObjectEx(CPU *c) { return nt_wait(A(0), A(2), A(3)); }

uint32_t kfile_close(uint32_t h);
static uint32_t k_NtClose(CPU *c)
{
    int kind;
    uint32_t h = A(0);
    ob_object(h, &kind);
    if (kind == OB_FILE || kind == OB_DIR) kfile_close(h);
    ob_handle_close(h);
    return STATUS_SUCCESS;
}

static uint32_t k_ObReferenceObjectByHandle(CPU *c)
{
    uint32_t o = object_of(A(0));
    if (!o) return STATUS_INVALID_HANDLE;
    wr32(A(2), o);
    return STATUS_SUCCESS;
}

static uint32_t k_ObfDereferenceObject(CPU *c) { return 0; }

// ---- I/O manager: the SDK's memory unit driver (Crazy Taxi links it) makes
// its devices; a Chihiro never has a memory unit, so no request ever arrives
static uint32_t k_IoCreateDevice(CPU *c)
{
    // driver, extension size, name, type, exclusive, *device
    uint32_t dev = pool_alloc(0x80 + A(1));
    memset(M + dev, 0, 0x80 + A(1));
    wr16(dev, 3), wr16(dev + 2, 0x80);
    wr32(dev + 8, A(0));
    wr32(dev + 0x18, A(1) ? dev + 0x80 : 0);
    M[dev + 0x1c] = (uint8_t)A(3);
    M[dev + 0x1e] = 1;
    wr32(A(5), dev);
    hle_log("IoCreateDevice(type 0x%x, ext 0x%x) -> 0x%x\n", A(3), A(1), dev);
    return STATUS_SUCCESS;
}
static uint32_t k_IoInvalidDeviceRequest(CPU *c) { hle_log("IoInvalidDeviceRequest\n"); return 0xc0000010; }
static uint32_t k_IofCompleteRequest(CPU *c) { hle_log("IofCompleteRequest(0x%x)\n", c->r[ECX]); return 0; }
static uint32_t k_IoStartPacket(CPU *c) { hle_log("IoStartPacket(0x%x, 0x%x)\n", A(0), A(1)); return 0; }
static uint32_t k_IoStartNextPacket(CPU *c) { hle_log("IoStartNextPacket(0x%x)\n", A(0)); return 0; }
static uint32_t k_IoMarkIrpMustComplete(CPU *c) { return 0; }
static uint32_t k_HalIsResetOrShutdownPending(CPU *c) { return 0; }
static uint32_t k_MmAllocateSystemMemory(CPU *c)
{
    uint32_t pa = phys_alloc((A(0) + 0xfff) & ~0xfffu, 0x1000, 0, 0);
    return pa == ~0u ? 0 : RAM_CONTIG | pa;
}
static uint32_t k_MmFreeSystemMemory(CPU *c) { phys_free(phys_of(A(0))); return 0; }

// ---- Rtl ---------------------------------------------------------------------------------------------------
static uint32_t k_RtlCompareMemoryUlong(CPU *c)
{
    uint32_t p = A(0), n = A(1) & ~3u, v = A(2), i;
    for (i = 0; i < n && rd32(p + i) == v; i += 4)
        ;
    return i;
}

static uint32_t k_RtlInitAnsiString(CPU *c)
{
    uint32_t s = A(0), src = A(1);
    uint32_t len = src ? (uint32_t)strlen(G(src)) : 0;
    wr16(s, (uint16_t)len);
    wr16(s + 2, (uint16_t)(src ? len + 1 : 0));
    wr32(s + 4, src);
    return 0;
}

static uint32_t k_RtlEqualString(CPU *c)
{
    uint32_t a = A(0), b = A(1);
    uint16_t la = rd16(a), lb = rd16(b);
    if (la != lb) return 0;
    const char *pa = G(rd32(a + 4)), *pb = G(rd32(b + 4));
    return A(2) & 0xff ? strncasecmp(pa, pb, la) == 0 : memcmp(pa, pb, la) == 0;
}

static uint32_t k_RtlNtStatusToDosError(CPU *c)
{
    uint32_t s = A(0);
    switch (s) {
    case STATUS_SUCCESS: return 0;
    case STATUS_OBJECT_NAME_NOT_FOUND: return 2;
    case STATUS_OBJECT_PATH_NOT_FOUND: return 3;
    case STATUS_ACCESS_DENIED: return 5;
    case STATUS_INVALID_HANDLE: return 6;
    case STATUS_NO_MEMORY: return 8;
    case STATUS_END_OF_FILE: return 38;
    case STATUS_INVALID_PARAMETER: return 87;
    case STATUS_OBJECT_NAME_COLLISION: return 183;
    case STATUS_TIMEOUT: return 1460;
    case STATUS_PENDING: return 997;
    default: return 317;
    }
}

// critical sections: +0 a synchronization event, +16 LockCount, +20
// RecursionCount, +24 OwningThread. The event's signal state doubles as
// "nobody waiting": a thread that finds the section owned clears it before
// it waits, so only a leave that finds it clear has anyone to wake. Taking
// and leaving an uncontended section wakes nobody (HotD3's loaders and CRI
// do 20,000+ pairs a second; waking every waiting thread on each one was
// the load stutter).
static uint32_t k_RtlInitializeCriticalSection(CPU *c)
{
    uint32_t cs = A(0);
    init_header(cs, 1, 4, 0);
    wr32(cs + 16, 0xffffffff);
    wr32(cs + 20, 0);
    wr32(cs + 24, 0);
    return 0;
}

static int cs_try(uint32_t cs, uint32_t self)
{
    uint32_t owner = rd32(cs + 24);
    if (owner == self) {
        wr32(cs + 20, rd32(cs + 20) + 1);
        wr32(cs + 16, rd32(cs + 16) + 1);
        return 1;
    }
    if (owner == 0) {
        wr32(cs + 24, self);
        wr32(cs + 20, 1);
        wr32(cs + 16, rd32(cs + 16) + 1);
        return 1;
    }
    return 0;
}

static uint32_t k_RtlEnterCriticalSection(CPU *c)
{
    uint32_t cs = A(0), self = kthread_current();
    for (;;) {
        kobj_lock();
        int ok = cs_try(cs, self);
        if (!ok) wr32(cs + 4, 0);    // the event: the owner sets it on leave
        kobj_unlock();
        if (ok) return 0;
        uint32_t ev = cs;
        int64_t to = -10000;          // 1 ms: a lost wake costs a millisecond at most
        ke_wait(1, &ev, 0, &to, 0);
    }
}

static uint32_t k_RtlTryEnterCriticalSection(CPU *c)
{
    uint32_t cs = A(0);
    kobj_lock();
    int ok = cs_try(cs, kthread_current());
    kobj_unlock();
    return ok;
}

static uint32_t k_RtlLeaveCriticalSection(CPU *c)
{
    uint32_t cs = A(0);
    kobj_lock();
    uint32_t r = rd32(cs + 20) - 1;
    wr32(cs + 20, r);
    wr32(cs + 16, rd32(cs + 16) - 1);
    int waiters = 0;
    if (!r) {
        wr32(cs + 24, 0);
        waiters = !rd32(cs + 4);
        wr32(cs + 4, 1);
    }
    if (waiters) kobj_unlock_signal();
    else kobj_unlock();
    // one processor: give it up for a moment so the woken waiter can take
    // the section. On the Xbox its priority boost switches to it at once;
    // here the leaving thread kept the processor and re-entered every time
    // (Crazy Taxi's main thread leaves and re-enters 0x33ad34 three times a
    // frame, and its course loader, thread 13, never got in). Contended
    // leaves are rare. (Turning newcomers away until a waiter took it
    // starved everything: a newcomer's wait consumed the leave's wake.)
    if (waiters && guest_one_core) kthread_host_sleep(200);
    return 0;
}

static uint32_t k_RtlRaiseException(CPU *c)
{
    uint32_t rec = A(0);
    char why[96];
    snprintf(why, sizeof why, "RtlRaiseException(code 0x%08x at 0x%08x)", rd32(rec), rd32(rec + 12));
    host_fatal(why);
}

static uint32_t k_RtlUnwind(CPU *c)
{
    // SEH unwinding to a frame: drop the registrations above it
    uint32_t target = A(0), kpcr = c->fs_base;
    if (target) wr32(kpcr, target);
    return 0;
}

// TIME_FIELDS: Year Month Day Hour Minute Second Milliseconds Weekday (shorts)
static uint32_t k_RtlTimeToTimeFields(CPU *c)
{
    uint64_t t = rd64(A(0));
    uint64_t ms = (t / 10000) % 1000;
    time_t s = (time_t)(t / 10000000ull - 11644473600ull);
    struct tm tm;
    gmtime_r(&s, &tm);
    uint32_t f = A(1);
    wr16(f, (uint16_t)(tm.tm_year + 1900));
    wr16(f + 2, (uint16_t)(tm.tm_mon + 1));
    wr16(f + 4, (uint16_t)tm.tm_mday);
    wr16(f + 6, (uint16_t)tm.tm_hour);
    wr16(f + 8, (uint16_t)tm.tm_min);
    wr16(f + 10, (uint16_t)tm.tm_sec);
    wr16(f + 12, (uint16_t)ms);
    wr16(f + 14, (uint16_t)tm.tm_wday);
    return 0;
}

static uint32_t k_RtlTimeFieldsToTime(CPU *c)
{
    uint32_t f = A(0);
    struct tm tm = {0};
    tm.tm_year = rd16(f) - 1900;
    tm.tm_mon = rd16(f + 2) - 1;
    tm.tm_mday = rd16(f + 4);
    tm.tm_hour = rd16(f + 6);
    tm.tm_min = rd16(f + 8);
    tm.tm_sec = rd16(f + 10);
    time_t s = timegm(&tm);
    wr64(A(1), ((uint64_t)s + 11644473600ull) * 10000000ull + rd16(f + 12) * 10000ull);
    return 1;
}

// ---- XBE sections ------------------------------------------------------------------------------------------
// every section is loaded with the image, so loading one is counting it
static uint32_t k_XeLoadSection(CPU *c) { return STATUS_SUCCESS; }
static uint32_t k_XeUnloadSection(CPU *c) { return STATUS_SUCCESS; }

// ---- files (kfile.c) -------------------------------------------------------------------------------------------
uint32_t k_NtCreateFile(CPU *c);
uint32_t k_NtOpenFile(CPU *c);
uint32_t k_NtReadFile(CPU *c);
uint32_t k_NtWriteFile(CPU *c);
uint32_t k_NtQueryInformationFile(CPU *c);
uint32_t k_NtSetInformationFile(CPU *c);
uint32_t k_NtQueryVolumeInformationFile(CPU *c);
uint32_t k_NtQueryDirectoryFile(CPU *c);
uint32_t k_NtDeviceIoControlFile(CPU *c);
uint32_t k_NtFsControlFile(CPU *c);
uint32_t k_NtFlushBuffersFile(CPU *c);
uint32_t k_IoCreateSymbolicLink(CPU *c);
uint32_t k_NtOpenSymbolicLinkObject(CPU *c);
uint32_t k_NtQuerySymbolicLinkObject(CPU *c);

// ---- the table -----------------------------------------------------------------------------------------------
static void impls(void)
{
#define I(o, f) kimpl[o] = k_##f
    I(1, AvGetSavedDataAddress); I(2, AvSendTVEncoderOption); I(3, AvSetDisplayMode); I(4, AvSetSavedDataAddress);
    I(8, DbgPrint); I(14, ExAllocatePool); I(15, ExAllocatePoolWithTag); I(17, ExFreePool);
    I(23, ExQueryPoolBlockSize); I(24, ExQueryNonVolatileSetting); I(44, HalGetInterruptVector);
    I(46, HalReadWritePCISpace); I(47, HalRegisterShutdownNotification); I(49, HalReturnToFirmware);
    I(67, IoCreateSymbolicLink); I(95, KeBugCheck); I(97, KeCancelTimer); I(98, KeConnectInterrupt);
    I(99, KeDelayExecutionThread); I(100, KeDisconnectInterrupt); I(107, KeInitializeDpc); I(108, KeInitializeEvent);
    I(109, KeInitializeInterrupt); I(112, KeInitializeSemaphore); I(113, KeInitializeTimerEx);
    I(119, KeInsertQueueDpc); I(124, KeQueryBasePriorityThread); I(125, KeQueryInterruptTime);
    I(128, KeQuerySystemTime); I(129, KeRaiseIrqlToDpcLevel); I(132, KeReleaseSemaphore); I(137, KeRemoveQueueDpc);
    I(139, KeRestoreFloatingPointState); I(142, KeSaveFloatingPointState); I(143, KeSetBasePriorityThread);
    I(144, KeSetDisableBoostThread); I(145, KeSetEvent); I(149, KeSetTimer); I(150, KeSetTimerEx);
    I(151, KeStallExecutionProcessor); I(153, KeSynchronizeExecution); I(158, KeWaitForMultipleObjects);
    I(159, KeWaitForSingleObject); I(160, KfRaiseIrql); I(161, KfLowerIrql); I(165, MmAllocateContiguousMemory);
    I(166, MmAllocateContiguousMemoryEx); I(168, MmClaimGpuInstanceMemory); I(171, MmFreeContiguousMemory);
    I(173, MmGetPhysicalAddress); I(175, MmLockUnlockBufferPages); I(176, MmLockUnlockPhysicalPage);
    I(178, MmPersistContiguousMemory); I(179, MmQueryAddressProtect); I(180, MmQueryAllocationSize);
    I(181, MmQueryStatistics); I(182, MmSetAddressProtect); I(184, NtAllocateVirtualMemory); I(186, NtClearEvent);
    I(187, NtClose); I(189, NtCreateEvent); I(190, NtCreateFile); I(193, NtCreateSemaphore);
    I(196, NtDeviceIoControlFile); I(198, NtFlushBuffersFile); I(199, NtFreeVirtualMemory); I(200, NtFsControlFile);
    I(202, NtOpenFile); I(203, NtOpenSymbolicLinkObject); I(205, NtPulseEvent); I(207, NtQueryDirectoryFile);
    I(211, NtQueryInformationFile); I(215, NtQuerySymbolicLinkObject); I(217, NtQueryVirtualMemory);
    I(218, NtQueryVolumeInformationFile); I(219, NtReadFile); I(222, NtReleaseSemaphore); I(224, NtResumeThread);
    I(225, NtSetEvent); I(226, NtSetInformationFile); I(231, NtSuspendThread); I(233, NtWaitForSingleObject);
    I(234, NtWaitForSingleObjectEx); I(236, NtWriteFile); I(238, NtYieldExecution); I(246, ObReferenceObjectByHandle);
    I(250, ObfDereferenceObject); I(255, PsCreateSystemThreadEx); I(258, PsTerminateSystemThread);
    I(269, RtlCompareMemoryUlong); I(277, RtlEnterCriticalSection); I(279, RtlEqualString); I(289, RtlInitAnsiString);
    I(291, RtlInitializeCriticalSection); I(294, RtlLeaveCriticalSection); I(301, RtlNtStatusToDosError);
    I(302, RtlRaiseException); I(304, RtlTimeFieldsToTime); I(305, RtlTimeToTimeFields);
    I(306, RtlTryEnterCriticalSection); I(312, RtlUnwind); I(327, XeLoadSection); I(328, XeUnloadSection);
    I(360, HalInitiateShutdown);
    I(65, IoCreateDevice); I(74, IoInvalidDeviceRequest); I(87, IofCompleteRequest); I(83, IoStartPacket);
    I(81, IoStartNextPacket); I(359, IoMarkIrpMustComplete); I(358, HalIsResetOrShutdownPending);
    I(167, MmAllocateSystemMemory); I(172, MmFreeSystemMemory);
#undef I
}

// Every kernel function is entered through here: the stdcall arguments are
// popped after it returns (fastcall ones are in ecx and edx; DbgPrint is cdecl).
static void kernel_call(CPU *c, unsigned ord)
{
    kfn f = ord < 379 ? kimpl[ord] : NULL;
    const char *name = ord < 379 && kexport_table[ord].name ? kexport_table[ord].name : "?";
    if (!f) {
        char why[96];
        snprintf(why, sizeof why, "kernel function %u (%s) is not implemented", ord, name);
        host_fatal(why);
    }
    if (guest_stopping) guest_exit_thread();
    kthread_check_suspend();
    kthread_freeze_point();
    if (hle_trace) hle_log("[%08x] %s(%x, %x, %x, %x)\n", rd32(c->r[ESP]), name, ARG(0), ARG(1), ARG(2), ARG(3));
    ch_kcaller = rd32(c->r[ESP]);
    if (ch_perf > 0) {
        uint32_t bp = c->r[EBP];
        ch_kcaller2 = bp > c->r[ESP] && bp - c->r[ESP] < 0x400 ? rd32(bp + 4) : 0;
        ch_perf_call(ord);
    }
    c->r[EAX] = f(c);
    ch_kcaller = 0;
    int bytes = kexport_table[ord].argbytes;
    if (kexport_table[ord].fastcall) bytes = bytes > 8 ? bytes - 8 : 0;
    if (bytes < 0) bytes = 0;   // DbgPrint
    c->r[ESP] += 4 + bytes;
}

const char *ch_kernel_name(unsigned ord)
{
    return ord < 379 && kexport_table[ord].name ? kexport_table[ord].name : "?";
}

static hle_fn kernel_thunks[380];

// one host function per ordinal, so dispatch needs no table lookup of its own
#define T(n) static void kt_##n(CPU *c) { kernel_call(c, n); }
#define T10(n) T(n##0) T(n##1) T(n##2) T(n##3) T(n##4) T(n##5) T(n##6) T(n##7) T(n##8) T(n##9)
T(1) T(2) T(3) T(4) T(5) T(6) T(7) T(8) T(9) T10(1) T10(2) T10(3) T10(4) T10(5) T10(6) T10(7) T10(8) T10(9)
T10(10) T10(11) T10(12) T10(13) T10(14) T10(15) T10(16) T10(17) T10(18) T10(19) T10(20) T10(21) T10(22) T10(23)
T10(24) T10(25) T10(26) T10(27) T10(28) T10(29) T10(30) T10(31) T10(32) T10(33) T10(34) T10(35) T10(36) T10(37)
#undef T
#undef T10

static void fill_thunks(void)
{
#define T(n) kernel_thunks[n] = kt_##n;
#define T10(n) T(n##0) T(n##1) T(n##2) T(n##3) T(n##4) T(n##5) T(n##6) T(n##7) T(n##8) T(n##9)
    T(1) T(2) T(3) T(4) T(5) T(6) T(7) T(8) T(9) T10(1) T10(2) T10(3) T10(4) T10(5) T10(6) T10(7) T10(8) T10(9)
    T10(10) T10(11) T10(12) T10(13) T10(14) T10(15) T10(16) T10(17) T10(18) T10(19) T10(20) T10(21) T10(22) T10(23)
    T10(24) T10(25) T10(26) T10(27) T10(28) T10(29) T10(30) T10(31) T10(32) T10(33) T10(34) T10(35) T10(36) T10(37)
#undef T
#undef T10
}

uint32_t kernel_export(unsigned ord)
{
    if (ord < 379 && kdata[ord]) return kdata[ord];
    if (ord >= 1 && ord < 379 && kernel_thunks[ord]) {
        const char *name = kexport_table[ord].name ? kexport_table[ord].name : "?";
        if (kexport_table[ord].argbytes < 0 && ord != 8)
            hle_log("kernel data export %u (%s) is not provided\n", ord, name);
        return magic_register(kernel_thunks[ord], name);
    }
    hle_log("kernel export %u is out of range\n", ord);
    return 0;
}

// Start the game as if the Chihiro's system menu had launched its GAME TEST
// MODE: the launch data page (type 0, launched by a title, with this title's
// ID) whose data starts with 1. VC3's XGetLaunchInfo caller reads exactly
// that as "enter the test menu". Called after the XBE is loaded.
void kernel_launch_test_mode(void)
{
    uint32_t pa = phys_alloc(0x1000, 0x1000, 0, 0);
    if (pa == ~0u) return;
    uint32_t page = RAM_CONTIG | pa;
    memset(M + pa, 0, 0x1000);
    uint32_t cert = rd32(0x10118);
    wr32(page, 0);                     // LDT_TITLE
    wr32(page + 4, rd32(cert + 8));    // the title ID
    wr32(page + 0x400, 1);             // the launch data: test mode
    wr32(kdata[164], page);
    hle_log("launching into the game's test mode\n");
}

// back to power-on state (guest memory is mapped afresh)
void kernel_reset(void)
{
    memset(kdata, 0, sizeof kdata);
    ke_tick_count = 0;
    memset(pool_free, 0, sizeof pool_free);
    pool_cur = pool_end = 0;
    gpu_instance = 0;
    memset(av_display_mode, 0, sizeof av_display_mode);
}

void kernel_init(void)
{
    impls();
    fill_thunks();
    data_exports();
    kfile_init();
    kobj_start();
}
