# Changelog

## Unreleased

- The manager's **Install a mod file...** and update buttons are above the list of mods, not below it.
- A failed update download says so in the message row.

## 1.1.0

For players:

- **Double-click a `.cleriamod` to install it.** Installing the loader makes CleriaOre open mod files; with
  CleriaOre already open, the file goes to that window.
- **Each mod's own settings in CleriaOre**, without starting the game: click the mod in the list. The same
  settings are still in the game under F1.
- **Mod updates.** When CleriaOre opens, it looks for newer releases of your mods on their GitHub pages. A mod
  with an update shows its new version in gold; open it and press **Update to ...**, or **Update N mods** under
  the list. Each mod keeps its settings, and a mod you had turned off stays off.
- **CleriaOre tells you when a newer CleriaOre is out**, on the Game page, and can download it.
- **Mods that need other mods.** Turning a mod on turns on the mods it requires; turning one off says which
  mods need it. A mod that cannot load says why in its row.
- **Settings are no longer lost.** Changing a mod in CleriaOre used to undo settings changed in the game (and
  the other way round); both now keep each other's changes. A mod installed while the game runs is turned on
  at the game's next start.
- **The F1 Mods tab** is one row per mod with its state (On, Off, On after a restart, Error), and a page per mod
  with its settings, On / Off and its place in the load order. With mods turned off, it says so and can turn
  them back on.
- **A "Mods" row in the title menu** opens the panel.
- **Borderless window**, in F1 > Mods > Window, for the game's windowed mode.

For mod makers:

- `Website=` in `mod.ini`, when it is the mod's GitHub repository, is where CleriaOre looks for updates: the
  latest release's tag (`v1.2.0` or `1.2.0`) against the mod's `Version`, and the release's `.cleriamod`.
- New on the original game: `game.skip_cutscenes`, `cleria.script.stop`, `ore.menu` and `ore.prompt`, save
  profiles, a mod's own New Game. See `docs/STATUS.md`.

## 1.0.0

The first release: one program that installs the loader and manages mods, and the F1 panel in the game.
