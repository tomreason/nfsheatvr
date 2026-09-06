#include "viewport_scaler.hpp"

#include "logger.hpp"
#include "vr_settings.hpp"

#include <MinHook.h>

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace nfsheatvr {
namespace {
using RsSetViewportsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using RsSetScissorRectsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);

RsSetViewportsFn gRsSetViewports = nullptr;
RsSetScissorRectsFn gRsSetScissorRects = nullptr;
void* gRsSetViewportsTarget = nullptr;
void* gRsSetScissorRectsTarget = nullptr;
std::atomic_bool gInstalled{};
std::atomic_bool gEnabled{};
std::atomic_bool gViewportExpansionLogged{};
std::atomic_bool gScissorExpansionLogged{};

bool GetActiveRenderSize(ID3D11DeviceContext* context, UINT& width, UINT& height) {
    width = 0;
    height = 0;
    if (context == nullptr) return false;
    ID3D11RenderTargetView* rawTarget = nullptr;
    ID3D11DepthStencilView* rawDepth = nullptr;
    context->OMGetRenderTargets(1, &rawTarget, &rawDepth);
    ComPtr<ID3D11RenderTargetView> target;
    target.Attach(rawTarget);
    ComPtr<ID3D11DepthStencilView> depth;
    depth.Attach(rawDepth);

    ComPtr<ID3D11Resource> resource;
    if (target != nullptr) target->GetResource(&resource);
    if (resource == nullptr && depth != nullptr) depth->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (resource == nullptr || FAILED(resource.As(&texture))) return false;

    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (description.Width == 0 || description.Height == 0) return false;
    const float aspect = static_cast<float>(description.Width) / static_cast<float>(description.Height);
    if (aspect < 2.2f || aspect > 2.55f) return false;
    width = description.Width;
    height = description.Height;
    return true;
}

bool ExpansionFactor(const UINT targetWidth, const UINT targetHeight, const float width, const float height,
                     float& factor) {
    if (targetWidth == 0 || targetHeight == 0 || width <= 0.0f || height <= 0.0f) return false;
    const float widthFactor = static_cast<float>(targetWidth) / width;
    const float heightFactor = static_cast<float>(targetHeight) / height;
    if (widthFactor < 1.5f || widthFactor > 2.1f || std::abs(widthFactor - heightFactor) > 0.05f) return false;
    factor = widthFactor;
    return true;
}

void STDMETHODCALLTYPE HookRsSetViewports(ID3D11DeviceContext* context, const UINT count,
                                           const D3D11_VIEWPORT* viewports) {
    if (gRsSetViewports == nullptr || !gEnabled.load() || viewports == nullptr || count == 0 || count > 16) {
        if (gRsSetViewports != nullptr) gRsSetViewports(context, count, viewports);
        return;
    }
    UINT targetWidth{};
    UINT targetHeight{};
    if (!GetActiveRenderSize(context, targetWidth, targetHeight)) {
        gRsSetViewports(context, count, viewports);
        return;
    }

    std::array<D3D11_VIEWPORT, 16> expanded{};
    bool changed{};
    for (UINT index = 0; index < count; ++index) {
        expanded[index] = viewports[index];
        float factor{};
        if (!ExpansionFactor(targetWidth, targetHeight, viewports[index].Width, viewports[index].Height, factor)) continue;
        expanded[index].TopLeftX *= factor;
        expanded[index].TopLeftY *= factor;
        expanded[index].Width *= factor;
        expanded[index].Height *= factor;
        changed = true;
    }
    if (changed && !gViewportExpansionLogged.exchange(true)) {
        Log(L"Frostbite scene viewport expanded to match the high-resolution render target.");
    }
    gRsSetViewports(context, count, changed ? expanded.data() : viewports);
}

void STDMETHODCALLTYPE HookRsSetScissorRects(ID3D11DeviceContext* context, const UINT count,
                                              const D3D11_RECT* rectangles) {
    if (gRsSetScissorRects == nullptr || !gEnabled.load() || rectangles == nullptr || count == 0 || count > 16) {
        if (gRsSetScissorRects != nullptr) gRsSetScissorRects(context, count, rectangles);
        return;
    }
    UINT targetWidth{};
    UINT targetHeight{};
    if (!GetActiveRenderSize(context, targetWidth, targetHeight)) {
        gRsSetScissorRects(context, count, rectangles);
        return;
    }

    std::array<D3D11_RECT, 16> expanded{};
    bool changed{};
    for (UINT index = 0; index < count; ++index) {
        expanded[index] = rectangles[index];
        const LONG width = rectangles[index].right - rectangles[index].left;
        const LONG height = rectangles[index].bottom - rectangles[index].top;
        float factor{};
        if (width <= 0 || height <= 0 || !ExpansionFactor(targetWidth, targetHeight,
                                                            static_cast<float>(width), static_cast<float>(height), factor)) {
            continue;
        }
        expanded[index].left = static_cast<LONG>(std::lround(rectangles[index].left * factor));
        expanded[index].top = static_cast<LONG>(std::lround(rectangles[index].top * factor));
        expanded[index].right = static_cast<LONG>(std::lround(rectangles[index].right * factor));
        expanded[index].bottom = static_cast<LONG>(std::lround(rectangles[index].bottom * factor));
        changed = true;
    }
    if (changed && !gScissorExpansionLogged.exchange(true)) {
        Log(L"Frostbite scene scissor rectangles expanded to match the high-resolution render target.");
    }
    gRsSetScissorRects(context, count, changed ? expanded.data() : rectangles);
}

bool InstallHook(void* target, LPVOID detour, LPVOID* original, const wchar_t* name) {
    if (target == nullptr) return false;
    const MH_STATUS created = MH_CreateHook(target, detour, original);
    if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED) {
        Log(L"Could not create " + std::wstring(name) + L" hook (MinHook " + std::to_wstring(created) + L").");
        return false;
    }
    const MH_STATUS enabled = MH_EnableHook(target);
    if (enabled != MH_OK && enabled != MH_ERROR_ENABLED) {
        Log(L"Could not enable " + std::wstring(name) + L" hook (MinHook " + std::to_wstring(enabled) + L").");
        return false;
    }
    return true;
}
} // namespace

bool InstallFrostbiteViewportScaler(ID3D11DeviceContext* context) {
    if (gInstalled.load() || context == nullptr) return gInstalled.load();
    gEnabled = LoadVrSettings().internalRenderScalePercent > 100;
    if (!gEnabled.load()) return false;
    auto** const vtable = *reinterpret_cast<void***>(context);
    if (vtable == nullptr || vtable[44] == nullptr || vtable[45] == nullptr) return false;
    gRsSetViewportsTarget = vtable[44];
    gRsSetScissorRectsTarget = vtable[45];
    const bool viewports = InstallHook(gRsSetViewportsTarget, reinterpret_cast<LPVOID>(&HookRsSetViewports),
                                       reinterpret_cast<LPVOID*>(&gRsSetViewports), L"ID3D11DeviceContext::RSSetViewports");
    const bool scissors = InstallHook(gRsSetScissorRectsTarget, reinterpret_cast<LPVOID>(&HookRsSetScissorRects),
                                      reinterpret_cast<LPVOID*>(&gRsSetScissorRects), L"ID3D11DeviceContext::RSSetScissorRects");
    if (!viewports && !scissors) return false;
    gViewportExpansionLogged = false;
    gScissorExpansionLogged = false;
    gInstalled = true;
    Log(L"Frostbite high-resolution viewport scaler installed.");
    return true;
}

void RemoveFrostbiteViewportScaler() {
    if (gInstalled.exchange(false)) {
        if (gRsSetViewportsTarget != nullptr) MH_DisableHook(gRsSetViewportsTarget);
        if (gRsSetScissorRectsTarget != nullptr) MH_DisableHook(gRsSetScissorRectsTarget);
    }
    gRsSetViewports = nullptr;
    gRsSetScissorRects = nullptr;
    gRsSetViewportsTarget = nullptr;
    gRsSetScissorRectsTarget = nullptr;
    gEnabled = false;
}

} // namespace nfsheatvr
