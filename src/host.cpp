// What a mod's script reaches of the retail game: cleria::script::Host over the memory of yso_win.exe
// (the build hooks.cpp checks; nothing here is read unless the hooks went in). What is not backed yet
// keeps script::Host's harmless default: gifts, the filters, storage, drawing, the page widgets.
#include <cstdint>
#include <cstdio>

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
