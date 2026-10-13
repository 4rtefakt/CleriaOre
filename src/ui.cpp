#include "ui.hpp"

#include <windows.h>
#include <d3d9.h>

#include <chrono>
#include <cstring>
#include <map>

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
bool g_modal_pause = false;           // ... and holds the game

struct Notice { std::string head, text; double t = -100; } g_notice;

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

script::Mod* script_of(const std::string& id) {
    for (auto& m : scripts())
        if (m->id() == id) return m.get();
    return nullptr;
}

// The Mods tab: the list, or one mod's page.
void mods_page(ui::List& L) {
    if (!g_sub.empty()) {
        script::Mod* m = script_of(g_sub);
        if (ui::back_row(L, "Mods", m ? m->name().c_str() : g_sub.c_str()) || !m) {
            g_sub.clear();
            return;
        }
        if (!m->ok()) {
            ui::section(L, "Settings");
            ui::info(L, "Status", "Stopped by an error", ui::kWarn,
                     ("This mod's script failed and was turned off until the game restarts.\n\n" + m->error()).c_str());
            return;
        }
        const std::size_t n = m->pages().size();
        for (std::size_t i = 0; i < n; ++i) m->draw_page(i, &L);
        return;
    }
    ui::section(L, "Mods");
    if (mod_list().empty())
        ui::info(L, "No mod installed", "", ui::kMuted, ("Put a mod's folder, .zip or .cleriamod in:\n\n" + (dir() / "mods").string()).c_str());
    for (mods::Mod& m : mod_list()) {
        std::string desc = m.m.description;
        desc += "\n\n" + m.m.id + " " + m.m.version + (m.m.author.empty() ? "" : ", by " + m.m.author);
        for (const std::string& e : m.errors) desc += "\n\n" + e;
        if (g_changed) desc += "\n\nRestart the game to apply.";
        bool on = m.enabled;
        if (ui::toggle(L, m.label().c_str(), &on, desc.c_str())) {
            m.enabled = on;
            g_changed = true;
            save_prefs();
        }
        script::Mod* s = script_of(m.m.id);
        if (s && (!s->pages().empty() || !s->ok()) &&
            ui::button(L, ("    " + m.label() + ": settings").c_str(), "This mod's own page."))
            g_sub = m.m.id;
    }
}

void init(IDirect3DDevice9* dev);

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
}

HRESULT WINAPI hook_end_scene(IDirect3DDevice9* dev) {
    if (!g_ready) init(dev);
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

    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) g_panel.open = !g_panel.open;
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

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    return g_end_scene(dev);
}

HRESULT WINAPI hook_reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (g_ready) ImGui_ImplDX9_InvalidateDeviceObjects();
    const HRESULT hr = g_reset(dev, pp);
    if (g_ready) ImGui_ImplDX9_CreateDeviceObjects();
    return hr;
}

// ImGui reads the window's messages; while the panel is open the game's window does not get the keys
// and the mouse.
LRESULT CALLBACK hook_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    ImGui_ImplWin32_WndProcHandler(wnd, msg, wp, lp);
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
    const HRESULT hr = g_get_device_state(self, size, data);
    if (data && size == 256 && (g_panel.open || g_modal || GetTickCount() - g_closed_at < 250)) std::memset(data, 0, size);
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

bool ui_holds_game() { return g_panel.open || g_modal_pause; }
bool ui_panel_open() { return g_panel.open; }
bool ui_key_known(const std::string& name, bool pad) { return key_named(name, pad) != nullptr; }
bool ui_key_down(const std::string& name, bool pad) {
    const KeyName* k = key_named(name, pad);
    return k && g_ready && ImGui::IsKeyDown(k->key);
}

void ui_notice(const std::string& head, const std::string& text) { g_notice = {head, text, now()}; }

}  // namespace ore
