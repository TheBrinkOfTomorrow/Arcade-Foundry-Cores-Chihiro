# Design documents

High-level designs for the Chihiro cores: what each part is for, how it is
built and why, how it is tested and what is still open. They describe the
design as it is and are updated in the same pull request as a change that
moves it. The dated history is in [HANDOFF.md](../../HANDOFF.md) and the
current state in [STATE.md](../../STATE.md). Designs shared by every core
(the `afcore` interface, x86recomp) are in Arcade-Foundry-Cores-Common's
`docs/design/`; the app's are in the app repository's `docs/design/`.

| Document | Covers | State |
|---|---|---|
| [CHIHIRO.md](CHIHIRO.md) | The Chihiro games: translation, kernel, NV2A renderers, sound, inputs, core library, speed, game files | VC3, HotD3, Crazy Taxi High Roller in the app on macOS and Linux; open items listed |

Each document follows the same outline: Purpose and scope, Requirements,
Design, Data and settings, Platforms, Testing, Open items, Decisions.
