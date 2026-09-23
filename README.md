# Whitehole Pro — The fastest, friendliest Super Mario Galaxy 1 & 2 level editor

<div align="center">

<img src="https://img.shields.io/badge/build-passing-brightgreen" alt="Build">
<img src="https://img.shields.io/badge/platform-Windows-b2e3f5" alt="Windows">
<img src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white" alt="C++20">
<img src="https://img.shields.io/badge/GUI-Dear%20ImGui-9f7aea" alt="Dear ImGui">
<img src="https://img.shields.io/badge/tests-passing-brightgreen" alt="Tests">
<img src="https://img.shields.io/badge/license-GPL--3.0-blueviolet" alt="License: GPL-3.0">

</div>

> ⚠️ **Whitehole Pro is in active development.** It is *playable and stable* —
> the editor launches, models render, editing is undoable, and all core tests
> pass — but this is **not** a finished product.
>
> AI (Claude) is used to accelerate development — but every feature is
> hand-reviewed, tested, and tuned before it lands.
> [How that works](docs/AI_POLICY.md).

![Editing a galaxy in the 3D viewport](ExampleImage.jpg)

---

## 🚀 Super-Quick Start (Windows — 2 double-clicks)

**You only need to do this once. Takes ~2–5 minutes.**

| Step | Do this | You'll see |
|------|---------|------------|
| **1. Build** | Double-click **`Build.bat`** | A window checks your tools, builds the app, runs tests, then tells you where `.exe` files landed. |
| **2. Run** | Double-click **`Run-Whitehole-Pro.bat`** | The editor opens. No black console window. |

That's it. ✅ If `Build.bat` reports something missing, it prints the exact fix —
see [Prerequisites](#-prerequisites--what-you-need-first).

**Where are my apps after building?**
- `build\Release\whitehole-pro.exe` — the **3D editor**
- `build\Release\whitehole-pro-console.exe` — the **CLI tool**
- (If you use Visual Studio, they may be in `build-msvc\Release\` — `Build.bat` will say.)

---

## ✨ Feature tour — what the editor does today

### 🎬 Point-and-click 3D viewport
- **Real game models** — BMD/BDL files render with their true shape, per-material diffuse colors, and category-colored placeholders when models aren't available.
- **Two-pass rendering** — opaque geometry first (depth on), then translucent materials back-to-front (glass, water, effects).
- **3D gizmo** — move / rotate / scale with axis-snapping; double-click fields for exact values.
- **Camera controls** — left-drag pan, right-drag orbit, middle-drag pan, scroll dolly (Shift = fast), **WASD fly** (Shift fast / Ctrl slow, E/Q vertical), `F` frame, `Space` jump-to, `Home` frame-all.
- **Object labels** — names float above geometry when toggled.

### 🛤 Rails, drawn properly
- Bezier paths render as tessellated curves with **clickable control handles**.
- **Stable per-rail colors** — each path keeps its color across sessions (the Java original randomized every run).
- Point counts auto-sync on add/remove. Loading tolerates missing point files gracefully.

### ⚡ Fast object workflow
- Searchable, category-filtered object list.
- **Add Object** picker (`Shift+A`) fed by the community database.
- Duplicate (`Ctrl+D`), delete (`Del`), group transform — **every action one undo step**.
- **Problems panel** — explains *and jumps to* each validation issue.
- Multi-selection with group gizmo.

### 📚 Object database on autopilot
- Friendly names + parameter descriptions download **once** from Luma's Workshop.
- Stored in your user config folder — survives reinstalls, never committed.

### 💻 CLI — the same engine, scriptable
- `map objects`, `map paths`, `objectdb check`, `hash`, `bcsv inspect`, and more.
- The **exact same C++ core** as the GUI — no duplicated logic.

---

**View menu settings** (all persisted across sessions):

| Setting | What it does | Default |
|---|---|---|
| *Overlays →* Axis / Cameras / Paths / Areas / Gravity | Toggle each world overlay | Axis ✓ Paths ✓ |
| *Object Labels* | Names float above geometry | Off |
| *Viewport →* Textures | Show TEX1 textures on models | On |
| *Viewport →* Translucency | Two-pass blend rendering | On |
| *Viewport →* Filtering | Nearest / Linear texture filtering | Linear |
| *Viewport →* Model detail | Low-poly (fast) / full detail | Low-poly |
| *Viewport →* MSAA | Multisample anti-aliasing (graceful fallback) | On |

---

## 🛠 Building from source

### Prerequisites — what you need first

| Tool | Why | Get it |
|------|-----|--------|
| **Visual Studio 2022** (or Build Tools) with *Desktop development with C++* | Compiles the app | [visualstudio.microsoft.com/downloads](https://visualstudio.microsoft.com/downloads/) — free Community edition |
| **CMake ≥ 3.20** | Generates the build | Bundled with VS, or [cmake.org/download](https://cmake.org/download/) |
| **Git** | Clones the repo | [git-scm.com/downloads](https://git-scm.com/downloads) |

That's it — ImGui, tests, and sample data are all vendored in the repo.

### The easy way

```powershell
.\Build.bat          # checks tools, builds, runs the test suite
.\Run-Whitehole-Pro.bat
```

### The manual way

```powershell
# Ninja (fast — what Build.bat uses)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# or Visual Studio
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
cmake --build build-msvc --config Release

# run the tests
ctest --test-dir build --output-on-failure
# or directly:
.\build\whitehole_core_tests.exe
```

**Build targets:** `whitehole_core` (engine, zero dependencies) · `whitehole_imgui` ·
`whitehole_app` (GUI) · `whitehole-pro-console` (CLI) · `whitehole_core_tests` (full test suite).

---

## ▶ Using the editor — a 30-second tour

1. **Launch** — the editor opens with no project.
2. **Open a stage** — `File → Open Stage…` and pick a stage archive, e.g. the bundled
   sample `data\templates\SMG2BigGalaxyMap.arc`, or a stage from your own game dump.
3. **Click things** — objects highlight as you hover; click to select; double-click a
   field in *Properties* for exact values.
4. **Edit with confidence** — every change is one `Ctrl+Z` step; the *Problems* panel
   explains any validation issue and jumps you to the object.
5. **Add content** — `Shift+A` opens the searchable Add Object picker, fed by the
   community database (auto-downloaded on first run).
6. **Save** — `Ctrl+S` round-trips the archive losslessly (verified by the test suite).

---

## 🎮 Controls & keyboard shortcuts

### Mouse (3D viewport)

| Input | Action |
|---|---|
| **Left-click** | Select object / rail point |
| **Left-drag** | Pan the camera |
| **Right-drag** | Orbit the camera |
| **Middle-drag** | Pan the camera (alternate) |
| **Scroll wheel** | Zoom in / out |
| **Shift + wheel** | Zoom faster (×3) |
| Drag the X/Y/Z fields in *Properties* (or double-click to type) | Move / rotate / scale the selection |
| Double-click an object in the list | Fly the 3D camera to it |

### Keyboard

| Shortcut | Action |
|---|---|
| <kbd>Ctrl</kbd>+<kbd>O</kbd> | Open a map archive |
| <kbd>Ctrl</kbd>+<kbd>S</kbd> | Save the current zone |
| <kbd>Ctrl</kbd>+<kbd>F</kbd> | Focus the object search |
| <kbd>Ctrl</kbd>+<kbd>Z</kbd> / <kbd>Ctrl</kbd>+<kbd>Y</kbd> | Undo / redo — add, duplicate and delete are each **one** undo step |
| <kbd>Shift</kbd>+<kbd>A</kbd> | Add Object picker (type to search, <kbd>Enter</kbd> places the top match) |
| <kbd>Ctrl</kbd>+<kbd>D</kbd> | Duplicate the selection |
| <kbd>Delete</kbd> | Delete the selection (undoable) |
| <kbd>Ctrl</kbd>+<kbd>C</kbd> / <kbd>Ctrl</kbd>+<kbd>V</kbd> | Copy / paste the selected transform (*Edit* menu also offers position-only, rotation-only, scale-only) |
| <kbd>F</kbd> | Frame the selected object |
| <kbd>Space</kbd> | Jump the camera to the selected object |
| <kbd>W</kbd><kbd>A</kbd><kbd>S</kbd><kbd>D</kbd> / arrows | Fly the camera (click the viewport first; <kbd>Shift</kbd> fast, <kbd>Ctrl</kbd> slow) |
| <kbd>E</kbd> / <kbd>Q</kbd> (or <kbd>PgUp</kbd>/<kbd>PgDn</kbd>) | Fly up / down |
| <kbd>1</kbd> / <kbd>2</kbd> / <kbd>3</kbd> | Gizmo mode: move / rotate / scale |
| <kbd>Home</kbd> | Frame the whole zone |

> All shortcuts are rebindable in **Settings → Keyboard Shortcuts…**

---

## 📁 Data files & the object database

The friendly object names and parameter descriptions come from the community
[galaxydatabase](https://github.com/SMGCommunity/galaxydatabase). The
`objectdb.json` file is ~2 MB and deliberately **not** committed — on first run
the editor fetches it on a background thread and installs it into your user
config folder (survives reinstalls, never committed). Nothing blocks the UI, and
if you're offline the editor still opens and edits normally — you just see raw
object names until a database is available.

Refresh it any time with **Settings → Update Object Database…** or
`whitehole-pro-console objectdb update`.

---

## 💻 CLI — the same engine, scriptable

`whitehole-pro-console.exe` shares the **exact same core** as the GUI:

```powershell
# List objects in the bundled template map (no game dump needed)
whitehole-pro-console.exe map objects data\templates\SMG2BigGalaxyMap.arc

# List rails in a stage
whitehole-pro-console.exe map paths data\templates\SMG2BigGalaxyMap.arc

# A real extracted SMG1/SMG2 folder (the one containing StageData)
whitehole-pro-console.exe game list C:\path\to\extracted\files
whitehole-pro-console.exe galaxy inspect C:\path\to\extracted\files HoneyBeeKingdomGalaxy

# Field-name hashes (parity with the Java reader)
whitehole-pro-console.exe hash Obj_arg0

# Inspect a BCSV table
whitehole-pro-console.exe bcsv inspect <file.arc> /Stage/MapObj/StageObjInfo.bcsv

# Check / update the object database
whitehole-pro-console.exe objectdb check
```

Full reference: [`docs/CPP_REWRITE.md`](docs/CPP_REWRITE.md).

---

## 🧯 Troubleshooting

| Symptom | Fix |
|---|---|
| `cmake is not recognized` | Install CMake (above), **close and reopen** your terminal, retry. Check with `cmake --version`. |
| `No CMAKE_CXX_COMPILER could be found` | Install VS2022 with **Desktop development with C++**, reboot, delete `build\` (`rmdir /s /q build`), run `Build.bat` again. |
| Build succeeds but the `.exe` is missing where the docs say | Visual Studio puts it in `build\Release\`, Ninja in `build\` — `Build.bat` and `Run-Whitehole-Pro.bat` find it either way. |
| Tests fail but a `.exe` exists | App may still run. Copy the red text into an issue plus your `cmake --version` and compiler (`cl` vs `g++ --version`). |
| `whitehole-pro-console.exe` flashes and closes | That's the console tool — run it **from a terminal** to see output, or use `whitehole-pro.exe` for the windowed editor. |
| Editor is empty / says "Drag a map archive…" | Normal! **File → Open Stage…** → `data\templates\SMG2BigGalaxyMap.arc` for an instant demo. |

Still stuck? Open an issue with: your Windows version, `cmake --version`,
which compiler you installed, and the last ~30 lines of the `Build.bat` window.

---

## 🗂 What's in this repo?

```
Build.bat                <- double-click to BUILD (start here)
Run-Whitehole-Pro.bat    <- double-click to RUN the editor
Whitehole.bat            <- old launcher, still works (forwards to Run-*.bat)
cpp/                     <- the active native C++20 app (this is what builds)
src/                     <- legacy Java reference (frozen — no new features)
data/templates/          <- bundled .arc maps for instant try-out
docs/CPP_REWRITE.md      <- CLI reference + C++ migration plan
docs/AI_POLICY.md        <- how AI is used in development
scripts/build-release.ps1<- one-command build + optional .zip packaging
CMakePresets.json        <- windows-release / lto / debug / msvc-release presets
GalaxyTools/             <- reference-only sibling tools (see Credits — not built)
```

> **Contributors:** new code belongs under `cpp/`. The `src/` Java tree is the
> frozen behavioral reference — see [`docs/CPP_REWRITE.md`](docs/CPP_REWRITE.md).

---

## 📜 Credits & licenses

Whitehole Pro stands on the shoulders of Ruan de Jager that has kept Super Mario
Galaxy modding alive for years. This project would not exist without him.

### This repository's license

**Whitehole Pro is licensed under the GNU General Public License v3
(GPL-3.0)** — see [`LICENSE`](LICENSE) for the full text.

- ✅ Free to use, study, share and modify — including for your own level-design
  tools and ROM-hack workflows
- 🔗 Distributing a modified Whitehole Pro means releasing your changes under
  GPL-3.0 as well
- 🎮 Nintendo's Super Mario Galaxy assets are **not included** — you supply
  your own legally obtained, extracted SMG1/SMG2 files. The bundled
  `data/templates/*.arc` are original sample maps made for this project.
  *Whitehole Pro is not affiliated with Nintendo; Super Mario Galaxy is a
  trademark of Nintendo / HAL Laboratory.*

### Bundled dependencies

| Library | License | Copyright |
|---|---|---|
| **[Dear ImGui](https://github.com/ocornut/imgui)** (`cpp/third_party/imgui/`) — the entire docked GUI | MIT | © 2014–2026 Omar Cornut — see `cpp/third_party/imgui/LICENSE.txt` |
| Everything else under `cpp/` (Yaz0, RARC, BCSV, BMD/BDL, BTI, editor, CLI, renderer…) | **GPL-3.0** (this project) | © 2026 IsaiasDV and Whitehole Pro contributors |

### `GalaxyTools/` — reference tools this project learned from

The `GalaxyTools/` directory holds sibling tools whose **ideas and techniques**
shaped Whitehole Pro's 3D viewport and renderer. They are reference-only:
**not compiled, not linked, not part of the build** — and no source code from
them is copied into this repository.

| Tool | License | How Whitehole Pro uses it |
|---|---|---|
| **[Supernova](https://github.com/)** (`GalaxyTools/Supernova-main/`) — SMG editor & viewer | **MIT** — © 2026 Super Mario Galaxy Modding Community | Studied its viewer architecture: joint-space vertex baking, the opaque/translucent two-pass draw ordering, and per-material draw state. All re-implemented independently in our C++20 renderer — **no code copied**. |
| **[Takochu](https://github.com/)** (`GalaxyTools/Takochu-main/`) — SMG model/scene viewer | **GPL-3.0** | Studied *concepts only* (material draw-flag classification, per-material depth/blend state). Because GPL-3.0 is copyleft, **zero Takochu code is present in this tree** — everything was re-derived from the J3D format specification and Whitehole Neo's own Java reader. |
| **[bmd-bdl-export](https://github.com/IsaiasDV/bmd-bdl-export)** (`GalaxyTools/Bmd-Bdl-Export-main/`) — Blender-side BMD/BDL exporter | **MIT** — © 2026 IsaiasDV | Texture/material handling in our TEX1 path was informed by this exporter's approach. Technique-level reference only. |

### Data & community

| Source | What it provides |
|---|---|
| **[galaxydatabase](https://github.com/SMGCommunity/galaxydatabase)** | The SMG object/parameter database behind `objectdb.json` — friendly names, descriptions, parameter docs. Fetched at first run, never committed. |
| **Luma's Workshop** | Object-database groundwork and the SMG reverse-engineering the modding scene builds on. |

### The legacy Java app also used (frozen `src/` tree only, not compiled anymore)

[JOGL/jogamp](https://jogamp.org/) & [gluegen](https://jogamp.org/gluegen/www/)
· [JSON-java](https://github.com/stleary/JSON-java) ·
[FlatLaf](https://github.com/JFormDesigner/FlatLaf) ·
[JWindowsFileDialog](https://github.com/JacksonBrienen/JWindowsFileDialog) ·
[DiscordIPC](https://github.com/LogicismDev/DiscordIPC)

### Colophon

Built with C++20, CMake, Win32, OpenGL, Dear ImGui, and a only the best love for
Super Mario Galaxy.

---

## 🗺️ Roadmap

**Done:** native C++20 GUI + 3D viewport · lossless archive round-trip ·
undo/redo · transform gizmo + picking · rail/path editing with undo · real
BMD/BDL models in-viewport (joint-baked, textured, two-pass translucent) ·
MSAA · per-material cull/alpha/blend/depth state · overlays (axis, rails,
cameras, areas, gravity) · object database auto-download · CLI sharing the
editor's core · full test suite.

**Next (in order):**

1. 🚧 KCL collision rendering — collision-aware selection & snapping
2. 🚧 Animations (BCK/BPK/BRK/BTK/BTP/BVA) — spin platforms, doors, live stars
3. 🚧 Layer filtering & hierarchical object tree
4. 🚧 BCSV spreadsheet editor
5. 🚧 Galaxy & zone management (create galaxy, zone add/remove)
6. 🚧 Specialized object renderers (star trajectories, world-map links…)
7. 🚧 Model/texture browser inside the app
8. ⏳ Cross-platform GUI (the core already builds anywhere)

Full migration status: [`docs/CPP_REWRITE.md`](docs/CPP_REWRITE.md).

---

## 📚 Documentation

| Document | What's in it |
|---|---|
| [`docs/CPP_REWRITE.md`](docs/CPP_REWRITE.md) | CLI reference + the full Java → C++ migration plan |
| [`docs/AI_POLICY.md`](docs/AI_POLICY.md) | How AI-assisted development is used in this repo |
| `cpp/tests/core_tests.cpp` | The behavioral test suite that pins Java parity |

---

<div align="center">

**★ Star the repo if Whitehole Pro saves your galaxy ★**

*Made by modders, for modders.*

[Report a bug](../../issues) · [Request a feature](../../issues) · [Whitehole-Neo upstream](https://github.com/SMGCommunity/Whitehole-Neo)

</div>


