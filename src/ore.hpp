// CleriaOre: CleriaCore's mod layer (cleria-modkit) hosted inside the retail yso_win.exe.
//
// Everything of CleriaOre lives in <the game folder>/cleriaore/:
//     cleriaore.ini    [cleria.mods] UseMods / Order / Active, as in CleriaCore's cleria.ini
//     cleriaore.log    this session's log
//     mods/            the mods: folders, .zip and .cleriamod (CleriaCore's samples/mods/README.md)
#pragma once
#include <filesystem>

namespace ore {

const std::filesystem::path& dir();    // <the game folder>/cleriaore
void log(const char* fmt, ...);        // one line into cleriaore.log
void start();                          // load the mods, start their Lua scripts (once, off DllMain)

}  // namespace ore
