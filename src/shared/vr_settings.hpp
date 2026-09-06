#pragma once

#include <filesystem>

namespace nfsheatvr {

// The values intentionally use ordinary, human-readable units because this
// file is also the contract with the small settings application.
struct VrSettings {
    int screenFovDegrees{78};
    int verticalFovPercent{100};
    int headRotationGainPercent{135};
    int headTranslationPercent{100};
    int cameraOffsetRightCentimetres{};
    int cameraOffsetUpCentimetres{};
    int cameraOffsetForwardCentimetres{};
    int cullTransformSplitEnabled{1};
    int headsetFullscreenEnabled{1};
    int engineFovOverrideEnabled{};
    // When enabled, Frostbite's real camera FOV follows the FOV currently
    // reported by OpenXR. This avoids rendering a wide Normal-FOV camera
    // when the headset itself is set to a narrower optical mode.
    int matchEngineFovToHeadset{1};
    // Frostbite stores this as radians.  Keep degrees here so the UI and INI
    // remain readable. Values above 120° are intentionally available for
    // experimental wide cockpit tuning on this specific Heat build.
    int engineVerticalFovDegrees{100};
    // Fullscreen presentation maps one Frostbite frame across the native HMD
    // field of view.  Keep its horizontal and vertical extent independent so
    // a wide Pimax view can be tuned without changing cockpit height.
    int headsetFullscreenHorizontalFovPercent{100};
    int headsetFullscreenVerticalFovPercent{100};
    // This is an adaptive spatial filter applied only while copying Heat's
    // completed colour image into the headset textures.  It is deliberately
    // not labelled DLSS: it has no engine motion-vector history.  Zero keeps
    // the unfiltered presentation; higher values add more local contrast.
    int headsetUpscalerSharpnessPercent{55};
    // Requested Heat back-buffer size. The settings panel stores this in the
    // mod INI; the launcher applies it to Heat before the next game start.
    int gameResolutionWidth{2560};
    int gameResolutionHeight{1080};
    // The selected high-resolution Frostbite target is experimental.  The
    // ordinary final frame remains the fallback until a complete scene target
    // is identified for this game build.
    int highResolutionSourceEnabled{};
    int highResolutionSourceCandidate{};
    int highResolutionSourceInspectionEnabled{};
    int highResolutionLatestPassEnabled{};
    int highResolutionLatestPassInspectionEnabled{};
    // Reports that size only to Frostbite before it creates its renderer. It
    // avoids an external virtual monitor or a global Windows mode change.
    int processVirtualDisplayEnabled{1};
    int depthStereoEnabled{};
    int depthStereoStrengthPercent{35};
    int alternateFrameStereoEnabled{};
    int alternateFrameStereoIpdPercent{100};
    // Incremented by the control panel.  A counter instead of a boolean lets
    // a button request repeated recentres while the game is running.
    int recenterRequestId{};

    bool operator==(const VrSettings&) const = default;
};

std::filesystem::path VrSettingsPath();
VrSettings LoadVrSettings();
bool SaveVrSettings(const VrSettings& settings);

} // namespace nfsheatvr
