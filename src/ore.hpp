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
float on_exp(float value, int source, const void* victim);    // -> the EXP awarded; source 0 a kill, 1 a boss
void on_kill(const void* victim, float* sp);                   // a monster died of the player's hit; *sp = the SP it gives, before the game's scale
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
// For the overlay (ui.cpp, the game thread):
std::vector<cleria::mods::Mod>& mod_list();                           // every mod found, in load order
std::vector<std::unique_ptr<cleria::script::Mod>>& scripts();         // the started scripts
void save_prefs();                                                    // mod_list()'s order and On / Off -> cleriaore.ini
// A mod's settings, cleriaore.ini [cleria.mod.<id>] (cleria.settings):
std::optional<std::string> setting(const std::string& mod, const std::string& key);
bool set_setting(const std::string& mod, const std::string& key, const std::string& value);

}  // namespace ore
