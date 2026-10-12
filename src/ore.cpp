#include "ore.hpp"

#include <windows.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "cleria/ini.hpp"
#include "cleria/mod_events.hpp"
#include "cleria/mods.hpp"
#include "cleria/modscript.hpp"

namespace fs = std::filesystem;
using namespace cleria;

namespace ore {

namespace {

const HANDLE g_started = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual reset: set for good
std::vector<mods::Mod> g_list;
std::vector<std::unique_ptr<script::Mod>> g_scripts;
std::string g_room;            // the room of the last tick ("" = none)
events::FlagWatch g_watch;     // g_flags at the last tick

}  // namespace

void wait_started() { WaitForSingleObject(g_started, INFINITE); }

void start(bool hooked) {
    // The game's first file lookup waits for this function (hooks.cpp): it must set g_started on every path.
    struct Done { ~Done() { SetEvent(g_started); } } done;
    log(hooked ? "[hooks] installed" : "[hooks] none: this process is not the yso_win.exe build CleriaOre knows");
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
        auto sm = std::make_unique<script::Mod>(m.m.id, m.label(), m.m.version, m.root, host());
        const bool ok = sm->start(m.m.script);
        log("[lua] %s: %s", m.label().c_str(), ok ? ("started (" + m.m.script + ")").c_str() : ("disabled: " + sm->error()).c_str());
        g_scripts.push_back(std::move(sm));
    }
    events::bus().subscribe([](const events::Event& e) {
        for (auto& m : g_scripts) m->on_event(e);
    });
}

void on_tick() {
    if (WaitForSingleObject(g_started, 0) != WAIT_OBJECT_0 || g_scripts.empty()) return;
    script::Host& h = host();
    const std::string room = h.room();
    if (room != g_room) {
        if (!room.empty()) {
            events::Event e;
            e.kind = events::Kind::RoomEnter;
            e.room = room;
            e.prev_room = g_room;
            events::bus().emit(e);
        }
        if (g_room.empty()) g_watch.primed = false;   // into a game from the title: a baseline, not 512 changes
        g_room = room;
    }
    if (!room.empty()) g_watch.diff(h.flags(), events::bus(), room);
    for (auto& m : g_scripts) m->on_tick();
}

void on_frame() {
    if (WaitForSingleObject(g_started, 0) != WAIT_OBJECT_0 || g_scripts.empty()) return;
    using clock = std::chrono::steady_clock;
    static clock::time_point last = clock::now();
    const clock::time_point now = clock::now();
    const double dt = std::chrono::duration<double>(now - last).count();
    last = now;
    for (auto& m : g_scripts) m->on_frame(dt);
}

}  // namespace ore
