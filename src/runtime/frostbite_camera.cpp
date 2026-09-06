#include "frostbite_camera.hpp"

#include "logger.hpp"

#include <MinHook.h>

#include <Windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace nfsheatvr {
namespace {

// Profiled from the retail NeedForSpeedHeat.exe 1.0.60.7040 installed on this
// PC. It is relative to the executable image, so ASLR is respected. The
// callback receives camera state in RCX and a 4x4 transform in RDX. A
// different build must be profiled before this hook is enabled.
constexpr std::uintptr_t kCameraUpdateCallbackRva = 0x08DB7DD0ull;
// The function Camera Toolkit V2 labels "FOV update" in this exact Heat build.
// It writes the active camera FOV to its state object after the normal camera
// update.  The toolkit detour calls the original, then changes [RCX+0x1168].
constexpr std::uintptr_t kFovUpdateCallbackRva = 0x088450D0ull;
constexpr std::size_t kTransformFloatCount = 16;
constexpr float kDefaultHeadRotationGain = 1.35f;
constexpr float kPositionLimitLateral = 0.22f;
constexpr float kPositionLimitVertical = 0.14f;
constexpr float kPositionLimitForward = 0.22f;

using CameraUpdateFn = void (*)(void* cameraState, const float* transform);
using FovUpdateFn = void (*)(void* fovState);

struct Quaternion {
    float x{};
    float y{};
    float z{};
    float w{1.0f};
};

struct Vector3 {
    float x{};
    float y{};
    float z{};
};

CameraUpdateFn gOriginalCameraUpdate = nullptr;
void* gCameraUpdateTarget = nullptr;
FovUpdateFn gOriginalFovUpdate = nullptr;
void* gFovUpdateTarget = nullptr;
std::atomic_bool gObserverInstalled{};
std::atomic_bool gFovHookInstalled{};
std::atomic_bool gFirstCallbackLogged{};
std::atomic_bool gFirstFovCallbackLogged{};
std::atomic_bool gHeadTrackingEnabled{};
std::atomic_bool gHeadPoseReady{};
std::atomic<float> gHeadRotationGain{kDefaultHeadRotationGain};
std::atomic<float> gHeadTranslationScale{1.0f};
std::atomic<float> gCameraOffsetRight{};
std::atomic<float> gCameraOffsetUp{};
std::atomic<float> gCameraOffsetForward{};
std::atomic_bool gCameraOffsetActive{};
std::atomic<float> gStereoEyeOffsetRight{};
std::atomic_bool gCullTransformSplitEnabled{true};
std::atomic_bool gEngineFovOverrideEnabled{};
std::atomic<float> gEngineVerticalFovRadians{1.74532925f};
std::atomic<std::uintptr_t> gLastCameraState{};
std::atomic<std::uintptr_t> gLastFovState{};
Quaternion gHeadCentre{};
Vector3 gHeadCentrePosition{};
bool gHaveHeadCentre{};
std::atomic<float> gRelativeOrientationX{0.0f};
std::atomic<float> gRelativeOrientationY{0.0f};
std::atomic<float> gRelativeOrientationZ{0.0f};
std::atomic<float> gRelativeOrientationW{1.0f};
std::atomic<float> gRelativePositionX{0.0f};
std::atomic<float> gRelativePositionY{0.0f};
std::atomic<float> gRelativePositionZ{0.0f};

float Clamp(const float value, const float low, const float high) {
    return value < low ? low : (value > high ? high : value);
}

bool IsWritableAddress(void* address, const std::size_t bytes) {
    MEMORY_BASIC_INFORMATION information{};
    if (address == nullptr || VirtualQuery(address, &information, sizeof(information)) == 0 ||
        information.State != MEM_COMMIT || (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const DWORD protection = information.Protect & 0xFF;
    const bool writable = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
                          protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto end = start + bytes;
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(information.BaseAddress) + information.RegionSize;
    return writable && end >= start && end <= regionEnd;
}

bool IsReadableAddress(const void* address, const std::size_t bytes) {
    MEMORY_BASIC_INFORMATION information{};
    if (address == nullptr || VirtualQuery(address, &information, sizeof(information)) == 0 ||
        information.State != MEM_COMMIT || (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto end = start + bytes;
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(information.BaseAddress) + information.RegionSize;
    return end >= start && end <= regionEnd;
}

Quaternion Normalise(const Quaternion value) {
    const float lengthSquared = value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
    if (!std::isfinite(lengthSquared) || lengthSquared < 0.000001f) return {};
    const float scale = 1.0f / std::sqrt(lengthSquared);
    return {value.x * scale, value.y * scale, value.z * scale, value.w * scale};
}

Quaternion Inverse(const Quaternion value) {
    return {-value.x, -value.y, -value.z, value.w};
}

Quaternion Multiply(const Quaternion a, const Quaternion b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

Quaternion ScaleRotation(const Quaternion value, const float gain) {
    const Quaternion normalised = Normalise(value);
    const float halfAngle = std::acos(Clamp(normalised.w, -1.0f, 1.0f));
    const float sine = std::sin(halfAngle);
    if (std::fabs(sine) < 0.00001f) return normalised;
    const float scaledSine = std::sin(halfAngle * gain) / sine;
    return Normalise({normalised.x * scaledSine, normalised.y * scaledSine, normalised.z * scaledSine,
                      std::cos(halfAngle * gain)});
}

// Project an orientation onto the two axes that a seated driving camera uses.
// Frostbite expects a level camera: forwarding the HMD's roll can rotate the
// entire game 90 degrees when the tracking origin or a recenter carries a
// shoulder tilt.  We therefore keep yaw and pitch, but never inject roll.
Quaternion WithoutRoll(const Quaternion value) {
    const Quaternion normalised = Normalise(value);
    const float pitchSine = Clamp(2.0f * (normalised.w * normalised.x - normalised.y * normalised.z), -1.0f, 1.0f);
    const float pitch = std::asin(pitchSine);
    const float yaw = std::atan2(2.0f * (normalised.w * normalised.y + normalised.z * normalised.x),
                                 1.0f - 2.0f * (normalised.y * normalised.y + normalised.x * normalised.x));
    const float halfPitch = pitch * 0.5f;
    const float halfYaw = yaw * 0.5f;
    const Quaternion yawOnly{0.0f, std::sin(halfYaw), 0.0f, std::cos(halfYaw)};
    const Quaternion pitchOnly{std::sin(halfPitch), 0.0f, 0.0f, std::cos(halfPitch)};
    return Normalise(Multiply(yawOnly, pitchOnly));
}

Vector3 Rotate(const Quaternion q, const Vector3 value) {
    const Vector3 axis{q.x, q.y, q.z};
    const Vector3 twiceCross{
        2.0f * (axis.y * value.z - axis.z * value.y),
        2.0f * (axis.z * value.x - axis.x * value.z),
        2.0f * (axis.x * value.y - axis.y * value.x),
    };
    return {
        value.x + q.w * twiceCross.x + (axis.y * twiceCross.z - axis.z * twiceCross.y),
        value.y + q.w * twiceCross.y + (axis.z * twiceCross.x - axis.x * twiceCross.z),
        value.z + q.w * twiceCross.z + (axis.x * twiceCross.y - axis.y * twiceCross.x),
    };
}

void ApplyHeadPoseToCameraTransform(float* matrix, const bool useHeadPose) {
    const Quaternion head = useHeadPose
        ? ScaleRotation({gRelativeOrientationX.load(), gRelativeOrientationY.load(),
                         gRelativeOrientationZ.load(), gRelativeOrientationW.load()}, gHeadRotationGain.load())
        : Quaternion{};
    const float x = head.x;
    const float y = head.y;
    const float z = head.z;
    const float w = head.w;
    const float headRotation[3][3]{
        {1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y + z * w), 2.0f * (x * z - y * w)},
        {2.0f * (x * y - z * w), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + x * w)},
        {2.0f * (x * z + y * w), 2.0f * (y * z - x * w), 1.0f - 2.0f * (x * x + y * y)},
    };

    float baseRotation[3][3]{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) baseRotation[row][column] = matrix[row * 4 + column];
    }
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            matrix[row * 4 + column] = headRotation[row][0] * baseRotation[0][column] +
                                       headRotation[row][1] * baseRotation[1][column] +
                                       headRotation[row][2] * baseRotation[2][column];
        }
    }

    const float translationScale = gHeadTranslationScale.load();
    const float headX = useHeadPose ? Clamp(gRelativePositionX.load() * translationScale, -kPositionLimitLateral, kPositionLimitLateral) : 0.0f;
    const float headY = useHeadPose ? Clamp(gRelativePositionY.load() * translationScale, -kPositionLimitVertical, kPositionLimitVertical) : 0.0f;
    const float headZ = useHeadPose ? Clamp(gRelativePositionZ.load() * translationScale, -kPositionLimitForward, kPositionLimitForward) : 0.0f;
    // The fixed seat offset is intentionally separate from the head-motion
    // safety limits. It lets the driver place the viewpoint in the cockpit;
    // moving their real head is still constrained to a comfortable 6DoF range.
    const float localX = gCameraOffsetRight.load() + headX;
    const float localY = gCameraOffsetUp.load() + headY;
    const float localZ = gCameraOffsetForward.load() + headZ;
    for (std::size_t column = 0; column < 3; ++column) {
        matrix[12 + column] += baseRotation[0][column] * localX +
                               baseRotation[1][column] * localY +
                               baseRotation[2][column] * localZ;
        // Unlike the seat position, a physical eye offset must turn with the
        // driver's head. The adjusted camera basis contains that rotation.
        matrix[12 + column] += matrix[column] * gStereoEyeOffsetRight.load();
    }
}

void LogCapturedCameraCallback(void* cameraState, const float* transform) {
    std::array<float, kTransformFloatCount> values{};
    std::memcpy(values.data(), transform, sizeof(values));
    std::wostringstream line;
    line << std::fixed << std::setprecision(5);
    line << L"Heat camera callback captured object=0x" << std::hex << std::uppercase
         << reinterpret_cast<std::uintptr_t>(cameraState) << std::dec << L" transform:";
    for (const float value : values) line << L" " << value;
    Log(line.str());
}

void LogFovStateProjectionCandidates(void* fovState) {
    // Read-only, one-shot reconnaissance. Frostbite's packed retail image is
    // unpacked at runtime, so this is the safe way to identify a real live
    // projection matrix before we ever consider changing one.
    constexpr std::size_t kProbeBytes = 0x3000;
    if (!IsReadableAddress(fovState, kProbeBytes)) {
        Log(L"FOV state probe skipped: camera object range is not readable.");
        return;
    }
    const auto object = reinterpret_cast<std::uintptr_t>(fovState);
    const auto lastCamera = gLastCameraState.load();
    float fovRadians{};
    std::memcpy(&fovRadians, reinterpret_cast<const std::byte*>(fovState) + 0x1168, sizeof(fovRadians));
    std::wostringstream header;
    header << std::fixed << std::setprecision(5)
           << L"FOV update captured object=0x" << std::hex << std::uppercase << object << std::dec
           << L"; last camera object=0x" << std::hex << lastCamera << std::dec
           << L"; delta=" << static_cast<std::int64_t>(object) - static_cast<std::int64_t>(lastCamera)
           << L"; live FOV radians=" << fovRadians;
    Log(header.str());

    unsigned candidates = 0;
    for (std::size_t offset = 0; offset + sizeof(float) * 16 <= kProbeBytes && candidates < 10; offset += 16) {
        std::array<float, 16> matrix{};
        std::memcpy(matrix.data(), reinterpret_cast<const std::byte*>(fovState) + offset, sizeof(matrix));
        bool finite = true;
        for (const float value : matrix) finite = finite && std::isfinite(value);
        if (!finite) continue;
        const bool plausibleScale = std::fabs(matrix[0]) > 0.05f && std::fabs(matrix[0]) < 10.0f &&
                                    std::fabs(matrix[5]) > 0.05f && std::fabs(matrix[5]) < 10.0f;
        const bool perspectiveRow = std::fabs(std::fabs(matrix[11]) - 1.0f) < 0.05f && std::fabs(matrix[15]) < 0.05f;
        const bool perspectiveColumn = std::fabs(std::fabs(matrix[14]) - 1.0f) < 0.05f && std::fabs(matrix[15]) < 0.05f;
        if (!plausibleScale || (!perspectiveRow && !perspectiveColumn)) continue;
        std::wostringstream line;
        line << std::fixed << std::setprecision(5) << L"Projection candidate +0x" << std::hex << offset << std::dec << L":";
        for (const float value : matrix) line << L" " << value;
        Log(line.str());
        ++candidates;
    }
    unsigned cameraCandidates = 0;
    const auto cameraState = reinterpret_cast<void*>(lastCamera);
    if (lastCamera != 0 && IsReadableAddress(cameraState, kProbeBytes)) {
        for (std::size_t offset = 0; offset + sizeof(float) * 16 <= kProbeBytes && cameraCandidates < 10; offset += 16) {
            std::array<float, 16> matrix{};
            std::memcpy(matrix.data(), reinterpret_cast<const std::byte*>(cameraState) + offset, sizeof(matrix));
            bool finite = true;
            for (const float value : matrix) finite = finite && std::isfinite(value);
            if (!finite) continue;
            const bool plausibleScale = std::fabs(matrix[0]) > 0.05f && std::fabs(matrix[0]) < 10.0f &&
                                        std::fabs(matrix[5]) > 0.05f && std::fabs(matrix[5]) < 10.0f;
            const bool perspectiveRow = std::fabs(std::fabs(matrix[11]) - 1.0f) < 0.05f && std::fabs(matrix[15]) < 0.05f;
            const bool perspectiveColumn = std::fabs(std::fabs(matrix[14]) - 1.0f) < 0.05f && std::fabs(matrix[15]) < 0.05f;
            if (!plausibleScale || (!perspectiveRow && !perspectiveColumn)) continue;
            std::wostringstream line;
            line << std::fixed << std::setprecision(5) << L"Camera projection candidate +0x" << std::hex << offset << std::dec << L":";
            for (const float value : matrix) line << L" " << value;
            Log(line.str());
            ++cameraCandidates;
        }
    }
    Log(L"FOV/camera state probe complete: " + std::to_wstring(candidates) + L" FOV-state and " +
        std::to_wstring(cameraCandidates) + L" camera-state projection-shaped matrices logged.");
}

void TryLogFovStateProjectionCandidates() {
    const auto fovState = gLastFovState.load();
    const auto cameraState = gLastCameraState.load();
    if (fovState == 0 || cameraState == 0) return;
    bool expected = false;
    if (gFirstFovCallbackLogged.compare_exchange_strong(expected, true)) {
        LogFovStateProjectionCandidates(reinterpret_cast<void*>(fovState));
    }
}

void CameraUpdateDetour(void* cameraState, const float* transform) {
    // The normal path remains completely untouched until F8 is enabled.
    if (cameraState != nullptr && transform != nullptr && !gFirstCallbackLogged.exchange(true)) {
        LogCapturedCameraCallback(cameraState, transform);
    }
    if (cameraState != nullptr) {
        gLastCameraState = reinterpret_cast<std::uintptr_t>(cameraState);
        TryLogFovStateProjectionCandidates();
    }
    if (gOriginalCameraUpdate == nullptr) return;
    if (transform == nullptr) {
        gOriginalCameraUpdate(cameraState, transform);
        return;
    }
    const bool useHeadPose = gHeadTrackingEnabled.load() && gHeadPoseReady.load();
    const bool cameraModified = useHeadPose || gCameraOffsetActive.load() ||
                                std::fabs(gStereoEyeOffsetRight.load()) > 0.00001f;
    if (!cameraModified) {
        gOriginalCameraUpdate(cameraState, transform);
        return;
    }

    // Keep Frostbite's own update and all its ancillary state intact. We change
    // only the incoming camera transform that it receives for this frame.
    alignas(16) std::array<float, kTransformFloatCount> adjusted{};
    std::memcpy(adjusted.data(), transform, sizeof(adjusted));
    ApplyHeadPoseToCameraTransform(adjusted.data(), useHeadPose);
    if (gCullTransformSplitEnabled.load() && cameraState != nullptr) {
        // The profiled callback writes RDX into two consecutive camera
        // matrices: [RCX+0x10] and [RCX+0x50]. Preserve the second (engine)
        // matrix and update only the first (render) matrix after the callback.
        // This keeps the vehicle culling and LOD decision at the stock camera
        // while the render matrix follows the driver's HMD pose.
        gOriginalCameraUpdate(cameraState, transform);
        auto* const renderTransform = static_cast<std::byte*>(cameraState) + 0x10;
        if (IsWritableAddress(renderTransform, sizeof(adjusted))) {
            std::memcpy(renderTransform, adjusted.data(), sizeof(adjusted));
        } else {
            Log(L"Cull-transform split skipped: render camera matrix is not writable.");
        }
        return;
    }
    gOriginalCameraUpdate(cameraState, adjusted.data());
}

void FovUpdateDetour(void* fovState) {
    // Keep every stock Frostbite update intact, then replace just the final
    // FOV value.  This mirrors the proven Camera Toolkit call order.
    if (gOriginalFovUpdate != nullptr) gOriginalFovUpdate(fovState);
    if (fovState != nullptr) {
        gLastFovState = reinterpret_cast<std::uintptr_t>(fovState);
        TryLogFovStateProjectionCandidates();
    }
    if (!gEngineFovOverrideEnabled.load() || fovState == nullptr) return;
    auto* const fovValue = static_cast<std::byte*>(fovState) + 0x1168;
    if (!IsWritableAddress(fovValue, sizeof(float))) return;
    const float radians = gEngineVerticalFovRadians.load();
    if (std::isfinite(radians)) std::memcpy(fovValue, &radians, sizeof(radians));
}

bool IsExpectedHeatImage(const HMODULE gameModule) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(gameModule);
    if (dos == nullptr || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const std::byte*>(gameModule) + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE && nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
           kCameraUpdateCallbackRva < nt->OptionalHeader.SizeOfImage &&
           kFovUpdateCallbackRva < nt->OptionalHeader.SizeOfImage;
}

} // namespace

void InstallFrostbiteCameraObserver() {
    if (gObserverInstalled.exchange(true)) return;

    const HMODULE gameModule = GetModuleHandleW(nullptr);
    if (gameModule == nullptr || !IsExpectedHeatImage(gameModule)) {
        gObserverInstalled = false;
        Log(L"Heat camera observer was not installed: executable does not match the 1.0.60.7040 profile.");
        return;
    }

    gCameraUpdateTarget = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(gameModule) + kCameraUpdateCallbackRva);
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(gCameraUpdateTarget, &memory, sizeof(memory)) == 0 || memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        gObserverInstalled = false;
        Log(L"Heat camera observer was not installed: callback address is unavailable.");
        return;
    }
    const MH_STATUS created = MH_CreateHook(gCameraUpdateTarget, reinterpret_cast<LPVOID>(&CameraUpdateDetour),
                                            reinterpret_cast<LPVOID*>(&gOriginalCameraUpdate));
    if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED) {
        gObserverInstalled = false;
        Log(L"Heat camera observer hook creation failed (MinHook " + std::to_wstring(created) + L").");
        return;
    }
    const MH_STATUS enabled = MH_EnableHook(gCameraUpdateTarget);
    if (enabled != MH_OK && enabled != MH_ERROR_ENABLED) {
        MH_RemoveHook(gCameraUpdateTarget);
        gOriginalCameraUpdate = nullptr;
        gObserverInstalled = false;
        Log(L"Heat camera observer hook enable failed (MinHook " + std::to_wstring(enabled) + L").");
        return;
    }
    Log(L"Heat camera observer installed at callback RVA 0x8DB7DD0 (observation only).");

    // Do not contend with the original third-party tool if it was manually
    // loaded into this process.  The built-in hook is otherwise self-contained.
    if (GetModuleHandleW(L"CamToolKitV2.dll") != nullptr) {
        Log(L"Engine FOV hook skipped because Camera Toolkit V2 is already loaded.");
        return;
    }
    const HMODULE module = GetModuleHandleW(nullptr);
    gFovUpdateTarget = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(module) + kFovUpdateCallbackRva);
    MEMORY_BASIC_INFORMATION fovMemory{};
    if (VirtualQuery(gFovUpdateTarget, &fovMemory, sizeof(fovMemory)) == 0 || fovMemory.State != MEM_COMMIT ||
        (fovMemory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        gFovUpdateTarget = nullptr;
        Log(L"Engine FOV hook was not installed: callback address is unavailable.");
        return;
    }
    const MH_STATUS fovCreated = MH_CreateHook(gFovUpdateTarget, reinterpret_cast<LPVOID>(&FovUpdateDetour),
                                               reinterpret_cast<LPVOID*>(&gOriginalFovUpdate));
    if (fovCreated != MH_OK && fovCreated != MH_ERROR_ALREADY_CREATED) {
        gFovUpdateTarget = nullptr;
        Log(L"Engine FOV hook creation failed (MinHook " + std::to_wstring(fovCreated) + L").");
        return;
    }
    const MH_STATUS fovEnabled = MH_EnableHook(gFovUpdateTarget);
    if (fovEnabled != MH_OK && fovEnabled != MH_ERROR_ENABLED) {
        MH_RemoveHook(gFovUpdateTarget);
        gFovUpdateTarget = nullptr;
        gOriginalFovUpdate = nullptr;
        Log(L"Engine FOV hook enable failed (MinHook " + std::to_wstring(fovEnabled) + L").");
        return;
    }
    gFovHookInstalled = true;
    Log(L"Heat engine FOV hook installed at callback RVA 0x88450D0.");
}

void RemoveFrostbiteCameraObserver() {
    if (!gObserverInstalled.exchange(false)) return;
    if (gFovHookInstalled.exchange(false) && gFovUpdateTarget != nullptr) {
        MH_DisableHook(gFovUpdateTarget);
        MH_RemoveHook(gFovUpdateTarget);
    }
    gFovUpdateTarget = nullptr;
    gOriginalFovUpdate = nullptr;
    if (gCameraUpdateTarget != nullptr) {
        MH_DisableHook(gCameraUpdateTarget);
        MH_RemoveHook(gCameraUpdateTarget);
    }
    gCameraUpdateTarget = nullptr;
    gOriginalCameraUpdate = nullptr;
}

void SetFrostbiteHeadTrackingEnabled(const bool enabled) {
    gHeadTrackingEnabled = enabled;
    if (!enabled) gHeadPoseReady = false;
}

void SetFrostbiteHeadTrackingTuning(const float rotationGain, const float translationScale) {
    gHeadRotationGain = Clamp(rotationGain, 0.5f, 2.5f);
    gHeadTranslationScale = Clamp(translationScale, 0.0f, 2.0f);
}

void SetFrostbiteCameraOffset(const float rightMetres, const float upMetres, const float forwardMetres) {
    gCameraOffsetRight = Clamp(rightMetres, -1.0f, 1.0f);
    gCameraOffsetUp = Clamp(upMetres, -1.0f, 1.0f);
    gCameraOffsetForward = Clamp(forwardMetres, -1.0f, 1.0f);
    gCameraOffsetActive = std::fabs(gCameraOffsetRight.load()) > 0.0001f ||
                          std::fabs(gCameraOffsetUp.load()) > 0.0001f ||
                           std::fabs(gCameraOffsetForward.load()) > 0.0001f;
}

void SetFrostbiteEngineFovOverride(const bool enabled, const float verticalDegrees) {
    constexpr float kRadiansPerDegree = 0.01745329251994329577f;
    gEngineVerticalFovRadians = Clamp(verticalDegrees, 60.0f, 150.0f) * kRadiansPerDegree;
    gEngineFovOverrideEnabled = enabled;
}

void SetFrostbiteStereoEyeOffset(const float rightMetres) {
    gStereoEyeOffsetRight = Clamp(rightMetres, -0.10f, 0.10f);
}

void SetFrostbiteCullTransformSplitEnabled(const bool enabled) {
    gCullTransformSplitEnabled = enabled;
}

void UpdateFrostbiteHeadPose(const float orientationX, const float orientationY, const float orientationZ, const float orientationW,
                             const float positionX, const float positionY, const float positionZ, const bool recenter) {
    const Quaternion current = Normalise({orientationX, orientationY, orientationZ, orientationW});
    const Vector3 currentPosition{positionX, positionY, positionZ};
    if (recenter || !gHaveHeadCentre) {
        // Keep the complete HMD orientation as the relative-pose origin.  A
        // yaw/pitch-only centre here is subtly wrong: subtracting it from a
        // full current pose leaves the current roll behind as an immediate
        // rotation.  On canted headsets that residual can be close to 90°.
        gHeadCentre = current;
        gHeadCentrePosition = currentPosition;
        gHaveHeadCentre = true;
    }

    // Centre first, then discard the *relative* roll.  Recentring while the
    // user happens to lean their head is now safe: straightening afterwards
    // cannot make the game horizon tilt, while yaw and pitch still work.
    const Quaternion localOrientation = WithoutRoll(Normalise(Multiply(Inverse(gHeadCentre), current)));
    const Vector3 trackingDelta{currentPosition.x - gHeadCentrePosition.x, currentPosition.y - gHeadCentrePosition.y,
                                currentPosition.z - gHeadCentrePosition.z};
    const Vector3 localPosition = Rotate(Inverse(gHeadCentre), trackingDelta);
    gRelativeOrientationX = localOrientation.x;
    gRelativeOrientationY = localOrientation.y;
    gRelativeOrientationZ = localOrientation.z;
    gRelativeOrientationW = localOrientation.w;
    gRelativePositionX = localPosition.x;
    gRelativePositionY = localPosition.y;
    gRelativePositionZ = localPosition.z;
    gHeadPoseReady = true;
}

} // namespace nfsheatvr
