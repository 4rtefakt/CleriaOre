// The overlay: Dear ImGui drawn in the game's own EndScene, with CleriaOre's UI kit (kit.inl: the API of
// CleriaCore's shell kit, in the look of Ys Origin's own menus).
//   - the mods' HUD layers (cleria.hud.layer), every frame;
//   - the F1 panel: the mods, On / Off, and each mod's own page (cleria.ui.page).
#pragma once
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"

namespace fs = std::filesystem;   // the kit names it so
#include "kit.inl"

namespace ore {

inline ImDrawList* g_hud = nullptr;   // the list a mod's HUD layer draws on; null outside its call

void ui_attach(void* device);         // hooks.cpp, each frame: the game's IDirect3DDevice9 (hooked once)
void ui_on_dinput(void* dinput8);     // the proxy: the game's IDirectInput8 was created
bool ui_holds_game();                 // the panel, or a mod's window that pauses, is open: the simulation is held
bool ui_panel_open();                 // the F1 panel is open
// Keys and pad buttons by SDL's names for them, as CleriaCore's mods spell them ("F5", "Left Shift", "A"; "a",
// "back", "dpup"): is one held now; is the name one this host knows.
bool ui_key_down(const std::string& name, bool pad);
bool ui_key_known(const std::string& name, bool pad);
void ui_notice(const std::string& head, const std::string& text);   // a card over the game for a few seconds
void ui_open_panel();                                               // the F1 panel, as F1 opens it
// A mod's picture for an item of its own or for an icon id (items.ini Icon=, cleria.ui.icon): kept by id, and shown
// beside the card for a few seconds.
void ui_item_icon(int item, const std::filesystem::path& file);
bool ui_show_item_icon(int item);                                   // false: no icon was given for it
// A mod's images (cleria.hud images, p:image): a .dds file (DXT1 / DXT3 / DXT5 or 32-bit) as a texture of the
// game's device, made at its first draw. The handle is for ui_image(); 0 = not a readable DDS.
int ui_load_image(const std::filesystem::path& file);
void* ui_image(int handle, int* w = nullptr, int* h = nullptr);     // the texture for ImGui, null until it can be made

}  // namespace ore
