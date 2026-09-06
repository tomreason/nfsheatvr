#include "depth_observer.hpp"

#include "logger.hpp"

#include <MinHook.h>

#include <Windows.h>
#include <d3d11.h>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <sstream>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace nfsheatvr {
namespace {

using CreateTexture2DFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                                       const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                                        ID3D11DepthStencilView*);
using PSSetShaderResourcesFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT,
                                                         ID3D11ShaderResourceView* const*);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
CreateTexture2DFn gCreateTexture2D = nullptr;
void* gCreateTexture2DTarget = nullptr;
std::atomic_bool gInstalled{};
std::atomic_bool gColourPassInstalled{};
OMSetRenderTargetsFn gOMSetRenderTargets = nullptr;
PSSetShaderResourcesFn gPSSetShaderResources = nullptr;
DrawIndexedFn gDrawIndexed = nullptr;
DrawFn gDraw = nullptr;
DrawIndexedInstancedFn gDrawIndexedInstanced = nullptr;
DrawInstancedFn gDrawInstanced = nullptr;
void* gOMSetRenderTargetsTarget = nullptr;
void* gPSSetShaderResourcesTarget = nullptr;
void* gDrawIndexedTarget = nullptr;
void* gDrawTarget = nullptr;
void* gDrawIndexedInstancedTarget = nullptr;
void* gDrawInstancedTarget = nullptr;
std::atomic_uint gLoggedCandidateCount{};
std::atomic_uint gLoggedColourCandidateCount{};
struct DepthCandidate {
    ComPtr<ID3D11Texture2D> texture;
    StereoDepthCandidateInfo info;
};
struct ColourCandidate {
    ComPtr<ID3D11Texture2D> texture;
    ColourRenderTargetCandidateInfo info;
};
std::mutex gCandidateMutex;
std::vector<DepthCandidate> gStereoCandidates;
std::vector<ColourCandidate> gColourCandidates;
std::mutex gColourPassMutex;
ComPtr<ID3D11Texture2D> gCurrentHighColourPass;
ColourRenderTargetCandidateInfo gCurrentHighColourPassInfo{};
ComPtr<ID3D11Texture2D> gCurrentHighColourSample;
ColourRenderTargetCandidateInfo gCurrentHighColourSampleInfo{};
ComPtr<ID3D11Texture2D> gLatestDrawnHighColourPass;
ColourRenderTargetCandidateInfo gLatestDrawnHighColourPassInfo{};

bool SupportsColourPresentation(DXGI_FORMAT format);

bool IsHighWideColourPass(const D3D11_TEXTURE2D_DESC& description) {
    if ((description.BindFlags & D3D11_BIND_RENDER_TARGET) == 0 || description.Width <= 2560 || description.Height == 0 ||
        description.SampleDesc.Count != 1 || !SupportsColourPresentation(description.Format)) return false;
    const float aspect = static_cast<float>(description.Width) / static_cast<float>(description.Height);
    return aspect >= 1.3f && aspect <= 3.5f;
}

void SetCurrentHighColourPass(ID3D11RenderTargetView* const* renderTargetViews, const UINT count) {
    ComPtr<ID3D11Texture2D> nextTexture;
    ColourRenderTargetCandidateInfo nextInfo{};
    if (renderTargetViews != nullptr && count > 0 && renderTargetViews[0] != nullptr) {
        ComPtr<ID3D11Resource> resource;
        renderTargetViews[0]->GetResource(&resource);
        if (resource && SUCCEEDED(resource.As(&nextTexture))) {
            D3D11_TEXTURE2D_DESC description{};
            nextTexture->GetDesc(&description);
            if (IsHighWideColourPass(description)) {
                nextInfo = {description.Width, description.Height, description.Format};
            } else {
                nextTexture.Reset();
            }
        }
    }
    std::lock_guard lock(gColourPassMutex);
    gCurrentHighColourPass = std::move(nextTexture);
    gCurrentHighColourPassInfo = nextInfo;
}

void ObserveHighColourShaderResource(ID3D11ShaderResourceView* const* shaderResourceViews, const UINT count) {
    if (shaderResourceViews == nullptr || count == 0) return;
    ComPtr<ID3D11Texture2D> nextTexture;
    ColourRenderTargetCandidateInfo nextInfo{};
    for (UINT index = 0; index < count && !nextTexture; ++index) {
        if (shaderResourceViews[index] == nullptr) continue;
        ComPtr<ID3D11Resource> resource;
        shaderResourceViews[index]->GetResource(&resource);
        if (!resource || FAILED(resource.As(&nextTexture))) continue;
        D3D11_TEXTURE2D_DESC description{};
        nextTexture->GetDesc(&description);
        if (IsHighWideColourPass(description)) {
            nextInfo = {description.Width, description.Height, description.Format};
        } else {
            nextTexture.Reset();
        }
    }
    if (!nextTexture) return;
    std::lock_guard lock(gColourPassMutex);
    gCurrentHighColourSample = std::move(nextTexture);
    gCurrentHighColourSampleInfo = nextInfo;
}

void MarkCurrentHighColourPassDrawn() {
    std::lock_guard lock(gColourPassMutex);
    if (gCurrentHighColourPass) {
        gLatestDrawnHighColourPass = gCurrentHighColourPass;
        gLatestDrawnHighColourPassInfo = gCurrentHighColourPassInfo;
    } else if (gCurrentHighColourSample) {
        // Frostbite's final scale-down is a low-resolution full-screen draw
        // that samples the high-res scene.  The sampled resource, rather
        // than the low output target, is the image the headset must retain.
        gLatestDrawnHighColourPass = gCurrentHighColourSample;
        gLatestDrawnHighColourPassInfo = gCurrentHighColourSampleInfo;
    } else {
        return;
    }
}

void STDMETHODCALLTYPE HookOMSetRenderTargets(ID3D11DeviceContext* context, const UINT renderTargetViewCount,
                                               ID3D11RenderTargetView* const* renderTargetViews,
                                               ID3D11DepthStencilView* depthStencilView) {
    if (gOMSetRenderTargets != nullptr) {
        gOMSetRenderTargets(context, renderTargetViewCount, renderTargetViews, depthStencilView);
    }
    SetCurrentHighColourPass(renderTargetViews, renderTargetViewCount);
}

void STDMETHODCALLTYPE HookPSSetShaderResources(ID3D11DeviceContext* context, const UINT startSlot,
                                                 const UINT shaderResourceViewCount,
                                                 ID3D11ShaderResourceView* const* shaderResourceViews) {
    if (gPSSetShaderResources != nullptr) {
        gPSSetShaderResources(context, startSlot, shaderResourceViewCount, shaderResourceViews);
    }
    ObserveHighColourShaderResource(shaderResourceViews, shaderResourceViewCount);
}

void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* context, const UINT indexCount, const UINT startIndex,
                                       const INT baseVertex) {
    if (gDrawIndexed != nullptr) gDrawIndexed(context, indexCount, startIndex, baseVertex);
    MarkCurrentHighColourPassDrawn();
}

void STDMETHODCALLTYPE HookDraw(ID3D11DeviceContext* context, const UINT vertexCount, const UINT startVertex) {
    if (gDraw != nullptr) gDraw(context, vertexCount, startVertex);
    MarkCurrentHighColourPassDrawn();
}

void STDMETHODCALLTYPE HookDrawIndexedInstanced(ID3D11DeviceContext* context, const UINT indexCountPerInstance,
                                                const UINT instanceCount, const UINT startIndex, const INT baseVertex,
                                                const UINT startInstance) {
    if (gDrawIndexedInstanced != nullptr) {
        gDrawIndexedInstanced(context, indexCountPerInstance, instanceCount, startIndex, baseVertex, startInstance);
    }
    MarkCurrentHighColourPassDrawn();
}

void STDMETHODCALLTYPE HookDrawInstanced(ID3D11DeviceContext* context, const UINT vertexCountPerInstance,
                                         const UINT instanceCount, const UINT startVertex, const UINT startInstance) {
    if (gDrawInstanced != nullptr) gDrawInstanced(context, vertexCountPerInstance, instanceCount, startVertex, startInstance);
    MarkCurrentHighColourPassDrawn();
}

void LogDepthCandidate(const D3D11_TEXTURE2D_DESC& description) {
    if ((description.BindFlags & D3D11_BIND_DEPTH_STENCIL) == 0) return;
    constexpr unsigned int kMaximumCandidateLogs = 24;
    const unsigned int candidateNumber = gLoggedCandidateCount.fetch_add(1) + 1;
    if (candidateNumber > kMaximumCandidateLogs) return;

    std::wostringstream line;
    line << L"Stereo depth candidate #" << candidateNumber
         << L": " << description.Width << L"x" << description.Height
         << L", format=" << static_cast<int>(description.Format)
         << L", bind=0x" << std::hex << description.BindFlags
         << L", samples=" << std::dec << description.SampleDesc.Count
         << L", shader-readable=" << ((description.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0 ? L"yes" : L"no")
         << L".";
    Log(line.str());
}

// This is a diagnostic inventory only. It neither retains nor copies the
// texture, so it cannot alter Frostbite's render path or resolution.
void LogColourRenderTargetCandidate(const D3D11_TEXTURE2D_DESC& description) {
    if ((description.BindFlags & D3D11_BIND_RENDER_TARGET) == 0 ||
        (description.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0) return;
    if (description.Width < 1280 || description.Height < 720) return;
    const float aspect = description.Height == 0 ? 0.0f :
        static_cast<float>(description.Width) / static_cast<float>(description.Height);
    // Exclude Pimax's tall OpenXR eye textures. Wide candidates are the only
    // plausible Frostbite scene-colour sources for Heat's final desktop frame.
    if (aspect < 1.3f || aspect > 3.5f) return;

    constexpr unsigned int kMaximumCandidateLogs = 48;
    const unsigned int candidateNumber = gLoggedColourCandidateCount.fetch_add(1) + 1;
    if (candidateNumber > kMaximumCandidateLogs) return;

    std::wostringstream line;
    line << L"Colour render-target candidate #" << candidateNumber
         << L": " << description.Width << L"x" << description.Height
         << L", format=" << static_cast<int>(description.Format)
         << L", bind=0x" << std::hex << description.BindFlags
         << L", usage=" << std::dec << static_cast<int>(description.Usage)
         << L", mips=" << description.MipLevels
         << L", array=" << description.ArraySize
         << L", samples=" << description.SampleDesc.Count
         << L", shader-readable=" << ((description.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0 ? L"yes" : L"no")
         << L".";
    Log(line.str());
}

void RememberColourRenderTargetCandidate(const D3D11_TEXTURE2D_DESC& description, ID3D11Texture2D* texture) {
    if (texture == nullptr ||
        (description.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)) !=
            (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)) return;
    if (description.Width < 1280 || description.Height < 720 || description.SampleDesc.Count != 1) return;
    const float aspect = description.Height == 0 ? 0.0f :
        static_cast<float>(description.Width) / static_cast<float>(description.Height);
    if (aspect < 1.3f || aspect > 3.5f) return;

    std::lock_guard lock(gCandidateMutex);
    for (const ColourCandidate& candidate : gColourCandidates) {
        if (candidate.texture.Get() == texture) return;
    }
    constexpr std::size_t kMaximumColourCandidates = 64;
    if (gColourCandidates.size() >= kMaximumColourCandidates) return;
    ColourCandidate candidate{};
    candidate.texture = texture;
    candidate.info = {description.Width, description.Height, description.Format};
    gColourCandidates.emplace_back(std::move(candidate));
}

bool SupportsColourPresentation(const DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return true;
    default:
        return false;
    }
}

int ColourPresentationPriority(const DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return 0;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return 1;
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return 2;
    default:
        return 3;
    }
}

std::vector<const ColourCandidate*> HighQualityCandidatesLocked(const D3D11_TEXTURE2D_DESC& backBuffer) {
    std::vector<const ColourCandidate*> candidates;
    if (backBuffer.Width == 0 || backBuffer.Height == 0) return candidates;
    const float backBufferAspect = static_cast<float>(backBuffer.Width) / static_cast<float>(backBuffer.Height);
    for (const ColourCandidate& candidate : gColourCandidates) {
        if (candidate.texture == nullptr || candidate.info.width <= backBuffer.Width ||
            candidate.info.height <= backBuffer.Height || !SupportsColourPresentation(candidate.info.format)) continue;
        const float aspect = static_cast<float>(candidate.info.width) / static_cast<float>(candidate.info.height);
        // In exclusive/borderless mode Frostbite can trim the final DXGI
        // surface by the Windows non-client/taskbar strip while retaining its
        // 21:9 internal scene target. Allow that small mismatch here; the VR
        // presenter still crops proportionally and never stretches either.
        if (std::abs(aspect - backBufferAspect) > 0.15f) continue;
        candidates.push_back(&candidate);
    }
    std::sort(candidates.begin(), candidates.end(), [](const ColourCandidate* left, const ColourCandidate* right) {
        const int leftPriority = ColourPresentationPriority(left->info.format);
        const int rightPriority = ColourPresentationPriority(right->info.format);
        if (leftPriority != rightPriority) return leftPriority < rightPriority;
        const uint64_t leftPixels = static_cast<uint64_t>(left->info.width) * left->info.height;
        const uint64_t rightPixels = static_cast<uint64_t>(right->info.width) * right->info.height;
        return leftPixels > rightPixels;
    });
    return candidates;
}

void RememberStereoCandidate(const D3D11_TEXTURE2D_DESC& description, ID3D11Texture2D* texture) {
    if (texture == nullptr || (description.BindFlags & (D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE)) !=
                                  (D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE)) return;
    const float aspect = description.Height == 0 ? 0.0f : static_cast<float>(description.Width) / description.Height;
    if (description.Width < 640 || description.Height < 360 || aspect < 1.3f || aspect > 3.0f) return;

    std::lock_guard lock(gCandidateMutex);
    for (const DepthCandidate& candidate : gStereoCandidates) {
        if (candidate.texture.Get() == texture ||
            (candidate.info.width == description.Width && candidate.info.height == description.Height &&
             candidate.info.format == description.Format)) return;
    }
    constexpr std::size_t kMaximumStereoCandidates = 4;
    if (gStereoCandidates.size() >= kMaximumStereoCandidates) return;
    DepthCandidate candidate{};
    candidate.texture = texture;
    candidate.info = {description.Width, description.Height, description.Format};
    gStereoCandidates.emplace_back(std::move(candidate));
    Log(L"Stereo depth candidate retained at index " + std::to_wstring(gStereoCandidates.size() - 1) + L".");
}

HRESULT STDMETHODCALLTYPE HookCreateTexture2D(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* description,
                                              const D3D11_SUBRESOURCE_DATA* initialData, ID3D11Texture2D** texture) {
    const HRESULT result = gCreateTexture2D != nullptr
        ? gCreateTexture2D(device, description, initialData, texture) : E_FAIL;
    if (SUCCEEDED(result) && description != nullptr) {
        LogDepthCandidate(*description);
        LogColourRenderTargetCandidate(*description);
        if (texture != nullptr && *texture != nullptr) RememberStereoCandidate(*description, *texture);
        if (texture != nullptr && *texture != nullptr) RememberColourRenderTargetCandidate(*description, *texture);
    }
    return result;
}

} // namespace

bool InstallDepthTextureObserver(ID3D11Device* device) {
    if (gInstalled.load()) return true;
    if (device == nullptr) return false;
    auto** const vtable = *reinterpret_cast<void***>(device);
    if (vtable == nullptr || vtable[5] == nullptr) return false; // ID3D11Device::CreateTexture2D
    gCreateTexture2DTarget = vtable[5];
    const MH_STATUS created = MH_CreateHook(gCreateTexture2DTarget, reinterpret_cast<LPVOID>(&HookCreateTexture2D),
                                            reinterpret_cast<LPVOID*>(&gCreateTexture2D));
    if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED) {
        Log(L"Stereo depth observer hook creation failed (MinHook " + std::to_wstring(created) + L").");
        return false;
    }
    const MH_STATUS enabled = MH_EnableHook(gCreateTexture2DTarget);
    if (enabled != MH_OK && enabled != MH_ERROR_ENABLED) {
        MH_RemoveHook(gCreateTexture2DTarget);
        gCreateTexture2D = nullptr;
        Log(L"Stereo depth observer hook enable failed (MinHook " + std::to_wstring(enabled) + L").");
        return false;
    }
    gLoggedCandidateCount = 0;
    gLoggedColourCandidateCount = 0;
    gInstalled = true;
    Log(L"Stereo depth creation observer installed before Frostbite render-target creation.");
    return true;
}

bool InstallColourPassObserver(ID3D11DeviceContext* context) {
    if (gColourPassInstalled.load()) return true;
    if (context == nullptr) return false;
    auto** const vtable = *reinterpret_cast<void***>(context);
    if (vtable == nullptr || vtable[8] == nullptr || vtable[12] == nullptr || vtable[13] == nullptr ||
        vtable[20] == nullptr || vtable[21] == nullptr || vtable[33] == nullptr) return false;

    gOMSetRenderTargetsTarget = vtable[33];
    gPSSetShaderResourcesTarget = vtable[8];
    gDrawIndexedTarget = vtable[12];
    gDrawTarget = vtable[13];
    gDrawIndexedInstancedTarget = vtable[20];
    gDrawInstancedTarget = vtable[21];
    const auto create = [](void* target, LPVOID replacement, LPVOID* original) {
        const MH_STATUS result = MH_CreateHook(target, replacement, original);
        return result == MH_OK || result == MH_ERROR_ALREADY_CREATED;
    };
    const auto enable = [](void* target) {
        const MH_STATUS result = MH_EnableHook(target);
        return result == MH_OK || result == MH_ERROR_ENABLED;
    };
    if (!create(gOMSetRenderTargetsTarget, reinterpret_cast<LPVOID>(&HookOMSetRenderTargets),
                reinterpret_cast<LPVOID*>(&gOMSetRenderTargets)) ||
        !create(gPSSetShaderResourcesTarget, reinterpret_cast<LPVOID>(&HookPSSetShaderResources),
                reinterpret_cast<LPVOID*>(&gPSSetShaderResources)) ||
        !create(gDrawIndexedTarget, reinterpret_cast<LPVOID>(&HookDrawIndexed), reinterpret_cast<LPVOID*>(&gDrawIndexed)) ||
        !create(gDrawTarget, reinterpret_cast<LPVOID>(&HookDraw), reinterpret_cast<LPVOID*>(&gDraw)) ||
        !create(gDrawIndexedInstancedTarget, reinterpret_cast<LPVOID>(&HookDrawIndexedInstanced),
                reinterpret_cast<LPVOID*>(&gDrawIndexedInstanced)) ||
        !create(gDrawInstancedTarget, reinterpret_cast<LPVOID>(&HookDrawInstanced),
                reinterpret_cast<LPVOID*>(&gDrawInstanced))) {
        Log(L"Could not create the high-resolution Frostbite pass observer.");
        return false;
    }
    if (!enable(gOMSetRenderTargetsTarget) || !enable(gPSSetShaderResourcesTarget) || !enable(gDrawIndexedTarget) || !enable(gDrawTarget) ||
        !enable(gDrawIndexedInstancedTarget) || !enable(gDrawInstancedTarget)) {
        Log(L"Could not enable the high-resolution Frostbite pass observer.");
        return false;
    }
    gColourPassInstalled = true;
    Log(L"High-resolution Frostbite pass observer installed; tracking the last live wide colour draw.");
    return true;
}

void RemoveDepthTextureObserver() {
    if (gColourPassInstalled.exchange(false)) {
        for (void* target : {gOMSetRenderTargetsTarget, gPSSetShaderResourcesTarget, gDrawIndexedTarget, gDrawTarget,
                             gDrawIndexedInstancedTarget, gDrawInstancedTarget}) {
            if (target != nullptr) {
                MH_DisableHook(target);
                MH_RemoveHook(target);
            }
        }
    }
    gOMSetRenderTargets = nullptr;
    gPSSetShaderResources = nullptr;
    gDrawIndexed = nullptr;
    gDraw = nullptr;
    gDrawIndexedInstanced = nullptr;
    gDrawInstanced = nullptr;
    gOMSetRenderTargetsTarget = nullptr;
    gPSSetShaderResourcesTarget = nullptr;
    gDrawIndexedTarget = nullptr;
    gDrawTarget = nullptr;
    gDrawIndexedInstancedTarget = nullptr;
    gDrawInstancedTarget = nullptr;
    if (gInstalled.exchange(false) && gCreateTexture2DTarget != nullptr) {
        MH_DisableHook(gCreateTexture2DTarget);
        MH_RemoveHook(gCreateTexture2DTarget);
    }
    gCreateTexture2D = nullptr;
    gCreateTexture2DTarget = nullptr;
    std::lock_guard lock(gCandidateMutex);
    gStereoCandidates.clear();
    gColourCandidates.clear();
    {
        std::lock_guard colourPassLock(gColourPassMutex);
        gCurrentHighColourPass.Reset();
        gCurrentHighColourSample.Reset();
        gLatestDrawnHighColourPass.Reset();
        gCurrentHighColourPassInfo = {};
        gCurrentHighColourSampleInfo = {};
        gLatestDrawnHighColourPassInfo = {};
    }
}

std::size_t StereoDepthCandidateCount() {
    std::lock_guard lock(gCandidateMutex);
    return gStereoCandidates.size();
}

bool AcquireStereoDepthCandidate(const std::size_t index, ID3D11Texture2D** texture, StereoDepthCandidateInfo& info) {
    if (texture == nullptr) return false;
    *texture = nullptr;
    std::lock_guard lock(gCandidateMutex);
    if (index >= gStereoCandidates.size() || gStereoCandidates[index].texture == nullptr) return false;
    *texture = gStereoCandidates[index].texture.Get();
    (*texture)->AddRef();
    info = gStereoCandidates[index].info;
    return true;
}

std::size_t HighQualityColourRenderTargetCount(const D3D11_TEXTURE2D_DESC& backBuffer) {
    std::lock_guard lock(gCandidateMutex);
    return HighQualityCandidatesLocked(backBuffer).size();
}

bool AcquireHighQualityColourRenderTarget(const D3D11_TEXTURE2D_DESC& backBuffer, const std::size_t index,
                                          ID3D11Texture2D** texture, ColourRenderTargetCandidateInfo& info) {
    if (texture == nullptr) return false;
    *texture = nullptr;
    std::lock_guard lock(gCandidateMutex);
    const std::vector<const ColourCandidate*> candidates = HighQualityCandidatesLocked(backBuffer);
    if (index >= candidates.size()) return false;
    const ColourCandidate* candidate = candidates[index];
    *texture = candidate->texture.Get();
    (*texture)->AddRef();
    info = candidate->info;
    return true;
}

bool AcquireLatestDrawnHighColourRenderTarget(ID3D11Texture2D** texture, ColourRenderTargetCandidateInfo& info) {
    if (texture == nullptr) return false;
    *texture = nullptr;
    std::lock_guard lock(gColourPassMutex);
    if (!gLatestDrawnHighColourPass) return false;
    *texture = gLatestDrawnHighColourPass.Get();
    (*texture)->AddRef();
    info = gLatestDrawnHighColourPassInfo;
    return true;
}

} // namespace nfsheatvr
