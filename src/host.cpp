// What a mod's script reaches of the retail game: cleria::script::Host over the memory of yso_win.exe
// (the build hooks.cpp checks; nothing here is read unless the hooks went in). What is not backed yet
// keeps script::Host's harmless default: images and icons, the stat modifiers, pickups, the save profiles.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <cstring>
#include <map>
#include <set>
#include <windows.h>
#include <shellapi.h>
#include <mmsystem.h>

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
constexpr std::uintptr_t kBookPopup = 0x738b40;   // int: the book's Use popup state, 3 = the warp map (CleriaCore's pstate_)
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

// FUN_00434970(ECX name, EDX 0; an argument block, a parent; the caller pops): a script's StartScript (0xb4). The name
// is one of the room's scripts, or "@name" for one of s_common. It answers 0 when a script of that name already
// runs. The new script runs its first burst at once.
__declspec(naked) void* __cdecl start_script(const char* /*name*/) {
    __asm {
        mov  ecx, dword ptr [esp + 4]
        xor  edx, edx
        push 0
        push 0
        mov  eax, 0x434970
        call eax
        add  esp, 8
        ret
    }
}

// FUN_00475710(ECX cast, EDX yaw in degrees; x, y, z, a stat argument; the caller pops): a monster of CASTINFO's
// row `cast` built as the game's own spawners build one (the factory, the stats of its ENEMYINFO record).
// FUN_0043a460: thiscall(the gauge's widget; the owner's id, a word not read, the maximum, the bar's scale), the HP
// in xmm3: what a boss's handler calls each step to show its HP on the boss gauge.
__declspec(naked) void __cdecl boss_gauge(int /*id*/, float /*hp*/, float /*max*/, float /*scale*/) {
    __asm {
        push ebp
        mov  ebp, esp
        movss xmm3, dword ptr [ebp + 12]
        push dword ptr [ebp + 20]
        push dword ptr [ebp + 16]
        push 0
        push dword ptr [ebp + 8]
        mov  ecx, 0x6d5fd4
        mov  eax, 0x43a460
        call eax
        pop  ebp
        ret
    }
}
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
    // A new game from the title, as its Difficulty page starts one: the character (flag 150), the difficulty's three
    // bytes, the story's mode, the game's own reset (FUN_0041fd10), then the first scene. `extra` 0 / 1 picks the
    // character's Normal or Extra version (0x74c0dc Yunica, 0x74c0d8 Hugo, 0x74c0d4 Toal), -1 leaves it. `room`
    // is the scene to begin in: "S_0000" is the game's own beginning.
    bool new_game(int ch, int diff, int extra, const std::string& room, const std::vector<std::pair<int, int>>& set = {}) {
        const char* first = scene_rec(room);
        if (!hooked() || game_state() != "title" || !first) return false;
        at<std::int32_t>(0x76b91c + 4 * 150) = ch;
        at<std::uint8_t>(0x74ac3c) = std::uint8_t(diff == 3 ? 1 : diff == 4 ? 2 : 0);
        at<std::uint8_t>(0x74e685) = diff <= 1;
        at<std::uint8_t>(0x74e684) = diff == 0;
        at<std::uint8_t>(0x74e25f) = 0;
        at<std::uint8_t>(0x74e25e) = 0;                                       // the story, not Time Attack or the Arena
        // (the Extra version only where it was bought, as Type Select offers it: 0x74ac00 Yunica, +4 Hugo, +8 Toal)
        if (extra >= 0) at<std::int32_t>(ch == 1 ? 0x74c0dc : ch == 2 ? 0x74c0d8 : 0x74c0d4) = extra != 0 && at<std::int32_t>(0x74ac00 + 4 * (ch - 1)) != 0;
        reinterpret_cast<void(__cdecl*)()>(0x41fd10)();
        for (const auto& [flag, value] : set)                                 // (after the reset, before the first room's script)
            if (flag >= 0 && flag < 0x200) at<std::int32_t>(0x76b91c + 4 * flag) = value;
        // The title's own tick commits its staged scene when its fade ends; without the title object's flag
        // nothing would, so the change is asked for here, at once.
        at<std::int32_t>(0x769fb8) = *reinterpret_cast<const std::uint16_t*>(first);
        std::memset(&at<char>(0x769fbc), 0, 0x194);
        stop_bgm(1500);
        return request_scene(*reinterpret_cast<const std::uint16_t*>(first), 1) == 0;
    }
    // The loader's own commands, for what the mod API has no call for (cleria.mods.command(name, args)):
    //   ore.title new_game                      the title's own New Game, past a mod's "title_new_game" command
    //   ore.menu id|caption|row|row...          a menu of the game's own; a pick runs the mod's command "menu_<id>"
    //                                           with the row (1..), Esc picks the last row; "id:3" starts on row 3
    //   ore.prompt id|caption|text[|secret]     a line typed in such a menu (Enter keeps, Esc drops, Ctrl+V pastes):
    //                                           "menu_<id>" gets "ok <text>" or "cancel"
    //   ore.continue                            loads the most recent save (of the save profile, if one is set)
    //   ore.new_game character=2 difficulty=2 extra=0 room=S_1000 flags=246:1,300:1
    //                                           a new game at once (each key optional): the scene it begins in, and
    //                                           flags set after the game's reset, before that scene's script
    bool own_command(const std::string& name, const std::string& args, bool* ran) {
        if (name.rfind("ore.", 0) != 0) return false;
        *ran = false;
        if (name == "ore.title") {
            if (args != "new_game" || game_state() != "title") return true;
            title_run();
            *ran = true;
        } else if (name == "ore.menu") {                                           // "id[:cursor]|caption|row|row..."; no argument closes it
            if (args.empty()) { game_menu_close(); *ran = true; return true; }
            std::vector<std::wstring> parts;
            std::string id;
            int cursor = 1;
            std::istringstream in(args);
            for (std::string part; std::getline(in, part, '|');) {
                if (id.empty()) {
                    const std::size_t colon = part.find(':');
                    id = part.substr(0, colon);
                    if (colon != std::string::npos) cursor = std::atoi(part.c_str() + colon + 1);
                    continue;
                }
                std::wstring w(part.size() + 1, L'\0');
                w.resize(std::size_t((std::max)(0, MultiByteToWideChar(CP_UTF8, 0, part.c_str(), int(part.size()), w.data(), int(w.size())))));
                parts.push_back(w);
            }
            if (id.empty() || parts.size() < 2) return true;
            const std::wstring caption = parts.front();
            parts.erase(parts.begin());
            // On the title it stands where the title's own list does, its last row at the bottom; elsewhere in the middle.
            const float rows = float(parts.size());
            const float y = !in_game() ? (std::max)(150.0f, 690.0f - 38.4f * rows) : (std::max)(96.0f, 340.0f - 19.2f * rows);
            *ran = game_menu(caption, parts, cursor, y);
            if (*ran) menu_owner(id);
        } else if (name == "ore.prompt") {                                         // "id|caption|text[|secret]": a line typed in a game menu
            std::vector<std::string> f;
            std::istringstream in(args);
            for (std::string part; std::getline(in, part, '|');) f.push_back(part);
            if (f.size() < 2 || f[0].empty()) return true;
            const std::wstring caption(f[1].begin(), f[1].end());
            const float y = !in_game() ? 690.0f - 38.4f : 320.0f;
            const std::string first = f.size() > 2 ? f[2] : std::string(), shown = (f.size() > 3 && f[3] == "secret" ? std::string(first.size(), '*') : first) + "_";
            *ran = game_menu(caption, {std::wstring(shown.begin(), shown.end())}, 1, y);
            if (*ran) { menu_owner(f[0]); ui_prompt(first, f.size() > 3 && f[3] == "secret"); }
        } else if (name == "ore.continue") {                                       // the most recent save (of the save profile), loaded
            const int slot = latest_slot();
            *ran = slot >= 0 && load_slot(slot);
        } else if (name == "ore.new_game") {
            int ch = 1, diff = 2, extra = -1;
            std::string room = "S_0000";
            std::vector<std::pair<int, int>> set;
            std::istringstream in(args);
            for (std::string kv; in >> kv;) {
                const std::size_t eq = kv.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
                if (k == "character") ch = std::clamp(std::atoi(v.c_str()), 1, 3);
                else if (k == "difficulty") diff = std::clamp(std::atoi(v.c_str()), 0, 4);
                else if (k == "extra") extra = std::atoi(v.c_str()) != 0;
                else if (k == "room") room = v;
                else if (k == "flags") {                                      // flags=246:1,300:1
                    std::istringstream fs_(v);
                    for (std::string one; std::getline(fs_, one, ',');)
                        if (const std::size_t c = one.find(':'); c != std::string::npos) set.emplace_back(std::atoi(one.c_str()), std::atoi(one.c_str() + c + 1));
                }
            }
            *ran = new_game(ch, diff, extra, room, set);
        }
        return true;
    }
    bool run_command(const std::string& name, const std::string& args) override {
        if (bool ran = false; own_command(name, args, &ran)) return ran;
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
    // here when they hold the world, "play" otherwise; the warp map alone is told apart ("map").
    std::string game_state() override {
        if (!in_game()) return "title";
        if (ui_panel_open()) return "panel";
        if (const script::Mod* m = window_mod(); m && m->window()->pause) return "window";
        if (paused()) return "held";
        if (at<float>(kHp) < 1) return "gameover";
        if (warp_map_open()) return "map";
        return frozen() ? "cutscene" : "play";
    }
    // The popup state stays 3 after the map's warp until the book opens its popup again, so the room the map opened
    // in is kept: once the room changes the map is closed until the state leaves 3 (the retail Archipelago mod's
    // warpmap.cpp, live-tested; walking back into that room does not open it again).
    bool warp_map_open() {
        static int opened_in = -1;
        static bool stale = false;
        if (at<std::int32_t>(kBookPopup) != 3) { opened_in = -1; stale = false; return false; }
        const int scene = flags()[kFlagScene];
        if (opened_in == -1) opened_in = scene;
        if (scene != opened_in) stale = true;
        return !stale;
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
        forms().clear();
        auras().clear();
        room =at<std::int32_t>(0x76b91c + 4 * kFlagScene);
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
    // A cast's name for display (CASTINFO +0x10, 20 characters: "Adol"), its record's name when it has none.
    static std::string cast_name(int cast) {
        const char* info = at<const char*>(0x74c11c);
        if (!info || cast < 0 || cast >= 186) return {};
        const char* rec = info + std::size_t(cast) * 0x200;
        const std::string display(rec + 0x10, strnlen(rec + 0x10, 0x14));
        return display.empty() ? std::string(rec, strnlen(rec, 0x10)) : display;
    }
    // A sheet of the game's by its name ("m_100", with or without an extension): its index in the AIA table, -1.
    static int sheet_named(std::string name) {
        name = fs::path(name).stem().string();
        for (int i = 0; i < 269; ++i)
            if (const char* s = at<const char*>(0x68a800 + 4 * i); s && _stricmp(s, name.c_str()) == 0) return i;
        return -1;
    }
    // What a mod made of a monster (world.form, a variant): its kind and name, and whether the boss gauge is its.
    struct Form { std::string kind, name; bool boss = false; };
    static std::map<int, Form>& forms() { static std::map<int, Form> m; return m; }
    struct Aura { int want = 0; void* fx = nullptr; int id = 0; };   // a puppet's: the effect asked for, the effect, the factory's id for it
    static std::map<int, Aura>& auras() { static std::map<int, Aura> m; return m; }
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
    // A mod's boss: the boss gauge is given its HP each step, as a boss's own handler does (FUN_0043a460 on the
    // widget 0x6d5fd4); gone or dead, the gauge is let go.
    static void boss_tick() {
        if (forms().empty()) return;
        held_room();
        for (auto& [id, f] : forms()) {
            if (!f.boss) continue;
            char* a = find_monster(id);
            if (a && in<float>(a, kAHp) > 0) { boss_gauge(id, in<float>(a, kAHp), in<float>(a, kAMaxHp), 4500.0f); continue; }   // (the bar's length: Nygtilger's)
            f.boss = false;
            if (at<std::int32_t>(0x6d5fd8) == id) at<std::int32_t>(0x6d5fd8) = 0;
        }
    }
    static std::map<const void*, std::string>& spawned_by() { static std::map<const void*, std::string> m; return m; }   // world.spawn's, until world_tick
    // The monsters built since the last step: each passes the mods' "spawn" filter once (the multipliers of its
    // numbers, another cast, or "suppress": it is removed), then enemy_spawned. The filter comes after the game
    // built the monster, so it cannot change its level.
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
                q.str = {{"name", cast_name(in<std::int32_t>(a, kACast))}};
                const auto by = spawned_by().find(a);
                q.str.emplace_back("by", by == spawned_by().end() ? std::string() : by->second);
                const script::WorldDecision d = world_filter(q);
                if (d.action == script::WorldDecision::Action::Suppress) {
                    in<std::uint32_t>(a, kAKind) &= ~kKindSolid;
                    in<std::uint8_t>(a, kADead) |= 2;
                    continue;
                }
                if (d.action == script::WorldDecision::Action::Replace) {
                    // Another cast: the monster built is removed and that one built where it stood.
                    const int cast = int(d.n("cast", double(in<std::int32_t>(a, kACast))));
                    if (cast != in<std::int32_t>(a, kACast) && cast >= 14 && cast < 186 && cast_sheet(cast) >= 0) {
                        const float pos[3] = {p[0], p[1], p[2]};
                        if (char* b = spawn_monster(cast, int(in<float>(a, kAYaw) * 57.29578f), pos)) {
                            in<std::uint32_t>(a, kAKind) &= ~kKindSolid;
                            in<std::uint8_t>(a, kADead) |= 2;
                            known.insert(actor_id(b));
                            a = b;
                            p = &in<float>(a, kAPos);
                        }
                    }
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
    // The mods' layer over the game's camera (camera.set): the eye's distance from its subject and the field of view
    // scaled, eye and subject moved together.
    struct CamLayer {
        float dist = 1.0f, fov = 1.0f, off[3] = {0, 0, 0};
        bool identity() const { return dist == 1.0f && fov == 1.0f && off[0] == 0 && off[1] == 0 && off[2] == 0; }
    };
    static CamLayer& cam_layer() { static CamLayer c; return c; }
    // A sprite's glow: the colour added to it as it is lit (FUN_00473c60), ARGB at +0x2b4, the alpha its strength.
    // It is what an animation script's anime_SetAura writes; 0 = none.
    static void glow(char* a, std::uint32_t rgb) { in<std::uint32_t>(a, 0x2b4) = rgb ? (0xc0000000u | rgb) : 0u; }
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
            o.ai = !held_set().count(a);
            o.name = cast_name(o.cast);
            if (const auto f = forms().find(o.id); f != forms().end()) {
                o.kind = f->second.kind;
                o.boss = f->second.boss;
                if (!f->second.name.empty()) o.name = f->second.name;
            }
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
        if (op == "bgm_file") return play_music_file(text);                       // an Ogg file of the mod's folder
        if (op == "se_file") {                                                    // a WAV of the mod's folder, by Windows itself: one at a time, at its own volume
            return !text.empty() && PlaySoundW(std::filesystem::u8path(text).c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT) != FALSE;
        }
        if (op == "load") return load_slot(int(arg(0, -1)));                      // from the title too
        // A new game, as the title's Difficulty popup starts one (menu_re/TITLE_DRAW.md §6.3): the character (flag
        // 150) and the difficulty's three cells, the game's own new-game init (FUN_0041fd10, which keeps the
        // character), then the title's "finish" (FUN_005758f0): a fade-out to the scene s_00\s_0000. From the title
        // only: (character 1..3, difficulty 0 Very Easy .. 4 Nightmare).
        if (op == "new_game") return new_game(std::clamp(int(arg(0, 1)), 1, 3), std::clamp(int(arg(1, 2)), 0, 4), -1, "S_0000");
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
        // or the death and its drops. A kill is credited here (script_kill), as the applier's kill block would.
        if (op == "hit") {
            char* a = find_monster(int(arg(0, -1)));
            const int dmg = int(arg(1));
            if (!alive || !a || dmg < 0) return false;
            in<std::uint32_t>(a, 0x3bc) |= dmg > 0 ? 6u : 4u;
            if (in<float>(a, kAHp) - float(dmg) < 1.0f) {
                script_kill(a);                                                   // the EXP and SP of a kill of the player's
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
                    double(in<std::int32_t>(player, 0x97c)),
                    in<std::uint32_t>(player, 0x2b4) >> 24 ? double(in<std::uint32_t>(player, 0x2b4) & 0xffffffu) : 0.0,
                    (at<std::uint32_t>(0x76c198) & 0x20u) ? (flags()[150] == 3 ? 171.0 : 39.0) : 0.0};   // the Boost's aura while it runs
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
            // The look: the scale the draw takes from the actor (+0x248), and for the tint the colour its sprite is lit
            // with on top of the room's light (+0x2b4, see glow()).
            if (arg(6, -1) > 0) in<float>(a, 0x248) = float(std::clamp(arg(6), 0.2, 5.0));
            if (arg(7, -1) >= 0) glow(a, std::uint32_t(arg(7)) & 0xffffffu);
            if (std::isfinite(arg(8, NAN))) hold(a, arg(8) == 0);
            const int id = actor_id(a);
            held_room();                                                          // (the forms are the room's)
            if (std::isfinite(arg(9, NAN))) {                                     // a mod's boss: the gauge shows its HP (boss_tick)
                forms()[id].boss = arg(9) != 0;
                if (arg(9) == 0 && at<std::int32_t>(0x6d5fd8) == id) at<std::int32_t>(0x6d5fd8) = 0;
            }
            if (const std::size_t tab = text.find('\t'); tab != std::string::npos) {   // "kind \t name [\t sheet]"
                const std::size_t tab2 = text.find('\t', tab + 1);
                Form& f = forms()[id];
                if (tab > 0) f.kind = text.substr(0, tab);
                const std::string name = text.substr(tab + 1, tab2 == std::string::npos ? std::string::npos : tab2 - tab - 1);
                if (!name.empty()) f.name = name;
                // The look: a sheet of the game's own by its name. (A sheet file of the mod's is not loaded: the
                // game draws from its table of 269 names.)
                if (tab2 != std::string::npos)
                    if (const int sheet = sheet_named(text.substr(tab2 + 1)); sheet >= 0) in<std::int32_t>(a, 0x97c) = sheet;
            }
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
        if (op == "puppet_effect") {                                              // (puppet, id, fwd, angle, voff, scale, attach)
            char* a = find_puppet(int(arg(0, -1)));
            return a && n.size() >= 7 && ore::puppet_effect(a, n.data() + 1);
        }
        if (op == "puppet_set" || op == "puppet_remove") {
            char* a = find_puppet(int(arg(0, -1)));
            if (!a) return false;
            if (op == "puppet_remove") {
                if (const auto au = auras().find(actor_id(a)); au != auras().end()) { fx_end(au->second.fx, au->second.id); auras().erase(au); }
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
            if (n.size() > 10 && std::isfinite(n[10])) glow(a, std::uint32_t((std::max)(0.0, n[10])) & 0xffffffu);
            if (n.size() > 8 && std::isfinite(n[8])) {                            // hittable: a body the player's hits find (puppet_hit)
                const std::uint32_t body = kKindEnemy | kKindSolid;
                in<std::uint32_t>(a, kAKind) = n[8] != 0 ? (in<std::uint32_t>(a, kAKind) | body) : (in<std::uint32_t>(a, kAKind) & ~body);
                in<float>(a, kAHp) = in<float>(a, kAMaxHp) = 1.0f;
            }
            if (n.size() > 11 && std::isfinite(n[11])) {                          // aura: an effect that follows it, 0 = none
                auto& au = auras()[actor_id(a)];
                const int want = int(n[11]);
                if (au.want != want || (want > 0 && !fx_live(au.fx, au.id))) {
                    fx_end(au.fx, au.id);
                    au = Aura{want, nullptr, 0};
                    if (want > 0) au.fx = aura_effect(a, want, &au.id);
                }
            }
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
        // The game-over menu's two rows are two functions without arguments (gameflow_re/DEATH_RETRY.md): Retry
        // (0x4276b0: the continue snapshot back, the scene it was taken in) and Return to Title (0x427750). Each
        // starts its own fade-out and scene change.
        if (op == "retry") {
            if (alive) return false;                                              // only once the player is down
            reinterpret_cast<void(__cdecl*)()>(0x4276b0)();
            return true;
        }
        if (op == "to_title") { reinterpret_cast<void(__cdecl*)()>(0x427750)(); return true; }
        // The camera's script operations are plain writes (event_re/CAMERA_OPS.md). Shake (0xCF): three amplitudes at
        // 0x76a594 and a frame count at 0x76a5ac. Scroll_MoveTo (0xC3) and Scroll_Return (0xC4): the scroll block at
        // 0x76a1c0..0x76a1f4, as FUN_0056ec40 and FUN_0056ecd0 fill it.
        if (op == "cam_shake") {
            for (std::size_t k = 0; k < 3; ++k) at<float>(0x76a594 + 4 * k) = float(arg(k));
            at<std::int32_t>(0x76a5ac) = std::clamp(int(arg(3, 20)), 1, 600);
            return true;
        }
        if (op == "cam_look") {                                                   // (x, y, z, frames): the camera's subject; no point: back to the player
            const int frames = std::clamp(int(arg(3, 30)), 0, 600);
            const bool to = n.size() >= 3 && std::isfinite(n[0]);
            at<float>(0x76a1f0) = frames ? 1.0f / float(frames) : 1.0f;
            if (to) {
                for (std::size_t k = 0; k < 3; ++k) at<float>(0x76a1d0 + 4 * k) = float(n[k]);
                at<float>(0x76a1e4) = at<float>(0x76a168);
                at<float>(0x76a1dc) = at<float>(0x76a160);
                at<float>(0x76a1e0) = at<float>(0x76a164);
                at<float>(0x76a1ec) = 0.0f;
                at<float>(0x76a1e8) = 1.0f;
            } else {
                at<float>(0x76a1e4) = at<float>(0x76a1cc);
                at<float>(0x76a1dc) = at<float>(0x76a1c4);
                at<float>(0x76a1e0) = at<float>(0x76a1c8);
                at<float>(0x76a1e8) = 0.0f;
            }
            at<std::int32_t>(0x76a1f4) = 0;
            if (!frames) at<std::uint8_t>(0x76a1c0) = 1;
            return true;
        }
        if (op == "script_run") {                                                 // (name): as a script's StartScript
            if (text.empty() || text.size() > 0x1f) return false;
            return start_script(text.c_str()) != nullptr;
        }
        // The room's doors are the RegionEntry pool (warp_re/TRANSIT.md §1): DAT_0076a2b4, DAT_0076a2b8 records of
        // 0x2a0; a live one has its byte +4 set, +0x38 & 2 means shut, +0x3c is the scene it leads to and +0x44 the
        // 0x194-byte descriptor of the arrival. `exits` gives each door's room number, 0 for one that is shut or
        // leads nowhere known.
        if (op == "exits" || op == "take_exit") {
            const char* pool = at<const char*>(0x76a2b4);
            const int count = pool ? at<std::int32_t>(0x76a2b8) : 0;
            const char* recs = at<const char*>(0x74c124);
            const int scenes = at<std::int32_t>(0x74e230);
            auto entry = [&](int i) { return pool + std::size_t(i) * 0x2a0; };
            auto room_of = [&](int i) {
                const char* e = entry(i);
                const int scene = *reinterpret_cast<const std::int32_t*>(e + 0x3c);
                if (!e[4] || (std::uint8_t(e[0x38]) & 2) || !recs || scene <= 0 || scene >= scenes) return 0;
                const std::string path(recs + std::size_t(scene) * 0x1c8 + 4);
                const std::size_t k = path.find_last_of('_');
                return k == std::string::npos ? 0 : std::atoi(path.c_str() + k + 1);
            };
            if (op == "exits") {
                if (!out) return false;
                out->clear();
                for (int i = 0; i < count && i < 64; ++i) out->push_back(double(room_of(i)));
                return true;
            }
            // As the trigger does when the player stands in the door (FUN_0042cf00, 0x42d0fc..): the fade starts
            // and the door's scene and descriptor are staged; the fade's end commits the change. Without the
            // walk-out, and refused where the trigger itself would not fire.
            const int i = int(arg(0, -1));
            if (!alive || i < 0 || i >= count || i >= 64 || !room_of(i)) return false;
            if (at<std::int32_t>(0x76a538) != 0 || flags()[156] != 0 || flags()[129] != 0 || frozen()) return false;
            const char* e = entry(i);
            at<float>(0x769f24) = 1.0f;
            at<std::int32_t>(0x769fb8) = *reinterpret_cast<const std::int32_t*>(e + 0x3c);
            at<std::int32_t>(0x769f18) = 0;
            at<float>(0x769f1c) = 0.0f;
            at<float>(0x769f2c) = 0.0f;
            std::memcpy(&at<char>(0x769fbc), e + 0x44, 0x194);
            at<std::int32_t>(0x76a538) = *reinterpret_cast<const std::int32_t*>(e + 0x3c);
            return true;
        }
        if (op == "player_look") {                                                // (scale, glow 0xRRGGBB, or under 0 for none): the player's own sprite
            if (std::isfinite(arg(0, NAN))) in<float>(player, 0x248) = float(std::clamp(arg(0), 0.3, 3.0));
            if (std::isfinite(arg(1, NAN))) glow(player, arg(1) < 0 ? 0u : std::uint32_t(arg(1)) & 0xffffffu);
            return true;
        }
        // The game's HUD has one fade for all its parts (0x74e688, and the tint 0x74e68c made from it each step): a
        // mod that hides the status part hides the whole HUD. The boss gauge or the messages alone are not backed.
        if (op == "hud_show") {
            static std::map<std::string, unsigned> hidden;
            unsigned& v = hidden[mod];
            const unsigned bit = unsigned(arg(0));
            v = arg(1) != 0 ? (v & ~bit) : (v | bit);
            set_hud_hidden(mod, v);
            return true;
        }
        if (op == "tune") {                                                       // (key, factor): speed, jump, mp_cost, mp_regen, boost_gain; 5 the extra air jumps
            const int k = int(arg(0, -1));
            if (k < 0 || k > 5) return false;
            set_tune(mod, k, k == 5 ? std::clamp(std::floor(arg(1)), 0.0, 8.0) : std::clamp(arg(1, 1.0), 0.0, 10.0));
            return true;
        }
        if (op == "cam_get") {
            if (!out) return false;
            const float* e = &at<float>(kCamEye);
            const float* t = &at<float>(kCamTarget);
            *out = {e[0], e[1], e[2], t[0], t[1], t[2], at<float>(kCamFov), cam_layer().dist, cam_layer().fov};
            return true;
        }
        if (op == "cam_set") {                                                    // (dist, fov, ox, oy, oz): a NaN keeps the value
            CamLayer& c = cam_layer();
            if (std::isfinite(arg(0, NAN))) c.dist = std::clamp(float(arg(0)), 0.2f, 5.0f);
            if (std::isfinite(arg(1, NAN))) c.fov = std::clamp(float(arg(1)), 0.3f, 2.5f);
            for (std::size_t k = 0; k < 3; ++k)
                if (std::isfinite(arg(2 + k, NAN))) c.off[k] = std::clamp(float(arg(2 + k)), -50.0f, 50.0f);
            return true;
        }
        if (op == "cam_reset") { cam_layer() = CamLayer{}; return true; }
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
    void register_icon(int id, const std::filesystem::path& file) override { ui_item_icon(id, file); }
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
    // The save profile: only while no game runs (the title), so a running game's saves never change folder.
    bool set_save_profile(const std::string& name) override {
        const std::string was = ore::save_profile();
        if (in_game() || !ore::set_save_profile(name)) return false;
        profile_storage();
        if (ore::save_profile() != was) ui_notice("Save profile", ore::save_profile().empty() ? "The normal saves" : ore::save_profile());
        return true;
    }
    std::string save_profile() override { return ore::save_profile(); }
    void unlock_character(int c) override { if (c == 3) open_toal(); }   // only Toal is ever locked on a new game
    // The game's text language: the exe has no setting of its own, Steam installs the data of the language chosen
    // in the game's properties and notes it in the library's appmanifest_207350.acf. "en" when that cannot be read.
    std::string locale() override {
        static const std::string found = [] {
            static const std::pair<const char*, const char*> kNames[] = {{"english", "en"}, {"french", "fr"}, {"japanese", "ja"}, {"german", "de"},
                {"spanish", "es"}, {"italian", "it"}, {"russian", "ru"}, {"koreana", "ko"}, {"schinese", "zh"}, {"tchinese", "zh-TW"}};
            std::ifstream f(dir().parent_path().parent_path().parent_path() / "appmanifest_207350.acf");
            std::string line;
            while (std::getline(f, line)) {
                const std::size_t k = line.find("\"language\"");
                if (k == std::string::npos) continue;
                for (const auto& [name, code] : kNames)
                    if (line.find(std::string("\"") + name + "\"", k + 10) != std::string::npos) return std::string(code);
            }
            return std::string("en");
        }();
        return found;
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
// q.from, q.to, and for a door its index in the room's pool and its label (-1 and "" for a script's change); the
// answer's `to` names another room, "suppress" cancels it.
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
    // A door's change (the fade's commit, 0x4108e1) still has its scene staged at 0x769fb8 and its descriptor at
    // 0x769fbc, the door's label 0x14 into it: the door is the pool's entry with that descriptor.
    int index = -1;
    std::string door;
    if (*reinterpret_cast<const std::int32_t*>(0x769fb8) == scene) {
        const char* pool = *reinterpret_cast<const char**>(0x76a2b4);
        const int doors = pool ? *reinterpret_cast<const std::int32_t*>(0x76a2b8) : 0;
        for (int i = 0; i < doors && i < 64 && index < 0; ++i)
            if (const char* e = pool + std::size_t(i) * 0x2a0; e[4] && std::memcmp(e + 0x44, reinterpret_cast<const char*>(0x769fbc), 0x194) == 0) index = i;
        const char* label = reinterpret_cast<const char*>(0x769fbc + 0x14);
        door.assign(label, strnlen(label, 0x40));
    }
    q.num = {{"index", double(index)}};
    q.str = {{"from", from}, {"to", to}, {"door", door}};
    const script::WorldDecision d = world_filter(q);
    if (d.action == script::WorldDecision::Action::Suppress) return 0;
    if (d.action != script::WorldDecision::Action::Replace) return scene;
    const auto it = d.str.find("to");
    const char* rec = it == d.str.end() ? nullptr : RetailHost::scene_rec(it->second);
    return rec ? int(*reinterpret_cast<const std::uint16_t*>(rec)) : scene;
}
void world_tick() {
    if (!hooked() || !*reinterpret_cast<char**>(0x74c09c)) return;
    RetailHost::new_monsters();
    RetailHost::boss_tick();
}
int puppet_id(const void* actor) {
    const auto& s = RetailHost::puppet_set();
    if (s.empty() || !actor) return 0;
    RetailHost::held_room();
    return s.count(actor) ? *reinterpret_cast<const std::int32_t*>(static_cast<const char*>(actor) + 0x14) : 0;
}
bool actor_held(const void* actor) {
    const auto& s = RetailHost::held_set();
    if (s.empty()) return false;
    RetailHost::held_room();
    return s.count(actor) != 0;
}
// The game publishes its camera each step (the subject at 0x76a160, the eye at 0x76a16c, the field of view at
// 0x7b38f4) and builds the frame's view from it. The layer is applied to what the game wrote; values still as the
// layer left them (a step that did not move the camera) are not scaled twice.
void cam_tick() {
    const RetailHost::CamLayer& c = RetailHost::cam_layer();
    static float left[7] = {0, 0, 0, 0, 0, 0, 0};
    if (!hooked() || c.identity()) return;
    float* tgt = reinterpret_cast<float*>(0x76a160);
    float* eye = reinterpret_cast<float*>(0x76a16c);
    float* fov = reinterpret_cast<float*>(0x7b38f4);
    const float now[7] = {tgt[0], tgt[1], tgt[2], eye[0], eye[1], eye[2], *fov};
    if (std::memcmp(now, left, sizeof now) == 0) return;
    for (int k = 0; k < 3; ++k) {
        eye[k] = tgt[k] + (eye[k] - tgt[k]) * c.dist + c.off[k];
        tgt[k] += c.off[k];
    }
    *fov *= c.fov;
    const float after[7] = {tgt[0], tgt[1], tgt[2], eye[0], eye[1], eye[2], *fov};
    std::memcpy(left, after, sizeof left);
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
    // mp_regen, boost_gain: what the step added to the gauge is scaled (a small gain only: not an herb, not a mod's
    // own write). The Boost's gauge is left alone while a Boost runs.
    static float last_mp = 0, last_boost = 0;
    float& mp = *reinterpret_cast<float*>(kMp);
    float& boost = *reinterpret_cast<float*>(kBoost);
    const float max_mp = *reinterpret_cast<float*>(kMaxMp);
    if (const float f = float(tune(3)), d = mp - last_mp; f != 1.0f && d > 0 && d < max_mp * 0.02f) mp = std::clamp(last_mp + d * f, 0.0f, max_mp);
    if (const float f = float(tune(4)), d = boost - last_boost; f != 1.0f && d > 0 && d < 6000.0f && !(*reinterpret_cast<std::uint32_t*>(0x76c198) & 0x20u))
        boost = std::clamp(last_boost + d * f, 0.0f, 30000.0f);
    last_mp = mp;
    last_boost = boost;
    // air_jumps: the mover allows one jump in the air with flag 163 (0x76bba8), once: +0x77c is set by it and
    // cleared on the ground. For each extra jump the mark is cleared again; without the ability the flag is lent
    // while the player is in the air and has jumps left.
    static int used = 0;
    static bool lent = false, was = false;
    std::int32_t& own = *reinterpret_cast<std::int32_t*>(0x76bba8);
    std::int32_t& mark = *reinterpret_cast<std::int32_t*>(player + 0x77c);
    const int extra = int(tune(5));
    if (*reinterpret_cast<std::int32_t*>(player + 0x388) != 0 || (extra <= 0 && !lent)) {
        if (lent) own = 0;
        used = 0;
        lent = was = false;
        return;
    }
    if (mark != 0 && !was) ++used;
    was = mark != 0;
    if (used < ((own != 0 && !lent) ? 1 : 0) + extra) {
        if (own == 0) { own = 1; lent = true; }
        if (mark != 0) { mark = 0; was = false; }
    } else if (lent) {
        own = 0;
        lent = false;
    }
}

}  // namespace ore
