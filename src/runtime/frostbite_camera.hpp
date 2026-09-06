#pragma once

namespace nfsheatvr {

// Installs an observation-only callback for Heat 1.0.60.7040's camera update.
// It always calls the original engine function and does not alter camera data.
void InstallFrostbiteCameraObserver();
void RemoveFrostbiteCameraObserver();

// OpenXR poses are submitted by the presenter. They are converted to a pose
// relative to F9's centre and consumed by the Frostbite camera callback.
void SetFrostbiteHeadTrackingEnabled(bool enabled);
void SetFrostbiteHeadTrackingTuning(float rotationGain, float translationScale);
void SetFrostbiteCameraOffset(float rightMetres, float upMetres, float forwardMetres);
// This is the real player-camera FOV used by Heat's Frostbite renderer.  The
// interception point was validated against the locally installed retail build.
void SetFrostbiteEngineFovOverride(bool enabled, float verticalDegrees);
// Signed eye displacement in the camera's local right direction. The
// presenter alternates it per game frame to produce real left/right renders.
void SetFrostbiteStereoEyeOffset(float rightMetres);
void SetFrostbiteCullTransformSplitEnabled(bool enabled);
void UpdateFrostbiteHeadPose(float orientationX, float orientationY, float orientationZ, float orientationW,
                             float positionX, float positionY, float positionZ, bool recenter);

} // namespace nfsheatvr
