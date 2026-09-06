#include "virtual_display.hpp"

#include "logger.hpp"
#include "vr_settings.hpp"

#include <MinHook.h>
#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <intrin.h>
#include <mutex>
#include <string>

namespace nfsheatvr {
namespace {

using GetSystemMetricsFn = int(WINAPI*)(int);
using GetSystemMetricsForDpiFn = int(WINAPI*)(int, UINT);
using GetDeviceCapsFn = int(WINAPI*)(HDC, int);
using EnumDisplaySettingsWFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DEVMODEW*);
using EnumDisplaySettingsAFn = BOOL(WINAPI*)(LPCSTR, DWORD, DEVMODEA*);
using EnumDisplaySettingsExWFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DEVMODEW*, DWORD);
using EnumDisplaySettingsExAFn = BOOL(WINAPI*)(LPCSTR, DWORD, DEVMODEA*, DWORD);
using GetMonitorInfoWFn = BOOL(WINAPI*)(HMONITOR, LPMONITORINFO);
using GetMonitorInfoAFn = BOOL(WINAPI*)(HMONITOR, LPMONITORINFO);
using ChangeDisplaySettingsWFn = LONG(WINAPI*)(DEVMODEW*, DWORD);
using ChangeDisplaySettingsAFn = LONG(WINAPI*)(DEVMODEA*, DWORD);
using ChangeDisplaySettingsExWFn = LONG(WINAPI*)(LPCWSTR, DEVMODEW*, HWND, DWORD, LPVOID);
using ChangeDisplaySettingsExAFn = LONG(WINAPI*)(LPCSTR, DEVMODEA*, HWND, DWORD, LPVOID);

GetSystemMetricsFn gGetSystemMetrics = nullptr;
GetSystemMetricsForDpiFn gGetSystemMetricsForDpi = nullptr;
GetDeviceCapsFn gGetDeviceCaps = nullptr;
EnumDisplaySettingsWFn gEnumDisplaySettingsW = nullptr;
EnumDisplaySettingsAFn gEnumDisplaySettingsA = nullptr;
EnumDisplaySettingsExWFn gEnumDisplaySettingsExW = nullptr;
EnumDisplaySettingsExAFn gEnumDisplaySettingsExA = nullptr;
GetMonitorInfoWFn gGetMonitorInfoW = nullptr;
GetMonitorInfoAFn gGetMonitorInfoA = nullptr;
ChangeDisplaySettingsWFn gChangeDisplaySettingsW = nullptr;
ChangeDisplaySettingsAFn gChangeDisplaySettingsA = nullptr;
ChangeDisplaySettingsExWFn gChangeDisplaySettingsExW = nullptr;
ChangeDisplaySettingsExAFn gChangeDisplaySettingsExA = nullptr;

std::atomic_bool gInstalled = false;
std::atomic_bool gLoggedMetrics = false;
std::atomic_bool gLoggedModes = false;
std::atomic_bool gLoggedMonitor = false;
std::atomic_bool gLoggedModeApply = false;
std::mutex gInstallMutex;
int gWidth = 3840;
int gHeight = 2160;

bool IsHeatCall() {
    const auto address = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto base = reinterpret_cast<const std::byte*>(GetModuleHandleW(nullptr));
    if (base == nullptr) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(base);
    const auto end = begin + nt->OptionalHeader.SizeOfImage;
    return address >= begin && address < end;
}

void LogOnce(std::atomic_bool& gate, const wchar_t* source) {
    if (!gate.exchange(true)) {
        Log(std::wstring(L"Heat-only virtual display supplied through ") + source + L": " +
            std::to_wstring(gWidth) + L"x" + std::to_wstring(gHeight) + L".");
    }
}

bool IsHorizontalMetric(const int index) {
    return index == SM_CXSCREEN || index == SM_CXVIRTUALSCREEN;
}

bool IsVerticalMetric(const int index) {
    return index == SM_CYSCREEN || index == SM_CYVIRTUALSCREEN;
}

int WINAPI HookGetSystemMetrics(const int index) {
    if (IsHeatCall()) {
        if (IsHorizontalMetric(index)) {
            LogOnce(gLoggedMetrics, L"GetSystemMetrics");
            return gWidth;
        }
        if (IsVerticalMetric(index)) {
            LogOnce(gLoggedMetrics, L"GetSystemMetrics");
            return gHeight;
        }
    }
    return gGetSystemMetrics != nullptr ? gGetSystemMetrics(index) : 0;
}

int WINAPI HookGetSystemMetricsForDpi(const int index, const UINT dpi) {
    if (IsHeatCall()) {
        if (IsHorizontalMetric(index)) {
            LogOnce(gLoggedMetrics, L"GetSystemMetricsForDpi");
            return gWidth;
        }
        if (IsVerticalMetric(index)) {
            LogOnce(gLoggedMetrics, L"GetSystemMetricsForDpi");
            return gHeight;
        }
    }
    return gGetSystemMetricsForDpi != nullptr ? gGetSystemMetricsForDpi(index, dpi) : 0;
}

int WINAPI HookGetDeviceCaps(HDC dc, const int index) {
    if (IsHeatCall()) {
        if (index == HORZRES || index == DESKTOPHORZRES) {
            LogOnce(gLoggedMetrics, L"GetDeviceCaps");
            return gWidth;
        }
        if (index == VERTRES || index == DESKTOPVERTRES) {
            LogOnce(gLoggedMetrics, L"GetDeviceCaps");
            return gHeight;
        }
    }
    return gGetDeviceCaps != nullptr ? gGetDeviceCaps(dc, index) : 0;
}

template <typename DevMode>
void RewriteDisplayMode(DevMode* mode) {
    if (mode == nullptr) return;
    mode->dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT;
    mode->dmPelsWidth = static_cast<DWORD>(gWidth);
    mode->dmPelsHeight = static_cast<DWORD>(gHeight);
    mode->dmBitsPerPel = 32;
}

BOOL WINAPI HookEnumDisplaySettingsW(LPCWSTR device, DWORD modeNumber, DEVMODEW* mode) {
    const BOOL result = gEnumDisplaySettingsW != nullptr ? gEnumDisplaySettingsW(device, modeNumber, mode) : FALSE;
    if (result && IsHeatCall()) {
        RewriteDisplayMode(mode);
        LogOnce(gLoggedModes, L"EnumDisplaySettingsW");
    }
    return result;
}

BOOL WINAPI HookEnumDisplaySettingsA(LPCSTR device, DWORD modeNumber, DEVMODEA* mode) {
    const BOOL result = gEnumDisplaySettingsA != nullptr ? gEnumDisplaySettingsA(device, modeNumber, mode) : FALSE;
    if (result && IsHeatCall()) {
        RewriteDisplayMode(mode);
        LogOnce(gLoggedModes, L"EnumDisplaySettingsA");
    }
    return result;
}

BOOL WINAPI HookEnumDisplaySettingsExW(LPCWSTR device, DWORD modeNumber, DEVMODEW* mode, DWORD flags) {
    const BOOL result = gEnumDisplaySettingsExW != nullptr ? gEnumDisplaySettingsExW(device, modeNumber, mode, flags) : FALSE;
    if (result && IsHeatCall()) {
        RewriteDisplayMode(mode);
        LogOnce(gLoggedModes, L"EnumDisplaySettingsExW");
    }
    return result;
}

BOOL WINAPI HookEnumDisplaySettingsExA(LPCSTR device, DWORD modeNumber, DEVMODEA* mode, DWORD flags) {
    const BOOL result = gEnumDisplaySettingsExA != nullptr ? gEnumDisplaySettingsExA(device, modeNumber, mode, flags) : FALSE;
    if (result && IsHeatCall()) {
        RewriteDisplayMode(mode);
        LogOnce(gLoggedModes, L"EnumDisplaySettingsExA");
    }
    return result;
}

template <typename MonitorInfo>
void RewriteMonitorBounds(MonitorInfo* info) {
    if (info == nullptr) return;
    info->rcMonitor.right = info->rcMonitor.left + gWidth;
    info->rcMonitor.bottom = info->rcMonitor.top + gHeight;
    info->rcWork.right = info->rcWork.left + gWidth;
    info->rcWork.bottom = info->rcWork.top + gHeight;
}

BOOL WINAPI HookGetMonitorInfoW(HMONITOR monitor, LPMONITORINFO info) {
    const BOOL result = gGetMonitorInfoW != nullptr ? gGetMonitorInfoW(monitor, info) : FALSE;
    if (result && IsHeatCall()) {
        RewriteMonitorBounds(info);
        LogOnce(gLoggedMonitor, L"GetMonitorInfoW");
    }
    return result;
}

BOOL WINAPI HookGetMonitorInfoA(HMONITOR monitor, LPMONITORINFO info) {
    const BOOL result = gGetMonitorInfoA != nullptr ? gGetMonitorInfoA(monitor, info) : FALSE;
    if (result && IsHeatCall()) {
        RewriteMonitorBounds(info);
        LogOnce(gLoggedMonitor, L"GetMonitorInfoA");
    }
    return result;
}

// Frostbite validates the mode returned by EnumDisplaySettings before creating
// the renderer. The physical driver would reject that Heat-only mode, so the
// validation must also succeed locally. We never call the real mode-setting API
// on this path: the Windows desktop remains exactly as it was.
LONG WINAPI HookChangeDisplaySettingsW(DEVMODEW* mode, DWORD flags) {
    if (IsHeatCall()) {
        RewriteDisplayMode(mode);
        LogOnce(gLoggedModeApply, L"ChangeDisplaySettingsW");
        return DISP_CHANGE_SUCCESSFUL;
    }
    return gChangeDisplaySettingsW != nullptr ? gChangeDisplaySettingsW(mode, flags) : DISP_CHANGE_FAILED;
}

LONG WINAPI HookChangeDisplaySettingsA(DEVMODEA* mode, DWORD flags) {
    if (IsHeatCall()) {
        RewriteDisplayMode(mode);
        LogOnce(gLoggedModeApply, L"ChangeDisplaySettingsA");
        return DISP_CHANGE_SUCCESSFUL;
    }
    return gChangeDisplaySettingsA != nullptr ? gChangeDisplaySettingsA(mode, flags) : DISP_CHANGE_FAILED;
}

LONG WINAPI HookChangeDisplaySettingsExW(LPCWSTR device, DEVMODEW* mode, HWND window, DWORD flags, LPVOID parameters) {
    if (IsHeatCall()) {
        RewriteDisplayMode(mode);
        LogOnce(gLoggedModeApply, L"ChangeDisplaySettingsExW");
        return DISP_CHANGE_SUCCESSFUL;
    }
    return gChangeDisplaySettingsExW != nullptr
        ? gChangeDisplaySettingsExW(device, mode, window, flags, parameters) : DISP_CHANGE_FAILED;
}

LONG WINAPI HookChangeDisplaySettingsExA(LPCSTR device, DEVMODEA* mode, HWND window, DWORD flags, LPVOID parameters) {
    if (IsHeatCall()) {
        RewriteDisplayMode(mode);
        LogOnce(gLoggedModeApply, L"ChangeDisplaySettingsExA");
        return DISP_CHANGE_SUCCESSFUL;
    }
    return gChangeDisplaySettingsExA != nullptr
        ? gChangeDisplaySettingsExA(device, mode, window, flags, parameters) : DISP_CHANGE_FAILED;
}

template <typename Function>
bool InstallHook(const HMODULE module, const char* exportName, const Function detour, Function* original) {
    if (module == nullptr) return false;
    const auto target = GetProcAddress(module, exportName);
    if (target == nullptr) return false;
    const MH_STATUS created = MH_CreateHook(target, reinterpret_cast<LPVOID>(detour), reinterpret_cast<LPVOID*>(original));
    if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED) return false;
    const MH_STATUS enabled = MH_EnableHook(target);
    return enabled == MH_OK || enabled == MH_ERROR_ENABLED;
}

} // namespace

bool InstallHeatVirtualDisplay() {
    std::scoped_lock lock(gInstallMutex);
    if (gInstalled) return true;

    const VrSettings settings = LoadVrSettings();
    if (settings.processVirtualDisplayEnabled == 0) {
        Log(L"Heat-only virtual display is disabled in NFSHeatVR.ini.");
        return true;
    }
    gWidth = settings.gameResolutionWidth;
    gHeight = settings.gameResolutionHeight;

    const MH_STATUS initialised = MH_Initialize();
    if (initialised != MH_OK && initialised != MH_ERROR_ALREADY_INITIALIZED) {
        Log(L"MinHook initialisation failed before Heat virtual display setup.");
        return false;
    }

    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const HMODULE gdi32 = GetModuleHandleW(L"gdi32.dll");
    bool hooked = false;
    hooked |= InstallHook(user32, "GetSystemMetrics", &HookGetSystemMetrics, &gGetSystemMetrics);
    hooked |= InstallHook(user32, "GetSystemMetricsForDpi", &HookGetSystemMetricsForDpi, &gGetSystemMetricsForDpi);
    hooked |= InstallHook(gdi32, "GetDeviceCaps", &HookGetDeviceCaps, &gGetDeviceCaps);
    hooked |= InstallHook(user32, "EnumDisplaySettingsW", &HookEnumDisplaySettingsW, &gEnumDisplaySettingsW);
    hooked |= InstallHook(user32, "EnumDisplaySettingsA", &HookEnumDisplaySettingsA, &gEnumDisplaySettingsA);
    hooked |= InstallHook(user32, "EnumDisplaySettingsExW", &HookEnumDisplaySettingsExW, &gEnumDisplaySettingsExW);
    hooked |= InstallHook(user32, "EnumDisplaySettingsExA", &HookEnumDisplaySettingsExA, &gEnumDisplaySettingsExA);
    hooked |= InstallHook(user32, "GetMonitorInfoW", &HookGetMonitorInfoW, &gGetMonitorInfoW);
    hooked |= InstallHook(user32, "GetMonitorInfoA", &HookGetMonitorInfoA, &gGetMonitorInfoA);
    hooked |= InstallHook(user32, "ChangeDisplaySettingsW", &HookChangeDisplaySettingsW, &gChangeDisplaySettingsW);
    hooked |= InstallHook(user32, "ChangeDisplaySettingsA", &HookChangeDisplaySettingsA, &gChangeDisplaySettingsA);
    hooked |= InstallHook(user32, "ChangeDisplaySettingsExW", &HookChangeDisplaySettingsExW, &gChangeDisplaySettingsExW);
    hooked |= InstallHook(user32, "ChangeDisplaySettingsExA", &HookChangeDisplaySettingsExA, &gChangeDisplaySettingsExA);
    gInstalled = hooked;
    if (hooked) {
        Log(L"Heat-only virtual display armed for " + std::to_wstring(gWidth) + L"x" + std::to_wstring(gHeight) +
            L"; the Windows desktop and monitor configuration are unchanged.");
    } else {
        Log(L"No Heat virtual-display API hooks could be installed.");
    }
    return hooked;
}

bool GetHeatVirtualDisplayResolution(unsigned* width, unsigned* height) {
    if (!gInstalled || width == nullptr || height == nullptr) return false;
    *width = static_cast<unsigned>(gWidth);
    *height = static_cast<unsigned>(gHeight);
    return true;
}

} // namespace nfsheatvr
