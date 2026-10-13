// A mod's content/*.ini on the retail game: what CleriaCore's view_content.inl does with cleria::content.
//   chests*.ini    a chest gives another item: the grant filter, after the Lua mods' own
//   items*.ini     a retail item's name, description and value patched in the game's table; a new item
//                  (0x100..0x1FF) kept in the save's sidecar, with what obtaining it does
//   pickups*.ini   a pickup placed in a room, collected once per save (an item of the game's own: the
//                  retail pickup draws its icon from the game's table)
//   rules*.ini     On an event, Do actions
//   text*.ini      a script's strings, written into the script as the game loads it (files.cpp)
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "cleria/content.hpp"
#include "cleria/mod_events.hpp"
#include "cleria/vfs.hpp"
#include "ore.hpp"
#include "ui.hpp"

using namespace cleria;

namespace ore {

namespace {

content::Content g_c;
constexpr std::uintptr_t kItems = 0x76f1c0;   // INVINFO in memory: 0xb8 a record (name +0, magnitude +0x24, description +0x4c)
bool g_patched = false;

void obtained(int id, int count, const std::string& source, const std::string& script) {
    events::Event e;
    e.kind = events::Kind::ItemObtained;
    e.room = host().room();
    e.item = id;
    e.count = count;
    e.source = source;
    e.script = script;
    events::bus().emit(e);
}

// The retail items' patched fields, written over the game's own table once it is loaded.
void patch_items() {
    if (g_patched || !hooked()) return;
    char* first = reinterpret_cast<char*>(kItems);
    if (!first[0]) return;   // not loaded yet
    g_patched = true;
    for (const auto& [id, d] : g_c.items) {
        if (id < 0 || id >= content::kRetailItems) continue;
        char* rec = first + std::size_t(id) * 0xb8;
        if (d.name) std::snprintf(rec, 0x20, "%s", d.name->c_str());
        if (d.description) std::snprintf(rec + 0x4c, 0x6c, "%s", d.description->c_str());
        if (d.magnitude) std::memcpy(rec + 0x24, &*d.magnitude, 4);
    }
}

// The pickups of pickups.ini for this room that this save has not collected.
void place_pickups(const std::string& room) {
    for (const auto& [id, pd] : g_c.pickups) {
        if (pd.room != room || sidecar().flag(pd.flag) != 0) continue;
        if (pd.item < 0 || pd.item >= content::kRetailItems) {
            log("[content] pickup %s: item 0x%X is not one of the game's, the retail pickup cannot show it", id.c_str(), pd.item);
            continue;
        }
        if (spawn_pickup(pd.item, pd.pos, true)) log("[content] pickup %s placed in %s", id.c_str(), room.c_str());
    }
}

void on_event(const events::Event& e) {
    if (e.kind == events::Kind::RoomEnter) {
        patch_items();
        place_pickups(e.room);
    }
    for (const auto& [name, rule] : g_c.rules) {
        if (!content::rule_fires(rule, e, sidecar())) continue;
        log("[content] rule %s fires on %s", name.c_str(), events::kind_name(e.kind));
        content::RuleEnv env;
        env.flags = host().flags();
        env.sc = &sidecar();
        env.give = [](int item, int n) { content_give(item, n, "rule", {}); };
        env.message = [&](const std::string& m) {
            std::string head = rule.mod;
            for (const mods::Mod& md : mod_list())
                if (md.m.id == rule.mod) head = md.label();
            ui_notice(head, m);
        };
        content::run_rule(rule, env);
    }
}

}  // namespace

void content_start() {
    for (const mods::Mod& m : mod_list()) {
        if (m.state != mods::State::Active) continue;
        const int n = content::merge_dir(g_c, m.m.id, m.root / "content");
        if (n > 0) log("[content] %s: %d file(s)", m.label().c_str(), n);
    }
    for (const content::Note& e : g_c.errors) log("[content]   %s: %s", e.mod.c_str(), e.text.c_str());
    for (const content::Note& n : g_c.notes) log("[content]   %s: %s", n.mod.c_str(), n.text.c_str());
    if (g_c.empty()) return;
    events::bus().subscribe(&on_event);
}

bool content_on() { return !g_c.empty(); }
bool content_text_on() { return !g_c.text.empty(); }

// text*.ini on a script: an XSO file is a header (the code's length in words at +0x1c, the string count at +0x20),
// the code, the strings' offsets and the string pool, which ends the file. The pool is written again with the
// new strings; nothing else refers to it.
int content_patch_script(const std::string& name, std::vector<std::uint8_t>& xso) {
    if (g_c.text.empty() || xso.size() < 0x24 || std::memcmp(xso.data(), "XSR\0", 4) != 0) return 0;
    std::uint32_t words = 0, count = 0;
    std::memcpy(&words, xso.data() + 0x1c, 4);
    std::memcpy(&count, xso.data() + 0x20, 4);
    const std::size_t table = 0x24 + std::size_t(words) * 4, pool = table + std::size_t(count) * 4;
    if (count == 0 || count > 4096 || pool > xso.size()) return 0;
    std::vector<std::string> strings;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t off = 0;
        std::memcpy(&off, xso.data() + table + std::size_t(i) * 4, 4);
        std::size_t at = pool + off, n = 0;
        while (at + n < xso.size() && xso[at + n] != 0) ++n;
        strings.emplace_back(at < xso.size() ? reinterpret_cast<const char*>(xso.data() + at) : "", at < xso.size() ? n : 0);
    }
    int changed = 0;
    auto apply = [&](const content::TextDef& td) {                                // as content::patch_script
        for (const auto& [i, t] : td.by_index)
            if (i >= 0 && i < int(count) && strings[std::size_t(i)] != t) { strings[std::size_t(i)] = t; ++changed; }
        for (std::string& t : strings)
            if (const auto it = td.by_text.find(t); it != td.by_text.end() && it->second != t) { t = it->second; ++changed; }
    };
    if (const auto all = g_c.text.find("*"); all != g_c.text.end()) apply(all->second);
    if (const auto it = g_c.text.find(vfs::key(name)); it != g_c.text.end()) apply(it->second);
    if (!changed) return 0;
    std::vector<std::uint8_t> out(xso.begin(), xso.begin() + std::ptrdiff_t(pool));
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t off = std::uint32_t(out.size() - pool);
        std::memcpy(out.data() + table + std::size_t(i) * 4, &off, 4);
        out.insert(out.end(), strings[i].begin(), strings[i].end());
        out.push_back(0);
    }
    while (out.size() % 4) out.push_back(0);
    xso = std::move(out);
    return changed;
}

// As CleriaCore's content_give: a retail item's count cell (gold into SP); a new item by its Kind, kept in
// the sidecar.
bool content_give(int id, int count, const std::string& source, const std::string& script) {
    if (id < 0 || count < 1) return false;
    script::Host& h = host();
    if (id < content::kRetailItems) return false;   // the host's own give
    if (id < content::kNewItemFirst || id > content::kNewItemLast) return false;
    const auto d = g_c.items.find(id);
    const std::string kind = d == g_c.items.end() ? std::string() : d->second.kind;
    const int amount = d == g_c.items.end() ? 0 : d->second.amount;
    if (kind == "heal") {
        h.set_stat(script::Host::Stat::Hp, h.stat(script::Host::Stat::Hp) + double(amount) * count);
    } else if (kind == "gold") {
        h.set_stat(script::Host::Stat::Sp, h.stat(script::Host::Stat::Sp) + double(amount) * count);
    } else {
        sidecar().add_item(id, count);
        if (kind == "flag" && d != g_c.items.end() && !d->second.sets_flag.empty()) {
            const std::optional<int> fi = content::parse_int(d->second.sets_flag);
            if (fi && *fi >= 0 && *fi < 0x200) h.flags()[*fi] = 1;
            else sidecar().set_flag(d->second.sets_flag, 1);
        }
    }
    log("[content] item 0x%X x%d from %s", id, count, source.c_str());
    // A mod's own item has no icon in the game's table, so none rises from a chest: its icon, when the mod gave
    // one, is shown with a card of its name.
    if (ui_show_item_icon(id)) ui_notice("Obtained", content_item_name(id));
    obtained(id, count, source, script);
    return true;
}

std::string content_item_name(int id) {
    const auto d = g_c.items.find(id);
    return d != g_c.items.end() && d->second.name ? *d->second.name : std::string();
}

// chests.ini's answer to a grant no Lua filter answered. What a redirect gives that the script's own store
// does not (a new item) is granted here.
GrantDecision content_grant(const GrantQuery& q, const std::string& room) {
    std::vector<content::Obtained> got;
    const GrantDecision d = content::decide(g_c, q, room, nullptr, &got);
    for (const content::Obtained& o : got) {
        if (o.apply) content_give(o.item, o.count, "chest", q.script);
        else obtained(o.item, o.count, "chest", q.script);
    }
    return d;
}

// A pickup of pickups.ini was taken: the one of this room and item nearest the touch.
void content_pickup_taken(int item, const float* pos, const std::string& room) {
    const content::PickupDef* best = nullptr;
    float best_d = 9.0f;   // within 3 units: it was thrown up from its place
    for (const auto& [id, pd] : g_c.pickups) {
        if (pd.room != room || pd.item != item || sidecar().flag(pd.flag) != 0) continue;
        const float dx = pd.pos[0] - pos[0], dz = pd.pos[2] - pos[2], d2 = dx * dx + dz * dz;
        if (d2 < best_d) { best_d = d2; best = &pd; }
    }
    if (!best) return;
    sidecar().set_flag(best->flag, 1);
    log("[content] pickup %s collected", best->id.c_str());
    if (best->count > 1) host().flags()[item] = content::add_count(host().flags()[item], best->count - 1);   // the touch gave one
}

}  // namespace ore
