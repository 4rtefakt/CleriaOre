# Retail notes

What was learnt about the running Steam game (`yso_win.exe`, linked 2024-01-08) while backing the mod API
on it, beyond what CleriaCore's notes already say. Each entry: what, where, and how it was checked.
"Driven" = checked in the running game through the test drive ([TEST_PROTOCOL.md](TEST_PROTOCOL.md)).
The notes themselves are gathered in the `yso` repository.

## Rooms

- **The scene table in memory.** `DAT_0074c124` points at SCENELIST.SL's records, `DAT_0074e230` is their
  count, 0x1c8 bytes each: the scene id at +0 (u16), the path (`s_10\s_1001\`) at +4, the display string at
  +0xc4. The display string is `"<id>: <name>"`; the name starts with the floor where the room has one
  (`4F Save (Forward)`, `2F Path 1`, `5F Velagunder`, but `Save Point`, `Boss Room`). Driven in S_2013,
  S_1009, S_1001, S_1099, S_2099. Backs `game.room_title`, `floor`, `zone`.
- **A room change.** `FUN_00430ef0(ecx = scene id, edx = force)` stages it; the next frame tears the room
  down and loads the new one. It is what a script's `ChangeScene` (op 0x97) calls, with force 0, and it
  answers non-zero while another change is pending. The player arrives at the room's default entry. The
  player's actor is rebuilt: what was written into it (the invulnerability timer) is lost. Driven: S_2013 to
  S_1001, to S_1099, to S_2099. Backs `game.goto_room`.
- A boss room whose boss is beaten in the save loads empty (S_1099); one not yet beaten starts its scene and
  the fight (S_2099).

## Hits

- **A monster's hit events are bits at actor +0x3bc**, left by the applier `FUN_004fbb40` for the monster's
  own brain: 4 struck, 2 hurt (damage above 0), 0x10 killed (with HP set to 0). Writing HP and these bits
  from outside is enough: the monster flinches, and at 0x10 it dies the game's way, with its drops (the
  drop hook fired). No EXP or SP comes of it: the applier credits those itself, in its kill block.
  Driven in S_1001 on cast 14: 60 to 55 HP, then two kills, 9 monsters to 7, gold dropped. Backs
  `combat.damage`.
- **The player's hurt invulnerability** is the int at player +0x734, in ticks, counted down by the mover.
  Driven: a second `combat.strike` in the same tick took nothing. Backs `player.set_invulnerable`.
- **The damage formula's first term** (`FUN_004fa550`): `atk * ((atk - def * [0x6bd414]) + [0x6bd928] + bonus)
  / [0x6bd9b0]`, floored at 0. `combat.strike` prices a hit on the player with it and takes at least 1.
  Not reproduced: the element table, the random +1, the launch factors, and the flinch and knockback, which
  are the hurt applier's (`FUN_004fd350`).
- **There are two appliers that call the formula.** `FUN_004fbb40` is the player's hit on a monster (a
  GoActor). `FUN_004faf20` is the player's hit on anything else, which it tells apart by the victim's class
  (its first virtual function): 0xd a map object (a boss's part, a breakable), 0x13 a sprite object. Its
  damage is in eax at `0x4fb0b3`, the victim in esi. Both are hooked now. Driven in S_2099: Hugo's shots on
  Nygtilger gave "damage dealt 3..4" on a part of DEF 98 and 0 on one of DEF 999.

## Bosses

- **The boss gauge** is widget 3 of the bar array, at `0x6d5fd4`: +4 the owner's instance id (an actor's
  +0x14; 0 = no gauge), +0x30 the HP shown, +0x38 the maximum. Driven in S_2099 (Nygtilger): 7000 / 7000.
  Backs `world.boss`.
- **A boss built of map objects is not an actor.** In the Nygtilger fight the actor list holds the player
  and Hugo's two Eyes (cast 176, kind 1) and nothing else. The gauge's owner id is `0x0D000007`: the high
  byte of an instance id is the object's class (0x01 an actor, 0x0D a map object), so the owner is a map
  object, in the room geometry pool (`0x730224`, count `0x730228`), not in `0x730098`. `world.boss().actor`
  is therefore nil for such a boss, and `world.actors()` does not list its parts.
- The gauge there reads 7000 / 7000 while CleriaCore's boss table gives Nygtilger 1200 HP. **Open:** what the
  gauge's owner sums.
- **The boss pool works through the part hook.** Driven: a pool of 3000, then shots; the pool was spent on
  the first hit above 0 and the gauge fell from 7000 to 6650. The game caps what a part can take (350
  there, [H] the part's own HP), so a pool larger than the part is partly lost.
- A boss's attacks respect the hurt invulnerability: with +0x734 set after the scene, the player stood in
  Nygtilger's lightning at full HP. Set before the room change, it is lost with the rebuilt actor.
