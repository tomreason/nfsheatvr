#include "vr_settings.hpp"
#include "../resources/nfs_heat_vr_resource.h"

#include <Windows.h>
#include <CommCtrl.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>

using nfsheatvr::LoadVrSettings;
using nfsheatvr::SaveVrSettings;
using nfsheatvr::VrSettings;

namespace {

constexpr COLORREF kBackground = RGB(31, 31, 31);
constexpr COLORREF kInputBackground = RGB(47, 47, 47);
constexpr COLORREF kButtonBackground = RGB(43, 43, 43);
constexpr COLORREF kButtonPressed = RGB(62, 62, 62);
constexpr COLORREF kText = RGB(232, 232, 232);
constexpr COLORREF kMutedText = RGB(165, 165, 165);
constexpr COLORREF kAccent = RGB(0, 160, 222);

constexpr int kResolutionWidthEdit = 101;
constexpr int kResolutionHeightEdit = 102;
constexpr int kDepthStrengthSlider = 201;
constexpr int kEngineFovSlider = 202;
constexpr int kRotationSlider = 203;
constexpr int kTranslationSlider = 204;
constexpr int kSeatRightSlider = 205;
constexpr int kSeatUpSlider = 206;
constexpr int kSeatForwardSlider = 207;
constexpr int kHeadsetHorizontalFovSlider = 208;
constexpr int kHeadsetVerticalFovSlider = 209;
constexpr int kHeadsetUpscalerSlider = 210;
constexpr int kDepthStrengthEdit = 301;
constexpr int kEngineFovEdit = 302;
constexpr int kRotationEdit = 303;
constexpr int kTranslationEdit = 304;
constexpr int kSeatRightEdit = 305;
constexpr int kSeatUpEdit = 306;
constexpr int kSeatForwardEdit = 307;
constexpr int kHeadsetHorizontalFovEdit = 308;
constexpr int kHeadsetVerticalFovEdit = 309;
constexpr int kHeadsetUpscalerEdit = 310;
constexpr int kFullscreenCheck = 401;
constexpr int kEngineFovCheck = 402;
constexpr int kDepthStereoCheck = 403;
constexpr int kVehicleVisibilityCheck = 404;
constexpr int kMatchHeadsetFovCheck = 405;
constexpr int kLaunchButton = 501;
constexpr int kRecenterButton = 502;
constexpr int kSteamLink = 601;
constexpr int kQuestLink = 602;

struct SliderBinding {
    int sliderId;
    int editId;
    int minimum;
    int maximum;
    int VrSettings::*value;
    const wchar_t* label;
    int y;
};

constexpr std::array<SliderBinding, 10> kSliders{{
    {kHeadsetUpscalerSlider, kHeadsetUpscalerEdit, 0, 100,
     &VrSettings::headsetUpscalerSharpnessPercent, L"Headset Upscaler", 257},
    {kHeadsetHorizontalFovSlider, kHeadsetHorizontalFovEdit, 50, 150,
     &VrSettings::headsetFullscreenHorizontalFovPercent, L"Headset H FOV", 287},
    {kHeadsetVerticalFovSlider, kHeadsetVerticalFovEdit, 50, 150,
     &VrSettings::headsetFullscreenVerticalFovPercent, L"Headset V FOV", 317},
    {kDepthStrengthSlider, kDepthStrengthEdit, 0, 600, &VrSettings::depthStereoStrengthPercent, L"Depth stereo", 347},
    {kEngineFovSlider, kEngineFovEdit, 60, 150, &VrSettings::engineVerticalFovDegrees, L"Engine FOV", 377},
    {kRotationSlider, kRotationEdit, 50, 250, &VrSettings::headRotationGainPercent, L"Head rotation", 407},
    {kTranslationSlider, kTranslationEdit, 0, 200, &VrSettings::headTranslationPercent, L"Head 6DoF", 437},
    {kSeatRightSlider, kSeatRightEdit, -100, 100, &VrSettings::cameraOffsetRightCentimetres, L"Seat X", 467},
    {kSeatUpSlider, kSeatUpEdit, -100, 100, &VrSettings::cameraOffsetUpCentimetres, L"Seat Y", 497},
    {kSeatForwardSlider, kSeatForwardEdit, -100, 100, &VrSettings::cameraOffsetForwardCentimetres, L"Seat Z", 527},
}};

HBRUSH gBackgroundBrush = CreateSolidBrush(kBackground);
HBRUSH gInputBrush = CreateSolidBrush(kInputBackground);
HFONT gTitleFont = nullptr;
HFONT gBodyFont = nullptr;
HFONT gSmallFont = nullptr;
HFONT gLinkFont = nullptr;

struct ControlState {
    VrSettings settings{LoadVrSettings()};
    HWND status{};
};

void SetControlFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND AddText(HWND parent, const wchar_t* text, const int x, const int y, const int width, const int height,
             const DWORD style = SS_LEFT, HFONT font = nullptr) {
    HWND label = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | style,
                                 x, y, width, height, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(label, font == nullptr ? gBodyFont : font);
    return label;
}

HWND AddEdit(HWND parent, const int id, const int value, const int x, const int y, const int width) {
    HWND edit = CreateWindowExW(0, L"EDIT", std::to_wstring(value).c_str(),
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_RIGHT,
                                x, y, width, 25, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                GetModuleHandleW(nullptr), nullptr);
    SendMessageW(edit, EM_SETLIMITTEXT, 6, 0);
    SetWindowTheme(edit, L"DarkMode_Explorer", nullptr);
    SetControlFont(edit, gBodyFont);
    return edit;
}

HWND AddSlider(HWND parent, const SliderBinding& binding, const int value) {
    HWND slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_NOTICKS | WS_TABSTOP,
                                  175, binding.y - 2, 270, 27, parent,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(binding.sliderId)),
                                  GetModuleHandleW(nullptr), nullptr);
    SendMessageW(slider, TBM_SETRANGEMIN, FALSE, binding.minimum);
    SendMessageW(slider, TBM_SETRANGEMAX, TRUE, binding.maximum);
    SendMessageW(slider, TBM_SETPOS, TRUE, value);
    SetWindowTheme(slider, L"DarkMode_Explorer", nullptr);
    return slider;
}

HWND AddToggle(HWND parent, const int id, const wchar_t* text, const int y) {
    HWND toggle = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                  24, y, 510, 25, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  GetModuleHandleW(nullptr), nullptr);
    SetControlFont(toggle, gBodyFont);
    return toggle;
}

HWND AddButton(HWND parent, const int id, const wchar_t* text, const int y, const int height) {
    HWND button = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                  24, y, 510, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  GetModuleHandleW(nullptr), nullptr);
    SetControlFont(button, gBodyFont);
    return button;
}

HWND AddLink(HWND parent, const int id, const wchar_t* text, const int x, const int y, const int width) {
    HWND link = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | SS_NOTIFY,
                                x, y, width, 20, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                GetModuleHandleW(nullptr), nullptr);
    SetControlFont(link, gLinkFont);
    return link;
}

const SliderBinding* FindSliderByControl(const int id) {
    for (const SliderBinding& binding : kSliders) {
        if (binding.sliderId == id || binding.editId == id) return &binding;
    }
    return nullptr;
}

int ReadInteger(HWND window, const int id, const int fallback, const int minimum, const int maximum) {
    wchar_t text[16]{};
    GetDlgItemTextW(window, id, text, static_cast<int>(std::size(text)));
    wchar_t* end = nullptr;
    const long value = wcstol(text, &end, 10);
    if (end == text || *end != L'\0') return fallback;
    return std::clamp(static_cast<int>(value), minimum, maximum);
}

void SetIntegerText(HWND window, const int id, const int value) {
    SetDlgItemTextW(window, id, std::to_wstring(value).c_str());
}

void Save(ControlState& state, HWND window, const wchar_t* status = L"Saved") {
    SaveVrSettings(state.settings);
    if (state.status != nullptr) SetWindowTextW(state.status, status);
    (void)window;
}

void CommitSliderEdit(HWND window, ControlState& state, const SliderBinding& binding) {
    const int value = ReadInteger(window, binding.editId, state.settings.*(binding.value), binding.minimum, binding.maximum);
    state.settings.*(binding.value) = value;
    SetIntegerText(window, binding.editId, value);
    SendDlgItemMessageW(window, binding.sliderId, TBM_SETPOS, TRUE, value);
    Save(state, window);
}

void CommitResolution(HWND window, ControlState& state) {
    state.settings.gameResolutionWidth = ReadInteger(window, kResolutionWidthEdit, state.settings.gameResolutionWidth, 320, 16384);
    state.settings.gameResolutionHeight = ReadInteger(window, kResolutionHeightEdit, state.settings.gameResolutionHeight, 320, 16384);
    SetIntegerText(window, kResolutionWidthEdit, state.settings.gameResolutionWidth);
    SetIntegerText(window, kResolutionHeightEdit, state.settings.gameResolutionHeight);
    Save(state, window, L"Resolution saved for next launch");
}

bool IsToggleEnabled(const ControlState& state, const int id) {
    switch (id) {
    case kFullscreenCheck: return state.settings.headsetFullscreenEnabled != 0;
    case kEngineFovCheck: return state.settings.engineFovOverrideEnabled != 0;
    case kMatchHeadsetFovCheck: return state.settings.matchEngineFovToHeadset != 0;
    case kDepthStereoCheck: return state.settings.depthStereoEnabled != 0;
    case kVehicleVisibilityCheck: return state.settings.cullTransformSplitEnabled != 0;
    default: return false;
    }
}

void ToggleSetting(ControlState& state, const int id) {
    switch (id) {
    case kFullscreenCheck: state.settings.headsetFullscreenEnabled = state.settings.headsetFullscreenEnabled == 0 ? 1 : 0; break;
    case kEngineFovCheck: state.settings.engineFovOverrideEnabled = state.settings.engineFovOverrideEnabled == 0 ? 1 : 0; break;
    case kMatchHeadsetFovCheck: state.settings.matchEngineFovToHeadset = state.settings.matchEngineFovToHeadset == 0 ? 1 : 0; break;
    case kDepthStereoCheck:
        state.settings.depthStereoEnabled = state.settings.depthStereoEnabled == 0 ? 1 : 0;
        state.settings.alternateFrameStereoEnabled = 0;
        break;
    case kVehicleVisibilityCheck: state.settings.cullTransformSplitEnabled = state.settings.cullTransformSplitEnabled == 0 ? 1 : 0; break;
    default: return;
    }
}

std::filesystem::path CurrentExecutableDirectory() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return std::filesystem::current_path();
    return std::filesystem::path(std::wstring(path.data(), length)).parent_path();
}

void LaunchVrGame(HWND window, ControlState& state) {
    CommitResolution(window, state);
    const std::filesystem::path directory = CurrentExecutableDirectory();
    const std::filesystem::path launcher = directory / L"NFSHeatVRLauncher.exe";
    if (!std::filesystem::is_regular_file(launcher)) {
        MessageBoxW(window, L"NFSHeatVRLauncher.exe was not found beside this panel.", L"NFS Heat VR", MB_OK | MB_ICONERROR);
        return;
    }
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", launcher.c_str(), nullptr,
                                                                  directory.c_str(), SW_SHOWNORMAL));
    if (result <= 32) {
        MessageBoxW(window, L"The VR launcher could not be started.", L"NFS Heat VR", MB_OK | MB_ICONERROR);
        return;
    }
    // The launcher is now running in its own process.  Fully close this
    // settings panel instead of merely hiding it in the background.
    DestroyWindow(window);
}

void DrawOwnedControl(HWND parent, const DRAWITEMSTRUCT& item) {
    const int id = static_cast<int>(item.CtlID);
    const RECT& bounds = item.rcItem;
    HDC dc = item.hDC;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    SetBkMode(dc, TRANSPARENT);

    if (id >= kFullscreenCheck && id <= kMatchHeadsetFovCheck) {
        FillRect(dc, &bounds, gBackgroundBrush);
        const RECT box{bounds.left + 2, bounds.top + 4, bounds.left + 18, bounds.top + 20};
        HBRUSH checkBrush = CreateSolidBrush(kAccent);
        FrameRect(dc, &box, checkBrush);
        auto* state = reinterpret_cast<ControlState*>(GetWindowLongPtrW(parent, GWLP_USERDATA));
        if (state != nullptr && IsToggleEnabled(*state, id)) {
            FillRect(dc, &box, checkBrush);
            SetTextColor(dc, RGB(255, 255, 255));
            SelectObject(dc, gSmallFont);
            RECT markBounds = box;
            DrawTextW(dc, L"X", 1, &markBounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        DeleteObject(checkBrush);
        wchar_t text[160]{};
        GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
        RECT textBounds{bounds.left + 27, bounds.top, bounds.right, bounds.bottom};
        SetTextColor(dc, kText);
        SelectObject(dc, gBodyFont);
        DrawTextW(dc, text, -1, &textBounds, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    HBRUSH fill = CreateSolidBrush(pressed ? kButtonPressed : kButtonBackground);
    FillRect(dc, &bounds, fill);
    DeleteObject(fill);
    HBRUSH border = CreateSolidBrush(kMutedText);
    FrameRect(dc, &bounds, border);
    DeleteObject(border);
    wchar_t text[160]{};
    GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
    SetTextColor(dc, kText);
    SelectObject(dc, gBodyFont);
    RECT textBounds = bounds;
    DrawTextW(dc, text, -1, &textBounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<ControlState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE: {
        auto* created = new ControlState{};
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));

        AddText(window, L"NFS Heat VR", 24, 15, 250, 27, SS_LEFT, gTitleFont);
        created->status = AddText(window, L"Direct Heat settings", 24, 45, 510, 18, SS_LEFT, gSmallFont);
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                        24, 70, 510, 2, window, nullptr, GetModuleHandleW(nullptr), nullptr);

        AddText(window, L"Game resolution", 24, 87, 145, 20);
        AddEdit(window, kResolutionWidthEdit, created->settings.gameResolutionWidth, 176, 84, 70);
        AddText(window, L"x", 250, 87, 16, 20, SS_CENTER);
        AddEdit(window, kResolutionHeightEdit, created->settings.gameResolutionHeight, 270, 84, 70);
        AddText(window, L"next launch", 365, 87, 105, 20, SS_LEFT, gSmallFont);

        AddToggle(window, kFullscreenCheck, L"Headset fullscreen", 119);
        AddToggle(window, kEngineFovCheck, L"Real engine FOV", 146);
        AddToggle(window, kMatchHeadsetFovCheck, L"Match engine FOV to HMD", 173);
        AddToggle(window, kDepthStereoCheck, L"Depth stereo", 200);
        AddToggle(window, kVehicleVisibilityCheck, L"Keep vehicle visible", 227);

        for (const SliderBinding& binding : kSliders) {
            AddText(window, binding.label, 24, binding.y + 2, 140, 20);
            AddSlider(window, binding, created->settings.*(binding.value));
            AddEdit(window, binding.editId, created->settings.*(binding.value), 454, binding.y, 80);
        }

        AddText(window, L"Upscaler 0 = off. FOV and camera settings save live. Resolution applies when the game starts.",
                24, 562, 510, 17, SS_LEFT, gSmallFont);
        AddButton(window, kLaunchButton, L"Launch NFS Heat VR", 588, 35);
        AddButton(window, kRecenterButton, L"Recenter headset (F9)", 632, 31);
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                        24, 684, 510, 2, window, nullptr, GetModuleHandleW(nullptr), nullptr);
        AddText(window, L"Created by: TOM REASON", 24, 699, 510, 20, SS_LEFT, gBodyFont);
        AddText(window, L"Support the developer by purchasing the game CYBRID", 24, 726, 510, 20, SS_LEFT, gBodyFont);
        AddLink(window, kSteamLink, L"STEAM VR", 24, 754, 70);
        AddText(window, L"or", 102, 754, 18, 20, SS_LEFT, gBodyFont);
        AddLink(window, kQuestLink, L"QUEST STORE", 137, 754, 105);
        return 0;
    }
    case WM_HSCROLL:
        if (state != nullptr && lParam != 0) {
            const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
            if (const SliderBinding* binding = FindSliderByControl(id)) {
                state->settings.*(binding->value) = static_cast<int>(SendDlgItemMessageW(window, binding->sliderId, TBM_GETPOS, 0, 0));
                SetIntegerText(window, binding->editId, state->settings.*(binding->value));
                Save(*state, window);
            }
        }
        return 0;
    case WM_COMMAND:
        if (state == nullptr) return 0;
        if (const int id = LOWORD(wParam); id == kLaunchButton && HIWORD(wParam) == BN_CLICKED) {
            LaunchVrGame(window, *state);
            return 0;
        } else if (id == kRecenterButton && HIWORD(wParam) == BN_CLICKED) {
            state->settings.recenterRequestId = state->settings.recenterRequestId >= 1000000000
                ? 0 : state->settings.recenterRequestId + 1;
            Save(*state, window, L"Recenter requested");
            return 0;
        } else if (id == kSteamLink && HIWORD(wParam) == STN_CLICKED) {
            ShellExecuteW(window, L"open", L"https://store.steampowered.com/app/1636850/CYBRID/", nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        } else if (id == kQuestLink && HIWORD(wParam) == STN_CLICKED) {
            ShellExecuteW(window, L"open", L"https://www.meta.com/en-gb/experiences/cybrid/24008454042113681/", nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        } else if (id >= kFullscreenCheck && id <= kMatchHeadsetFovCheck && HIWORD(wParam) == BN_CLICKED) {
            ToggleSetting(*state, id);
            Save(*state, window);
            InvalidateRect(GetDlgItem(window, id), nullptr, TRUE);
            return 0;
        } else if (id == kResolutionWidthEdit || id == kResolutionHeightEdit) {
            if (HIWORD(wParam) == EN_KILLFOCUS) CommitResolution(window, *state);
            return 0;
        } else if (const SliderBinding* binding = FindSliderByControl(id); binding != nullptr && HIWORD(wParam) == EN_KILLFOCUS) {
            CommitSliderEdit(window, *state, *binding);
            return 0;
        }
        return 0;
    case WM_DRAWITEM:
        if (lParam != 0) DrawOwnedControl(window, *reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        const int controlId = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
        SetTextColor(dc, controlId == kSteamLink || controlId == kQuestLink ? kAccent : kText);
        SetBkColor(dc, kBackground);
        return reinterpret_cast<INT_PTR>(gBackgroundBrush);
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kText);
        SetBkColor(dc, kInputBackground);
        return reinterpret_cast<INT_PTR>(gInputBrush);
    }
    case WM_ERASEBKGND: {
        RECT client{};
        GetClientRect(window, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client, gBackgroundBrush);
        return TRUE;
    }
    case WM_DESTROY:
        delete state;
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    gTitleFont = CreateFontW(22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    gBodyFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    gSmallFont = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    gLinkFont = CreateFontW(14, 0, 0, 0, FW_SEMIBOLD, FALSE, TRUE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    const wchar_t windowClass[] = L"NFSHeatVRControlWindow";
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = instance;
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_NFS_HEAT_VR));
    cls.hIconSm = cls.hIcon;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = gBackgroundBrush;
    cls.lpszClassName = windowClass;
    cls.lpfnWndProc = WindowProc;
    RegisterClassExW(&cls);

    HWND window = CreateWindowExW(WS_EX_APPWINDOW, windowClass, L"NFS Heat VR - Settings",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 580, 840, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) return 1;
    ShowWindow(window, showCommand);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    DeleteObject(gTitleFont);
    DeleteObject(gBodyFont);
    DeleteObject(gSmallFont);
    DeleteObject(gLinkFont);
    DeleteObject(gInputBrush);
    DeleteObject(gBackgroundBrush);
    return static_cast<int>(message.wParam);
}
