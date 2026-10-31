// Extra Settings menu (F1 or L3+R3; Dear ImGui) drawn over the game: Display, Graphics and Update
// tabs, plus the update notification once the player signs in at the main menu. ImGui runs on the
// render thread; the window thread only toggles the menu and queues the input messages it takes.
#pragma once
#include <windows.h>
#include <settings.h>

class D3D11Renderer;

// Render thread, once the renderer is up: applies the settings (scale, post-processing) and draws
// the menu / notification over every presented frame.
void OverlayAttach(D3D11Renderer& renderer, HWND hwnd, const DisplaySettings& settings);

// Posted to the game window by the render thread when L3+R3 is pressed: toggle like F1.
constexpr UINT kOverlayToggleMessage = WM_APP + 1;

// Window thread. F1 (and Esc while open) toggles the menu; returns the new state. The menu never
// opens by itself.
bool OverlayToggle();
bool OverlayOpen();
// While the menu is open, mouse and keyboard messages go to it (true: taken, not for the game).
bool OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
