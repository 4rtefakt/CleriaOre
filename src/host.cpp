// What a mod's script reaches of the retail game: cleria::script::Host over the memory of yso_win.exe
// (the build hooks.cpp checks; nothing here is read unless the hooks went in). What is not backed yet
// keeps script::Host's harmless default: images and icons, the stat modifiers, pickups, the save profiles.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <windows.h>
#include <shellapi.h>

#include "cleria/content.hpp"
#include "cleria/mod_events.hpp"
#include "cleria/modscript.hpp"
#include "ore.hpp"
#include "ui.hpp"

using namespace cleria;

namespace ore {

namespace {

template <class T>
T& at(std::uintptr_t address) { return *reinterpret_cast<T*>(address); }

constexpr std::uintptr_t kFlags = 0x76b91c;       // g_flags[0x200], int32
constexpr std::uintptr_t kWarps = 0x76c11c;       // the warp registry, bytes (right after g_flags)
constexpr std::uintptr_t kPlayer = 0x74c09c;      // the player's actor, null outside a room
constexpr std::uintptr_t kMaxHp = 0x76a72c, kHp = 0x76a730;            // floats
constexpr std::uintptr_t kExp = 0x76a748;         // float
constexpr std::uintptr_t kSp = 0x76a75c, kLevel = 0x76a760;            // ints
constexpr std::uintptr_t kRecalc = 0x76b914;      // |= 0x75: the derived state is rebuilt (as the pickup FUN_00585ff0 asks)
constexpr std::uintptr_t kItemMagnitude = 0x76f1e4;   // INVINFO in memory: the i32 magnitude of item id at + id * 0xb8
constexpr int kFlagCharacter = 150, kFlagWeapon = 0x94, kFlagScene = 0x1f9;
constexpr std::uintptr_t kMaxMp = 0x76a734, kMp = 0x76a738, kAtk = 0x76a73c, kDef = 0x76a740;   // floats of the same record
constexpr std::uintptr_t kExpNext = 0x76b910;     // float: the EXP total the next level needs
constexpr std::uintptr_t kBoost = 0x76c1b4;       // float: the Boost gauge, 30000 = full
constexpr std::uintptr_t kBuffs = 0x76b900;       // floats: the MP, Strength and Defense Drop buffs ...
constexpr std::uintptr_t kBuffTimers = 0x76c19c;  // ... and the ticks left of Strength, Defense, MP
constexpr std::uintptr_t kFix = 0x75ad90;         // int32[2]: the scripts' Fix counters; any set = the world is held
constexpr std::uintptr_t kUpgrades = 0x76a654;    // int32[54]: the gear-upgrade cells
constexpr std::uintptr_t kItems = 0x76f1c0;       // INVINFO in memory: 0xb8 a record, its name first
constexpr std::uintptr_t kSaveSlot = 0x74ac3e;    // byte: the slot the save writer's callers set first
constexpr int kFlagBoosting = 141, kFlagFight = 156, kFlagControl = 185, kFlagArena = 190, kFlagRun = 193, kFlagWarpTo = 137;

// The actors (CleriaCore's arch_re/WIRING.md): the live list, in spawn order, and what a record holds.
constexpr std::uintptr_t kActors = 0x730098, kActorCount = 0x73009c;
constexpr std::size_t kAKind = 0x60, kAMaxHp = 0x94, kAHp = 0x98, kAAtk = 0xa4, kADef = 0xa8, kALevel = 0xc8, kACast = 0x124, kAId = 0x128,
                      kAPos = 0x50, kAYaw = 0x314, kADead = 0x11;
constexpr std::uint32_t kKindEnemy = 0x8, kKindSolid = 0x80;
// The camera the game publishes each frame (camera_re/CAMERA.md): a left-handed look-at, Y up.
constexpr std::uintptr_t kCamTarget = 0x76a160, kCamEye = 0x76a16c, kCamFov = 0x7b38f4, kCamAspect = 0x7b38f8;
constexpr std::uintptr_t kRandom = 0x74622c;      // the game's one generator: xorshift128, four words

// The game's own functions (of the build hooks.cpp checks).
const auto set_level = reinterpret_cast<void(__fastcall*)(unsigned level)>(0x4200f0);         // level, EXP at its threshold, stats
const auto set_tier = reinterpret_cast<void(__fastcall*)(unsigned slot, int tier)>(0x4201d0); // a weapon slot's tier, stats
const auto level_ups = reinterpret_cast<void(__cdecl*)()>(0x420c40);                          // the level-ups the EXP total asks for, stats
const auto write_save = reinterpret_cast<int(__fastcall*)(char* file)>(0x42e9a0);             // hooks.cpp hooks it: the sidecar follows
const auto request_scene = reinterpret_cast<int(__fastcall*)(int scene, int force)>(0x430ef0);   // the next frame tears the room down and loads this one
const auto play_bgm = reinterpret_cast<void(__fastcall*)(int id, int param)>(0x432ee0);          // Bgm_Play: -1 = the room's own
const auto stop_bgm = reinterpret_cast<void(__fastcall*)(int fade)>(0x432fc0);                   // Bgm_Stop
const auto sync_gauges = reinterpret_cast<void(__cdecl*)()>(0x4fa0e0);                        // the HUD's HP and MP gauges take the record's values
const auto play_sound = reinterpret_cast<void(__fastcall*)(int id, int volume)>(0x43a040);    // a sound of EFFECTDEF, 0x400 = its own volume

// FUN_00475710(ECX cast, EDX yaw in degrees; x, y, z, a stat argument; the caller pops): a monster of CASTINFO's
// row `cast` built as the game's own spawners build one (the factory, the stats of its ENEMYINFO record).
__declspec(naked) char* __cdecl spawn_monster(int /*cast*/, int /*yaw*/, const float* /*pos*/) {
    __asm {
        push ebp
        mov  ebp, esp
        mov  eax, dword ptr [ebp + 16]
        push 0
        push dword ptr [eax + 8]
        push dword ptr [eax + 4]
        push dword ptr [eax]
        mov  ecx, dword ptr [ebp + 8]
        mov  edx, dword ptr [ebp + 12]
        mov  eax, 0x475710
        call eax
        add  esp, 16
        pop  ebp
        ret
    }
}

// The Crystal's warp, as its menu confirms one (0x5accaf..0x5acd16): the target statue is already in
// g_flags[137]. From the retail Archipelago mod (hook_ap.cpp do_warp_native), where it is proven.
__declspec(naked) void warp_to_target() {
    __asm {
        push esi
        push edi
        push ebx
        xor  edi, edi
        mov  ebx, 0x0074e238
        mov  esi, dword ptr [ebx]
        test esi, esi                          // not built yet (the title): no warp
        je   done
        test byte ptr [esi + 8], 0x80
        jne  fade
        mov  ebx, 0x006d6067
        cmp  byte ptr [ebx], 0
        je   go
        xor  ecx, ecx
        mov  eax, 0x00439ee0
        call eax
        test eax, eax
        js   go
    fade:
        mov  edx, dword ptr [esi + 8]
        xor  ecx, ecx
        push 0x400
        and  edx, 1
        mov  eax, 0x005ce2a0
        call eax
        add  esp, 4
    go:
        mov  ebx, 0x0074c09c
        push dword ptr [ebx]                   // the player
        xor  edx, edx
        mov  ecx, 0x006a7620
        push 0
        mov  eax, 0x00434970                   // the warp
        call eax
        add  esp, 8
        mov  ebx, 0x0074c09c
        mov  eax, dword ptr [ebx]
        mov  ebx, 0x00739160
        mov  byte ptr [ebx], 1
        test eax, eax
        je   done
        mov  word ptr [eax + 0x4c8], di
    done:
        pop  ebx
        pop  edi
        pop  esi
        ret
    }
}

struct RetailHost : script::Host {
    void log(const std::string& mod, const std::string& text) override { ore::log_script(mod, text); }
    std::map<std::string, std::string> bus_;   // cleria.bus's process-wide values
    void notice(const std::string& mod, const std::string& head, const std::string& text) override {
        ore::log("[lua:%s] notice: %s %s", mod.c_str(), head.c_str(), text.c_str());
        ui_notice(head, text);
    }
    void failed(const std::string& mod, const std::string& error) override { ore::log("[lua:%s] disabled: %s", mod.c_str(), error.c_str()); }

    std::int32_t* flags() override { return hooked() ? &at<std::int32_t>(kFlags) : nullptr; }
    std::uint8_t* warps() override { return hooked() ? &at<std::uint8_t>(kWarps) : nullptr; }
    bool in_game() override { return hooked() && flags()[kFlagScene] > 0 && at<void*>(kPlayer) != nullptr; }
    int character() override { return hooked() ? flags()[kFlagCharacter] : 1; }
    // g_flags[0x1F9] is the loaded room as a number: 1004 in S_1004, 0 before the first room.
    std::string room() override {
        const int scene = hooked() ? flags()[kFlagScene] : 0;
        if (scene <= 0) return {};
        char name[16];
        std::snprintf(name, sizeof name, "S_%04d", scene);
        return name;
    }
    void emit(const events::Event& e) override { events::bus().emit(e); }
    // The game's scene table in memory (SCENELIST.SL: the array at DAT_0074c124, DAT_0074e230 records of 0x1c8; the
    // id at +0, the path "s_10\s_1001\" at +4, the display string at +0xc4): a room's record.
    static const char* scene_rec(const std::string& room) {
        const char* recs = hooked() ? at<const char*>(0x74c124) : nullptr;
        const int count = recs ? at<std::int32_t>(0x74e230) : 0;
        if (room.empty() || count <= 0 || count > 4096) return nullptr;
        for (int i = 0; i < count; ++i) {
            const char* rec = recs + std::size_t(i) * 0x1c8;
            std::string path(rec + 4, strnlen(rec + 4, 0xc0));
            for (char& c : path) c = char(std::toupper(static_cast<unsigned char>(c)));
            if (path.find(room + '\\') != std::string::npos) return rec;
        }
        return nullptr;
    }
    // The room's own name, after its ':' as CleriaCore gives it.
    std::string room_title() override {
        const char* rec = scene_rec(room());
        if (!rec) return {};
        const std::string display(rec + 0xc4, strnlen(rec + 0xc4, 0x100));
        const std::size_t k = display.find(':');
        return k != std::string::npos && k + 2 <= display.size() ? display.substr(k + 2) : display;
    }
    bool frozen() override { return hooked() && (at<std::int32_t>(kFix) | at<std::int32_t>(kFix + 4)) != 0; }
    std::int32_t* upgrades() override { return hooked() ? &at<std::int32_t>(kUpgrades) : nullptr; }
    std::map<std::string, std::pair<int, std::string>> chests() override { return ore::chests(); }
    int new_item_count(int id) override { return sidecar().item(id); }
    std::string item_name(int id) override {
        if (std::string n = content_item_name(id); !n.empty()) return n;
        if (!hooked() || id < 0 || id >= content::kRetailItems) return {};
        const char* name = &at<char>(kItems + std::uintptr_t(id) * 0xb8);
        return std::string(name, strnlen(name, 0x20));
    }
    std::filesystem::path data_dir(const std::string& mod) override { return dir() / "modsdata" / mod; }
    // ---- API 7: the mods see each other, and the player's machine ----
    int bus_publish(const std::string& from, const std::string& topic, const std::string& json_text) override {
        int n = 0;
        for (std::size_t i = 0; i < scripts().size(); ++i)   // by index: a handler may fail a mod
            if (scripts()[i]->id() != from) n += scripts()[i]->on_bus(topic, json_text, from);
        return n;
    }
    std::optional<std::string> bus_get(const std::string& key) override {
        const auto it = bus_.find(key);
        return it == bus_.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
    void bus_set(const std::string& key, const std::string& json_text) override {
        if (json_text.empty()) bus_.erase(key);
        else if (bus_.size() < 4096 || bus_.count(key)) bus_[key] = json_text;
    }
    bool run_command(const std::string& name, const std::string& args) override {
        for (auto& m : scripts())
            if (m->ok() && m->has_command(name)) { m->command(name, args); return true; }
        return false;
    }
    std::vector<std::string> all_commands() override {
        std::vector<std::string> v;
        for (auto& m : scripts())
            if (m->ok())
                for (const std::string& c : m->command_names()) v.push_back(c);
        std::sort(v.begin(), v.end());
        return v;
    }
    std::vector<std::string> log_lines(int n) override {
        const auto& t = log_tail();
        const std::size_t k = (std::min)(std::size_t((std::max)(n, 0)), t.size());
        return std::vector<std::string>(t.end() - std::ptrdiff_t(k), t.end());
    }
    bool request_reload(const std::string& mod) override { ore::request_reload(mod); return true; }
    bool set_clipboard(const std::string& text) override {
        if (!ImGui::GetCurrentContext()) return false;
        ImGui::SetClipboardText(text.c_str());
        return true;
    }
    // Only reached from a page's button (the mod layer checks it): the player pressed it. A web address only.
    bool open_url(const std::string& mod, const std::string& url) override {
        ore::log("[lua:%s] opens %s", mod.c_str(), url.c_str());
        if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) return false;
        return reinterpret_cast<INT_PTR>(ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
    }
    // What the game is doing, as far as the retail host tells: its own menus, book and dialogs are "cutscene"
    // here when they hold the world, "play" otherwise.
    std::string game_state() override {
        if (!in_game()) return "title";
        if (ui_panel_open()) return "panel";
        if (const script::Mod* m = window_mod(); m && m->window()->pause) return "window";
        if (paused()) return "held";
        if (at<float>(kHp) < 1) return "gameover";
        return frozen() ? "cutscene" : "play";
    }
    bool key_down(const std::string& name) override { return ui_key_down(name, false); }
    bool pad_down(const std::string& name) override { return ui_key_down(name, true); }
    bool key_known(const std::string& name, bool pad) override { return ui_key_known(name, pad); }
    void set_modifier(const std::string& mod, int stat, double add, double mul) override { ore::set_modifier(mod, stat, add, mul); }
    std::pair<double, double> modifier(const std::string& mod, int stat) override { return ore::modifier(mod, stat); }
    // ---- cleria.world: the room's monsters ----
    static char* actor_at(int i) { return at<char**>(kActors)[i]; }
    template <class T>
    static T& in(char* actor, std::size_t off) { return *reinterpret_cast<T*>(actor + off); }
    static bool monster(char* a) {
        return a && !puppet_set().count(a) && (in<std::uint32_t>(a, kAKind) & kKindEnemy) && in<std::int32_t>(a, kACast) != 13 && in<float>(a, kAHp) >= 1.0f &&
               !(in<std::uint8_t>(a, kADead) & 2);
    }
    // A monster by the id world.actors() gave it: its place in the live list + 1 would move as others die, so the
    // id is the record's own address.
    static int actor_id(char* a) { return in<std::int32_t>(a, 0x14); }
    // The actors whose handler a mod holds. A room's actors are rebuilt with it: the set is the room's.
    static std::set<const void*>& held_set() { static std::set<const void*> s; return s; }
    // The puppets: actors a mod builds and moves itself (another player, a prop). Held from their first step.
    static std::set<const void*>& puppet_set() { static std::set<const void*> s; return s; }
    static void held_room() {                                                     // another room: nobody is held, no puppet is left
        static std::int32_t room = 0;
        if (room == at<std::int32_t>(0x76b91c + 4 * kFlagScene)) return;
        held_set().clear();
        puppet_set().clear();
        room = at<std::int32_t>(0x76b91c + 4 * kFlagScene);
    }
    // A cast's sprite sheet. CASTINFO +0x68 (the table at DAT_0074c11c, 0x200 a record, 186 records) is the cast's
    // animation script, an index of the ASO name table (185 names at 0x68a510: "m_100"); the sheet is the same name
    // in the AIA name table (269 names at 0x68a800). -1: none.
    static int cast_sheet(int cast) {
        const char* info = at<const char*>(0x74c11c);
        if (!info || cast < 0 || cast >= 186) return -1;
        const std::int32_t aso = *reinterpret_cast<const std::int32_t*>(info + std::size_t(cast) * 0x200 + 0x68);
        if (aso < 0 || aso >= 185) return -1;
        const char* name = at<const char*>(0x68a510 + 4 * aso);
        for (int i = 0; i < 269; ++i)
            if (const char* s = at<const char*>(0x68a800 + 4 * i); s && _stricmp(s, name) == 0) return i;
        return -1;
    }
    static char* find_puppet(int id) {
        held_room();
        const int n = at<std::int32_t>(kActorCount);
        for (int i = 0; i < n && i < 256; ++i)
            if (char* a = actor_at(i); a && actor_id(a) == id && puppet_set().count(a) && !(in<std::uint8_t>(a, kADead) & 2)) return a;
        return nullptr;
    }
    static void hold(char* a, bool on) {
        held_room();
        if (on) held_set().insert(a);
        else held_set().erase(a);
    }
    static std::map<const void*, std::string>& spawned_by() { static std::map<const void*, std::string> m; return m; }   // world.spawn's, until world_tick
    // The monsters built since the last step: each passes the mods' "spawn" filter once (the multipliers of its
    // numbers, or "suppress": it is removed), then enemy_spawned. The filter comes after the game built the
    // monster, so it cannot change its cast or its level.
    static void new_monsters() {
        static std::set<int> known;
        static std::int32_t room = 0;
        if (room != at<std::int32_t>(0x76b91c + 4 * kFlagScene)) { known.clear(); spawned_by().clear(); room = at<std::int32_t>(0x76b91c + 4 * kFlagScene); }
        const int n = at<std::int32_t>(kActorCount);
        for (int i = 0; i < n && i < 256; ++i) {
            char* a = actor_at(i);
            if (!monster(a) || !known.insert(actor_id(a)).second) continue;
            const float* p = &in<float>(a, kAPos);
            if (world_on("spawn")) {
                script::WorldQuery q;
                q.kind = "spawn";
                q.num = {{"cast", double(in<std::int32_t>(a, kACast))}, {"level", double(in<std::int32_t>(a, kALevel))}, {"id", double(actor_id(a))},
                         {"x", double(p[0])}, {"y", double(p[1])}, {"z", double(p[2])}, {"group", 0.0}};
                const char* info = at<const char*>(0x74c11c);
                q.str = {{"name", info ? std::string(info + std::size_t(in<std::int32_t>(a, kACast)) * 0x200) : std::string()}};
                const auto by = spawned_by().find(a);
                q.str.emplace_back("by", by == spawned_by().end() ? std::string() : by->second);
                const script::WorldDecision d = world_filter(q);
                if (d.action == script::WorldDecision::Action::Suppress) {
                    in<std::uint32_t>(a, kAKind) &= ~kKindSolid;
                    in<std::uint8_t>(a, kADead) |= 2;
                    continue;
                }
                if (d.action == script::WorldDecision::Action::Replace) {
                    auto mul = [&](const char* k) { return float(std::clamp(d.n(k, 1.0), 0.01, 100.0)); };
                    in<float>(a, kAMaxHp) = (std::max)(1.0f, in<float>(a, kAMaxHp) * mul("hp"));
                    in<float>(a, kAHp) = in<float>(a, kAMaxHp);
                    in<float>(a, kAAtk) *= mul("atk");
                    in<float>(a, kADef) *= mul("def");
                    in<float>(a, 0xb0) *= mul("exp");
                    in<std::int32_t>(a, 0xc4) = int(float(in<std::int32_t>(a, 0xc4)) * mul("sp"));
                }
            }
            emit_named(events::Event::named("enemy_spawned").with("actor", actor_id(a)).with("cast", in<std::int32_t>(a, kACast))
                           .with("level", in<std::int32_t>(a, kALevel)).with("x", p[0]).with("y", p[1]).with("z", p[2]));
        }
        spawned_by().clear();
    }
    static char* find_monster(int id) {
        const int n = at<std::int32_t>(kActorCount);
        for (int i = 0; i < n && i < 256; ++i)
            if (char* a = actor_at(i); a && actor_id(a) == id && monster(a)) return a;
        return nullptr;
    }
    std::vector<ActorInfo> actors() override {
        std::vector<ActorInfo> v;
        if (!in_game()) return v;
        const int n = at<std::int32_t>(kActorCount);
        for (int i = 0; i < n && i < 256; ++i) {
            char* a = actor_at(i);
            if (!monster(a)) continue;
            ActorInfo o;
            o.id = actor_id(a);
            o.cast = in<std::int32_t>(a, kACast);
            o.level = in<std::int32_t>(a, kALevel);
            o.hp = in<float>(a, kAHp);
            o.max_hp = in<float>(a, kAMaxHp);
            o.atk = in<float>(a, kAAtk);
            o.def = in<float>(a, kADef);
            std::memcpy(o.pos, a + kAPos, sizeof o.pos);
            o.yaw = in<float>(a, kAYaw) * 57.29577951f;
            o.hittable = (in<std::uint32_t>(a, kAKind) & kKindSolid) != 0;
            v.push_back(o);
        }
        return v;
    }
    // A world point on the screen: the game's view (eye, target, Y up) and its perspective (the vertical field of
    // view and the aspect it gives Direct3D), as fractions of the picture from its top left.
    bool to_screen(float x, float y, float z, float* sx, float* sy) override {
        if (!in_game()) return false;
        const float* eye = &at<float>(kCamEye);
        const float* tgt = &at<float>(kCamTarget);
        float f[3] = {tgt[0] - eye[0], tgt[1] - eye[1], tgt[2] - eye[2]};
        const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (fl < 1e-4f) return false;
        for (float& c : f) c /= fl;
        float r[3] = {f[2], 0.0f, -f[0]};                         // up x forward, up = (0, 1, 0)
        const float rl = std::sqrt(r[0] * r[0] + r[2] * r[2]);
        if (rl < 1e-4f) return false;
        r[0] /= rl; r[2] /= rl;
        const float u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};   // forward x right
        const float d[3] = {x - eye[0], y - eye[1], z - eye[2]};
        const float cz = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
        if (cz <= 0.05f) return false;                            // behind the camera
        const float cx = d[0] * r[0] + d[1] * r[1] + d[2] * r[2], cy = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
        const float t = std::tan(at<float>(kCamFov) * 0.5f), aspect = at<float>(kCamAspect);
        if (t < 1e-4f || aspect < 1e-4f) return false;
        *sx = 0.5f + 0.5f * cx / (cz * t * aspect);
        *sy = 0.5f - 0.5f * cy / (cz * t);
        return true;
    }
    // The writes of API 7 the retail host backs; any other answers false, as with no game.
    bool game_op(const std::string& mod, const std::string& op, const std::vector<double>& n, const std::string& text, std::vector<double>* out) override {
        auto arg = [&](std::size_t i, double d = 0.0) { return i < n.size() ? n[i] : d; };
        if (op == "pause") { set_pause(mod, arg(0) != 0); return true; }          // these two hold with or without a game
        if (op == "speed") { set_speed(mod, arg(0, 1.0)); return true; }
        if (!in_game()) return false;
        char* player = at<char*>(kPlayer);
        const bool alive = at<float>(kHp) >= 1;
        if (op == "set_position") {
            if (!alive || n.size() < 3) return false;
            const float p[3] = {float(n[0]), float(n[1]), float(n[2])};
            std::memcpy(player + kAPos, p, sizeof p);
            std::memcpy(player + 0x3d4, p, sizeof p);                             // the previous frame's, so nothing sweeps between the two
            std::memcpy(player + 0x808, p, sizeof p);                             // the last good place
            in<float>(player, 0x330) = 0;                                         // no fall or jump carried over
            if (n.size() >= 4) in<float>(player, kAYaw) = float(n[3]) / 57.29577951f;
            return true;
        }
        if (op == "set_mp") {
            at<float>(kMp) = float(std::clamp(arg(0), 0.0, double(at<float>(kMaxMp))));
            in<float>(player, 0xa0) = at<float>(kMp);
            sync_gauges();
            return true;
        }
        if (op == "set_boost") { at<float>(kBoost) = float(std::clamp(arg(0), 0.0, 30000.0)); return true; }
        if (op == "set_exp") {
            at<float>(kExp) = float((std::max)(arg(0), 0.0));
            level_ups();
            return true;
        }
        if (op == "heal" || op == "hurt") {                                       // HP given or taken as it is; 0 is the death
            if (!alive || arg(0) < 0) return false;
            set_stat(Stat::Hp, at<float>(kHp) + (op == "heal" ? arg(0) : -arg(0)));
            return true;
        }
        if (op == "seed_random") {
            std::uint32_t* rs = &at<std::uint32_t>(kRandom);
            const std::uint32_t s = std::uint32_t(arg(0));
            rs[0] = s ? s : 1u; rs[1] = 362436069u; rs[2] = 521288629u; rs[3] = 88675123u;
            return true;
        }
        if (op == "set_actor") {                                                  // hp (at least 1), position, heading
            char* a = find_monster(int(arg(0, -1)));
            if (!a) return false;
            if (n.size() > 1 && std::isfinite(n[1])) in<float>(a, kAHp) = std::clamp(float(n[1]), 1.0f, (std::max)(1.0f, in<float>(a, kAMaxHp)));
            for (std::size_t k = 0; k < 3; ++k)
                if (n.size() > 2 + k && std::isfinite(n[2 + k])) in<float>(a, kAPos + 4 * k) = float(n[2 + k]);
            if (n.size() > 5 && std::isfinite(n[5])) in<float>(a, kAYaw) = float(n[5]) / 57.29577951f;
            return true;
        }
        if (op == "remove_actor") {                                               // gone at the game's next sweep: no EXP, no drops
            char* a = find_monster(int(arg(0, -1)));
            if (!a) return false;
            in<std::uint32_t>(a, kAKind) &= ~kKindSolid;                          // as the game's own kill paths do first
            in<std::uint8_t>(a, kADead) |= 2;
            return true;
        }
        if (op == "spawn_actor") {
            // (cast, x, y, z, level, yaw, group). Any monster cast: the game loads a sheet when it is first drawn.
            if (n.size() < 4 || !out) return false;
            const int cast = int(n[0]);
            if (cast < 14 || cast >= 186 || cast_sheet(cast) < 0 || at<std::int32_t>(kActorCount) >= 120) return false;
            const float pos[3] = {float(n[1]), float(n[2]), float(n[3])};
            char* a = spawn_monster(cast, int(arg(5)), pos);
            if (!a) return false;
            spawned_by()[a] = mod;
            *out = {double(actor_id(a))};
            return true;
        }
        if (op == "se") { play_sound(int(arg(0)), int(std::clamp(arg(1, 1.0), 0.0, 4.0) * 0x400)); return true; }
        if (op == "goto_room") {                                                  // as a script's ChangeScene (0x97): staged, done at the next frame
            const char* rec = scene_rec(text);
            if (!rec || !alive) return false;
            return request_scene(*reinterpret_cast<const std::uint16_t*>(rec), 0) == 0;   // refused while another change is pending
        }
        // A hit of a script's on a monster: the HP it takes and the event bits the game's own applier (FUN_004fbb40)
        // leaves at +0x3bc for the monster's brain: 4 struck, 2 hurt, 0x10 killed (HP 0). The brain plays the flinch
        // or the death and its drops. No EXP or SP: the applier credits those to the hitter itself.
        if (op == "hit") {
            char* a = find_monster(int(arg(0, -1)));
            const int dmg = int(arg(1));
            if (!alive || !a || dmg < 0) return false;
            in<std::uint32_t>(a, 0x3bc) |= dmg > 0 ? 6u : 4u;
            if (in<float>(a, kAHp) - float(dmg) < 1.0f) {
                in<float>(a, kAHp) = 0;
                in<std::uint32_t>(a, 0x3bc) |= 0x10;
            } else {
                in<float>(a, kAHp) -= float(dmg);
            }
            return true;
        }
        // A hit on the player priced as the game prices one (FUN_004fa550, its first lines): the attacker's ATK
        // against the player's DEF. Skipped under the hurt invulnerability (+0x734), which it then gives (12 ticks,
        // a medium hit's). [H] no flinch or knockback: those are the hurt applier's (FUN_004fd350) own.
        if (op == "strike") {
            if (!alive || arg(0) <= 0) return false;
            if (in<std::int32_t>(player, 0x734) > 0) return true;
            const float atk = float(arg(0)), def = in<float>(player, kADef);
            const float d = atk * ((atk - def * at<float>(0x6bd414)) + at<float>(0x6bd928) + float(arg(1))) / at<float>(0x6bd9b0);
            const int dmg = (std::max)(1, int(d));
            in<std::int32_t>(player, 0x734) = 12;
            set_stat(Stat::Hp, at<float>(kHp) - float(dmg));
            return true;
        }
        if (op == "invulnerable") {                                               // the hurt invulnerability, in ticks: raised, never cut
            if (!alive) return false;
            in<std::int32_t>(player, 0x734) = (std::max)(in<std::int32_t>(player, 0x734), int(arg(0)));
            return true;
        }
        if (op == "boss_bonus") {
            const double pool = boss_bonus(arg(0, -1));
            if (out) *out = {pool};
            return true;
        }
        if (op == "boss") {                                                       // the boss gauge (its widget at 0x6d5fd4): owner, HP, max
            const int owner = at<std::int32_t>(0x6d5fd8);
            if (!out || !owner) return false;
            double id = 0;
            const int count = at<std::int32_t>(kActorCount);
            for (int i = 0; i < count && i < 256; ++i)
                if (char* a = actor_at(i); a && actor_id(a) == owner && monster(a)) id = double(owner);
            *out = {id, double(at<float>(0x6d5fd4 + 0x30)), double(at<float>(0x6d5fd4 + 0x38))};
            return true;
        }
        if (op == "actors_raw") {                                                 // for study: every record of the live list, to the log
            const int count = at<std::int32_t>(kActorCount);
            for (int i = 0; i < count && i < 256; ++i)
                if (char* a = actor_at(i))
                    ore::log("[actors] %2d %p vt %08x kind %08x cast %3d id %5d hp %.0f/%.0f flags %08x dead %02x pos %.1f %.1f %.1f", i, a,
                        in<std::uint32_t>(a, 0), in<std::uint32_t>(a, kAKind), in<std::int32_t>(a, kACast), in<std::int32_t>(a, 0x14),
                        in<float>(a, kAHp), in<float>(a, kAMaxHp), in<std::uint32_t>(a, 0x12c), in<std::uint8_t>(a, kADead),
                        in<float>(a, kAPos), in<float>(a, kAPos + 4), in<float>(a, kAPos + 8)),
                    ore::log("[actors]    +0x24c %d, state: sheet %d tag %d frame %d, +0x8f0 %08x +0x978 %d", in<std::int32_t>(a, 0x24c), in<std::int32_t>(a, 0x97c),
                             in<std::int32_t>(a, 0x9b0), in<std::int32_t>(a, 0x9b8), in<std::uint32_t>(a, 0x8f0), in<std::int32_t>(a, 0x978));
            for (int i = 0; i < count && i < 256; ++i)
                if (char* a = actor_at(i)) {
                    const unsigned char* d = in<const unsigned char*>(a, 0x8f0);
                    std::string hex;
                    char t[4];
                    for (int k = 0; d && k < 0xb0; ++k) { std::snprintf(t, sizeof t, "%02x", d[k]); hex += t; if (k % 4 == 3) hex += ' '; }
                    ore::log("[actors] def of cast %d: %s", in<std::int32_t>(a, kACast), hex.c_str());
                }
            ore::log("[actors] gauge owner %d; sheets of casts 13 %d, 14 %d, 19 %d, 176 %d", at<std::int32_t>(0x6d5fd8), cast_sheet(13), cast_sheet(14),
                     cast_sheet(19), cast_sheet(176));
            return true;
        }
        // An actor's animation state is the object embedded at +0x8f0 (FUN_00446220 and FUN_00446340 run on it): the sheet (an index of the
        // AIA name table) at +0x8c, the animation tag at +0xc0, the frame at +0xc8; its aura (anime_SetAura) at +0x2b4.
        if (op == "pose") {
            if (!out) return false;
            *out = {double(in<float>(player, kAPos)), double(in<float>(player, kAPos + 4)), double(in<float>(player, kAPos + 8)),
                    double(in<float>(player, kAYaw) * 57.29577951f), double(in<std::int32_t>(player, 0x9b0)), double(in<std::int32_t>(player, 0x9b8)),
                    double(in<std::int32_t>(player, 0x97c)), 0.0, double(in<std::uint32_t>(player, 0x2b4))};
            return true;
        }
        if (op == "familiars") {                                                  // Hugo's Eyes: the actors of cast 176
            if (!out) return false;
            out->clear();
            const int count = at<std::int32_t>(kActorCount);
            for (int i = 0; i < count && i < 256; ++i) {
                char* a = actor_at(i);
                if (!a || in<std::int32_t>(a, kACast) != 176 || (in<std::uint8_t>(a, kADead) & 2)) continue;
                for (double v : {double(176), double(in<float>(a, kAPos)), double(in<float>(a, kAPos + 4)), double(in<float>(a, kAPos + 8)),
                                 double(in<float>(a, kAYaw) * 57.29577951f), double(in<std::int32_t>(a, 0x9b0)), double(in<std::int32_t>(a, 0x9b8)),
                                 double(in<std::int32_t>(a, 0x97c))})
                    out->push_back(v);
            }
            return true;
        }
        // A monster's numbers scaled, and its handler held or let go (world.form). The look (scale, tint, name) is
        // not backed.
        if (op == "actor_form") {
            char* a = find_monster(int(arg(0, -1)));
            if (!a) return false;
            auto mul = [&](std::size_t i) { const double v = arg(i, NAN); return std::isfinite(v) ? float(std::clamp(v, 0.01, 1000.0)) : 1.0f; };
            if (mul(1) != 1.0f) { in<float>(a, kAMaxHp) = (std::max)(1.0f, in<float>(a, kAMaxHp) * mul(1)); in<float>(a, kAHp) = in<float>(a, kAMaxHp); }
            in<float>(a, kAAtk) *= mul(2);
            in<float>(a, kADef) *= mul(3);
            in<float>(a, 0xb0) *= mul(4);
            in<std::int32_t>(a, 0xc4) = int(float(in<std::int32_t>(a, 0xc4)) * mul(5));
            if (std::isfinite(arg(8, NAN))) hold(a, arg(8) == 0);
            return true;
        }
        if (op == "world_snapshot") {                                             // 12 numbers a monster: id cast level x y z yaw hp max_hp tag frame sheet
            if (!out) return false;
            out->clear();
            const int count = at<std::int32_t>(kActorCount);
            for (int i = 0; i < count && i < 256; ++i) {
                char* a = actor_at(i);
                if (!monster(a)) continue;
                out->insert(out->end(), {double(actor_id(a)), double(in<std::int32_t>(a, kACast)), double(in<std::int32_t>(a, kALevel)),
                                         double(in<float>(a, kAPos)), double(in<float>(a, kAPos + 4)), double(in<float>(a, kAPos + 8)),
                                         double(in<float>(a, kAYaw) * 57.29577951f), double(in<float>(a, kAHp)), double(in<float>(a, kAMaxHp)),
                                         double(in<std::int32_t>(a, 0x9b0)), double(in<std::int32_t>(a, 0x9b8)), double(in<std::int32_t>(a, 0x97c))});
            }
            return true;
        }
        // The room's monsters made to match a snapshot (flags: 1 hold their handlers, 2 the others fall; then 12 a
        // monster) -> how many matched. A monster that is in the snapshot and not here is not built: an id is the
        // game's own and cannot be given to a new actor.
        if (op == "world_apply") {
            if (!alive || n.empty()) return false;
            const bool held = (int(n[0]) & 1) != 0, sweep = (int(n[0]) & 2) != 0;
            auto fall = [&](char* a) {                                            // it died over there: its death here, no EXP, no SP
                hold(a, false);
                in<std::uint32_t>(a, 0x12c) |= 0x4000000u;
                in<float>(a, kAHp) = 0;
                in<std::uint32_t>(a, 0x3bc) |= 0x16u;
            };
            std::set<int> seen;
            int done = 0;
            for (std::size_t i = 1; i + 12 <= n.size(); i += 12) {
                const int id = int(n[i]);
                char* a = find_monster(id);
                if (!a || in<std::int32_t>(a, kACast) != int(n[i + 1])) continue;
                seen.insert(id);
                if (n[i + 7] < 1.0) { fall(a); continue; }
                const float p[3] = {float(n[i + 3]), float(n[i + 4]), float(n[i + 5])};
                std::memcpy(a + kAPos, p, sizeof p);
                std::memcpy(a + 0x3d4, p, sizeof p);                              // the previous step's: the draw goes between the two
                in<float>(a, kAYaw) = float(n[i + 6]) / 57.29577951f;
                in<float>(a, kAMaxHp) = (std::max)(1.0f, float(n[i + 8]));
                in<float>(a, kAHp) = (std::min)(float(n[i + 7]), in<float>(a, kAMaxHp));
                if (held) {
                    hold(a, true);
                    if (n[i + 9] >= 0) { in<std::int32_t>(a, 0x9b0) = int(n[i + 9]); in<std::int32_t>(a, 0x9b8) = (std::max)(0, int(n[i + 10])); }
                    if (n[i + 11] >= 0 && n[i + 11] < 269) in<std::int32_t>(a, 0x97c) = int(n[i + 11]);   // the AIA name table has 269 entries
                }
                ++done;
            }
            if (sweep) {
                const int count = at<std::int32_t>(kActorCount);
                for (int i = 0; i < count && i < 256; ++i)
                    if (char* a = actor_at(i); monster(a) && !seen.count(actor_id(a))) fall(a);
            }
            if (out) *out = {double(done)};
            return true;
        }
        // A puppet is an actor of the game's own, built by its monster factory, whose handler never runs (it is held)
        // and whose animation state the mod writes: the sheet (15 Yunica, 16 Hugo, 17 Toal; the game loads a sheet
        // when it is first drawn), the tag, the frame. It is not solid and not an enemy: nothing hits it.
        if (op == "puppet_spawn" || op == "prop_spawn") {                         // (character 1..3 | cast, x, y, z, yaw) -> its id
            if (!alive || n.size() < 4 || !out || at<std::int32_t>(kActorCount) >= 120) return false;
            const bool prop = op == "prop_spawn";
            const int sheet = prop ? cast_sheet(int(n[0])) : 14 + std::clamp(int(n[0]), 1, 3);
            if (sheet < 0) return false;
            const float pos[3] = {float(n[1]), float(n[2]), float(n[3])};
            // Built as the player's own cast, whose factory makes nothing else (an Eye's adds two more Eyes).
            char* a = spawn_monster(in<std::int32_t>(player, kACast), int(arg(4)), pos);
            if (!a) return false;
            in<float>(a, kAHp) = in<float>(a, kAMaxHp) = 1.0f;                    // the player's cast has no monster record: its HP is noise
            held_room();
            puppet_set().insert(a);
            held_set().insert(a);
            in<std::uint32_t>(a, kAKind) &= ~(kKindSolid | kKindEnemy);
            std::memcpy(a + 0x3d4, pos, sizeof pos);
            in<std::int32_t>(a, 0x97c) = sheet;
            in<std::int32_t>(a, 0x9b0) = 2;                                       // standing
            in<std::int32_t>(a, 0x9b8) = 0;
            *out = {double(actor_id(a))};
            return true;
        }
        if (op == "puppet_set" || op == "puppet_remove") {
            char* a = find_puppet(int(arg(0, -1)));
            if (!a) return false;
            if (op == "puppet_remove") {
                in<std::uint8_t>(a, kADead) |= 2;
                puppet_set().erase(a);
                held_set().erase(a);
                return true;
            }
            for (std::size_t k = 0; k < 3; ++k)
                if (n.size() > 1 + k && std::isfinite(n[1 + k])) in<float>(a, kAPos + 4 * k) = in<float>(a, 0x3d4 + 4 * k) = float(n[1 + k]);
            if (n.size() > 4 && std::isfinite(n[4])) in<float>(a, kAYaw) = float(n[4]) / 57.29577951f;
            if (n.size() > 5 && std::isfinite(n[5])) in<std::int32_t>(a, 0x9b0) = int(n[5]);
            if (n.size() > 6 && std::isfinite(n[6])) in<std::int32_t>(a, 0x9b8) = (std::max)(0, int(n[6]));
            if (n.size() > 7 && std::isfinite(n[7]) && n[7] >= 0 && n[7] < 269) in<std::int32_t>(a, 0x97c) = int(n[7]);
            return true;
        }
        if (op == "show_bullet") return alive && n.size() >= 19 && ore::show_bullet(n.data());
        // The equipment (progress_re/INVENTORY.md): the weapon, armor and leggings are g_flags 151..153; the accessory,
        // the held item and the bracelet are the three ints at 0x76a628 (-1 = none).
        if (op == "equipment") {
            if (!out) return false;
            *out = {double(flags()[151]), double(flags()[152]), double(flags()[153]), double(at<std::int32_t>(0x76a628)),
                    double(at<std::int32_t>(0x76a630)), double(at<std::int32_t>(0x76a62c))};
            return true;
        }
        if (op == "equip") {                                                      // an owned item of the slot's own range, or -1 where the game allows none
            const int slot = int(arg(0)), item = int(arg(1, -1));
            const bool owned = item >= 0 && item < 0x80 && flags()[item] >= 1;
            auto is = [&](int lo, int hi) { return item >= lo && item <= hi && owned; };
            switch (slot) {
                case 0: if (!is(0, 5)) return false; flags()[151] = item; break;
                case 1: if (!is(6, 29)) return false; flags()[152] = item; break;
                case 2: if (!is(30, 53)) return false; flags()[153] = item; break;
                case 3: if (item != -1 && !is(54, 65)) return false; at<std::int32_t>(0x76a628) = item; break;
                case 4: if (item != -1 && !is(116, 119)) return false; at<std::int32_t>(0x76a630) = item; break;
                case 5: if (item != -1 && !is(66, 115)) return false; at<std::int32_t>(0x76a62c) = item; break;
                default: return false;
            }
            recompute_stats();
            return true;
        }
        // The ailments (mech_re/STATUS_AILMENTS.md): the active mask at 0x76c15c (poison 1, heavy 8, confuse 0x10,
        // curse 0x20) and a timer each from 0x76c160, in ticks. Set here with its full duration and none of the
        // hit's show (the effect, the sound, the popup); cleared by running its timer out.
        if (op == "status" || op == "set_status") {
            static const std::uint32_t bits[4] = {0x01, 0x08, 0x10, 0x20};
            static const int ticks[4] = {900, 300, 600, 1200};
            std::int32_t* timer = &at<std::int32_t>(0x76c160);
            std::uint32_t& mask = at<std::uint32_t>(0x76c15c);
            if (op == "status") {
                if (!out) return false;
                out->clear();
                for (int i = 0; i < 4; ++i) out->push_back(mask & bits[i] ? double((std::max)(timer[i], 1)) : 0.0);
                return true;
            }
            const int i = int(arg(0));
            if (i < 0 || i > 3 || !alive) return false;
            if (arg(1) != 0.0) { mask |= bits[i]; timer[i] = ticks[i]; }
            else if (mask & bits[i]) timer[i] = 0;
            return true;
        }
        // Music, as a script's Bgm_Play (0xb9) and Bgm_Stop (0xba), and the fader a script drives (0xc8..0xcc): the
        // two colours at 0x769f30 / 34, the current and target alpha at 0x769f1c / 24, the rate a frame at 0x769f28.
        if (op == "bgm") { play_bgm(int(arg(0)), 0); return true; }
        if (op == "bgm_stop") { stop_bgm(int(arg(0, 1000))); return true; }
        if (op == "fade") {
            at<std::uint32_t>(0x769f30) = at<std::uint32_t>(0x769f34) = std::uint32_t(arg(0)) & 0xffffffu;
            at<float>(0x769f1c) = std::clamp(float(arg(1)), 0.0f, 1.0f);
            at<float>(0x769f24) = std::clamp(float(arg(2)), 0.0f, 1.0f);
            at<float>(0x769f28) = 1.0f / float((std::max)(1.0, arg(3, 20.0)));
            return true;
        }
        if (op == "tune") {                                                       // (key, factor): speed 0, jump 1; the rest is not backed
            const int k = int(arg(0, -1));
            if (k < 0 || k > 1) return false;
            set_tune(mod, k, std::clamp(arg(1, 1.0), 0.0, 10.0));
            return true;
        }
        if (op == "cam_get") {
            if (!out) return false;
            const float* e = &at<float>(kCamEye);
            const float* t = &at<float>(kCamTarget);
            *out = {e[0], e[1], e[2], t[0], t[1], t[2], at<float>(kCamFov), 1.0, 1.0};
            return true;
        }
        return false;
    }
    std::vector<ModInfo> mod_list() override {
        std::vector<ModInfo> out;
        for (const mods::Mod& m : ore::mod_list())
            out.push_back({m.m.id, m.label(), m.m.version, m.state == mods::State::Active, !m.m.script.empty()});
        return out;
    }

    // As CleriaCore's content_give for the retail ids: the count cell (0..99), the gold 0x78..0x7F into SP.
    void give(int id, int count, const std::string& source) override {
        if (in_game() && content_give(id, count, source, {})) return;   // a mod's own item
        if (!in_game() || id < 0 || id >= content::kRetailItems || count < 1) return;
        if (id >= 0x78) {
            const std::int64_t sp = at<std::int32_t>(kSp) + std::int64_t(at<std::int32_t>(kItemMagnitude + id * 0xb8)) * count;
            at<std::int32_t>(kSp) = std::int32_t(std::clamp<std::int64_t>(sp, 0, 999999));
        } else {
            flags()[id] = content::add_count(flags()[id], count);
            refresh();
        }
        events::Event e;
        e.kind = events::Kind::ItemObtained;
        e.room = room();
        e.item = id;
        e.count = count;
        e.source = source;
        events::bus().emit(e);
    }
    bool save_ready() override { return !session_pending(); }
    std::optional<std::string> setting(const std::string& mod, const std::string& key) override { return ore::setting(mod, key); }
    bool set_setting(const std::string& mod, const std::string& key, const std::string& value) override { return ore::set_setting(mod, key, value); }

    // ---- drawing (a HUD layer, ui.cpp) and the page widgets: CleriaCore's own UI kit (view_lua.inl) ----
    std::map<std::string, std::uint32_t> named_colors() override {
        return {{"gold", ui::kGold}, {"label", ui::kLabel}, {"value", ui::kValue}, {"muted", ui::kMuted},
                {"desc", ui::kDesc}, {"good", ui::kGood}, {"warn", ui::kWarn}, {"section", ui::kSection},
                {"panel", ui::kPanel & 0x00FFFFFFu}, {"edge", ui::kPanelEdge & 0x00FFFFFFu}, {"row", ui::kRow},
                {"white", IM_COL32(255, 255, 255, 255)}, {"black", IM_COL32(0, 0, 0, 255)}};
    }
    static ImFont* font(int f) {
        switch (f) {
            case 1: return ui::F.medium;
            case 2: return ui::F.bold;
            case 3: return ui::F.cond;
            case 4: return ui::F.cond_med;
            default: return ui::F.body;
        }
    }
    void draw_text(float x, float y, const std::string& t, float px, int f, std::uint32_t col) override {
        if (g_hud && px > 0) ui::text(g_hud, font(f), px, {x, y}, col, t.c_str());
    }
    std::pair<float, float> text_size(const std::string& t, float px, int f) override {
        if (!font(f)) return {0.0f, 0.0f};   // before the overlay exists
        const ImVec2 s = ui::text_size(font(f), px, t.c_str());
        return {s.x, s.y};
    }
    void draw_rect(float x0, float y0, float x1, float y1, std::uint32_t col, float rounding, float thickness) override {
        if (!g_hud) return;
        if (thickness <= 0) g_hud->AddRectFilled({x0, y0}, {x1, y1}, col, rounding);
        else g_hud->AddRect({x0, y0}, {x1, y1}, col, rounding, 0, thickness);
    }
    void draw_line(float x0, float y0, float x1, float y1, std::uint32_t col, float thickness) override {
        if (g_hud) g_hud->AddLine({x0, y0}, {x1, y1}, col, thickness);
    }
    void draw_circle(float x, float y, float r, std::uint32_t col, float thickness) override {
        if (!g_hud) return;
        if (thickness <= 0) g_hud->AddCircleFilled({x, y}, r, col, 40);
        else g_hud->AddCircle({x, y}, r, col, 40, thickness);
    }
    void ui_section(void* list, const std::string& title) override { ui::section(*static_cast<ui::List*>(list), title.c_str()); }
    bool ui_toggle(void* list, const std::string& label, bool* v, const std::string& desc, bool enabled) override {
        return ui::toggle(*static_cast<ui::List*>(list), label.c_str(), v, desc.c_str(), enabled);
    }
    bool ui_slider(void* list, const std::string& label, float* v, float lo, float hi, float step, const std::string& fmt,
                   const std::string& desc, bool enabled) override {
        return ui::slider(*static_cast<ui::List*>(list), label.c_str(), v, lo, hi, step, fmt.c_str(), desc.c_str(), enabled);
    }
    bool ui_choice(void* list, const std::string& label, int* idx, const std::vector<std::string>& items, const std::string& desc,
                   bool enabled) override {
        std::vector<const char*> p;
        for (const std::string& s : items) p.push_back(s.c_str());
        return ui::choice(*static_cast<ui::List*>(list), label.c_str(), idx, p.data(), int(p.size()), desc.c_str(), enabled);
    }
    bool ui_button(void* list, const std::string& label, const std::string& desc, bool enabled) override {
        return ui::button(*static_cast<ui::List*>(list), label.c_str(), desc.c_str(), enabled);
    }
    bool ui_text_field(void* list, const std::string& label, std::string* v, const std::string& desc, bool secret,
                       const std::string& empty) override {
        return ui::text_field(*static_cast<ui::List*>(list), label.c_str(), v, desc.c_str(), secret, empty.c_str());
    }
    void ui_info(void* list, const std::string& label, const std::string& value, std::uint32_t col, const std::string& desc) override {
        ui::info(*static_cast<ui::List*>(list), label.c_str(), value.c_str(), col, desc.empty() ? nullptr : desc.c_str());
    }
    int load_image(const std::filesystem::path& file) override { return ui_load_image(file); }
    void draw_image(int handle, float x0, float y0, float x1, float y1, std::uint32_t tint) override {
        if (!g_hud) return;
        if (void* t = ore::ui_image(handle)) g_hud->AddImage(ImTextureID(reinterpret_cast<std::intptr_t>(t)), {x0, y0}, {x1, y1}, {0, 0}, {1, 1}, tint);
    }
    void ui_image(void* list, int handle, float height, std::uint32_t tint) override {
        ui::List& L = *static_cast<ui::List*>(list);
        int w = 0, h = 0;
        void* t = ore::ui_image(handle, &w, &h);
        if (!t || h <= 0) return;
        const float ph = std::clamp(height, 16.0f, 600.0f) * ui::S, pw = (std::min)(ph * float(w) / float(h), L.size.x - 26.0f * ui::S);
        const float x0 = L.org.x + 13.0f * ui::S, y = L.org.y + L.y - L.scroll + 4.0f * ui::S;
        L.dl->AddImage(ImTextureID(reinterpret_cast<std::intptr_t>(t)), {x0, y}, {x0 + pw, y + ph}, {0, 0}, {1, 1}, tint);
        L.y += ph + 12.0f * ui::S;
    }
    void ui_text(void* list, const std::string& text, std::uint32_t col) override { ui::paragraph(*static_cast<ui::List*>(list), text.c_str(), col); }
    void ui_table(void* list, const std::vector<std::string>& cells, bool header, std::uint32_t col) override {
        ui::table_row(*static_cast<ui::List*>(list), cells, header, col);
    }
    void ui_progress(void* list, const std::string& label, double fraction, const std::string& text, std::uint32_t col) override {
        ui::progress(*static_cast<ui::List*>(list), label.c_str(), float(fraction), text.c_str(), col);
    }
    std::map<std::string, std::string>* save_storage(const std::string& mod) override { return &sidecar().mod[mod]; }
    void refresh() override {
        if (in_game()) at<std::int32_t>(kRecalc) |= 0x75;
    }

    double stat(Stat s) override {
        if (!in_game()) return 0;
        switch (s) {
            case Stat::Hp: return at<float>(kHp);
            case Stat::MaxHp: return at<float>(kMaxHp);
            case Stat::Sp: return at<std::int32_t>(kSp);
            case Stat::Level: return at<std::int32_t>(kLevel);
            case Stat::Exp: return at<float>(kExp);
            case Stat::WeaponTier: return flags()[kFlagWeapon];
        }
        return 0;
    }
    // The player's actor keeps a copy of the stat record (+0x94 on); the game's own setters refresh it.
    void set_stat(Stat s, double v) override {
        if (!in_game()) return;
        const int i = int(std::lround(v));
        switch (s) {
            case Stat::Hp:
                at<float>(kHp) = float(std::clamp(i, 0, int(at<float>(kMaxHp))));
                *reinterpret_cast<float*>(at<char*>(kPlayer) + 0x98) = at<float>(kHp);
                sync_gauges();
                break;
            case Stat::Sp: at<std::int32_t>(kSp) = std::clamp(i, 0, 999999); break;
            case Stat::Level: set_level(unsigned(std::clamp(i, 1, 60))); break;
            case Stat::WeaponTier:   // as the script op 0x7F SetWeaponLevel: the four characters' slots, 0..10.
                // The recompute each call ends with copies the current character's slot into g_flags[148].
                for (unsigned slot = 0; slot < 4; ++slot) set_tier(slot, std::clamp(i, 0, 10));
                break;
            default: break;   // MaxHp / Exp follow the game's stat model
        }
    }
    void add_exp(double n) override {
        if (!in_game()) return;
        at<float>(kExp) += float(n);
        level_ups();
    }
    bool player_info(script::PlayerInfo& o) override {
        if (!in_game()) return false;
        o.level = at<std::int32_t>(kLevel);
        o.hp = int(at<float>(kHp));
        o.max_hp = int(at<float>(kMaxHp));
        o.sp = at<std::int32_t>(kSp);
        o.weapon_tier = flags()[kFlagWeapon];
        o.exp = at<float>(kExp);
        o.exp_next = o.level < 60 ? at<float>(kExpNext) : 0.0;
        o.atk = at<float>(kAtk);
        o.def = at<float>(kDef);
        o.base_max_hp = base_stat(0);               // the game's own, before the mods' modifiers
        o.base_atk = base_stat(1);
        o.base_def = base_stat(2);
        o.mp = at<float>(kMp);
        o.max_mp = at<float>(kMaxMp);
        o.boost = at<float>(kBoost);
        o.boosting = flags()[kFlagBoosting] != 0;
        const float* buff = &at<float>(kBuffs);     // MP, Strength, Defense
        o.buff[0] = buff[1]; o.buff[1] = buff[2]; o.buff[2] = buff[0];
        for (int k = 0; k < 3; ++k) o.buff_timer[k] = (&at<std::int32_t>(kBuffTimers))[k];
        std::memcpy(o.pos, at<char*>(kPlayer) + 0x50, sizeof o.pos);   // the actor's position
        return true;   // luck is not read (0 in every game)
    }
    // An own value for the pickup is CleriaCore's: the retail pickup gives what its item gives.
    bool spawn_pickup(int item, float x, float y, float z, double) override {
        if (!in_game() || item < 0 || item >= content::kRetailItems) return false;
        const float pos[3] = {x, y, z};
        return ore::spawn_pickup(item, pos, false);
    }
    void warp(int index) override {
        if (!in_game() || index < 0 || index >= 24) return;
        flags()[kFlagWarpTo] = index;
        warp_to_target();
    }
    // The statue's save when the game can be saved, by CleriaCore's gate (view_game.inl) and the retail
    // Archipelago mod's: the player has the controls (185), no boss fight (156), arena (190) or run (193),
    // no Fix, alive, a second in the room.
    bool save(int slot) override {
        if (slot < 0 || slot > 63 || !in_game() || session_pending() || frozen() || room_ticks() < 60) return false;
        const std::int32_t* f = flags();
        if (f[kFlagControl] != 1 || f[kFlagFight] != 0 || f[kFlagArena] != 0 || f[kFlagRun] != 0 || at<float>(kHp) < 1) return false;
        char file[16];
        std::snprintf(file, sizeof file, "yso_%02d.bin", slot);
        at<char>(kSaveSlot) = char(slot);
        return write_save(file) >= 0;
    }
};

}  // namespace

script::Host& host() {
    static RetailHost h;
    return h;
}

// The player's motion constants are copied from CASTINFO into its actor when it is built (each room): the speed
// base at +0x30c (CASTINFO +0x48), the jump's first velocity at +0x408. The mods' factors are kept on top of
// whatever the game last put there.
// A room change between two of the game's rooms ("S_nnnn"; not the title, a movie or the first room of a session):
// q.from, q.to; the answer's `to` names another room, "suppress" cancels it. index and door are not known here
// (-1, ""): the request does not say which door asked.
int exit_filter(int scene) {
    if (!world_on("exit")) return scene;
    const char* recs = *reinterpret_cast<const char**>(0x74c124);
    const int count = *reinterpret_cast<const std::int32_t*>(0x74e230);
    const std::string from = host().room();
    if (!recs || scene <= 0 || scene >= count || from.empty()) return scene;
    std::string to(recs + std::size_t(scene) * 0x1c8 + 4);
    while (!to.empty() && to.back() == '\\') to.pop_back();
    if (const std::size_t k = to.rfind('\\'); k != std::string::npos) to.erase(0, k + 1);
    for (char& c : to) c = char(std::toupper(static_cast<unsigned char>(c)));
    if (to.rfind("S_", 0) != 0) return scene;
    script::WorldQuery q;
    q.kind = "exit";
    q.num = {{"index", -1.0}};
    q.str = {{"from", from}, {"to", to}, {"door", ""}};
    const script::WorldDecision d = world_filter(q);
    if (d.action == script::WorldDecision::Action::Suppress) return 0;
    if (d.action != script::WorldDecision::Action::Replace) return scene;
    const auto it = d.str.find("to");
    const char* rec = it == d.str.end() ? nullptr : RetailHost::scene_rec(it->second);
    return rec ? int(*reinterpret_cast<const std::uint16_t*>(rec)) : scene;
}
void world_tick() {
    if (hooked() && *reinterpret_cast<char**>(0x74c09c)) RetailHost::new_monsters();
}
bool actor_held(const void* actor) {
    const auto& s = RetailHost::held_set();
    if (s.empty()) return false;
    RetailHost::held_room();
    return s.count(actor) != 0;
}
void tune_tick() {
    static float wrote[2] = {0, 0}, base[2] = {0, 0};
    static const std::size_t off[2] = {0x30c, 0x408};
    char* player = hooked() ? *reinterpret_cast<char**>(0x74c09c) : nullptr;
    if (!player) return;
    for (int k = 0; k < 2; ++k) {
        float& v = *reinterpret_cast<float*>(player + off[k]);
        if (v != wrote[k]) base[k] = v;              // the game's own value (a new actor, or never touched)
        const float want = base[k] * float(tune(k));
        if (v != want) v = want;
        wrote[k] = want;
    }
}

}  // namespace ore
