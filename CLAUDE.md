# CLAUDE.md — Ulrich's Creality Print fork

Context for Claude (and me) when working on this repo from any machine.
This is a personal fork of Creality Print with three features. Started
2026-10-13 on the work laptop. Keep the "Status" section up to date.

## Home PC: state as of 2026-10-15

The home PC is set up and built (`build\OrcaSlicer\CrealityPrint.exe` runs; it shares
`%APPDATA%\Creality\Creality Print\7.3` with the official install, so close the official app
first). Build gotchas hit on this PC, all solved:
- Breakpad needs the **C++ ATL** component in the VS Build Tools (add it via the VS
  installer, with no MSBuild processes running) and the DIA SDK env vars for the deps build too.
- Use limited parallelism for the app build or the compiler runs out of memory (PCH heap
  errors): `cmake --build . --config Release --target ALL_BUILD -- -m:2 /p:CL_MPCount=3`.
- Needs ~30 GB free on C: during deps + app build. Close the running app before the
  `install` target, or the DLL is locked.

Still to test on the KE (printer idle first):
a. **Change material** (Device tab → open KE → bar at top).
b. **Exclude Objects** panel during a multi-object print — may already cover feature 2.
c. **Follow print** against the real printer — see the Live preview item below.
Measured 2026-10-15: the KE's port-9999 websocket sends ~4 `curPosition` changes/s while
printing, plus `layer`, `TotalLayer`, `printFileName`, `printProgress`.

## The goal

Add these to Creality Print itself (not via Mainsail/Fluidd/OctoPrint):

1. **Live print preview** – while a print is running, the G-code preview (layers +
   toolpath) should follow the printer in real time, showing exactly where the
   nozzle is and where it's going next.
2. **Cancel a single object mid-print** – when printing several objects and one
   fails, cancel just that object and let the rest carry on.
3. **Change material** – guided filament swap from the Device tab: pause (or
   park when idle), heat to 245 °C (editable), push 35 mm out, prompt for the new
   material, "New material" purges 50 mm more / "Same material" doesn't, resume.

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
- Breakpad needs the DIA SDK headers, which its project doesn't add. Set these in
  the same shell first (PowerShell):
  `$dia="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\DIA SDK"`,
  `$env:CL="/I`"$dia\include`""`, `$env:LINK="/LIBPATH:`"$dia\lib\amd64`""`.
- Run builds at low priority (`start "" /low /b /wait ...`) or the laptop crawls.
- Timings on the laptop (12 cores): deps ~2.5 h, app ~1.5 h, at low priority.
- Runnable app: `build\OrcaSlicer\CrealityPrint.exe`. Upstream installs a
  WebView2 offline installer that isn't in the public repo; `src/CMakeLists.txt`
  now skips it when missing.
- Status 2026-10-13: laptop deps + app build succeed (incl. live preview code).

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
- [x] First build on the laptop (deps + app), 2026-10-13 — see Build section
- [ ] Same build setup on the home PC
- [x] Locate code (2026-10-13), see "Code map" below
- [ ] At home: test the existing Exclude Objects panel on the KE; log what the
      port-9999 websocket reports while printing (how often `curPosition` updates)
- [ ] Implement cancel object (or fix/enable the existing one)
- [ ] Change material (feature 3) — written + compiles 2026-10-14, untested on the
      KE. Device tab → open the KE → "Change material" bar at the top.
      Code: `GUI/ChangeMaterialDialog.*`, `print_manage/Device/CrealityPrinterSocket.*`
      (own ws://ip:9999 connection), bar in `PrinterMgrView::create_device_tools_bar`.
      Test idle first. Check: second websocket connection accepted, `nozzleTemp` /
      `targetNozzleTemp` / `state` / `curPosition` read correctly, park position
      (idle: X10 Y10 Z50) and extrude speed (3 mm/s) OK. Log lines:
      `CrealityPrinterSocket`.
      2026-10-17: the dialog showed "Â°C" (UTF-8 `°` read as ANSI by MSVC); fixed with
      `degrees_celsius()` (explicit UTF-8 bytes), installed 2026-10-19. Same trap applies to
      any non-ASCII literal.
- [x] Live preview — **follows the real KE as of 2026-10-16** (`CrealityPrinterPositionSource`;
      address = env `CREALITY_LIVE_PRINTER_IP` or the last LAN printer opened in the
      Device tab, stored as app config `live_print_printer_address`; falls back to the
      simulation). The 2026-10-15 failure was a **locale bug**: `sscanf("%lf")`/`stod`/`strtod`
      follow the system locale and reject "133.97" under a decimal-comma locale. Use
      `std::from_chars` for printer numbers (fixed in LivePrintTracker and ChangeMaterialDialog).
      2026-10-19: rewritten after two failed attempts (index glide, then a time-based clock —
      feedrate-only time estimates were ~5x too fast, so it stalled and lurched). Now
      **"good enough"** on the KE: 85–100% of reports match, no resyncs in steady printing.
      How it works: `PrintPositionMatcher` maps each report to mm along the toolpath
      (`m_length`), matching **XY only** (printer Z is off by bed mesh/z-hops), searching
      15 mm behind to well ahead of a prediction from the learned nozzle speed, and
      preferring the printer's layer: **KE `layer` is 1-based = slicer `layer_id` + 1**
      (neighbouring layers share XY paths, so without this it followed the layer below).
      `LivePrintTracker` shows where the nozzle was 0.8 s ago, interpolated along the path
      between reports; burst reports are spread ≥0.15 s apart. Known limits: ~0.8 s lag;
      printer report stalls >~1 s still pause it; the preview draws whole G-code moves, so a
      long straight line appears in one step (fix would be a marker drawn inside the move).
      `MoveVertex::time` is the move index, not a duration — don't use it for timing.
      Limits: the preview must be the unchanged slice of the file being printed; changing
      slice settings (e.g. adding a filament) discards the G-code, so nothing moves until
      re-sliced, and the new slice no longer matches the print. Idea: show "waiting for a
      sliced preview" / distance-to-path in the banner. Diagnostics: every 5 s warning-level
      `LivePrintTracker:` lines (samples/matched; keys + raw curPosition when none parse).
      Original simulated-only notes: "Follow print"
      button above "Slice plate" in Preview (styled like it; also key L; arrow keys
      stop it). Code: `src/slic3r/GUI/LivePrintTracker.*` (position source → move
      matcher → timer), `GLCanvas3D::toggle_live_print_follow` /
      `_apply_live_print_move` / `_render_live_print_button`, and
      `get_moves_slider_value_for_move` in both renderers. Simulation speed:
      `CREALITY_LIVE_SIM_SPEED` (default real time). Not yet checked on screen
      that following looks right. Next: real source from the KE (`curPosition`).

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

## Fork behaviour changes (besides the two features)

- Builds default to **Release** (`cmake/BuildInfoUtil.cmake`). The old default
  "Alpha" used Creality's internal test servers (pre.crealitycloud.com) + vConsole.
- **Login is optional**: the first-run region page's automatic login request is
  skipped (`GUI_App::skip_login_after_region_choice` + `ShowUserLogin`, which
  reports the dialog closed so the page moves on). Logins the user asks for,
  e.g. the AI assistant's Login button, open normally.
- **No app updates**: `GUI_App::check_new_version_cx` returns immediately. It
  had offered the official 7.3.0 installer, which went to Program Files and ran
  instead of our build. To get Creality's newer versions: merge `upstream` into
  `ulrich/features` and rebuild.
- Version is set in `version.inc` to **7.3.0.6157** (the upstream release the
  source matches). At 7.0.0.0, cloud features like the AI assistant demanded an
  update. Bump it when merging a newer upstream release.
- Run our build from `build\OrcaSlicer\CrealityPrint.exe`, not the Start menu
  (that may point at an official install).

## Notes

- Work laptop: I'm the IT department, so no policy blocks installing build tools
  or doing this project here.
