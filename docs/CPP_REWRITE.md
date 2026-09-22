# Whitehole C++ rewrite

Whitehole is moving from its Java 11/Swing implementation to a native C++20 application. The rewrite is incremental so each migrated format can be verified against real Super Mario Galaxy data before the remaining editor surfaces depend on it.

## Current native milestone

The repository builds `whitehole-pro` (windowed editor) and `whitehole-pro-console` (CLI) from `whitehole_core`. The native app currently owns:

- bounds-checked endian-aware binary I/O;
- path-safe project filesystem access;
- Yaz0 compression and decompression;
- big- and little-endian RARC reading, mutation, extraction, repacking, and Yaz0 recompression;
- big- and little-endian BCSV/JMap reading and writing;
- SMG1/SMG2 game, galaxy, and stage archives;
- placement object loading (name, layer, type, position, rotation, scale) with round-trip save;
- galaxy/zone display names from `data/galaxies.json` and `data/zones.json`;
- object metadata from the community object database (`data/objectdb.json`) with a
  compiled cache, plus a non-blocking first-run download when the file is absent;
- a Windows desktop editor that can open a game folder or a map archive, list objects, edit transforms, and save;
- object authoring in that editor: a searchable Add Object picker fed by the community database, duplicate, delete, and the transform copy/paste from the Java Edit menu — each one a single undo step, shared with the CLI (`map add` writes the same rows the editor does);
- path (rail) support: `CommonPathInfo` plus its `CommonPathPointInfo` point tables are loaded, saved and edited — the viewport draws the tessellated bezier with its control handles, path and point selection is shared by the Objects and Properties panels, and every rename or point move is a single undo step;
- a command-line interface on every platform.

The bundled `.arc` galaxy templates are part of the native test suite.

`data/objectdb.json` is deliberately not committed: it is roughly 2 MB and is
maintained by the community. The editor downloads it in the background on first
run and reloads the editor state when it lands, so the UI never blocks on the
network; if the download fails the editor stays fully usable with raw object
names, reports the reason as a toast plus a Log entry, and offers a retry from
**Settings > Update Object Database...** or
`whitehole-pro-console objectdb update`. The Properties panel states plainly when
no database is installed instead of showing an empty parameter grid.

## Build

You need CMake 3.16+ and a C++20 compiler (Visual Studio 2019 16.11+, Visual Studio 2022, or a recent GCC/Clang). No Java install is required for the native app.

### Windows (Visual Studio)

```bat
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The editor is `build\Release\whitehole-pro.exe` (or `build\Debug\whitehole-pro.exe` for a Debug build). Launching it with no arguments opens the desktop editor. The console tool is `build\Release\whitehole-pro-console.exe`.

### Other generators

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

## Run

```sh
# Windows desktop editor
build\Release\whitehole-pro.exe
build\Release\whitehole-pro-console.exe gui

# List objects inside a bundled template map
whitehole-pro-console map objects data/templates/SMG2BigGalaxyMap.arc

# Open a real SMG1/SMG2 workspace (the folder that contains StageData)
whitehole-pro-console game list path\to\extracted\files
whitehole-pro-console galaxy inspect path\to\extracted\files HoneyBeeKingdomGalaxy
whitehole-pro-console zone objects path\to\extracted\files HoneyBeeKingdomGalaxy

# Archive / BCSV / Yaz0 tools
whitehole-pro-console archive list data/templates/SMG2BigGalaxyMap.arc
whitehole-pro-console archive extract data/templates/SMG2BigGalaxyMap.arc extracted
whitehole-pro-console bcsv inspect extracted/Stage/jmp/Placement/Common/ObjInfo
whitehole-pro-console yaz0 decompress input.szs output.arc
whitehole-pro-console hash Obj_arg0

# Object database (downloads when missing; the editor does this by itself too)
whitehole-pro-console objectdb check
whitehole-pro-console objectdb update
whitehole-pro-console objectdb query Kinopio
```

In the Windows editor: **File > Open Map Archive...** and choose `data/templates/SMG2BigGalaxyMap.arc` to load objects without a full game dump. Use **File > Open Game Directory...** for an extracted SMG workspace. Edit name/position/rotation/scale, click **Apply**, then **File > Save Zone**.

## Migration boundary

The existing `src/` Java tree remains the behavioral reference until its equivalent is present and covered by native tests. New functionality belongs under `cpp/`; Java code should not gain new features.

Still to migrate:

1. KCL collision and animation data (BMD/BTI parsing already feeds the viewport).
2. The remaining specialised object renderers (gravity/area shapes, PowerStar, world-map links) and the world overlays that are not built yet.
3. The remaining desktop editors (BCSV spreadsheet, world map, galaxy creation).
4. Cross-platform GUI packaging, then removal of the Java/Ant build.

Each stage should replace a complete vertical slice. Java is removed only when the native replacement can open, edit, save, and reopen representative SMG1 and SMG2 data without loss.
