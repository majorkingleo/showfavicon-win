#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <objbase.h>

#include "http.h"
#include "icon.h"
#include "util.h"

namespace {

constexpr wchar_t kWindowClassName[] = L"ShowFaviconMainWindow";
constexpr UINT kTrayCallbackMsg = WM_APP + 1;

std::wstring g_siteUrl;
NOTIFYICONDATAW g_nid = {};
bool g_ownsIcon = false;

void openSite() {
    ShellExecuteW(nullptr, L"open", g_siteUrl.c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
}

void showTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Open");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2, L"Exit");

    POINT pt = {};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                              pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == 1)
        openSite();
    else if (cmd == 2)
        DestroyWindow(hwnd);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case kTrayCallbackMsg:
            if (lParam == WM_LBUTTONUP) {
                openSite();
            } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                showTrayMenu(hwnd);
            }
            return 0;
        case WM_DESTROY:
            if (g_nid.cbSize)
                Shell_NotifyIconW(NIM_DELETE, &g_nid);
            if (g_ownsIcon && g_nid.hIcon)
                DestroyIcon(g_nid.hIcon);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                    PWSTR /*pCmdLine*/, int /*nCmdShow*/) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    auto sites = sf::loadSites();
    if (sites.empty())
        sites.push_back(L"https://github.com/");
    g_siteUrl = sites[0];

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClassName;

    if (!RegisterClassExW(&wc)) {
        CoUninitialize();
        return 1;
    }

    // Real top-level window (not a message-only window) so that broadcasts
    // such as "TaskbarCreated" can be received later. It is never shown.
    HWND hwnd = CreateWindowExW(
        0, kWindowClassName, L"ShowFavicon",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        CoUninitialize();
        return 1;
    }

    // Milestones 1+2: fetch + decode synchronously at startup. On failure,
    // show the last known icon grayed (if cached) and set the failure flag.
    std::wstring host = sf::hostFromUrl(g_siteUrl);
    if (host.empty()) host = L"site";
    std::wstring cacheFile = sf::iconCacheDir() + L"\\" + host + L".png";

    sf::RgbaImage img;
    if (sf::fetchIconForSite(g_siteUrl, img)) {
        sf::writePngFile(img, cacheFile);
        sf::setFailureFlag(cacheFile, false);
        g_nid.hIcon = sf::imageToHicon(img);
        g_ownsIcon = (g_nid.hIcon != nullptr);
    } else {
        sf::RgbaImage cached;
        if (sf::loadPngFile(cacheFile, cached)) {
            sf::grayscale(cached, 0.55f);
            g_nid.hIcon = sf::imageToHicon(cached);
            g_ownsIcon = (g_nid.hIcon != nullptr);
        }
        sf::setFailureFlag(cacheFile, true);
    }
    if (!g_nid.hIcon)
        g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = kTrayCallbackMsg;

    std::wstring tip = sf::hostFromUrl(g_siteUrl);
    if (tip.empty()) tip = L"ShowFavicon";
    lstrcpynW(g_nid.szTip, tip.c_str(), sizeof(g_nid.szTip) / sizeof(g_nid.szTip[0]));
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
