#include "input_bridge.hpp"

#include "logger.hpp"

#include <MinHook.h>

#include <Xinput.h>

#include <atomic>

namespace nfsheatvr {
namespace {
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

GetRawInputDataFn gGetRawInputData = nullptr;
XInputGetStateFn gXInputGetState = nullptr;
XInputGetStateFn gXInputGetStateEx = nullptr;
std::atomic<LONG> gPendingDeltaX{};
std::atomic<LONG> gPendingDeltaY{};
std::atomic_int gHeadStickX{};
std::atomic_int gHeadStickY{};
std::atomic_uint gSyntheticPacket{};
std::atomic<ULONGLONG> gNextXInputAttempt{};
std::atomic_bool gInstalled{};
std::atomic_bool gXInputInstalled{};
std::atomic_bool gFirstDeliveryLogged{};
std::atomic_bool gFirstGamepadDeliveryLogged{};

UINT WINAPI HookGetRawInputData(HRAWINPUT rawInput, UINT command, LPVOID data, PUINT size, UINT headerSize) {
    const UINT result = gGetRawInputData != nullptr ? gGetRawInputData(rawInput, command, data, size, headerSize) : static_cast<UINT>(-1);
    if (result == static_cast<UINT>(-1) || command != RID_INPUT || data == nullptr ||
        result < sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE)) {
        return result;
    }

    auto* input = static_cast<RAWINPUT*>(data);
    if (input->header.dwType != RIM_TYPEMOUSE) return result;
    const LONG deltaX = gPendingDeltaX.exchange(0);
    const LONG deltaY = gPendingDeltaY.exchange(0);
    if (deltaX == 0 && deltaY == 0) return result;

    input->data.mouse.lLastX += deltaX;
    input->data.mouse.lLastY += deltaY;
    if (!gFirstDeliveryLogged.exchange(true)) {
        Log(L"Raw Input bridge delivered head-look delta: dx=" + std::to_wstring(deltaX) +
            L", dy=" + std::to_wstring(deltaY) + L".");
    }
    return result;
}

DWORD ApplyHeadLookGamepadState(const DWORD result, XINPUT_STATE* state) {
    const int rightX = gHeadStickX.load();
    const int rightY = gHeadStickY.load();
    if ((rightX == 0 && rightY == 0) || state == nullptr) return result;

    // NFS Heat can run exclusively on keyboard/mouse. While head-look is
    // actually deflected, expose a neutral virtual XInput device and overlay
    // only its right stick. No controller is reported while centred or F8 is off.
    if (result != ERROR_SUCCESS) *state = XINPUT_STATE{};
    state->dwPacketNumber += gSyntheticPacket.fetch_add(1) + 1;
    state->Gamepad.sThumbRX = static_cast<SHORT>(rightX);
    state->Gamepad.sThumbRY = static_cast<SHORT>(rightY);
    if (!gFirstGamepadDeliveryLogged.exchange(true)) {
        Log(L"XInput bridge delivered head-look right stick: x=" + std::to_wstring(rightX) +
            L", y=" + std::to_wstring(rightY) + L".");
    }
    return ERROR_SUCCESS;
}

DWORD WINAPI HookXInputGetState(DWORD userIndex, XINPUT_STATE* state) {
    const DWORD result = gXInputGetState != nullptr ? gXInputGetState(userIndex, state) : ERROR_DEVICE_NOT_CONNECTED;
    return ApplyHeadLookGamepadState(result, state);
}

// XInputGetStateEx is the widely used, undocumented ordinal-100 variant. It
// has the same parameters as XInputGetState and also reports the Guide button.
DWORD WINAPI HookXInputGetStateEx(DWORD userIndex, XINPUT_STATE* state) {
    const DWORD result = gXInputGetStateEx != nullptr ? gXInputGetStateEx(userIndex, state) : ERROR_DEVICE_NOT_CONNECTED;
    return ApplyHeadLookGamepadState(result, state);
}

bool InstallXInputFunction(const FARPROC target, LPVOID detour, LPVOID* original, const wchar_t* name) {
    if (target == nullptr) return false;
    const MH_STATUS created = MH_CreateHook(reinterpret_cast<LPVOID>(target), detour, original);
    if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED) {
        Log(L"Could not create " + std::wstring(name) + L" head-look bridge (MinHook " + std::to_wstring(created) + L").");
        return false;
    }
    const MH_STATUS enabled = MH_EnableHook(reinterpret_cast<LPVOID>(target));
    if (enabled != MH_OK && enabled != MH_ERROR_ENABLED) {
        Log(L"Could not enable " + std::wstring(name) + L" head-look bridge (MinHook " + std::to_wstring(enabled) + L").");
        return false;
    }
    return true;
}

bool InstallXInputBridge(const bool logUnavailable) {
    if (gXInputInstalled) return true;
    const HMODULE xinput = GetModuleHandleW(L"xinput1_4.dll");
    if (xinput == nullptr) {
        if (logUnavailable) Log(L"XInput head-look bridge not loaded yet; it will retry after the game loads it.");
        return false;
    }
    const bool standard = InstallXInputFunction(GetProcAddress(xinput, "XInputGetState"),
                                                reinterpret_cast<LPVOID>(&HookXInputGetState),
                                                reinterpret_cast<LPVOID*>(&gXInputGetState), L"XInputGetState");
    const bool extended = InstallXInputFunction(GetProcAddress(xinput, MAKEINTRESOURCEA(100)),
                                                reinterpret_cast<LPVOID>(&HookXInputGetStateEx),
                                                reinterpret_cast<LPVOID*>(&gXInputGetStateEx), L"XInputGetStateEx");
    if (!standard && !extended) {
        if (logUnavailable) Log(L"No usable XInput state function was found yet; it will retry.");
        return false;
    }
    gXInputInstalled = true;
    Log(L"XInput head-look bridge installed (standard=" + std::to_wstring(standard) +
        L", extended=" + std::to_wstring(extended) + L").");
    return true;
}
} // namespace

bool InstallInputBridge() {
    if (gInstalled) return true;
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const FARPROC target = user32 == nullptr ? nullptr : GetProcAddress(user32, "GetRawInputData");
    if (target == nullptr) {
        Log(L"Raw Input bridge unavailable: user32!GetRawInputData was not loaded.");
        return false;
    }
    const MH_STATUS created = MH_CreateHook(reinterpret_cast<LPVOID>(target), reinterpret_cast<LPVOID>(&HookGetRawInputData),
                                            reinterpret_cast<LPVOID*>(&gGetRawInputData));
    if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED) {
        Log(L"Could not create Raw Input bridge (MinHook " + std::to_wstring(created) + L").");
        return false;
    }
    const MH_STATUS enabled = MH_EnableHook(reinterpret_cast<LPVOID>(target));
    if (enabled != MH_OK && enabled != MH_ERROR_ENABLED) {
        Log(L"Could not enable Raw Input bridge (MinHook " + std::to_wstring(enabled) + L").");
        return false;
    }
    gInstalled = true;
    Log(L"Raw Input bridge installed.");
    InstallXInputBridge(true);
    return true;
}

void RemoveInputBridge() {
    // RemoveDxgiHooks disables every MinHook target before uninitialising the
    // library; this bridge has no independent target lifetime.
    gInstalled = false;
    gXInputInstalled = false;
    ClearHeadLookRawDeltas();
    ClearHeadLookGamepadStick();
}

void QueueHeadLookRawDelta(const LONG deltaX, const LONG deltaY) {
    gPendingDeltaX.fetch_add(deltaX);
    gPendingDeltaY.fetch_add(deltaY);
}

void ClearHeadLookRawDeltas() {
    gPendingDeltaX = 0;
    gPendingDeltaY = 0;
}

void SetHeadLookGamepadStick(const SHORT rightX, const SHORT rightY) {
    gHeadStickX = rightX;
    gHeadStickY = rightY;
    if ((rightX != 0 || rightY != 0) && !gXInputInstalled) {
        const ULONGLONG now = GetTickCount64();
        ULONGLONG retryAt = gNextXInputAttempt.load();
        if (now >= retryAt && gNextXInputAttempt.compare_exchange_strong(retryAt, now + 1000)) {
            InstallXInputBridge(false);
        }
    }
}

void ClearHeadLookGamepadStick() {
    gHeadStickX = 0;
    gHeadStickY = 0;
}

} // namespace nfsheatvr
