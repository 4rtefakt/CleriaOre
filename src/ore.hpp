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
bool stop_filtered_script();           // hooks.cpp: the script whose store a filter answers ends after it; false: none
void set_cutscene_skip(bool on);       // hooks.cpp: every cutscene's waits end at once, the intro movies are not opened
// hooks.cpp, the game thread: a file as the game reads it ("data\map\s_10\s_1001\s_box01.xso.z"; a mod's
// override, else the game's archive), a ".z" unpacked. False: there is none.
bool game_file(const char* path, std::vector<std::uint8_t>& out);
bool game_file_own(const char* path, std::vector<std::uint8_t>& out);   // the game's own file, unpacked: no mod's override
// content.cpp: text*.ini. Whether any is loaded, and a script's bytes rewritten with its new strings (`name` as the
// game's path under data\, without ".z"); -> the number of strings replaced.
bool content_text_on();
int content_patch_script(const std::string& name, std::vector<std::uint8_t>& xso);
void set_exp_factor(float f);          // hooks.cpp: the game's own EXP rate, which every award is multiplied by
// Combat, asked by hooks.cpp as the game applies it (the game thread). An actor is the game's own record.
// -> the damage that leaves the HP. `source` is the object that hit (the formula's first argument: its ATK at +0xa4,
// the hit's power at +0xfc, its class at +0x109); for a hit dealt, `attacker` is the player it is credited to.
int on_damage(bool dealt, int value, const void* victim, const void* attacker, const void* source);
// A bullet is being made (FUN_00593550): `args` are its 17 stack words (the effect, &pos, &dir, f0..f12, out).
void on_bullet(const void* owner, int kind, const std::uint32_t* args);
void on_effect(const void* actor, const std::int32_t* args);                      // an animation script makes an effect (FUN_004496f0)
bool puppet_effect(void* actor, const double* n);                                // hooks.cpp: (id, fwd, angle, voff, scale, attach) on that actor
void* aura_effect(void* actor, int id, int* made_id);                                          // hooks.cpp: an effect that stays on that actor; -> it
bool fx_live(const void* fx, int id);                                             // hooks.cpp: that effect still runs
void fx_end(void* fx, int id);
bool play_music_file(const std::string& file);                                   // hooks.cpp: an Ogg file of a mod's as the game's music
bool load_slot(int slot);                                                        // hooks.cpp: the Load page's own read of a save, 0-based
bool show_bullet(const double* n);                                               // hooks.cpp: the 19 numbers of a player_bullet event, as a bullet that hurts nobody
void script_kill(void* victim);                                                  // a script's hit killed this monster: its EXP and SP, as a kill of the player's
bool shop_on();                                                                  // some mod has a shop filter
// The Price query of a blessing: `of` names it ("@Grow03": the running script, or the menu row's label).
int on_shop_price(int sub, const char* script, const char* of, int price);
const char* on_shop_row(int sub, const char* script, const char* label, const char* text, int price);
int on_shop_owned(const char* script, int index);
bool on_script(const char* name, const void* owner, std::uintptr_t from);         // a script starts by name; false = refused
bool on_skill(const void* player);                                               // a skill is about to be cast; false = refused
void on_skill_done(const void* player, std::uint32_t serial_before);
void on_jump(const void* player);
int on_blessing(int index);                                                    // a blessing's effect is about to be given: -> the index, -1 = none
int on_part_damage(int value, const void* victim, const void* source);           // the same for a hit on a map object (a boss's part)
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
const std::string& save_profile();     // the save profile, "" = the game's own saves
bool set_save_profile(const std::string& name);   // false: not a usable name
bool save_name(const char* base);      // hooks.cpp: a save file's name (yso_NN.bin, yso_NN.nacci)
bool profile_storage();                // hooks.cpp: Steam's storage follows the save profile; false: there is none (yet)
bool on_title_item(const char* item);   // a row of the title's list was chosen; false = a mod's "title_<item>" command took it
void title_run();                      // hooks.cpp: the title's own New Game, chosen at the next step, past the mods
void title_tick();
// hooks.cpp: a menu of the game's own (its caption, up to 16 rows, the row the cursor starts on, its height on the screen); a pick calls
// on_menu_pick with the row, 1-based, and Esc picks the last row.
bool game_menu(const std::wstring& caption, const std::vector<std::wstring>& rows, int cursor, float y);   // y: the first row's top, of 768
void game_menu_close();
void game_prompt_text(const std::string& text);        // hooks.cpp: the first row of the menu now up
void on_prompt_done(bool kept, const std::string& text);   // a prompt ended: the mod's command "menu_<id>" gets "ok <text>" or "cancel"
void on_menu_pick(int row);
void menu_owner(const std::string& id);   // the menu now up is this one: its picks go to the mods' command "menu_<id>"
void open_toal();                      // hooks.cpp: Character Select offers Toal for the rest of this run
int latest_slot();                     // hooks.cpp: the most recent save's slot (of the save profile), -1 = none
bool steam_delete(const char* name);   // hooks.cpp: for the drive's tests
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
void set_skip(const std::string& mod, bool on);      // a mod's game.skip_cutscenes
bool paused();                                       // some mod holds the game
void set_speed(const std::string& mod, double factor);
double speed();                                      // the product of the mods' factors, 0.1 .. 4
// cleria.combat.boss_bonus: damage added to the player's next hit on the boss gauge's owner. n > 0 adds, 0 empties,
// < 0 only reads; -> the pool.
double boss_bonus(double n);
// cleria.player.tune: a mod's factor of the player's speed (0) and jump (1); the product of the mods'.
void set_tune(const std::string& mod, int key, double factor);
double tune(int key);
// cleria.hud.show: the parts a mod hides (1 status, 2 boss gauge, 4 messages).
void set_hud_hidden(const std::string& mod, unsigned bits);
unsigned hud_hidden();
void cam_tick();                                     // host.cpp: the mods' layer over the camera the game publishes
void tune_tick();                                    // host.cpp: the player's actor follows, each step
// cleria.world.add_filter: whether a mod has a filter of that kind, and the first answer that is not "pass".
bool world_on(const char* kind);
cleria::script::WorldDecision world_filter(const cleria::script::WorldQuery& q);
void emit_named(const cleria::events::Event& e);     // an event of API 7, to the mods
int exit_filter(int scene);                          // host.cpp: the mods' "exit" filter on a room change; 0 = cancelled
void world_tick();                                   // host.cpp: the monsters built since the last step ("spawn", enemy_spawned)
int puppet_id(const void* actor);                    // host.cpp: the id of a mod's puppet, 0 for any other actor
bool actor_held(const void* actor);                  // host.cpp: a mod holds this actor's handler (world.form ai = false, world.apply)
// For the overlay (ui.cpp, the game thread):
std::vector<cleria::mods::Mod>& mod_list();                           // every mod found, in load order
std::vector<std::unique_ptr<cleria::script::Mod>>& scripts();         // the started scripts
void save_prefs();
void save_order(const std::vector<std::string>& ids);   // the load order as the panel changed it: kept for the next start                                                    // mod_list()'s order and On / Off -> cleriaore.ini
// A mod's settings, cleriaore.ini [cleria.mod.<id>] (cleria.settings):
std::optional<std::string> setting(const std::string& mod, const std::string& key);
bool set_setting(const std::string& mod, const std::string& key, const std::string& value);

}  // namespace ore
