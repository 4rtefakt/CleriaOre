#include "ore.hpp"

#include <windows.h>
#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cleria/ini.hpp"
#include "cleria/mods.hpp"
#include "cleria/modscript.hpp"

namespace fs = std::filesystem;
using namespace cleria;

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

namespace {

// What a script reaches of the retail game. Nothing of the game yet: every method the game hooks
// will back keeps script::Host's harmless default.
struct RetailHost : script::Host {
    void log(const std::string& mod, const std::string& text) override { ore::log("[lua:%s] %s", mod.c_str(), text.c_str()); }
    void notice(const std::string& mod, const std::string& head, const std::string& text) override {
        ore::log("[lua:%s] notice: %s %s", mod.c_str(), head.c_str(), text.c_str());
    }
    void failed(const std::string& mod, const std::string& error) override { ore::log("[lua:%s] disabled: %s", mod.c_str(), error.c_str()); }
};

RetailHost g_host;
std::vector<mods::Mod> g_list;
std::vector<std::unique_ptr<script::Mod>> g_scripts;

}  // namespace

void start() {
    Ini ini;
    ini.load(dir() / "cleriaore.ini");
    mods::Prefs prefs;
    prefs.use = ini.get_int("cleria.mods", "UseMods").value_or(1) != 0;
    prefs.order = mods::split_list(ini.get_string("cleria.mods", "Order").value_or(""));
    prefs.active = mods::split_list(ini.get_string("cleria.mods", "Active").value_or(""));

    const fs::path mods_dir = dir() / "mods";
    if (prefs.use) g_list = mods::load(mods_dir, prefs);
    log("[mods] folder %s: %zu installed, active: %s", mods_dir.string().c_str(), g_list.size(),
        mods::active_summary().c_str());
    for (mods::Mod& m : g_list) {
        for (const std::string& e : m.errors) log("[mods]   %s: %s", m.label().c_str(), e.c_str());
        for (const std::string& n : m.notes) log("[mods]   %s: %s", m.label().c_str(), n.c_str());
        if (m.state != mods::State::Active || m.m.script.empty()) continue;
        if (m.m.api < script::kScriptApi) {
            log("[mods]   %s: Script= ignored: a Lua script needs ApiVersion=3", m.label().c_str());
            continue;
        }
        auto sm = std::make_unique<script::Mod>(m.m.id, m.label(), m.m.version, m.root, g_host);
        const bool ok = sm->start(m.m.script);
        log("[lua] %s: %s", m.label().c_str(), ok ? ("started (" + m.m.script + ")").c_str() : ("disabled: " + sm->error()).c_str());
        g_scripts.push_back(std::move(sm));
    }
}

}  // namespace ore
