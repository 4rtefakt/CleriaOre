// The Get mods tab (included by main.cpp, after updates.inl): mods a player can install with one click.
//
// The list is catalog/mods.json in CleriaOre's repository, read from GitHub when the tab first opens: each mod's
// id, name, author, description and GitHub repository. Install takes the repository's latest release's .cleriamod
// and installs it as a dropped file is (take_updates: checked to be the same id, turned on). Adding a mod to the
// list is a change to that file; no new CleriaOre is needed. Tests: CLERIAORE_CATALOG=<a local mods.json>.

struct Listed {
    std::string id, name, author, description, repo;
};
struct Catalog {
    std::mutex mu;
    bool asked = false, busy = false;
    std::string status;          // "" = read; else why there is no list yet
    std::vector<Listed> mods;
    std::string installing;      // the id being downloaded
} g_cat;
constexpr char kCatalogUrl[] = "https://raw.githubusercontent.com/4rtefakt/CleriaOre/main/catalog/mods.json";

std::vector<Listed> parse_catalog(const std::string& text) {
    std::vector<Listed> out;
    const auto doc = json::parse(text);
    if (!doc) return out;
    auto str = [](const json::Value& v, const char* k) { const json::Value* f = v.find(k); return f ? f->as_string() : std::string(); };
    if (const json::Value* mods = doc->find("mods"))
        for (const json::Value& m : mods->as_array()) {
            Listed l{str(m, "id"), str(m, "name"), str(m, "author"), str(m, "description"), str(m, "repo")};
            if (mods::valid_id(l.id) && !github_repo("https://github.com/" + l.repo).empty()) out.push_back(std::move(l));
        }
    return out;
}

// The list's text: CLERIAORE_CATALOG's file, or this repository's on GitHub. "" when it cannot be had.
std::string catalog_text() {
    std::string text;
    if (const char* local = std::getenv("CLERIAORE_CATALOG")) return read_file(local, text) ? text : std::string();
    const net::HttpResponse rs = github_get(kCatalogUrl, 1u << 20);
    return rs.ok && rs.status == 200 ? rs.body : std::string();
}
std::map<std::string, std::string> catalog_repos() {
    std::map<std::string, std::string> out;
    for (const Listed& l : parse_catalog(catalog_text())) out[l.id] = l.repo;
    return out;
}

void read_catalog() {
    {
        std::lock_guard lk(g_cat.mu);
        if (g_cat.busy) return;
        g_cat.asked = g_cat.busy = true;
        g_cat.status = "Reading the list of mods...";
    }
    std::thread([] {
        const std::string text = catalog_text();
        std::string why = text.empty() ? "GitHub could not be reached: try again later" : "";
        std::vector<Listed> mods = why.empty() ? parse_catalog(text) : std::vector<Listed>{};
        if (why.empty() && mods.empty()) why = "The list of mods could not be read";
        std::lock_guard lk(g_cat.mu);
        g_cat.mods = std::move(mods);
        g_cat.status = why;
        g_cat.busy = false;
    }).detach();
}

// The latest release's .cleriamod of `l`, downloaded on a worker and handed to take_updates.
void install_listed(const Listed& l) {
    {
        std::lock_guard lk(g_cat.mu);
        if (!g_cat.installing.empty()) return;
        g_cat.installing = l.id;
    }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path dir = fs::path(tmp) / "CleriaOre" / "updates";
    std::thread([l, dir] {
        bool failed = false;
        const std::optional<Update> u = newer_release(l.repo, "0", ".cleriamod", &failed);
        std::string error;
        if (!u) error = l.name + ": " + (failed ? "GitHub could not be reached" : "no release to install yet");
        else {
            const net::HttpResponse rs = github_get(u->url, 64u << 20);
            if (rs.ok && rs.status == 200 && !rs.truncated && write_file(dir / u->file, rs.body)) {
                std::lock_guard lk(g_up.mu);
                g_up.ready.emplace_back(l.id, dir / u->file);
            } else {
                error = u->file + " could not be downloaded";
            }
        }
        std::lock_guard lk(g_cat.mu);
        g_cat.installing.clear();
        if (!error.empty()) {
            std::lock_guard lk2(g_up.mu);
            g_up.error = error;
        }
    }).detach();
}

void get_mods_page(ui::List& L) {
    if (g.game.empty()) {
        ui::section(L, "Get mods");
        ui::info(L, "Status", "The game was not found", ui::kWarn, "Choose the game's folder on the Game tab first.");
        return;
    }
    bool asked;
    {
        std::lock_guard lk(g_cat.mu);
        asked = g_cat.asked;
    }
    if (!asked) read_catalog();
    std::vector<Listed> mods;
    std::string status, installing;
    bool reading;
    {
        std::lock_guard lk(g_cat.mu);
        mods = g_cat.mods;
        status = g_cat.status;
        installing = g_cat.installing;
        reading = g_cat.busy;
    }
    if (!g.msg.empty()) ui::info(L, "", g.msg.c_str(), g.msg_bad ? ui::kWarn : ui::kGood, g.msg.c_str());
    ui::section(L, "Get mods");
    if (!status.empty()) {
        ui::info(L, "", status.c_str(), ui::kMuted);
        if (!reading && ui::button(L, "Try again")) read_catalog();
        return;
    }
    for (const Listed& l : mods) {
        const mods::Mod* have = find_mod(l.id);
        const bool busy = installing == l.id;
        std::string desc = l.name + (l.author.empty() ? "" : "\nby " + l.author) + "\n\n" + l.description +
                           (have ? "\n\nInstalled: " + have->m.version + ". Its page is on the Mods tab."
                                 : "\n\nOK installs its latest release and turns it on. Its settings are then on its page, "
                                   "on the Mods tab.");
        const ui::RowIo r = ui::row_frame(L, desc.c_str(), !have && installing.empty());
        ui::row_texts(L, r, l.name.c_str(), have ? "Installed" : busy ? "Installing..." : "Install", !have && installing.empty());
        if (r.activate && !have && installing.empty()) install_listed(l);
    }
    ui::paragraph(L, "Mods are made by their authors, not by CleriaOre. Each one's page on GitHub says what it does and how "
                     "it was tested.");
}
