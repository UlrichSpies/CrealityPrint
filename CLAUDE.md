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

Prerequisites (VS 2026 alone does NOT work: `deps/deps-windows.cmake` rejects
MSVC >= 1950, and pinning v143 inside VS 2026 fails in CMake):
- `winget install Microsoft.VisualStudio.2022.BuildTools` with the VCTools workload
  (gives the "Visual Studio 17 2022" generator + MSVC 14.44 + its own CMake).
- `winget install StrawberryPerl.StrawberryPerl` (OpenSSL rejects Git's Perl).
- Python not needed.

Build (from a cmd with the BuildTools CMake + `C:\Strawberry\perl\bin` on PATH,
script called as `.\build_release_vs2022.bat` so cmd finds it):
- `.\build_release_vs2022.bat deps` → `deps\build\OrcaSlicer_dep` (Release only)
- `.\build_release_vs2022.bat slicer` → `build\` (VS solution + install)

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
- [ ] Read build scripts/docs, run first build (deps, then app) — laptop deps
      build in progress 2026-10-13
- [x] Locate code (2026-10-13), see "Code map" below
- [ ] At home: test the existing Exclude Objects panel on the KE; log what the
      port-9999 websocket reports while printing (how often `curPosition` updates)
- [ ] Implement cancel object (or fix/enable the existing one)
- [ ] Implement live preview — first version written 2026-10-13, not yet compiled:
      `src/slic3r/GUI/LivePrintTracker.*` (position source → move matcher → timer),
      `GLCanvas3D::toggle_live_print_follow` / `_apply_live_print_move`, and
      `get_moves_slider_value_for_move` in both renderers. Press **L** in Preview to
      follow a simulated print (`CREALITY_LIVE_SIM_SPEED`, default 10x); arrow keys
      or L stop it. Next: compile and test, then a real source from the port-9999
      websocket (`curPosition`).

## Code map

- **Device tab is a prebuilt web bundle**, not C++: `resources/web/deviceMgr/`
  (minified `assets/*.js`, no Vue sources in repo). It talks to the printer via
  `ws://<host>:9999`, messages `{method:"set"|"get", params:{...}}`.
  Status fields: `curPosition` ("X:.. Y:.. Z:.."), `layer`, `TotalLayer`,
  `printProgress`, `printFileName`, `printJobTime`, `objects`,
  `excluded_objects`, `current_object`. No byte file position seen.
- **Exclude objects already exists** in that bundle: panel draws outlines and
  sends `{method:"set",params:{excludeObjects:[name]}}` (LAN). Slicer emits
  `EXCLUDE_OBJECT_*` (`src/libslic3r/GCode.cpp` ~5301, 9874, 15544; option
  `exclude_object` in `PrintConfig.cpp:3917`). → First test on the KE whether it
  just works; Feature 2 may only need enabling/fixing, not writing.
- C++ websocket proxy for port 9999: `src/slic3r/GUI/print_manage/App/`
  (`WebSocketProxy.cpp`, `WebSocketProxySession.cpp`, `DeviceMessageFilter.cpp`,
  host webview in `PrinterMgrView.cpp`). Possible tap for live position.
- Preview: `src/slic3r/GUI/GCodeViewer.hpp` (facade, `get_layers_slider()`,
  `get_moves_slider()`, `update_sequential_view_current`);
  `GCodeRenderer/BaseRenderer.*` (sliders, `SequentialView`, `gcode_ids`);
  `GLCanvas3D.cpp` ~4492 (arrow keys drive sliders from code — template for
  "follow printer"); `IMSlider.*` (`SetHigherValue`).
  `GCodeProcessorResult::moves[].gcode_id` maps moves to G-code lines.

## Notes

- Work laptop: I'm the IT department, so no policy blocks installing build tools
  or doing this project here.
