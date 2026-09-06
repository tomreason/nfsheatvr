#include "camera_probe.hpp"

#include "logger.hpp"

#include <MinHook.h>

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_set>

using Microsoft::WRL::ComPtr;

namespace nfsheatvr {
namespace {
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);

DrawIndexedFn gDrawIndexed = nullptr;
DrawFn gDraw = nullptr;
DrawIndexedInstancedFn gDrawIndexedInstanced = nullptr;
DrawInstancedFn gDrawInstanced = nullptr;
void* gDrawIndexedTarget = nullptr;
void* gDrawTarget = nullptr;
void* gDrawIndexedInstancedTarget = nullptr;
void* gDrawInstancedTarget = nullptr;
ComPtr<ID3D11Device> gDevice;
std::atomic_bool gInstalled{};
std::atomic_int gRemainingDraws{};
std::atomic_uint gDrawSamples{};
std::mutex gProbeMutex;
std::unordered_set<ID3D11Buffer*> gSeenBuffers;

void LogBoundBuffers(ID3D11DeviceContext* context, const wchar_t* callName, const UINT workItems, const UINT sample) {
    std::lock_guard lock(gProbeMutex);
    constexpr UINT slotCount = 14;
    std::array<ID3D11Buffer*, slotCount> buffers{};
    context->VSGetConstantBuffers(0, slotCount, buffers.data());
    for (UINT slot = 0; slot < slotCount; ++slot) {
        ComPtr<ID3D11Buffer> buffer;
        buffer.Attach(buffers[slot]);
        if (!buffer || !gSeenBuffers.insert(buffer.Get()).second) continue;
        D3D11_BUFFER_DESC description{};
        buffer->GetDesc(&description);
        if (description.ByteWidth < sizeof(float) * 16 || description.ByteWidth > 1024) continue;

        D3D11_BUFFER_DESC staging{};
        staging.ByteWidth = description.ByteWidth;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> readable;
        if (FAILED(gDevice->CreateBuffer(&staging, nullptr, &readable))) continue;
        context->CopyResource(readable.Get(), buffer.Get());
        context->Flush();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(readable.Get(), 0, D3D11_MAP_READ, 0, &mapped))) continue;

        const auto* values = static_cast<const float*>(mapped.pData);
        const UINT matrixCount = (std::min)(4u, description.ByteWidth / static_cast<UINT>(sizeof(float) * 16));
        for (UINT matrix = 0; matrix < matrixCount; ++matrix) {
            std::wostringstream line;
            line << std::fixed << std::setprecision(3);
            line << L"Draw probe #" << sample << L" call=" << callName << L" items=" << workItems << L" VS" << slot << L" bytes="
                 << description.ByteWidth << L" m" << matrix << L":";
            for (UINT value = 0; value < 16; ++value) line << L" " << values[matrix * 16 + value];
            Log(line.str());
        }
        context->Unmap(readable.Get(), 0);
    }
}

void CaptureDraw(ID3D11DeviceContext* context, const wchar_t* callName, const UINT workItems) {
    int remaining = gRemainingDraws.load();
    while (remaining > 0 && !gRemainingDraws.compare_exchange_weak(remaining, remaining - 1)) {}
    if (remaining <= 0) return;

    const UINT sample = gDrawSamples.fetch_add(1) + 1;
    LogBoundBuffers(context, callName, workItems, sample);
    if (remaining == 1) Log(L"Camera draw probe complete.");
}

void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* context, UINT indexCount, UINT startIndex, INT baseVertex) {
    CaptureDraw(context, L"DrawIndexed", indexCount);
    if (gDrawIndexed != nullptr) gDrawIndexed(context, indexCount, startIndex, baseVertex);
}

void STDMETHODCALLTYPE HookDraw(ID3D11DeviceContext* context, UINT vertexCount, UINT startVertex) {
    CaptureDraw(context, L"Draw", vertexCount);
    if (gDraw != nullptr) gDraw(context, vertexCount, startVertex);
}

void STDMETHODCALLTYPE HookDrawIndexedInstanced(ID3D11DeviceContext* context, UINT indexCountPerInstance,
                                                 UINT instanceCount, UINT startIndex, INT baseVertex,
                                                 UINT startInstance) {
    CaptureDraw(context, L"DrawIndexedInstanced", indexCountPerInstance);
    if (gDrawIndexedInstanced != nullptr) {
        gDrawIndexedInstanced(context, indexCountPerInstance, instanceCount, startIndex, baseVertex, startInstance);
    }
}

void STDMETHODCALLTYPE HookDrawInstanced(ID3D11DeviceContext* context, UINT vertexCountPerInstance,
                                          UINT instanceCount, UINT startVertex, UINT startInstance) {
    CaptureDraw(context, L"DrawInstanced", vertexCountPerInstance);
    if (gDrawInstanced != nullptr) {
        gDrawInstanced(context, vertexCountPerInstance, instanceCount, startVertex, startInstance);
    }
}

bool InstallHook(void* target, LPVOID detour, LPVOID* original, const wchar_t* name) {
    const MH_STATUS created = MH_CreateHook(target, detour, original);
    if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED) {
        Log(L"Could not create " + std::wstring(name) + L" camera probe hook (MinHook " + std::to_wstring(created) + L").");
        return false;
    }
    const MH_STATUS enabled = MH_EnableHook(target);
    if (enabled != MH_OK && enabled != MH_ERROR_ENABLED) {
        Log(L"Could not enable " + std::wstring(name) + L" camera probe hook (MinHook " + std::to_wstring(enabled) + L").");
        return false;
    }
    return true;
}
} // namespace

bool InstallCameraDrawProbe(ID3D11Device* device, ID3D11DeviceContext* context) {
    // Frostbite's D3D11 execution path may dispatch through implementation stubs
    // that cannot safely be detoured at the immediate-context vtable level.
    // Keep the experimental probe dormant until it can be moved to a safe hook.
    (void)device;
    (void)context;
    Log(L"Camera draw probe is disabled: preserving the stable VR renderer.");
    return false;

#if 0
    if (gInstalled) return true;
    if (device == nullptr || context == nullptr) return false;
    auto** vtable = *reinterpret_cast<void***>(context);
    // ID3D11DeviceContext vtable indices include the three IUnknown entries.
    gDrawIndexedTarget = vtable[8];
    gDrawTarget = vtable[9];
    gDrawIndexedInstancedTarget = vtable[16];
    gDrawInstancedTarget = vtable[17];
    const bool indexed = InstallHook(gDrawIndexedTarget, reinterpret_cast<LPVOID>(&HookDrawIndexed),
                                     reinterpret_cast<LPVOID*>(&gDrawIndexed), L"DrawIndexed");
    const bool draw = InstallHook(gDrawTarget, reinterpret_cast<LPVOID>(&HookDraw),
                                  reinterpret_cast<LPVOID*>(&gDraw), L"Draw");
    const bool indexedInstanced = InstallHook(gDrawIndexedInstancedTarget, reinterpret_cast<LPVOID>(&HookDrawIndexedInstanced),
                                              reinterpret_cast<LPVOID*>(&gDrawIndexedInstanced), L"DrawIndexedInstanced");
    const bool instanced = InstallHook(gDrawInstancedTarget, reinterpret_cast<LPVOID>(&HookDrawInstanced),
                                       reinterpret_cast<LPVOID*>(&gDrawInstanced), L"DrawInstanced");
    if (!indexed && !draw && !indexedInstanced && !instanced) return false;
    gDevice = device;
    gInstalled = true;
    Log(L"Camera draw probe installed. Press F6 to capture the next render draws.");
    return true;
#endif
}

void RemoveCameraDrawProbe() {
    if (gInstalled.exchange(false)) {
        if (gDrawIndexedTarget != nullptr) MH_DisableHook(gDrawIndexedTarget);
        if (gDrawTarget != nullptr) MH_DisableHook(gDrawTarget);
        if (gDrawIndexedInstancedTarget != nullptr) MH_DisableHook(gDrawIndexedInstancedTarget);
        if (gDrawInstancedTarget != nullptr) MH_DisableHook(gDrawInstancedTarget);
    }
    gRemainingDraws = 0;
    {
        std::lock_guard lock(gProbeMutex);
        gSeenBuffers.clear();
    }
    gDevice.Reset();
}

void RequestCameraDrawProbe() {
    if (!gInstalled) {
        Log(L"Camera draw probe is not available for this renderer.");
        return;
    }
    {
        std::lock_guard lock(gProbeMutex);
        gSeenBuffers.clear();
    }
    gDrawSamples = 0;
    gRemainingDraws = 48;
    Log(L"Camera draw probe armed: sampling the next 48 render draws.");
}

} // namespace nfsheatvr
