// The developer front end (Linux, and macOS with Metal): runs one translated
// game from the folder holding its XBE, which is the game's D: drive.
// usage: chihiro-run path/to/game.xbe [save folder]
#include "af_debug.h"
#include "xbox.h"
#include <libgen.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <SDL2/SDL.h>
#include <SDL2/SDL_metal.h>
#include <mach-o/dyld.h>
void gpu_metal_set_layer(void *metal_layer);   // gpu_metal.m
uint32_t gpu_frame(void);
void input_frame(uint32_t frame, int win_w, int win_h);   // input_sdl.c
#endif

extern const gen_program CH_PROGRAM;   // gen_program_game (CMake)
#ifdef CH_TEST_XBE
extern const gen_program gen_program_test;   // the game's test program (CMake GAME_TEST_XBE)
#endif

static char **saved_argv;

// The test button (or leaving the test menu): run this program again, into
// the game's test menu or not. A CH_INPUT script is for the first run;
// CH_INPUT_NEXT, if set, becomes the next run's.
static void relaunch(int into_test)
{
    hle_log("restarting the game%s\n", into_test ? " in its test menu" : "");
    // Set under the AF_DEBUG_ names, and clear the old ones, so the next run
    // reads these whichever name the first run was given.
    if (into_test) setenv("AF_DEBUG_CH_TESTMODE", "1", 1);
    else unsetenv("AF_DEBUG_CH_TESTMODE");
    unsetenv("CH_TESTMODE");
    const char *next = af_debug_env("CH_INPUT_NEXT");
    if (next) setenv("AF_DEBUG_CH_INPUT", next, 1);
    else unsetenv("AF_DEBUG_CH_INPUT");
    unsetenv("CH_INPUT");
    unsetenv("AF_DEBUG_CH_INPUT_NEXT");
    unsetenv("CH_INPUT_NEXT");
    fflush(NULL);
#ifdef __APPLE__
    char self[PATH_MAX];
    uint32_t n = sizeof self;
    if (_NSGetExecutablePath(self, &n) == 0) execv(self, saved_argv);
#else
    execv("/proc/self/exe", saved_argv);
#endif
    perror("execv");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s path/to/game.xbe [save folder]\n", argv[0]);
        return 1;
    }
    saved_argv = argv;
    char xbe_path[PATH_MAX], dir[PATH_MAX];
    if (!realpath(argv[1], xbe_path)) {
        perror(argv[1]);
        return 1;
    }
    snprintf(dir, sizeof dir, "%s", xbe_path);
    snprintf(game_root, sizeof game_root, "%s", dirname(dir));
    snprintf(save_root, sizeof save_root, "%s", argc > 2 ? argv[2] : "save");
    if (af_debug_env("CH_VERBOSE")) hle_verbose = atoi(af_debug_env("CH_VERBOSE"));
    hle_trace = af_debug_env("CH_TRACE") != NULL;
    launch_test_mode = af_debug_env("CH_TESTMODE") != NULL;   // start in the game's test menu
    guest_one_core = CH_ONE_CORE && !af_debug_env("CH_ALLCORES");
    front_end_reboot = relaunch;
#ifdef __APPLE__
    // Cocoa wants windows and their events on the main thread: the window
    // is made here and its layer handed to the renderer (on the GPU thread)
    SDL_Window *win = NULL;
    if (!af_debug_env("CH_NOGPU") && SDL_Init(SDL_INIT_VIDEO) == 0) {
        win = SDL_CreateWindow("Chihiro", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480,
                               SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        SDL_MetalView view = win ? SDL_Metal_CreateView(win) : NULL;
        if (view) gpu_metal_set_layer(SDL_Metal_GetLayer(view));
        else hle_log("no Metal window (%s): nothing will be shown\n", SDL_GetError());
    }
#endif
    const gen_program *run = &CH_PROGRAM;
#ifdef CH_TEST_XBE
    // a test program of its own: CH_TESTMODE (the test button) runs it, from beside the XBE
    char test_path[PATH_MAX + 64];
    snprintf(test_path, sizeof test_path, "%s/%s", game_root, CH_TEST_XBE);
    if (access(test_path, R_OK) == 0) {
        test_program = &gen_program_test;
        if (launch_test_mode) run = test_program, snprintf(xbe_path, sizeof xbe_path, "%s", test_path);
    }
#endif
    run_game(run, xbe_path);
#ifdef __APPLE__
    // the window's events and the controls, once each frame the renderer shows
    for (uint32_t last = 0;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_QUIT) exit(0);
        uint32_t f = gpu_frame();
        if (win && f != last) {
            int w = 0, h = 0;
            SDL_GetWindowSize(win, &w, &h);
            input_frame(f, w, h);
            last = f;
        }
        SDL_Delay(2);
    }
#endif
    // the entry point returning ends the main thread; the game's own threads carry on
    for (;;) pause();
}
