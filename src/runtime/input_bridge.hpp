#pragma once

#include <Windows.h>

namespace nfsheatvr {

// Frostbite reads relative mouse movement through Raw Input.  This bridge is
// dormant until head-look queues a delta, so ordinary mouse/gamepad input is
// passed through untouched.
bool InstallInputBridge();
void RemoveInputBridge();
void QueueHeadLookRawDelta(LONG deltaX, LONG deltaY);
void ClearHeadLookRawDeltas();

// A fallback for games that deliberately ignore mouse-look while driving.
// Values are standard XInput right-stick units and are dormant at (0, 0).
void SetHeadLookGamepadStick(SHORT rightX, SHORT rightY);
void ClearHeadLookGamepadStick();

} // namespace nfsheatvr
