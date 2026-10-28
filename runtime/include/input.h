// Controller input for XamInput*: XInput pad 0, else the keyboard while the game window has focus,
// plus SCDA_AUTOPRESS scripted presses (unattended menu testing).
#pragma once
#include <windows.h>
#include <xinput.h>
#include <cstdint>
#include <vector>

// Writes a guest X_INPUT_STATE (packet number + XINPUT_GAMEPAD, big-endian) at addr.
void StoreInputState(uint32_t addr, uint32_t packet, const XINPUT_GAMEPAD& g);

// Keyboard -> gamepad: Enter A, Backspace B, E X, Q Y, Esc Start, Tab Back, arrows d-pad,
// WASD left stick, IJKL right stick, Z/C triggers, 1/3 shoulders.
XINPUT_GAMEPAD KeyboardGamepad(bool (*down)(int vk));

struct AutoPress
{
    uint32_t ms;      // since start
    uint16_t buttons; // XINPUT_GAMEPAD_* bits
};
// "<seconds>:<button>,..." with buttons A B X Y START BACK UP DOWN LEFT RIGHT LB RB; bad entries are skipped.
std::vector<AutoPress> ParseAutoPress(const char* text);
// Buttons the script holds at time ms (each press lasts 200 ms).
uint16_t AutoPressButtons(const std::vector<AutoPress>& script, uint32_t ms);
