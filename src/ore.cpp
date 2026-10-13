#include "ore.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cleria/content.hpp"
#include "cleria/ini.hpp"
#include "cleria/mod_events.hpp"
#include "cleria/mods.hpp"
#include "cleria/modscript.hpp"
#include "mark.hpp"

namespace fs = std::filesystem;
using namespace cleria;

namespace ore {

namespace {

const HANDLE g_started = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual reset: set for good
Ini g_ini;                     // cleriaore.ini
bool g_use = true;             // [cleria.mods] UseMods
std::vector<mods::Mod> g_list;
std::vector<std::unique_ptr<script::Mod>> g_scripts;
std::string g_room;            // the room of the last tick ("" = none)
int g_room_ticks = 0;          // the steps in g_room
std::string g_last_room;       // the last room there was: a room_enter's prev_room (the scene is 0 between two rooms of an intro)
events::FlagWatch g_watch;     // g_flags at the last tick
Sidecar g_sidecar;             // the session's: read with a load, written with a save, emptied by a new game
enum class Session { None, NewGame, Load } g_session = Session::None;   // its event waits for the next tick
int g_session_slot = -1;

// "yso_05.bin" -> 5: the file number CleriaCore calls the slot; -1 for a name without one.
int slot_of(const char* file) {
    int slot = -1;
    for (const char* p = file; *p; ++p)
        if (*p >= '0' && *p <= '9') slot = (slot < 0 ? 0 : slot * 10) + (*p - '0');
        else if (slot >= 0) break;
    return slot;
}

fs::path save_file(const char* file) { return dir() / "saves" / fs::path(file).filename(); }

// ---- what CleriaCore's content layer reports (its view_content.inl), read off the retail game ----------
constexpr int kFlagFight = 156, kFlagRun = 193, kFlagScene = 0x1f9, kPanacea = 0x59;
std::map<std::string, std::pair<int, std::string>> g_chests;   // the room's: script -> {box flag, object}
std::string g_fight_room;      // the room in which 156 last went 0 -> 1
std::string g_defeated_room;   // one boss_defeated a flag and a room
unsigned g_defeated_mask = 0;
double g_hp = 0;               // the player's HP at the last tick
bool g_script_panacea = false; // a script stored the Panacea's cell this tick: not a use

// A chest's box flag is the cell its script tests first (Flag_IsEqualInt, sub-op 0x5F), as CleriaCore
// reads it (scene.cpp chest_opened_flag). An XSO of one entry: the code's words start at 0x24.
int box_flag(const std::vector<std::uint8_t>& xso) {
    if (xso.size() < 0x24 + 4 || std::memcmp(xso.data(), "XSR", 4) != 0) return -1;
    std::uint32_t word = 0;
    std::memcpy(&word, xso.data() + 0x24, 4);
    const std::size_t at = 0x24 + 4 * (1 + ((word >> 8) & 0xf));   // past the operands' type words
    if (word >> 24 != 2 || ((word >> 12) & 0xfff) != 0x5f || (word & 0xff) < 1 || at + 4 > xso.size()) return -1;
    std::int32_t flag = -1;
    std::memcpy(&flag, xso.data() + at, 4);
    return flag >= 0 && flag < 0x200 ? flag : -1;
}

// The chests of room `scene`: its S_BOXnn scripts (the game ships 01..04; a room's objects box_nn run them).
void read_chests(int scene) {
    g_chests.clear();
    std::vector<std::uint8_t> xso;
    for (int n = 1; n <= 9; ++n) {
        char path[64], script[16], object[16];
        std::snprintf(path, sizeof path, "data\\map\\s_%02d\\s_%04d\\s_box%02d.xso.z", scene / 100, scene, n);
        if (!game_file(path, xso)) continue;
        std::snprintf(script, sizeof script, "s_box%02d", n);
        std::snprintf(object, sizeof object, "box_%02d", n);
        if (const int flag = box_flag(xso); flag >= 0) g_chests[script] = {flag, object};
    }
}

void emit(events::Event e, const std::string& room) {
    e.room = room;
    events::bus().emit(e);
}

// boss_defeated, death and item_used, from the cells the tick's diff is about to report. The two boss
// rules are CleriaCore's content_boss_watch: a floor boss's flag 220..227 goes 0 -> 1 (once a room), or
// the fight flag 156 goes 1 -> 0 in the room it rose in, outside the eight rooms whose boss has a flag,
// while the player lives [H: CleriaCore knows a lost duel by its own death code].
void watch_game(const std::int32_t* flags, double hp, const std::string& room) {
    if (g_hp > 0 && hp <= 0) emit({events::Kind::PlayerDeath}, room);
    g_hp = hp;
    const bool script_panacea = g_script_panacea;
    g_script_panacea = false;
    if (!g_watch.primed) {
        g_fight_room = room;
        return;
    }
    const auto& prev = g_watch.prev;
    if (prev[kPanacea] > 0 && flags[kPanacea] == prev[kPanacea] - 1 && !script_panacea) {
        events::Event e{events::Kind::ItemUsed};
        e.item = kPanacea;
        emit(e, room);
    }
    if (prev[kFlagFight] == 0 && flags[kFlagFight] == 1) g_fight_room = room;
    static const char* const kFlagged[] = {"S_1099", "S_2099", "S_3099", "S_4099", "S_5099", "S_6082", "S_7080", "S_7099"};
    auto raise = [&](int flag) {
        if (flags[kFlagRun] != 0) return;   // a Time Attack / Arena run: a replay, not the story's defeat
        events::Event e{events::Kind::BossDefeated};
        e.index = flag;
        emit(e, room);
    };
    if (g_defeated_room != room) {
        g_defeated_room = room;
        g_defeated_mask = 0;
    }
    for (int f = 220; f <= 227; ++f)
        if (prev[std::size_t(f)] == 0 && flags[f] == 1 && !(g_defeated_mask & (1u << (f - 220)))) {
            g_defeated_mask |= 1u << (f - 220);
            raise(f);
        }
    if (prev[kFlagFight] == 1 && flags[kFlagFight] == 0 && hp > 0 && g_fight_room == room &&
        std::find_if(std::begin(kFlagged), std::end(kFlagged), [&](const char* r) { return room == r; }) == std::end(kFlagged))
        raise(-1);
}

}  // namespace

const std::map<std::string, std::pair<int, std::string>>& chests() { return g_chests; }

std::vector<mods::Mod>& mod_list() { return g_list; }
std::vector<std::unique_ptr<script::Mod>>& scripts() { return g_scripts; }

void save_prefs() {
    const mods::Prefs p = mods::prefs_of(g_list, g_use);
    g_ini.set_int("cleria.mods", "UseMods", p.use ? 1 : 0);
    g_ini.set_string("cleria.mods", "Order", mods::join_list(p.order));
    g_ini.set_string("cleria.mods", "Active", mods::join_list(p.active));
    g_ini.save(dir() / "cleriaore.ini");
}

// As CleriaCore keeps them in cleria.ini (view_lua.inl LuaHost::setting).
std::optional<std::string> setting(const std::string& mod, const std::string& key) {
    auto v = g_ini.raw("cleria.mod." + mod, key);
    if (v && v->size() >= 2 && v->front() == '"' && v->back() == '"') v = v->substr(1, v->size() - 2);
    return v;
}
bool set_setting(const std::string& mod, const std::string& key, const std::string& value) {
    g_ini.set_string("cleria.mod." + mod, key, value);
    return g_ini.save(dir() / "cleriaore.ini");
}

void wait_started() { WaitForSingleObject(g_started, INFINITE); }

void start(bool hooked) {
    // The game's first file lookup waits for this function (hooks.cpp): it must set g_started on every path.
    struct Done { ~Done() { SetEvent(g_started); } } done;
    log("%s", kLoaderMark);
    log(hooked ? "[hooks] installed" : "[hooks] none: this process is not the yso_win.exe build CleriaOre knows");
    Ini& ini = g_ini;
    ini.load(dir() / "cleriaore.ini");
    mods::Prefs prefs;
    prefs.use = g_use = ini.get_int("cleria.mods", "UseMods").value_or(1) != 0;
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

Sidecar& sidecar() { return g_sidecar; }
bool session_pending() { return g_session != Session::None; }
int room_ticks() { return g_room_ticks; }

// The storage is the new session's at once; the event is the next tick's, after that session's room_enter
// (CleriaCore's order, LUA_API.md section 3).
void on_new_game() {
    g_sidecar.clear();
    g_session = Session::NewGame;
    g_session_slot = -1;
    log("[save] new game");
}

void on_loaded(const char* file) {
    g_sidecar = read_sidecar(save_file(file));
    g_session = Session::Load;
    g_session_slot = slot_of(file);
    log("[save] loaded %s (slot %d)", file, g_session_slot);
}

void on_saved(const char* file) {
    const bool ok = write_sidecar(save_file(file), g_sidecar);
    log("[save] saved %s (slot %d)%s", file, slot_of(file), ok ? "" : ": its .cleria could not be written");
    events::Event e;
    e.kind = events::Kind::Save;
    e.room = host().room();
    e.slot = slot_of(file);
    events::bus().emit(e);
}

void on_tick() {
    if (WaitForSingleObject(g_started, 0) != WAIT_OBJECT_0 || g_scripts.empty()) return;
    script::Host& h = host();
    const std::string room = h.room();
    const bool session = g_session != Session::None && !room.empty();   // a session starts with its first room
    if (room != g_room || session) {
        if (!room.empty()) {
            read_chests(h.flags()[kFlagScene]);
            events::Event e;
            e.kind = events::Kind::RoomEnter;
            e.room = room;
            e.prev_room = g_last_room;
            events::bus().emit(e);
            g_last_room = room;
        }
        if (g_room.empty()) g_watch.primed = false;   // into a game from the title: a baseline, not 512 changes
        g_room = room;
        g_room_ticks = 0;
    }
    ++g_room_ticks;
    if (session) {
        g_watch.primed = false;   // another save's flags: a baseline too
        events::Event e;
        e.kind = g_session == Session::NewGame ? events::Kind::NewGame : events::Kind::Load;
        e.room = room;
        e.slot = g_session_slot;
        e.character = h.character();
        g_session = Session::None;
        events::bus().emit(e);
    }
    if (!room.empty()) {
        watch_game(h.flags(), h.in_game() ? h.stat(script::Host::Stat::Hp) : g_hp, room);   // no player between two rooms
        g_watch.diff(h.flags(), events::bus(), room);
    }
    float factor = 1.0f;   // cleria.game.exp_factor: the mods' factors of the EXP the game awards at this level
    const int level = int(h.stat(script::Host::Stat::Level));
    for (auto& m : g_scripts)
        if (m->ok()) factor *= m->exp_factor(level);
    set_exp_factor(factor);
    for (auto& m : g_scripts) m->on_tick();
}

GrantDecision on_grant(GrantQuery q) {
    GrantDecision d;
    if (WaitForSingleObject(g_started, 0) != WAIT_OBJECT_0 || g_scripts.empty()) return d;
    script::Host& h = host();
    const std::string room = h.room();
    q.flags = h.flags();
    const auto chest = g_chests.find(q.script);
    if (chest != g_chests.end()) q.object = chest->second.second;
    for (auto& m : g_scripts) {
        if (!m->has_filter() || !m->ok()) continue;
        d = m->filter(q, room);
        if (d.action != GrantDecision::Action::Pass) break;
    }
    // What CleriaCore reports of a script's store (view_content.inl content_filter).
    if (q.kind != GrantQuery::Kind::Store) return d;
    const bool pass = d.action == GrantDecision::Action::Pass;
    if (q.index == kPanacea) g_script_panacea = true;
    if (chest != g_chests.end() && chest->second.first == q.index && q.old_value == 0 && q.value != 0) {
        events::Event e{events::Kind::ChestOpened};
        e.index = q.index;
        e.script = q.script;
        e.object = q.object;
        emit(e, room);
    }
    // A locked door opened with its key or medallion: the room's UseKey script stores the door's flag.
    if (pass && q.script == "usekey" && q.index >= content::kRetailItems && q.old_value == 0 && q.value != 0) {
        events::Event e{events::Kind::DoorOpened};
        e.index = q.index;
        e.script = q.script;
        emit(e, room);
    }
    if (pass && q.index >= 0 && q.index < content::kRetailItems && q.value > (std::max)(q.old_value, 0)) {
        events::Event e{events::Kind::ItemObtained};
        e.item = q.index;
        e.count = q.value - (std::max)(q.old_value, 0);
        e.source = chest != g_chests.end() ? "chest" : "script";
        e.script = q.script;
        emit(e, room);
    }
    return d;
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
