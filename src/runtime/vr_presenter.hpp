#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <memory>

namespace nfsheatvr {

// Deliberately D3D11-only in the first milestone. DX12 is detected and logged,
// but not altered: an unsupported graphics path must never make the game crash.
class VrPresenter {
public:
    struct State;

    VrPresenter();
    ~VrPresenter();

    VrPresenter(const VrPresenter&) = delete;
    VrPresenter& operator=(const VrPresenter&) = delete;

    bool Initialise(IDXGISwapChain* swapChain);
    void PresentFrame(IDXGISwapChain* swapChain);
    bool IsReady() const;

private:
    std::unique_ptr<State> state_;
};

} // namespace nfsheatvr
