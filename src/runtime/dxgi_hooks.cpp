#include "dxgi_hooks.hpp"

#include "logger.hpp"
#include "camera_probe.hpp"
#include "depth_observer.hpp"
#include "frostbite_camera.hpp"
#include "input_bridge.hpp"
#include "virtual_display.hpp"
#include "vr_presenter.hpp"

#include <MinHook.h>
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <atomic>
#include <memory>
#include <mutex>

namespace nfsheatvr {
namespace {
using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
using CreateFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using CreateSwapChainForCoreWindowFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using CreateSwapChainForCompositionFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using EnumAdaptersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, UINT, IDXGIAdapter**);
using EnumAdapters1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory1*, UINT, IDXGIAdapter1**);
using EnumOutputsFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIAdapter*, UINT, IDXGIOutput**);
using GetOutputDescFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput*, DXGI_OUTPUT_DESC*);
using GetDisplayModeListFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput*, DXGI_FORMAT, UINT, UINT*, DXGI_MODE_DESC*);
using FindClosestMatchingModeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput*, const DXGI_MODE_DESC*, DXGI_MODE_DESC*, IUnknown*);
using SetFullscreenStateFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
using ResizeTargetFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, const DXGI_MODE_DESC*);

CreateFactoryFn gCreateFactory = nullptr;
CreateFactoryFn gCreateFactory1 = nullptr;
CreateFactory2Fn gCreateFactory2 = nullptr;
CreateSwapChainFn gCreateSwapChain = nullptr;
CreateSwapChainForHwndFn gCreateSwapChainForHwnd = nullptr;
CreateSwapChainForCoreWindowFn gCreateSwapChainForCoreWindow = nullptr;
CreateSwapChainForCompositionFn gCreateSwapChainForComposition = nullptr;
PresentFn gPresent = nullptr;
EnumAdaptersFn gEnumAdapters = nullptr;
EnumAdapters1Fn gEnumAdapters1 = nullptr;
EnumOutputsFn gEnumOutputs = nullptr;
GetOutputDescFn gGetOutputDesc = nullptr;
GetDisplayModeListFn gGetDisplayModeList = nullptr;
FindClosestMatchingModeFn gFindClosestMatchingMode = nullptr;
SetFullscreenStateFn gSetFullscreenState = nullptr;
ResizeTargetFn gResizeTarget = nullptr;

std::atomic_bool gInstalled = false;
std::mutex gHookMutex;
std::mutex gPresenterMutex;
std::unique_ptr<VrPresenter> gPresenter;
DWORD gNextInitialiseAttempt = 0;
std::atomic_bool gLoggedOutputDescription = false;
std::atomic_bool gLoggedOutputModeCount = false;
std::atomic_bool gLoggedOutputModeList = false;
std::atomic_bool gLoggedClosestMode = false;
std::atomic_bool gLoggedWindowedSwapChain = false;
std::atomic_bool gLoggedFullscreenBlock = false;
std::atomic_bool gUsePhysicalDxgiOutput = false;

bool GetHeatVirtualResolution(UINT* width, UINT* height) {
    unsigned configuredWidth = 0;
    unsigned configuredHeight = 0;
    if (!GetHeatVirtualDisplayResolution(&configuredWidth, &configuredHeight)) return false;
    if (width != nullptr) *width = static_cast<UINT>(configuredWidth);
    if (height != nullptr) *height = static_cast<UINT>(configuredHeight);
    return true;
}

bool GetHeatVirtualOutputResolution(UINT* width, UINT* height) {
    return !gUsePhysicalDxgiOutput && GetHeatVirtualResolution(width, height);
}

void RestorePhysicalDxgiOutputAfterCreation(IDXGISwapChain* swapChain) {
    if (swapChain == nullptr || gUsePhysicalDxgiOutput) return;
    UINT virtualWidth = 0;
    UINT virtualHeight = 0;
    if (!GetHeatVirtualResolution(&virtualWidth, &virtualHeight)) return;
    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(swapChain->GetDesc(&description)) ||
        description.BufferDesc.Width < virtualWidth || description.BufferDesc.Height < virtualHeight) return;
    gUsePhysicalDxgiOutput = true;
    Log(L"Heat created its " + std::to_wstring(virtualWidth) + L"x" + std::to_wstring(virtualHeight) +
        L" render target; DXGI presentation is restored to the physical desktop.");
}

HRESULT STDMETHODCALLTYPE HookEnumOutputs(IDXGIAdapter* adapter, UINT index, IDXGIOutput** output);
HRESULT STDMETHODCALLTYPE HookGetOutputDesc(IDXGIOutput* output, DXGI_OUTPUT_DESC* description);
HRESULT STDMETHODCALLTYPE HookGetDisplayModeList(IDXGIOutput* output, DXGI_FORMAT format, UINT flags,
                                                  UINT* modeCount, DXGI_MODE_DESC* modes);
HRESULT STDMETHODCALLTYPE HookFindClosestMatchingMode(IDXGIOutput* output, const DXGI_MODE_DESC* requested,
                                                       DXGI_MODE_DESC* closest, IUnknown* device);
HRESULT STDMETHODCALLTYPE HookSetFullscreenState(IDXGISwapChain* swapChain, BOOL fullscreen, IDXGIOutput* output);
HRESULT STDMETHODCALLTYPE HookResizeTarget(IDXGISwapChain* swapChain, const DXGI_MODE_DESC* mode);

void LogVirtualDxgiOnce(std::atomic_bool& gate, const std::wstring& message) {
    if (!gate.exchange(true)) Log(message);
}

template <typename Function>
bool InstallHook(void* target, Function detour, Function* original, const wchar_t* name) {
    if (target == nullptr) return false;
    const MH_STATUS created = MH_CreateHook(target, reinterpret_cast<LPVOID>(detour), reinterpret_cast<LPVOID*>(original));
    if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED) {
        Log(std::wstring(L"Could not create ") + name + L" hook (MinHook " + std::to_wstring(created) + L").");
        return false;
    }
    const MH_STATUS enabled = MH_EnableHook(target);
    if (enabled != MH_OK && enabled != MH_ERROR_ENABLED) {
        Log(std::wstring(L"Could not enable ") + name + L" hook (MinHook " + std::to_wstring(enabled) + L").");
        return false;
    }
    return true;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    {
        std::scoped_lock lock(gPresenterMutex);
        if (!gPresenter) gPresenter = std::make_unique<VrPresenter>();
        if (!gPresenter->IsReady() && GetTickCount() >= gNextInitialiseAttempt) {
            if (!gPresenter->Initialise(swapChain)) {
                gNextInitialiseAttempt = GetTickCount() + 5000;
            }
        }
        if (gPresenter->IsReady()) gPresenter->PresentFrame(swapChain);
    }
    return gPresent != nullptr ? gPresent(swapChain, syncInterval, flags) : S_OK;
}

void ObserveSwapChainAndInstallPresent(IDXGISwapChain* swapChain) {
    if (swapChain == nullptr) return;
    auto** vtable = *reinterpret_cast<void***>(swapChain);
    if (InstallHook(vtable[8], &HookPresent, &gPresent, L"IDXGISwapChain::Present")) {
        Log(L"DXGI swap chain discovered; waiting for first VR frame.");
    }
    InstallHook(vtable[10], &HookSetFullscreenState, &gSetFullscreenState, L"IDXGISwapChain::SetFullscreenState");
    InstallHook(vtable[14], &HookResizeTarget, &gResizeTarget, L"IDXGISwapChain::ResizeTarget");
}

void ObserveDepthTextures(IUnknown* device) {
    if (device == nullptr) return;
    ID3D11Device* d3d11Device = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d3d11Device)))) {
        InstallDepthTextureObserver(d3d11Device);
        ID3D11DeviceContext* immediateContext = nullptr;
        d3d11Device->GetImmediateContext(&immediateContext);
        if (immediateContext != nullptr) {
            InstallColourPassObserver(immediateContext);
            immediateContext->Release();
        }
        d3d11Device->Release();
    }
}

void ObserveOutput(IDXGIOutput* output) {
    if (output == nullptr) return;
    auto** vtable = *reinterpret_cast<void***>(output);
    // IDXGIOutput starts after IDXGIObject. These calls are where Frostbite
    // obtains the physical driver mode list after Win32 has reported a mode.
    InstallHook(vtable[7], &HookGetOutputDesc, &gGetOutputDesc, L"IDXGIOutput::GetDesc");
    InstallHook(vtable[8], &HookGetDisplayModeList, &gGetDisplayModeList, L"IDXGIOutput::GetDisplayModeList");
    InstallHook(vtable[9], &HookFindClosestMatchingMode, &gFindClosestMatchingMode, L"IDXGIOutput::FindClosestMatchingMode");
}

void ObserveAdapter(IDXGIAdapter* adapter) {
    if (adapter == nullptr) return;
    auto** vtable = *reinterpret_cast<void***>(adapter);
    InstallHook(vtable[7], &HookEnumOutputs, &gEnumOutputs, L"IDXGIAdapter::EnumOutputs");
}

HRESULT STDMETHODCALLTYPE HookEnumOutputs(IDXGIAdapter* adapter, UINT index, IDXGIOutput** output) {
    const HRESULT result = gEnumOutputs != nullptr ? gEnumOutputs(adapter, index, output) : E_FAIL;
    if (SUCCEEDED(result) && output != nullptr) ObserveOutput(*output);
    return result;
}

HRESULT STDMETHODCALLTYPE HookEnumAdapters(IDXGIFactory* factory, UINT index, IDXGIAdapter** adapter) {
    const HRESULT result = gEnumAdapters != nullptr ? gEnumAdapters(factory, index, adapter) : E_FAIL;
    if (SUCCEEDED(result) && adapter != nullptr) ObserveAdapter(*adapter);
    return result;
}

HRESULT STDMETHODCALLTYPE HookEnumAdapters1(IDXGIFactory1* factory, UINT index, IDXGIAdapter1** adapter) {
    const HRESULT result = gEnumAdapters1 != nullptr ? gEnumAdapters1(factory, index, adapter) : E_FAIL;
    if (SUCCEEDED(result) && adapter != nullptr) ObserveAdapter(*adapter);
    return result;
}

HRESULT STDMETHODCALLTYPE HookGetOutputDesc(IDXGIOutput* output, DXGI_OUTPUT_DESC* description) {
    const HRESULT result = gGetOutputDesc != nullptr ? gGetOutputDesc(output, description) : E_FAIL;
    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(result) && description != nullptr && GetHeatVirtualOutputResolution(&width, &height)) {
        description->DesktopCoordinates.right = description->DesktopCoordinates.left + static_cast<LONG>(width);
        description->DesktopCoordinates.bottom = description->DesktopCoordinates.top + static_cast<LONG>(height);
        LogVirtualDxgiOnce(gLoggedOutputDescription, L"DXGI output description supplied Heat-only mode " +
                                                   std::to_wstring(width) + L"x" + std::to_wstring(height) + L".");
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookGetDisplayModeList(IDXGIOutput* output, DXGI_FORMAT format, UINT flags,
                                                  UINT* modeCount, DXGI_MODE_DESC* modes) {
    UINT width = 0;
    UINT height = 0;
    if (!GetHeatVirtualOutputResolution(&width, &height)) {
        return gGetDisplayModeList != nullptr ? gGetDisplayModeList(output, format, flags, modeCount, modes) : E_FAIL;
    }
    if (modeCount == nullptr) return E_INVALIDARG;
    if (modes == nullptr) {
        *modeCount = 1;
        LogVirtualDxgiOnce(gLoggedOutputModeCount, L"DXGI output mode count supplied Heat-only mode " +
                                                 std::to_wstring(width) + L"x" + std::to_wstring(height) + L".");
        return S_OK;
    }
    if (*modeCount == 0) return DXGI_ERROR_MORE_DATA;
    DXGI_MODE_DESC mode{};
    mode.Width = width;
    mode.Height = height;
    mode.RefreshRate.Numerator = 60;
    mode.RefreshRate.Denominator = 1;
    mode.Format = format;
    mode.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    mode.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    modes[0] = mode;
    *modeCount = 1;
    LogVirtualDxgiOnce(gLoggedOutputModeList, L"DXGI output mode list supplied Heat-only mode " +
                                            std::to_wstring(width) + L"x" + std::to_wstring(height) + L".");
    return S_OK;
}

HRESULT STDMETHODCALLTYPE HookFindClosestMatchingMode(IDXGIOutput* output, const DXGI_MODE_DESC* requested,
                                                       DXGI_MODE_DESC* closest, IUnknown* device) {
    UINT width = 0;
    UINT height = 0;
    if (!GetHeatVirtualOutputResolution(&width, &height)) {
        return gFindClosestMatchingMode != nullptr
            ? gFindClosestMatchingMode(output, requested, closest, device) : E_FAIL;
    }
    if (closest == nullptr) return E_INVALIDARG;
    *closest = requested != nullptr ? *requested : DXGI_MODE_DESC{};
    closest->Width = width;
    closest->Height = height;
    closest->RefreshRate.Numerator = 60;
    closest->RefreshRate.Denominator = 1;
    LogVirtualDxgiOnce(gLoggedClosestMode, L"DXGI closest-mode query accepted Heat-only mode " +
                                       std::to_wstring(width) + L"x" + std::to_wstring(height) + L".");
    return S_OK;
}

HRESULT STDMETHODCALLTYPE HookSetFullscreenState(IDXGISwapChain* swapChain, BOOL fullscreen, IDXGIOutput* output) {
    return gSetFullscreenState != nullptr ? gSetFullscreenState(swapChain, fullscreen, output) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE HookResizeTarget(IDXGISwapChain* swapChain, const DXGI_MODE_DESC* mode) {
    return gResizeTarget != nullptr ? gResizeTarget(swapChain, mode) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* description, IDXGISwapChain** swapChain) {
    const HRESULT result = gCreateSwapChain(factory, device, description, swapChain);
    if (SUCCEEDED(result) && swapChain != nullptr) {
        RestorePhysicalDxgiOutputAfterCreation(*swapChain);
        ObserveDepthTextures(device);
        ObserveSwapChainAndInstallPresent(*swapChain);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1* description, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output, IDXGISwapChain1** swapChain) {
    const HRESULT result = gCreateSwapChainForHwnd(factory, device, window, description, fullscreen, output, swapChain);
    if (SUCCEEDED(result) && swapChain != nullptr) {
        RestorePhysicalDxgiOutputAfterCreation(*swapChain);
        ObserveDepthTextures(device);
        ObserveSwapChainAndInstallPresent(*swapChain);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForCoreWindow(IDXGIFactory2* factory, IUnknown* device, IUnknown* window, const DXGI_SWAP_CHAIN_DESC1* description, IDXGIOutput* output, IDXGISwapChain1** swapChain) {
    const HRESULT result = gCreateSwapChainForCoreWindow(factory, device, window, description, output, swapChain);
    if (SUCCEEDED(result) && swapChain != nullptr) {
        ObserveDepthTextures(device);
        ObserveSwapChainAndInstallPresent(*swapChain);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForComposition(IDXGIFactory2* factory, IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* description, IDXGIOutput* output, IDXGISwapChain1** swapChain) {
    const HRESULT result = gCreateSwapChainForComposition(factory, device, description, output, swapChain);
    if (SUCCEEDED(result) && swapChain != nullptr) {
        ObserveDepthTextures(device);
        ObserveSwapChainAndInstallPresent(*swapChain);
    }
    return result;
}


void ObserveFactory(IUnknown* unknown) {
    if (unknown == nullptr) return;

    IDXGIFactory* factory = nullptr;
    if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&factory)))) return;
    auto** baseVtable = *reinterpret_cast<void***>(factory);
    InstallHook(baseVtable[7], &HookEnumAdapters, &gEnumAdapters, L"IDXGIFactory::EnumAdapters");
    // IDXGIFactory::CreateSwapChain.
    InstallHook(baseVtable[10], &HookCreateSwapChain, &gCreateSwapChain, L"IDXGIFactory::CreateSwapChain");
    factory->Release();

    IDXGIFactory2* factory2 = nullptr;
    if (SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&factory2)))) {
        auto** vtable = *reinterpret_cast<void***>(factory2);
        // IDXGIFactory2 additions begin at index 14. The chosen methods cover
        // Win32, UWP and composition swap-chain creation without an EXE patch.
        InstallHook(vtable[15], &HookCreateSwapChainForHwnd, &gCreateSwapChainForHwnd, L"IDXGIFactory2::CreateSwapChainForHwnd");
        InstallHook(vtable[16], &HookCreateSwapChainForCoreWindow, &gCreateSwapChainForCoreWindow, L"IDXGIFactory2::CreateSwapChainForCoreWindow");
        InstallHook(vtable[24], &HookCreateSwapChainForComposition, &gCreateSwapChainForComposition, L"IDXGIFactory2::CreateSwapChainForComposition");
        factory2->Release();
    }

    IDXGIFactory1* factory1 = nullptr;
    if (SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&factory1)))) {
        auto** vtable = *reinterpret_cast<void***>(factory1);
        InstallHook(vtable[12], &HookEnumAdapters1, &gEnumAdapters1, L"IDXGIFactory1::EnumAdapters1");
        factory1->Release();
    }
}

HRESULT WINAPI HookCreateDXGIFactory(REFIID riid, void** factory) {
    const HRESULT result = gCreateFactory(riid, factory);
    if (SUCCEEDED(result) && factory != nullptr) ObserveFactory(static_cast<IUnknown*>(*factory));
    return result;
}

HRESULT WINAPI HookCreateDXGIFactory1(REFIID riid, void** factory) {
    const HRESULT result = gCreateFactory1(riid, factory);
    if (SUCCEEDED(result) && factory != nullptr) ObserveFactory(static_cast<IUnknown*>(*factory));
    return result;
}

HRESULT WINAPI HookCreateDXGIFactory2(UINT flags, REFIID riid, void** factory) {
    const HRESULT result = gCreateFactory2(flags, riid, factory);
    if (SUCCEEDED(result) && factory != nullptr) ObserveFactory(static_cast<IUnknown*>(*factory));
    return result;
}
} // namespace

bool InstallDxgiHooks() {
    std::scoped_lock lock(gHookMutex);
    if (gInstalled) return true;
    const MH_STATUS minHookStatus = MH_Initialize();
    if (minHookStatus != MH_OK && minHookStatus != MH_ERROR_ALREADY_INITIALIZED) {
        Log(L"MinHook initialisation failed.");
        return false;
    }

    const HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (dxgi == nullptr) return false;
    bool anyHook = false;
    anyHook |= InstallHook(GetProcAddress(dxgi, "CreateDXGIFactory"), &HookCreateDXGIFactory, &gCreateFactory, L"CreateDXGIFactory");
    anyHook |= InstallHook(GetProcAddress(dxgi, "CreateDXGIFactory1"), &HookCreateDXGIFactory1, &gCreateFactory1, L"CreateDXGIFactory1");
    anyHook |= InstallHook(GetProcAddress(dxgi, "CreateDXGIFactory2"), &HookCreateDXGIFactory2, &gCreateFactory2, L"CreateDXGIFactory2");
    gInstalled = anyHook;
    if (gInstalled) InstallInputBridge();
    return anyHook;
}

void RemoveDxgiHooks() {
    if (gInstalled.exchange(false)) {
        RemoveCameraDrawProbe();
        RemoveDepthTextureObserver();
        RemoveFrostbiteCameraObserver();
        RemoveInputBridge();
    }
    // The Heat-only display shim can initialise MinHook before dxgi.dll is
    // loaded.  Always release its hooks too, even if Frostbite never reached
    // swap-chain creation.
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

} // namespace nfsheatvr
