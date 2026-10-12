// CleriaOre's folder and its log (ore.hpp).
#include <windows.h>
#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "ore.hpp"

namespace fs = std::filesystem;

namespace ore {

const fs::path& dir() {
    static const fs::path d = [] {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        return fs::path(exe).parent_path() / "cleriaore";
    }();
    return d;
}

void log(const char* fmt, ...) {
    static std::mutex mtx;
    static FILE* f = nullptr;
    std::lock_guard<std::mutex> lk(mtx);
    if (!f) {
        std::error_code ec;
        fs::create_directories(dir(), ec);
        f = _wfsopen((dir() / "cleriaore.log").c_str(), L"w", _SH_DENYWR);
        if (!f) return;
    }
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fflush(f);
}

}  // namespace ore
