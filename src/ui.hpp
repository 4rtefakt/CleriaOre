// The overlay: Dear ImGui drawn in the game's own EndScene, with CleriaCore's shell UI kit (its
// apps/cleria-view/view_ui.inl, used as it is: the same panel, rows and palette as CleriaCore's F1).
//   - the mods' HUD layers (cleria.hud.layer), every frame;
//   - the F1 panel: the mods, On / Off, and each mod's own page (cleria.ui.page).
#pragma once
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"

namespace fs = std::filesystem;   // view_ui.inl names it so
#include "view_ui.inl"

namespace ore {

inline ImDrawList* g_hud = nullptr;   // the list a mod's HUD layer draws on; null outside its call

void ui_attach(void* device);         // hooks.cpp, each frame: the game's IDirect3DDevice9 (hooked once)
void ui_on_dinput(void* dinput8);     // the proxy: the game's IDirectInput8 was created
bool ui_holds_game();                 // the panel is open: the game's simulation and keyboard are held
void ui_notice(const std::string& head, const std::string& text);   // a card over the game for a few seconds

}  // namespace ore
