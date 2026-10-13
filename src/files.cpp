#include "files.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cleria/vfs.hpp"
#include "miniz.h"
#include "ore.hpp"

namespace fs = std::filesystem;
using namespace cleria;

namespace ore::files {

namespace {

// The game unpacks a ".z" into one shared buffer of this size (DAT_00764bb8, set by FUN_005d6700) and
// fails the load of a larger file.
constexpr std::size_t kUnpackMax = 0x1800000;

struct Blob {
    TocEntry entry;
    std::vector<std::uint8_t> data;
};

std::mutex g_mtx;
std::unordered_map<std::string, std::unique_ptr<Blob>> g_by_path;   // the game's path, lowercase
std::unordered_map<const TocEntry*, const Blob*> g_by_entry;        // blobs are never freed

bool read_file(const fs::path& p, std::vector<std::uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// u32 crc32, u32 unpacked size, a zlib stream: what FUN_005eeae0 reads.
bool pack(const std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out) {
    mz_ulong n = mz_compressBound(mz_ulong(in.size()));
    out.resize(8 + n);
    if (mz_compress2(out.data() + 8, &n, in.data(), mz_ulong(in.size()), MZ_BEST_SPEED) != MZ_OK) return false;
    out.resize(8 + n);
    const std::uint32_t head[2] = {std::uint32_t(mz_crc32(MZ_CRC32_INIT, in.data(), in.size())), std::uint32_t(in.size())};
    std::memcpy(out.data(), head, 8);
    return true;
}

const vfs::Entry* provided(const std::string& name) {
    const auto& t = vfs::table();
    const auto it = t.find(vfs::key(name));
    return it == t.end() ? nullptr : &it->second;
}

}  // namespace

TocEntry* lookup(const char* path) {
    const bool text = content_text_on();
    if (!path || (vfs::table().empty() && !text)) return nullptr;
    // As FUN_005defa0 reads a path: lowercase, without the leading "data\", "\.\" and "\..\" folded.
    std::string p(path);
    std::transform(p.begin(), p.end(), p.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; });
    if (p.rfind("data\\", 0) == 0) p.erase(0, 5);
    const std::string name = fs::path(p).lexically_normal().generic_string();

    std::lock_guard<std::mutex> lk(g_mtx);
    if (const auto it = g_by_path.find(name); it != g_by_path.end()) return it->second ? &it->second->entry : nullptr;
    std::unique_ptr<Blob>& slot = g_by_path[name];   // stays null when no mod provides it, or its file fails

    const vfs::Entry* e = provided(name);
    const bool z = !e && name.size() > 2 && name.compare(name.size() - 2, 2, ".z") == 0;
    if (z) e = provided(name.substr(0, name.size() - 2));
    // A script (".xso.z") may have new strings from a mod's text*.ini: the game's own file, or the mod's, rewritten.
    const bool script = text && z && name.size() > 6 && name.compare(name.size() - 6, 6, ".xso.z") == 0;
    if (!e && !script) return nullptr;

    std::string who = e ? vfs::layers()[std::size_t(e->layer)].id : std::string("the game's own");
    auto b = std::make_unique<Blob>();
    std::vector<std::uint8_t> raw;
    if (e) {
        if (!read_file(e->file, z ? raw : b->data)) {
            log("[files] %s: %s cannot be read (%s)", name.c_str(), e->file.string().c_str(), who.c_str());
            return nullptr;
        }
    } else if (!game_file_own(path, raw)) {
        return nullptr;
    }
    if (script) {
        const int n = content_patch_script(name.substr(0, name.size() - 2), raw);
        if (!e && n == 0) return nullptr;
        if (n) who += ", " + std::to_string(n) + " string(s) from text.ini";
    }
    if (z && (raw.size() > kUnpackMax || !pack(raw, b->data))) {
        log("[files] %s: larger than the game can unpack (%zu bytes, %zu at most) (%s)", name.c_str(), raw.size(), kUnpackMax, who.c_str());
        return nullptr;
    }
    b->entry.size = std::uint32_t(b->data.size());
    log("[files] %s <- %s", name.c_str(), who.c_str());
    g_by_entry[&b->entry] = b.get();
    slot = std::move(b);
    return &slot->entry;
}

bool read(const TocEntry* e, std::uint32_t& offset, void* dst, std::uint32_t n, std::uint32_t& got) {
    const Blob* b = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        const auto it = g_by_entry.find(e);
        if (it == g_by_entry.end()) return false;
        b = it->second;
    }
    const std::size_t size = b->data.size();
    got = offset < size ? std::uint32_t(std::min<std::size_t>(n, size - offset)) : 0;
    if (got) std::memcpy(dst, b->data.data() + offset, got);
    offset += got;
    return true;
}

bool unpack(const std::vector<std::uint8_t>& z, std::vector<std::uint8_t>& out) {
    std::uint32_t head[2];
    if (z.size() < 8) return false;
    std::memcpy(head, z.data(), 8);
    if (head[1] > kUnpackMax) return false;
    out.resize(head[1]);
    mz_ulong n = head[1];
    return mz_uncompress(out.data(), &n, z.data() + 8, mz_ulong(z.size() - 8)) == MZ_OK && n == head[1];
}

}  // namespace ore::files
