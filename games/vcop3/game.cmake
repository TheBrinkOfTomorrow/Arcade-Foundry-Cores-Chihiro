set(GAME_GEN_COUNT 32)
set(GAME_WIDTH 640)
set(GAME_HEIGHT 480)
# VC3 threads count on one processor too: side by side, one freed an object
# another was still calling through (Linux, "no function at 0" from 0xd40e2)
set(GAME_ONE_CORE 1)
