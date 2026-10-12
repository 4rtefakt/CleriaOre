// CleriaOre: CleriaCore's mod layer (cleria-modkit) hosted inside the retail yso_win.exe.
//
// Everything of CleriaOre lives in <the game folder>/cleriaore/:
//     cleriaore.ini    [cleria.mods] UseMods / Order / Active, as in CleriaCore's cleria.ini
//     cleriaore.log    this session's log
//     mods/            the mods: folders, .zip and .cleriamod (CleriaCore's samples/mods/README.md)
#pragma once
#include <filesystem>

namespace cleria::script { struct Host; }

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

}  // namespace ore
