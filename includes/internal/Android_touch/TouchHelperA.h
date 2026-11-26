#pragma once
// ============================================================================
// TOUCH DIGITIZER EVENT INJECTION & MULTI-REGION INPUT PIPELINE INTERFACE
// ============================================================================
// Project: crack_master
// Target Architecture: ARM64-v8a Linux Multi-Touch Input Subsystem
//
// SYSTEM ROLE:
// Controls low-level Linux /dev/input event handling and /dev/uinput virtual device injection:
// - Touch_Init(): Detects real digitizer hardware node (e.g. /dev/input/eventX),
//   probes absolute min/max coordinate ranges, and creates a virtual uinput device.
// - Multi-Region UI Barrier:
//   Touch_ClearUiRects() / Touch_AddUiRect() / Touch_AddUiCircle() / Touch_CommitUiRects()
//   Atomic double-buffered registration prevents race conditions between render thread
//   and asynchronous touch worker threads.
// - Sticky Swallow: Latches swallow on touch start inside UI hitboxes, enabling 100%
//   continuous dragging across screen without leaking gestures to the underlying game.
// - Touch_Close(): Symmetrically ungrabs digitizers and destroys the uinput node.
// ============================================================================

#include <cstdint>

bool Touch_Init(int w, int h, uint32_t orientation_, bool readOnly = false);
void UpdateScreenData(int w, int h, uint32_t orientation_);

bool Touch_IsPointInUi(float x, float y);

// Multi-region API: clear once per frame, register every live UI panel,
// then commit so the touch thread sees the whole set at once.
void Touch_ClearUiRects();
void Touch_AddUiRect(float x, float y, float w, float h);
void Touch_AddUiCircle(float cx, float cy, float radius);
void Touch_CommitUiRects();

// Backwards-compatible legacy single-rect setter
inline void Touch_SetUiRect(float x, float y, float w, float h, bool active) {
    if (active) {
        Touch_ClearUiRects();
        Touch_AddUiRect(x, y, w, h);
        Touch_CommitUiRects();
    }
}

// Slot-addressed injection used by non-gesture callers.
bool IsFingerDown(int slot);
bool GetFingerScreenPos(int slot, float& outX, float& outY);
bool Touch_IsUinputReady();
void Touch_Down_Slot(int slot, float xt, float yt);
void Touch_Move_Slot(int slot, float x, float y);
void Touch_Up_Slot(int slot);

void Touch_Close();
void Touch_Down(float x, float y);
void Touch_Move(float x, float y);
void Touch_Up();
