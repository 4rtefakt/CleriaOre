// ore-filestest: the file overrides (src/files) asked as the game's archive reader asks, on a scratch mod
// folder under %TEMP%. No game, no hook: lookup() and read() are called as hooks.cpp calls them.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cleria/vfs.hpp"
#include "files.hpp"
#include "miniz.h"

namespace fs = std::filesystem;
using namespace ore;

namespace ore {   // what files.cpp asks of the loader: here, no game and no text.ini
bool game_file_own(const char*, std::vector<std::uint8_t>&) { return false; }
bool content_text_on() { return false; }
int content_patch_script(const std::string&, std::vector<std::uint8_t>&) { return 0; }
}  // namespace ore

int main() {
    int failed = 0;
    auto check = [&](const char* what, bool ok) {
        std::printf("[files] %-78s %s\n", what, ok ? "OK" : "FAILED");
        if (!ok) ++failed;
    };

    // A mod's files/ folder: one texture-like file of 300,000 bytes that compresses, by its game path.
    const fs::path root = fs::temp_directory_path() / "cleriaore-filestest";
    fs::remove_all(root);
    fs::create_directories(root / "MAP" / "S_10");
    std::vector<std::uint8_t> want(300000);
    for (std::size_t i = 0; i < want.size(); ++i) want[i] = std::uint8_t((i / 7) * 31);
    std::ofstream(root / "MAP" / "S_10" / "TEST.DDS", std::ios::binary).write(reinterpret_cast<const char*>(want.data()), std::streamsize(want.size()));

    check("no mod: every lookup is the game's", files::lookup("data\\map\\s_10\\test.dds") == nullptr);
    cleria::vfs::set_layers({{"test.mod", root}});

    // The plain name, spelled as the game spells paths; read in two parts, the second past the end.
    files::TocEntry* e = files::lookup("DATA\\MAP\\S_10\\.\\Test.dds");
    check("the plain name is provided, with the file's size", e && e->size == want.size());
    std::vector<std::uint8_t> got(want.size() + 16);
    std::uint32_t off = 0, n1 = 0, n2 = 0, n3 = 0;
    check("a read on it is ours",
          e && files::read(e, off, got.data(), 100000, n1) && files::read(e, off, got.data() + n1, 0x7fffffff, n2) &&
              files::read(e, off, got.data(), 16, n3));
    check("the reads stop at the end of the file", n1 == 100000 && n2 == want.size() - 100000 && n3 == 0 && off == want.size());
    check("the bytes are the mod's", std::memcmp(got.data(), want.data(), want.size()) == 0);
    check("the same path gives the same entry", files::lookup("data\\map\\s_10\\test.dds") == e);

    // The ".z" the game asks for when the archive holds the file packed: crc32, size, a zlib stream.
    files::TocEntry* z = files::lookup("data\\map\\s_10\\test.dds.z");
    check("the .z name is provided, packed", z && z != e && z->size > 8 && z->size < want.size());
    std::vector<std::uint8_t> packed(z ? z->size : 0);
    std::uint32_t zoff = 0, zn = 0;
    check("it reads whole", z && files::read(z, zoff, packed.data(), z->size, zn) && zn == z->size);
    std::uint32_t head[2] = {0, 0};
    if (packed.size() >= 8) std::memcpy(head, packed.data(), 8);
    check("its header is the crc32 and the size of the file",
          head[0] == std::uint32_t(mz_crc32(MZ_CRC32_INIT, want.data(), want.size())) && head[1] == want.size());
    std::vector<std::uint8_t> un(want.size());
    mz_ulong un_n = mz_ulong(un.size());
    check("its stream inflates to the mod's bytes",
          packed.size() > 8 && mz_uncompress(un.data(), &un_n, packed.data() + 8, mz_ulong(packed.size() - 8)) == MZ_OK &&
              un_n == want.size() && un == want);

    check("a path no mod provides is the game's", files::lookup("data\\map\\s_10\\other.dds") == nullptr &&
                                                      files::lookup("data\\map\\s_10\\other.dds.z") == nullptr);
    files::TocEntry theirs;
    std::uint32_t o = 0, n = 0;
    check("a read on the game's own entry is not ours", !files::read(&theirs, o, got.data(), 16, n));

    fs::remove_all(root);
    if (failed) std::printf("%d FAILED\n", failed);
    else std::printf("all passed (0 failures)\n");
    return failed ? 1 : 0;
}
