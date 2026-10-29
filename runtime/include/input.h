// Controller input for XamInput*: XInput pad 0, else the keyboard while the game window has focus,
// plus SCDA_AUTOPRESS scripted presses (unattended menu testing).
#pragma once
#include <windows.h>
#include <xinput.h>
#include <cstdint>
#include <vector>

// Writes a guest X_INPUT_STATE (packet number + XINPUT_GAMEPAD, big-endian) at addr.
void StoreInputState(uint32_t addr, uint32_t packet, const XINPUT_GAMEPAD& g);

// Keyboard/mouse buttons -> gamepad: Enter/Space A, Backspace/Ctrl B, E X, Q Y, Esc Start, Tab Back,
// arrows d-pad, WASD left stick, IJKL right stick, Z/right mouse LT, C/left mouse RT, 1/3 shoulders.
XINPUT_GAMEPAD KeyboardGamepad(bool (*down)(int vk));

// Mouse look. The window thread adds raw motion and wheel notches; each XamInputGetState poll
// takes them (MouseTake zeroes the totals) and turns the motion into the right stick: full
// deflection at 50 / sensitivity pixels, +dy (down) pulls the stick down unless inverted.
void MouseAccumulate(int dx, int dy, int wheel);
void MouseTake(int& dx, int& dy, int& wheel);
void MouseToStick(int dx, int dy, float sensitivity, bool invertY, int16_t& rx, int16_t& ry);

struct AutoPress
{
    uint32_t frame;   // guest frame number (as SCDA_DUMP_FRAME counts)
    uint16_t buttons; // XINPUT_GAMEPAD_* bits
    uint32_t frames;  // frames held
    int axis;         // -1 none; 0-3 LX LY RX RY; 4-5 LT RT
    float value;      // axis value: sticks [-1, 1], triggers [0, 1]
};
// "<frame>:<name>[=<value>][:<frames>],..." with buttons A B X Y START BACK UP DOWN LEFT RIGHT LB RB,
// sticks LX LY RX RY (value required, [-1, 1]) and triggers LT RT ([0, 1]). Each entry is held for
// <frames> frames (default kAutoPressFrames). Bad entries are skipped.
constexpr uint32_t kAutoPressFrames = 4;
std::vector<AutoPress> ParseAutoPress(const char* text);
// Applies the script at a frame: buttons are ORed in, held axes replace the pad's.
void AutoPressApply(const std::vector<AutoPress>& script, uint32_t frame, XINPUT_GAMEPAD& g);
// Buttons the script holds at a frame.
uint16_t AutoPressButtons(const std::vector<AutoPress>& script, uint32_t frame);
