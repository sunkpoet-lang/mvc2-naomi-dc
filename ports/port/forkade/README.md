# ForkAde's additions to the port

What [ForkAde](https://github.com/sunkpoet-lang) adds to gibletto's port so that Marvel vs. Capcom 2 plays like
ForkAde's other games: save states, rollback's saves, the menu bar, replays with ForkAde's viewer, and online play
for two with spectators and chat. Everything here is marked `(ForkAde)` where it touches the port's own files.

| Files | What | Where from, license |
|---|---|---|
| `state*.c`, `state*.h`, `rollback.c/.h`, `../sys/coro_x86.c` | Save states (the RAM, the programs' and the host's data, the state heap, the sound RAM), rollback's saves (Windows' write watch) and the game's coroutine on a stack in the state heap | ForkAde, AGPL-3.0 (this repository's) |
| `bar.cpp/.h` | The menu bar | ForkAde, AGPL-3.0 |
| `replays.cpp/.h` | Replays and the viewer's board | ForkAde, AGPL-3.0 |
| `fareplay.cpp/.h` | The `.fareplay` file | sunkpoet-lang/cps2recomp 260326d (`port/replay.cpp`), GPL-3.0 |
| `online/` | ForkAde's online session | sunkpoet-lang/sf3comp 8e7f280 (`cps3recomp/port/`), from the CPS fork, GPL-3.0; `port.h`, `port_mvc2.cpp`, `online_mvc2.h`, `online_c.h` are ForkAde's, AGPL-3.0 |
| `recomp-common/` | The menu bar, the in-game menu, the replay viewer, the chat, the font, the words | recomp-common (Tommy McLeroy and ForkAde's fork), GPL-3.0, the commit in `COMMIT` |
| `patch_gekkonet_sf3.cmake` | GekkoNet's spectator state, compressed and sent a few chunks at a time | sunkpoet-lang/sf3comp, GPL-3.0 |
| `patch_gekkonet_mvc2.cmake`, `zlib/` | GekkoNet's portable state for MvC2's 52 MB; zlib's names over the port's miniz | ForkAde, AGPL-3.0 |

GekkoNet (HeatXD/GekkoNet, BSD-2-Clause) is not in the repository: CMake takes its sources from `GEKKONET_SOURCE`
(the release ForkAde pins, v20261024222910-9da2363) and patches a copy in the build.

The GNU GPL-3.0 and the GNU AGPL-3.0 may be combined (section 13 of each); the program as a whole is under the
AGPL-3.0, and its source is this repository.
