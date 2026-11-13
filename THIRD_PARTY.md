# Third-party components

| Component | Used for | Upstream | Licence |
|---|---|---|---|
| Arcade-Foundry-Cores-Common (`common/`, git submodule) | https://github.com/TheBrinkOfTomorrow/Arcade-Foundry-Cores-Common | BSD-3-Clause (this project's own code) | `tools/x86recomp`, the recompiler shared with Lindbergh. Pinned by the submodule. |
| nxdk `lib/xboxkrnl/xboxkrnl.exe.def` | the kernel's export names, ordinals and argument sizes, turned into `runtime/kernel_names.h` | https://github.com/XboxDev/nxdk | CC0-1.0 |
| MAME `src/mame/sega/chihiro.cpp`, `src/devices/bus/jvs/` (jvs13551, jvsdev, jvshost), `src/mame/microsoft/xbox_usb.cpp` | the baseboard's USB command set, descriptors and JVS answers in `runtime/baseboard.c` (written from them, not copied) | https://github.com/mamedev/mame | BSD-3-Clause, Copyright Samuele Zannoli (chihiro, xbox_usb) and Olivier Galibert (jvs) |
| capstone (Python bindings) | disassembly in the recompiler, at build time only; not linked into the cores | https://github.com/capstone-engine/capstone | BSD-3-Clause |

Reference only, nothing copied: xemu (GPL-2.0; its hw/xbox nv2a, apu and
chihiro files are LGPL-2.1+), Cxbx-Reloaded (GPL-2.0).
