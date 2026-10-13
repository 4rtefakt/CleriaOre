// CleriaOre: CleriaCore's mod layer (cleria-modkit) hosted inside the retail yso_win.exe.
//
// Everything of CleriaOre lives in <the game folder>/cleriaore/:
//     cleriaore.ini    [cleria.mods] UseMods / Order / Active and a mod's settings [cleria.mod.<id>],
//                      as in CleriaCore's cleria.ini
//     fonts/           the overlay's fonts (CleriaCore's Fira Sans)
//     cleriaore.log    this session's log
//     mods/            the mods: folders, .zip and .cleriamod (CleriaCore's samples/mods/README.md)
//     saves/           yso_NN.cleria: what the mods keep with the save yso_NN.bin (cleria::Sidecar)
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cleria/mods.hpp"
#include "cleria/modscript.hpp"
#include "cleria/sidecar.hpp"

namespace ore {

const std::filesystem::path& dir();    // <the game folder>/cleriaore
void log(const char* fmt, ...);        // one line into cleriaore.log
bool install_hooks();                  // hooks.cpp: false = not the yso_win.exe build the addresses are of, nothing hooked
void start(bool hooked);               // load the mods, start their Lua scripts (once, off DllMain)
void wait_started();                   // returns once start() is done
bool hooked();                         // hooks.cpp: the hooks are in (this process is the game)
// hooks.cpp, the game thread: a file as the game reads it ("data\map\s_10\s_1001\s_box01.xso.z"; a mod's
// override, else the game's archive), a ".z" unpacked. False: there is none.
bool game_file(const char* path, std::vector<std::uint8_t>& out);
void set_exp_factor(float f);          // hooks.cpp: the game's own EXP rate, which every award is multiplied by
// Combat, asked by hooks.cpp as the game applies it (the game thread). An actor is the game's own record.
int on_damage(bool dealt, int value, const void* victim, const void* attacker);   // -> the damage that leaves the HP
int on_part_damage(int value, const void* victim);                               // the same for a hit on a map object (a boss's part)
float on_exp(float value, int source, const void* victim);    // -> the EXP awarded; source 0 a kill, 1 a boss
void on_kill(const void* victim, float* sp);                   // a monster died of the player's hit; *sp = the SP it gives, before the game's scale
bool on_drop(int* item, const float* pos);                     // a kill's drop that passed its roll; false = nothing drops
bool on_pickup(void* pickup);                                  // the player touched a pickup; false = it is taken and the game grants nothing
// hooks.cpp: a pickup thrown up as an enemy's drop is; `resting`: one that lies where it is put and stays.
bool spawn_pickup(int item, const float* pos, bool resting = false);
// content.cpp: the active mods' content/*.ini (CleriaCore's CONTENT.md).
void content_start();                                          // read them (after the mods are loaded)
bool content_on();
cleria::GrantDecision content_grant(const cleria::GrantQuery& q, const std::string& room);   // chests.ini's answer
bool content_give(int id, int count, const std::string& source, const std::string& script);  // a new item (0x100..); false: not one
std::string content_item_name(int id);                         // a patched or new item's name, "" = none
void content_pickup_taken(int item, const float* pos, const std::string& room);
cleria::script::Host& host();          // host.cpp: the retail game as a script reaches it
const std::map<std::string, std::pair<int, std::string>>& chests();   // the room's: script -> {box flag, object}
void on_tick();                        // one step of the game's 60 Hz simulation ran (the game thread)
void on_frame();                       // one frame was rendered (the game thread)
// A grant of the game's script VM, before it happens (hooks.cpp fills `q` but its flags): the mods'
// filters answer in load order, and a store that passes with a higher item count raises item_obtained.
cleria::GrantDecision on_grant(cleria::GrantQuery q);
// The sessions (the game thread; `file` is the save's name as the game passes it, "yso_05.bin"):
void on_new_game();                    // the title started a game: the flags were just reset
void on_loaded(const char* file);      // a save was read into the game
void on_saved(const char* file);       // a save was written
cleria::Sidecar& sidecar();            // what the mods keep with this session's save
bool session_pending();                // a new game / a load whose event was not delivered yet
int room_ticks();                      // the steps since the room was entered
// What the scripts share (the game thread):
void on_input(const std::string& name, bool pad);    // ui.cpp: a key or a pad button went down, by SDL's name for it
void request_reload(const std::string& mod);         // its script starts afresh at the next frame
const std::vector<std::string>& log_tail();          // the last lines the scripts logged
void log_script(const std::string& mod, const std::string& text);   // one of them, into cleriaore.log too
cleria::script::Mod* window_mod();                   // the first mod with a window of its own open, or null
// The stat modifiers (cleria.player.set_modifier), applied by hooks.cpp at each of the game's recomputes:
// `stat` 0 max HP, 1 ATK, 2 DEF.
void set_modifier(const std::string& mod, int stat, double add, double mul);
std::pair<double, double> modifier(const std::string& mod, int stat);
float modified(int stat, float base);                // (the game's + the mods' adds) x their muls, within the game's limits
float base_stat(int stat);                           // hooks.cpp: the game's own value, before the mods
void recompute_stats();                              // hooks.cpp: the game's recompute, now
// A mod's hold on the game (game.pause) and its factor of the game's speed (game.speed), per mod.
void set_pause(const std::string& mod, bool on);
bool paused();                                       // some mod holds the game
void set_speed(const std::string& mod, double factor);
double speed();                                      // the product of the mods' factors, 0.1 .. 4
// cleria.combat.boss_bonus: damage added to the player's next hit on the boss gauge's owner. n > 0 adds, 0 empties,
// < 0 only reads; -> the pool.
double boss_bonus(double n);
// cleria.player.tune: a mod's factor of the player's speed (0) and jump (1); the product of the mods'.
void set_tune(const std::string& mod, int key, double factor);
double tune(int key);
void tune_tick();                                    // host.cpp: the player's actor follows, each step
// For the overlay (ui.cpp, the game thread):
std::vector<cleria::mods::Mod>& mod_list();                           // every mod found, in load order
std::vector<std::unique_ptr<cleria::script::Mod>>& scripts();         // the started scripts
void save_prefs();                                                    // mod_list()'s order and On / Off -> cleriaore.ini
// A mod's settings, cleriaore.ini [cleria.mod.<id>] (cleria.settings):
std::optional<std::string> setting(const std::string& mod, const std::string& key);
bool set_setting(const std::string& mod, const std::string& key, const std::string& value);

}  // namespace ore
