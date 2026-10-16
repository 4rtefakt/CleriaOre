#include "ui.hpp"

#include <windows.h>
#include <d3d9.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <share.h>
#include <map>
#include <vector>

#include "MinHook.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "ore.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using namespace cleria;

namespace ore {

namespace {

using EndSceneFn = HRESULT(WINAPI*)(IDirect3DDevice9*);
using ResetFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
EndSceneFn g_end_scene = nullptr;
ResetFn g_reset = nullptr;
WNDPROC g_wndproc = nullptr;
bool g_attached = false, g_ready = false;

ui::Panel g_panel;
std::string g_sub;                    // the mod whose page the Mods tab shows ("" = the list)
bool g_changed = false;               // a mod was turned on or off: it applies at the next start
DWORD g_closed_at = 0;                // when the panel closed: the keyboard is held a moment longer
bool g_modal = false;                 // a mod's own window is open: it has the input
// A line typed into a menu of the game's (ui_prompt): the keys go here, not to the game; the row shows the text.
bool g_prompting = false, g_prompt_secret = false;
int g_prompt_done = 0;                // 1 kept (Enter), 2 dropped (Esc): told to the mod at the frame's end
std::string g_prompt_text;
bool g_prompt_dirty = false;          // the line changed: shown once, at the frame's end (the menu is made anew for it)
void prompt_show() { g_prompt_dirty = true; }
void prompt_key(unsigned ch) {        // a character as WM_CHAR gives it
    if (ch == 13) g_prompt_done = 1;
    else if (ch == 27) g_prompt_done = 2;
    else if (ch == 8) { if (!g_prompt_text.empty()) g_prompt_text.pop_back(); }
    else if (ch == 0x16) {            // Ctrl+V: the clipboard's first line
        if (OpenClipboard(nullptr)) {
            if (HANDLE h = GetClipboardData(CF_TEXT))
                if (const char* s = static_cast<const char*>(GlobalLock(h))) {
                    for (; *s && *s != '\r' && *s != '\n' && g_prompt_text.size() < 120; ++s)
                        if (static_cast<unsigned char>(*s) >= 32) g_prompt_text.push_back(*s);
                    GlobalUnlock(h);
                }
            CloseClipboard();
        }
    } else if (ch >= 32 && ch < 256 && ch != 127 && ch != '|' && g_prompt_text.size() < 120) g_prompt_text.push_back(char(ch));
    if (g_prompt_done) g_prompting = false;
    else prompt_show();
}
bool g_modal_pause = false;           // ... and holds the game

struct Notice { std::string head, text; double t = -100; } g_notice;
std::map<int, int> g_item_icons;                       // item id -> image handle
struct { int image = 0; double t = -100; } g_icon;     // the icon shown beside the card

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

script::Mod* script_of(const std::string& id) {
    for (auto& m : scripts())
        if (m->id() == id) return m.get();
    return nullptr;
}

// ---- the borderless window ----
// The game has windowed and exclusive fullscreen, no borderless: in windowed mode the frame is dropped and the
// window covers its monitor, which with the game's resolution at the monitor's is borderless fullscreen
// ([cleriaore] Borderless, the retail Archipelago client's borderless=1). Exclusive fullscreen is left alone.
IDirect3DDevice9* g_dev = nullptr;
HWND g_wnd = nullptr;
bool g_borderless = false;            // the window is made borderless now
LONG g_style0 = 0;                    // ... and what it was before
RECT g_rect0{};

bool windowed(IDirect3DDevice9* dev) {
    IDirect3DSwapChain9* sc = nullptr;
    if (!dev || FAILED(dev->GetSwapChain(0, &sc)) || !sc) return false;
    D3DPRESENT_PARAMETERS pp{};
    const HRESULT hr = sc->GetPresentParameters(&pp);
    sc->Release();
    return SUCCEEDED(hr) && pp.Windowed;
}

void set_borderless(bool on) {
    if (!g_wnd) return;
    if (on) {
        if (!windowed(g_dev)) return;
        MONITORINFO mi{};
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
        if (!g_borderless) {
            g_style0 = GetWindowLongW(g_wnd, GWL_STYLE);
            GetWindowRect(g_wnd, &g_rect0);
        }
        const LONG style = g_style0 & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
        SetWindowLongW(g_wnd, GWL_STYLE, style | WS_POPUP);
        const RECT& m = mi.rcMonitor;
        SetWindowPos(g_wnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        if (!g_borderless) log("[ui] borderless: the window covers its monitor, %ldx%ld", m.right - m.left, m.bottom - m.top);
        g_borderless = true;
    } else if (g_borderless) {
        SetWindowLongW(g_wnd, GWL_STYLE, g_style0);
        SetWindowPos(g_wnd, nullptr, g_rect0.left, g_rect0.top, g_rect0.right - g_rect0.left, g_rect0.bottom - g_rect0.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOZORDER | SWP_SHOWWINDOW);
        g_borderless = false;
    }
}

// The Mods tab: a row a mod (its place in the load order, its name, its state), and one mod's page: its own
// settings, On / Off and its place in the load order, as the manager shows them. Changes are kept for the next start.
std::vector<std::string> g_next_order;   // the load order for the next start, as the panel changed it

mods::Mod* mod_by_id(const std::string& id) {
    for (mods::Mod& m : mod_list())
        if (m.m.id == id) return &m;
    return nullptr;
}
void order_init() {
    if (g_next_order.size() == mod_list().size()) return;
    g_next_order.clear();
    for (const mods::Mod& m : mod_list()) g_next_order.push_back(m.m.id);
}
// Left / Right on a mod: one place up / down. False when it is at that end already.
bool order_move(std::size_t i, int d) {
    const std::size_t j = i + std::size_t(d);
    if (d == 0 || j >= g_next_order.size()) return false;
    std::swap(g_next_order[i], g_next_order[j]);
    g_changed = true;
    save_order(g_next_order);
    return true;
}
// On turns on what it requires too (as the manager does).
void set_enabled(mods::Mod& m, bool on) {
    m.enabled = on;
    if (on)
        for (const mods::Dep& d : m.m.requires_)
            if (mods::Mod* o = mod_by_id(d.id); o && !o->enabled) set_enabled(*o, true);
    g_changed = true;
    save_prefs();
}
// What it does now, and what it will do after a restart.
const char* state_of(const mods::Mod& m, ImU32* col) {
    const script::Mod* s = script_of(m.m.id);
    const bool running = m.state == mods::State::Active;
    *col = ui::kWarn;
    if (m.state == mods::State::Error) return "Error";
    if (s && !s->ok()) return "Stopped";
    if (m.enabled != running) return m.enabled ? "On after a restart" : "Off after a restart";
    *col = running ? ui::kGood : ui::kMuted;
    return running ? "On" : "Off";
}
std::string about(const mods::Mod& m) {
    std::string d = m.label() + "  " + m.m.version + (m.m.author.empty() ? "" : "\nby " + m.m.author);
    if (!m.m.description.empty()) d += "\n\n" + m.m.description;
    for (const std::string& e : m.errors) d += "\n\nError: " + e;
    return d;
}

// The mods are read at the game's start: once they changed, a button restarts it, from the title only (nothing
// unsaved is lost there). The game quits, and a hidden cmd.exe starts the same yso_win.exe two seconds later.
void restart_game() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & start \"\" \"" + std::wstring(exe) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    const std::wstring dir = fs::path(exe).parent_path().wstring();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
        log("[ui] restart: cmd.exe could not be started (%lu)", GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    log("[ui] restart: the game quits and starts again");
    ExitProcess(0);
}
void restart_row(ui::List& L) {
    const bool title = !host().in_game();
    if (ui::button(L, "Restart the game to apply the changes",
                   title ? "Quits the game and starts it again, with the mods as you set them."
                         : "The mods are read when the game starts. Save, quit to the title, then restart from here.",
                   title))
        restart_game();
}

void mod_page(ui::List& L) {
    mods::Mod* m = mod_by_id(g_sub);
    if (ui::back_row(L, "Mods", m ? m->label().c_str() : g_sub.c_str(), m ? about(*m).c_str() : nullptr) || !m) {
        g_sub.clear();
        return;
    }
    if (script::Mod* s = script_of(m->m.id)) {
        if (!s->ok()) {
            ui::info(L, "Settings", "Stopped by an error", ui::kWarn,
                     ("This mod's script failed and was turned off until the game restarts.\n\n" + s->error()).c_str());
        } else {
            const std::vector<std::string> pages = s->pages();
            for (std::size_t i = 0; i < pages.size(); ++i) {
                if (pages.size() > 1) ui::section(L, pages[i].c_str());
                s->draw_page(i, &L);
            }
        }
    }
    ui::section(L, "Mod");
    bool on = m->enabled;
    if (ui::toggle(L, "Enabled", &on, "Turns the mod on or off, with the mods it needs. Applies when the game restarts.")) set_enabled(*m, on);
    ImU32 col;
    const char* st = state_of(*m, &col);
    ui::info(L, "State", st, col, m->errors.empty() ? nullptr : about(*m).c_str());
    order_init();
    const std::size_t i = std::size_t(std::find(g_next_order.begin(), g_next_order.end(), m->m.id) - g_next_order.begin());
    char pos[32];
    std::snprintf(pos, sizeof pos, "%d of %d", int(i) + 1, int(g_next_order.size()));
    const ui::RowIo r = ui::row_frame(L, "Its place in the load order. Left / Right move it up / down: when two mods change the same "
                                         "file, the one lower in the list wins. Applies when the game restarts.", true);
    ui::row_texts(L, r, "Load order", pos, true);
    order_move(i, r.delta);
}

void mods_page(ui::List& L) {
    static std::string shown;   // the page drawn last frame: another one starts at its top
    if (shown != g_sub) {
        shown = g_sub;
        L.sel = 0;
        L.scroll = L.scroll_to = 0;
    }
    if (!g_sub.empty()) return mod_page(L);
    ui::section(L, "Mods");
    if (!use_mods_now()) {   // the game started without mods: nothing was loaded, so there is no list to show
        static bool on = false;
        ui::info(L, "Mods are off", "", ui::kWarn, "Use mods is off: the game started as shipped.");
        if (ui::toggle(L, "Use mods", &on, "Turns the mods back on at the game's next start, with the choices you had."))
            set_use_mods(on);
        if (on) restart_row(L);
    } else if (mod_list().empty())
        ui::info(L, "No mod installed", "", ui::kMuted, ("Put a mod's folder, .zip or .cleriamod in:\n\n" + (dir() / "mods").string()).c_str());
    if (g_changed) restart_row(L);
    order_init();
    for (std::size_t i = 0; i < g_next_order.size(); ++i) {
        mods::Mod* m = mod_by_id(g_next_order[i]);
        if (!m) continue;
        const std::string desc = about(*m) + "\n\nOK opens its page: its settings, On / Off. Left / Right move it in the load order: "
                                             "when two mods change the same file, the one lower in the list wins.";
        const ui::RowIo r = ui::row_frame(L, desc.c_str(), true);
        ImU32 col;
        const char* st = state_of(*m, &col);
        char label[160];
        std::snprintf(label, sizeof label, "%d   %s", int(i) + 1, m->label().c_str());
        ui::row_texts(L, r, label, "", true);
        const float px = 16.5f * ui::S, cy = (r.a.y + r.b.y) * 0.5f;
        ImFont* f = r.selected ? ui::F.medium : ui::F.body;
        ui::text(L.dl, f, px, {r.b.x - r.pad - ui::text_size(f, px, st).x, cy - px * 0.55f}, col, st);
        if (r.activate) {
            g_sub = m->m.id;
            break;
        }
        if (order_move(i, r.delta)) {
            L.sel += r.delta;   // the selection follows the mod
            break;
        }
    }
    ui::section(L, "Window");
    bool borderless = loader_option("Borderless", false);
    if (ui::toggle(L, "Borderless window", &borderless,
                   "In the game's windowed mode, no frame and the whole monitor: with the game's resolution set to the "
                   "monitor's, this is borderless fullscreen. Not in the game's own fullscreen.")) {
        set_loader_option("Borderless", borderless);
        set_borderless(borderless);
    }
}

void init(IDirect3DDevice9* dev);

// ---- a mod's images ----
struct Image {
    fs::path file;
    IDirect3DTexture9* tex = nullptr;
    int w = 0, h = 0;
    bool tried = false;
};
std::vector<Image> g_images;
IDirect3DDevice9* g_device = nullptr;

// A DDS file's first level into a managed texture: the block formats as they are, 32-bit BGRA row by row.
void make_texture(Image& im) {
    im.tried = true;
    std::FILE* f = _wfsopen(im.file.c_str(), L"rb", _SH_DENYNO);
    if (!f) return;
    std::vector<unsigned char> d;
    unsigned char buf[65536];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    if (d.size() < 128 || std::memcmp(d.data(), "DDS ", 4) != 0) return;
    auto u32 = [&](std::size_t at) { std::uint32_t v; std::memcpy(&v, d.data() + at, 4); return v; };
    const std::uint32_t h = u32(12), w = u32(16), pf_flags = u32(80), fourcc = u32(84), bits = u32(88);
    if (!w || !h || w > 4096 || h > 4096) return;
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
    std::size_t row = 0, rows = 0;   // bytes a row of the data, and how many
    if (pf_flags & 4) {
        fmt = fourcc == MAKEFOURCC('D', 'X', 'T', '1') ? D3DFMT_DXT1 : fourcc == MAKEFOURCC('D', 'X', 'T', '3') ? D3DFMT_DXT3
            : fourcc == MAKEFOURCC('D', 'X', 'T', '5') ? D3DFMT_DXT5 : D3DFMT_UNKNOWN;
        row = std::size_t((w + 3) / 4) * (fmt == D3DFMT_DXT1 ? 8 : 16);
        rows = (h + 3) / 4;
    } else if (bits == 32) {
        fmt = D3DFMT_A8R8G8B8;
        row = std::size_t(w) * 4;
        rows = h;
    }
    if (fmt == D3DFMT_UNKNOWN || d.size() < 128 + row * rows) return;
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(g_device->CreateTexture(w, h, 1, 0, fmt, D3DPOOL_MANAGED, &tex, nullptr))) return;
    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, nullptr, 0))) { tex->Release(); return; }
    for (std::size_t y = 0; y < rows; ++y)
        std::memcpy(static_cast<unsigned char*>(lr.pBits) + y * std::size_t(lr.Pitch), d.data() + 128 + y * row, row);
    tex->UnlockRect(0);
    im.tex = tex;
    im.w = int(w);
    im.h = int(h);
}

// ---- the drive: the game steered from outside, for tests (only with CLERIAORE_DRIVE set when the game starts) ----
// Each frame cleriaore/drive/keys.txt is read: the DirectInput keys held (hex DIK codes, "D0 1C"), fed to the
// game whether its window has the focus or not; and cleriaore/drive/cmd.txt: one cleria.command a line
// ("name args"), run and the file removed.
const bool g_drive = std::getenv("CLERIAORE_DRIVE") != nullptr;
unsigned char g_drive_keys[256] = {};
bool g_drive_mouse[4] = {};   // codes 100..103: the mouse buttons
bool g_panel_asked = false;  // the panel is to open (ui_open_panel)
std::string g_drive_shot;     // "shot <name>": the frame being drawn is written to drive/<name>.bmp
// The frame as the device holds it (so whatever covers the game's window on the desktop does not matter).
void drive_shot(IDirect3DDevice9* dev) {
    const std::string name = std::exchange(g_drive_shot, std::string());
    IDirect3DSurface9 *rt = nullptr, *flat = nullptr, *mem = nullptr;
    D3DSURFACE_DESC d{};
    D3DLOCKED_RECT lr{};
    bool ok = false;
    if (SUCCEEDED(dev->GetRenderTarget(0, &rt)) && SUCCEEDED(rt->GetDesc(&d)) && (d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8)) {
        IDirect3DSurface9* from = rt;
        if (d.MultiSampleType != D3DMULTISAMPLE_NONE &&     // a multisampled target is resolved first
            SUCCEEDED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &flat, nullptr)) &&
            SUCCEEDED(dev->StretchRect(rt, nullptr, flat, nullptr, D3DTEXF_NONE)))
            from = flat;
        if (SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &mem, nullptr)) &&
            SUCCEEDED(dev->GetRenderTargetData(from, mem)) && SUCCEEDED(mem->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            if (std::FILE* f = _wfsopen((dir() / "drive" / (name + ".bmp")).c_str(), L"wb", _SH_DENYWR)) {
                BITMAPFILEHEADER fh{};
                BITMAPINFOHEADER ih{};
                fh.bfType = 0x4d42;
                fh.bfOffBits = sizeof fh + sizeof ih;
                fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
                ih.biSize = sizeof ih;
                ih.biWidth = LONG(d.Width);
                ih.biHeight = -LONG(d.Height);
                ih.biPlanes = 1;
                ih.biBitCount = 32;
                std::fwrite(&fh, sizeof fh, 1, f);
                std::fwrite(&ih, sizeof ih, 1, f);
                for (UINT y = 0; y < d.Height; ++y) std::fwrite(static_cast<const char*>(lr.pBits) + y * lr.Pitch, 4, d.Width, f);
                std::fclose(f);
                ok = true;
            }
            mem->UnlockRect();
        }
    }
    if (mem) mem->Release();
    if (flat) flat->Release();
    if (rt) rt->Release();
    log("[drive] shot %s%s", name.c_str(), ok ? "" : ": failed");
}
void drive_poll() {
    if (!g_drive) return;
    std::memset(g_drive_keys, 0, sizeof g_drive_keys);
    std::memset(g_drive_mouse, 0, sizeof g_drive_mouse);
    if (std::FILE* f = _wfsopen((dir() / "drive" / "keys.txt").c_str(), L"r", _SH_DENYNO)) {
        unsigned k = 0;
        while (std::fscanf(f, "%x", &k) == 1)
            if (k < 256) g_drive_keys[k] = 0x80;
            else if (k < 0x104) g_drive_mouse[k - 0x100] = true;
        std::fclose(f);
    }
    const fs::path cmd = dir() / "drive" / "cmd.txt";
    if (std::FILE* f = _wfsopen(cmd.c_str(), L"r", _SH_DENYNO)) {
        std::vector<std::string> lines;
        char line[1024];
        while (std::fgets(line, sizeof line, f)) lines.emplace_back(line);
        std::fclose(f);
        std::error_code ec;
        if (!fs::remove(cmd, ec)) return;   // still being written: next frame
        for (std::string& l : lines) {
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            const std::size_t sp = l.find(' ');
            if (l.empty()) continue;
            if (l == "panel") { g_panel.open = !g_panel.open; continue; }
            if (l.rfind("type ", 0) == 0 && g_prompting) { for (const char c : l.substr(5)) prompt_key(static_cast<unsigned char>(c)); continue; }   // into a prompt
            if (g_prompting && (l == "nav ok" || l == "nav back")) { prompt_key(l == "nav ok" ? 13 : 27); continue; }            if (l.rfind("nav ", 0) == 0) {                                    // a key for the panel or a mod's window: up down left right ok back
                static const std::pair<const char*, ImGuiKey> kNav[] = {{"up", ImGuiKey_UpArrow}, {"down", ImGuiKey_DownArrow}, {"left", ImGuiKey_LeftArrow},
                                                                        {"right", ImGuiKey_RightArrow}, {"ok", ImGuiKey_Enter}, {"back", ImGuiKey_Escape}};
                for (const auto& [word, key] : kNav)
                    if (l.substr(4) == word) { ImGui::GetIO().AddKeyEvent(key, true); ImGui::GetIO().AddKeyEvent(key, false); }
                continue;
            }
            if (l.rfind("shot ", 0) == 0) { g_drive_shot = l.substr(5); continue; }   // this frame, as drive/<name>.bmp
            if (l.rfind("profile", 0) == 0) { set_save_profile(l.size() > 8 ? l.substr(8) : std::string()); profile_storage(); continue; }   // for tests: at any time
            if (l.rfind("steamrm ", 0) == 0) { log("[drive] %s: %s", l.c_str(), steam_delete(l.c_str() + 8) ? "deleted" : "no"); continue; }
            if (l == "rawactors") { host().game_op("drive", "actors_raw", {}, {}, nullptr); continue; }   // the live list, to the log
            // For study: "peek <addr> [n]" logs n dwords, "poke <addr> <value>" writes one; hexadecimal, and "p+off"
            // for an offset into the player's actor.
            if (l.rfind("peek ", 0) == 0 || l.rfind("poke ", 0) == 0) {
                char where[64] = "";
                unsigned second = 0;
                const int got = std::sscanf(l.c_str() + 5, "%63s %x", where, &second);
                std::uintptr_t base = 0;
                const char* num = where;
                if (where[0] == 'p' && where[1] == '+') { base = *reinterpret_cast<std::uintptr_t*>(0x74c09c); num = where + 2; }
                const std::uintptr_t addr = base + std::strtoul(num, nullptr, 16);
                if (got < 1 || addr < 0x10000 || (where[0] == 'p' && !base)) { log("[drive] %s: no address", l.c_str()); continue; }
                if (l[1] == 'o') {
                    if (got == 2) *reinterpret_cast<std::uint32_t*>(addr) = second;
                    log("[drive] poke %08x = %08x", unsigned(addr), second);
                } else {
                    std::string out;
                    char t[40];
                    for (unsigned k = 0; k < (got == 2 && second ? (std::min)(second, 64u) : 1u); ++k) {
                        const std::uint32_t v = reinterpret_cast<std::uint32_t*>(addr)[k];
                        float f;
                        std::memcpy(&f, &v, 4);
                        std::snprintf(t, sizeof t, " %08x(%g)", v, double(f));
                        out += t;
                    }
                    log("[drive] peek %s (%08x):%s", where, unsigned(addr), out.c_str());
                }
                continue;
            }
            const bool ok = host().run_command(l.substr(0, sp), sp == std::string::npos ? std::string() : l.substr(sp + 1));
            log("[drive] %s%s", l.c_str(), ok ? "" : ": no such command");
        }
    }
}

// ---- keys by SDL's names (cleria.input) ----
struct KeyName { ImGuiKey key; const char* name; };
const KeyName kKeys[] = {
    {ImGuiKey_A, "A"}, {ImGuiKey_B, "B"}, {ImGuiKey_C, "C"}, {ImGuiKey_D, "D"}, {ImGuiKey_E, "E"}, {ImGuiKey_F, "F"}, {ImGuiKey_G, "G"},
    {ImGuiKey_H, "H"}, {ImGuiKey_I, "I"}, {ImGuiKey_J, "J"}, {ImGuiKey_K, "K"}, {ImGuiKey_L, "L"}, {ImGuiKey_M, "M"}, {ImGuiKey_N, "N"},
    {ImGuiKey_O, "O"}, {ImGuiKey_P, "P"}, {ImGuiKey_Q, "Q"}, {ImGuiKey_R, "R"}, {ImGuiKey_S, "S"}, {ImGuiKey_T, "T"}, {ImGuiKey_U, "U"},
    {ImGuiKey_V, "V"}, {ImGuiKey_W, "W"}, {ImGuiKey_X, "X"}, {ImGuiKey_Y, "Y"}, {ImGuiKey_Z, "Z"},
    {ImGuiKey_0, "0"}, {ImGuiKey_1, "1"}, {ImGuiKey_2, "2"}, {ImGuiKey_3, "3"}, {ImGuiKey_4, "4"}, {ImGuiKey_5, "5"}, {ImGuiKey_6, "6"},
    {ImGuiKey_7, "7"}, {ImGuiKey_8, "8"}, {ImGuiKey_9, "9"},
    {ImGuiKey_F1, "F1"}, {ImGuiKey_F2, "F2"}, {ImGuiKey_F3, "F3"}, {ImGuiKey_F4, "F4"}, {ImGuiKey_F5, "F5"}, {ImGuiKey_F6, "F6"},
    {ImGuiKey_F7, "F7"}, {ImGuiKey_F8, "F8"}, {ImGuiKey_F9, "F9"}, {ImGuiKey_F10, "F10"}, {ImGuiKey_F11, "F11"}, {ImGuiKey_F12, "F12"},
    {ImGuiKey_UpArrow, "Up"}, {ImGuiKey_DownArrow, "Down"}, {ImGuiKey_LeftArrow, "Left"}, {ImGuiKey_RightArrow, "Right"},
    {ImGuiKey_Space, "Space"}, {ImGuiKey_Enter, "Return"}, {ImGuiKey_Escape, "Escape"}, {ImGuiKey_Tab, "Tab"}, {ImGuiKey_Backspace, "Backspace"},
    {ImGuiKey_Insert, "Insert"}, {ImGuiKey_Delete, "Delete"}, {ImGuiKey_Home, "Home"}, {ImGuiKey_End, "End"},
    {ImGuiKey_PageUp, "PageUp"}, {ImGuiKey_PageDown, "PageDown"},
    {ImGuiKey_LeftShift, "Left Shift"}, {ImGuiKey_RightShift, "Right Shift"}, {ImGuiKey_LeftCtrl, "Left Ctrl"}, {ImGuiKey_RightCtrl, "Right Ctrl"},
    {ImGuiKey_LeftAlt, "Left Alt"}, {ImGuiKey_RightAlt, "Right Alt"},
    {ImGuiKey_Minus, "-"}, {ImGuiKey_Equal, "="}, {ImGuiKey_LeftBracket, "["}, {ImGuiKey_RightBracket, "]"}, {ImGuiKey_Semicolon, ";"},
    {ImGuiKey_Apostrophe, "'"}, {ImGuiKey_Comma, ","}, {ImGuiKey_Period, "."}, {ImGuiKey_Slash, "/"}, {ImGuiKey_Backslash, "\\"},
    {ImGuiKey_GraveAccent, "`"},
    {ImGuiKey_Keypad0, "Keypad 0"}, {ImGuiKey_Keypad1, "Keypad 1"}, {ImGuiKey_Keypad2, "Keypad 2"}, {ImGuiKey_Keypad3, "Keypad 3"},
    {ImGuiKey_Keypad4, "Keypad 4"}, {ImGuiKey_Keypad5, "Keypad 5"}, {ImGuiKey_Keypad6, "Keypad 6"}, {ImGuiKey_Keypad7, "Keypad 7"},
    {ImGuiKey_Keypad8, "Keypad 8"}, {ImGuiKey_Keypad9, "Keypad 9"}, {ImGuiKey_KeypadEnter, "Keypad Enter"},
};
const KeyName kPad[] = {
    {ImGuiKey_GamepadFaceDown, "a"}, {ImGuiKey_GamepadFaceRight, "b"}, {ImGuiKey_GamepadFaceLeft, "x"}, {ImGuiKey_GamepadFaceUp, "y"},
    {ImGuiKey_GamepadBack, "back"}, {ImGuiKey_GamepadStart, "start"}, {ImGuiKey_GamepadL1, "leftshoulder"}, {ImGuiKey_GamepadR1, "rightshoulder"},
    {ImGuiKey_GamepadL3, "leftstick"}, {ImGuiKey_GamepadR3, "rightstick"}, {ImGuiKey_GamepadDpadUp, "dpup"}, {ImGuiKey_GamepadDpadDown, "dpdown"},
    {ImGuiKey_GamepadDpadLeft, "dpleft"}, {ImGuiKey_GamepadDpadRight, "dpright"},
};
const KeyName* key_named(const std::string& name, bool pad) {
    const KeyName* t = pad ? kPad : kKeys;
    const std::size_t n = pad ? std::size(kPad) : std::size(kKeys);
    for (std::size_t i = 0; i < n; ++i)
        if (_stricmp(t[i].name, name.c_str()) == 0) return &t[i];
    return nullptr;
}

// ---- a mod's own window over the game (cleria.ui.window / message / choose), as CleriaCore draws it ----
std::map<std::string, ui::List> g_win_lists;
std::string g_win_shown;
void draw_window(ImDrawList* dl, float w, float h, const ui::Nav& nav) {
    script::Mod* m = window_mod();
    if (!m) return;
    const script::Mod::Window win = *m->window();
    using namespace ui;
    if (win.pause) dl->AddRectFilled({0, 0}, {w, h}, kDim);
    List& L = g_win_lists[m->id() + "/" + win.id];
    const std::string shown = m->id() + "#" + std::to_string(m->window_serial_);
    if (shown != g_win_shown) { L = List{}; g_win_shown = shown; }   // a newly opened window starts on its first row
    const float head = 46.0f * S, bw = w * win.width;     // as tall as its rows were last frame, within the screen
    const float bh = std::clamp(L.content_h > 0 ? head + L.content_h + 28.0f * S : h * 0.3f, 110.0f * S, h * 0.8f);
    const ImVec2 a{(w - bw) * 0.5f, (h - bh) * 0.5f}, b{a.x + bw, a.y + bh};
    window_frame(dl, a, b, win.title.c_str(), head);
    list_begin(L, dl, {a.x + 14.0f * S, a.y + head + 10.0f * S}, {bw - 28.0f * S, bh - head - 20.0f * S}, nav, true);
    m->draw_window(&L);
    list_end(L);
    if (nav.back && m->window()) m->close_window(true);
}

void draw_notice(ImDrawList* dl, float w, float h) {
    if (now() - g_notice.t < 5.0) ui::toast(dl, w, h, g_notice.head.c_str(), g_notice.text.c_str());
    if (now() - g_icon.t < 5.0) {                      // an item's icon: a framed square left of where the card is
        if (void* tex = ui_image(g_icon.image)) {
            const float side = 74.0f * ui::S, x = w * 0.5f - 170.0f * ui::S - side - 8.0f * ui::S, y = h - 105.0f * ui::S;
            dl->AddRectFilled({x, y}, {x + side, y + side}, IM_COL32(10, 14, 28, 240));
            dl->AddImage(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(tex)), {x + 5.0f * ui::S, y + 5.0f * ui::S}, {x + side - 5.0f * ui::S, y + side - 5.0f * ui::S});
            dl->AddRect({x, y}, {x + side, y + side}, ui::kGold, 0.0f, 0, 1.1f * ui::S);
        }
    }
}

HRESULT WINAPI hook_end_scene(IDirect3DDevice9* dev) {
    if (!g_ready) init(dev);
    g_device = dev;
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = g_panel.open;
    ImGui::NewFrame();
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    ui::S = h / 1000.0f;   // as CleriaCore's shell (view_shell.inl)

    g_hud = ImGui::GetBackgroundDrawList();
    for (auto& m : scripts())
        if (m->ok()) m->draw_hud(w, h, ui::S);
    g_hud = nullptr;
    draw_notice(ImGui::GetBackgroundDrawList(), w, h);

    drive_poll();
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) g_panel.open = !g_panel.open;
    // The title's "Mods" row asked for the panel: opened once the key that chose the row is up, so it chooses nothing here.
    if (g_panel_asked && !ImGui::IsKeyDown(ImGuiKey_Enter) && !ImGui::IsKeyDown(ImGuiKey_KeypadEnter) && !ImGui::IsKeyDown(ImGuiKey_Space) &&
        !ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown)) {
        g_panel_asked = false;
        g_panel.open = true;
    }
    const bool was_open = g_panel.open || window_mod();
    io.MouseDrawCursor = was_open;
    const ui::Nav nav = ui::read_nav();
    if (!was_open) {   // the game has the input: the mods' actions and the "input" event
        for (const KeyName& k : kKeys)
            if (k.key != ImGuiKey_F1 && ImGui::IsKeyPressed(k.key, false)) on_input(k.name, false);
        for (const KeyName& k : kPad)
            if (ImGui::IsKeyPressed(k.key, false)) on_input(k.name, true);
    }
    if (g_panel.open) g_panel.draw(ImGui::GetForegroundDrawList(), w, h, nav);
    else if (window_mod()) draw_window(ImGui::GetForegroundDrawList(), w, h, nav);
    g_modal = window_mod() != nullptr;
    g_modal_pause = g_modal && window_mod()->window()->pause;
    if (was_open && !g_panel.open && !g_modal) g_closed_at = GetTickCount();
    if (std::exchange(g_prompt_dirty, false) && g_prompting) game_prompt_text((g_prompt_secret ? std::string(g_prompt_text.size(), '*') : g_prompt_text) + "_");
    if (const int done = std::exchange(g_prompt_done, 0)) {
        g_closed_at = GetTickCount();                   // (the Enter that ended it is not the game's)
        on_prompt_done(done == 1, g_prompt_text);
    }

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    if (!g_drive_shot.empty()) drive_shot(dev);
    return g_end_scene(dev);
}

HRESULT WINAPI hook_reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (g_ready) ImGui_ImplDX9_InvalidateDeviceObjects();
    const HRESULT hr = g_reset(dev, pp);
    if (g_ready) ImGui_ImplDX9_CreateDeviceObjects();
    if (SUCCEEDED(hr) && g_wnd && loader_option("Borderless", false)) {
        if (pp && !pp->Windowed) g_borderless = false;   // the game's fullscreen took the window over
        else set_borderless(true);
    }
    return hr;
}

// ImGui reads the window's messages; while the panel is open the game's window does not get the keys
// and the mouse.
LRESULT CALLBACK hook_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_prompting && msg >= WM_KEYFIRST && msg <= WM_KEYLAST) {
        if (msg == WM_CHAR) prompt_key(unsigned(wp));
        if (msg == WM_KEYDOWN || msg == WM_KEYUP) {     // (the game's own loop may not translate them)
            MSG m{wnd, msg, wp, lp};
            TranslateMessage(&m);
        }
        return 0;
    }    ImGui_ImplWin32_WndProcHandler(wnd, msg, wp, lp);
    if ((g_panel.open || g_modal) && ((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST))) return 0;
    return CallWindowProcW(g_wndproc, wnd, msg, wp, lp);
}

void init(IDirect3DDevice9* dev) {
    D3DDEVICE_CREATION_PARAMETERS cp{};
    dev->GetCreationParameters(&cp);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // no imgui.ini beside the game
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NavEnableGamepad;
    const bool fonts = ui::load_fonts(dir() / "fonts");
    ImGui_ImplWin32_Init(cp.hFocusWindow);
    ImGui_ImplDX9_Init(dev);
    g_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(cp.hFocusWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&hook_wndproc)));
    g_dev = dev;
    g_wnd = cp.hFocusWindow;
    if (loader_option("Borderless", false)) set_borderless(true);
    ui::Page& mods = g_panel.add("Mods", &mods_page);
    mods.back = [](ui::List&) {
        if (g_sub.empty()) return false;
        g_sub.clear();
        return true;
    };
    g_ready = true;
    log("[ui] ready%s", fonts ? "" : " (no fonts in cleriaore/fonts: the built-in one is used)");
}

// ---- the game's keyboard (DirectInput) while the panel is open -------------------------------------
using CreateDeviceFn = HRESULT(WINAPI*)(void* self, const void* guid, void** device, void* outer);
using GetDeviceStateFn = HRESULT(WINAPI*)(void* self, DWORD size, void* data);
CreateDeviceFn g_create_device = nullptr;
GetDeviceStateFn g_get_device_state = nullptr;

// Only the keyboard (its state is 256 key bytes): a stick's rest is a mid value, not 0.
HRESULT WINAPI hook_get_device_state(void* self, DWORD size, void* data) {
    HRESULT hr = g_get_device_state(self, size, data);
    if (data && size == 256 && (g_panel.open || g_modal || g_prompting || GetTickCount() - g_closed_at < 250)) std::memset(data, 0, size);
    if (g_drive && data && size == 256) {   // the drive's keys, with or without the focus
        if (FAILED(hr)) { std::memset(data, 0, size); hr = S_OK; }
        for (int k = 0; k < 256; ++k)
            if (g_drive_keys[k]) static_cast<unsigned char*>(data)[k] = 0x80;
    }
    if (g_drive && data && (size == 16 || size == 20)) {   // DIMOUSESTATE: three axes, then the buttons
        if (FAILED(hr)) { std::memset(data, 0, size); hr = S_OK; }
        for (int k = 0; k < 4; ++k)
            if (g_drive_mouse[k]) static_cast<unsigned char*>(data)[12 + k] = 0x80;
    }
    return hr;
}

// Every device of one dinput8 shares a vtable: the first one hooks GetDeviceState (slot 9) for all.
HRESULT WINAPI hook_create_device(void* self, const void* guid, void** device, void* outer) {
    const HRESULT hr = g_create_device(self, guid, device, outer);
    if (SUCCEEDED(hr) && device && *device && !g_get_device_state) {
        void* fn = (*reinterpret_cast<void***>(*device))[9];
        if (MH_CreateHook(fn, reinterpret_cast<void*>(&hook_get_device_state), reinterpret_cast<void**>(&g_get_device_state)) == MH_OK)
            MH_EnableHook(fn);
    }
    return hr;
}

}  // namespace

void ui_attach(void* device) {
    if (g_attached || !device) return;
    g_attached = true;
    void** vt = *reinterpret_cast<void***>(device);   // IDirect3DDevice9: Reset is slot 16, EndScene 42
    const bool ok = MH_CreateHook(vt[42], reinterpret_cast<void*>(&hook_end_scene), reinterpret_cast<void**>(&g_end_scene)) == MH_OK &&
                    MH_CreateHook(vt[16], reinterpret_cast<void*>(&hook_reset), reinterpret_cast<void**>(&g_reset)) == MH_OK &&
                    MH_EnableHook(vt[42]) == MH_OK && MH_EnableHook(vt[16]) == MH_OK;
    if (!ok) log("[ui] the device could not be hooked: no overlay");
}

void ui_on_dinput(void* dinput8) {
    if (!hooked() || !dinput8 || g_create_device) return;
    void* fn = (*reinterpret_cast<void***>(dinput8))[3];   // IDirectInput8::CreateDevice
    if (MH_CreateHook(fn, reinterpret_cast<void*>(&hook_create_device), reinterpret_cast<void**>(&g_create_device)) == MH_OK)
        MH_EnableHook(fn);
}

int ui_load_image(const std::filesystem::path& file) {
    std::error_code ec;
    if (!fs::exists(file, ec) || g_images.size() >= 512) return 0;
    for (std::size_t i = 0; i < g_images.size(); ++i)
        if (g_images[i].file == file) return int(i) + 1;
    g_images.push_back({file});
    return int(g_images.size());
}
void* ui_image(int handle, int* w, int* h) {
    if (handle < 1 || handle > int(g_images.size())) return nullptr;
    Image& im = g_images[std::size_t(handle - 1)];
    if (!im.tex && !im.tried && g_device) make_texture(im);
    if (w) *w = im.w;
    if (h) *h = im.h;
    return im.tex;
}

bool ui_holds_game() { return g_panel.open || g_modal_pause; }
bool ui_panel_open() { return g_panel.open; }
bool ui_key_known(const std::string& name, bool pad) { return key_named(name, pad) != nullptr; }
bool ui_key_down(const std::string& name, bool pad) {
    const KeyName* k = key_named(name, pad);
    return k && g_ready && ImGui::IsKeyDown(k->key);
}

void ui_notice(const std::string& head, const std::string& text) { g_notice = {head, text, now()}; }
void ui_open_panel() { g_panel_asked = true; }
void ui_prompt(const std::string& text, bool secret) {
    g_prompt_text = text.substr(0, 120);
    g_prompt_secret = secret;
    g_prompt_done = 0;
    g_prompting = true;                 // (the menu was made with the line already in it)
}
// A filled rectangle straight to the device, in the game's 1024 x 768 layout (centred, scaled by the height): for
// what must lie under something the game draws next. Every state it touches is put back.
void ui_fill_layout(float x0, float y0, float x1, float y1, std::uint32_t argb) {
    IDirect3DDevice9* dev = g_device;
    D3DVIEWPORT9 vp{};
    IDirect3DStateBlock9* was = nullptr;
    if (!dev || FAILED(dev->GetViewport(&vp)) || FAILED(dev->CreateStateBlock(D3DSBT_ALL, &was))) return;
    const float s = float(vp.Height) / 768.0f, ox = (float(vp.Width) - 1024.0f * s) * 0.5f;
    struct Vertex { float x, y, z, w; DWORD c; };
    const Vertex v[4] = {{ox + x0 * s, y0 * s, 0, 1, argb}, {ox + x1 * s, y0 * s, 0, 1, argb}, {ox + x0 * s, y1 * s, 0, 1, argb}, {ox + x1 * s, y1 * s, 0, 1, argb}};
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    dev->SetTexture(0, nullptr);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
    was->Apply();
    was->Release();
}
void ui_item_icon(int item, const std::filesystem::path& file) {
    if (const int h = ui_load_image(file)) g_item_icons[item] = h;
}
bool ui_show_item_icon(int item) {
    const auto it = g_item_icons.find(item);
    if (it == g_item_icons.end()) return false;
    g_icon.image = it->second;
    g_icon.t = now();
    return true;
}

}  // namespace ore
