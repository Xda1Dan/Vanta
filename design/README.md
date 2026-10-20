# Vanta UI — monochrome, native, Madium-inspired

Runnable prototype: `build/ui/Vanta Prototype.app`, built by `./ui/build.sh`.
Source: `ui/`. The old `gui/` directory is not used.

## Screens

- `final-home.png` — Home with real cards, quick actions, recent files, and
  folder counts.
- `final-settings.png` — real connection state, executor switches, editor
  settings.
- `final-search.png` — relevance-ranked ScriptBlox search with thumbnails,
  filters, sort, relative dates, and a native source preview.
- `final-console.png` — real console output over the HTTP console endpoint.
- `palette.png` — the command palette.

## Shell

```
custom 48 pt top bar: V mark · tabs · Run (editor) · window controls
┌──────────┬──────────────┬──────────────────────────────────────┐
│ rail 50  │ explorer 228 │ Home / Editor                        │
├──────────┴──────────────┴──────────────────────────────────────┤
│ 22 pt status: No client / Attached · Ln/Col                   │
└────────────────────────────────────────────────────────────────┘
```

The macOS menu bar handles File/Edit/View/Run/Help. There is no second
in-window menu row. The app is attach-only and talks HTTP to
`127.0.0.1:5553`.

## Design language

- Vanta black + off-white. There is no green chrome; the logo and app icon are
  monochrome white.
- Base `#09090B` · chrome `#0C0C0E` · panel `#0F0F12` · card `#1F1F24`.
- Primary actions are off-white `#F2F2F2` with dark text, like the reference.
- Cards: 148 pt, 14 pt radius, soft top highlight, 0.18 s hover transition.
- Home padding: 52 pt sides, 36 pt top; 20 pt card gap.
- Sidebar: 228 pt, 30 pt rows, 17 pt Explorer title, 12.5 pt section titles,
  no counts and no footer switches.
- Controls: 7–10 pt radii, press scale 0.99, hover/press 0.14–0.18 s ease-out.
- One status label at the bottom left. No pills, no status dots, no repeated
  offline labels.
- No mock console lines, fake sessions, fake queue numbers, or fake cards.

## Real data only

Everything visible comes from the dylib, app state, or the filesystem. Search
thumbnails and dates come from ScriptBlox (`image`, `createdAt`,
`reviewedAt`/`lastBump`), formatted as relative time.

## Motion

Cards lighten and lift slightly; tabs and rail selection transition; the
sidebar and console animate open; the non-error status is quiet; toasts are
small and have no progress bar; the app fades in on launch. Focus mode,
session restore, command palette and the scripts-only explorer filter are
native behavior, not decoration.
