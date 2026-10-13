#include "ore.hpp"
#include "ui.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <set>
#include <cmath>
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
std::set<std::string> g_reload;        // scripts to start afresh at the next frame
std::vector<std::string> g_log_tail;   // the last lines the scripts logged (cleria.log_lines)
std::string g_state;                   // game.state() at the last tick, for state_changed
std::map<std::string, std::array<std::pair<double, double>, 3>> g_stat_mods;   // per mod: {add, mul} of max HP, ATK, DEF
std::set<std::string> g_pause;         // the mods that hold the game
double g_boss_bonus = 0;               // cleria.combat.boss_bonus
std::map<std::string, unsigned> g_hud_hidden;   // cleria.hud.show: per mod, the parts it hides
float g_exp_factor = 1.0f;             // the mods' exp_factor product, as last written into the game's rate
std::map<std::string, std::array<double, 2>> g_tune;   // per mod: the factors of the player's speed and jump
constexpr std::uintptr_t kBossGaugeOwner = 0x6d5fd8;   // the boss gauge's widget +4: its owner's instance id (actor +0x14), 0 = no gauge
std::map<std::string, double> g_speed; // a mod's factor of the game's speed

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

// ---- combat (cleria.combat.add_filter): the mods in load order, the first answer that is not Pass ----------
// What the game keeps of an actor (CleriaCore's ENTITY.md, LEVELING.md): floats but the cast, level and SP.
template <class T>
T field(const void* actor, std::size_t at) {
    T v;
    std::memcpy(&v, static_cast<const char*>(actor) + at, sizeof v);
    return v;
}
constexpr std::size_t kActPos = 0x50, kActAtk = 0xa4, kActDef = 0xa8, kActExp = 0xb0, kActSp = 0xc4, kActLevel = 0xc8, kActCast = 0x124, kActFlags = 0x12c;
constexpr int kCastPlayer = 13;
constexpr std::uint32_t kActCredited = 0x4000000;   // +0x12c: its EXP was given (the game's own once-only guard)
const auto scale_sp = reinterpret_cast<int(__fastcall*)(int base)>(0x4fa390);   // the difficulty, the blessing, the game's SP rate
const auto add_sp = reinterpret_cast<void(__fastcall*)(int amount)>(0x4fa290);   // into the wallet (0..999999) and the player's record

bool combat_on(script::CombatQuery::Kind k) {
    if (WaitForSingleObject(g_started, 0) != WAIT_OBJECT_0) return false;
    for (auto& m : g_scripts)
        if (m->ok() && m->has_combat_filter(k)) return true;
    return false;
}
script::CombatDecision combat(const script::CombatQuery& q) {
    const std::string room = host().room();
    for (auto& m : g_scripts) {
        if (!m->ok() || !m->has_combat_filter(q.kind)) continue;
        const script::CombatDecision d = m->combat_filter(q, room);
        if (d.action != script::CombatDecision::Action::Pass) return d;
    }
    return {};
}
// The answer as a number within 0..`top`.
double answered(const script::CombatDecision& d, double value, double top) {
    if (d.action == script::CombatDecision::Action::Suppress) return 0;
    if (d.action == script::CombatDecision::Action::Replace) return std::clamp(d.value, 0.0, top);
    return value;
}
float g_kill_exp = 0;   // the EXP of the kill being credited, for its enemy_killed

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

std::vector<std::string> g_order;      // the load order the panel chose; empty: as the list stands
void save_order(const std::vector<std::string>& ids) {
    g_order = ids;
    save_prefs();
}
void save_prefs() {
    mods::Prefs p = mods::prefs_of(g_list, g_use);
    if (!g_order.empty()) p.order = g_order;
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
    log("%s %s", kLoaderMark, kVersion);
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
    content_start();
}

Sidecar& sidecar() { return g_sidecar; }

void request_reload(const std::string& mod) { g_reload.insert(mod); }
void set_pause(const std::string& mod, bool on) {
    if (on) g_pause.insert(mod);
    else g_pause.erase(mod);
}
bool paused() { return !g_pause.empty(); }
void set_tune(const std::string& mod, int key, double factor) {
    g_tune.try_emplace(mod, std::array<double, 2>{1, 1}).first->second[std::size_t(key)] = factor;
}
double tune(int key) {
    double f = 1.0;
    for (const auto& [id, a] : g_tune) f *= a[std::size_t(key)];
    return key == 0 ? std::clamp(f, 0.1, 5.0) : std::clamp(f, 0.1, 4.0);
}
void set_hud_hidden(const std::string& mod, unsigned bits) {
    if (bits) g_hud_hidden[mod] = bits;
    else g_hud_hidden.erase(mod);
}
unsigned hud_hidden() {
    unsigned v = 0;
    for (const auto& [id, b] : g_hud_hidden) v |= b;
    return v;
}
double boss_bonus(double n) {
    if (n >= 0) g_boss_bonus = n > 0 ? (std::min)(g_boss_bonus + n, 1.0e7) : 0.0;
    return g_boss_bonus;
}
void set_speed(const std::string& mod, double factor) {
    if (factor == 1.0) g_speed.erase(mod);
    else g_speed[mod] = factor;
}
double speed() {
    double f = 1.0;
    for (const auto& [id, x] : g_speed) f *= x;
    return std::clamp(f, 0.1, 4.0);
}
const std::vector<std::string>& log_tail() { return g_log_tail; }
void log_script(const std::string& mod, const std::string& text) {
    log("[lua:%s] %s", mod.c_str(), text.c_str());
    g_log_tail.push_back(mod + ": " + text);
    if (g_log_tail.size() > 200) g_log_tail.erase(g_log_tail.begin());
}
script::Mod* window_mod() {
    for (auto& m : g_scripts)
        if (m->ok() && m->window()) return m.get();
    return nullptr;
}

// As CleriaCore's (view_lua.inl LuaHost::set_modifier): a mod's {0, 0} is "none"; the adds are summed, the muls multiplied.
void set_modifier(const std::string& mod, int stat, double add, double mul) {
    if (stat < 0 || stat > 2) return;
    auto& a = g_stat_mods[mod];
    for (auto& x : a)
        if (x.first == 0 && x.second == 0) x.second = 1;
    a[std::size_t(stat)] = {add, mul};
    recompute_stats();
}
std::pair<double, double> modifier(const std::string& mod, int stat) {
    const auto it = g_stat_mods.find(mod);
    if (it == g_stat_mods.end() || stat < 0 || stat > 2) return {0.0, 1.0};
    return it->second[std::size_t(stat)];
}
float modified(int stat, float base) {
    double add = 0, mul = 1;
    for (const auto& [id, a] : g_stat_mods) {
        add += a[std::size_t(stat)].first;
        mul *= a[std::size_t(stat)].second;
    }
    if (add == 0 && mul == 1) return base;
    const double v = (double(base) + add) * mul;
    return float(stat == 0 ? std::clamp(v, 1.0, 999.0) : stat == 1 ? (std::max)(v, 0.0) : v);   // the game's own cap; ATK is raised to a power
}

// A key or a pad button went down while no panel holds the input: the mods' actions bound to it, then "input".
void on_input(const std::string& name, bool pad) {
    if (WaitForSingleObject(g_started, 0) != WAIT_OBJECT_0 || g_scripts.empty() || name.empty()) return;
    auto same = [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; };
    for (std::size_t mi = 0; mi < g_scripts.size(); ++mi) {
        script::Mod& m = *g_scripts[mi];
        for (std::size_t i = 0; i < m.actions_.size(); ++i)
            if (same(pad ? m.actions_[i].pad : m.actions_[i].key, name)) m.run_action(i);
    }
    emit(events::Event::named("input").with(pad ? "pad" : "key", name), host().room());
}

// A script started afresh: its Lua state is closed with all it registered, its stat modifiers go, its main
// file runs again.
void reload(const std::string& id) {
    for (auto& up : g_scripts) {
        if (up->id() != id) continue;
        const mods::Mod* def = nullptr;
        for (const mods::Mod& m : g_list)
            if (m.m.id == id) def = &m;
        if (!def) return;
        g_stat_mods.erase(id);
        g_pause.erase(id);
        g_tune.erase(id);
        g_hud_hidden.erase(id);
        g_speed.erase(id);
        recompute_stats();
        up.reset();
        up = std::make_unique<script::Mod>(def->m.id, def->label(), def->m.version, def->root, host());
        const bool ok = up->start(def->m.script);
        log("[lua] %s: %s", def->label().c_str(), ok ? "reloaded" : ("reload failed: " + up->error()).c_str());
        return;
    }
}
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

bool world_on(const char* kind) {
    if (WaitForSingleObject(g_started, 0) != WAIT_OBJECT_0) return false;
    for (auto& m : g_scripts)
        if (m->ok() && m->has_world_filter(kind)) return true;
    return false;
}
script::WorldDecision world_filter(const script::WorldQuery& q) {
    const std::string room = host().room();
    for (auto& m : g_scripts) {
        if (!m->ok() || !m->has_world_filter(q.kind)) continue;
        const script::WorldDecision d = m->world_filter(q, room);
        if (d.action != script::WorldDecision::Action::Pass) return d;
    }
    return {};
}
// A blessing bought reaches its effect: the shop filters' Buy query (another blessing's effect, or none), then
// blessing_bought, whatever they said. The price is what its script last took (0x69).
// The player's bullet as it is made: player_bullet, with what another screen needs to show the same one.
void on_bullet(const void* owner, int kind, const std::uint32_t* a) {
    if (WaitForSingleObject(g_started, 0) != WAIT_OBJECT_0 || g_scripts.empty()) return;
    if (!owner || owner != *reinterpret_cast<void**>(0x74c09c) || kind == 0xf) return;
    auto f = [&](int i) { float v; std::memcpy(&v, &a[i], 4); return double(v); };
    const float* pos = reinterpret_cast<const float*>(a[1]);
    const float* dir = reinterpret_cast<const float*>(a[2]);
    emit(events::Event::named("player_bullet").with("kind", kind).with("effect", int(a[0]))
             .with("x", pos[0]).with("y", pos[1]).with("z", pos[2]).with("dx", dir[0]).with("dy", dir[1]).with("dz", dir[2])
             .with("speed", f(3)).with("accel", f(4)).with("gravity", f(5))
             .with("a", double(std::int32_t(a[8]))).with("b", double(std::int32_t(a[9]))).with("c", double(std::int32_t(a[10])))
             .with("d", f(11)).with("e", f(12)).with("f", f(13)).with("g", f(14)).with("h", f(15)),
         host().room());
}
// The player's animation script makes an effect: player_effect, with what a puppet needs to show the same one.
void on_effect(const void* actor, const std::int32_t* a) {
    if (WaitForSingleObject(g_started, 0) != WAIT_OBJECT_0 || g_scripts.empty()) return;
    if (!actor || actor != *reinterpret_cast<void**>(0x74c09c)) return;
    auto f = [&](int i) { float v; std::memcpy(&v, &a[i], 4); return double(v); };
    emit(events::Event::named("player_effect").with("id", a[0]).with("fwd", f(1)).with("angle", f(2)).with("voff", f(3)).with("scale", f(4))
             .with("attach", a[5]),
         host().room());
}
// The blessing a script's name stands for: "@Grow03" -> 3, "@GrowAr" -> 7 (armor), "@GrowLe" -> 8 (leggings), any
// case; -1 for anything else. For 0xdd the label operand names the script, but the menu's own script runs it: the
// price is then keyed by what the filter is asked with, the sub-op and the price itself.
int blessing_of(const char* script) {
    char s[8] = {0};
    const char* p = script && script[0] == '@' ? script + 1 : script;
    if (!p || std::strlen(p) != 6) return -1;
    for (int i = 0; i < 6; ++i) s[i] = char(std::tolower(static_cast<unsigned char>(p[i])));
    if (std::memcmp(s, "grow", 4) != 0) return -1;
    if (std::memcmp(s + 4, "ar", 2) == 0) return 7;
    if (std::memcmp(s + 4, "le", 2) == 0) return 8;
    return std::isdigit(static_cast<unsigned char>(s[4])) && std::isdigit(static_cast<unsigned char>(s[5])) ? (s[4] - '0') * 10 + (s[5] - '0') : -1;
}
bool g_shop = false;                   // some mod has a shop filter (looked up each tick: the fetch hook is hot)
int g_shop_paid = 0;                   // the SP the last blessing's script took
bool shop_on() { return g_shop; }
int on_shop_price(int sub, const char* script, const char* of, int price) {
    const int bi = blessing_of(of);
    if (bi < 0) return price;
    ShopQuery q;
    q.kind = ShopQuery::Kind::Price;
    q.sub = sub;
    q.index = bi;
    q.value = price;
    q.script = script ? script : "";
    const std::string room = host().room();
    int out = price;
    for (auto& m : g_scripts) {
        if (!m->ok() || !m->has_shop_filter()) continue;
        const GrantDecision d = m->shop_filter(q, room);
        if (d.action == GrantDecision::Action::Replace) { out = (std::max)(0, d.value); break; }
        if (d.action != GrantDecision::Action::Pass) break;
    }
    if (sub == 0x69 || sub == 0xb0) g_shop_paid = out;
    return out;
}
int on_blessing(int index) {
    if (WaitForSingleObject(g_started, 0) != WAIT_OBJECT_0 || g_scripts.empty()) return index;
    const std::string room = host().room();
    ShopQuery q;
    q.kind = ShopQuery::Kind::Buy;
    q.sub = 0xaf;
    q.index = index;
    q.value = g_shop_paid;
    GrantDecision d;
    for (auto& m : g_scripts) {
        if (!m->ok() || !m->has_shop_filter()) continue;
        d = m->shop_filter(q, room);
        if (d.action != GrantDecision::Action::Pass) break;
    }
    const bool off = d.action == GrantDecision::Action::Suppress;
    log("[shop] blessing %d bought%s", index, off ? " (effect suppressed)" : "");
    events::Event e;
    e.kind = events::Kind::BlessingBought;
    e.index = index;
    e.new_value = g_shop_paid;
    e.old_value = off ? 1 : 0;
    emit(e, room);
    if (off) return -1;
    return d.action == GrantDecision::Action::Replace && d.index >= 0 && d.index <= 0x21 ? d.index : index;
}
void emit_named(const events::Event& e) { emit(e, host().room()); }
void on_tick() {
    if (WaitForSingleObject(g_started, 0) != WAIT_OBJECT_0 || (g_scripts.empty() && !content_on())) return;
    g_shop = false;
    for (auto& m : g_scripts) g_shop = g_shop || (m->ok() && m->has_shop_filter());
    if (!g_tune.empty()) tune_tick();
    cam_tick();
    if (!g_hud_hidden.empty()) hud_tick();
    world_tick();
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
    if (const std::string st = h.game_state(); st != g_state) {   // API 7: state_changed
        if (!g_state.empty()) emit(events::Event::named("state_changed").with("state", st).with("old", g_state), room);
        g_state = st;
    }
    float factor = 1.0f;   // cleria.game.exp_factor: the mods' factors of the EXP the game awards at this level
    const int level = int(h.stat(script::Host::Stat::Level));
    if (h.in_game()) {     // API 7: level_up, retry (the game's retry count, g_flags[196]), floor_changed
        static int was_level = 0, was_retry = 0, was_floor = -2;
        static std::string was_room;
        if (was_level > 0 && level > was_level) emit(events::Event::named("level_up").with("level", level).with("max_hp", h.stat(script::Host::Stat::MaxHp)), room);
        was_level = level;
        const int retries = h.flags()[196];
        if (retries > was_retry) emit(events::Event::named("retry").with("snapshot", 1), room);
        was_retry = retries;
        if (room != was_room) {
            was_room = room;
            int fl = 0;
            const std::string title = h.room_title();
            std::size_t i = 0;
            while (i < title.size() && title[i] >= '0' && title[i] <= '9') fl = fl * 10 + (title[i++] - '0');
            if (i == 0 || i >= title.size() || title[i] != 'F') fl = 0;
            if (was_floor != -2 && fl > 0 && fl != was_floor) emit(events::Event::named("floor_changed").with("floor", fl).with("old", was_floor > 0 ? was_floor : 0), room);
            if (fl > 0 || was_floor == -2) was_floor = fl;
        }
    }
    for (auto& m : g_scripts)
        if (m->ok()) factor *= m->exp_factor(level);
    g_exp_factor = factor;
    set_exp_factor(factor);
    for (auto& m : g_scripts) m->on_tick();
}

GrantDecision on_grant(GrantQuery q) {
    GrantDecision d;
    if (WaitForSingleObject(g_started, 0) != WAIT_OBJECT_0 || (g_scripts.empty() && !content_on())) return d;
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
    if (d.action == GrantDecision::Action::Pass && content_on()) d = content_grant(q, room);   // chests.ini
    // The retail game draws an item's icon from its own table: a mod's item (0x100..) has none there.
    if (d.action == GrantDecision::Action::Replace && q.kind != GrantQuery::Kind::Store && (d.index < 0 || d.index >= content::kRetailItems)) {
        // ... so nothing rises from the chest; the mod's own picture, when it gave one (an item's Icon=, or
        // cleria.ui.icon for an id of 0x200..), is shown beside the card instead.
        ui_show_item_icon(d.index);
        if (q.kind == GrantQuery::Kind::Icon) d.action = GrantDecision::Action::Suppress;
        else d.index = content::kNoIcon;
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

// A hit's final damage. Dealt: the player's on a monster. Taken: on the player.
int on_damage(bool dealt, int value, const void* victim, const void* attacker, const void* source) {
    using Kind = script::CombatQuery::Kind;
    if (!victim) return value;
    const int vcast = field<std::int32_t>(victim, kActCast);
    // A monster's hit found a mod's puppet (one the monsters may chase): puppet_struck, and no damage. A puppet is
    // built as the player's cast, so the game would otherwise take it for the player.
    if (!dealt) {
        if (const int pid = puppet_id(victim)) {
            events::Event e = events::Event::named("puppet_struck");
            e.with("puppet", pid).with("damage", value).with("atk", source ? double(field<float>(source, kActAtk)) : 0.0)
                .with("power", source ? double(field<float>(source, 0xfc)) : 0.0);
            const bool actor = attacker && field<std::uint32_t>(attacker, 0) == 0x69ba3c;
            e.with("actor", actor ? double(field<std::int32_t>(attacker, 0x14)) : 0.0).with("cast", actor ? double(field<std::int32_t>(attacker, kActCast)) : -1.0)
                .with("level", actor ? double(field<std::int32_t>(attacker, kActLevel)) : 0.0);
            emit(e, host().room());
            return 0;
        }
    }
    if (dealt ? (!attacker || field<std::int32_t>(attacker, kActCast) != kCastPlayer) : vcast != kCastPlayer) return value;
    // The mods' pooled damage: past the filters, spent at once, on the actor the boss gauge shows.
    auto pooled = [&](int d) {
        const int owner = *reinterpret_cast<const std::int32_t*>(kBossGaugeOwner);
        if (!dealt || d <= 0 || g_boss_bonus < 1.0 || !owner || field<std::int32_t>(victim, 0x14) != owner) return d;
        d += int(g_boss_bonus);
        g_boss_bonus = 0;
        return d;
    };
    // API 7: the hit as it lands, past the filters.
    auto landed = [&](int d) {
        if (g_scripts.empty()) return d;
        const float hp = field<float>(victim, 0x98) - float(d), max_hp = field<float>(victim, 0x94);
        if (dealt) {
            emit(events::Event::named("enemy_hit").with("actor", double(field<std::int32_t>(victim, 0x14))).with("cast", double(vcast))
                     .with("damage", d).with("hp", (std::max)(hp, 0.0f)).with("max_hp", max_hp).with("killed", hp < 1.0f ? 1 : 0),
                 host().room());
        } else {
            events::Event e = events::Event::named("player_hit");
            e.with("damage", d).with("hp", (std::max)(hp, 0.0f)).with("max_hp", max_hp);
            if (attacker && field<std::uint32_t>(attacker, 0) == 0x69ba3c)
                e.with("cast", double(field<std::int32_t>(attacker, kActCast))).with("level", double(field<std::int32_t>(attacker, kActLevel)));
            emit(e, host().room());
        }
        return d;
    };
    if (!combat_on(Kind::Damage)) return landed(pooled(value));
    script::CombatQuery q;
    q.kind = Kind::Damage;
    q.value = value;
    // [H] the formula's own result is not kept apart. What hit carries the formula's inputs; the monster of the two
    // (the victim of a hit dealt, the hitter of one taken, when that is an actor) gives cast and level.
    q.num = {{"formula", double(value)}, {"def", double(field<float>(victim, kActDef))}, {"hp", double(field<float>(victim, 0x98))},
             {"max_hp", double(field<float>(victim, 0x94))}, {"atk", source ? double(field<float>(source, kActAtk)) : 0.0},
             {"power", source ? double(field<float>(source, 0xfc)) : 0.0}};
    if (source) q.num.push_back({"class", double(field<std::uint8_t>(source, 0x109))});
    const void* monster = dealt ? victim : (attacker && field<std::uint32_t>(attacker, 0) == 0x69ba3c ? attacker : nullptr);   // GoActor's vtable
    if (monster) {
        q.num.push_back({"cast", double(field<std::int32_t>(monster, kActCast))});
        q.num.push_back({"level", double(field<std::int32_t>(monster, kActLevel))});
        q.num.push_back({"actor", double(field<std::int32_t>(monster, 0x14))});
    }
    q.str = {{"dir", dealt ? "dealt" : "taken"}};
    return landed(pooled(int(answered(combat(q), value, 999999.0))));
}

// The player's hit on a map object (class 0xd: a boss's part, a breakable), priced by FUN_004faf20: the "damage"
// filter with no cast, then the mods' pooled damage while the boss gauge is up.
int on_part_damage(int value, const void* victim, const void* source) {
    using Kind = script::CombatQuery::Kind;
    if (!victim) return value;
    int d = value;
    if (combat_on(Kind::Damage)) {
        script::CombatQuery q;
        q.kind = Kind::Damage;
        q.value = value;
        q.num = {{"formula", double(value)}, {"def", double(field<float>(victim, kActDef))}, {"hp", double(field<float>(victim, 0x98))},
                 {"max_hp", double(field<float>(victim, 0x94))}, {"atk", source ? double(field<float>(source, kActAtk)) : 0.0},
                 {"power", source ? double(field<float>(source, 0xfc)) : 0.0}};
        q.str = {{"dir", "dealt"}};
        d = int(answered(combat(q), value, 999999.0));
    }
    if (d > 0 && g_boss_bonus >= 1.0 && *reinterpret_cast<const std::int32_t*>(kBossGaugeOwner)) {
        d += int(g_boss_bonus);
        g_boss_bonus = 0;
    }
    return d;
}

// forward: defined below
float on_exp(float value, int source, const void* victim);
void on_kill(const void* victim, float* sp);
// A monster a script's hit killed (cleria.combat.damage) is credited as the applier's kill block credits one of the
// player's: the EXP (its base scaled by the levels' gap: x1.5 a level up to three above the player, /1.5 a level
// below; x0.6666, the rate, the difficulty; then the chain), the SP, the "exp" and "sp" filters, enemy_killed.
void script_kill(void* victim) {
    if (!victim || (field<std::uint32_t>(victim, kActFlags) & kActCredited)) return;
    auto mem = [](std::uintptr_t a) { return *reinterpret_cast<const float*>(a); };
    auto flag = [](std::uintptr_t a) { return *reinterpret_cast<const std::int32_t*>(a) != 0; };
    script::Host& h = host();
    const int d = field<std::int32_t>(victim, kActLevel) - int(h.stat(script::Host::Stat::Level));
    const double gap = std::pow(1.5, d >= 0 ? (std::min)(d, 3) : d);
    const double diff = flag(0x76bc48) ? double(mem(0x6bd580)) : flag(0x76bb58) ? double(mem(0x6bd464)) : 1.0;
    const double chain = 1.0 + double(*reinterpret_cast<const std::int32_t*>(0x76c1a8)) * double(mem(0x6bd084));
    float exp = float((std::max)(1.0, std::floor(double(field<float>(victim, kActExp)) * gap * 0.6666 * double(mem(0x76a5fc)) * diff)) * chain);
    exp = on_exp(exp, 0, victim);
    if (exp > 0) h.add_exp(exp);
    float sp = float(field<std::int32_t>(victim, kActSp));
    on_kill(victim, &sp);
    if (sp > 0) add_sp(scale_sp(int(sp)));
    *reinterpret_cast<std::uint32_t*>(static_cast<char*>(victim) + kActFlags) |= kActCredited;
}

// An EXP award above 0, as it joins the total.
float on_exp(float value, int source, const void* victim) {
    using Kind = script::CombatQuery::Kind;
    if (source == 0) g_kill_exp = value;
    if (!(value > 0) || !combat_on(Kind::Exp)) return value;
    script::CombatQuery q;
    q.kind = Kind::Exp;
    q.value = value;
    // The kill's award as the game builds it (FUN_004fbb40's kill block): the base, scaled by the levels' gap, times
    // the EXP rate 0x76a5fc (the game's own times the mods' exp_factor, which lives there too), the difficulty's
    // factor and the kill chain's.
    auto mem = [](std::uintptr_t a) { return *reinterpret_cast<const float*>(a); };
    auto flag = [](std::uintptr_t a) { return *reinterpret_cast<const std::int32_t*>(a) != 0; };
    const double factor = g_exp_factor > 0 ? g_exp_factor : 1.0;
    q.num = {{"level", host().stat(script::Host::Stat::Level)}, {"rate", double(mem(0x76a5fc)) / factor}, {"factor", factor},
             {"diff_mult", flag(0x76bc48) ? double(mem(0x6bd580)) : flag(0x76bb58) ? double(mem(0x6bd464)) : 1.0},
             {"chain", 1.0 + double(*reinterpret_cast<const std::int32_t*>(0x76c1a8)) * double(mem(0x6bd084))}};
    if (victim) {
        q.num.push_back({"cast", double(field<std::int32_t>(victim, kActCast))});
        q.num.push_back({"enemy_level", double(field<std::int32_t>(victim, kActLevel))});
        q.num.push_back({"base", double(field<float>(victim, kActExp))});
    }
    q.str = {{"source", source == 1 ? "boss" : "kill"}};
    const float out = float(answered(combat(q), value, 9999999.0));
    if (source == 0) g_kill_exp = out;
    return out;
}

// The kill block of the player's hit, past its EXP: the "sp" filter on what the game would credit, then
// enemy_killed. A filter's SP is credited here and the game's own credit is switched off (*sp = 0).
void on_kill(const void* victim, float* sp) {
    using Kind = script::CombatQuery::Kind;
    if (!victim || WaitForSingleObject(g_started, 0) != WAIT_OBJECT_0 || g_scripts.empty()) return;
    const int cast = field<std::int32_t>(victim, kActCast), base = int(*sp);
    int credited = base > 0 ? scale_sp(base) : 0;
    if (credited > 0 && combat_on(Kind::Sp)) {
        script::CombatQuery q;
        q.kind = Kind::Sp;
        q.value = credited;
        q.num = {{"base", double(base)}, {"cast", double(cast)}};
        q.str = {{"source", "kill"}};
        const script::CombatDecision d = combat(q);
        if (d.action != script::CombatDecision::Action::Pass) {
            credited = int(answered(d, credited, 999999.0));
            *sp = 0;
            if (credited > 0) add_sp(credited);
        }
    }
    if (field<std::uint32_t>(victim, kActFlags) & kActCredited) return;   // a revived monster killed again: no second event
    events::Event e{events::Kind::EnemyKilled};
    e.cast = cast;
    e.level = field<std::int32_t>(victim, kActLevel);
    for (int k = 0; k < 3; ++k) e.pos[k] = field<float>(victim, kActPos + 4 * std::size_t(k));
    e.amount = g_kill_exp;
    e.amount2 = credited;
    e.num.emplace_back("actor", double(field<std::int32_t>(victim, 0x14)));   // API 7: which one (a mod's variant)
    emit(e, host().room());
}

// One drop of a killed monster. A filter may suppress it or name another item of the game's.
bool on_drop(int* item, const float* pos) {
    using Kind = script::CombatQuery::Kind;
    if (!combat_on(Kind::Drop)) return true;
    script::CombatQuery q;
    q.kind = Kind::Drop;
    q.value = -1;
    q.item = *item;
    q.num = {{"x", double(pos[0])}, {"y", double(pos[1])}, {"z", double(pos[2])}};
    q.str = {{"name", host().item_name(*item)}};
    const script::CombatDecision d = combat(q);
    if (d.action == script::CombatDecision::Action::Suppress) return false;
    if (d.action == script::CombatDecision::Action::Replace && d.item >= 0 && d.item < content::kRetailItems) *item = d.item;
    return true;
}

// A pickup the player touched (PICKUPS.md section 7.3): what it would give, the "pickup" filter, the events.
// The game's own grant runs unless a filter answered; then what the answer says is applied here.
bool on_pickup(void* pickup) {
    using Kind = script::CombatQuery::Kind;
    if (WaitForSingleObject(g_started, 0) != WAIT_OBJECT_0 || (g_scripts.empty() && !content_on())) return true;
    script::Host& h = host();
    std::int32_t* flags = h.flags();
    auto cell = [](std::uintptr_t a) -> float& { return *reinterpret_cast<float*>(a); };
    const int id = field<std::int32_t>(pickup, 0x134);
    if (id < 0 || id >= content::kRetailItems) return true;
    const std::uint32_t bless = std::uint32_t(flags[217]);
    const double mag = *reinterpret_cast<std::int32_t*>(0x76f1e4 + std::uintptr_t(id) * 0xb8);
    const char* what = "item";
    double value = 1;
    int ticks = 0;
    if (id >= 0x78) {
        if (h.stat(script::Host::Stat::Sp) >= 999999) return true;   // a full wallet: the game leaves the piece
        what = "sp";
        value = (std::max)(1.0, std::floor(mag * cell(0x76a5f8)));   // [H] without the difficulty's scale
    } else if (id >= 0x42 && id <= 0x47) {
        what = "hp";
        value = bless & 0x20000 ? std::floor(mag * 1.5) : bless & 0x1000 ? std::floor(mag * 1.25) : mag;
    } else if (id >= 0x48 && id <= 0x4d) {
        static const char* const kBuff[] = {"atk_buff", "def_buff", "mp_buff"};
        static const double kStep[] = {0.5, 0.3, 0.2};
        what = kBuff[(id - 0x48) / 2];
        value = kStep[(id - 0x48) / 2] * ((id - 0x48) % 2 + 1);
        ticks = bless & 0x800000 ? 2200 : bless & 0x10 ? 2000 : 1800;
    } else if (id == 0x72) {
        what = "boost";
        value = 30000;
    } else if (id == 0x73) {
        what = "oxygen";
        value = 6000;
    } else if (flags[id] >= 99) {
        return true;   // as many as the game holds: it leaves the pickup
    }
    const std::string room = h.room();
    float pos[3];
    for (int k = 0; k < 3; ++k) pos[k] = field<float>(pickup, kActPos + 4 * std::size_t(k));
    script::CombatDecision d;
    if (combat_on(Kind::Pickup)) {
        script::CombatQuery q;
        q.kind = Kind::Pickup;
        q.value = value;
        q.item = id;
        q.timer = ticks;
        q.num = {{"x", double(pos[0])}, {"y", double(pos[1])}, {"z", double(pos[2])}};
        q.str = {{"what", what}, {"name", h.item_name(id)}};
        d = combat(q);
    }
    using Action = script::CombatDecision::Action;
    const std::string w = what;
    // What a Replace can set here; the buffs and a plain item keep the game's own grant.
    const bool mine = d.action == Action::Suppress || (d.action == Action::Replace && (w == "sp" || w == "hp" || w == "boost" || w == "oxygen"));
    if (d.action == Action::Replace && mine) {
        value = std::clamp(d.value, 0.0, 999999.0);
        if (w == "sp") add_sp(int(value));
        else if (w == "hp") h.set_stat(script::Host::Stat::Hp, h.stat(script::Host::Stat::Hp) + value);
        else cell(w == "boost" ? 0x76c1b4 : 0x76c1b8) = float(value);
    }
    if (content_on()) content_pickup_taken(id, pos, room);
    {
        events::Event e{events::Kind::ItemObtained};
        e.item = id;
        e.count = 1;
        e.source = "pickup";
        emit(e, room);
    }
    events::Event e{events::Kind::PickupCollected};
    e.item = id;
    e.source = what;
    e.amount = d.action == Action::Suppress ? 0 : value;
    e.count = ticks;
    e.old_value = d.action == Action::Suppress ? 1 : 0;
    std::memcpy(e.pos, pos, sizeof pos);
    emit(e, room);
    return !mine;
}

void on_frame() {
    if (WaitForSingleObject(g_started, 0) != WAIT_OBJECT_0 || g_scripts.empty()) return;
    using clock = std::chrono::steady_clock;
    static clock::time_point last = clock::now();
    const clock::time_point now = clock::now();
    const double dt = std::chrono::duration<double>(now - last).count();
    last = now;
    if (!g_reload.empty()) {
        const std::set<std::string> ids = std::move(g_reload);
        g_reload.clear();
        for (const std::string& id : ids) reload(id);
    }
    for (auto& m : g_scripts) m->on_frame(dt);
}

}  // namespace ore
