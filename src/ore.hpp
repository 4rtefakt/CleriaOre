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
#include <filesystem>
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
cleria::script::Host& host();          // host.cpp: the retail game as a script reaches it
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
// For the overlay (ui.cpp, the game thread):
std::vector<cleria::mods::Mod>& mod_list();                           // every mod found, in load order
std::vector<std::unique_ptr<cleria::script::Mod>>& scripts();         // the started scripts
void save_prefs();                                                    // mod_list()'s order and On / Off -> cleriaore.ini
// A mod's settings, cleriaore.ini [cleria.mod.<id>] (cleria.settings):
std::optional<std::string> setting(const std::string& mod, const std::string& key);
bool set_setting(const std::string& mod, const std::string& key, const std::string& value);

}  // namespace ore
