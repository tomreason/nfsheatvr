#include "logger.hpp"

#include <Windows.h>

#include <fstream>
#include <mutex>

namespace nfsheatvr {
namespace {
std::mutex gLogMutex;
std::wstring gLogPath;

std::wstring RuntimeDirectory() {
    wchar_t path[MAX_PATH]{};
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&RuntimeDirectory), &module);
    GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring value(path);
    const auto slash = value.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : value.substr(0, slash);
}
} // namespace

void InitialiseLogger() {
    std::scoped_lock lock(gLogMutex);
    if (!gLogPath.empty()) return;
    gLogPath = RuntimeDirectory() + L"\\NFSHeatVR-" + std::to_wstring(GetCurrentProcessId()) + L".log";
    std::wofstream stream(gLogPath, std::ios::trunc);
    stream << L"NFS Heat VR runtime started\n";
}

void Log(const std::wstring& message) {
    std::scoped_lock lock(gLogMutex);
    if (gLogPath.empty()) gLogPath = RuntimeDirectory() + L"\\NFSHeatVR-" + std::to_wstring(GetCurrentProcessId()) + L".log";
    std::wofstream stream(gLogPath, std::ios::app);
    stream << message << L"\n";
    OutputDebugStringW((L"[NFSHeatVR] " + message + L"\n").c_str());
}

void LogHr(const wchar_t* operation, const long result) {
    Log(std::wstring(operation) + L" failed: 0x" + std::to_wstring(static_cast<unsigned long>(result)));
}

} // namespace nfsheatvr
