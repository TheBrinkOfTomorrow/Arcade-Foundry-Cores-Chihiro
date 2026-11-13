set(GAME_TITLE "Crazy Taxi High Roller")
set(GAME_GEN_COUNT 32)
set(GAME_WIDTH 640)
set(GAME_HEIGHT 480)
# a wheel (analog 0), accelerator (1), brake (2); the shifter's Drive and Reverse are up and down
set(GAME_DRIVE 1)
# test mode is a program of its own, which the cabinet's system menu starts
set(GAME_TEST_XBE "ctx_ac_t.xbe")
# one guest thread at a time, as on the Xbox: CRI's threads race otherwise
# (the boot logo's stream sometimes errored and never ended); see HotD3
set(GAME_ONE_CORE 1)
