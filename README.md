# Whitehole Pro
## The latest and greatest cutting-edge Whitehole
### (and we can do this 'cause Whitehole Neo is open-source hahaha)

> Yes, AI is used in this project. So what? No apologies here — we use
> every tool we've got (including Claude) to move faster and make SMG
> modding easier for everyone.

![Editing Flipswitch and Flip-Swap Galaxy](https://github.com/IsiasDV/Whitehole-Pro/blob/master/ExampleImage.jpg)

Whitehole Pro is a friendly editor for Super Mario Galaxy 1 & 2 level files.
It opens `.arc` map files and extracted game folders, lets you browse objects,
tweak positions/rotations/scales, and save them back — with a point-and-click
Windows editor **plus** a command-line tool for power users and scripts.

> Beyond Whitehole Neo: Whitehole Neo is cool and all that, right? And since
> it's open source, we're taking everything great about it and making it
> BETTER with Claude — an overhauled interface, optimized code, and extreme
> user-friendliness. Tutorials inside the app are on the roadmap too.
>
> Built on [Whitehole Neo](https://github.com/SMGCommunity/Whitehole-Neo),
> rewritten in modern C++20 for speed and for a true double-click `.exe`.

---

## ⭐ Super-Quick Start (Windows, 2 double-clicks)

**You only need to do this once. Takes ~2–5 minutes.**

| Step | What to do | What happens |
|------|------------|--------------|
| **1. Build** | Double-click **`Build.bat`** | Checks your tools, builds the app, runs tests, tells you where the `.exe` files are |
| **2. Run** | Double-click **`Run-Whitehole-Pro.bat`** | Opens the editor — no black console window |

That's it. If `Build.bat` says something is missing, it tells you the exact
one-line fix (see [Prerequisites](#-prerequisites--what-you-need-first) below).

**Where are my apps after building?**

- `build\Release\whitehole-pro.exe` — the **editor** (double-click this, no console pop-up)
- `build\Release\whitehole-pro-console.exe` — the **console/CLI tool** (for commands & scripts)
- (If you use Visual Studio, they may be in `build-msvc\Release\` instead — `Build.bat` tells you.)

### First 60 seconds in the editor

1. Run the app (`Run-Whitehole-Pro.bat`).
2. Go to **File → Open Map Archive…**
3. Pick `data\templates\SMG2BigGalaxyMap.arc` (ships with the repo — no game dump needed).
4. Click an object in the list → edit position/rotation/scale → click **Apply**.
5. **File → Save Zone** to write your changes.

Got a full extracted SMG1/SMG2 dump (the folder containing `StageData`)?
Use **File → Open Game Directory…** instead, then pick a galaxy.

---

### The object database (automatic, one time)

The friendly object names and the whole parameter grid come from the community
[galaxydatabase](https://github.com/SMGCommunity/galaxydatabase). That
`objectdb.json` is roughly 2 MB and deliberately **not** committed, so on first
run the editor fetches it on a background thread and installs it into `data\`.
Nothing blocks the UI, and if you are offline the editor still opens and edits
normally — you just see raw object names until a database is available.

Refresh it whenever you like with **Settings → Update Object Database…** (or
`whitehole-pro-console objectdb update`). The Properties panel says so inline
when no database is installed.

## 📦 Prerequisites — what you need first

You need **exactly 2 things**. `Build.bat` checks both for you.

### 1. CMake 3.16 or newer

Easiest (PowerShell):

```powershell
winget install Kitware.CMake
```

Or download the installer: <https://cmake.org/download/>
(tick **"Add CMake to the system PATH"** during install, then open a fresh terminal.)

Check yours:

```bat
cmake --version
```

### 2. A C++20 compiler — pick ONE

**Option A — Visual Studio 2022 Community (recommended):**

```powershell
winget install Microsoft.VisualStudio.2022.Community --silent --override "--add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended --passive"
```

Or download from <https://visualstudio.microsoft.com/downloads/>
and tick **"Desktop development with C++"**. Reboot after installing.

**Option B — MSYS2 UCRT64 (lightweight, no Visual Studio):**

```powershell
winget install MSYS2.MSYS2
```

Then in the **UCRT64** terminal:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-make
```

**You do NOT need:** Java (old technology, anyways 😄), Python, Node, or
anything else. No weird preinstalls — this installs and works out of the box, baby!

---

## 🛠️ Build options (pick your comfort level)

### 🟢 Easiest: double-click (recommended for everyone)

> Double-click **`Build.bat`**

It configures `.\build\`, builds Release, runs the tests, smoke-tests the
CLI, prints exactly where your `.exe` files are, and offers to launch the
editor. Run `Build.bat --clean` to wipe `.\build\` and start over.

### 🟡 One-command PowerShell (clean checkouts + shareable zip)

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-release.ps1
```

Add `-Package` to also create `dist\WhiteholePro-<version>-win64.zip`
(both `.exe` files + `data\` + docs). Extra flags: `-Configuration`,
`-BuildDirectory`, `-SkipTests`, `-Lto` (see header of that script).

### 🔴 Manual (developers / CI / Mac / Linux)

Windows (Visual Studio — note `--config Release`!):

```bat
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
build\Release\whitehole-pro.exe
```

Windows (MSYS2 UCRT64) or Linux/macOS:

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Presets (see `CMakePresets.json`):

```sh
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

> ⚠️ **Beginner gotcha:** with Visual Studio the `.exe` lands in
> `build\Release\` (not just `build\`). With Ninja/MSYS2 it lands directly
> in `build\`. `Build.bat` and `Run-Whitehole-Pro.bat` find it either way.

---

## ▶️ Run — every way to launch it

### The editor (what most people want)

```bat
Run-Whitehole-Pro.bat
```

…or double-click `build\Release\whitehole-pro.exe` directly. No arguments =
empty editor (use the **File** menu). Drag-and-drop / "Open with…" a `.arc`
file also works. The old `Whitehole.bat` still works — it forwards here.

### The console tool (scripting, quick checks, CI)

```bat
build\Release\whitehole-pro-console.exe --help
build\Release\whitehole-pro-console.exe gui
```

Handy commands (run from the repo root so `data\` resolves):

```bat
:: List objects in the bundled template map (no game dump needed)
build\Release\whitehole-pro-console.exe map objects data\templates\SMG2BigGalaxyMap.arc

:: Real extracted SMG1/SMG2 folder (the one containing StageData)
build\Release\whitehole-pro-console.exe game list C:\path\to\extracted\files
build\Release\whitehole-pro-console.exe galaxy inspect C:\path\to\extracted\files HoneyBeeKingdomGalaxy
build\Release\whitehole-pro-console.exe zone objects C:\path\to\extracted\files HoneyBeeKingdomGalaxy

:: Archive / table / compression tools
build\Release\whitehole-pro-console.exe archive list data\templates\SMG2BigGalaxyMap.arc
build\Release\whitehole-pro-console.exe archive extract data\templates\SMG2BigGalaxyMap.arc extracted
build\Release\whitehole-pro-console.exe bcsv inspect extracted\Stage\jmp\Placement\Common\ObjInfo
build\Release\whitehole-pro-console.exe yaz0 decompress input.szs output.arc
build\Release\whitehole-pro-console.exe hash Obj_arg0
```

Full CLI reference + migration plan: [`docs/CPP_REWRITE.md`](docs/CPP_REWRITE.md).

---

## 🆘 Troubleshooting — "it didn't work, now what?"

| Symptom | Fix (try in order) |
|---------|-------------------|
| `cmake is not recognized` | Install CMake (above), **close and reopen** the terminal, retry. Check with `cmake --version`. |
| `No CMAKE_CXX_COMPILER could be found` | Install VS2022 with **Desktop development with C++**, reboot, delete `build\` (`rmdir /s /q build`), run `Build.bat` again. |
| Built it but can't find the `.exe` | Look in `build\Release\` (VS) or `build\` (Ninja/MSYS2). Or just use `Run-Whitehole-Pro.bat` — it finds it. |
| Configure keeps failing | `Build.bat --clean`, then `Build.bat` again. |
| MSYS2 user failing in `cmd.exe` | Use the **UCRT64** terminal instead, with the Ninja commands above. |
| Tests fail but a `.exe` exists | App may still run. Copy the red text into an issue + your `cmake --version` and compiler (`cl` vs `g++ --version`). |
| `whitehole-pro-console.exe` flashes and closes | That's the console tool — run it **from a terminal** to see output, or use `whitehole-pro.exe` for the windowed editor. |
| Editor is empty / says "Drag a map archive…" | Normal! **File → Open Map Archive…** → `data\templates\SMG2BigGalaxyMap.arc` for an instant demo. |

Still stuck? Open an issue with: Windows version, `cmake --version`,
which compiler you installed, and the last ~30 lines of the `Build.bat` window.

---

## 🗂️ What's in this repo?

```
Build.bat                <- double-click to BUILD (start here)
Run-Whitehole-Pro.bat    <- double-click to RUN the editor
Whitehole.bat            <- old launcher, still works (forwards to Run-*.bat)
cpp/                     <- the active native C++20 app (this is what builds)
src/                     <- legacy Java reference (frozen — no new features)
data/templates/          <- bundled .arc maps for instant try-out
docs/CPP_REWRITE.md      <- CLI reference + C++ migration plan
scripts/build-release.ps1<- one-command build + optional .zip packaging
CMakePresets.json        <- windows-release / lto / debug / msvc-release presets
```

> **Contributors:** new code belongs under `cpp/`. The `src/` Java tree is the
> frozen behavioral reference — see [`docs/CPP_REWRITE.md`](docs/CPP_REWRITE.md).

> **We're competent.** Yeah, the tone on this page is a little ironic — we know!
> But seriously: this project is all about lowering the learning curve so SMG
> modding is for *anyone*. Great tutorials, a friendly UI, and docs a total
> beginner can follow — that's the goal.

---

## 🗺️ Roadmap — what we plan to include

- Something cool (and lots of it — overhauled UI, optimized native code,
  in-app tutorials, and every great Whitehole Neo feature, but friendlier).

## Conclusion!

Whitehole Neo is great but aging; Starforge could be the future but still in deep development. Whitehole Pro is the fast, zero-install C++ tool you can use right now to build maps today. And don't worry about switching to Supernova or Starforge! We'll make sure to closely monitor what features and updates they'll be implementing, so we'll have a better equivalent ready for you.

---

## Controls
- Left Click: Select object
- Left Click Drag: Pan camera
- Right Click Drag: Orbit camera
- Scroll Wheel: Zoom the camera in/out
- Drag the X/Y/Z fields in Properties (or double-click to type): move/rotate/scale the selection
- Double-click an object in the list: fly the 3D camera to it

## Useful Keyboard Shortcuts
- <kbd>Ctrl</kbd>+<kbd>O</kbd>: Open a map archive
- <kbd>Ctrl</kbd>+<kbd>S</kbd>: Save the current zone
- <kbd>Ctrl</kbd>+<kbd>F</kbd>: Focus the object search
- <kbd>Ctrl</kbd>+<kbd>Z</kbd> / <kbd>Ctrl</kbd>+<kbd>Y</kbd>: Undo / redo — adding, duplicating and deleting objects are all one undo step each
- <kbd>Shift</kbd>+<kbd>A</kbd>: Add Object picker (type to search, Enter places the top match)
- <kbd>Ctrl</kbd>+<kbd>D</kbd>: Duplicate the selected object
- <kbd>Delete</kbd>: Delete the selected object (undoable with Ctrl+Z)
- <kbd>Ctrl</kbd>+<kbd>C</kbd> / <kbd>Ctrl</kbd>+<kbd>V</kbd>: Copy / paste the selected object's transform (Edit → Copy / Paste also offers position, rotation or scale only)
- <kbd>F</kbd>: Frame the selected object in the 3D viewport
- <kbd>Space</kbd>: Jump the camera to the selected object

## Libraries
- **jogamp**: https://jogamp.org/
- **gluegen**: https://jogamp.org/gluegen/www/
- **org.json**: https://github.com/stleary/JSON-java
- **flatlaf**: https://github.com/JFormDesigner/FlatLaf
- **JWindowsFileDialog**: https://github.com/JacksonBrienen/JWindowsFileDialog
- **DiscordIPC**: https://github.com/LogicismDev/DiscordIPC

