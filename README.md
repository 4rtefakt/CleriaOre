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
- File overrides: a mod's `files/` folder replaces the game's packed files. The archive reader's lookup
  and read (`FUN_005defa0`, `FUN_005df220`) are hooked; a file the game holds packed (`.z`) is packed on
  demand from the mod's plain file. Not covered: the loose folders (`music`, movies) and file names that
  are not ASCII.
- Lua on the game thread: `tick` (the game's 60 Hz step) and `frame`, the `room_enter` and `flag_changed`
  events, and what a script reads of the game: the room, the flags, the character, the player's HP, level,
  SP, EXP and weapon tier.
- The script VM's grants: `cleria.content.add_filter` sees a script's flag stores (ops 0x64 and 0x67, not
  the rest of the store family), the item icon (0x116) and the "Acquired" box (0xD5), and may pass,
  suppress or replace them; `cleria.content.give`; `item_obtained`. A query's `object` is empty, and
  `item_obtained` tells a chest from another script by the script's name (`s_box...`).

The hooks are for one build only, the Steam `yso_win.exe` linked 2024-01-08: on any other exe nothing is
hooked and the log says so.

What does not exist yet, so a script's call answers its default: `chest_opened` and the other events,
the setters of the player's numbers, `new_game` / `load` / `save` and the per-save storage (a load from
inside a game is seen as many `flag_changed`), `frozen()`, `content/*.ini`, and everything drawn (HUD
layers, a mod's page).

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
