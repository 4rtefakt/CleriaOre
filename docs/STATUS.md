# What works, and how to build it

The technical side of CleriaOre: what of CleriaCore's mod API is backed on the retail game, and how to
build it from source. For players, see the [README](../README.md).

## Status

What works:

- `dinput8.dll`, a 32-bit proxy the game loads from its own folder; every export is forwarded to the
  system DLL.
- CleriaCore's mod layer built into it (the `cleria-modkit` library, shared source, not a copy): mods are
  discovered, checked and ordered, and their Lua scripts start in the sandbox.
- File overrides: a mod's `files/` folder replaces the game's packed files. The archive reader's lookup
  and read (`FUN_005defa0`, `FUN_005df220`) are hooked; a file the game holds packed (`.z`) is packed on
  demand from the mod's plain file. Not covered: the loose folders (`music`, movies) and file names that
  are not ASCII.
- Lua on the game thread: `tick` (the game's 60 Hz step) and `frame`, the `room_enter` and `flag_changed`
  events, and what a script reads of the game: the room and its chests, the flags, the gear upgrades, the
  character, item names, `frozen()` and the player's numbers (`cleria.player.stats()`, but the luck and
  the position).
- What a script changes: the flags, HP, SP, the level, the weapon tier, EXP (`add_exp`, `exp_factor`), a
  warp to a statue, `cleria.game.save`, and its own files (`cleria.data`, in `cleriaore/modsdata/<id>/`).
  Each goes through the game's own function where it has one.
- The events CleriaCore's content layer reports: `chest_opened`, `door_opened`, `boss_defeated`, `death`
  and `item_used`.
- The script VM's grants: `cleria.content.add_filter` sees a script's flag stores (ops 0x64 and 0x67, not
  the rest of the store family), the item icon (0x116) and the "Acquired" box (0xD5), and may pass,
  suppress or replace them; `cleria.content.give`; `item_obtained`.
- Sessions: `new_game`, `load` and `save`, and `cleria.save`, a mod's storage kept with the save. It is
  CleriaCore's sidecar file, `yso_NN.cleria`, but in `cleriaore/saves/`, not beside the game's save (which
  may be in the Steam Cloud only): it does not follow the save to another machine.
- The overlay, drawn in the game's own frame in the look of the game's own menus (`src/kit.inl`, a UI kit
  with the API of CleriaCore's, so a mod's page is drawn by either): the mods' HUD layers, their notices, and the **F1** panel, with the mods On / Off (applied at
  the next start), each mod's own page and its settings (`cleriaore.ini`, `[cleria.mod.<id>]`). The game
  is held while the panel is open.

- `CleriaOre.exe`, the manager, for outside the game: it finds the game (Steam's libraries, or you choose
  it), installs, updates and removes the loader (another mod's `dinput8.dll` is kept aside and put back),
  and lists the mods: On / Off, the load order, each mod's details, install from a `.cleriamod` or a `.zip`
  (the file dialog, a drop on the window, or as its argument), remove to the Recycle Bin. It is the same
  panel as in the game and writes the same `cleriaore.ini`.

The hooks are for one build only, the Steam `yso_win.exe` linked 2024-01-08: on any other exe nothing is
hooked and the log says so.

What does not exist yet, so a script's call answers its default: the combat and shop filters and their
events (`enemy_killed`, `pickup_collected`, `blessing_bought`), `spawn_pickup`, the stat modifiers,
`unlock_character`, the save profiles, `content/*.ini`, images and icons, and the load order in the panel.

## Build

Windows, Visual Studio 2022, CMake 3.24 or newer, Python 3. The build must be 32-bit, as the game is.

```
git submodule update --init
python external/CleriaCore/tools/fetch_deps.py --only lua,miniz,fonts
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
ctest --test-dir build -C Release
```

The result is `build/Release/CleriaOre.exe`: the manager and installer in one file. The loader
(`dinput8.dll`) and the panel's fonts are inside it as resources, and the C runtime is linked in, so that
file is the whole release.

## Layout in the game folder

```
<Ys Origin>/
    yso_win.exe
    dinput8.dll                 CleriaOre
    cleriaore/
        cleriaore.ini           [cleria.mods] UseMods / Order / Active (as in CleriaCore's cleria.ini)
        cleriaore.log           this session's log
        mods/                   mod folders, .zip and .cleriamod
        CleriaOre.exe           the manager, copied here by its own Install (the Start menu points at it)
        fonts/                  the overlay's fonts
        saves/                  yso_NN.cleria, what the mods keep with each save
        modsdata/<mod id>/      a mod's own files (cleria.data)
```

A mod found in `mods/` starts disabled: turn it on in the manager or in the F1 panel, then restart the
game.
