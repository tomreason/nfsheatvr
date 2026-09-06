#include "dxgi_hooks.hpp"
#include "logger.hpp"

#include <Windows.h>

namespace {
DWORD WINAPI Bootstrap(void*) {
    nfsheatvr::InitialiseLogger();
    // Native ResolutionScale rendering deliberately uses Heat's real DXGI
    // output.  The synthetic-display path stays compiled for rollback, but is
    // not installed here: reporting a made-up monitor can make Frostbite
    // allocate a valid-looking yet black present surface.
    nfsheatvr::Log(L"Native ResolutionScale path selected; no virtual display hooks are active.");
    nfsheatvr::Log(L"Waiting for DXGI to be loaded by Frostbite.");

    // Frostbite loads its render API dynamically. Polling here avoids patching
    // the game's executable or assuming that DX11 is selected in advance.
    for (int elapsed = 0; elapsed < 1800; ++elapsed) {
        const bool dxgi = GetModuleHandleW(L"dxgi.dll") != nullptr;
        if (dxgi && nfsheatvr::InstallDxgiHooks()) {
            nfsheatvr::Log(L"DXGI hooks installed.");
            return 0;
        }
        if (elapsed % 50 == 0) {
            const bool d3d11 = GetModuleHandleW(L"d3d11.dll") != nullptr;
            const bool d3d12 = GetModuleHandleW(L"d3d12.dll") != nullptr;
            nfsheatvr::Log(L"Graphics modules: dxgi=" + std::wstring(dxgi ? L"yes" : L"no") +
                           L", d3d11=" + (d3d11 ? L"yes" : L"no") +
                           L", d3d12=" + (d3d12 ? L"yes" : L"no"));
        }
        Sleep(100);
    }

    nfsheatvr::Log(L"Timed out waiting for DXGI; no game memory was changed.");
    return 0;
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        const HANDLE thread = CreateThread(nullptr, 0, Bootstrap, nullptr, 0, nullptr);
        if (thread != nullptr) CloseHandle(thread);
    }
    if (reason == DLL_PROCESS_DETACH) {
        nfsheatvr::RemoveDxgiHooks();
    }
    return TRUE;
}
