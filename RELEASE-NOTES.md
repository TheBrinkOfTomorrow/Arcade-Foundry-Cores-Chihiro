# Release notes

Newest first. Each release is also on the repository's Releases page.

## 1.3.0 (2026-11-12)

**Arcade Foundry Cores: Chihiro 1.3.0**

First public release of the Sega Chihiro cores for Arcade Foundry: Insert
Coin, shipped with Insert Coin 1.3.0.

- **Games:** Virtua Cop 3 (vcop3), The House of the Dead III (hotd3) and
  Crazy Taxi High Roller (crtaxihr).
- **How it works:** each game's XBE is translated to C ahead of time by the
  shared x86 recompiler (in Arcade-Foundry-Cores-Common) and runs on our own
  Xbox kernel, with the graphics (NV2A), sound (MCPX APU), USB, JVS and the
  media board handled at the hardware registers. One core per game, behind
  the same C interface as the other cores.
- **Platforms:** macOS and iPad (Metal) and Linux (OpenGL). iPhone is untested.
- **Crazy Taxi High Roller:** no more hang at the ADX logo on about one boot
  in four (the guest runs on one processor, and a contended critical section
  yields it), and the city is no longer drawn under the water (the
  recompiler's function starts). Use a dump with intact movies (the Rev B
  image); some older dumps have them blanked, which leaves attract black for
  about 70 s. Core 1.1.0.
- **ROM check:** `check_roms` compares a game folder with the list of a
  known-good copy (every file's size and CRC), without starting the game,
  so the app's Downloads can say whether a game is complete. Core versions:
  Virtua Cop 3 and HotD3 1.1.0, Crazy Taxi 1.3.0.
- **Crazy Taxi High Roller's Jump:** A or B on a controller (keys 1 and 2)
  presses the cabinet's JUMP button (core 1.2.0).
- **Debug switches:** the developer environment variables are `AF_DEBUG_`
  plus their old name (`AF_DEBUG_CH_VERBOSE`); the old names still work in
  this release. The list is in Arcade-Foundry-Cores-Common's
  docs/DEBUG-SWITCHES.md.

No games, BIOS or Xbox kernel are included. You supply your own game folders
and MAME's chihiro.zip. **Licence: BSD-3-Clause.**
