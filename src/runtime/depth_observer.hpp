#pragma once

#include <cstddef>

#include <d3d11.h>

namespace nfsheatvr {

struct StereoDepthCandidateInfo {
    UINT width{};
    UINT height{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
};

struct ColourRenderTargetCandidateInfo {
    UINT width{};
    UINT height{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
};

// Observes creation of depth textures and retains only a few shader-readable,
// non-square candidates. The retained reference lets the presenter validate
// real Frostbite depth without altering the game-owned resource.
bool InstallDepthTextureObserver(ID3D11Device* device);
// Records the high-resolution render target that Frostbite most recently
// drew into.  This distinguishes the live scene pass from merely allocated
// temporary targets of the same size.
bool InstallColourPassObserver(ID3D11DeviceContext* context);
void RemoveDepthTextureObserver();
std::size_t StereoDepthCandidateCount();
bool AcquireStereoDepthCandidate(std::size_t index, ID3D11Texture2D** texture, StereoDepthCandidateInfo& info);

// Lists retained, shader-readable colour targets that are larger than the
// final back buffer. The returned resource is AddRef'd and must be released
// by the caller. These helpers never write to the game-owned texture.
std::size_t HighQualityColourRenderTargetCount(const D3D11_TEXTURE2D_DESC& backBuffer);
bool AcquireHighQualityColourRenderTarget(const D3D11_TEXTURE2D_DESC& backBuffer, std::size_t index,
                                          ID3D11Texture2D** texture, ColourRenderTargetCandidateInfo& info);
bool AcquireLatestDrawnHighColourRenderTarget(ID3D11Texture2D** texture, ColourRenderTargetCandidateInfo& info);

} // namespace nfsheatvr
