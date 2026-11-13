# Arcade Foundry: Insert Coin — Sega Chihiro cores

Static recompilation of Sega Chihiro games for Arcade Foundry: Insert Coin.
The Chihiro is an Xbox (Pentium III, NV2A, MCPX) with Sega's media board and
baseboard. Each game's XBE is translated from x86 to C at build time; the C
runs on a host reimplementation of the Xbox kernel, the NV2A and the Sega
boards.

Games in progress: Virtua Cop 3 (`vc3`), The House of the Dead III (`hotd3`).

## Building (Linux, for development)

You need your own dump of the game: its XBE and the files beside it, and
SDL2 and OpenGL 3.3 (`apt install libsdl2-dev libgl-dev`).

    git submodule update --init
    common/tools/afic-venv/setup.sh      # the shared Python venv, ~/venvs/afic (once)
    cmake -S . -B build/vc3 -DGAME=vc3 -DGAME_XBE=/path/to/vc3/vc3.xbe
    cmake --build build/vc3 -j
    build/vc3/chihiro-run /path/to/vc3/vc3.xbe save

The translator needs `capstone` and `pyelftools` from the one shared venv
every Arcade Foundry repo uses (`~/venvs/afic`, or `AFIC_VENV`; never the
system's Python). CMake picks it up on its own in a new build folder;
`-DPython3_EXECUTABLE=...` overrides it. An old `venv/` in this checkout can
be deleted.

The folder holding the XBE is the game's D: drive. Saves and the hard disk
partitions go in the folder given second (`save` by default).

Environment: `AF_DEBUG_CH_VERBOSE=1` (or 2, 3) logs more, `AF_DEBUG_CH_SHOT=dir` saves every
`AF_DEBUG_CH_SHOT_EVERY`th frame (default 60) as a PPM, `AF_DEBUG_CH_PBTRACE=file` logs every
GPU method, `AF_DEBUG_CH_NOGPU=1` runs without drawing.

Nothing derived from a game is committed: see `rules.md`.
