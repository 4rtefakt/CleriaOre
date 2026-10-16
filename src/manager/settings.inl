// A mod's own settings page outside the game (included by main.cpp, after the App and the ini helpers).
//
// The mod's Lua script is started here as the loader starts it, on a host with no game (script::Host's
// defaults: in_game() false, no room, no player), no network (Limits::network) and no events, ticks or
// frames: only its pages are drawn, with the widgets of the game's F1 panel (the same kit), and what it sets
// goes to cleriaore.ini [cleria.mod.<id>], where the game reads it at its start. A page's buttons act on the
// game, so here they are shown and do nothing. The widgets are those of host.cpp's RetailHost.

struct ManagerHost : script::Host {
    std::optional<std::string> setting(const std::string& mod, const std::string& key) override {
        auto v = g.ini.raw("cleria.mod." + mod, key);   // as the loader reads it (ore.cpp setting)
        if (v && v->size() >= 2 && v->front() == '"' && v->back() == '"') v = v->substr(1, v->size() - 2);
        return v;
    }
    bool set_setting(const std::string& mod, const std::string& key, const std::string& value) override {
        g.ini.load(ore_dir() / "cleriaore.ini");   // what the game wrote meanwhile is kept
        g.ini.set_string("cleria.mod." + mod, key, value);
        return g.ini.save(ore_dir() / "cleriaore.ini");
    }
    fs::path data_dir(const std::string& mod) override { return ore_dir() / "modsdata" / mod; }
    std::vector<ModInfo> mod_list() override {
        std::vector<ModInfo> out;
        for (const mods::Mod& m : g.list) out.push_back({m.m.id, m.label(), m.m.version, m.state == mods::State::Active, !m.m.script.empty()});
        return out;
    }
    void log(const std::string&, const std::string&) override {}   // a window program: no console

    std::map<std::string, std::uint32_t> named_colors() override {
        return {{"gold", ui::kGold}, {"label", ui::kLabel}, {"value", ui::kValue}, {"muted", ui::kMuted},
                {"desc", ui::kDesc}, {"good", ui::kGood}, {"warn", ui::kWarn}, {"section", ui::kSection},
                {"panel", ui::kPanel & 0x00FFFFFFu}, {"edge", ui::kPanelEdge & 0x00FFFFFFu}, {"row", ui::kRow},
                {"white", IM_COL32(255, 255, 255, 255)}, {"black", IM_COL32(0, 0, 0, 255)}};
    }
    static ImFont* font(int f) { return f == 1 ? ui::F.medium : f == 2 ? ui::F.bold : f == 3 ? ui::F.cond : f == 4 ? ui::F.cond_med : ui::F.body; }
    std::pair<float, float> text_size(const std::string& t, float px, int f) override {
        const ImVec2 s = ui::text_size(font(f), px, t.c_str());
        return {s.x, s.y};
    }
    static ui::List& list(void* l) { return *static_cast<ui::List*>(l); }
    void ui_section(void* l, const std::string& title) override { ui::section(list(l), title.c_str()); }
    bool ui_toggle(void* l, const std::string& label, bool* v, const std::string& desc, bool enabled) override {
        return ui::toggle(list(l), label.c_str(), v, desc.c_str(), enabled);
    }
    bool ui_slider(void* l, const std::string& label, float* v, float lo, float hi, float step, const std::string& fmt,
                   const std::string& desc, bool enabled) override {
        return ui::slider(list(l), label.c_str(), v, lo, hi, step, fmt.c_str(), desc.c_str(), enabled);
    }
    bool ui_choice(void* l, const std::string& label, int* idx, const std::vector<std::string>& items, const std::string& desc,
                   bool enabled) override {
        std::vector<const char*> p;
        for (const std::string& s : items) p.push_back(s.c_str());
        return ui::choice(list(l), label.c_str(), idx, p.data(), int(p.size()), desc.c_str(), enabled);
    }
    bool ui_button(void* l, const std::string& label, const std::string& desc, bool) override {
        ui::button(list(l), label.c_str(), (desc + (desc.empty() ? "" : "\n\n") + "In the game only (F1 > Mods).").c_str(), false);
        return false;
    }
    bool ui_text_field(void* l, const std::string& label, std::string* v, const std::string& desc, bool secret,
                       const std::string& empty) override {
        return ui::text_field(list(l), label.c_str(), v, desc.c_str(), secret, empty.c_str());
    }
    void ui_info(void* l, const std::string& label, const std::string& value, std::uint32_t col, const std::string& desc) override {
        ui::info(list(l), label.c_str(), value.c_str(), col, desc.empty() ? nullptr : desc.c_str());
    }
    void ui_text(void* l, const std::string& text, std::uint32_t col) override { ui::paragraph(list(l), text.c_str(), col); }
    void ui_table(void* l, const std::vector<std::string>& cells, bool header, std::uint32_t col) override {
        ui::table_row(list(l), cells, header, col);
    }
    void ui_progress(void* l, const std::string& label, double fraction, const std::string& text, std::uint32_t col) override {
        ui::progress(list(l), label.c_str(), float(fraction), text.c_str(), col);
    }
};
ManagerHost g_host;
std::map<std::string, std::unique_ptr<script::Mod>> g_scripts;   // started at their page's first draw; dropped by reload_mods

// The script of `m`, started now if it was not; null when the mod has none (or one too old to run).
script::Mod* script_of(const mods::Mod& m) {
    if (m.m.script.empty() || m.m.api < script::kScriptApi || !m.problems.empty()) return nullptr;
    auto& s = g_scripts[m.m.id];
    if (!s) {
        script::Limits lim;
        lim.network = false;
        s = std::make_unique<script::Mod>(m.m.id, m.label(), m.m.version, m.root, g_host, lim);
        s->start(m.m.script);
    }
    return s.get();
}

// The pages its script registered, at the top of its page as in CleriaCore's Mods tab (a title each when
// there are several).
void settings_pages(ui::List& L, const mods::Mod& m) {
    script::Mod* s = script_of(m);
    if (!s) return;
    if (!s->ok()) {
        ui::info(L, "Settings", "Its script stopped", ui::kWarn, ("Its settings could not be shown here:\n\n" + s->error()).c_str());
        return;
    }
    const std::vector<std::string> pages = s->pages();
    if (!pages.empty() && g.running)
        ui::info(L, "", "The game runs: changes apply when it restarts", ui::kWarn,
                 "The game read the mods' settings at its start. A change made here applies the next time it starts; "
                 "for one that applies at once, use the mod's page in the game (F1 > Mods).");
    for (std::size_t i = 0; i < pages.size(); ++i) {
        if (pages.size() > 1) ui::section(L, pages[i].c_str());
        s->draw_page(i, &L);
    }
}
