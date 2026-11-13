// The host side of a Chihiro game: guest memory, the Xbox kernel, the GPU,
// sound and the Sega boards. A handler is entered the way the guest function
// would be: the return address at esp, the arguments above it.
#pragma once
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARG(i) rd32(c->r[ESP] + 4 + 4 * (i))
#define G(a) ((char *)(M + (uint32_t)(a)))
#define RET(v) do { c->r[EAX] = (uint32_t)(v); return; } while (0)

// ---- guest memory -----------------------------------------------------------------------
// The Chihiro's 128 MB of RAM is one host object seen at three guest
// addresses, as the Xbox kernel maps it: from 0 (the game's own pages: a
// virtual address below 128 MB is its physical address), from 0x80000000
// (contiguous memory) and from 0xF0000000 (the write-combined view D3D uses).
// The GPU reads physical addresses, so all three agree by construction.
#define RAM_SIZE 0x08000000u
#define RAM_CONTIG 0x80000000u
#define RAM_WC 0xF0000000u
#define NV2A_BASE 0xFD000000u         // the GPU's registers (16 MB)
#define NV2A_SIZE 0x01000000u
#define HOSTDATA_BASE 0xC0000000u     // kernel data the game sees (KPCR, exported variables)
#define HOSTDATA_END 0xC1000000u
#define STACKS_BASE 0xD0000000u       // thread stacks: outside RAM, so they never take the game's memory
#define STACKS_END 0xE0000000u
#define MAGIC_BASE 0xE0000000u        // addresses that dispatch to host functions
#define RETURN_MAGIC 0xEFFFFFF0u

static inline uint32_t phys_of(uint32_t va) { return va & (RAM_SIZE - 1); }
// A bus address a device may be handed (MmGetPhysicalAddress). Thread stacks
// and kernel data live outside RAM here, so they keep their own address and a
// device reaches them through dma_addr; everything else is RAM.
static inline int outside_ram(uint32_t va) { return va >= HOSTDATA_BASE && va < STACKS_END; }
static inline uint32_t bus_of(uint32_t va) { return outside_ram(va) ? va : phys_of(va); }
static inline uint32_t dma_addr(uint32_t pa) { return outside_ram(pa) ? pa : pa & (RAM_SIZE - 1); }

uint32_t hostdata_alloc(uint32_t n);          // zeroed, never freed
uint32_t stack_alloc(uint32_t size);          // returns the top
// physical pages: a page-granular allocator over RAM above the game's image
uint32_t phys_alloc(uint32_t size, uint32_t align, uint32_t lo, uint32_t hi);   // physical address, or ~0u
void phys_free(uint32_t pa);
uint32_t phys_size(uint32_t pa);              // bytes of the block at pa, or 0
uint32_t phys_free_bytes(void);

typedef void (*hle_fn)(CPU *c);
uint32_t magic_register(hle_fn fn, const char *name);

// ---- logging ---------------------------------------------------------------------------
void hle_log(const char *fmt, ...);
extern int hle_verbose;
extern int hle_trace;          // CH_TRACE=1: log every kernel call
void host_fatal(const char *why) __attribute__((noreturn));

// ---- the translated program --------------------------------------------------------------
typedef struct gen_program {
    const char *name;
    const uint32_t *fn_addr;
    const guest_fn *fn_ptr;
    unsigned fn_count;
    const char *const *import_names;
    const uint32_t *import_got, *import_plt;
    unsigned import_count;
    const uint32_t *copy_addr;
    const char *const *copy_names;
    unsigned copy_count;
    const char *const *obj_names;
    const uint32_t *obj_addr;
    unsigned obj_count;
    const uint32_t *copy_size;
    const char *const *mod_file;
    const uint32_t *mod_base, *mod_init;
    unsigned mod_count;
    const char *const *export_names;
    const uint32_t *export_addr;
    unsigned export_count;
    const uint32_t *reloc_addr;
    const uint8_t *reloc_type;
    const char *const *reloc_sym;
    unsigned reloc_count;
} gen_program;
extern const gen_program *prog;

// ---- the game ------------------------------------------------------------------------------
extern char game_root[1024];   // the folder holding the game's files (D:)
extern char save_root[1024];   // where its EEPROM, settings and partitions live

typedef struct xbe_info {
    uint32_t base, image_size, entry, thunk, tls;
    uint32_t cert;                 // guest address of the certificate
    char title[41];
} xbe_info;
extern xbe_info xbe;
int load_xbe(const char *path);
void run_game(const gen_program *p, const char *xbe_path);

// ---- kernel ---------------------------------------------------------------------------------
void kernel_init(void);
void kernel_launch_test_mode(void);   // after the XBE is loaded
extern int launch_test_mode;
// the game's own test program, when its test mode is one (else NULL): the
// front end runs it instead of the game when launch_test_mode is set
extern const gen_program *test_program;
// set by the front end: restart the game, into its test menu or not
// (the test button, see k_HalReturnToFirmware); it does not return
extern void (*front_end_reboot)(int into_test);          // run_game: start in the game's test menu
uint32_t kernel_export(unsigned ordinal);      // what the thunk table gets
void kernel_tick(void);                        // advance KeTickCount and the clocks
CPU *cpu_new(void);
CPU *cpu_current(void);
void cpu_set_current(CPU *c);
uint32_t guest_call(CPU *c, uint32_t addr, int nargs, const uint32_t *args);   // stdcall or cdecl
uint32_t guest_fastcall(CPU *c, uint32_t addr, uint32_t ecx, uint32_t edx);

// threads and waits (kthread.c)
void kthread_init(CPU *main);
uint32_t kthread_create(uint32_t start, uint32_t ctx1, uint32_t ctx2, uint32_t stack, int suspended,
                        uint32_t system_routine);
// A game's single-processor priority lock, patched to "out PRIO_LOCK_PORT, al"
// and "out PRIO_UNLOCK_PORT, al" (games/<set>/patches.txt): see kthread.c.
#define PRIO_LOCK_PORT 0xe8
#define PRIO_UNLOCK_PORT 0xe9
// CRI's "release the lock entirely / take it again n times" pair, which on
// the Xbox looks at the thread's priority: "in eax, PRIO_RELEASE_PORT" gives
// up all of this thread's depth and reads it; "out PRIO_RETAKE_PORT, eax"
// takes the lock that many times
#define PRIO_RELEASE_PORT 0xea
#define PRIO_RETAKE_PORT 0xeb
int prio_release_all(void);
void prio_retake(int n);
void prio_enter(void);
extern int guest_one_core;     // one guest thread runs at a time (set before run_game; kthread.c)
void kthread_yield(void);                       // let the next waiting thread run first
void kthread_host_sleep(unsigned us);           // a host sleep in a kernel call; others may run
void kthread_main_done(void);                   // the entry point returned
void prio_leave(void);
uint32_t kthread_current(void);               // guest address of the running thread's KTHREAD
void kthread_exit(uint32_t status) __attribute__((noreturn));
void kthread_check_suspend(void);              // stop here if another thread suspended this one
// IRQL: raising to DISPATCH_LEVEL or above holds the processor lock, so code
// that relies on not being interrupted (spin locks, DPC queues) stays atomic
// against other threads and the interrupt thread
uint8_t irql_raise(uint8_t to);
void irql_lower(uint8_t to);
uint8_t irql_current(void);
void cpu_lock(void);                          // as KeRaiseIrql(HIGH_LEVEL), for the interrupt thread
void cpu_unlock(void);

// dispatcher objects (KEVENT, KSEMAPHORE, KMUTANT, KTIMER, KTHREAD) live in
// guest memory with the kernel's own layouts; a wait blocks on a host
// condition variable woken by every signal
#define STATUS_SUCCESS 0x00000000u
#define STATUS_TIMEOUT 0x00000102u
#define STATUS_ALERTED 0x00000101u
#define STATUS_USER_APC 0x000000C0u
#define STATUS_PENDING 0x00000103u
#define STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define STATUS_OBJECT_NAME_COLLISION 0xC0000035u
#define STATUS_INVALID_HANDLE 0xC0000008u
#define STATUS_INVALID_PARAMETER 0xC000000Du
#define STATUS_NO_MEMORY 0xC0000017u
#define STATUS_END_OF_FILE 0xC0000011u
#define STATUS_NOT_IMPLEMENTED 0xC0000002u
#define STATUS_NO_MORE_FILES 0x80000006u
#define STATUS_ACCESS_DENIED 0xC0000022u
#define STATUS_OBJECT_PATH_NOT_FOUND 0xC000003Au
#define STATUS_INVALID_DEVICE_REQUEST 0xC0000010u
#define STATUS_UNSUCCESSFUL 0xC0000001u
#define STATUS_BUFFER_TOO_SMALL 0xC0000023u
#define STATUS_SEMAPHORE_LIMIT_EXCEEDED 0xC0000047u
#define STATUS_MUTANT_NOT_OWNED 0xC0000046u
#define STATUS_ABANDONED 0x00000080u

uint32_t ke_wait(int count, const uint32_t *objects, int wait_all, int64_t *timeout_100ns, int alertable);
void ke_signal(uint32_t obj);                 // after a SignalState change: wake its waiters
void ke_set_event(uint32_t ev);
void ke_insert_dpc(uint32_t dpc, uint32_t a1, uint32_t a2);
void ke_set_timer(uint32_t timer, int64_t due_100ns, int32_t period_ms, uint32_t dpc);
int ke_cancel_timer(uint32_t timer);
void kobj_poll(void);                         // timers and queued DPCs, from the interrupt thread
uint64_t ke_interrupt_time(void);             // 100 ns units since boot
// the core's one time source (clock.c): the host's clocks, or in render mode
// (the front end's clock=frames option) time that stops at each frame's end
// until the next run_frame; everything the game sees of time reads it
extern int clk_render;
void clk_init(int render);                     // create: render mode or not
uint64_t clk_mono_ns(void);                    // CLOCK_MONOTONIC, or render time
uint64_t clk_real_ns(void);                    // CLOCK_REALTIME, or render time from the start's date
uint64_t clk_epoch(void);                      // where a count of 60ths or APU frames starts
int64_t clk_host_wait(uint64_t at, uint64_t cap);   // host ns until clk_mono_ns reaches at, at most cap
void clk_frame(void);                          // render: run_frame asks for one more frame
int clk_at_limit(void);                        // render: time stands at the frame's end
int clk_tick_due(void);                        // render: the end-of-frame tick is still to run
void clk_frame_ticked(void);                   // render: it ran (interrupt thread)
void clk_irq_live(int live);                   // the interrupt thread started or ended
int clk_wait_frame(volatile int *stop, uint64_t host_cap_ns);   // render: until the frame has run
void kthread_clock_kick(void);                 // wake the interrupt thread: time moved on
void kthread_freeze(int on);                   // render mode: hold the game's threads (or let them go on)
void kthread_freeze_point(void);               // a kernel call: stop here while held
uint64_t ke_system_time(void);                // 100 ns units since 1601
// interrupts: the GPU's (vblank, PGRAPH) arrive on the interrupt thread
void ke_connect_interrupt(uint32_t vector, uint32_t kinterrupt);
void ke_raise_interrupt(uint32_t vector);

// handles (NtCreateEvent, files, threads)
uint32_t ob_handle_new(uint32_t object, int kind);
uint32_t ob_object(uint32_t handle, int *kind);
void ob_handle_close(uint32_t handle);
enum { OB_EVENT = 1, OB_SEMAPHORE, OB_MUTANT, OB_TIMER, OB_THREAD, OB_FILE, OB_DIR, OB_SYMLINK };

// files (kfile.c)
void kfile_init(void);

// ---- devices ----------------------------------------------------------------------------------
void nv2a_init(void);
void nv2a_poll(void);           // run the pushbuffer, on the GPU thread
extern volatile uint32_t nv2a_frames;
void chihiro_init(void);        // media board, baseboard (JVS), EEPROM
void apu_init(void);
void apu_tick(void);            // the audio frames due, from the interrupt thread
int chihiro_audio_read(int16_t *out, int frames);   // played sound: stereo 48 kHz

// Stopping: guest_stop sets guest_stopping; threads leave at their next wait
extern volatile int guest_stopping;
void guest_exit_thread(void) __attribute__((noreturn));
void guest_stop(void);            // end every thread of the game (the core library)
void guest_reset(void);           // then back to power-on state, for run_game again
extern void (*host_fatal_hook)(const char *why);   // the core library's: does not return

#ifdef __cplusplus
}
#endif
