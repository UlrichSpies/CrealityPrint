# CLAUDE.md — Ulrich's Creality Print fork

Context for Claude (and me) when working on this repo from any machine.
This is a personal fork of Creality Print to add two features. It is summarised
from the first planning chat (2026-10-13). Keep the "Status" section up to date.

## The goal

Add two features to Creality Print itself (not via Mainsail/Fluidd/OctoPrint):

1. **Live print preview** – while a print is running, the G-code preview (layers +
   toolpath) should follow the printer in real time, showing exactly where the
   nozzle is and where it's going next.
2. **Cancel a single object mid-print** – when printing several objects and one
   fails, cancel just that object and let the rest carry on.

## Setup

- **Printer:** Creality Ender 3 V3 KE. Runs Creality OS, which is Klipper-based (same
  family as the K1). Prints are only done from the **home PC**.
- **Repo:** fork `https://github.com/UlrichSpies/CrealityPrint` (`origin`),
  upstream `https://github.com/CrealityOfficial/CrealityPrint` (`upstream`).
  Code is at **v7.3.0** (upstream `master`, 2026-09-24). Creality Print is AGPL-3.0
  and derived from OrcaSlicer / Bambu Studio / PrusaSlicer, so there's no reverse engineering
  needed — we change the source.
- **Our work goes on the `ulrich/features` branch**, so `master` can track upstream.
- **Machines:**
  - Work laptop: `C:\Users\ulric\CrealityPrint`. Code + build only; no printer
    access. Has Visual Studio Community 2026 with the C++ workload, and Git.
    Cloned with `--filter=blob:none` (partial clone).
  - Home PC: needs its own clone and build setup (VS C++ tools, the first long
    deps build). Testing against the real printer happens here.
- **Workflow:** Claude edits; I watch the diffs in VS Code.

## Build (Windows)

Repo-root scripts include `build_deps.bat`, `build_release_vs2022.bat`,
`build_vs2022_unified.bat`. Not yet read or run. Expect a long dependency build
(30–60+ min) on first run, then the app build. Strawberry Perl may be needed.

## Technical plan

### Feature 2 – cancel object (do this first, smaller)
- Slicer side: Creality Print/Orca already labels objects in G-code
  ("Label objects" / "Exclude objects", emits `EXCLUDE_OBJECT_DEFINE` etc.).
- Printer side: Klipper's `[exclude_object]` +
  `EXCLUDE_OBJECT NAME=...`. **Check** whether the KE stock firmware has it
  enabled (may need one line in `printer.cfg`).
- App side: find out how Creality Print talks to the KE (Creality's websocket,
  port 9999 on K-series, as far as we know) and whether it already has an
  exclude-object UI for some models. Add/enable a "cancel this object" action in
  the Device/preview UI that sends the command.

### Feature 1 – live preview
1. Read the print position from the printer over the network.
2. Map it to a G-code line/move in the sliced file.
3. Drive the preview's existing layer slider + in-layer (move) slider to that
   point, updating continuously.
- **Unknown:** does Creality's websocket report an exact file position, or only
  % / layer? If only %, fall back to installing **Moonraker** on the KE, which
  exposes `virtual_sdcard.file_position` exactly.
- On the work laptop this can be developed against a fake/simulated position feed;
  real testing at home.

## Status / next steps

- [x] Fork cloned, upstream remote added, CLAUDE.md written
- [ ] Read build scripts/docs, run first build (deps, then app)
- [ ] Locate code: G-code preview (GCodeViewer / sliders), printer connection
      (Device tab, websocket client), object labelling
- [ ] Find out what the KE reports while printing (needs home network)
- [ ] Implement cancel object
- [ ] Implement live preview

## Notes

- It's a work laptop: check IT policy on installing build tools and doing personal
  projects, and code ownership on company equipment.
