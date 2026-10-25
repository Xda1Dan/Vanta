# Vanta — macOS Roblox Executor

macOS Roblox executor built on the Nexa internals, restructured into a clean
LunarModule-style layout: one concern per folder, no catch-all "core/client/utils"
split.

## Layout

```
Vanta/                          # ACTIVE PROJECT
├── build/libVanta.dylib        # built dylib that gets injected
├── src/
│   ├── main.cpp               # constructor init, hooks, join loop, autoexec
│   ├── RBX/                   # Roblox-facing subsystems
│   │   ├── task_scheduler/    # TaskScheduler, Heartbeat pump
│   │   ├── security/          # signature checks, load guard/clock, verification
│   │   ├── execution/         # compiler, executor thread, queue, trace, async
│   │   └── environment/       # Registry, Lua libraries, chunks, EncStrings
│   ├── Roblox/                # Offsets, LuaObject, ClientAPI
│   ├── network/               # HTTP control server, sockets, websocket
│   ├── renderer/              # AppKit overlay
│   ├── managers/              # console, decompiler shim, input, dialogs
│   └── Utilitys/              # filesystem, journal, version
├── ui/                        # SwiftUI app ("Vanta Prototype"), built with ui/build.sh
├── tools/                     # vanta-machopatch, vanta-luaucheck, generators
├── tests/                     # sUNC harness
└── Dependencies/              # tinyhook, Luau, base64, lz4

# References (not part of the build)
unix-roblox/                   # proven Nexa implementation + offset dumper
luau-0.744/                    # matching Luau VM source (tag 0.744)
LunarModule/                   # Windows executor, architecture reference
roblox-macplayer-internals/    # RE notes for macOS build 0.742
design/                        # UI design language and mockups
Vanta-alpen-backup/            # previous iteration
RobloxExecutor/                # Alpen (Windows) reference
```

Runtime data lives outside the repo in `~/Documents/Vanta/`: `autoexec/`,
`workspace/` (scripts), `library/`, `console/`.

## Build

```bash
cd /Users/user/Documents/Vanta/Vanta
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8

# Optional UI
bash ui/build.sh          # -> build/ui/Vanta Prototype.app
```

## Launch

```bash
./launch-vanta.sh 90441122676618    # any place ID; defaults to this one
```

The launcher:

1. Strips Roblox's codesign when the signature is still valid (the pinned
   0.744 client is ad-hoc re-signed once at install; see below).
2. Runs `vanta-machopatch --status` and, if any executor `LC_LOAD_*_DYLIB`
   remains in `RobloxPlayer` (e.g. a stale `libNexa.dylib`), removes it with
   `--remove`. Two executors in one client is what produced the grey-screen
   freeze; Vanta must be the only one loaded.
3. Injects `build/libVanta.dylib` via `DYLD_INSERT_LIBRARIES` and opens the place.

## Running scripts

- **Autoexec** — every text file in `~/Documents/Vanta/autoexec/`, name order, on each join.
- **HTTP** — `POST http://127.0.0.1:5553/execute` with the script as the body.
  Console history: `GET /console?since=<seq>`; console input: `POST /console/input`.
  Every route requires `Authorization: Bearer <token>`; the token is written to
  `~/Documents/Vanta/.vanta-token` (mode 0600) when the dylib starts. `/status`
  reports `state` (starting/waiting/ready/reinstall/failed), `detail`, `game`,
  `armed` and `queued`.
- **UI** — `build/ui/Vanta Prototype.app`, talks to the same loopback server.

Journal (dylib diagnostics, `RUN start/done/failed` lines):
`/tmp/vanta_executor.log`.

## Configuration

`~/Documents/Vanta/autoexec/.vanta-autoexec`

```
enabled=1
off=script-name.lua
```

`~/Documents/Vanta/.vanta-settings`

```
safemode=1    # 1 = block unsafe members, 0 = allow all
```

Both files are read by the dylib on each join; the UI writes the same files.

## Architecture

1. `DYLD_INSERT_LIBRARIES` loads the dylib; the constructor runs before `main`.
2. Load guard/signature patches go in, then the HTTP control server binds
   `127.0.0.1:5553`.
3. `waitForGame` polls (from the init thread, it does not block the client) until
   the new `ScriptContext` is readable.
4. Hooks are installed: bytecode signature bypass, player-leave, teleport.
5. The executor thread is created with the sandboxed UNC environment (identity 8,
   executor mark), then autoexec runs.
6. Queued and autoexec scripts are pumped one per Heartbeat. The queue gate waits
   for `game:IsLoaded()` and a LocalPlayer, and times out after 30 s.
7. Teleport/rejoin re-hooks the new context and reruns autoexec; it does not
   restart the executor.

## Tooling

- `tools/machopatch.c` → `vanta-machopatch`: inspect (`--status`), add, or remove
  (`--remove`) executor load commands in a Mach-O, without a pristine backup.
  Writes are atomic (tmp + fsync + rename).
- `tools/luaucheck.cpp` → `vanta-luaucheck`: compile a script with Vanta's Luau
  settings to check syntax/bytecode outside the client.
- `tools/gen-encstrings.py`: regenerate `src/RBX/environment/EncStrings.h` from
  `src/RBX/environment/Lua/*.lua`.

## Updating offsets

```bash
cd /Users/user/Documents/Vanta/unix-roblox/offset-dumper
python3 dump.py
# copy the result into Vanta/src/Roblox/Offsets.h
```

## Status

- Vanta builds and runs: joins places, runs autoexec and HTTP scripts, survives
  the sUNC suite past `debug.getstack`.
- CallInfo reads in the registry are layout-checked and fault-safe (0.742:
  `base` +0x00, `top` +0x08, `func` +0x18, stride 0x30; vararg frames have a
  larger `base - func`).
- `/status` reports executor readiness (`starting` / `waiting` / `ready` /
  `reinstall` / `failed`), the game-loaded flag, the pump, and queue depth; the
  UI follows it instead of treating HTTP 200 as "ready".
- The control server requires a per-process bearer token written to
  `~/Documents/Vanta/.vanta-token` (0600) and no longer sends a wildcard CORS
  header.
- Known gaps: offsets are still tied to build 0.742.0.7421053 (a mismatch shows
  as REINSTALL, not a crash); teleport/rejoin relies on polling the
  ScriptContext; there is no auto-updater by design.

---

**Educational use only** — for learning about macOS injection, Luau VM internals,
and reverse engineering. Use only on games you own, per Roblox ToS.
