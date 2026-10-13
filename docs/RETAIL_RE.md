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

## The player

- **An actor's animation state is the object embedded at +0x8f0** (`FUN_00446220` and the draw `FUN_00446340`
  run on it with `ecx = actor + 0x8f0`): the sheet at +0x8c (actor +0x97c), the animation tag at +0xc0
  (+0x9b0), the frame at +0xc8 (+0x9b8). Not +0x150, which the decompiler shows as the first argument.
  Driven as Hugo: sheet 16; tag 2 standing, 266 walking, 256 attacking, the frame cycling 0..7. His Eyes
  (cast 176, kind 1, in the actor list) read tag 2 or 3 and sheet 255. Backs `player.pose`, `familiars`.
- **The motion constants live in the player's actor**, copied from CASTINFO when it is built (each room): the
  speed base at +0x30c, the jump's first velocity at +0x408. Scaling them is enough. Driven in S_1099: 0.7 s
  of walking covered 6.92 units, 14.41 at a factor of 2, 3.46 at 0.5; a held jump peaked 2.68 above the
  floor, 8.2 at a factor of 1.5, 0.53 at 0.5. Backs `player.tune` (speed, jump).
- The WASD preset's keys, for the drive: jump is the right mouse button (`101`), attack the left (`100`).

## Actors: holding, drawing, puppets

- **Every actor is updated through one function**, `FUN_0045a2f0` (the third entry of GoActor's vtable
  `0x69ba3c`): a jump to the actor's handler, by its index at +0x120, in the table at `0x69b8c0`. Hooked: an
  actor a mod holds is not updated.
- **A handler hands its own actor to the frame's draw list.** An actor whose handler is skipped is frozen
  and also invisible. `FUN_00473540(ecx = actor)` is the drawing half alone (what a handler itself calls on
  a step that is not a real one); the hook calls it for a held actor. It draws between the previous
  position (+0x3d4) and the current one (+0x50), so both are written when an actor is moved. Driven in
  S_1001: nine monsters held, frozen and visible; moved by `world.apply`; released, they act again.
- **An actor's instance id (+0x14) is its class in the high byte and a counter in the rest**, restarted with
  each room: the player 0x01000001 after a load, the room's monsters then 9, 11, 13... in the script's
  order. It is what `world.actors()` gives as `id` now (it was the record's address).
- **The sheet of an animation state can be changed freely, and the game loads a sheet when it is first
  drawn.** Writing +0x97c on a held monster drew it as Hugo (16), Yunica (15) and Toal (17) while playing
  Hugo, in a room that had never shown them.
- **The two name tables.** ASO (animation scripts): 185 names at `0x68a510`, indexed by CASTINFO +0x68
  (which the factory copies to the actor's +0x24c and +0x978). AIA (sheets): 269 names at `0x68a800`,
  indexed by the animation state's sheet. A cast's sheet is its ASO name looked up in the AIA table: cast
  14 is ASO 13 `m_100`, sheet 18; cast 19 sheet 23; Hugo's Eye, cast 176, ASO 175 `m_900`, sheet 255.
  CASTINFO is at `DAT_0074c11c`, 0x200 a record, its count at `DAT_0074c100`, a record's name at +0 (not a
  file name).
- **The monster factory builds any cast in any room** (`FUN_00475710`): cast 14 and 19 in S_2013, which has
  no monster. `world.spawn` no longer asks for a cast the room holds. Building an Eye (cast 176) made the
  game add two more live Eyes; building the player's own cast (13) made nothing else, and the player
  pointer stayed.
- **A puppet** is therefore an actor built as the player's cast, held from its first step, neither solid
  nor an enemy (kind bits 0x80 and 0x8 cleared), with its sheet, tag and frame written by the mod. Driven
  in S_2013: Yunica and Toal beside Hugo, one of them led round the player every frame, an Eye prop and a
  monster prop; `familiars` still 2, `actors` 0; gone with the room. Backs `world.puppet`, `set_puppet`,
  `remove_puppet`, `prop`.
- `world.apply` does not build a monster that the snapshot has and the room has not (an id cannot be given
  to a new actor), and a monster it lets fall drops its items.

## The world's filters

- **"spawn" is asked after the fact.** The loader does not hook the factory: each step it looks for monsters
  in the actor list it has not seen in this room, and puts each through the filter once. The answer's
  multipliers are applied to the built actor (max HP +0x94, HP +0x98, ATK +0xa4, DEF +0xa8, EXP +0xb0, SP
  +0xc4 as an int); "suppress" removes it at the game's next sweep. A monster therefore lives one step with
  its own numbers, and the filter cannot change its cast or its level. Driven in S_1001: seven of cast 14 at
  180 HP and ATK 76 (60 and 38 without), the two of cast 19 gone, `enemy_spawned` seven times, and an eighth
  for `world.spawn`.
- **"exit" is asked in the scene request itself** (`FUN_00430ef0`, hooked at its entry), for a change from one
  `S_nnnn` room to another. The request carries a scene id only: which door asked is not known there, so
  `index` is -1 and `door` empty. Driven: `goto_room("S_1001")` sent to S_1099 by the filter; a change to
  S_2099 refused, the game staying in play.

## What a hit carries

- **The object that hits is the appliers' first argument** (ebx in `FUN_004fbb40`, kept at `[ebp-0x20]`; edi in
  `FUN_004faf20`): its ATK at +0xa4, the hit's power at +0xfc (the formula's added term, xmm2), a floor at
  +0x100 (xmm3), the damage class at +0x109. A "damage" query now carries `atk`, `power`, `class`, the
  victim's `hp` and `max_hp`, and for the monster of the two `cast`, `level` and `actor`.

## The blessing shop

- **A blessing's effect** is the script VM's 0xAF. At `0x568d5d` its index is in eax, before the jump table
  (34 entries) that gives it; every case ends at `0x5664c9`. Hooked there: `blessing_bought`, and the shop
  filter's Buy answer (another blessing's effect, or none). The SP was taken earlier by the script's own
  flag operations, so the price is not known at this site, and the Price, Row and Owned queries are not
  asked. Driven in S_2013: "Strengthen current leggings" bought at the statue, reported as blessing 8 when
  the "Your prayers have been answered" box was dismissed.

## A kill's EXP

- The kill block of `FUN_004fbb40` builds the award as: the monster's base (+0xb0) scaled by the levels' gap
  (`FUN_004fa2f0`), times the EXP rate `0x76a5fc`, times the difficulty's factor (`[0x6bd464]` when
  `0x76bb58` is set, `[0x6bd580]` when `0x76bc48` is), floored at 1; then times the kill chain's factor,
  `1 + [0x76c1a8] * [0x6bd084]`. The "exp" query carries these as `rate`, `factor` (the mods' own share of
  the rate), `diff_mult` and `chain`.

## Bullets

- **The bullet factory** `FUN_00593550` is `fastcall(ecx owner, edx kind)` with 17 stack words that the caller
  pops: the effect id, &pos, &dir, f0..f12 (speed, accel, gravity, power, knock, three ints, five floats) and
  an out pointer. Hooked at its entry: a bullet whose owner is the player's actor raises `player_bullet`.
  Driven as Hugo in S_1099: kind 2, effect 130, speed 0.3, the third int 1, from 0.4 under the player's
  height, three shots three events.
- `world.show_bullet` calls the factory again with the player as owner and no power or knock. Driven: five
  copies of a recorded shot flew side by side, and raised no event of their own. A shown bullet is still the
  player's, so where it meets a monster it lands a hit of 0.

## Equipment, ailments, music, the fader

- **Equipment.** The weapon, armor and leggings are g_flags 151..153; the accessory, the held item and the
  bracelet are three ints at `0x76a628`, `0x76a62c`, `0x76a630` (-1 none). Driven as Hugo: weapon 1, armor 13,
  leggings 36, accessory 55, bracelet 116, held 84. `equip` writes them and runs the game's recompute; not
  driven.
- **Ailments.** The active mask is at `0x76c15c` (poison 1, heavy 8, confuse 0x10, curse 0x20), the four timers
  from `0x76c160`, in ticks. Setting the bit and the timer is enough for the game to run the ailment: driven
  in S_1099, poison took 1 HP about every second and its timer counted down from 900; a timer written to 0
  ends it at the next tick. A goddess statue's room clears them at once (set in S_2013, they were gone 1.5 s
  later). The hit's own show (effect, sound, popup) is not made.
- **Music.** `FUN_00432ee0(ecx id, edx 0)` is a script's Bgm_Play (-1: the room's own), `FUN_00432fc0(ecx
  fade)` Bgm_Stop. Called without a crash; what plays was not checked by ear.
- **The fader** is plain memory: the two colours at `0x769f30` / `0x769f34`, the current alpha at `0x769f1c`,
  the target at `0x769f24`, the step a frame at `0x769f28`. Driven: a purple veil to 0.6 over half a second,
  then back to 0.

## Sessions

- **Retry and Return to Title are two functions without arguments**, the game-over menu's rows: `0x4276b0`
  (the continue snapshot back over the save block, and the scene it was taken in) and `0x427750`. Each starts
  its own fade-out and scene change. Driven: killed in S_1099, Retry brought the game back to S_2013 in play
  at full HP; Return to Title gave the title.
- **Loading a slot** is the Load page's "Yes": the slot byte at `0x74ac3e`, then `FUN_0042f5e0("yso_NN.bin")`,
  which reads the save and stages its scene itself. The file name is bare; the game resolves it (the Steam
  Cloud included). Driven from play (S_1001 to the save's S_2013) and from the title.

## The camera

- **Shake** (a script's 0xCF) is three float amplitudes at `0x76a594` and an int frame count at `0x76a5ac`.
- **The camera's subject** (Scroll_MoveTo 0xC3, Scroll_Return 0xC4) is the block `0x76a1c0..0x76a1f4`, which
  `FUN_0056ec40` and `FUN_0056ecd0` only fill: the target point at `0x76a1d0`, where it starts from at
  `0x76a1dc` / `0x76a1e4` (copied from the live target `0x76a160` / `0x76a168`, or from the saved default
  `0x76a1c4` / `0x76a1cc` to go back), the step at `0x76a1f0`, the progress at `0x76a1ec`, the direction at
  `0x76a1e8` (1 to the point, 0 back), a done byte at `0x76a1c0`.

- **The frame's view is built from the camera the game publishes each step** (the subject `0x76a160`, the eye
  `0x76a16c`, the field of view `0x7b38f4`, in radians): what is written there after the step is what the
  frame is drawn with. `camera.set` is a layer applied to those values after each step (the eye's distance
  from its subject and the field of view scaled, both points moved). Driven in S_2013: the eye 30.00 from
  its subject, 60.00 at a distance of 2 with the whole room in view, the field of view 0.454 to 0.272 at
  0.6, both back after `reset`.

## Effects

- **`FUN_004496f0(actor, args)`, stdcall,** is an animation script's anime_GenerateEffect: ten words, the
  effect id, then as floats the distance ahead, the angle, the height and the scale, then the attach mode
  and four words the factory takes as they are. Hooked: the player's raise `player_effect`. `puppet_effect`
  calls it again with a puppet as the actor. Driven as Hugo in S_1099: his skill (the middle mouse button,
  `102`) raised effects 155 and 134; replayed on a Yunica puppet, both showed on her. His plain shots raise
  none (they are bullets).
- **A script is started** by `FUN_00434970(ecx name, edx 0; an argument block, a parent)`, the caller popping
  the two: a script's StartScript (0xb4). `script.run` calls it; not driven.

## Doors

- **A room's doors are the RegionEntry pool**: `DAT_0076a2b4`, `DAT_0076a2b8` records of 0x2a0. A live one has
  its byte +4 set; +0x38 & 2 means shut; the box is at +8..+0x1c round the point +0x20; +0x3c is the scene it
  leads to and +0x44 the 0x194-byte descriptor of the arrival. Driven in S_2013: two doors, to S_2004 and
  S_2080. Backs `game.exits`.
- **Taking a door** is what the trigger `FUN_0042cf00` does when the player stands in one: the fade's target
  to 1 (`0x769f24`), its current alpha and hold to 0, the door's scene into `0x769fb8` and `0x76a538`, its
  descriptor into `0x769fbc`. The fade's end commits the change. `game.take_exit` makes those writes, without
  the walk through the doorway.

## The look

- **An actor's scale is the float at +0x248**, the last argument its draw (`FUN_004740e0`) hands on. Driven in
  S_1001: the player at 1.6, a monster at 2 and another at 0.6, each drawn at that size, the running ones
  included (a handler does not write it back).
- **A sprite's colour is rebuilt every step** by `FUN_00473c60`, into +0x148: writing +0x148 itself is undone
  at once (it read `ffffffff` again 0.6 s after a poke). What that function adds is the actor's own light:
  the ARGB at +0x2b4, its alpha the strength, added to the lit colour (it is what an animation script's
  anime_SetAura writes), and three signed bytes at +0x4bb..+0x4bd, a negative one darkening its channel, a
  positive one adding to it. Driven: `c0ff2020` at +0x2b4 turned Hugo glowing red, and it stays until
  written again; in S_1001 the player green, monsters red and blue. Backs `player.set_look`'s `glow`,
  `world.form`'s `tint`, a puppet's `glow`, and the `glow` of `player.pose`.

## The HUD

- **The whole HUD has one fade**: the float at `0x74e688`, which the step moves by 0.1 toward its target
  (`0x428660..`), and the tint `0x74e68c` it makes from it (white, the alpha from the fade), which every HUD
  quad is drawn with. Writing both after each step (the fade 0, the tint `0x00ffffff`) keeps the HUD out of
  the frame; let go, the game fades it back in ten steps. There is no separate switch for the status part,
  the boss gauge or the messages.

## Events the loader raises itself

- `enemy_hit` and `player_hit` come from the two damage hooks, past the filters; `level_up` from the level
  in the stat record rising; `retry` from the game's own retry count (g_flags 196) rising, so the menu's
  Retry raises it too; `floor_changed` from the floor in the room's name. Driven in S_1001 and S_1009: all
  but `retry` seen with the right numbers, and a monster variant defined in Lua (four times the HP, a phase at
  half, `on_hit`, `on_death`) ran its callbacks.

## A kill by a script

- `combat.damage` that takes a monster under 1 HP now credits it as the applier's kill block would: the EXP
  as `max(1, floor(base * gap * 0.6666 * rate * difficulty)) * chain`, the gap 1.5 to the power of the
  levels' difference (at most three levels up), through the "exp" filter; the SP through the game's own
  scaling (`FUN_004fa390`) and the "sp" filter; `enemy_killed`; and the monster's once-only credit bit.
  Driven in S_1001: two kills of cast 14 (level 1 against the player's 11) gave 1.00 EXP and 4 SP each, the
  same as a kill by a shot. [H] the gap's formula is CleriaCore's reading of `FUN_004fa2f0`, not re-derived.

## A new game

- The title's Difficulty popup starts a game by writing the character (flag 150) and the difficulty's cells
  (`0x74ac3c` 0 / 1 Hard / 2 Nightmare, `0x74e685` easy, `0x74e684` very easy), calling the new-game init
  `FUN_0041fd10`, and finishing the title toward the scene `s_00\s_0000`. The title object's own tick commits
  that scene at the end of its fade; from outside, the staged scene alone does nothing (the game stayed on the
  title), and asking for the scene (`FUN_00430ef0`, forced) does it. Driven from the title: the game went to
  S_0000 and asked its first question, "SP Bonus Acquisition". Backs `game.new_game`.

## Study commands of the drive

- `peek <addr> [n]` logs n dwords (as hex and as floats) and `poke <addr> <value>` writes one; `p+<offset>`
  addresses the player's actor. How the glow was found: poke, look, peek again.

## A script's text

- **A script file (XSO) can be rewritten as the game loads it.** The file is a header (the code's length in
  words at +0x1c, the string count at +0x20), the code, one offset a string, and the string pool, which ends
  the file (2,225 files checked: nothing follows the pool but up to three bytes of padding, and no other
  field of the header measures it). The game asks for every script through the archive lookup the loader
  already hooks (`data\map\s_10\s_1009\SAVE.xso.z`); for a script a mod's `text*.ini` names, the game's own
  file is read through the original functions, its pool written again with the new strings, packed, and
  served. Driven with the Chest Swap sample: S_1009's statue script, four strings, one replaced. The game
  asks for the statue scripts of many rooms as it starts, so a text change is in place before the room is
  entered. Not seen on screen: the line is the one after a statue's cleanse.

## A chest's redirect, seen

- `script.run("S_BOX01")` in S_1001, with the chest's flag put back to 0, runs the chest as opening it does. With
  the Chest Swap sample the whole chain was seen for the first time on the retail game: the "Acquired" box
  naming the mod's item, the rule's card ("The Lantern of Solace glows softly."), no icon rising from the
  chest, and the mod's own picture (the item's `Icon=`, a DDS among the mod's files) in a frame beside the
  card. A mod's picture for an icon id of 0x200.. (`cleria.ui.icon`) is shown the same way.

## Monsters and a puppet: tried, not kept

- ENEMYAI.md gives a monster a target override at +0x888 (the player when 0). Writing a puppet's actor there
  for every monster of S_1001 (casts 14 and 19), with the puppet a step from one of them and the player far
  off, moved nothing in eight seconds: these brains find their prey by the player's own pointer and distance.
  Read again in the decompiler's output: 75 functions read +0x888, most monster brains among them (M120,
  M140, M160 ... by `names.csv`), and they take from it only the other actor's heading (+0x308) and height
  (+0x318), with the player as the fallback; the distance they act on is still measured to the player. It is
  not a target to chase. A puppet stays out of the monsters' reach.

## Music files

- **The game streams its music from plain files**, `release\music\<name>.ogg`, opened through Windows and not
  through the archive, `<name>` from a table of 0x14-byte records (`DAT_0074c118`, `DAT_0074c114` of them: 16
  characters of name, then the id at +0x10). The game's own imports of `CreateFileA` and `CreateFileW` are
  pointed at the loader's (kernel32 itself is not patched): a file of that folder
  that a mod provides (`files/music/YSO_004.ogg`) is opened in its place. Driven: with such a file in a mod,
  entering S_1001 (music 4) logged the redirect.
- `audio.music("file.ogg")` plays any Ogg file of a mod the same way: the record of music id 30, which no
  room uses, is given a name of the loader's, and that name leads to the mod's file. Driven: the game opened
  the mod's file. Neither was checked by ear.

## The blessing shop's prices

- **Every integer operand of a script is fetched through one function**, `FUN_00566190`: thiscall(script; a
  context, the sub-op, a word, the operands, the operand's index), answering a pointer to the value;
  `FUN_00566100` is its twin for strings. Hooked: answering a pointer to another value changes what a script
  reads without touching the script.
- A blessing's script `@GrowNN` (its name at script +0x124) tests the SP against its price with 0x61 (flag
  216, 6, price) and takes it with 0x69 (flag 216, price); the menu's script `@GrowMenu` shows it with 0xdd
  (text, the blessing's script as a label, price). The armor's and the leggings' scripts (`@GrowAr`,
  `@GrowLe`) put their price in flag 218 with 0xb0 and then compare (0x62: 216, 6, 218) and take by that
  flag. The mods' shop filter is asked the price at each of these. Driven in S_2013 with a filter answering
  50: every row of the menu read "[SP:]50". Then two purchases: the armor's at 50 in place of 300 and the heal rate's at 50 in place of 1,000, the SP going 2,952 to 2,902 to 2,852, and `blessing_bought` carrying the 50.
