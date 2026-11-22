// Pip-Net proof-of-concept for 32-bit Fallout: New Vegas (runtime 1.4.0.525).
// Independent WebView2 HWND overlay (not a texture in the 3D Pip-Boy mesh).
// Uses an STA UI thread; no NVSE opcodes are registered.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <wrl.h>
#include <WebView2.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <string>
// xNVSE uses unqualified min/max in legacy headers; keep Windows macros disabled.
using std::max;
using std::min;
#include "nvse/PluginAPI.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT kToggleMessage = WM_APP + 201;
constexpr UINT kStopMessage   = WM_APP + 202;
constexpr UINT_PTR kCheckTimer = 1;
constexpr int kButtonBack = 101;
constexpr int kButtonForward = 102;
constexpr int kButtonReload = 103;
constexpr int kAddress = 104;
constexpr int kGo = 105;
constexpr int kClose = 106;
constexpr wchar_t kWindowClass[] = L"PipNetBrowserWindow";

// Verified in xNVSE GameUI.cpp for the 32-bit 1.4.0.525 runtime.
constexpr uintptr_t kMenuVisibilityBase = 0x011F308F;
constexpr unsigned kInventoryMenu = 1002;
constexpr unsigned kStatsMenu = 1003;
constexpr unsigned kMapDataMenu = 1023;

std::atomic<bool> g_wantOpen{false};
std::atomic<bool> g_started{false};
std::atomic<bool> g_shuttingDown{false};
std::atomic<HWND> g_uiWindow{nullptr};
std::atomic<bool> g_keyWasDown{false};
HANDLE g_thread = nullptr;
HWND g_gameWindow = nullptr;
HMODULE g_module = nullptr;

struct Config {
    int key = VK_F9;
    int x = 36; int y = 16; int width = 60; int height = 70;
    std::wstring homepage = L"https://www.wikipedia.org/";
};
Config g_config;

// The following handles/COM objects belong *only* to the browser UI thread.
HWND g_host = nullptr;
HWND g_address = nullptr;
HWND g_back = nullptr;
HWND g_forward = nullptr;
HWND g_reload = nullptr;
HWND g_go = nullptr;
HWND g_close = nullptr;
ComPtr<ICoreWebView2Environment> g_environment;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
HBRUSH g_darkBrush = nullptr;

void Log(const wchar_t* msg) {
    OutputDebugStringW(L"[PipNet] ");
    OutputDebugStringW(msg);
    OutputDebugStringW(L"\n");
}

bool IsPipboyOpen() {
    // Only accessed after the NVSE runtime-version check in NVSEPlugin_Query.
    auto base = reinterpret_cast<volatile const unsigned char*>(kMenuVisibilityBase);
    return base[kInventoryMenu] != 0 || base[kStatsMenu] != 0 || base[kMapDataMenu] != 0;
}

BOOL CALLBACK FindWindowForCurrentProcess(HWND hwnd, LPARAM param) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || GetWindow(hwnd, GW_OWNER) || !IsWindowVisible(hwnd))
        return TRUE;
    RECT rect{};
    if (!GetClientRect(hwnd, &rect) || rect.right - rect.left < 400)
        return TRUE;
    *reinterpret_cast<HWND*>(param) = hwnd;
    return FALSE;
}

HWND FindGameWindow() {
    HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &pid);
    if (pid == GetCurrentProcessId()) return GetAncestor(foreground, GA_ROOT);
    HWND found = nullptr;
    EnumWindows(FindWindowForCurrentProcess, reinterpret_cast<LPARAM>(&found));
    return found;
}

std::wstring GameDirectory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    auto last = p.find_last_of(L"\\/");
    return last == std::wstring::npos ? L"." : p.substr(0, last);
}

void ReadConfiguration() {
    const std::wstring ini = GameDirectory() + L"\\Data\\NVSE\\Plugins\\PipNet.ini";
    auto read = [&](const wchar_t* key, int value) -> int {
        return GetPrivateProfileIntW(L"Browser", key, value, ini.c_str());
    };
    g_config.key = std::clamp(read(L"Hotkey", VK_F9), 1, 255);
    g_config.x = std::clamp(read(L"XPercent", 36), 0, 90);
    g_config.y = std::clamp(read(L"YPercent", 16), 0, 90);
    g_config.width = std::clamp(read(L"WidthPercent", 60), 15, 100);
    g_config.height = std::clamp(read(L"HeightPercent", 70), 15, 100);
    wchar_t url[2048]{};
    GetPrivateProfileStringW(L"Browser", L"HomePage", L"https://www.wikipedia.org/",
                             url, 2048, ini.c_str());
    g_config.homepage = url;
}

std::wstring DataDirectory() {
    wchar_t base[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE,
                                nullptr, SHGFP_TYPE_CURRENT, base))) {
        GetTempPathW(MAX_PATH, base);
    }
    std::wstring app = std::wstring(base) + L"\\PipNet";
    CreateDirectoryW(app.c_str(), nullptr);
    return app + L"\\WebView2";
}

bool IsAllowedUrl(const std::wstring& url) {
    if (url.empty() || url.size() > 2048) return false;
    std::wstring lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return lower.rfind(L"https://", 0) == 0 || lower.rfind(L"http://", 0) == 0;
}

void ResizeControls() {
    if (!g_host) return;
    RECT rect{}; GetClientRect(g_host, &rect);
    const int w = rect.right;
    const int h = rect.bottom;
    const int margin = 7, toolbarH = 36;
    if (w < 250 || h < 100) return;
    int x = margin;
    MoveWindow(g_back, x, 5, 32, 26, TRUE); x += 36;
    MoveWindow(g_forward, x, 5, 32, 26, TRUE); x += 36;
    MoveWindow(g_reload, x, 5, 32, 26, TRUE); x += 36;
    MoveWindow(g_address, x, 5, std::max(40, w - x - 84), 26, TRUE);
    MoveWindow(g_go, w - 80, 5, 34, 26, TRUE);
    MoveWindow(g_close, w - 42, 5, 34, 26, TRUE);
    if (g_controller) {
        RECT bounds{margin, toolbarH, w - margin, h - margin};
        g_controller->put_Bounds(bounds);
    }
}

void PositionOverlay() {
    if (!g_host || !g_gameWindow || !IsWindow(g_gameWindow)) return;
    RECT client{};
    if (!GetClientRect(g_gameWindow, &client)) return;
    POINT topLeft{0,0};
    if (!ClientToScreen(g_gameWindow, &topLeft)) return;
    const int gameW = client.right - client.left;
    const int gameH = client.bottom - client.top;
    if (gameW < 400 || gameH < 300) return;
    int x = g_config.x * gameW / 100;
    int y = g_config.y * gameH / 100;
    int w = std::min(g_config.width * gameW / 100, gameW - x);
    int h = std::min(g_config.height * gameH / 100, gameH - y);
    RECT current{}; GetWindowRect(g_host, &current);
    if (current.left != topLeft.x + x || current.top != topLeft.y + y ||
        current.right - current.left != w || current.bottom - current.top != h) {
        SetWindowPos(g_host, nullptr, topLeft.x + x, topLeft.y + y, w, h,
                     SWP_NOACTIVATE | SWP_NOZORDER);
    }
}

void CloseOverlay(bool restoreGameFocus) {
    g_wantOpen.store(false);
    if (g_controller) g_controller->put_IsVisible(FALSE);
    if (g_host) ShowWindow(g_host, SW_HIDE);
    if (restoreGameFocus && IsWindow(g_gameWindow))
        SetForegroundWindow(g_gameWindow);
}

void OpenOverlay() {
    if (!g_host || !IsPipboyOpen()) {
        g_wantOpen.store(false); return;
    }
    PositionOverlay();
    ShowWindow(g_host, SW_SHOW);
    if (g_controller) g_controller->put_IsVisible(TRUE);
    SetForegroundWindow(g_host);
    if (g_address) SetFocus(g_address);
}

void NavigateAddress() {
    if (!g_webview || !g_address) return;
    wchar_t url[2048]{};
    GetWindowTextW(g_address, url, 2048);
    std::wstring target = url;
    if (target.find(L"://") == std::wstring::npos) target = L"https://" + target;
    if (!IsAllowedUrl(target)) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    g_webview->Navigate(target.c_str());
}

void BeginWebView() {
    if (!g_host) return;
    const std::wstring userFolder = DataDirectory();
    auto completeEnvironment = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result) || !env) {
                Log(L"WebView2 environment failed; check Evergreen Runtime installation.");
                if (g_host) SetWindowTextW(g_host, L"Pip-Net - WebView2 initialization failed");
                return S_OK;
            }
            g_environment = env;
            auto completeController = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [](HRESULT hr, ICoreWebView2Controller* controller) -> HRESULT {
                    if (FAILED(hr) || !controller || !g_host) {
                        Log(L"WebView2 controller creation failed.");
                        return S_OK;
                    }
                    g_controller = controller;
                    if (FAILED(controller->get_CoreWebView2(g_webview.GetAddressOf())) || !g_webview) {
                        Log(L"WebView2 browser object unavailable."); return S_OK;
                    }
                    EventRegistrationToken token{};
                    g_controller->add_AcceleratorKeyPressed(
                        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
                            [](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* e)->HRESULT {
                                UINT key = 0;
                                COREWEBVIEW2_KEY_EVENT_KIND kind{};
                                e->get_VirtualKey(&key); e->get_KeyEventKind(&kind);
                                if (key == static_cast<UINT>(g_config.key) &&
                                    (kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN ||
                                     kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)) {
                                    COREWEBVIEW2_PHYSICAL_KEY_STATUS status{};
                                    e->get_PhysicalKeyStatus(&status);
                                    e->put_Handled(TRUE);
                                    if (!status.WasKeyDown && g_host)
                                        PostMessageW(g_host, kToggleMessage, 0, 0);
                                }
                                return S_OK;
                            }).Get(), &token);
                    g_webview->add_SourceChanged(
                        Callback<ICoreWebView2SourceChangedEventHandler>(
                            [](ICoreWebView2* sender, ICoreWebView2SourceChangedEventArgs*)->HRESULT {
                                LPWSTR source = nullptr;
                                if (SUCCEEDED(sender->get_Source(&source)) && source) {
                                    if (g_address) SetWindowTextW(g_address, source);
                                    CoTaskMemFree(source);
                                }
                                return S_OK;
                            }).Get(), &token);
                    g_webview->add_NavigationStarting(
                        Callback<ICoreWebView2NavigationStartingEventHandler>(
                            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* e)->HRESULT {
                                LPWSTR uri = nullptr;
                                if (SUCCEEDED(e->get_Uri(&uri)) && uri) {
                                    if (!IsAllowedUrl(uri)) e->put_Cancel(TRUE);
                                    CoTaskMemFree(uri);
                                }
                                return S_OK;
                            }).Get(), &token);
                    g_webview->add_PermissionRequested(
                        Callback<ICoreWebView2PermissionRequestedEventHandler>(
                            [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* e)->HRESULT {
                                e->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                                return S_OK;
                            }).Get(), &token);
                    ResizeControls();
                    g_controller->put_IsVisible(g_wantOpen.load() ? TRUE : FALSE);
                    if (IsAllowedUrl(g_config.homepage))
                        g_webview->Navigate(g_config.homepage.c_str());
                    return S_OK;
                });
            HRESULT createResult = env->CreateCoreWebView2Controller(g_host, completeController.Get());
            if (FAILED(createResult)) Log(L"CreateCoreWebView2Controller failed.");
            return S_OK;
        });
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, userFolder.c_str(),
                                                           nullptr, completeEnvironment.Get());
    if (FAILED(hr)) Log(L"CreateCoreWebView2EnvironmentWithOptions failed.");
}

LRESULT CALLBACK BrowserWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_host = hwnd;
        g_darkBrush = CreateSolidBrush(RGB(10, 25, 13));
        const DWORD buttonStyle = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON;
        g_back    = CreateWindowW(L"BUTTON", L"<", buttonStyle, 0,0,0,0,hwnd,(HMENU)kButtonBack,g_module,nullptr);
        g_forward = CreateWindowW(L"BUTTON", L">", buttonStyle, 0,0,0,0,hwnd,(HMENU)kButtonForward,g_module,nullptr);
        g_reload  = CreateWindowW(L"BUTTON", L"R", buttonStyle, 0,0,0,0,hwnd,(HMENU)kButtonReload,g_module,nullptr);
        g_address = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,
                                    0,0,0,0,hwnd,(HMENU)kAddress,g_module,nullptr);
        g_go      = CreateWindowW(L"BUTTON", L"GO", buttonStyle,0,0,0,0,hwnd,(HMENU)kGo,g_module,nullptr);
        g_close   = CreateWindowW(L"BUTTON", L"X", buttonStyle,0,0,0,0,hwnd,(HMENU)kClose,g_module,nullptr);
        SetWindowTextW(g_address, g_config.homepage.c_str());
        SetTimer(hwnd, kCheckTimer, 100, nullptr);
        return 0;
    }
    case WM_SIZE: ResizeControls(); return 0;
    case WM_COMMAND: {
        switch (LOWORD(wp)) {
        case kButtonBack: if (g_webview) g_webview->GoBack(); return 0;
        case kButtonForward: if (g_webview) g_webview->GoForward(); return 0;
        case kButtonReload: if (g_webview) g_webview->Reload(); return 0;
        case kGo: NavigateAddress(); return 0;
        case kClose: CloseOverlay(true); return 0;
        default: break;
        }
        break;
    }
    case WM_KEYDOWN:
        if (wp == static_cast<WPARAM>(g_config.key) || wp == VK_ESCAPE) {
            CloseOverlay(true); return 0;
        }
        if (wp == VK_RETURN && GetFocus() == g_address) {
            NavigateAddress(); return 0;
        }
        break;
    case WM_TIMER:
        if (wp == kCheckTimer) {
            if (g_wantOpen.load() && !IsPipboyOpen()) CloseOverlay(false);
            if (g_wantOpen.load() && (!IsWindowVisible(g_gameWindow) || IsIconic(g_gameWindow)))
                CloseOverlay(false);
            if (g_wantOpen.load()) PositionOverlay();
            return 0;
        }
        break;
    case kToggleMessage:
        if (wp == 0) CloseOverlay(true);
        else if (g_wantOpen.load()) OpenOverlay();
        return 0;
    case kStopMessage:
        DestroyWindow(hwnd); return 0;
    case WM_CLOSE:
        CloseOverlay(true); return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wp), RGB(110, 240, 135));
        SetBkColor(reinterpret_cast<HDC>(wp), RGB(10, 25, 13));
        return reinterpret_cast<LRESULT>(g_darkBrush);
    case WM_DESTROY:
        KillTimer(hwnd, kCheckTimer);
        if (g_controller) g_controller->Close();
        g_webview.Reset(); g_controller.Reset(); g_environment.Reset();
        if (g_darkBrush) { DeleteObject(g_darkBrush); g_darkBrush = nullptr; }
        g_uiWindow.store(nullptr); g_host = nullptr;
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

DWORD WINAPI BrowserThread(void*) {
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) {
        Log(L"Failed to initialize COM STA for WebView2.");
        g_started.store(false); g_wantOpen.store(false);
        return 0;
    }
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls); cls.lpfnWndProc = BrowserWndProc;
    cls.hInstance = g_module; cls.lpszClassName = kWindowClass;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&cls);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"Pip-Net [F9 to close]",
                                WS_POPUP | WS_BORDER | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 900, 600,
                                g_gameWindow, nullptr, g_module, nullptr);
    if (!hwnd) {
        Log(L"Failed to create Pip-Net overlay window.");
        g_started.store(false); g_wantOpen.store(false);
        CoUninitialize(); return 0;
    }
    g_uiWindow.store(hwnd);
    BeginWebView();
    if (g_wantOpen.load()) OpenOverlay();
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        // Enter in the URL bar even while focus is on an EDIT control.
        if (message.message == WM_KEYDOWN && message.hwnd == g_address &&
            message.wParam == VK_RETURN) { NavigateAddress(); continue; }
        if (message.message == WM_KEYDOWN && message.wParam == static_cast<WPARAM>(g_config.key) &&
            message.hwnd != g_host) { CloseOverlay(true); continue; }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    g_started.store(false);
    CoUninitialize();
    return 0;
}

void RequestToggle() {
    if (g_shuttingDown.load()) return;
    const bool show = !g_wantOpen.load();
    if (show && !IsPipboyOpen()) return;
    g_wantOpen.store(show);
    HWND hwnd = g_uiWindow.load();
    if (hwnd && IsWindow(hwnd)) {
        PostMessageW(hwnd, kToggleMessage, show ? 1 : 0, 0);
    } else if (show && !g_started.exchange(true)) {
        g_gameWindow = FindGameWindow();
        if (!g_gameWindow) {
            Log(L"Unable to find the FalloutNV main window.");
            g_started.store(false); g_wantOpen.store(false);
            return;
        }
        g_thread = CreateThread(nullptr, 0, BrowserThread, nullptr, 0, nullptr);
        if (!g_thread) {
            Log(L"Failed to start browser thread.");
            g_started.store(false); g_wantOpen.store(false);
        } else {
            CloseHandle(g_thread); g_thread = nullptr;
        }
    }
}

void MessageHandler(NVSEMessagingInterface::Message* message) {
    switch (message->type) {
    case NVSEMessagingInterface::kMessage_MainGameLoop: {
        if (g_shuttingDown.load()) return;
        const bool down = (GetAsyncKeyState(g_config.key) & 0x8000) != 0;
        const bool before = g_keyWasDown.exchange(down);
        if (down && !before && IsPipboyOpen()) RequestToggle();
        break;
    }
    case NVSEMessagingInterface::kMessage_ExitToMainMenu:
        g_wantOpen.store(false);
        if (HWND wnd = g_uiWindow.load()) PostMessageW(wnd, kToggleMessage, 0, 0);
        break;
    case NVSEMessagingInterface::kMessage_ExitGame:
    case NVSEMessagingInterface::kMessage_ExitGame_Console:
        g_shuttingDown.store(true);
        g_wantOpen.store(false);
        if (HWND wnd = g_uiWindow.load()) PostMessageW(wnd, kStopMessage, 0, 0);
        break;
    default: break;
    }
}
} // namespace

extern "C" __declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info) {
    info->infoVersion = PluginInfo::kInfoVersion;
    info->name = "PipNet";
    info->version = 1;
    return !nvse->isEditor && nvse->runtimeVersion == RUNTIME_VERSION_1_4_0_525;
}

extern "C" __declspec(dllexport) bool NVSEPlugin_Load(const NVSEInterface* nvse) {
    ReadConfiguration();
    auto* messaging = static_cast<NVSEMessagingInterface*>(nvse->QueryInterface(kInterface_Messaging));
    if (!messaging || messaging->version < 4) return false;
    return messaging->RegisterListener(nvse->GetPluginHandle(), "NVSE", MessageHandler);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
