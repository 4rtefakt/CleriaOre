// The mods' file overrides (a mod's files/ folder, cleria::vfs) served to the retail game's archive reader.
//
// The game reads every packed file in two steps (yso_win.exe, the 2024-01-08 build):
//   FUN_005defa0  entry = lookup(archive, path)   path as the game spells it: "data\map\s_10\x.dds.z";
//                                                 the entry is a 16-byte record of the archive's index
//   FUN_005df220  read(reader {entry, offset}, dst, n, got)
// and a ".z" file is a u32 crc32 and a u32 unpacked size, then a zlib stream (FUN_005eeae0 inflates it).
// lookup() answers an entry of our own for a path a mod provides, and read() serves the reads on it, so
// the game's loaders run unchanged. hooks.cpp puts the two in front of the game's functions.
#pragma once
#include <cstdint>

namespace ore::files {

struct TocEntry {             // the game's index record
    std::uint32_t hash = 0;
    std::uint32_t size = 0;   // the bytes a reader may read
    std::uint32_t pos = 0;
    std::uint32_t namepos = 0;
};

// The entry of the mod file that replaces `path`, or null: no mod provides it and the game's own lookup
// answers. A mod provides files unpacked, under the name without ".z"; when the game asks for the ".z"
// the file is packed here.
TocEntry* lookup(const char* path);

// A read of up to `n` bytes at `offset` of an entry lookup() returned; `offset` advances by `got`.
// False: `e` is not one of ours.
bool read(const TocEntry* e, std::uint32_t& offset, void* dst, std::uint32_t n, std::uint32_t& got);

}  // namespace ore::files
