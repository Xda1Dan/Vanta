# Vanta UI

Native macOS frontend for the Vanta Luau executor. SwiftUI shell, AppKit text
editor, no WebView and no JavaScript runtime.

## Build and run

```sh
./ui/build.sh
open "build/ui/Vanta Prototype.app"
```

The dylib exposes its control channel at `http://127.0.0.1:5553`; the app does
not launch, patch or inject into Roblox. Start Roblox with Vanta yourself, join
a game, and the UI switches to `Attached`. The listener comes up before the
executor is ready, so the app follows `/status` rather than HTTP 200 alone.

## HTTP control API

Every route except `OPTIONS` requires
`Authorization: Bearer <token>`. The dylib writes a fresh token to
`~/Documents/Vanta/.vanta-token` (mode 0600) before it starts listening; the
app reads it per request. There is no CORS grant.

| Method | Path | Meaning |
|---|---|---|
| `GET` | `/status` | `{ok, state, detail, game, armed, queued, console, version}` |
| `POST` | `/execute` | body is Luau source; queues it and returns `{status}` |
| `GET` | `/console?since=N` | bounded console history since sequence `N` |
| `POST` | `/console/input` | answers an `rconsoleinput` prompt |

`state` is `starting`, `waiting`, `ready`, `reinstall` or `failed`; `detail`
is a sentence for the user. `game` is the bootstrap's last answer to
`game:IsLoaded()` + `LocalPlayer`, and `armed` is the pump. A script sent while
`waiting` is queued for the next join; `reinstall` and `failed` will never run
it.

The app uses ordinary URLSession requests. There is no Unix socket, no stale
socket file, and no framing handshake.

## What works

- **Execute** the active buffer or a file through the dylib. `Sent` is shown as
  soon as the HTTP request is accepted; `Executed` or `Failed` is shown when the
  journal reports the pump actually ran it. No “queued script” toast.
- **Console** polls real `rconsole` records. It auto-sizes to its content,
  opens automatically for errors and `rconsoleinput`, and can be collapsed.
- **Explorer** reads the real `~/Documents/Vanta/{autoexec,workspace,library}`
  folders; **Scripts only** is the default filter, with an All-files toggle.
  Refresh is silent, with a spinning icon and no toast. Right-click for open,
  run, reveal, new file/folder, and trash. Drag files from Finder to copy them
  into the workspace.
- **Search** has ScriptBlox thumbnails, relevance ranking (title exact/phrase/
  token coverage, `matched`, verified, working, views, recency), filters
  (Working, No key, Verified, Universal, Free), sorting (Best match, Most views,
  Most likes, Newest, Recently updated), relative upload/update dates,
  pagination, native source preview, retry, and Save to Library.
- **Library** is a real `~/Documents/Vanta/library` folder; saved scripts show
  up in the explorer and local search.
- **Command palette** (`⇧⌘P`) for files, actions, search, console, focus mode,
  settings and about.
- **Focus mode** (`⇧⌘F`) hides the rail, explorer and console.
- **Session restore**: open tabs and the active tab come back on relaunch.
  Dirty buffers are saved into the session too, so a quit does not reset an
  edited file to its last saved contents. Closing a dirty tab asks Save /
  Don't Save / Cancel.
- **Autoexec** and **Safe mode** are in Settings only and apply on the next
  join. Safe mode is explained there rather than repeated in the shell.
- **Status** is one label at the bottom left: `No client`, `Waiting for game`,
  `Attached`, `Reinstall required`, or `Executor failed`. The server's detail
  string (for example "Roblox is not the pinned build") is in the tooltip and
  in Settings → Connection; a REINSTALL transition also raises a toast and an
  error line in the console, and disables Run.

## Design

Monochrome like the reference, not a green-accented imitation. Base `#09090B`,
chrome `#0C0C0E`, panel `#0F0F12`, card `#1F1F24`, off-white action `#F2F2F2`.
Cards are 148 pt with 14 pt radius and a soft top highlight. Hover/press
transitions are 0.14–0.18 s ease-out; the app fades in on launch. There is one
status label and no pills or status dots.

## Not yet implemented

Split panes, rscripts, and filtering ScriptBlox by the current game. The core
MVP — execute, console, files, search, library, autoexec/safe mode — is wired.
