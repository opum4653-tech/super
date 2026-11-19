#pragma once
// ============================================================================
// TOUCH DIGITIZER EVENT INJECTION & INPUT PIPELINE INTERFACE
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Controls low-level Linux /dev/input event handling and /dev/uinput virtual device injection:
// - Touch_Init(): Detects real digitizer hardware node (e.g. /dev/input/eventX),
//   probes absolute min/max coordinate ranges, and creates a virtual uinput device.
// - Touch_SetUiRect() / Touch_IsPointInUi(): Hit-tests incoming finger touches.
//   Touches inside the ImGui menu or floating logo are captured by our UI.
//   Touches outside the menu are forwarded to the game without lag or interruption.
// - Touch_Close(): Symmetrically ungrabs digitizers and destroys the uinput node.
// ============================================================================

#include <cstdint>


bool Touch_Init(int w, int h, uint32_t orientation_, bool readOnly = false);
void UpdateScreenData(int w, int h, uint32_t orientation_);

void Touch_SetUiRect(float x, float y, float w, float h, bool active);
bool Touch_IsPointInUi(float x, float y);

void Touch_Close();
void Touch_Down(float x, float y);
void Touch_Move(float x, float y);
void Touch_Up();


