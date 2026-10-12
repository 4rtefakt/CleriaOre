// What a mod's script reaches of the retail game: cleria::script::Host over the memory of yso_win.exe
// (the build hooks.cpp checks; nothing here is read unless the hooks went in). What is not backed yet
// keeps script::Host's harmless default: the setters, settings, drawing, the page widgets.
#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "cleria/content.hpp"
#include "cleria/mod_events.hpp"
#include "cleria/modscript.hpp"
#include "ore.hpp"

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

struct RetailHost : script::Host {
    void log(const std::string& mod, const std::string& text) override { ore::log("[lua:%s] %s", mod.c_str(), text.c_str()); }
    void notice(const std::string& mod, const std::string& head, const std::string& text) override {
        ore::log("[lua:%s] notice: %s %s", mod.c_str(), head.c_str(), text.c_str());
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

    // As CleriaCore's content_give for the retail ids: the count cell (0..99), the gold 0x78..0x7F into SP.
    void give(int id, int count, const std::string& source) override {
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
};

}  // namespace

script::Host& host() {
    static RetailHost h;
    return h;
}

}  // namespace ore
