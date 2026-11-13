#!/usr/bin/env python3
"""Write the files that list this repo's games from games.toml.

  gen_games.py          rewrite them
  gen_games.py --check  only compare (CI): exit 1 when one is out of date

Writes cmake/games.cmake: CH_GAMES and each game's title and core version,
which CMakeLists.txt uses over games/<set>/game.cmake. A new game: its
[[game]] in games.toml and its games/<set>/ folder, then this.
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "common", "tools", "games"))
import afgames  # noqa: E402


def generate(out):
    games = afgames.load_core(os.path.join(ROOT, "games.toml"))
    cm = ["# Written by tools/gen_games.py from games.toml: edit that, not this.\n",
          f"set(CH_GAMES {afgames.cmake_list(g['set'] for g in games)})\n"]
    for g in games:
        if g["board"] != "chihiro":
            raise afgames.ManifestError(f"{g['set']}: only Chihiro games belong in this repo")
        cm.append(f"set(CH_TITLE_{g['set']} {afgames.c_string(g['title'])})\n")
        cm.append(f"set(CH_CORE_VERSION_{g['set']} {g['core_version']})\n")
    out.write("cmake/games.cmake", "".join(cm))


if __name__ == "__main__":
    afgames.main(generate, ROOT, __doc__)
