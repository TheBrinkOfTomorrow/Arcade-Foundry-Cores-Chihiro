// The Linux runner's controls, into jvs_inputs (the Apple app sets them itself).
//
//   5, 6         coin 1, coin 2
//   1, 2         start 1, start 2
//   F2           test
//   9            service 1
//   mouse        player 1's gun: aim on analog channels 0 (x) and 1 (y),
//                left button the trigger (0x0200), right button fires off
//                screen (the screen-out switch 0x0100, aim at the corner),
//                middle button or C the gun's change button (0x0080)
//   Space        player 1's pedal (0x0040)
//   arrows, Z X  player 1's stick and buttons 1-2
// Player 2's gun is analog channels 2 and 3 with the same switches, as
// VC3's INPUT TEST shows them. A driving game (CH_DRIVE, Crazy Taxi High
// Roller) takes Left/Right as the wheel (analog 0), Up the accelerator (1),
// Down the brake (2), Q and A the shifter's Drive (up, 0x2000) and Reverse
// (down, 0x1000), and Z or X (buttons 1 and 2) jump.
//
// CH_INPUT scripts the same for runs without a person, one step per
// comma: <frame>:<action>[+<action>...], actions coin1 coin2 start1 start2
// test service trig1 trig2 off1 off2 change1 change2 pedal1 pedal2, each held for 6 frames (or N with
// a *N suffix, as trig1*30), and
// aim1=<x>/<y> aim2=<x>/<y> (0-65535) which stay, and bits1=<mask>
// bits2=<mask> holding raw switch bits of a player (as the trigger's 0x200), and
// an<N>=<v> setting analog channel N (0-7, 0-65535), which stays. Frames are the renderer's
// shown frames. Example: CH_INPUT=600:coin1,660:start1,900:aim1=32768/32768+trig1
#include "af_debug.h"
#include "xbox.h"
#include "baseboard.h"
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>

#define HOLD 6
#ifndef CH_DRIVE
#define CH_DRIVE 0
#endif

enum { A_COIN1, A_COIN2, A_START1, A_START2, A_TEST, A_SERVICE, A_TRIG1, A_TRIG2, A_OFF1, A_OFF2,
       A_CHANGE1, A_CHANGE2, A_PEDAL1, A_PEDAL2, A_COUNT };
static const char *const names[A_COUNT] = { "coin1", "coin2", "start1", "start2", "test", "service",
                                            "trig1", "trig2", "off1", "off2", "change1", "change2",
                                            "pedal1", "pedal2" };

typedef struct step {
    uint32_t frame;
    int action;          // or -1 for aim
    int player;
    uint32_t hold;
    uint16_t x, y;
} step;
static step *script;
static int nsteps, next_step, parsed;
static uint32_t held_until[A_COUNT];
static uint16_t raw[2];
static uint32_t raw_until[2];
static uint16_t aim[2][2] = { { 0x8000, 0x8000 }, { 0x8000, 0x8000 } };
static int an_set[8];          // analog channels a script set (anN=V), which stay
static uint16_t an_val[8];

static void parse(void)
{
    parsed = 1;
    const char *s = af_debug_env("CH_INPUT");
    if (!s) return;
    char *copy = strdup(s), *save1 = NULL;
    for (char *item = strtok_r(copy, ",", &save1); item; item = strtok_r(NULL, ",", &save1)) {
        char *colon = strchr(item, ':');
        if (!colon) continue;
        uint32_t frame = (uint32_t)strtoul(item, NULL, 0);
        char *save2 = NULL;
        for (char *a = strtok_r(colon + 1, "+", &save2); a; a = strtok_r(NULL, "+", &save2)) {
            step st = { .frame = frame, .action = -2, .hold = HOLD };
            char *star = strchr(a, '*');
            if (star) *star = 0, st.hold = (uint32_t)strtoul(star + 1, NULL, 0);
            if (!strncmp(a, "bits1=", 6) || !strncmp(a, "bits2=", 6)) {
                st.action = -3, st.player = a[4] - '1';
                st.x = (uint16_t)strtoul(a + 6, NULL, 0);
            } else if (a[0] == 'a' && a[1] == 'n' && a[2] >= '0' && a[2] <= '7' && a[3] == '=') {
                st.action = -4, st.player = a[2] - '0';
                st.x = (uint16_t)strtoul(a + 4, NULL, 0);
            } else if (!strncmp(a, "aim1=", 5) || !strncmp(a, "aim2=", 5)) {
                st.action = -1, st.player = a[3] - '1';
                st.x = (uint16_t)strtoul(a + 5, NULL, 0);
                const char *slash = strchr(a, '/');
                st.y = slash ? (uint16_t)strtoul(slash + 1, NULL, 0) : 0x8000;
            } else
                for (int i = 0; i < A_COUNT; i++)
                    if (!strcmp(a, names[i])) st.action = i;
            if (st.action == -2) {
                hle_log("CH_INPUT: unknown action %s\n", a);
                continue;
            }
            script = realloc(script, (nsteps + 1) * sizeof *script);
            script[nsteps++] = st;
        }
    }
    free(copy);
}

void input_frame(uint32_t frame, int win_w, int win_h)
{
    if (!parsed) parse();
    while (next_step < nsteps && script[next_step].frame <= frame) {
        step *st = &script[next_step++];
        if (st->action == -1) aim[st->player][0] = st->x, aim[st->player][1] = st->y;
        else if (st->action == -3) raw[st->player] = st->x, raw_until[st->player] = frame + st->hold;
        else if (st->action == -4) an_set[st->player] = 1, an_val[st->player] = st->x;
        else held_until[st->action] = frame + st->hold;
        if (hle_verbose > 0) hle_log("input: frame %u %s\n", frame, st->action >= 0 ? names[st->action] : st->action == -1 ? "aim" : st->action == -4 ? "analog" : "bits");
    }
    int held[A_COUNT];
    for (int i = 0; i < A_COUNT; i++) held[i] = frame < held_until[i];

    const uint8_t *k = SDL_GetKeyboardState(NULL);
    int mx = 0, my = 0;
    uint32_t mb = k ? SDL_GetMouseState(&mx, &my) : 0;
    // the mouse takes over the gun once it has moved inside the window
    static int lastx = -1, lasty = -1, mouse_used;
    if (k && SDL_GetMouseFocus() && (lastx >= 0 && (mx != lastx || my != lasty))) mouse_used = 1;
    lastx = mx, lasty = my;
    int mouse_in = mouse_used && SDL_GetMouseFocus() != NULL;
    if (k) {
        held[A_COIN1] |= k[SDL_SCANCODE_5];
        held[A_COIN2] |= k[SDL_SCANCODE_6];
        held[A_START1] |= k[SDL_SCANCODE_1];
        held[A_START2] |= k[SDL_SCANCODE_2];
        held[A_TEST] |= k[SDL_SCANCODE_F2];
        held[A_SERVICE] |= k[SDL_SCANCODE_9];
        held[A_TRIG1] |= mouse_in && (mb & SDL_BUTTON(SDL_BUTTON_LEFT));
        held[A_OFF1] |= mouse_in && (mb & SDL_BUTTON(SDL_BUTTON_RIGHT));
        held[A_CHANGE1] |= k[SDL_SCANCODE_C] || (mouse_in && (mb & SDL_BUTTON(SDL_BUTTON_MIDDLE)));
        held[A_PEDAL1] |= k[SDL_SCANCODE_SPACE];
        if (mouse_in && win_w > 0 && win_h > 0) {
            aim[0][0] = (uint16_t)(mx * 65535 / win_w);
            aim[0][1] = (uint16_t)(my * 65535 / win_h);
        }
    }

    // coins count on the press
    static int was_coin[2];
    for (int c = 0; c < 2; c++) {
        int now = held[A_COIN1 + c];
        if (now && !was_coin[c]) jvs_inputs.coins[c]++;
        was_coin[c] = now;
    }
    jvs_inputs.system = held[A_TEST] ? 0x80 : 0;
    for (int p = 0; p < 2; p++) {
        uint16_t v = 0;
        if (held[A_START1 + p]) v |= 0x8000;
        if (p == 0 && held[A_SERVICE]) v |= 0x4000;
        if (held[A_TRIG1 + p] || held[A_OFF1 + p]) v |= 0x0200;
        if (held[A_OFF1 + p]) v |= 0x0100;
        if (held[A_CHANGE1 + p]) v |= 0x0080;
        if (held[A_PEDAL1 + p]) v |= 0x0040;
        if (frame < raw_until[p]) v |= raw[p];
        if (p == 0 && k) {
            if (k[SDL_SCANCODE_UP]) v |= 0x2000;
            if (k[SDL_SCANCODE_DOWN]) v |= 0x1000;
            if (k[SDL_SCANCODE_LEFT]) v |= 0x0800;
            if (k[SDL_SCANCODE_RIGHT]) v |= 0x0400;
            if (k[SDL_SCANCODE_Z]) v |= 0x0200;
            if (k[SDL_SCANCODE_X]) v |= 0x0100;
        }
        jvs_inputs.player[p] = v;
        int off = held[A_OFF1 + p];
        jvs_inputs.analog[2 * p] = off ? 0 : aim[p][0];
        jvs_inputs.analog[2 * p + 1] = off ? 0 : aim[p][1];
    }
    if (CH_DRIVE) {
        // a driving game (CH_DRIVE): Left/Right steer, Up accelerates, Down
        // brakes, Q and A shift to Drive and Reverse; the gun bits are not used
        uint16_t v = jvs_inputs.player[0] & 0xc000;
        if (frame < raw_until[0]) v |= raw[0];
        if (k && k[SDL_SCANCODE_Q]) v |= 0x2000;
        if (k && k[SDL_SCANCODE_A]) v |= 0x1000;
        if (k && k[SDL_SCANCODE_Z]) v |= 0x0200;   // JUMP (button 1; button 2 is JUMP too)
        if (k && k[SDL_SCANCODE_X]) v |= 0x0100;
        jvs_inputs.player[0] = v;
        jvs_inputs.player[1] = 0;
        int l = k && k[SDL_SCANCODE_LEFT], r = k && k[SDL_SCANCODE_RIGHT];
        jvs_inputs.analog[0] = l == r ? 0x8000 : l ? 0x0000 : 0xffff;
        jvs_inputs.analog[1] = k && k[SDL_SCANCODE_UP] ? 0xffff : 0;
        jvs_inputs.analog[2] = k && k[SDL_SCANCODE_DOWN] ? 0xffff : 0;
    }
    for (int i = 0; i < 8; i++)
        if (an_set[i]) jvs_inputs.analog[i] = an_val[i];
}
