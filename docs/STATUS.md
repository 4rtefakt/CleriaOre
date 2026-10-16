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
  demand from the mod's plain file. The music folder too (`files/music/<name>.ogg`). Not covered: the movies and file names that
  are not ASCII.
- Lua on the game thread: `tick` (the game's 60 Hz step) and `frame`, the `room_enter` and `flag_changed`
  events, and what a script reads of the game: the room and its chests, the flags, the gear upgrades, the
  character, item names, `frozen()` and the player's numbers (`cleria.player.stats()`, but the luck and
  the position).
- What a script changes: the flags, HP, SP, the level, the weapon tier, EXP (`add_exp`, `exp_factor`), a
  warp to a statue, `cleria.game.save`, and its own files (`cleria.data`, in `cleriaore/modsdata/<id>/`).
  Each goes through the game's own function where it has one.
- Combat (`cleria.combat.add_filter`): `"damage"` on the player's hits and on the hits he takes, `"exp"` on
  a kill's and a boss's EXP, `"sp"` on a kill's SP, `"drop"` on a kill's drops (suppress, or another item),
  `"pickup"` on a pickup taken (suppress; another value for gold, healing, the Boost and Air seeds), and the
  `enemy_killed` and `pickup_collected` events; `game.spawn_pickup`. A query carries less than CleriaCore's:
  no separate `formula` for a hit, no intermediate steps for EXP, no `cast`, `slot` or
  `chance` for a drop; a pickup has no value of its own.
- `player.set_modifier` (max HP, ATK, DEF), on top of each of the game's stat recomputes.
- API 7's host side: a mod's own window (`cleria.ui.window`, `message`, `choose`), `p:text`, `p:progress`,
  `p:table` and `p:image`, input actions and key state (`cleria.input`), timers, the bus, commands across
  mods, script reload, `game.state` (`title`, `play`, `cutscene`, `panel`, `window`, `held`, `gameover`, and `map` for the
  warp map: the game's other menus are not told apart) and `state_changed`, a mod's `.dds` images on its HUD layers.
- `cleria.world`: the room's monsters (`actors`), `to_screen` and the `d:world_*` drawing (the camera the
  game publishes), `set_actor`, `remove`, and `spawn` for a cast the room already holds (the game loads a
  monster's model with the room). An actor's id is the game's own instance id, good for the room.
- Game operations: `player.set_position`, `set_mp`, `set_boost`, `set_exp`, heal and hurt, `game.pause`,
  `game.set_speed`, `game.room_title` (the game's scene table; so `floor` and `zone`), `game.goto_room` (the request a script's ChangeScene makes), the random seed, a sound of the game's (`audio.se`), `camera.get`, `player.pose` and `familiars`, `player.tune` (speed, jump, MP cost, MP regeneration, Boost gain, extra air jumps), `player.set_invulnerable`, a script's hits (`combat.damage` on a monster, `combat.strike` on the player, without the flinch), `world.boss` (the gauge; no `actor` for a boss built of map objects) and `combat.boss_bonus`. The "damage" filter also sees the player's hits on a boss's parts. What was learnt of the retail game for these is in [RETAIL_RE.md](RETAIL_RE.md).
- `game.skip_cutscenes` (the retail Archipelago client's fast-forward, on its sites): while a mod asks, a cutscene's
  timed waits and its "wait for a walk, a pan, a fade" waits end at once (never a scene load's), and the intro movies
  are not opened. Not CleriaCore's whole skip: what a waited-for walk or fade would have done is cut short, not
  landed. Unasked, a script that stands 15 s on one `Wait_ChrMoving` (an actor that cannot reach its mark) is let
  through, with a line in the log.
- `cleria.script.stop()` from a content filter on a flag store (0x64 / 0x67): that script ends right after the store.
  Not from the icon or the box filter.
- `content/*.ini`: `chests.ini`, `rules.ini`, `pickups.ini` (items of the game's own), `text.ini` (a script's
  strings), new items
  (0x100..0x1FF) kept with the save, the retail items' patched name, description and value.
- The events CleriaCore's content layer reports: `chest_opened`, `door_opened`, `boss_defeated`, `death`
  and `item_used`.
- The script VM's grants: `cleria.content.add_filter` sees a script's flag stores (ops 0x64 and 0x67, not
  the rest of the store family), the item icon (0x116) and the "Acquired" box (0xD5), and may pass,
  suppress or replace them; `cleria.content.give`; `item_obtained`.
- Sessions: `new_game`, `load` and `save`, and `cleria.save`, a mod's storage kept with the save. It is
  CleriaCore's sidecar file, `yso_NN.cleria`, but in `cleriaore/saves/`, not beside the game's save (which
  may be in the Steam Cloud only): it does not follow the save to another machine.
- The overlay, drawn in the game's own frame in the look of the game's own menus (`src/kit.inl`, a UI kit
  with the API of CleriaCore's, so a mod's page is drawn by either): the mods' HUD layers, their notices, and the **F1** panel (also the title's own "Mods" row, added to the game's menu before Exit),
  and for a mod the title's New Game row (its command `title_new_game` runs in the row's place) and the loader's own commands
  `ore.title new_game`, `ore.new_game character= difficulty= extra= room= flags=` (a new game in any room),
  `ore.menu` (a menu of the game's own: its framed list, with a mod's caption and rows) and `ore.prompt` (a line typed in one), with the mods On / Off (applied at
  the next start), each mod's own page and its settings (`cleriaore.ini`, `[cleria.mod.<id>]`). The game
  is held while the panel is open.

- `CleriaOre.exe`, the manager, for outside the game: it finds the game (Steam's libraries, or you choose
  it), installs, updates and removes the loader (another mod's `dinput8.dll` is kept aside and put back),
  and lists the mods: On / Off, the load order, each mod's details, install from a `.cleriamod` or a `.zip`
  (the file dialog, a drop on the window, or as its argument), remove to the Recycle Bin. It is the same
  panel as in the game and writes the same `cleriaore.ini`.

The hooks are for one build only, the Steam `yso_win.exe` linked 2024-01-08: on any other exe nothing is
hooked and the log says so.

- Puppets (`world.puppet`, `set_puppet`, `remove_puppet`, `prop`: any of the three characters or a cast's
  sprite as an actor the mod moves, with `glow`, `aura` and `hittable`; not `target`), `world.form` (the numbers,
  `ai`, the name, the boss gauge), `world.snapshot` and `world.apply`, `world.spawn` for any monster cast.
- `player.equipment` and `equip`, `player.status` and `set_status` (without the hit's show), `audio.music` and
  `stop_music`, `screen.fade` and `flash`.
- Sessions: `game.load` (from the title too), `game.retry`, `game.to_title`. The camera: `camera.set` and `reset`,
  `camera.shake`, `camera.look_at`. `audio.se_file`, a WAV of the mod's (by Windows: one at a time, untested). Effects: `player_effect` and `world.puppet_effect`. `script.run`, a script of the game's.
- The room's doors: `game.exits` and `take_exit`. The look: the `scale` of `player.set_look` and of `world.form`.
- API 7's events from the game: `enemy_hit`, `player_hit`, `level_up`, `floor_changed`, `retry`, `enemy_spawned`,
  `player_bullet`, `player_effect`. Monster variants (`content.define_monster`, `morph`): the numbers, the scale,
  `on_hit`, `on_death` and the phases, the name and kind in `world.actors()`, a boss's gauge. `hud.show`: `"status"` and
- A mod's own picture beside the card when a chest gives its item (`Icon=` in `items.ini`, or `cleria.ui.icon`).
- `audio.music("file.ogg")`, a music file of the mod's own. The load order in the F1 panel (for the next start).
- `game.new_game` (from the title). The glow: `player.set_look`'s `glow`, `world.form`'s `tint`, a puppet's `glow`.
  A kill by `combat.damage` gives its EXP and SP and raises `enemy_killed`.
- Bullets: `player_bullet` for each of the player's, and `world.show_bullet`, the same bullet again for show.
- The blessing shop: `blessing_bought` with the price paid, and `cleria.shop.add_filter`'s Price answer (in the
  menu, the test and the payment), Row answer (a row's text), Owned answer and Buy answer (another effect, or none).
- The world's filters: `"spawn"` (the multipliers, another cast, "suppress"; not another level) with
  `enemy_spawned`, `"exit"` (another room, or "suppress"; the door's `index` and name), `"script"` (a script
  refused) and `"skill"` (a cast refused).
- Save profiles (`cleria.save.set_profile`): the saves of a profile are kept under `profiles/<name>/`, in
  Steam's storage as the game's own are, their sidecars in `cleriaore/saves/profiles/<name>/`. `cleria.locale`
  (the language Steam installed). `game.unlock_character(3)`: Toal on Character Select for this run, the clear
  data untouched (not seen working: this install has him open already).

What does not exist, so a script's call answers its default: a puppet as a `target` (the game's monsters
chase and hit only its own player), a sprite sheet file of a mod's own for a variant (a sheet of the game's by
its name works; the game draws from its table of 269), another level from the "spawn" filter, and
`world.boss().actor` for a boss built of map objects.

The scripted mods of `cleriacore-mods` (17 with the probes) all start and run a fight and room changes without
a script error. **Co-op runs against a fake teammate** (`tools/coop_bot.py`), as host (the join, the
teammate's puppet with its name, the party list, chat, the pose and snapshots going out) and as guest (the
host's snapshots place and hold the room's monsters, the others fall). Not tried: two real games, hits and
kills passed between them, a boss.

How to test a build by hand, and the drive that steers the game for unattended tests: [TEST_PROTOCOL.md](TEST_PROTOCOL.md).

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
