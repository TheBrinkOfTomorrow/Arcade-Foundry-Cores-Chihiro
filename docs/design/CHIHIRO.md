# Sega Chihiro games

How the Chihiro cores were planned and built: Virtua Cop 3 (`vcop3`), The
House of the Dead III (`hotd3`) and Crazy Taxi High Roller (`crtaxihr`),
for Arcade Foundry: Insert Coin 1.3.0. The dated history is in
[HANDOFF.md](../../HANDOFF.md) and the current state in
[STATE.md](../../STATE.md).

## Purpose and scope

Play Sega's Chihiro arcade games natively in the app, on the same terms as
the other boards: the user supplies their own game files, the build turns
them into a core, and the app plays it with a mouse, a light gun setup, a
gamepad or a wheel.

The Chihiro is an Xbox: a Pentium III, the NV2A GPU and the MCPX audio and
I/O chip, with Sega's baseboard (JVS inputs, EEPROMs) and media board added.
A game is an Xbox program (an XBE) plus a folder of data files, not a ROM
set.

In scope:

- the three games above, on macOS (Metal) and Linux (OpenGL 3.3)
- attract, play, test menu, sound, saves and the operator settings
- the app's library, Add ROM, controls and restart

Planned: iPhone, iPad and Apple TV; more games (OutRun 2 and Special Tours,
Ghost Squad, the Maximum Tune games, Ollie King, Sega Club Golf 2006).

## Requirements

1. Nothing from a game, the Xbox or the Chihiro is in the repository: no XBE,
   no generated C, no BIOS, kernel, EEPROM or firmware (`rules.md`). The game
   is translated on the user's machine at build time.
2. The game runs as on the cabinet: 640x480 at 60 frames a second, its own
   timing, its own test menu and operator settings.
3. One core per game behind the shared `afcore.h` interface, so the app
   handles a Chihiro game like any other: create, run a frame, hand over the
   picture and sound, take the inputs, stop and restart in-process.
4. The app's existing controls work unchanged: the gun games take the gun
   inputs (aim, trigger, reload, gun button), the driving game takes steer,
   accelerator, brake and shift up/down.
5. The user's dumps are used as they come (a `.7z` of the game folder and
   MAME's `chihiro.zip`), with one packing step at most.

## How it was planned

The plan came from a feasibility study of the board and its dumps. It
compared three ways in:

- running the XBE under an existing Xbox emulator;
- a high-level emulation of the Xbox SDK libraries;
- the static recompilation already proven on the Lindbergh cores.

Lindbergh is also x86, so its recompiler (now `x86recomp` in
Arcade-Foundry-Cores-Common) was the shortest path. The chosen design takes
from all three:

- **Recompile the whole XBE ahead of time.** That includes the SDK linked
  into it (Direct3D, DirectSound, XAPI, the USB stack and CRI's
  Sofdec/ADX middleware). The SDK then runs as the game's own code and
  needs no high-level emulation.
- **Rewrite the kernel, not the SDK.** A game calls about 110 kernel
  exports, a small and well documented surface. VC3 calls 110, HotD3 108,
  and Crazy Taxi 10 more.
- **Catch the hardware at its registers.** The SDK programs the NV2A, the
  APU, the USB controller and the boards directly. Trapping those
  registers keeps the SDK's own logic (its pushbuffer, its sound mixing
  setup, its USB driver) intact.
- **Use oracles.** xemu and MAME are the behavioural oracles, and are read
  for register layouts and method numbers only (GPL code is never copied).

The order was chosen so each step could be checked on its own:

1. VC3 boots and passes JVS.
2. Fixed-function 3D.
3. Sound and movies.
4. The media board.
5. Controls and test mode, then a full scripted game.
6. HotD3.
7. Metal.
8. The `afcore` library and the app.
9. Crazy Taxi High Roller.
10. Speed.

## Design

### Overview

```
game folder (XBE + data)            chihiro.zip (EEPROMs)
  │                                    │
  ├─ build time: x86recomp (xbe.py) ── C for every function in the XBE
  │                                    │
  └─ run time: afcore_<set> ───────────┘
        generated C  ⇄  runtime: Xbox kernel, threads, files
                         NV2A → Metal or OpenGL
                         MCPX APU → audio ring
                         baseboard / JVS ← afcore inputs
                         media board
```

### Translating the XBE

`common/tools/x86recomp` translates the XBE's x86 into C ahead of time
(`recomp.py`, with `xbe.py` reading the XBE). The XBE has no symbols, so
`xbe.py` finds the functions itself by recursive descent from the entry
point. It follows call targets, jump tables and code addresses found in data,
and rejects guesses that decode across known code. Every function becomes C;
indirect calls and callbacks go through a lookup table at run time
(`guest_dispatch`). Kernel imports are thunks into the runtime.

Discovery was the source of several bugs, each fixed in `xbe.py` with a rule
and checked against all three games:

- Two overlapping guessed starts with equal scores were both kept. A guess
  inside an instruction split a real loop (Crazy Taxi's model loader) and
  the misdecode cleared its counter. On a tie, the start inside the other's
  instruction is dropped.
- A function that starts right after a switch table was missed when nothing
  else pointed at that table (VC3: a method at 0x57d70 whose own table was
  in a function only it referenced). A start right after code-address words
  now counts.
- Instructions with no handler (`xlatb`) were traps; they are translated.
- A constant the code pushed or stored (`push 0x30002`, a D3D method word)
  that happened to be the address of an instruction inside reached code
  became a function start. The function was split there, and the flags a
  `test` set were lost across the split: Crazy Taxi's water check
  (0x2ff30) then took every point as inside a pool, so the game was drawn
  under water. Such a constant is now a start only if it looks like an
  entry (16-byte aligned, after a `ret`/`jmp`/`call`/padding, or nothing
  reached ends there): 41 false starts gone in Crazy Taxi, 80 in VC3 and
  54 in HotD3.

Per-game fixes that cannot be general go in `games/<set>/`: `overrides.txt`
(a hand-written C function in place of a translated one) and `patches.txt`
(bytes changed by address before translation). `game.cmake` holds the
game's build switches.

### The Xbox kernel

The runtime reimplements the parts of the Xbox kernel the games import
(`kernel.c`, `kthread.c`, `kfile.c`), from public behaviour only:

- **Threads:** each guest thread is a host thread with its own guest stack.
  Waits, events, semaphores, timers, DPCs and APCs are host primitives.
- **Files:** the game folder is the D: drive (the media board's disc), and
  the hard disk partitions (T:, U:, Z:) are folders in the save directory.
- **Memory:** guest memory is one 4 GB address window; the 128 MB of RAM and
  the GPU's instance memory sit at their Xbox addresses.
- **Memory map:** the RAM is shared memory mapped at 0, 0x80000000 and
  0xF0000000, so the CPU's views and the GPU's physical addresses agree.
  Kernel data is at 0xC0000000 and thread stacks at 0xD0000000. Devices
  reach memory through `dma_addr()`, because the USB driver hands the
  controller buffers on thread stacks.
- **Waits:** one mutex and condition variable carry every kernel wait. A
  per-object set count makes an event that is set and reset at once (the
  vertical blank) still wake its waiters.
- **Interrupts:** an interrupt thread raises the vertical blank (60 Hz),
  timers and device interrupts, and runs the games' interrupt routines and
  DPCs. Its 1 ms sleep reads the clock once: reading it a second time, after
  the tick had passed, made the time left wrap to centuries on Apple
  platforms. The thread then slept until something else woke it, the
  vertical blank stopped, and about 1 boot in 50 froze (Crazy Taxi showed it
  most).

**One processor.** The Chihiro has one CPU, and its games count on that:
CRI's movie and file threads in HotD3, and VC3's object lifetimes. With
guest threads running side by side, HotD3's movies froze and VC3 called
methods on objects another thread had just freed. `GAME_ONE_CORE` runs one
guest thread at a time: the translated code yields at loop back edges
(`GUEST_LOOP`) and at every wait. macOS cannot pin threads to one core, so
this lock replaced pinning. All three games use it.

Under one processor a critical section can starve its waiter: Crazy Taxi's
main thread leaves and re-enters the same section in a loop, so the thread
waiting on it never runs while it is free, and the Sofdec logo at boot
stuck in 1–3% of boots. Leaving a section that has waiters now
yields the processor for 200 µs (`kernel.c`), and the hang went from 1–3%
to 0 in 171 boots (Mac mini M4 Pro, 2026-11-06). Handing the section
straight to the waiter was tried first and failed: a newcomer turned away
cleared the section's event and used up the waiter's wake.

CRI's own lock is mapped to the runtime's priority lock in HotD3 and Crazy
Taxi (`games/<set>/patches.txt`, ports 0xe8/0xe9). Crazy Taxi's CRI also
has a release-all/retake pair: `in eax, 0xea` releases all of a thread's
holds and returns the count, `out 0xeb, eax` takes the lock that many times
again (IN for the return value, as an OUT's registers are not read back).

### The NV2A and the renderers

The GPU is caught at its registers (`nv2a.c`): the game writes its
pushbuffer, and a GPU thread runs the methods (`pgraph.c`). Vertex programs
and the fixed-function pipeline are translated to shaders (`vsh.c`,
`vsh_ff.c`), and register combiners to fragment shaders (`psh.c`).
Textures are decoded from the NV2A's formats and swizzling (`tex.c`).
Direct3D also writes `NOP <arg>` into the pushbuffer to be told when the GPU
gets there. The GPU raises PGRAPH's error interrupt with the trapped address
and data, then stalls until the game's interrupt routine clears it and turns
PGRAPH_FIFO back on.

Two renderers share that front end:

- **Metal** (`gpu_metal.m`) on Apple platforms. The shaders are written as
  GLSL bodies, and a preamble of Metal definitions compiles them unchanged.
- **OpenGL 3.3** (`gpu_gl.c`) on Linux, with EGL and no surface for the core
  library.

Each surface the game draws to is a texture found by its address. The
picture the game shows (its flip) is what the front end gets. Two
corrections came from comparing the games' output with the cabinet:

- **Cull winding:** front and back faces were swapped in both renderers, so
  HotD3's level geometry vanished and its red clear colour showed through.
- **Y orientation:** Metal's Y axis is the reverse of GL's, so the vertex
  epilogue maps Y the other way and the winding with it.

### Sound, inputs and the boards

- **Sound:** the MCPX APU's voice processor is mixed on the host (`apu.c`)
  into a ring the core hands to the app (48 kHz stereo). DSP effects such as
  reverb are not done.
- **Inputs:** the baseboard's JVS I/O (`baseboard.c`) takes the inputs the
  core sets from `afcore_input`:
  - **Guns (VC3, HotD3):** the aim on analog channels 0 and 1, the trigger,
    the screen-out switch for a shot off screen, the gun's button, and start
    and service. The game's own INPUT TEST was the reference for each bit.
  - **HotD3 reload (`GAME_PUMP`):** HotD3 reloads only by pumping the
    shotgun's grip. The grip switch must be pulled twice; a shot off screen
    does nothing. The app's Reload (the gun off screen with the trigger)
    becomes two grip pulls: 8 frames on, 12 off, 8 on.
  - **TEST and SERVICE:** the JVS switches carry them for the games, but a
    test program (Crazy Taxi's `ctx_ac_t.xbe`) reads them from the
    AN2131QC's port byte (PINSA, bits 6 and 7, active low). PINSA carries
    the buttons only while a game's own test program runs; the games read
    it too and would see every press twice.
  - **Driving (`GAME_DRIVE`, Crazy Taxi):** the wheel on analog 0, the
    accelerator on 1 and the brake on 2. Drive and Reverse on the shifter
    are player 1's up and down, which are afcore's `SHIFT_UP` and
    `SHIFT_DOWN`. They were found by pulsing each switch in a game and
    watching the on-screen D/R lamp. JUMP is player 1's button 1 (0x0200)
    or button 2 (0x0100); both light it in the INPUT TEST. The core sends
    them from afcore's `VIEW1` and `VIEW2` (A and B on a controller).
- **EEPROMs and backup memory:** the baseboard has three EEPROMs: ic10 and
  ic11 behind the AN2131QC, which the game reads and writes, and pc20
  behind the AN2131SC, which it only reads. The EEPROMs' starting contents,
  including the board ID the test menu shows as MAIN BD ID, come from
  MAME's `chihiro.zip`, read in place with Common's zip reader. The core
  looks for it at `AF_DEBUG_CH_BIOS`, beside the game folder or one folder
  up. Whatever the game writes to ic10 and ic11 is saved as
  `baseboard-eeprom.bin` in the save directory and loaded over the zip's
  contents at every start. Some operator settings (Crazy Taxi's START TIME)
  live in the AN2131QC's backup memory instead, kept as
  `baseboard-memory.bin`. So test-menu changes survive a restart with or
  without the zip. Without the zip the EEPROMs start blank: the game fills
  in its factory settings on the first start, and there is no board ID.
- **Media board:** the media board is the D: drive and a small device for
  its ID and data area.

### The core library

`-DCH_CORE=ON` builds `afcore_<set>` (a framework on Apple platforms, a
shared library on Linux), following the Lindbergh core:

- **Threads:** a supervisor thread starts the game on its own threads. The
  GPU thread hands over each shown frame in lockstep with `run_frame`.
- **Stop and restart:** `guest_stop` ends every guest thread at its next
  wait, kernel call or loop back edge, and `guest_reset` puts each module
  back to its power-on state. Stop and restart happen without leaving the
  process.
- **Test button:** the game reboots through `HalReturnToFirmware`, and the
  core restarts it into its test menu, or back out of it, as the cabinet's
  system menu does.
- **Build-time constants:** the core's XBE name and size are fixed at build
  time, so a core only runs the dump it was translated from.
- **ROM check:** `check_roms` (afcore 5.2) compares the game folder with
  `filelists/<set>.h`, every file's path, size and CRC-32 from a known-good
  copy, made by Common's `tools/filelist/make_filelist.py` (as Lindbergh).
  Core versions 1.1.0 (VC3, HotD3) and 1.3.0 (Crazy Taxi) add it.

### Speed

VC3 and HotD3 hold 60 fps on the Mac, and so does Crazy Taxi in play
(median 60, a 2-minute drive on the Mac mini M4 Pro, 2026-11-06), with
dips only at loads.

First builds ran at about 33 fps (VC3) and 40–50 fps (HotD3). Profiling
showed the threads mostly waiting, not working. The fixes were in the
hand-offs between threads:

- a vertical-blank wake-up that could be lost
- interrupts raised at once instead of on the next 1 ms tick
- the GPU thread waiting on a condition instead of polling
- critical sections waking a thread only when one waits
- an interrupt thread that no longer spins on the clock

Result: about 60 fps on both, and HotD3's CPU use fell from about 130% to
about 30%. `AF_DEBUG_CH_PERF=1` prints, once a second:

- frame rate and frames per vblank
- GPU time per frame
- the top waits with their objects and guest callers
- kernel calls, wakes and sleeps

### Game files in the app

A Chihiro game is a folder named after its set, holding the XBE:

- `vcop3/vc3.xbe`
- `hotd3/hod3xb.xbe`
- `crtaxihr/ctx_ac[r].xbe`

The app moves it as one `<set>.tar`, like a Lindbergh game.

**Which dump.** Two Crazy Taxi dumps (a `.7z` file set and a TeknoParrot
zip, identical to each other) have the attract's movies blanked:
`Media/ADX/adx0.afs` entries 23–33 are one 16 KB chunk repeated and entry 22
is partly so. Sofdec never gets past preparing them, so the attract shows
about 17 s of black with the credit text between demo replays. The
decrypted GD-ROM image (GDX-0002B, a FATX file system) has the same XBEs and
the real movies, and they play. The HotD3 GD-ROM image (GDX-0001) matches
the file set exactly; the VC3 one (GDX-0003B) is a later revision with a
different `vc3.xbe`, so the cores keep the file-set VC3. Reading the GD-ROM
images directly is the app's `scripts/fatx.py` (their FAT is sized for a
512 MB volume, so the data starts at 0x11000, not where the image size
says).
`scripts/pack_games.py` makes that `.tar` from the dump's `.7z` (it finds
the XBE inside), and `build_cores.py` builds the cores from it. The app's
Add ROM, iCloud Drive and the ROM server take the `.tar`, and `chihiro.zip`
goes beside it.

## Data and settings

- **Game data:** the game folder, read-only (D:).
- **Saves:** the hard disk partitions as folders (partition 1 holds TDATA
  and UDATA; partition 5 is the cache drive Z:), the EEPROMs the game writes
  (`baseboard-eeprom.bin`) and the baseboard's backup memory
  (`baseboard-memory.bin`), all in the core's save directory.
- **Settings:** the operator settings are the game's own (test menu,
  EEPROM). The core has no options of its own yet.
- **Build switches** (`games/<set>/game.cmake`):

  | Switch | Effect | Used by |
  |---|---|---|
  | `GAME_ONE_CORE` | One guest thread runs at a time | vcop3, hotd3, crtaxihr |
  | `GAME_PUMP` | The app's Reload pumps the shotgun's grip | hotd3 |
  | `GAME_DRIVE` | Wheel, accelerator, brake and shifter instead of guns | crtaxihr |
  | `GAME_TITLE` | The title, when it is not in `games.toml` | crtaxihr |

- **Run-time variables** (development): `AF_DEBUG_CH_BIOS`, `AF_DEBUG_CH_PERF`, `AF_DEBUG_CH_SHOT`,
  `AF_DEBUG_CH_INPUT` (scripted inputs), `AF_DEBUG_CH_TESTMODE`, `AF_DEBUG_CH_ALLCORES`, `AF_DEBUG_CH_PBTRACE`,
  `AF_DEBUG_CH_WAV`, `AF_DEBUG_CH_VERBOSE`.

## Platforms

| Platform | Renderer | State |
|---|---|---|
| macOS (Apple silicon) | Metal | The three games build and run. VC3 and HotD3 at 60 fps |
| Linux | OpenGL 3.3, EGL for the core | The three games build and run. The app runs the core on its game thread |
| iPhone, iPad | Metal | The cores build for iPadOS; on-device speed figures still to come |
| Apple TV | — | Not listed: the games need a 4 GB address window |
| Windows | — | Not planned yet |

## Testing

- **`chihiro-run`:** a stand-alone runner with a window. `AF_DEBUG_CH_INPUT` scripts
  coins, start, gun shots, reloads and analog channels by frame, so a full
  game can run without a person. `AF_DEBUG_CH_SHOT` saves frames to compare.
- **`tools/core_test`:** drives a core library as the app does: create,
  frames, inputs, screenshots, restart and test-menu reboots, paced at the
  core's rate with `CORE_TEST_PACE=1`.
- **Checks used for each change:**
  - a 60 s attract run, with its frame count and screenshots
  - a scripted full game: coin, start, a stage, continue, game over, name
    entry and rankings
  - the test menu in and out, and the input test
  - a 15-minute soak, for movie freezes and crashes
  - a Start-hold stress test, for VC3's thread race
  - `AF_DEBUG_CH_PERF` figures before and after
- **Rare hangs:** batches of 64 boots or more, without `AF_DEBUG_CH_SHOT`
  (writing frames changes the timing enough to cause or hide a hang), with
  a script that samples the process (`sample`) as soon as its log stops
  growing. A fix counts only when it beats the rate measured before it.
- **Trying other game data:** copy the XBE into the trial folder rather
  than linking it. D: is the folder of the XBE's resolved path, so a
  linked XBE makes the game read the original folder's files.
- **Discovery changes:** the list of function starts for all three games is
  compared before and after, and each removed or added start is checked by
  hand.
- **CI:** the release scan, plus a Linux build of the runtime with no game
  files.

## Open items

1. **Crazy Taxi attract with blanked dumps:** with the `.7z`/TeknoParrot
   data the movie slots are black (see Game files). The fix is the GD-ROM
   image's data; a patch that skips the blanked movies (script command 3
   for movies 23 and up) exists on a local branch if a set without the
   movies has to be supported.
2. **Crazy Taxi test mode:** done. The core translates `ctx_ac_t.xbe` too
   (GAME_TEST_XBE) and runs it for the test button; every menu item opens,
   INPUT TEST reads the wheel, pedals and shifter, and settings survive a
   restart.
3. **Load stutter:** 1–2 s dips to about 45–55 fps at loads. These are paced
   by the games' own polling (CRI's file reads sleep 1 ms and 10 ms between
   checks), not by host wake-ups.
4. **Linux presentation:** the app's screen rate can fall below the game's
   60 while the GL frame is read back and presented.
5. **Not done yet:** iPhone, iPad and Apple TV; `screen_surface` (handing
   over GPU surfaces instead of pixels); DSP effects; link play.
6. **Metal statistics:** the renderer does not yet fill AF_DEBUG_CH_PERF's draw and
   upload counters.

## Decisions

- **Translate ahead of time, not emulate the CPU.** This is the same
  approach as the other Arcade Foundry cores. It gives native speed and
  needs no JIT, which iOS does not allow.
- **Reimplement the kernel, never ship one.** No Xbox BIOS or kernel, from
  any source, is used. Behaviour comes from public documentation and from
  running the games; xemu and Cxbx-Reloaded are references only.
- **Hardware at its registers.** Interrupt status, FIFO and idle registers
  are live and trapped, not plain memory: the games' D3D waits depend on
  them.
- **One processor as a lock, not pinning.** macOS cannot pin threads, and
  the lock gives the games the single-CPU ordering they need on every host.
- **Game-specific inputs live in the core.** The HotD3 pump and the Crazy
  Taxi shifter are mapped in the core, so the app's controls stay
  board-agnostic.
- **`.tar` for game folders, not `.7z` in the app.** The dumps are solid 7z
  archives, and Common's 7z reader drops folder paths and decodes whole
  blocks in memory. `.tar` already streams and is what Lindbergh uses.
- **The EEPROMs from `chihiro.zip` in place.** Users have MAME's set, and
  reading it zipped avoids an unpacking step and a second copy.
