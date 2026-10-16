// CleriaOre.exe: the installer, and the mods and the loader managed outside the game.
//
// It is the one file a player downloads: the loader (dinput8.dll) and the panel's fonts are inside it
// (res/cleriaore.rc). It finds the game (its own folder, the folder chosen last time, Steam's libraries),
// puts the loader into it with a copy of itself (<game>/cleriaore/CleriaOre.exe, and a Start menu shortcut
// to it), and edits what the loader reads at the game's start: <game>/cleriaore/mods/ and cleriaore.ini [cleria.mods] (the same keys the F1 panel in
// the game writes). The window is the panel of the game's F1 (kit.inl): a Mods tab modelled on CleriaCore's
// own (apps/cleria-view/view_mods.inl) and a Game tab.
#include <windows.h>
#include <commdlg.h>
#include <d3d9.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <thread>

#include "ui.hpp"   // Dear ImGui and the UI kit

#include "cleria/fileassoc.hpp"
#include "cleria/ini.hpp"
#include "cleria/json.hpp"
#include "cleria/modinstall.hpp"
#include "cleria/mods.hpp"
#include "cleria/modscript.hpp"
#include "cleria/net/socket.hpp"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "mark.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using namespace cleria;

namespace {

constexpr std::uint32_t kLinkTime = 0x659ca02d;   // the yso_win.exe the loader's hooks are of (hooks.cpp)
constexpr wchar_t kSteamRun[] = L"steam://rungameid/207350";
constexpr char kBackup[] = "dinput8.dll.before-cleriaore";   // another dinput8.dll found in the game folder

enum class Loader { None, Ours, OtherVersion, Foreign };

// What the exe carries (res/cleriaore.rc).
std::string resource(const wchar_t* name) {
    const HRSRC r = FindResourceW(nullptr, name, MAKEINTRESOURCEW(10));   // RT_RCDATA
    const HGLOBAL h = r ? LoadResource(nullptr, r) : nullptr;
    return h ? std::string(static_cast<const char*>(LockResource(h)), SizeofResource(nullptr, r)) : std::string();
}
struct Carried { const wchar_t* name; const char* file; };
constexpr Carried kFonts[] = {{L"FONT1", "FiraSans-Regular.ttf"}, {L"FONT2", "FiraSans-Medium.ttf"}, {L"FONT3", "FiraSans-SemiBold.ttf"},
                              {L"FONT4", "FiraSansCondensed-SemiBold.ttf"}, {L"FONT5", "FiraSansCondensed-Medium.ttf"}, {L"FONT6", "OFL.txt"}};

bool write_file(const fs::path& p, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    f.write(bytes.data(), std::streamsize(bytes.size()));
    return bool(f);
}
bool write_fonts(const fs::path& dir) {
    bool ok = true;
    for (const Carried& c : kFonts) ok = write_file(dir / c.file, resource(c.name)) && ok;
    return ok;
}

fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    fs::path out;
    if (SHGetKnownFolderPath(id, 0, nullptr, &p) == S_OK) out = p;
    CoTaskMemFree(p);
    return out;
}
fs::path own_ini() { return known_folder(FOLDERID_RoamingAppData) / "CleriaOre" / "manager.ini"; }   // this exe's own note
fs::path shortcut() { return known_folder(FOLDERID_Programs) / "CleriaOre.lnk"; }                     // the Start menu's

bool make_shortcut(const fs::path& lnk, const fs::path& target) {
    IShellLinkW* link = nullptr;
    if (CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&link)) != S_OK) return false;
    link->SetPath(target.wstring().c_str());
    link->SetWorkingDirectory(target.parent_path().wstring().c_str());
    link->SetDescription(L"Mods for Ys Origin");
    IPersistFile* file = nullptr;
    bool ok = false;
    if (link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)) == S_OK) {
        ok = file->Save(lnk.wstring().c_str(), TRUE) == S_OK;
        file->Release();
    }
    link->Release();
    return ok;
}

struct App {
    fs::path self;                    // this exe
    fs::path game;                    // the game folder, "" = not found
    bool known_build = false;         // its yso_win.exe is the build the loader hooks
    Loader loader = Loader::None;
    bool running = false;             // the game runs now
    Ini ini;                          // <game>/cleriaore/cleriaore.ini
    bool use = true;                  // [cleria.mods] UseMods
    std::vector<mods::Mod> list;      // what is installed, with the choices
    std::string msg;                  // the last action's result
    bool msg_bad = false;
    std::string sub;                  // the Mods tab's sub-page: "" = the list, or a mod id
    std::optional<std::string> next;  // a sub-page to switch to at the start of the next frame
    std::string removing;             // the mod id whose Remove was pressed once
    std::vector<fs::path> dropped;    // files dropped on the window, or handed over by another CleriaOre.exe
    bool refresh = false;             // look at the game folder again: the window came back, the game quit
    bool testing = false;             // CLERIAORE_GAME: a scratch game folder, the registry left alone
};
App g;
ui::Panel g_panel;

fs::path ore_dir() { return g.game / "cleriaore"; }
fs::path mods_dir() { return ore_dir() / "mods"; }

void say(const std::string& text, bool bad = false) {
    g.msg = text;
    g.msg_bad = bad;
}

#include "settings.inl"   // a mod's settings page, its script run here

bool read_file(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// ---- the game and the loader in it -------------------------------------------------------------------
bool is_game(const fs::path& dir) {
    std::error_code ec;
    return !dir.empty() && fs::exists(dir / "yso_win.exe", ec);
}

// Steam's library folders: <Steam>/steamapps/libraryfolders.vdf, each `"path" "D:\\SteamLibrary"`.
fs::path steam_game() {
    wchar_t steam[MAX_PATH];
    DWORD n = sizeof steam;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam, &n) != ERROR_SUCCESS)
        return {};
    std::vector<fs::path> libraries{fs::path(steam)};
    std::string vdf;
    if (read_file(libraries[0] / "steamapps" / "libraryfolders.vdf", vdf)) {
        static const std::regex path("\"path\"\\s+\"([^\"]+)\"");
        for (std::sregex_iterator it(vdf.begin(), vdf.end(), path), end; it != end; ++it)
            libraries.push_back(fs::path(std::regex_replace((*it)[1].str(), std::regex("\\\\\\\\"), "\\")));
    }
    for (const fs::path& lib : libraries)
        if (const fs::path dir = lib / "steamapps" / "common" / "Ys Origin"; is_game(dir)) return dir;
    return {};
}

bool game_runs() {
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof e;
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e)) {
        if (_wcsicmp(e.szExeFile, L"yso_win.exe") != 0) continue;
        found = true;   // this game folder's, when its path can be read (another copy of the game is not ours)
        if (const HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID)) {
            wchar_t path[MAX_PATH];
            DWORD n = MAX_PATH;
            std::error_code ec;
            if (QueryFullProcessImageNameW(p, 0, path, &n)) found = fs::equivalent(fs::path(path), g.game / "yso_win.exe", ec);
            CloseHandle(p);
        }
    }
    CloseHandle(snap);
    return found;
}

// What the game folder holds: the exe's build, and whose dinput8.dll is there.
void look_at_game() {
    g.known_build = false;
    g.loader = Loader::None;
    if (g.game.empty()) return;
    std::string exe, there, ours;
    if (std::ifstream f(g.game / "yso_win.exe", std::ios::binary); f) {   // the PE header's TimeDateStamp
        exe.resize(0x400);
        f.read(exe.data(), std::streamsize(exe.size()));
        std::uint32_t pe = 0, stamp = 0;
        std::memcpy(&pe, exe.data() + 0x3c, 4);
        if (pe + 12 <= exe.size()) std::memcpy(&stamp, exe.data() + pe + 8, 4);
        g.known_build = stamp == kLinkTime;
    }
    if (!read_file(g.game / "dinput8.dll", there)) return;
    ours = resource(L"LOADER");
    g.loader = there == ours ? Loader::Ours : there.find(ore::kLoaderMark) != std::string::npos ? Loader::OtherVersion : Loader::Foreign;
}

void reload_mods() {
    g_scripts.clear();   // started again at their page's next draw, on what the files say now
    g.list.clear();
    g.ini = Ini{};
    if (g.game.empty()) return;
    g.ini.load(ore_dir() / "cleriaore.ini");
    mods::Prefs p;
    p.use = g.use = g.ini.get_int("cleria.mods", "UseMods").value_or(1) != 0;
    p.order = mods::split_list(g.ini.get_string("cleria.mods", "Order").value_or(""));
    p.active = mods::split_list(g.ini.get_string("cleria.mods", "Active").value_or(""));
    g.list = mods::discover(mods_dir());
    mods::arrange(g.list, p);
}

// This exe's own note of the game folder, for a game Steam does not list.
void set_game(const fs::path& dir, bool remember) {
    g.game = dir;
    g.sub.clear();
    if (remember) {
        Ini own;
        own.load(own_ini());
        own.set_string("manager", "Game", dir.string());
        own.save(own_ini());
    }
    look_at_game();
    reload_mods();
}

void find_game() {
    if (const wchar_t* forced = _wgetenv(L"CLERIAORE_GAME"); forced && *forced) {
        g.testing = true;
        return set_game(forced, false);
    }   // tests: a scratch game folder
    Ini own;
    own.load(own_ini());
    const fs::path saved = own.get_string("manager", "Game").value_or("");
    const fs::path here = g.self.parent_path();   // installed, it is <game>/cleriaore/CleriaOre.exe
    set_game(is_game(here) ? here : is_game(here.parent_path()) ? here.parent_path() : is_game(saved) ? saved : steam_game(), false);
}

// The choices -> cleriaore.ini, as the loader's F1 panel writes them (ore.cpp save_prefs).
void apply(mods::Prefs p) {
    p.use = g.use;
    mods::arrange(g.list, p);
    std::error_code ec;
    fs::create_directories(ore_dir(), ec);
    g.ini.load(ore_dir() / "cleriaore.ini");   // what the game wrote since (a mod's settings) is kept
    g.ini.set_int("cleria.mods", "UseMods", p.use ? 1 : 0);
    g.ini.set_string("cleria.mods", "Order", mods::join_list(p.order));
    g.ini.set_string("cleria.mods", "Active", mods::join_list(p.active));
    if (!g.ini.save(ore_dir() / "cleriaore.ini")) say("cleriaore.ini could not be written", true);
}

const mods::Mod* find_mod(const std::string& id) {
    for (const mods::Mod& m : g.list)
        if (m.m.id == id) return &m;
    return nullptr;
}

// On: the mods it requires are turned on with it (and theirs). Off: the mods that require it are named,
// as they will not load without it.
void toggle_mod(int i) {
    const std::string id = g.list[std::size_t(i)].m.id;
    if (id.empty()) return;
    const std::string name = g.list[std::size_t(i)].label();
    mods::Prefs p = mods::prefs_of(g.list, g.use);
    auto on = [&](const std::string& x) { return std::find(p.active.begin(), p.active.end(), x) != p.active.end(); };
    std::string also, missing, needed_by;
    if (!on(id)) {
        g.use = true;   // turning a mod on also turns UseMods on
        std::vector<std::string> todo{id};
        while (!todo.empty()) {
            const std::string x = todo.back();
            todo.pop_back();
            if (on(x)) continue;
            p.active.push_back(x);
            if (x != id) also += (also.empty() ? "" : ", ") + find_mod(x)->label();
            for (const mods::Dep& d : find_mod(x)->m.requires_) {
                if (find_mod(d.id)) todo.push_back(d.id);
                else missing += (missing.empty() ? "" : ", ") + d.id;
            }
        }
    } else {
        p.active.erase(std::find(p.active.begin(), p.active.end(), id));
        for (const mods::Mod& m : g.list)
            if (m.enabled && m.m.id != id)
                for (const mods::Dep& d : m.m.requires_)
                    if (d.id == id) needed_by += (needed_by.empty() ? "" : ", ") + m.label();
    }
    apply(p);
    if (!missing.empty()) say(name + " needs " + missing + ", which is not installed", true);
    else if (!also.empty()) say(name + " is on, with " + also + ", which it needs");
    else if (!needed_by.empty()) say(name + " is off: " + needed_by + " cannot load without it", true);
    else say(name + (on(id) ? " is on" : " is off") + std::string(g.running ? ": restart the game to apply" : ""));
}

bool move_mod(int i, int d) {
    const int j = i + d;
    if (i < 0 || j < 0 || i >= int(g.list.size()) || j >= int(g.list.size())) return false;
    std::swap(g.list[std::size_t(i)], g.list[std::size_t(j)]);
    apply(mods::prefs_of(g.list, g.use));
    return true;
}

void install_mod(const fs::path& file) {
    if (g.game.empty()) return say("Find the game first (the Game tab)", true);
    std::error_code ec;
    fs::create_directories(mods_dir(), ec);
    mods::Prefs p = mods::prefs_of(g.list, g.use);
    const mods::InstallResult r = mods::install(file, mods_dir(), p);
    if (r.ok) {
        g.use = p.use;
        g.list = mods::discover(mods_dir());
        apply(p);
        g_scripts.erase(r.id);   // its page runs the new script
        if (g.running) mods::queue_enable(mods_dir(), r.id);   // the game may write its own list over ours before it quits
    }
    say(r.message() + (r.ok && g.running ? ": restart the game to play with it" : ""), !r.ok);
}

// Into the Recycle Bin: its folder, or its .zip / .cleriamod.
void remove_mod(const mods::Mod& m) {
    std::wstring from = m.source.wstring();
    from.push_back(L'\0');   // the list ends with two
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    const std::string name = m.label();
    if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) return say(name + " could not be removed", true);
    mods::Prefs p = mods::prefs_of(g.list, g.use);
    g.list = mods::discover(mods_dir());
    apply(p);
    say(name + " was moved to the Recycle Bin");
}

#include "updates.inl"   // a mod's newer release on GitHub

// .cleriamod files opened by a double-click with the installed CleriaOre.exe (CleriaCore's file association,
// fileassoc.hpp: the same ProgId, so whichever of the two was installed last opens them).
fs::path installed_exe() { return ore_dir() / "CleriaOre.exe"; }
std::string installed_exe_utf8() {
    const std::u8string u = installed_exe().u8string();
    return std::string(u.begin(), u.end());
}
bool opens_mod_files() {
    const auto reg = g.testing ? nullptr : fileassoc::user_registry();
    return reg != nullptr && fileassoc::is_current(*reg, installed_exe_utf8());
}
bool associate(bool want) {
    const auto reg = g.testing ? nullptr : fileassoc::user_registry();
    if (!reg) return false;
    bool ok = true;
    if (fileassoc::sync(*reg, installed_exe_utf8(), want, &ok) != fileassoc::Action::None) fileassoc::notify_shell();
    return ok;
}

// The loader and the fonts into the game folder, this exe beside the mods, a shortcut to it in the Start menu.
void install_loader() {
    const fs::path to = g.game / "dinput8.dll", exe = ore_dir() / "CleriaOre.exe";
    std::error_code ec;
    if (g.loader == Loader::Foreign) {   // somebody else's: kept beside it, put back by Remove
        if (fs::exists(g.game / kBackup, ec)) return say(std::string("Another dinput8.dll is there, and ") + kBackup + " already exists: move one of them away", true);
        fs::rename(to, g.game / kBackup, ec);
        if (ec) return say("The other dinput8.dll could not be moved aside: " + ec.message(), true);
    }
    if (!write_file(to, resource(L"LOADER")))
        return say("dinput8.dll could not be written" + std::string(g.running ? " (close the game first)" : " into the game folder"), true);
    fs::create_directories(mods_dir(), ec);
    write_fonts(ore_dir() / "fonts");
    if (!fs::equivalent(g.self, exe, ec)) fs::copy_file(g.self, exe, fs::copy_options::overwrite_existing, ec);
    const bool linked = fs::exists(exe, ec) && (g.testing || make_shortcut(shortcut(), exe));
    if (linked) associate(true);
    look_at_game();
    say(linked ? "Installed. CleriaOre is in the Start menu; start the game and press F1" : "The loader is installed. Start the game and press F1");
}

void remove_loader() {
    std::error_code ec;
    fs::remove(g.game / "dinput8.dll", ec);
    if (ec) return say("dinput8.dll could not be removed" + std::string(g.running ? " (close the game first)" : ": " + ec.message()), true);
    const bool back = fs::exists(g.game / kBackup, ec);
    if (back) fs::rename(g.game / kBackup, g.game / "dinput8.dll", ec);
    fs::remove(shortcut(), ec);
    associate(false);   // only when it is ours
    look_at_game();
    say(back ? "The loader is removed; the dinput8.dll that was there before is back" : "The loader is removed. The mods and their settings stay in cleriaore/");
}

fs::path pick_file(HWND owner, const wchar_t* title, const wchar_t* filter) {
    wchar_t file[1024] = L"";
    OPENFILENAMEW o{};
    o.lStructSize = sizeof o;
    o.hwndOwner = owner;
    o.lpstrFilter = filter;
    o.lpstrFile = file;
    o.nMaxFile = DWORD(std::size(file));
    o.lpstrTitle = title;
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&o) ? fs::path(file) : fs::path();
}

void open_path(const fs::path& p) { ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

// ---- the Mods tab (after CleriaCore's view_mods.inl) -------------------------------------------------
std::string deps(const std::vector<mods::Dep>& v) {
    std::string s;
    for (const mods::Dep& x : v) s += (s.empty() ? "" : ", ") + x.id + (x.op.empty() ? "" : " " + x.op + " " + x.version);
    return s;
}

std::string describe(const mods::Mod& m) {
    std::string d = m.label();
    if (!m.m.version.empty()) d += "  " + m.m.version;
    if (!m.m.author.empty()) d += "\nby " + m.m.author;
    if (!m.m.description.empty()) d += "\n\n" + m.m.description;
    d += "\n\n" + (m.m.id.empty() ? std::string("(no id)") : m.m.id) + "  \xE2\x80\xA2  " + std::to_string(m.files) +
         " file(s)  \xE2\x80\xA2  " + m.source.filename().string() + (m.zip ? "" : "/");
    if (!m.m.requires_.empty()) d += "\nRequires: " + deps(m.m.requires_);
    if (!m.m.conflicts.empty()) d += "\nConflicts with: " + deps(m.m.conflicts);
    for (const std::string& e : m.errors) d += "\nError: " + e;
    for (const std::string& n : m.notes) d += "\n" + n;
    return d;
}

// One mod in the list: "N  NAME  version" on the left; the up / down arrows, its state and a chevron on the
// right. OK / a click opens its page; a click on the state turns it on / off; Left / Right or the arrows
// move it in the load order.
void mod_row(ui::List& L, int i) {
    mods::Mod& m = g.list[std::size_t(i)];
    const std::string desc = describe(m) +
        "\n\nOK / click opens its page. A click on On / Off turns it on or off here. Left / Right or the arrows move it "
        "up / down the load order: when two mods change the same file, the one lower in the list wins. Changes apply "
        "when the game starts.";
    ui::RowIo r = ui::row_frame(L, desc.c_str(), true);
    const float S = ui::S, cy = (r.a.y + r.b.y) * 0.5f;
    int move = 0;
    const float rx = r.b.x - r.pad + 13.0f * S;   // the plate's right end, inside its point
    const float ax = rx - 126.0f * S;
    for (int k = 0; k < 2; ++k) {   // the arrows (mouse); a click on one does not open the page
        const ImVec2 c{ax + float(k) * 24.0f * S, cy};
        const ImVec2 a{c.x - 10.0f * S, r.a.y + 2.0f * S}, b{c.x + 10.0f * S, r.b.y - 2.0f * S};
        const bool hov = L.focus && ui::hovering(a, b);
        if (hov) ui::hand();
        if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { move = k == 0 ? -1 : 1; r.activate = false; }
        const ImU32 col = hov ? ui::kTabOn : (r.selected ? ui::kGold : ui::kRowEdge);
        const float h = 5.0f * S, w = 6.0f * S;
        if (k == 0) L.dl->AddTriangleFilled({c.x - w, cy + h * 0.6f}, {c.x + w, cy + h * 0.6f}, {c.x, cy - h}, col);
        else L.dl->AddTriangleFilled({c.x - w, cy - h * 0.6f}, {c.x + w, cy - h * 0.6f}, {c.x, cy + h}, col);
    }
    if (r.delta != 0) move = r.delta;
    const float lpx = 18.0f * S, vpx = 16.5f * S;
    char num[8];
    std::snprintf(num, sizeof num, "%d", i + 1);
    float x = r.a.x + r.pad;
    ui::text(L.dl, ui::F.serif, lpx, {x, cy - lpx * 0.58f}, ui::kTabOff, num);
    x += 26.0f * S;
    const std::string& name = m.label();
    ui::text_sh(L.dl, ui::F.serif, lpx, {x, cy - lpx * 0.58f}, ui::kLabel, name.c_str());
    x += ui::text_size(ui::F.serif, lpx, name.c_str()).x + 10.0f * S;
    if (x < ax - 60.0f * S) {   // its version, why it cannot load, or its update
        const bool bad = m.state == mods::State::Error && !m.errors.empty();
        const std::optional<Update> up = update_of(m.m.id);
        const std::string note = bad ? m.errors.front() : up ? m.m.version + "  \xE2\x86\x92  " + up->version : m.m.version;
        L.dl->PushClipRect({x, r.a.y}, {ax - 18.0f * S, r.b.y}, true);
        ui::text(L.dl, ui::F.body, 15.0f * S, {x, cy - 15.0f * S * 0.55f}, bad ? ui::kWarn : up ? ui::kGold : ui::kMuted, note.c_str());
        L.dl->PopClipRect();
    }
    // the state: a click on it toggles
    const char* v = m.state == mods::State::Error ? "Error" : m.enabled ? "On" : "Off";
    const ImU32 vc = m.state == mods::State::Error ? ui::kWarn : m.enabled ? ui::kGood : ui::kMuted;
    ImFont* vf = r.selected ? ui::F.medium : ui::F.body;
    const ImVec2 vs = ui::text_size(vf, vpx, v);
    const float vx = rx - 32.0f * S - vs.x;
    const ImVec2 sa{vx - 8.0f * S, r.a.y + 5.0f * S}, sb{rx - 26.0f * S, r.b.y - 5.0f * S};
    bool toggle = false;
    if (L.focus && ui::hovering(sa, sb)) {
        ui::hand();
        L.dl->AddRect(sa, sb, ui::kRowEdge, 0.0f, 0, 1.0f * S);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { toggle = true; r.activate = false; }
    }
    ui::text(L.dl, vf, vpx, {vx, cy - vpx * 0.55f}, vc, v);
    {   // the chevron: OK opens the page
        const float cx = rx - 15.0f * S;
        const ImU32 cc = r.selected ? ui::kGold : ui::kTabOff;
        L.dl->AddLine({cx - 3.0f * S, cy - 5.0f * S}, {cx + 2.0f * S, cy}, cc, 1.6f * S);
        L.dl->AddLine({cx + 2.0f * S, cy}, {cx - 3.0f * S, cy + 5.0f * S}, cc, 1.6f * S);
    }
    if (toggle) toggle_mod(i);
    else if (r.activate && !m.m.id.empty()) g.next = m.m.id;
    else if (move != 0 && move_mod(i, move)) L.sel += move;   // the selection follows the mod
}

// Mods > <mod>: On / Off, the load order, the manifest, the errors and notes, Remove.
void mod_page(ui::List& L) {
    int i = -1;
    for (int k = 0; k < int(g.list.size()); ++k)
        if (g.list[std::size_t(k)].m.id == g.sub) i = k;
    if (i < 0) { g.next = ""; return; }   // gone (a rescan, a removal)
    mods::Mod& m = g.list[std::size_t(i)];
    if (ui::back_row(L, "Mods", m.label().c_str(), describe(m).c_str())) g.next = "";
    if (const std::optional<Update> up = update_of(m.m.id)) {
        ui::section(L, "Update");
        const std::string label = "Update to " + up->version;
        if (ui::button(L, label.c_str(), ("Downloads " + up->file + " from the mod's GitHub page and installs it in place of " +
                                          m.m.version + ". Its settings stay.").c_str()))
            download_update(m.m.id);
    }
    settings_pages(L, m);
    ui::section(L, "Mod");
    bool on = m.enabled;
    if (ui::toggle(L, "Enabled", &on, "Turns the mod on or off. New mods start off, so a dropped-in folder never changes the "
                                      "game by itself. Changes apply when the game starts."))
        toggle_mod(i);
    ui::info(L, "State", m.state == mods::State::Error ? "Error" : m.enabled ? "On" : "Off",
             m.state == mods::State::Error ? ui::kWarn : m.enabled ? ui::kGood : ui::kMuted);
    {   // the load order: Left / Right move it
        char pos[32];
        std::snprintf(pos, sizeof pos, "%d of %d", i + 1, int(g.list.size()));
        const ui::RowIo r = ui::row_frame(L, "Its place in the load order. Left / Right move it up / down: when two mods "
                                             "change the same file, the one lower in the list wins.", true);
        ui::row_texts(L, r, "Load order", pos, true);
        if (r.delta != 0 && move_mod(i, r.delta)) return;   // `m` is another mod now
    }
    ui::section(L, "About");
    if (!m.m.version.empty()) ui::info(L, "Version", m.m.version.c_str());
    if (!m.m.author.empty()) ui::info(L, "Author", m.m.author.c_str());
    if (!m.m.website.empty() && ui::choice_label(L, "Website", m.m.website.c_str(), "Opens it in the browser.") &&
        (m.m.website.rfind("https://", 0) == 0 || m.m.website.rfind("http://", 0) == 0))
        open_path(m.m.website);
    ui::info(L, "Id", m.m.id.c_str());
    {
        const std::string src = m.source.filename().string() + (m.zip ? "" : "/");
        ui::info(L, "Source", src.c_str(), ui::kValue, m.source.string().c_str());
    }
    ui::info(L, "Files", std::to_string(m.files).c_str(), ui::kValue, "Game files it replaces or adds (its files/ folder).");
    if (!m.m.script.empty()) ui::info(L, "Script", m.m.script.c_str(), ui::kValue, "Its Lua entry file.");
    ui::info(L, "Mod API", std::to_string(m.m.api).c_str(), ui::kValue, "The API level the mod was written for.");
    if (!m.m.requires_.empty()) ui::info(L, "Requires", deps(m.m.requires_).c_str());
    if (!m.m.conflicts.empty()) ui::info(L, "Conflicts with", deps(m.m.conflicts).c_str());
    for (const std::string& e : m.errors) ui::info(L, "Error", e.c_str(), ui::kWarn, e.c_str());
    for (const std::string& n : m.notes) ui::info(L, "Note", n.c_str(), ui::kMuted, n.c_str());
    ui::section(L, "Remove");
    const bool sure = g.removing == m.m.id;
    if (ui::button(L, sure ? "Press again to remove it" : "Remove this mod",
                   "Moves the mod's folder or file to the Recycle Bin. What it kept with your saves and its settings stay.")) {
        if (sure) { remove_mod(m); g.removing.clear(); g.next = ""; }
        else g.removing = m.m.id;
    }
}

void mods_page(ui::List& L) {
    if (g.next) {   // a sub-page asked for last frame: switch now, with the list's selection at its top
        g.sub = *g.next;
        g.next.reset();
        g.removing.clear();
        L.sel = 0;
        L.scroll = L.scroll_to = 0;
    }
    if (g.game.empty()) {
        ui::section(L, "Mods");
        ui::info(L, "Status", "The game was not found", ui::kWarn, "Choose the game's folder on the Game tab first.");
        return;
    }
    if (!g.sub.empty()) return mod_page(L);
    {
        std::size_t on = 0;
        for (const mods::Mod& m : g.list) on += m.state == mods::State::Active;
        std::string s = !g.use ? "Mods off" : std::to_string(on) + (on == 1 ? " mod on" : " mods on");
        if (g.loader == Loader::None || g.loader == Loader::Foreign) s += "  \xE2\x80\xA2  the loader is not installed";
        else if (g.running) s += "  \xE2\x80\xA2  applies when the game restarts";
        ui::info(L, "Status", s.c_str(), g.loader == Loader::None || g.loader == Loader::Foreign || g.running ? ui::kWarn : ui::kGood,
                 g.running ? "The game runs now: it keeps the mods it started with. Do not change the mods in its F1 panel and "
                             "here at once: the last one to write wins."
                           : nullptr);
    }
    if (!g.msg.empty()) ui::info(L, "", g.msg.c_str(), g.msg_bad ? ui::kWarn : ui::kGood, g.msg.c_str());
    if (!g.running && (g.loader == Loader::Ours || g.loader == Loader::OtherVersion) &&
        ui::button(L, "Start the game", "Through Steam, with the mods as they are set here."))
        ShellExecuteW(nullptr, L"open", kSteamRun, nullptr, nullptr, SW_SHOWNORMAL);
    ui::section(L, "Add and update");
    if (ui::button(L, "Install a mod file...", "A .cleriamod or a .zip of a mod. It is checked, copied into the mods folder and "
                                                "turned on. You can also drop the file on this window.")) {
        if (const fs::path f = pick_file(GetActiveWindow(), L"Install a mod", L"Mods (*.cleriamod;*.zip)\0*.cleriamod;*.zip\0All files\0*.*\0"); !f.empty())
            install_mod(f);
    }
    {
        std::string status;
        bool busy;
        std::vector<std::string> ids;
        {
            std::lock_guard lk(g_up.mu);
            status = g_up.status;
            busy = g_up.busy;
            for (const auto& [id, u] : g_up.found) ids.push_back(id);
        }
        const std::string desc = std::string(ids.size() > 1 ? "Downloads and installs every update found. Each mod's settings stay."
                                                            : "Looks on the GitHub page of each mod that names one (its Website) for a "
                                                              "newer release.") + (status.empty() ? "" : "\n\nLast check: " + status);
        if (ui::button(L, busy ? status.c_str() : ids.size() > 1 ? ("Update " + std::to_string(ids.size()) + " mods").c_str()
                                                 : "Check for updates", desc.c_str(), !busy)) {
            if (ids.size() > 1) g_up_queue = ids;
            else check_updates();
        }
    }
    ui::section(L, "Installed mods");
    if (g.list.empty()) ui::info(L, "None", "Install a mod file, or drop one on this window", ui::kMuted);
    for (int i = 0; i < int(g.list.size()); ++i) mod_row(L, i);
    ui::section(L, "Options");
    if (ui::toggle(L, "Use mods", &g.use, "Off runs the game exactly as shipped, whatever is installed or turned on."))
        apply(mods::prefs_of(g.list, g.use));
    if (ui::button(L, "Open mods folder", "A mod is a folder (or a .zip of one) with a mod.ini; one dropped in there shows here "
                                           "after a Rescan, turned off.")) {
        std::error_code ec;
        fs::create_directories(mods_dir(), ec);
        open_path(mods_dir());
    }
    if (ui::button(L, "Rescan", "Look for mods added or removed in the mods folder.")) {
        reload_mods();
        say(std::to_string(g.list.size()) + " mod(s) found");
    }
}

// ---- the Game tab ------------------------------------------------------------------------------------
void game_page(ui::List& L) {
    ui::section(L, "Ys Origin");
    ui::info(L, "Game folder", g.game.empty() ? "Not found" : g.game.string().c_str(), g.game.empty() ? ui::kWarn : ui::kValue,
             g.game.empty() ? "Steam does not list Ys Origin. Choose its yso_win.exe below." : g.game.string().c_str());
    if (ui::button(L, "Choose the game...", "Pick the game's yso_win.exe, if it is not the one found.")) {
        const fs::path exe = pick_file(GetActiveWindow(), L"Ys Origin's yso_win.exe", L"yso_win.exe\0yso_win.exe\0");
        if (!exe.empty()) set_game(exe.parent_path(), true);
    }
    if (g.game.empty()) return;
    ui::info(L, "Version", g.known_build ? "Supported" : "Not supported", g.known_build ? ui::kGood : ui::kWarn,
             g.known_build ? "The Steam build of 2024-01-08, the one the loader knows."
                           : "The loader knows one build of the game, the Steam one of 2024-01-08. On this one it loads and "
                             "does nothing: the game runs without mods.");
    ui::section(L, "Loader");
    const char* state = g.loader == Loader::Ours ? "Installed" : g.loader == Loader::OtherVersion ? "Another version is installed"
                      : g.loader == Loader::Foreign ? "Another dinput8.dll is there" : "Not installed";
    const std::string shown = g.loader == Loader::Ours ? std::string("Installed  \xE2\x80\xA2  ") + ore::kVersion : std::string(state);
    ui::info(L, "CleriaOre", shown.c_str(), g.loader == Loader::Ours ? ui::kGood : ui::kWarn,
             "The loader is one file in the game folder, dinput8.dll: the game loads it at its start, and it loads the mods.");
    if (ui::button(L, g.loader == Loader::OtherVersion ? "Update the loader" : "Install the loader",
                   g.loader == Loader::Foreign ? "Another mod's dinput8.dll is in the game folder. It is kept beside it "
                                                 "(dinput8.dll.before-cleriaore) and put back when you remove the loader."
                                               : "Puts the loader (dinput8.dll) into the game folder, with this program beside the mods and a shortcut "
                                                 "to it in the Start menu.",
                   g.loader != Loader::Ours))
        install_loader();
    if (ui::button(L, "Remove the loader", "Takes dinput8.dll out of the game folder and the shortcut out of the Start menu: the game "
                                            "runs as shipped. The mods and their settings stay in cleriaore/.",
                   g.loader == Loader::Ours || g.loader == Loader::OtherVersion))
        remove_loader();
    {
        std::optional<Update> self;
        bool busy;
        std::string status;
        {
            std::lock_guard lk(g_up.mu);
            self = g_up.self;
            busy = g_up.busy;
            status = g_up.status;
        }
        if (self) {
            const std::string label = busy ? status : "Download CleriaOre " + self->version;
            if (ui::button(L, label.c_str(), ("A newer CleriaOre is out (" + self->version + ", this one is " + ore::kVersion +
                                              "). It is downloaded and started; then press Update the loader in it.").c_str(), !busy))
                download_self();
        }
    }
    if (g.loader == Loader::Ours && !g.testing) {
        const bool ours = opens_mod_files();
        ui::info(L, "Mod files", ours ? "Open with CleriaOre" : "Not opened by CleriaOre", ours ? ui::kGood : ui::kMuted,
                 "A double-click on a downloaded .cleriamod installs it and turns it on.");
        if (!ours && ui::button(L, "Open mod files with CleriaOre", "Makes a double-click on a .cleriamod file install it with CleriaOre."))
            say(associate(true) ? "A double-click on a .cleriamod file now installs it" : "The file association could not be written", !opens_mod_files());
    }
    ui::section(L, "Play");
    if (ui::button(L, "Start the game", "Through Steam.", !g.running)) ShellExecuteW(nullptr, L"open", kSteamRun, nullptr, nullptr, SW_SHOWNORMAL);
    if (ui::button(L, "Open the game folder")) open_path(g.game);
    if (ui::button(L, "Open the log", "cleriaore.log: what the loader and the mods wrote at the game's last start.")) open_path(ore_dir() / "cleriaore.log");
    if (!g.msg.empty()) ui::info(L, "", g.msg.c_str(), g.msg_bad ? ui::kWarn : ui::kGood, g.msg.c_str());
}

// ---- the window --------------------------------------------------------------------------------------
IDirect3D9* g_d3d = nullptr;
IDirect3DDevice9* g_dev = nullptr;
D3DPRESENT_PARAMETERS g_pp{};
bool g_lost = false;

// Tests (CLERIAORE_GAME set): a file <game>/cleriaore/manager.shot asks for the frame drawn, written beside it as
// manager.bmp (the window's picture cannot be had from outside: Direct3D draws it).
void test_shot() {
    std::error_code ec;
    const fs::path ask = ore_dir() / "manager.shot";
    if (!g.testing || !fs::exists(ask, ec)) return;
    IDirect3DSurface9 *back = nullptr, *copy = nullptr;
    D3DSURFACE_DESC d{};
    if (g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back) != D3D_OK) return;
    back->GetDesc(&d);
    D3DLOCKED_RECT lr{};
    if (g_dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr) == D3D_OK &&
        g_dev->GetRenderTargetData(back, copy) == D3D_OK && copy->LockRect(&lr, nullptr, D3DLOCK_READONLY) == D3D_OK) {
        BITMAPFILEHEADER fh{0x4D42, 0, 0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
        BITMAPINFOHEADER ih{sizeof ih, LONG(d.Width), -LONG(d.Height), 1, 32, BI_RGB};
        fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
        std::ofstream f(ore_dir() / "manager.bmp", std::ios::binary);
        f.write(reinterpret_cast<const char*>(&fh), sizeof fh).write(reinterpret_cast<const char*>(&ih), sizeof ih);
        for (UINT y = 0; y < d.Height; ++y) f.write(static_cast<const char*>(lr.pBits) + y * UINT(lr.Pitch), std::streamsize(d.Width) * 4);
        copy->UnlockRect();
        fs::remove(ask, ec);
    }
    if (copy) copy->Release();
    back->Release();
}

void reset_device() {
    ImGui_ImplDX9_InvalidateDeviceObjects();
    if (g_dev->Reset(&g_pp) == D3D_OK) g_lost = false;
    ImGui_ImplDX9_CreateDeviceObjects();
}

LRESULT CALLBACK wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(wnd, msg, wp, lp)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (g_dev && wp != SIZE_MINIMIZED) {
                g_pp.BackBufferWidth = LOWORD(lp);
                g_pp.BackBufferHeight = HIWORD(lp);
                reset_device();
            }
            return 0;
        case WM_DROPFILES: {
            const HDROP drop = reinterpret_cast<HDROP>(wp);
            wchar_t file[1024];
            for (UINT i = 0, n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0); i < n; ++i)
                if (DragQueryFileW(drop, i, file, UINT(std::size(file)))) g.dropped.push_back(file);
            DragFinish(drop);
            return 0;
        }
        case WM_COPYDATA: {   // a file another CleriaOre.exe was started with (a double-click on a .cleriamod)
            const auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(lp);
            if (cd->dwData == 1 && cd->cbData >= sizeof(wchar_t))
                g.dropped.push_back(std::wstring(static_cast<const wchar_t*>(cd->lpData), cd->cbData / sizeof(wchar_t) - 1));
            return TRUE;
        }
        case WM_ACTIVATEAPP:
            if (wp) g.refresh = true;   // a mod copied into the folder meanwhile shows at once
            break;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {700, 500};
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

// One frame: the panel, as large as the window lets it be.
bool frame() {
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    const ImGuiIO& io = ImGui::GetIO();
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    ui::S = (std::min)(w / (ui::kPanelW + 40.0f), h / (ui::kPanelH + 40.0f));   // the panel, and 20 around it
    for (const fs::path& f : g.dropped) install_mod(f);
    g.dropped.clear();
    take_updates();
    static int age = 0;
    if (age++ % 60 == 0) {
        const bool was = g.running;
        g.running = game_runs();
        if (was && !g.running) g.refresh = true;   // what the game's F1 panel changed is read again
    }
    if (g.refresh) {
        g.refresh = false;
        look_at_game();
        reload_mods();
    }
    const ui::Nav nav = ui::read_nav();
    g_panel.open = true;
    const bool open = g_panel.draw(ImGui::GetBackgroundDrawList(), w, h, nav);
    ImGui::EndFrame();
    return open || nav.back;   // Esc on a page does not quit; the cross does
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g.self = exe;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // the shortcut
    find_game();
    int argc = 0;
    if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc))   // CleriaOre.exe <a mod file>: install it
        for (int i = 1; i < argc; ++i) g.dropped.push_back(argv[i]);
    if (const HWND other = FindWindowW(L"CleriaOre", nullptr)) {   // one window: the files go to it
        for (const fs::path& f : g.dropped) {
            const std::wstring w = fs::absolute(f).wstring();
            COPYDATASTRUCT cd{1, DWORD((w.size() + 1) * sizeof(wchar_t)), const_cast<wchar_t*>(w.c_str())};
            SendMessageW(other, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd));
        }
        if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
        SetForegroundWindow(other);
        return 0;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
    wc.hIcon = wc.hIconSm = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"CleriaOre";
    RegisterClassExW(&wc);
    RECT r{0, 0, 1175, 825};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    const HWND wnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"CleriaOre", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                     r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    g_pp.Windowed = TRUE;
    g_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_pp.BackBufferFormat = D3DFMT_UNKNOWN;
    g_pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (!wnd || !g_d3d || g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_pp, &g_dev) != D3D_OK) {
        MessageBoxW(nullptr, L"CleriaOre could not open its window (Direct3D 9).", L"CleriaOre", MB_ICONERROR);
        return 1;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    {   // the fonts it carries: laid in the temp folder for the kit to load
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        const fs::path fonts = fs::path(tmp) / "CleriaOre" / "fonts";
        write_fonts(fonts);
        ui::load_fonts(fonts);
    }
    ImGui_ImplWin32_Init(wnd);
    ImGui_ImplDX9_Init(g_dev);
    ui::Page& mods = g_panel.add("Mods", &mods_page);
    mods.back = [](ui::List&) {
        if (g.sub.empty()) return false;
        g.next = "";
        return true;
    };
    g_panel.add("Game", &game_page);
    if (g.game.empty() || g.loader != Loader::Ours) g_panel.tab = 1;   // first things first
    ShowWindow(wnd, show);
    check_updates();

    for (bool run = true; run;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) run = false;
        }
        if (!run) break;
        if (IsIconic(wnd)) { Sleep(50); continue; }
        if (g_lost) {
            if (g_dev->TestCooperativeLevel() == D3DERR_DEVICENOTRESET) reset_device();
            if (g_lost) { Sleep(50); continue; }
        }
        if (!frame()) run = false;
        g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(8, 11, 20), 1.0f, 0);
        if (g_dev->BeginScene() == D3D_OK) {
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_dev->EndScene();
        }
        test_shot();
        if (g_dev->Present(nullptr, nullptr, nullptr, nullptr) == D3DERR_DEVICELOST) g_lost = true;
    }
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_dev->Release();
    g_d3d->Release();
    DestroyWindow(wnd);
    if (!g_launch_after.empty()) ShellExecuteW(nullptr, L"open", g_launch_after.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return 0;
}
