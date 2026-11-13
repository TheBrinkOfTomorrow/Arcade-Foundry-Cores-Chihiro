// Guest memory, XBE loading, dispatch and the CPU helpers generated code calls.
#define _GNU_SOURCE
#include "xbox.h"
#include "baseboard.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#include <mach/mach.h>
#if TARGET_OS_IPHONE
#include <mach/vm_map.h>   // the iOS SDK has no mach_vm.h; vm_remap is 64-bit on arm64
#else
#include <mach/mach_vm.h>
#endif
#endif

uint8_t *M;
int hle_verbose = 1;
int hle_trace;
char game_root[1024];
char save_root[1024];
const gen_program *prog;
xbe_info xbe;
volatile int guest_stopping;

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

// a message repeated many times is cut down: the first 20 in full, then one
// line every 1000 with its count
void hle_log(const char *fmt, ...)
{
    static struct { uint32_t hash; unsigned count; } seen[256];
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    uint32_t h = 2166136261u;
    for (const char *p = buf; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    pthread_mutex_lock(&log_lock);
    unsigned *count = &seen[h & 255].count;
    if (seen[h & 255].hash != h) seen[h & 255].hash = h, *count = 0;
    ++*count;
    if (*count <= 20) fputs(buf, stderr);
    else if (*count % 1000 == 0) fprintf(stderr, "(the next line has been logged %u times)\n%s", *count, buf);
    fflush(stderr);
    pthread_mutex_unlock(&log_lock);
}

void (*host_fatal_hook)(const char *why);

void host_fatal(const char *why)
{
    hle_log("fatal: %s\n", why);
    if (host_fatal_hook) host_fatal_hook(why);   // the core library: the game stops, the app carries on
    abort();
}

// ---- memory ------------------------------------------------------------------------------
// 4 GB of address space, with the RAM object mapped at its three views
static void map_ram_view(int fd, uint32_t at)
{
    void *p = mmap(M + at, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (p == MAP_FAILED) host_fatal("cannot map the Xbox RAM");
}

#if defined(__APPLE__)
// Apple: no shm_open in the app sandbox (iOS, and the sandboxed macOS app:
// "cannot create the Xbox RAM"). The RAM is the window's own pages at 0, and
// the other two views are the same pages remapped (shared, not copied).
static void map_ram_alias(uint32_t at)
{
    vm_address_t dst = (vm_address_t)(uintptr_t)(M + at);
    vm_prot_t cur, max;
    kern_return_t kr = vm_remap(mach_task_self(), &dst, RAM_SIZE, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                mach_task_self(), (vm_address_t)(uintptr_t)M, FALSE, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS || dst != (vm_address_t)(uintptr_t)(M + at)) host_fatal("cannot map the Xbox RAM");
}
#endif

static void mem_init(void)
{
    // a 4 GB window needs iOS's extended virtual addressing (the app's entitlement)
    void *p = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) host_fatal("cannot reserve the guest's 4 GB address space");
    M = p;
#if defined(__APPLE__)
    // the pages at 0 must be shared before they are aliased, or a remap of a
    // private mapping copies on write
    void *r = mmap(M, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (r == MAP_FAILED) host_fatal("cannot map the Xbox RAM");
    map_ram_alias(RAM_CONTIG);
    map_ram_alias(RAM_WC);
    return;
#endif
#if defined(__linux__)
    int fd = memfd_create("xbox-ram", 0);
#else
    char name[64];
    snprintf(name, sizeof name, "/af-chihiro-%d", (int)getpid());
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) shm_unlink(name);
#endif
    if (fd < 0 || ftruncate(fd, RAM_SIZE)) host_fatal("cannot create the Xbox RAM");
    map_ram_view(fd, 0);
    map_ram_view(fd, RAM_CONTIG);
    map_ram_view(fd, RAM_WC);
    close(fd);
}

static uint32_t hostdata_top = HOSTDATA_BASE;
uint32_t hostdata_alloc(uint32_t n)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    uint32_t p = hostdata_top;
    hostdata_top = (hostdata_top + n + 15) & ~15u;
    pthread_mutex_unlock(&lock);
    if (hostdata_top > HOSTDATA_END) host_fatal("host data exhausted");
    memset(M + p, 0, n);
    return p;
}

static uint32_t stacks_top = STACKS_BASE;
uint32_t stack_alloc(uint32_t size)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    size = (size + 0xffff) & ~0xffffu;
    pthread_mutex_lock(&lock);
    uint32_t base = stacks_top + 0x10000;    // a gap below each stack
    stacks_top = base + size;
    pthread_mutex_unlock(&lock);
    if (stacks_top > STACKS_END) host_fatal("thread stacks exhausted");
    return base + size - 16;
}

// physical pages: blocks in address order, first fit
#define PAGE 4096u
static pthread_mutex_t phys_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t page_used[RAM_SIZE / PAGE];
static uint32_t block_pages[RAM_SIZE / PAGE];    // at a block's first page: its length
static uint32_t phys_floor;                      // first page the allocator may hand out

uint32_t phys_alloc(uint32_t size, uint32_t align, uint32_t lo, uint32_t hi)
{
    uint32_t n = (size + PAGE - 1) / PAGE;
    if (!n) n = 1;
    if (align < PAGE) align = PAGE;
    uint32_t step = align / PAGE;
    uint32_t first = lo / PAGE, last = hi ? hi / PAGE : RAM_SIZE / PAGE - 1;
    if (first < phys_floor) first = phys_floor;
    first = (first + step - 1) / step * step;
    pthread_mutex_lock(&phys_lock);
    for (uint32_t p = first; p + n - 1 <= last && p + n <= RAM_SIZE / PAGE; p += step) {
        uint32_t k = 0;
        while (k < n && !page_used[p + k]) k++;
        if (k == n) {
            memset(page_used + p, 1, n);
            block_pages[p] = n;
            pthread_mutex_unlock(&phys_lock);
            return p * PAGE;
        }
    }
    pthread_mutex_unlock(&phys_lock);
    return ~0u;
}

void phys_free(uint32_t pa)
{
    uint32_t p = phys_of(pa) / PAGE;
    pthread_mutex_lock(&phys_lock);
    uint32_t n = block_pages[p];
    if (n) {
        memset(page_used + p, 0, n);
        block_pages[p] = 0;
    }
    pthread_mutex_unlock(&phys_lock);
}

uint32_t phys_size(uint32_t pa)
{
    return block_pages[phys_of(pa) / PAGE] * PAGE;
}

uint32_t phys_free_bytes(void)
{
    uint32_t n = 0;
    for (uint32_t p = phys_floor; p < RAM_SIZE / PAGE; p++) n += !page_used[p];
    return n * PAGE;
}

static void phys_reserve(uint32_t lo, uint32_t hi)
{
    for (uint32_t p = lo / PAGE; p < (hi + PAGE - 1) / PAGE && p < RAM_SIZE / PAGE; p++) page_used[p] = 1;
}

// ---- host functions at guest addresses -----------------------------------------------------
#define MAX_MAGIC 8192
static hle_fn magic_fns[MAX_MAGIC];
static const char *magic_names[MAX_MAGIC];
static unsigned magic_count;

uint32_t magic_register(hle_fn fn, const char *name)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    if (magic_count == MAX_MAGIC) host_fatal("too many host functions");
    unsigned i = magic_count++;
    magic_fns[i] = fn;
    magic_names[i] = name;
    pthread_mutex_unlock(&lock);
    return MAGIC_BASE + i * 16;
}

guest_fn guest_lookup(uint32_t addr)
{
    unsigned lo = 0, hi = prog->fn_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (prog->fn_addr[mid] < addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo < prog->fn_count && prog->fn_addr[lo] == addr) return prog->fn_ptr[lo];
    return NULL;
}

// A host function returns like the guest one would: it pops the return
// address here, and its own arguments (stdcall) by adjusting esp itself.
void guest_dispatch(CPU *c, uint32_t addr)
{
    guest_fn f = guest_lookup(addr);
    if (f) {
        f(c);
        return;
    }
    if (addr >= MAGIC_BASE && addr < MAGIC_BASE + MAX_MAGIC * 16 && magic_fns[(addr - MAGIC_BASE) / 16]) {
        unsigned k = (addr - MAGIC_BASE) / 16;
        magic_fns[k](c);
        return;
    }
    char why[160];
    snprintf(why, sizeof why, "no function at 0x%08x (called from 0x%08x)", addr, rd32(c->r[ESP]));
    host_fatal(why);
}

uint32_t guest_call(CPU *c, uint32_t addr, int nargs, const uint32_t *args)
{
    uint32_t saved[8];
    memcpy(saved, c->r, sizeof saved);
    uint32_t sp = c->r[ESP];
    for (int i = nargs - 1; i >= 0; i--) {
        sp -= 4;
        wr32(sp, args[i]);
    }
    sp -= 4;
    wr32(sp, RETURN_MAGIC);
    c->r[ESP] = sp;
    guest_dispatch(c, addr);
    uint32_t r = c->r[EAX];
    // works for stdcall and cdecl: esp is restored from before the pushes
    saved[EAX] = r;
    saved[EDX] = c->r[EDX];
    saved[ECX] = c->r[ECX];
    memcpy(c->r, saved, sizeof saved);
    return r;
}

uint32_t guest_fastcall(CPU *c, uint32_t addr, uint32_t ecx, uint32_t edx)
{
    uint32_t saved[8];
    memcpy(saved, c->r, sizeof saved);
    c->r[ECX] = ecx;
    c->r[EDX] = edx;
    c->r[ESP] -= 4;
    wr32(c->r[ESP], RETURN_MAGIC);
    guest_dispatch(c, addr);
    uint32_t r = c->r[EAX];
    saved[EAX] = r;
    memcpy(c->r, saved, sizeof saved);
    return r;
}

void guest_unimpl(CPU *c, uint32_t addr, const char *what)
{
    char why[200];
    snprintf(why, sizeof why, "unimplemented at 0x%08x: %s", addr, what);
    host_fatal(why);
}

void guest_bad_return(CPU *c, uint32_t from, uint32_t expected, uint32_t got)
{
    char why[128];
    snprintf(why, sizeof why, "bad return at 0x%08x: expected 0x%08x got 0x%08x", from, expected, got);
    host_fatal(why);
}

// I/O ports: the MCPX's ACPI block at 0x8000 (0x80c0 is the GPU's
// interrupt line, which D3D's interrupt handler reads), SMBus at 0xc000.
// Nothing answers yet; each port is logged the first time.
static uint8_t port_seen[65536];

void guest_port_out(CPU *c, uint16_t port, uint32_t value, int size)
{
    // not Xbox hardware: a game's priority lock, patched to these (see prio_enter)
    if (port == PRIO_LOCK_PORT) return prio_enter();
    if (port == PRIO_UNLOCK_PORT) return prio_leave();
    if (port == PRIO_RETAKE_PORT) return prio_retake((int)value);
    if (!port_seen[port] || hle_verbose > 2) hle_log("port out 0x%04x = 0x%x (%d bits)\n", port, value, size);
    port_seen[port] = 1;
}

// The media board's LPC registers at 0x4000-0x40ff, as MAME's chihiro.cpp
// answers them (mediaboard_r): the "XBAM" signature at 0x401e-0x4024,
// board present at 0x40f0, DIMM size code 2 (512 MB) at 0x40f4.
static uint32_t mediaboard_in(uint16_t port, int size)
{
    static const uint16_t words[0x80] = {
        [0x1e / 2] = 0x1000, [0x20 / 2] = 0x00a0, [0x22 / 2] = 0x4258, [0x24 / 2] = 0x4d41,
        [0xf4 / 2] = 2,
    };
    uint32_t o = port & 0xff, v = 0;
    for (int i = 0; i < size / 8; i++, o++)
        v |= (uint32_t)(words[(o & 0xff) / 2] >> (8 * (o & 1)) & 0xff) << (8 * i);
    return v;
}

uint32_t guest_port_in(CPU *c, uint16_t port, int size)
{
    if (port == PRIO_RELEASE_PORT) return (uint32_t)prio_release_all();   // see prio_enter
    if (!port_seen[port] || hle_verbose > 2) hle_log("port in 0x%04x (%d bits)\n", port, size);
    port_seen[port] = 1;
    if (port >= 0x4000 && port < 0x4100) return mediaboard_in(port, size);
    return 0;
}

void guest_cpuid(CPU *c, uint32_t *a, uint32_t *b, uint32_t *cc, uint32_t *d)
{
    // the Chihiro's 733 MHz Pentium III (Coppermine): MMX, SSE, no SSE2
    switch (*a) {
    case 0:
        *a = 2;
        *b = 0x756e6547;
        *d = 0x49656e69;
        *cc = 0x6c65746e;
        break;
    case 1:
        *a = 0x0000068a;
        *b = 0;
        *cc = 0;
        *d = 0x0383f9ff;
        break;
    default:
        *a = *b = *cc = *d = 0;
        break;
    }
}

// int 0x2d: service 1 prints the ANSI_STRING at a; the rest need a debugger
void guest_debug_service(CPU *c, uint32_t service, uint32_t a, uint32_t b)
{
    (void)c; (void)b;
    if (service == 1 && hle_verbose > 0) {
        uint16_t len = rd16(a);
        uint32_t p = rd32(a + 4);
        char buf[512];
        if (len >= sizeof buf) len = sizeof buf - 1;
        for (uint16_t i = 0; i < len; i++) buf[i] = (char)rd8(p + i);
        buf[len] = 0;
        hle_log("dbg: %s%s", buf, len && buf[len - 1] == '\n' ? "" : "\n");
    }
}

uint64_t guest_rdtsc(CPU *c)
{
    if (guest_stopping) guest_exit_thread();
    // the kernel reports 733,333,333 Hz (KeQueryPerformanceFrequency uses the
    // ACPI timer; the TSC runs at the CPU clock); the core's clock
    uint64_t ns = clk_mono_ns();
    return ns / 1000000000ull * 733333333ull + ns % 1000000000ull * 733333333ull / 1000000000ull;
}

// ---- x87 helpers ---------------------------------------------------------------------------
double x87_load80(uint32_t a)
{
    uint64_t mant = rd64(a);
    uint16_t se = rd16(a + 8);
    int sign = se >> 15;
    int exp = se & 0x7fff;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7fff) v = (mant << 1) ? NAN : INFINITY;
    else v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}

void x87_store80(uint32_t a, double v)
{
    uint64_t mant = 0;
    uint16_t se = 0;
    if (signbit(v)) se = 0x8000;
    v = fabs(v);
    if (v == 0.0) {
    } else if (isinf(v)) {
        se |= 0x7fff;
        mant = 1ull << 63;
    } else if (isnan(v)) {
        se |= 0x7fff;
        mant = 0xc000000000000000ull;
    } else {
        int e;
        double m = frexp(v, &e);
        mant = (uint64_t)ldexp(m, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    wr64(a, mant);
    wr16(a + 8, se);
}

// fnsave/frstor: the 108-byte 32-bit protected-mode image
void x87_fnsave(CPU *c, uint32_t a)
{
    memset(M + a, 0, 108);
    wr16(a, c->fcw);
    wr16(a + 4, fstatus(c));
    wr16(a + 8, 0);    // tag word: all valid (the tags are not tracked)
    for (int i = 0; i < 8; i++) x87_store80(a + 28 + 10 * i, ST(i));
    c->fcw = 0x37f;
    c->fsw = 0;
    c->ftop = 0;
}

void x87_frstor(CPU *c, uint32_t a)
{
    c->fcw = rd16(a);
    uint16_t sw = rd16(a + 4);
    c->ftop = (sw >> 11) & 7;
    c->fsw = sw & ~0x3800;
    for (int i = 0; i < 8; i++) ST(i) = x87_load80(a + 28 + 10 * i);
}

// C3 C2 C0 for the class of ST(0); C1 its sign
void x87_fxam(CPU *c)
{
    double v = ST(0);
    uint16_t cc;
    if (isnan(v)) cc = 0x0100;
    else if (isinf(v)) cc = 0x0500;
    else if (v == 0.0) cc = 0x4000;
    else if (fpclassify(v) == FP_SUBNORMAL) cc = 0x4400;
    else cc = 0x0400;
    if (signbit(v)) cc |= 0x0200;
    c->fsw = (c->fsw & ~0x4700) | cc;
}

static int64_t round_mode(double v, int mode)
{
    switch (mode & 3) {
    case 0: return (int64_t)nearbyint(v);
    case 1: return (int64_t)floor(v);
    case 2: return (int64_t)ceil(v);
    default: return (int64_t)trunc(v);
    }
}

int64_t x87_round(CPU *c, double v) { return round_mode(v, (c->fcw >> 10) & 3); }
int64_t sse_round(CPU *c, double v) { return round_mode(v, (c->mxcsr >> 13) & 3); }

// ---- per-thread CPU ----------------------------------------------------------------------------
static __thread CPU *tls_cpu;
CPU *cpu_current(void) { return tls_cpu; }
void cpu_set_current(CPU *c) { tls_cpu = c; }

CPU *cpu_new(void)
{
    CPU *c = calloc(1, sizeof(CPU));
    c->fcw = 0x37f;
    c->mxcsr = 0x1f80;
    return c;
}

// ---- XBE -------------------------------------------------------------------------------------
static const uint32_t entry_keys[3] = {0xA8FC57AB, 0x94859D4B, 0x40B5C16E};
static const uint32_t thunk_keys[3] = {0x5B6D40B6, 0xEFB1F152, 0x2290059D};

int load_xbe(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        hle_log("cannot open %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(size);
    if (fread(d, 1, size, f) != (size_t)size) {
        fclose(f);
        return -1;
    }
    fclose(f);
    uint32_t base, hsize, isize, cert, nsec, secaddr, ep, th, tls;
    memcpy(&base, d + 0x104, 4);
    memcpy(&hsize, d + 0x108, 4);
    memcpy(&isize, d + 0x10C, 4);
    memcpy(&cert, d + 0x118, 4);
    memcpy(&nsec, d + 0x11C, 4);
    memcpy(&secaddr, d + 0x120, 4);
    memcpy(&ep, d + 0x128, 4);
    memcpy(&tls, d + 0x12C, 4);
    memcpy(&th, d + 0x158, 4);
    xbe.base = base;
    xbe.image_size = isize;
    xbe.cert = cert;
    xbe.tls = tls;
    for (int k = 0; k < 3; k++)
        if ((ep ^ entry_keys[k]) - base < isize && (th ^ thunk_keys[k]) - base < isize) {
            xbe.entry = ep ^ entry_keys[k];
            xbe.thunk = th ^ thunk_keys[k];
        }
    if (!xbe.entry) {
        hle_log("%s: unknown XBE key\n", path);
        return -1;
    }
    // the headers, then each section at its address (uninitialised tails zero)
    memcpy(M + base, d, hsize);
    for (uint32_t i = 0; i < nsec; i++) {
        uint32_t s[6];
        memcpy(s, d + secaddr - base + i * 0x38, sizeof s);
        uint32_t va = s[1], vs = s[2], raw = s[3], rs = s[4];
        memset(M + va, 0, vs);
        memcpy(M + va, d + raw, rs < vs ? rs : vs);
    }
    // the title, from the certificate (UTF-16)
    for (int i = 0; i < 40; i++) xbe.title[i] = (char)d[cert - base + 0xC + 2 * i];
    xbe.title[40] = 0;
    free(d);
    // the image's pages are not the allocator's
    phys_reserve(0, base + isize);
    phys_floor = (base + isize + 0xffff) / PAGE;
    // kernel imports
    for (uint32_t a = xbe.thunk; rd32(a); a += 4) {
        uint32_t ord = rd32(a) & 0x7fffffff;
        wr32(a, kernel_export(ord));
    }
    hle_log("loaded %s: \"%s\" base 0x%x size 0x%x entry 0x%x\n", path, xbe.title, base, isize, xbe.entry);
    return 0;
}

void guest_exit_thread(void)
{
    kthread_exit(0);
}

int launch_test_mode;
// a game whose test mode is a program of its own (Crazy Taxi High Roller's
// ctx_ac_t.xbe): its translation, set by the front end. run_game with it is
// the test mode, so no launch data is passed.
const gen_program *test_program;

// ---- stopping and starting again (the core library: runtime/core_api.c) -----------------------
void kthread_stop_all(void);
void kthread_reset(void);
void kernel_reset(void);
void kfile_reset(void);
void nv2a_stop(void);
void nv2a_reset(void);
void apu_reset(void);
void ohci_reset(void);
void baseboard_reset(void);

// every thread the game started has ended, and the GPU thread with the renderer
// (the thread that called run_game ends at its next check; its caller joins it)
void guest_stop(void)
{
    guest_stopping = 1;
    kthread_stop_all();
    nv2a_stop();
}

// back to power-on state for the next run_game, once guest_stop and the join are done
void guest_reset(void)
{
    kthread_reset();
    kernel_reset();
    kfile_reset();
    nv2a_reset();
    apu_reset();
    ohci_reset();
    baseboard_reset();
    hostdata_top = HOSTDATA_BASE;
    stacks_top = STACKS_BASE;
    memset(page_used, 0, sizeof page_used);
    memset(block_pages, 0, sizeof block_pages);
    phys_floor = 0;
    memset(magic_fns, 0, sizeof magic_fns);
    memset(magic_names, 0, sizeof magic_names);
    magic_count = 0;
    memset(&xbe, 0, sizeof xbe);
    memset(port_seen, 0, sizeof port_seen);
    if (M) munmap(M, 1ull << 32);
    M = NULL;
    prog = NULL;
    guest_stopping = 0;
}

void run_game(const gen_program *p, const char *xbe_path)
{
    prog = p;
    mem_init();
    kernel_init();
    if (load_xbe(xbe_path)) host_fatal("cannot load the XBE");
    if (launch_test_mode && p != test_program) kernel_launch_test_mode();
    baseboard_panel_buttons = p == test_program;
    CPU *c = cpu_new();
    cpu_set_current(c);
    kthread_init(c);
    nv2a_init();
    apu_init();
    chihiro_init();
    // the entry point is called like a thread start routine
    uint32_t r = guest_call(c, xbe.entry, 0, NULL);
    hle_log("entry point returned 0x%x\n", r);
    kthread_main_done();
}

// MSVC's longjmp(buf, value): back to the host setjmp the translated
// _setjmp3 made, whose jmp_buf pointer is in the guest buffer's UnwindData
#include <setjmp.h>
void ovr_msvc_longjmp(CPU *c)
{
    uint32_t buf = rd32(c->r[ESP] + 4), v = rd32(c->r[ESP] + 8);
    jmp_buf *hp;
    memcpy(&hp, M + buf + 40, sizeof hp);
    if (rd32(buf + 32) != 0x56433230u || !hp) host_fatal("longjmp to a buffer setjmp did not fill");
    longjmp(*hp, v ? (int)v : 1);
}
