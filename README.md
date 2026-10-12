# CleriaOre

A mod loader for the retail **Ys Origin** (Steam, `yso_win.exe`). It hosts the mod layer of
[CleriaCore](https://github.com/4rtefakt/YsOrigin-CleriaCore) inside the game, so that one mod package
(`mod.ini`, `files/`, `content/`, Lua on the `cleria.*` API, `.cleriamod`) runs on both: CleriaCore, the
engine reimplementation, and the retail game through CleriaOre.

How to write a mod: CleriaCore's `samples/mods/README.md`.

## Status

A scaffold. What works:

- `dinput8.dll`, a 32-bit proxy the game loads from its own folder; every export is forwarded to the
  system DLL.
- CleriaCore's mod layer built into it (the `cleria-modkit` library, shared source, not a copy): mods are
  discovered, checked and ordered, and their Lua scripts start in the sandbox.

What does not exist yet: every hook into the game. No file override reaches the game, no event fires, and a
script's `cleria.*` calls about the game answer their defaults (no game). Not yet run inside the real game:
only `ore-loadtest`, which loads the DLL in a stand-in process.

## Build

Windows, Visual Studio 2022, CMake 3.24 or newer, Python 3. The build must be 32-bit, as the game is.

```
git submodule update --init
python external/CleriaCore/tools/fetch_deps.py --only lua,miniz
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
ctest --test-dir build -C Release
```

The result is `build/Release/dinput8.dll`.

## Layout in the game folder

```
<Ys Origin>/
    yso_win.exe
    dinput8.dll                 CleriaOre
    cleriaore/
        cleriaore.ini           [cleria.mods] UseMods / Order / Active (as in CleriaCore's cleria.ini)
        cleriaore.log           this session's log
        mods/                   mod folders, .zip and .cleriamod
```

A mod found in `mods/` starts disabled. Until there is a UI, enable it by its id:

```ini
[cleria.mods]
Active="sample.hello"
```
