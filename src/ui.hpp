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
bool ui_holds_game();                 // the panel is open: the game's simulation and keyboard are held
void ui_notice(const std::string& head, const std::string& text);   // a card over the game for a few seconds

}  // namespace ore
