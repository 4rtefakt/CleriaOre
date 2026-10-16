// Mod updates (included by main.cpp, after install_mod).
//
// A mod whose manifest's Website is a GitHub repository (https://github.com/<owner>/<repo>) is looked up there:
// its latest release, when its tag ("v1.2.0" or "1.2.0") is a newer version than the installed one and the
// release carries a .cleriamod, is offered as an update. The update is that file, installed as a dropped file is
// (modinstall.hpp: the older copy replaced, the mod's settings kept), once it is checked to be the same mod.
// The look-ups and the download run on a worker thread; each frame takes what they finished.

struct Update {
    std::string version, url, file;   // the release's version, its .cleriamod's address and name
};
struct Updates {
    std::mutex mu;
    std::map<std::string, Update> found;                     // mod id -> a newer release
    bool busy = false;                                       // a check or a download runs
    std::string status;                                      // what the last check or download came to
    std::vector<std::pair<std::string, fs::path>> ready;     // downloaded, to install: mod id, file
} g_up;
std::vector<std::string> g_up_queue;   // "Update N mods": the ones still to download, one at a time

// "https://github.com/owner/repo" (a trailing "/" or ".git" allowed) -> "owner/repo"; "" otherwise.
std::string github_repo(const std::string& website) {
    static const std::regex re(R"(^https?://(?:www\.)?github\.com/([A-Za-z0-9_.-]+)/([A-Za-z0-9_.-]+?)(?:\.git)?/?$)");
    std::smatch m;
    return std::regex_match(website, m, re) ? m[1].str() + "/" + m[2].str() : std::string();
}

net::HttpResponse github_get(const std::string& url, std::size_t max_body) {
    net::HttpRequest rq;
    rq.url = url;
    rq.headers = {{"User-Agent", "CleriaOre"}, {"Accept", "application/vnd.github+json"}};
    rq.timeout_ms = 30000;
    rq.max_body = max_body;
    return net::http_request(rq);
}

// Every mod with a GitHub Website: its latest release, on a worker.
void check_updates() {
    struct Ask { std::string id, version, repo; };
    std::vector<Ask> asks;
    for (const mods::Mod& m : g.list)
        if (const std::string r = github_repo(m.m.website); !r.empty() && !m.m.id.empty()) asks.push_back({m.m.id, m.m.version, r});
    {
        std::lock_guard lk(g_up.mu);
        if (g_up.busy) return;
        if (asks.empty()) { g_up.status = "No mod names a GitHub page to update from"; return; }
        g_up.busy = true;
        g_up.status = "Looking for updates...";
    }
    std::thread([asks] {
        std::map<std::string, Update> found;
        int failed = 0;
        for (const Ask& a : asks) {
            const net::HttpResponse rs = github_get("https://api.github.com/repos/" + a.repo + "/releases/latest", 1u << 20);
            const auto doc = rs.ok && rs.status == 200 ? json::parse(rs.body) : std::nullopt;
            if (!doc) { failed += !(rs.ok && rs.status == 404);   // 404: no release yet
                        continue; }
            std::string tag = doc->find("tag_name") ? doc->find("tag_name")->as_string() : std::string();
            if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
            if (tag.empty() || mods::compare_versions(tag, a.version) <= 0) continue;
            if (const json::Value* assets = doc->find("assets"))
                for (const json::Value& as : assets->as_array()) {
                    const std::string name = as.find("name") ? as.find("name")->as_string() : std::string();
                    const json::Value* url = as.find("browser_download_url");
                    if (url && name.size() > 10 && name.compare(name.size() - 10, 10, ".cleriamod") == 0) {
                        found[a.id] = {tag, url->as_string(), name};
                        break;
                    }
                }
        }
        std::lock_guard lk(g_up.mu);
        g_up.found = std::move(found);
        g_up.busy = false;
        const std::size_t n = g_up.found.size();
        g_up.status = failed && !n ? "GitHub could not be reached"
                    : n ? std::to_string(n) + (n == 1 ? " update" : " updates") + " found" : "Every mod is up to date";
    }).detach();
}

// The release's .cleriamod into %TEMP%\CleriaOre\updates, then installed by the next frame (take_updates).
void download_update(const std::string& id) {
    Update u;
    {
        std::lock_guard lk(g_up.mu);
        const auto it = g_up.found.find(id);
        if (g_up.busy || it == g_up.found.end()) return;
        u = it->second;
        g_up.busy = true;
        g_up.status = "Downloading " + u.file + "...";
    }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path to = fs::path(tmp) / "CleriaOre" / "updates" / u.file;
    std::thread([id, u, to] {
        const net::HttpResponse rs = github_get(u.url, 64u << 20);
        const bool ok = rs.ok && rs.status == 200 && !rs.truncated && write_file(to, rs.body);
        std::lock_guard lk(g_up.mu);
        g_up.busy = false;
        if (ok) g_up.ready.emplace_back(id, to);
        else g_up.status = u.file + " could not be downloaded" + (rs.ok ? " (HTTP " + std::to_string(rs.status) + ")" : ": " + rs.error);
    }).detach();
}

// Each frame: the downloads done, installed here (the list and the ini are this thread's); the next one queued.
void take_updates() {
    std::vector<std::pair<std::string, fs::path>> ready;
    bool busy;
    {
        std::lock_guard lk(g_up.mu);
        ready.swap(g_up.ready);
        busy = g_up.busy;
    }
    if (!busy && ready.empty() && !g_up_queue.empty()) {
        download_update(g_up_queue.front());
        g_up_queue.erase(g_up_queue.begin());
    }
    for (const auto& [id, file] : ready) {
        const mods::Package pkg = mods::inspect_package(file);
        if (pkg.ok() && pkg.m.id != id) {
            say(file.filename().string() + " is another mod (" + pkg.m.id + "), not installed", true);
        } else {
            const mods::Mod* before = find_mod(id);
            const bool was_on = before && before->enabled;
            install_mod(file);   // turns it on
            if (!g.msg_bad) {
                if (!was_on) {   // an update keeps the mod as it was
                    mods::Prefs p = mods::prefs_of(g.list, g.use);
                    std::erase(p.active, id);
                    apply(p);
                    say(g.msg.substr(0, g.msg.rfind(" \xE2\x80\x94 ")) + ": it stays off");   // without its "- enabled"
                }
                std::lock_guard lk(g_up.mu);
                g_up.found.erase(id);
                g_up.status.clear();
            }
        }
        std::error_code ec;
        fs::remove(file, ec);
    }
}

std::optional<Update> update_of(const std::string& id) {
    std::lock_guard lk(g_up.mu);
    const auto it = g_up.found.find(id);
    return it == g_up.found.end() ? std::nullopt : std::optional<Update>(it->second);
}
