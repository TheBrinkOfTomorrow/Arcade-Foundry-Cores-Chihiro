// The game as an Arcade Foundry core (common/include/afcore.h): the front end calls
// run_frame; the game runs on threads of its own, and the GPU thread hands
// over each frame it shows in present(), where it waits for the next run_frame.
//
// Render mode (the option clock=frames, for video renders): the game's time
// moves 1/60 s per run_frame however long the front end takes (runtime/clock.c),
// and each run_frame returns the newest screen the game showed before that
// frame's end, with exactly that frame's sound (800 samples).
//
// The test button reboots a Chihiro into its system menu, which starts the
// game again in its test menu; leaving the menu reboots into the game. Here
// a reboot ends every thread of the game, puts the runtime back to power-on
// state and starts the game again, in its test menu or not.
#include "af_debug.h"
#include "afcore.h"
#include "xbox.h"
#include "pgraph.h"
#include "baseboard.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STR2(x) #x
#define STR(x) STR2(x)
#define API_FN2(r) afcore_api_##r
#define API_FN(r) API_FN2(r)

// the program this core was built from: its name in the game folder and its size (CMake)
extern const gen_program gen_program_game;
#ifdef CORE_TEST_XBE
extern const gen_program gen_program_test;   // the game's test program (CMake GAME_TEST_XBE)
#endif

AFCORE_EXPORT const afcore_api *API_FN(CORE_ROMSET)(void);

enum { STOP_NONE, STOP_REBOOT, STOP_FATAL, STOP_DESTROY };

#define AUDIO_RATE 48000
#define AUDIO_CAP (AUDIO_RATE * 2)     // frames: two seconds

struct afcore {
    char root[1024], xbe[1100], test_xbe[1100];
    pthread_t super;
    int in_test;                        // the next start is into the game's test menu
    char error[512];
};

static afcore *the_core;   // the runtime is one per process
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int stop_reason, destroying, reboot_into_test;
static int go;                          // run_frame's permit for the GPU thread to pass its next present
static unsigned frame_seq;
typedef struct { uint32_t *px; size_t cap; int w, h; } framebuf;
static framebuf frames[3];              // [0] screen() returns it, [1] the game's latest,
                                        // [2] render mode: shown while time stands at a frame's end (the next frame's)
static int fresh, fresh_next;           // frames[1] is newer than [0]; frames[2] holds one
static int opt_render;                  // clock=frames for the next create (set_option)
static uint32_t last_buttons;
static float pump_aim[2][2] = { { 0.5f, 0.5f }, { 0.5f, 0.5f } };   // CORE_PUMP: each gun's last aim on the screen
static int pump_t[2], pump_was[2];   // CORE_PUMP: the frame of each gun's pump (0: none), its reload held last frame

// ---- what the runtime asks of its host -----------------------------------------------------------
// under lock: the game must stop (it rebooted, or failed, or the front end is leaving)
static void request_stop(int why)
{
    if (!stop_reason) stop_reason = why;
    guest_stopping = 1;
    pthread_cond_broadcast(&cv);
}

// the GPU thread, each frame shown: lockstep with the front end
static void present(const uint32_t *px, int w, int h)
{
    size_t n = (size_t)w * h;
    pthread_mutex_lock(&lock);
    // frames[0] is the front end's until its next run_frame: only [1] changes here
    // (render mode: [2] while time stands at the frame's end). In both modes
    // it then waits for the next run_frame: Crazy Taxi's D3D counts on that
    // (with flips not held, it stayed on its Hitmaker logo)
    int next = clk_render && clk_at_limit();
    framebuf *f = &frames[next ? 2 : 1];
    if (f->cap < n) {
        free(f->px);
        f->px = malloc(n * 4);
        f->cap = n;
    }
    memcpy(f->px, px, n * 4);
    f->w = w, f->h = h;
    if (next) fresh_next = 1;
    else fresh = 1;
    frame_seq++;
    pthread_cond_broadcast(&cv);
    while (!go && !guest_stopping) pthread_cond_wait(&cv, &lock);
    go = 0;
    pthread_mutex_unlock(&lock);
    // stopping: the GPU thread sees guest_stopping and ends with its renderer
}

// k_HalReturnToFirmware: the test button, or leaving the test menu
static void core_reboot(int into_test)
{
    hle_log("restarting the game%s\n", into_test ? " in its test menu" : "");
    pthread_mutex_lock(&lock);
    reboot_into_test = into_test;
    request_stop(STOP_REBOOT);
    pthread_mutex_unlock(&lock);
    guest_exit_thread();
}

static void fatal(const char *why)
{
    pthread_mutex_lock(&lock);
    if (the_core && !the_core->error[0]) snprintf(the_core->error, sizeof the_core->error, "%s", why);
    request_stop(STOP_FATAL);
    pthread_mutex_unlock(&lock);
    guest_exit_thread();
}

// ---- the game's threads ------------------------------------------------------------------------------
static void *guest_main(void *p)
{
    afcore *c = p;
    // the entry point returning ends this thread only: the game's own threads carry on
    if (launch_test_mode && test_program) run_game(test_program, c->test_xbe);
    else run_game(&gen_program_game, c->xbe);
    return NULL;
}

// starts the game, waits for it to stop, cleans up, and starts it again after a reboot
static void *supervise(void *p)
{
    afcore *c = p;
    for (;;) {
        snprintf(game_root, sizeof game_root, "%s", c->root);
        launch_test_mode = c->in_test;
        guest_one_core = CORE_ONE_CORE && !af_debug_env("CH_ALLCORES");
        gpu_offscreen = 1;
        gpu_present = present;
        host_fatal_hook = fatal;
        front_end_reboot = core_reboot;
#ifdef CORE_TEST_XBE
        test_program = c->test_xbe[0] ? &gen_program_test : NULL;
#endif
        hle_log("starting %s%s\n", c->in_test && test_program ? c->test_xbe : c->xbe, c->in_test ? " in its test menu" : "");
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 8 << 20);   // generated code nests deep
        pthread_t t;
        pthread_create(&t, &at, guest_main, c);
        pthread_attr_destroy(&at);

        pthread_mutex_lock(&lock);
        while (!stop_reason && !destroying) pthread_cond_wait(&cv, &lock);
        int why = stop_reason;
        pthread_mutex_unlock(&lock);

        guest_stop();      // every thread the game started has ended, the renderer with the GPU thread
        pthread_join(t, NULL);
        guest_reset();
        pthread_mutex_lock(&lock);
        stop_reason = STOP_NONE;
        go = 0;
        int leaving = destroying;
        c->in_test = reboot_into_test;
        pthread_mutex_unlock(&lock);
        if (leaving || why != STOP_REBOOT) break;
    }
    return NULL;
}

// ---- the controls -----------------------------------------------------------------------------------
// The guns, as the games' INPUT TEST shows them: trigger 0x0200, screen-out
// 0x0100 (the gun sees no screen: its aim reads 0), the gun's button 0x0080
// (VC3: change weapon), the pedal 0x0040, start 0x8000, service 0x4000;
// player 1's aim on analog channels 0 (x) and 1 (y), player 2's on 2 and 3.
static uint16_t axis(float v)
{
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    return (uint16_t)(v * 65535.0f + 0.5f);
}

static int on_screen(float x, float y) { return x >= 0 && x <= 1 && y >= 0 && y <= 1; }

// The driving games (CORE_DRIVE; Crazy Taxi High Roller), as its test
// program's INPUT TEST names them: the wheel on analog channel 0 (centre
// 0x8000), the accelerator on 1 and the brake on 2 (AD1, AD2); the shifter's
// DRIVE GEAR and REVERSE GEAR are player 1's up (0x2000) and down (0x1000),
// afcore's SHIFT_UP and SHIFT_DOWN; JUMP is button 1 or 2 (views 1 and 2).
static void set_drive_inputs(const afcore_input *in, uint32_t b)
{
    uint16_t v = 0;
    if (b & AFCORE_START) v |= 0x8000;
    if (b & AFCORE_SERVICE) v |= 0x4000;
    if (b & (AFCORE_SHIFT_UP | AFCORE_UP)) v |= 0x2000;
    if (b & (AFCORE_SHIFT_DOWN | AFCORE_DOWN)) v |= 0x1000;
    // JUMP: buttons 1 (0x0200) and 2 (0x0100) both light it in the INPUT
    // TEST, found by sending a different button from each of views 1-4. The
    // app's views 1 and 2 (A and B, keys 1 and 2) press it.
    if (b & AFCORE_VIEW1) v |= 0x0200;
    if (b & AFCORE_VIEW2) v |= 0x0100;
    jvs_inputs.player[0] = v;
    jvs_inputs.player[1] = 0;
    float s = in->steer < -1 ? -1 : in->steer > 1 ? 1 : in->steer;
    jvs_inputs.analog[0] = axis((s + 1) * 0.5f);
    jvs_inputs.analog[1] = axis(in->accel);
    jvs_inputs.analog[2] = axis(in->brake);
}

static void set_inputs(const afcore_input *in)
{
    uint32_t b = in->buttons, rise = b & ~last_buttons;
    last_buttons = b;
    if (CORE_DRIVE) {
        set_drive_inputs(in, b);
        if (rise & AFCORE_COIN) jvs_inputs.coins[0]++;
        jvs_inputs.system = b & AFCORE_TEST ? 0x80 : 0;
        return;
    }
    const uint32_t trigger[2] = {AFCORE_TRIGGER1, AFCORE_TRIGGER2};
    const uint32_t start[2] = {AFCORE_START, AFCORE_START2};
    const uint32_t mine[2] = {b, in->buttons2};   // player 2's gun button and pedal in buttons2
    for (int p = 0; p < 2; p++) {
        uint16_t v = 0;
        if (b & start[p]) v |= 0x8000;
        if (b & trigger[p]) v |= 0x0200;
        if (mine[p] & AFCORE_BUTTON3) v |= 0x0080;
        if (mine[p] & AFCORE_BUTTON2) v |= 0x0040;
        int seen = on_screen(in->gun_x[p], in->gun_y[p]);
        float gx = in->gun_x[p], gy = in->gun_y[p];
        if (CORE_PUMP) {
            // HotD3's shotgun reloads by its pump: the grip switch (0x0080)
            // pulled twice (one pull does nothing; measured, its RELOAD prompt
            // clears every time). A front end's reload, a shot off screen,
            // starts two pulls (8 frames on, 12 off, 8 on) instead; the gun
            // stays where it last pointed on the screen meanwhile.
            int reload = !seen && (b & trigger[p]);
            if (reload && !pump_was[p] && !pump_t[p]) pump_t[p] = 1;
            pump_was[p] = reload;
            if (seen && !pump_t[p]) {
                pump_aim[p][0] = gx;
                pump_aim[p][1] = gy;
            }
            if (pump_t[p] || reload) {
                int t = pump_t[p];
                if ((t >= 1 && t <= 8) || (t >= 21 && t <= 28)) v |= 0x0080;
                v &= (uint16_t)~0x0200;
                gx = pump_aim[p][0];
                gy = pump_aim[p][1];
                seen = 1;
                if (pump_t[p]) pump_t[p] = pump_t[p] >= 28 ? 0 : pump_t[p] + 1;
            }
        }
        if (!seen) v |= 0x0100;
        if (p == 0) {
            if (b & AFCORE_SERVICE) v |= 0x4000;
            if (b & AFCORE_UP) v |= 0x2000;
            if (b & AFCORE_DOWN) v |= 0x1000;
            if (b & AFCORE_LEFT) v |= 0x0800;
            if (b & AFCORE_RIGHT) v |= 0x0400;
        }
        jvs_inputs.player[p] = v;
        jvs_inputs.analog[2 * p] = seen ? axis(gx) : 0;
        jvs_inputs.analog[2 * p + 1] = seen ? axis(gy) : 0;
    }
    if (rise & AFCORE_COIN) jvs_inputs.coins[0]++;
    jvs_inputs.system = b & AFCORE_TEST ? 0x80 : 0;
}

// ---- the interface ------------------------------------------------------------------------------------
static int is_file(const char *p)
{
    struct stat st;
    return !stat(p, &st) && S_ISREG(st.st_mode);
}

static off_t file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) ? -1 : st.st_size;
}

static afcore *create(const char *rom_path, const char *save_dir, char *err, size_t err_size)
{
    if (the_core) {
        snprintf(err, err_size, "only one Chihiro game can run at a time");
        return NULL;
    }
    afcore *c = calloc(1, sizeof *c);
    // rom_path: the game's folder, the one holding its XBE
    snprintf(c->root, sizeof c->root, "%s", rom_path);
    size_t len = strlen(c->root);
    while (len > 1 && c->root[len - 1] == '/') c->root[--len] = 0;
    snprintf(c->xbe, sizeof c->xbe, "%s/%s", c->root, CORE_XBE);
    if (!is_file(c->xbe)) {
        snprintf(err, err_size, "%s is not a %s game folder: no %s", rom_path, CORE_TITLE, CORE_XBE);
        free(c);
        return NULL;
    }
    if (file_size(c->xbe) != CORE_XBE_SIZE) {
        snprintf(err, err_size, "%s is not the version this core was built from", CORE_XBE);
        free(c);
        return NULL;
    }
#ifdef CORE_TEST_XBE
    // the test program, if the folder has the one this core was built from;
    // without it the test button restarts the game as before
    snprintf(c->test_xbe, sizeof c->test_xbe, "%s/%s", c->root, CORE_TEST_XBE);
    if (file_size(c->test_xbe) != CORE_TEST_XBE_SIZE) {
        hle_log("no %s of this version: the test button restarts the game\n", CORE_TEST_XBE);
        c->test_xbe[0] = 0;
    }
#endif
    if (save_dir) {
        snprintf(save_root, sizeof save_root, "%s", save_dir);
        mkdir(save_root, 0755);
    } else {
        // nothing kept: the saves go to a new scratch folder of their own
        snprintf(save_root, sizeof save_root, "%s/chihiro-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
        if (!mkdtemp(save_root)) {
            snprintf(err, err_size, "no scratch folder for the saves (%s): %s", save_root, strerror(errno));
            save_root[0] = 0;
            free(c);
            return NULL;
        }
    }
    hle_verbose = af_debug_env("CH_VERBOSE") ? atoi(af_debug_env("CH_VERBOSE")) : 0;
    memset(&jvs_inputs, 0, sizeof jvs_inputs);
    for (int i = 0; i < 8; i++) jvs_inputs.analog[i] = 0x8000;
    if (CORE_DRIVE) jvs_inputs.analog[1] = jvs_inputs.analog[2] = 0;   // pedals up
    last_buttons = 0;
    frame_seq = 0;
    fresh = fresh_next = 0;
    clk_init(opt_render);
    if (opt_render) hle_log("render clock: 1/60 s of game time per frame\n");
    opt_render = 0;   // an option holds for the next create only
    stop_reason = STOP_NONE;
    reboot_into_test = 0;
    go = 0;
    the_core = c;
    pthread_create(&c->super, NULL, supervise, c);
    return c;
}

static void destroy(afcore *c)
{
    if (!c) return;
    pthread_mutex_lock(&lock);
    destroying = 1;
    request_stop(STOP_DESTROY);
    pthread_mutex_unlock(&lock);
    pthread_join(c->super, NULL);
    pthread_mutex_lock(&lock);
    the_core = NULL;
    stop_reason = STOP_NONE;
    destroying = 0;
    guest_stopping = 0;
    for (int i = 0; i < 3; i++) frames[i].w = frames[i].h = 0;
    fresh = fresh_next = 0;
    pthread_mutex_unlock(&lock);
    save_root[0] = game_root[0] = 0;
    free(c);
}

static void run_frame(afcore *c, const afcore_input *in)
{
    afcore_input input;
    memset(&input, 0, sizeof input);
    input.gun_x[0] = input.gun_x[1] = input.gun_y[0] = input.gun_y[1] = 0.5f;
    if (in) memcpy(&input, in, in->size < sizeof input ? in->size : sizeof input);
    set_inputs(&input);
    pthread_mutex_lock(&lock);
    if (c->error[0]) {
        pthread_mutex_unlock(&lock);
        return;
    }
    if (clk_render) {
        // what the game showed while time stood at the last frame's end is
        // this frame's; then one more frame of time, and its end-of-frame tick
        if (fresh_next) {
            framebuf t = frames[1];
            frames[1] = frames[2];
            frames[2] = t;
            fresh = 1;
            fresh_next = 0;
        }
        clk_frame();
        go = 1;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&lock);
        kthread_freeze(0);
        kthread_clock_kick();
        clk_wait_frame(&guest_stopping, 5ull * 1000000000ull);
        pthread_mutex_lock(&lock);
        if (fresh) {
            framebuf t = frames[0];
            frames[0] = frames[1];
            frames[1] = t;
            fresh = 0;
        }
        pthread_mutex_unlock(&lock);
        return;
    }
    unsigned seq = frame_seq;
    go = 1;
    pthread_cond_broadcast(&cv);
    // the next frame, or give up for now while the game is loading or
    // restarting; the front end keeps showing the last one
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 100 * 1000 * 1000;
    if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
    while (frame_seq == seq && !c->error[0])
        if (pthread_cond_timedwait(&cv, &lock, &ts) == ETIMEDOUT) break;
    if (fresh) {
        framebuf t = frames[0];
        frames[0] = frames[1];
        frames[1] = t;
        fresh = 0;
    }
    pthread_mutex_unlock(&lock);
}

static const uint32_t *screen(afcore *c, int *w, int *h)
{
    static const uint32_t black[4] = {0xff000000u, 0xff000000u, 0xff000000u, 0xff000000u};
    (void)c;
    pthread_mutex_lock(&lock);
    const uint32_t *p = frames[0].px;
    if (p && frames[0].w) {
        *w = frames[0].w, *h = frames[0].h;
    } else {
        p = black;
        *w = *h = 2;
    }
    pthread_mutex_unlock(&lock);
    return p;
}

static double audio_rate(afcore *c, int stream)
{
    (void)c;
    return stream == 0 ? AUDIO_RATE : 0;
}

static size_t audio_read(afcore *c, int stream, float *out, size_t max)
{
    (void)c;
    if (stream != 0) return 0;
    int16_t buf[1024 * 2];
    size_t done = 0;
    while (done < max) {
        int want = max - done < 1024 ? (int)(max - done) : 1024;
        int n = chihiro_audio_read(buf, want);
        for (int i = 0; i < 2 * n; i++) out[2 * done + i] = buf[i] / 32768.0f;
        done += (size_t)n;
        if (n < want) break;
    }
    return done;
}

// the game writes its settings and records to its partitions (files in
// save_dir) as it goes: nothing is held back
static void save(afcore *c) { (void)c; }

static int link_start(afcore *c, int port, const char *next, int framesync, char *err, size_t err_size)
{
    (void)c, (void)port, (void)next, (void)framesync;
    snprintf(err, err_size, "no link play for this game");
    return -1;
}

static int link_state(afcore *c, int *id, int *count)
{
    (void)c;
    if (id) *id = 0;
    if (count) *count = 0;
    return AFCORE_LINK_OFF;
}

// afcore 5.3: clock=frames for video renders (see the top); the app's game
// page shows only region=, so this one stays out of its menus
static const char options[] = "clock=realtime|frames\n";

static int set_option(const char *name, const char *value)
{
    if (!name || !value || strcmp(name, "clock")) return -1;
    if (!strcmp(value, "frames")) opt_render = 1;
    else if (!strcmp(value, "realtime")) opt_render = 0;
    else return -1;
    return 0;
}

static const char *error(afcore *c)
{
    pthread_mutex_lock(&lock);
    const char *e = c->error[0] ? c->error : NULL;
    pthread_mutex_unlock(&lock);
    return e;
}

#ifdef CORE_FILELIST
#include "afc_filelist.h" // filelists/<set>.h: every file of a known-good copy
// afcore 5.2: the game folder against the list made from a known-good copy
// (tools/filelist in common/): every listed file, its size and CRC.
static int check_roms(const char *rom_path, char *report, size_t report_size)
{
    char root[1024];
    snprintf(root, sizeof root, "%s", rom_path ? rom_path : "");
    size_t len = strlen(root);
    while (len > 1 && root[len - 1] == '/') root[--len] = 0;
    return afc_check_files(root, afc_filelist, AFC_FILELIST_COUNT, report, report_size);
}
#define CHECK_ROMS check_roms
#else
#define CHECK_ROMS NULL // no list for this game yet
#endif

const afcore_api *API_FN(CORE_ROMSET)(void)
{
    static const afcore_api api = {
        AFCORE_API_VERSION, sizeof(afcore_api), AFCORE_MAKE_VERSION(CORE_VERSION_MAJOR, CORE_VERSION_MINOR, CORE_VERSION_PATCH), STR(CORE_ROMSET), CORE_TITLE,
        60.0, CORE_WIDTH, CORE_HEIGHT, create, destroy, run_frame, screen, audio_rate, audio_read, save,
        link_start, link_state, error, NULL, NULL, NULL, 0.0f, NULL, CHECK_ROMS, options, set_option,
    };
    return &api;
}
