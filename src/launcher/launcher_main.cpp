#include <Windows.h>
#include <ShlObj_core.h>
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {
constexpr ULONGLONG kRelayWatchWindowMs = 180000;
constexpr unsigned kDefaultVrRenderWidth = 2560;
constexpr unsigned kDefaultVrRenderHeight = 1080;

struct RequestedRenderResolution {
    unsigned width{kDefaultVrRenderWidth};
    unsigned height{kDefaultVrRenderHeight};
};

std::filesystem::path CurrentExecutableDirectory() {
    wchar_t value[MAX_PATH]{};
    GetModuleFileNameW(nullptr, value, MAX_PATH);
    return std::filesystem::path(value).parent_path();
}

bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right) {
    const auto leftText = std::filesystem::absolute(left).lexically_normal().wstring();
    const auto rightText = std::filesystem::absolute(right).lexically_normal().wstring();
    return CompareStringOrdinal(leftText.c_str(), -1, rightText.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool IsTargetProcess(const DWORD processId, const std::filesystem::path& game) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) return false;
    std::array<wchar_t, 32768> image{};
    DWORD length = static_cast<DWORD>(image.size());
    const bool queried = QueryFullProcessImageNameW(process, 0, image.data(), &length) != FALSE;
    CloseHandle(process);
    return queried && SamePath(std::filesystem::path(std::wstring(image.data(), length)), game);
}

std::vector<DWORD> FindTargetProcesses(const std::filesystem::path& game) {
    std::vector<DWORD> result;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, game.filename().c_str()) == 0 && IsTargetProcess(entry.th32ProcessID, game)) {
                result.push_back(entry.th32ProcessID);
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

bool InjectRuntime(const HANDLE process, const std::filesystem::path& runtime) {
    const std::wstring runtimeText = runtime.wstring();
    const SIZE_T bytes = (runtimeText.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (remote == nullptr) return false;

    SIZE_T written = 0;
    const bool wrote = WriteProcessMemory(process, remote, runtimeText.c_str(), bytes, &written) != FALSE && written == bytes;
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"));
    HANDLE thread = wrote && loadLibrary != nullptr
        ? CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr)
        : nullptr;
    DWORD moduleAddress = 0;
    const bool completed = thread != nullptr && WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0 &&
                           GetExitCodeThread(thread, &moduleAddress) != FALSE && moduleAddress != 0;
    if (thread != nullptr) CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return completed;
}

bool InjectProcessById(const DWORD processId, const std::filesystem::path& runtime) {
    const HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                       PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                       FALSE, processId);
    if (process == nullptr) return false;
    const bool injected = InjectRuntime(process, runtime);
    CloseHandle(process);
    return injected;
}

std::wstring Quote(const std::filesystem::path& path) {
    return L"\"" + path.wstring() + L"\"";
}

std::optional<std::filesystem::path> HeatProfileOptionsPath() {
    PWSTR documents = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)) || documents == nullptr) return std::nullopt;
    const std::filesystem::path result = std::filesystem::path(documents) / L"Need for Speed Heat" /
                                         L"settings" / L"PROFILEOPTIONS_profile";
    CoTaskMemFree(documents);
    return result;
}

RequestedRenderResolution LoadRequestedRenderResolution() {
    const auto settings = CurrentExecutableDirectory() / L"NFSHeatVR.ini";
    const UINT width = GetPrivateProfileIntW(L"Game", L"ResolutionWidth", kDefaultVrRenderWidth, settings.c_str());
    const UINT height = GetPrivateProfileIntW(L"Game", L"ResolutionHeight", kDefaultVrRenderHeight, settings.c_str());
    return {std::clamp(static_cast<unsigned>(width), 320u, 16384u),
            std::clamp(static_cast<unsigned>(height), 320u, 16384u)};
}

bool ReplaceProfileValue(std::string& text, const std::string& key, const std::string& value) {
    std::size_t lineStart = 0;
    while (lineStart < text.size()) {
        const std::size_t lineEnd = text.find_first_of("\r\n", lineStart);
        const std::size_t lineLength = (lineEnd == std::string::npos ? text.size() : lineEnd) - lineStart;
        const std::string prefix = key + " ";
        if (lineLength >= prefix.size() && text.compare(lineStart, prefix.size(), prefix) == 0) {
            const std::string replacement = prefix + value;
            if (text.compare(lineStart, lineLength, replacement) == 0) return false;
            text.replace(lineStart, lineLength, replacement);
            return true;
        }
        if (lineEnd == std::string::npos) break;
        lineStart = lineEnd + 1;
        if (lineStart < text.size() && text[lineEnd] == '\r' && text[lineStart] == '\n') ++lineStart;
    }
    if (!text.empty() && text.back() != '\n') text += "\r\n";
    text += key + " " + value + "\r\n";
    return true;
}

bool ConfigureHeatVrResolution() {
    const auto profile = HeatProfileOptionsPath();
    if (!profile || !std::filesystem::is_regular_file(*profile)) {
        std::wcerr << L"Heat VR resolution setup skipped: PROFILEOPTIONS_profile was not found.\n";
        return false;
    }

    std::ifstream input(*profile, std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) {
        std::wcerr << L"Heat VR resolution setup could not read PROFILEOPTIONS_profile.\n";
        return false;
    }
    const RequestedRenderResolution requested = LoadRequestedRenderResolution();
    bool changed = false;
    changed |= ReplaceProfileValue(contents, "GstRender.ResolutionWidth", std::to_string(requested.width));
    changed |= ReplaceProfileValue(contents, "GstRender.ResolutionHeight", std::to_string(requested.height));
    // Do not force an internal Frostbite supersampling scale. A 5120x2160
    // desktop target at the old 200% value created 10240x4320 scene targets,
    // which caused a confirmed NVIDIA TDR/DirectX device loss.
    changed |= ReplaceProfileValue(contents, "GstRender.ResolutionScale", "1.0");
    if (!changed) {
        std::wcout << L"Heat VR render profile is already " << requested.width << L"x" << requested.height
                   << L" at the game's native 100% scene scale. Window mode was left unchanged.\n";
        return true;
    }

    const auto backup = std::filesystem::path(profile->wstring() + L".NFSHeatVR.backup");
    std::error_code copyError;
    if (!std::filesystem::exists(backup) && !std::filesystem::copy_file(*profile, backup, std::filesystem::copy_options::none,
                                                                          copyError)) {
        std::wcerr << L"Heat VR resolution setup could not create a one-time profile backup.\n";
        return false;
    }
    const auto temporary = std::filesystem::path(profile->wstring() + L".NFSHeatVR.tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!output) {
            std::wcerr << L"Heat VR resolution setup could not write its temporary profile.\n";
            return false;
        }
    }
    // A cloud-sync or antivirus scan can briefly grab the profile between
    // writing the temporary file and replacing it. Retry that atomic path a
    // few times, then fall back to a direct replacement before the game starts.
    DWORD replaceError = ERROR_SUCCESS;
    bool replaced = false;
    for (unsigned attempt = 0; attempt < 5 && !replaced; ++attempt) {
        SetLastError(ERROR_SUCCESS);
        replaced = MoveFileExW(temporary.c_str(), profile->c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        if (replaced) break;
        replaceError = GetLastError();
        if (replaceError != ERROR_SHARING_VIOLATION && replaceError != ERROR_ACCESS_DENIED) break;
        Sleep(120);
    }
    if (!replaced) {
        SetLastError(ERROR_SUCCESS);
        replaced = CopyFileW(temporary.c_str(), profile->c_str(), FALSE) != FALSE;
        const DWORD copyError = replaced ? ERROR_SUCCESS : GetLastError();
        if (replaced) {
            DeleteFileW(temporary.c_str());
        } else {
            // Capture the Win32 errors before touching the temporary file;
            // cleanup APIs may otherwise reset the thread's last-error code.
            DeleteFileW(temporary.c_str());
            std::wcerr << L"Heat VR resolution setup could not replace PROFILEOPTIONS_profile "
                       << L"(atomic write Win32 error " << replaceError
                       << L", fallback write Win32 error " << copyError << L").\n";
            return false;
        }
    }
    std::wcout << L"Heat VR render profile configured: " << requested.width << L"x" << requested.height
               << L", game-native 100% scene scale. Window mode was left unchanged.\n";
    return true;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    const auto launcherDirectory = CurrentExecutableDirectory();
    if (argc == 4 && _wcsicmp(argv[1], L"--inject-pid") == 0) {
        wchar_t* parseEnd = nullptr;
        const unsigned long parsedPid = wcstoul(argv[2], &parseEnd, 10);
        const std::filesystem::path library = argv[3];
        if (parseEnd == argv[2] || *parseEnd != L'\0' || parsedPid == 0 ||
            !std::filesystem::is_regular_file(library)) {
            std::wcerr << L"Usage: NFSHeatVRLauncher.exe --inject-pid <PID> <absolute DLL path>\n";
            return 6;
        }
        if (!InjectProcessById(static_cast<DWORD>(parsedPid), library)) {
            std::wcerr << L"DLL injection failed (Win32 error " << GetLastError() << L").\n";
            return 7;
        }
        std::wcout << L"DLL injected into process " << parsedPid << L".\n";
        return 0;
    }
    // Portable-release default: the three mod files are extracted directly
    // next to NeedForSpeedHeat.exe, so no user-specific Steam path is baked
    // into the public launcher.
    std::filesystem::path game = launcherDirectory / L"NeedForSpeedHeat.exe";
    if (argc == 2) game = argv[1];
    // Keep the runtime filename versioned so an orphaned Steam/EA child cannot
    // block a safe update of the next launch. This remains entirely internal
    // to the mod folder; users still launch the same NFSHeatVRLauncher.exe.
    const auto runtime = launcherDirectory / L"NFSHeatVRRuntime.dll";
    if (!std::filesystem::is_regular_file(game)) {
        std::wcerr << L"NeedForSpeedHeat.exe was not found beside the launcher:\n" << game << L"\n"
                   << L"Extract NFS Heat VR directly into the game folder, or pass the full game path as an argument.\n";
        return 2;
    }
    if (!std::filesystem::is_regular_file(runtime)) {
        std::wcerr << L"NFSHeatVRRuntime.dll must be beside the launcher.\n";
        return 3;
    }
    ConfigureHeatVrResolution();

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION started{};
    std::wstring command = Quote(game);
    if (!CreateProcessW(game.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, game.parent_path().c_str(), &startup, &started)) {
        std::wcerr << L"Could not launch the game (Win32 error " << GetLastError() << L").\n";
        return 4;
    }

    std::set<DWORD> inspected{started.dwProcessId};
    const bool initialInjected = InjectRuntime(started.hProcess, runtime);
    if (!initialInjected) {
        TerminateProcess(started.hProcess, ERROR_DLL_INIT_FAILED);
        CloseHandle(started.hThread);
        CloseHandle(started.hProcess);
        std::wcerr << L"VR runtime injection failed; the game was not started.\n";
        return 5;
    }
    ResumeThread(started.hThread);
    CloseHandle(started.hThread);
    CloseHandle(started.hProcess);

    // Steam and the EA app can hand off execution to another NeedForSpeedHeat.exe.
    // The exact-path filter prevents injecting an unrelated process with the same name.
    std::wcout << L"Watching the Steam/EA launch handoff for up to three minutes...\n";
    const ULONGLONG deadline = GetTickCount64() + kRelayWatchWindowMs;
    while (GetTickCount64() < deadline) {
        for (const DWORD processId : FindTargetProcesses(game)) {
            if (!inspected.insert(processId).second) continue;
            if (InjectProcessById(processId, runtime)) {
                std::wcout << L"VR runtime injected into game process " << processId << L".\n";
            } else {
                std::wcerr << L"Could not inject game process " << processId << L" (Win32 error " << GetLastError() << L").\n";
            }
        }
        Sleep(250);
    }
    std::wcout << L"Handoff watch completed. The game can remain open.\n";
    return 0;
}
