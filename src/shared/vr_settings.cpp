#include "vr_settings.hpp"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>

namespace nfsheatvr {
namespace {

std::filesystem::path SettingsDirectory() {
    // Resolve the module containing this function instead of relying on a
    // hard-coded DLL filename. That lets the launcher switch safely to a new
    // runtime build while an already-exited Steam/EA child still holds an old
    // one open. In the settings app this simply resolves to its EXE.
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&VrSettingsPath), &module);
    if (module == nullptr) module = GetModuleHandleW(nullptr);
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return std::filesystem::current_path();
    return std::filesystem::path(std::wstring(path.data(), length)).parent_path();
}

int ReadInt(const wchar_t* section, const wchar_t* key, const int fallback, const int minimum, const int maximum) {
    const auto path = VrSettingsPath().wstring();
    const UINT value = GetPrivateProfileIntW(section, key, fallback, path.c_str());
    return std::clamp(static_cast<int>(value), minimum, maximum);
}

int ReadSignedInt(const wchar_t* section, const wchar_t* key, const int fallback, const int minimum, const int maximum) {
    const auto path = VrSettingsPath().wstring();
    wchar_t text[32]{};
    GetPrivateProfileStringW(section, key, L"", text, static_cast<DWORD>(std::size(text)), path.c_str());
    if (text[0] == L'\0') return fallback;
    wchar_t* end = nullptr;
    const long parsed = wcstol(text, &end, 10);
    if (end == text || *end != L'\0') return fallback;
    return std::clamp(static_cast<int>(parsed), minimum, maximum);
}

bool WriteInt(const wchar_t* section, const wchar_t* key, const int value, const std::wstring& path) {
    return WritePrivateProfileStringW(section, key, std::to_wstring(value).c_str(), path.c_str()) != FALSE;
}

} // namespace

std::filesystem::path VrSettingsPath() {
    return SettingsDirectory() / L"NFSHeatVR.ini";
}

VrSettings LoadVrSettings() {
    VrSettings settings{};
    settings.screenFovDegrees = ReadInt(L"Mono", L"ScreenFovDegrees", settings.screenFovDegrees, 45, 150);
    settings.verticalFovPercent = ReadInt(L"Mono", L"VerticalFovPercent", settings.verticalFovPercent, 100, 175);
    settings.headRotationGainPercent = ReadInt(L"Camera", L"HeadRotationGainPercent", settings.headRotationGainPercent, 50, 250);
    settings.headTranslationPercent = ReadInt(L"Camera", L"HeadTranslationPercent", settings.headTranslationPercent, 0, 200);
    settings.cameraOffsetRightCentimetres = ReadSignedInt(L"Camera", L"CameraOffsetRightCentimetres",
                                                           settings.cameraOffsetRightCentimetres, -100, 100);
    settings.cameraOffsetUpCentimetres = ReadSignedInt(L"Camera", L"CameraOffsetUpCentimetres",
                                                        settings.cameraOffsetUpCentimetres, -100, 100);
    settings.cameraOffsetForwardCentimetres = ReadSignedInt(L"Camera", L"CameraOffsetForwardCentimetres",
                                                             settings.cameraOffsetForwardCentimetres, -100, 100);
    settings.cullTransformSplitEnabled = ReadInt(L"Camera", L"CullTransformSplitEnabled",
                                                 settings.cullTransformSplitEnabled, 0, 1);
    settings.headsetFullscreenEnabled = ReadInt(L"Mono", L"HeadsetFullscreenEnabled",
                                                settings.headsetFullscreenEnabled, 0, 1);
    settings.engineFovOverrideEnabled = ReadInt(L"Engine", L"FovOverrideEnabled", settings.engineFovOverrideEnabled, 0, 1);
    settings.matchEngineFovToHeadset = ReadInt(L"Engine", L"MatchHeadsetFov", settings.matchEngineFovToHeadset, 0, 1);
    settings.engineVerticalFovDegrees = ReadInt(L"Engine", L"VerticalFovDegrees", settings.engineVerticalFovDegrees, 60, 150);
    settings.headsetFullscreenHorizontalFovPercent = ReadInt(L"Fullscreen", L"HorizontalFovPercent",
                                                             settings.headsetFullscreenHorizontalFovPercent, 50, 150);
    settings.headsetFullscreenVerticalFovPercent = ReadInt(L"Fullscreen", L"VerticalFovPercent",
                                                           settings.headsetFullscreenVerticalFovPercent, 50, 150);
    settings.headsetUpscalerSharpnessPercent = ReadInt(L"HeadsetUpscaler", L"SharpnessPercent",
                                                        settings.headsetUpscalerSharpnessPercent, 0, 100);
    settings.gameResolutionWidth = ReadInt(L"Game", L"ResolutionWidth", settings.gameResolutionWidth, 320, 16384);
    settings.gameResolutionHeight = ReadInt(L"Game", L"ResolutionHeight", settings.gameResolutionHeight, 320, 16384);
    settings.highResolutionSourceEnabled = ReadInt(L"Game", L"HighResolutionSourceEnabled",
                                                   settings.highResolutionSourceEnabled, 0, 1);
    settings.highResolutionSourceCandidate = ReadInt(L"Game", L"HighResolutionSourceCandidate",
                                                     settings.highResolutionSourceCandidate, 0, 63);
    settings.highResolutionSourceInspectionEnabled = ReadInt(L"Game", L"HighResolutionSourceInspectionEnabled",
                                                             settings.highResolutionSourceInspectionEnabled, 0, 1);
    settings.highResolutionLatestPassEnabled = ReadInt(L"Game", L"HighResolutionLatestPassEnabled",
                                                       settings.highResolutionLatestPassEnabled, 0, 1);
    settings.highResolutionLatestPassInspectionEnabled = ReadInt(L"Game", L"HighResolutionLatestPassInspectionEnabled",
                                                                 settings.highResolutionLatestPassInspectionEnabled, 0, 1);
    settings.processVirtualDisplayEnabled = ReadInt(L"Game", L"ProcessVirtualDisplayEnabled",
                                                    settings.processVirtualDisplayEnabled, 0, 1);
    settings.depthStereoEnabled = ReadInt(L"Stereo", L"DepthStereoEnabled", settings.depthStereoEnabled, 0, 1);
    settings.depthStereoStrengthPercent = ReadInt(L"Stereo", L"DepthStereoStrengthPercent",
                                                  settings.depthStereoStrengthPercent, 0, 600);
    settings.alternateFrameStereoEnabled = ReadInt(L"Stereo", L"AlternateFrameStereoEnabled",
                                                    settings.alternateFrameStereoEnabled, 0, 1);
    settings.alternateFrameStereoIpdPercent = ReadInt(L"Stereo", L"AlternateFrameStereoIpdPercent",
                                                       settings.alternateFrameStereoIpdPercent, 50, 150);
    settings.recenterRequestId = ReadInt(L"Camera", L"RecenterRequestId", settings.recenterRequestId,
                                         0, 1000000000);
    return settings;
}

bool SaveVrSettings(const VrSettings& input) {
    VrSettings settings = input;
    settings.screenFovDegrees = std::clamp(settings.screenFovDegrees, 45, 150);
    settings.verticalFovPercent = std::clamp(settings.verticalFovPercent, 100, 175);
    settings.headRotationGainPercent = std::clamp(settings.headRotationGainPercent, 50, 250);
    settings.headTranslationPercent = std::clamp(settings.headTranslationPercent, 0, 200);
    settings.cameraOffsetRightCentimetres = std::clamp(settings.cameraOffsetRightCentimetres, -100, 100);
    settings.cameraOffsetUpCentimetres = std::clamp(settings.cameraOffsetUpCentimetres, -100, 100);
    settings.cameraOffsetForwardCentimetres = std::clamp(settings.cameraOffsetForwardCentimetres, -100, 100);
    settings.cullTransformSplitEnabled = std::clamp(settings.cullTransformSplitEnabled, 0, 1);
    settings.headsetFullscreenEnabled = std::clamp(settings.headsetFullscreenEnabled, 0, 1);
    settings.engineFovOverrideEnabled = std::clamp(settings.engineFovOverrideEnabled, 0, 1);
    settings.matchEngineFovToHeadset = std::clamp(settings.matchEngineFovToHeadset, 0, 1);
    settings.engineVerticalFovDegrees = std::clamp(settings.engineVerticalFovDegrees, 60, 150);
    settings.headsetFullscreenHorizontalFovPercent = std::clamp(settings.headsetFullscreenHorizontalFovPercent, 50, 150);
    settings.headsetFullscreenVerticalFovPercent = std::clamp(settings.headsetFullscreenVerticalFovPercent, 50, 150);
    settings.headsetUpscalerSharpnessPercent = std::clamp(settings.headsetUpscalerSharpnessPercent, 0, 100);
    settings.gameResolutionWidth = std::clamp(settings.gameResolutionWidth, 320, 16384);
    settings.gameResolutionHeight = std::clamp(settings.gameResolutionHeight, 320, 16384);
    settings.highResolutionSourceEnabled = std::clamp(settings.highResolutionSourceEnabled, 0, 1);
    settings.highResolutionSourceCandidate = std::clamp(settings.highResolutionSourceCandidate, 0, 63);
    settings.highResolutionSourceInspectionEnabled = std::clamp(settings.highResolutionSourceInspectionEnabled, 0, 1);
    settings.highResolutionLatestPassEnabled = std::clamp(settings.highResolutionLatestPassEnabled, 0, 1);
    settings.highResolutionLatestPassInspectionEnabled = std::clamp(settings.highResolutionLatestPassInspectionEnabled, 0, 1);
    settings.processVirtualDisplayEnabled = std::clamp(settings.processVirtualDisplayEnabled, 0, 1);
    settings.depthStereoEnabled = std::clamp(settings.depthStereoEnabled, 0, 1);
    settings.depthStereoStrengthPercent = std::clamp(settings.depthStereoStrengthPercent, 0, 600);
    settings.alternateFrameStereoEnabled = std::clamp(settings.alternateFrameStereoEnabled, 0, 1);
    settings.alternateFrameStereoIpdPercent = std::clamp(settings.alternateFrameStereoIpdPercent, 50, 150);
    settings.recenterRequestId = std::clamp(settings.recenterRequestId, 0, 1000000000);
    const auto path = VrSettingsPath().wstring();
    return WriteInt(L"Mono", L"ScreenFovDegrees", settings.screenFovDegrees, path) &&
           WriteInt(L"Mono", L"VerticalFovPercent", settings.verticalFovPercent, path) &&
           WriteInt(L"Mono", L"HeadsetFullscreenEnabled", settings.headsetFullscreenEnabled, path) &&
           WriteInt(L"Engine", L"FovOverrideEnabled", settings.engineFovOverrideEnabled, path) &&
           WriteInt(L"Engine", L"MatchHeadsetFov", settings.matchEngineFovToHeadset, path) &&
           WriteInt(L"Engine", L"VerticalFovDegrees", settings.engineVerticalFovDegrees, path) &&
           WriteInt(L"Fullscreen", L"HorizontalFovPercent", settings.headsetFullscreenHorizontalFovPercent, path) &&
           WriteInt(L"Fullscreen", L"VerticalFovPercent", settings.headsetFullscreenVerticalFovPercent, path) &&
           WriteInt(L"HeadsetUpscaler", L"SharpnessPercent", settings.headsetUpscalerSharpnessPercent, path) &&
           WriteInt(L"Game", L"ResolutionWidth", settings.gameResolutionWidth, path) &&
           WriteInt(L"Game", L"ResolutionHeight", settings.gameResolutionHeight, path) &&
           WriteInt(L"Game", L"HighResolutionSourceEnabled", settings.highResolutionSourceEnabled, path) &&
           WriteInt(L"Game", L"HighResolutionSourceCandidate", settings.highResolutionSourceCandidate, path) &&
           WriteInt(L"Game", L"HighResolutionSourceInspectionEnabled", settings.highResolutionSourceInspectionEnabled, path) &&
           WriteInt(L"Game", L"HighResolutionLatestPassEnabled", settings.highResolutionLatestPassEnabled, path) &&
           WriteInt(L"Game", L"HighResolutionLatestPassInspectionEnabled", settings.highResolutionLatestPassInspectionEnabled, path) &&
           WriteInt(L"Game", L"ProcessVirtualDisplayEnabled", settings.processVirtualDisplayEnabled, path) &&
           WriteInt(L"Stereo", L"DepthStereoEnabled", settings.depthStereoEnabled, path) &&
           WriteInt(L"Stereo", L"DepthStereoStrengthPercent", settings.depthStereoStrengthPercent, path) &&
           WriteInt(L"Stereo", L"AlternateFrameStereoEnabled", settings.alternateFrameStereoEnabled, path) &&
           WriteInt(L"Stereo", L"AlternateFrameStereoIpdPercent", settings.alternateFrameStereoIpdPercent, path) &&
           WriteInt(L"Camera", L"HeadRotationGainPercent", settings.headRotationGainPercent, path) &&
           WriteInt(L"Camera", L"HeadTranslationPercent", settings.headTranslationPercent, path) &&
           WriteInt(L"Camera", L"CameraOffsetRightCentimetres", settings.cameraOffsetRightCentimetres, path) &&
           WriteInt(L"Camera", L"CameraOffsetUpCentimetres", settings.cameraOffsetUpCentimetres, path) &&
           WriteInt(L"Camera", L"CameraOffsetForwardCentimetres", settings.cameraOffsetForwardCentimetres, path) &&
           WriteInt(L"Camera", L"CullTransformSplitEnabled", settings.cullTransformSplitEnabled, path) &&
           WriteInt(L"Camera", L"RecenterRequestId", settings.recenterRequestId, path);
}

} // namespace nfsheatvr
