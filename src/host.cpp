// What a mod's script reaches of the retail game: cleria::script::Host over the memory of yso_win.exe
// (the build hooks.cpp checks; nothing here is read unless the hooks went in). What is not backed yet
// keeps script::Host's harmless default: the setters, images and icons, a mod's data folder.
#include <algorithm>
#include <cstdint>
#include <cstdio>

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

struct RetailHost : script::Host {
    void log(const std::string& mod, const std::string& text) override { ore::log("[lua:%s] %s", mod.c_str(), text.c_str()); }
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
