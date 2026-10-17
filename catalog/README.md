# The Get mods list

`mods.json` is the list CleriaOre's **Get mods** page shows. CleriaOre reads it from this repository's
`main` branch, so a change here reaches every player without a new CleriaOre.

Each entry is a mod with a GitHub repository whose latest release carries a `.cleriamod`:

```json
{"id": "author.mod", "name": "Shown name", "author": "Who", "description": "One paragraph.", "repo": "owner/repo"}
```

`id` must be the `Id=` of the mod's `mod.ini`: CleriaOre refuses a download that holds another mod.
