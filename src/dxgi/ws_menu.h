// WarmerSun - in-game settings panel (Dear ImGui, MIT).
#pragma once

#include <windows.h>
#include <d3d11.h>

#include <string>

#include "ws_common.h"

namespace ws {

bool menu_init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context,
               const std::wstring& configPath);
void menu_shutdown();

bool menu_visible();
void menu_toggle();

// Advances ImGui and draws the panel into the currently bound render target.
void menu_render(ID3D11DeviceContext* context, Config& cfg);

} // namespace ws
