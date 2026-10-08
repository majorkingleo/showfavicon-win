#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <wininet.h>
#include <iphlpapi.h>

#include "http.h"
#include "icon.h"
#include "logging.h"
#include "settings.h"
#include "util.h"

#include <CpputilsDebug.h>
#include <format.h>

#include <algorithm>
#include <climits>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kWindowClassName[] = L"ShowFaviconMainWindow";
constexpr UINT kTrayCallbackMsg = WM_APP + 1;
constexpr ULONGLONG kHourMs = 3600000ull;

struct Site {
    std::wstring url;
    std::wstring cacheFile;
    NOTIFYICONDATAW nid = {};
    HICON icon = nullptr;
    bool ownsIcon = false;
    ULONGLONG nextRefreshAt = 0;
    int failures = 0;
};

HINSTANCE g_hInstance = nullptr;
HWND g_hwnd = nullptr;
std::vector<Site> g_sites;
std::mutex g_sitesMutex;

HANDLE g_hStop = nullptr;     // manual-reset; set on shutdown
HANDLE g_hRefresh = nullptr;  // auto-reset; set on network/config change

bool networkAvailable() {
    return InternetGetConnectedState(nullptr, 0) != FALSE;
}

// True when `name` appears among the command-line arguments. `-d` is the one
// ShowFavicon reacts to: it turns on the console side of the logging.
bool hasFlag(int argc, wchar_t* const* argv, const wchar_t* name) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && lstrcmpiW(argv[i], name) == 0) return true;
    }
    return false;
}

ULONGLONG backoffMs(int failures) {
    if (failures <= 0) return kHourMs;
    if (failures > 6) return kHourMs;  // stop retrying until the next trigger
    return 30000ull * failures;        // 30s, 60s, ..., 180s
}

std::wstring cacheFileFor(const std::wstring& url) {
    std::wstring host = sf::hostFromUrl(url);
    if (host.empty()) host = L"site";
    return sf::iconCacheDir() + L"\\" + host + L".png";
}

void destroyOwnedIcon(Site& site) {
    if (site.ownsIcon && site.icon) {
        DestroyIcon(site.icon);
        site.icon = nullptr;
        site.ownsIcon = false;
    }
}

// Add a placeholder tray icon (uID = index + 1). Caller holds the lock.
void addSiteIcon(Site& site, size_t index) {
    site.nid = {};
    site.nid.cbSize = sizeof(site.nid);
    site.nid.hWnd = g_hwnd;
    site.nid.uID = static_cast<UINT>(index + 1);
    site.nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    site.nid.uCallbackMessage = kTrayCallbackMsg;
    site.nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    site.icon = site.nid.hIcon;
    site.ownsIcon = false;

    std::wstring tip = sf::hostFromUrl(site.url);
    if (tip.empty()) tip = L"ShowFavicon";
    lstrcpynW(site.nid.szTip, tip.c_str(),
              sizeof(site.nid.szTip) / sizeof(site.nid.szTip[0]));
    Shell_NotifyIconW(NIM_ADD, &site.nid);

    CPPDEBUG( Tools::format( "tray: added icon %u for %s",
                             site.nid.uID, sf::wideToUtf8(tip) ) );
}

// Remove and destroy all tray icons. Caller holds the lock.
void removeAllIcons() {
    CPPDEBUG( Tools::format( "tray: removing %d icon(s)",
                             static_cast<int>(g_sites.size()) ) );
    for (auto& site : g_sites) {
        if (site.nid.cbSize)
            Shell_NotifyIconW(NIM_DELETE, &site.nid);
        destroyOwnedIcon(site);
    }
    g_sites.clear();
}

// (Re)build the icon set from a list of URLs.
void rebuildSites(const std::vector<std::wstring>& urls) {
    CPPDEBUG( Tools::format( "config: rebuilding %d tray icon(s)",
                             static_cast<int>(urls.size()) ) );
    {
        std::lock_guard<std::mutex> lk(g_sitesMutex);
        removeAllIcons();
        g_sites.reserve(urls.size());
        for (size_t i = 0; i < urls.size(); ++i) {
            Site site;
            site.url = urls[i];
            site.cacheFile = cacheFileFor(urls[i]);
            addSiteIcon(site, i);
            site.nextRefreshAt = 0;  // fetch immediately
            g_sites.push_back(std::move(site));
        }
    }
    SetEvent(g_hRefresh);
}

void refreshSite(size_t idx) {
    std::wstring url;
    std::wstring cacheFile;
    UINT uID = 0;
    {
        std::lock_guard<std::mutex> lk(g_sitesMutex);
        if (idx >= g_sites.size()) return;
        url = g_sites[idx].url;
        cacheFile = g_sites[idx].cacheFile;
        uID = g_sites[idx].nid.uID;
    }

    CPPDEBUG( Tools::format( "refresh: site %u %s", uID, sf::wideToUtf8(url) ) );

    if (!networkAvailable()) {
        CPPDEBUG( Tools::format( "refresh: no network, site %u deferred", uID ) );
        std::lock_guard<std::mutex> lk(g_sitesMutex);
        if (idx < g_sites.size() && g_sites[idx].nid.uID == uID) {
            g_sites[idx].failures++;
            g_sites[idx].nextRefreshAt =
                GetTickCount64() + backoffMs(g_sites[idx].failures);
        }
        return;
    }

    sf::RgbaImage img;
    bool ok = sf::fetchOrCachedIcon(url, cacheFile, img);
    HICON icon = ok ? sf::imageToHicon(img) : nullptr;
    bool owns = (icon != nullptr);
    if (!icon) icon = LoadIcon(nullptr, IDI_APPLICATION);

    NOTIFYICONDATAW nid = {};
    {
        std::lock_guard<std::mutex> lk(g_sitesMutex);
        if (idx < g_sites.size() && g_sites[idx].nid.uID == uID) {
            Site& site = g_sites[idx];
            destroyOwnedIcon(site);
            site.icon = icon;
            site.ownsIcon = owns;
            site.nid.hIcon = icon;
            nid = site.nid;

            if (ok) {
                site.failures = 0;
                site.nextRefreshAt = GetTickCount64() + kHourMs;
            } else {
                site.failures++;
                site.nextRefreshAt = GetTickCount64() + backoffMs(site.failures);
            }
        }
    }
    CPPDEBUG( Tools::format( "refresh: site %u %s", uID,
                             ok ? "ok" : "failed" ) );

    if (nid.cbSize)
        Shell_NotifyIconW(NIM_MODIFY, &nid);
}

DWORD WINAPI workerProc(LPVOID) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    for (;;) {
        if (WaitForSingleObject(g_hStop, 0) == WAIT_OBJECT_0) break;

        ULONGLONG now = GetTickCount64();
        ULONGLONG earliest = ULLONG_MAX;
        std::vector<size_t> due;
        {
            std::lock_guard<std::mutex> lk(g_sitesMutex);
            for (size_t i = 0; i < g_sites.size(); ++i) {
                if (g_sites[i].nextRefreshAt <= now)
                    due.push_back(i);
                else if (g_sites[i].nextRefreshAt < earliest)
                    earliest = g_sites[i].nextRefreshAt;
            }
        }

        for (size_t i : due) refreshSite(i);

        DWORD waitMs = 1000;
        if (earliest != ULLONG_MAX && earliest > now)
            waitMs = static_cast<DWORD>(std::min<ULONGLONG>(earliest - now, 60000));

        HANDLE hs[2] = {g_hStop, g_hRefresh};
        DWORD w = WaitForMultipleObjects(2, hs, FALSE, waitMs);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_OBJECT_0 + 1) {
            std::lock_guard<std::mutex> lk(g_sitesMutex);
            for (auto& s : g_sites) s.nextRefreshAt = 0;  // force immediate refresh
        }
    }

    CoUninitialize();
    return 0;
}

DWORD WINAPI networkProc(LPVOID) {
    for (;;) {
        if (WaitForSingleObject(g_hStop, 0) == WAIT_OBJECT_0) break;

        HANDLE hNotify = nullptr;
        if (NotifyAddrChange(&hNotify, nullptr) == NO_ERROR && hNotify) {
            HANDLE hs[2] = {g_hStop, hNotify};
            DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
            CloseHandle(hNotify);
            if (w == WAIT_OBJECT_0) break;
            if (w == WAIT_OBJECT_0 + 1) {
                CPPDEBUG( "network: address change, refreshing all sites" );
                SetEvent(g_hRefresh);  // network changed
            }
        } else {
            WaitForSingleObject(g_hStop, 5000);  // retry registration shortly
        }
    }
    return 0;
}

void openSite(size_t idx) {
    std::wstring url;
    {
        std::lock_guard<std::mutex> lk(g_sitesMutex);
        if (idx < g_sites.size()) url = g_sites[idx].url;
    }
    if (!url.empty()) {
        CPPDEBUG( Tools::format( "site: opening %s", sf::wideToUtf8(url) ) );
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void openSettings(HWND hwnd) {
    std::vector<std::wstring> urls;
    {
        std::lock_guard<std::mutex> lk(g_sitesMutex);
        for (const auto& s : g_sites) urls.push_back(s.url);
    }
    if (urls.empty()) urls.push_back(L"https://github.com/");

    int res = sf::showSettingsDialog(g_hInstance, hwnd, urls);
    CPPDEBUG( Tools::format( "settings: dialog closed with %d", res ) );
    if (res == IDOK) {
        sf::saveSites(urls);
        rebuildSites(urls);
    }
}

void showTrayMenu(HWND hwnd, size_t idx) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Open");
    AppendMenuW(menu, MF_STRING, 3, L"Configure...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2, L"Exit");

    POINT pt = {};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                              pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == 1)
        openSite(idx);
    else if (cmd == 3)
        openSettings(hwnd);
    else if (cmd == 2)
        DestroyWindow(hwnd);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case kTrayCallbackMsg: {
            if (wParam < 1) return 0;
            size_t idx = static_cast<size_t>(wParam) - 1;
            if (lParam == WM_LBUTTONUP) {
                openSite(idx);
            } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                showTrayMenu(hwnd, idx);
            }
            return 0;
        }
        case WM_DESTROY:
            CPPDEBUG( "shutdown: window destroyed" );
            SetEvent(g_hStop);
            {
                std::lock_guard<std::mutex> lk(g_sitesMutex);
                removeAllIcons();
            }
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                    PWSTR /*pCmdLine*/, int /*nCmdShow*/) {
    // A WIN32 GUI program has no console of its own, so `-d` is how the log
    // becomes visible there. Every message also goes to the log file.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const bool console = argv && hasFlag(argc, argv, L"-d");
    if (argv) LocalFree(argv);

    const std::wstring logFile = sf::appDataDir() + L"\\showfavicon.log";
    sf::logInit(logFile, console);

    CPPDEBUG( Tools::format( "ShowFavicon starting (console=%s, log=%s)",
                             console ? "yes" : "no",
                             sf::wideToUtf8(logFile) ) );

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_hInstance = hInstance;

    std::vector<std::wstring> urls = sf::loadSites();
    if (urls.empty()) urls.push_back(L"https://github.com/");

    CPPDEBUG( Tools::format( "config: %d site(s)", static_cast<int>(urls.size()) ) );
    for (const auto& u : urls)
        CPPDEBUG( Tools::format( "config: site %s", sf::wideToUtf8(u) ) );

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClassName;

    if (!RegisterClassExW(&wc)) {
        CPPDEBUG( "startup: RegisterClassExW failed" );
        sf::logShutdown();
        CoUninitialize();
        return 1;
    }

    // Real top-level window (not a message-only window) so broadcasts such as
    // "TaskbarCreated" can be received later. It is never shown.
    HWND hwnd = CreateWindowExW(
        0, kWindowClassName, L"ShowFavicon",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        CPPDEBUG( "startup: CreateWindowExW failed" );
        sf::logShutdown();
        CoUninitialize();
        return 1;
    }
    g_hwnd = hwnd;

    g_hStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_hRefresh = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    {
        std::lock_guard<std::mutex> lk(g_sitesMutex);
        g_sites.reserve(urls.size());
        for (size_t i = 0; i < urls.size(); ++i) {
            Site site;
            site.url = urls[i];
            site.cacheFile = cacheFileFor(urls[i]);
            addSiteIcon(site, i);
            site.nextRefreshAt = 0;  // fetch immediately on startup
            g_sites.push_back(std::move(site));
        }
    }

    HANDLE hWorker = CreateThread(nullptr, 0, workerProc, nullptr, 0, nullptr);
    HANDLE hNet = CreateThread(nullptr, 0, networkProc, nullptr, 0, nullptr);

    CPPDEBUG( "startup: running" );

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CPPDEBUG( "shutdown: message loop ended" );

    SetEvent(g_hStop);
    WaitForSingleObject(hWorker, 30000);
    WaitForSingleObject(hNet, 5000);
    CloseHandle(hWorker);
    CloseHandle(hNet);
    CloseHandle(g_hStop);
    CloseHandle(g_hRefresh);

    CoUninitialize();

    CPPDEBUG( "shutdown: done" );
    sf::logShutdown();

    return static_cast<int>(msg.wParam);
}
