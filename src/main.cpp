#include <windows.h>
#include <windowsx.h>
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
constexpr wchar_t kPaletteClassName[] = L"ShowFaviconPalette";
constexpr UINT kTrayCallbackMsg = WM_APP + 1;
constexpr UINT kShowSettingsMsg = WM_APP + 2;
constexpr UINT kIconsChangedMsg = WM_APP + 3;
constexpr UINT_PTR kPromoteTimerId = 1;
// The shell writes its per-icon settings entry a moment after the icon is first
// added, so promoting is retried a few times instead of once.
constexpr int kPromoteTries = 4;
constexpr UINT kPromoteRetryMs = 1200;
constexpr wchar_t kInstanceMutex[] = L"ShowFavicon.SingleInstance";
constexpr ULONGLONG kHourMs = 3600000ull;
constexpr wchar_t kDefaultSite[] = L"https://github.com/";

// Palette layout, in pixels.
constexpr int kIconSize = 32;
constexpr int kIconPad = 8;
constexpr int kMaxColumns = 6;

struct Site {
    std::wstring url;
    std::wstring cacheFile;
    UINT id = 0;  // slot identity, so a refresh that raced a rebuild is ignored
    NOTIFYICONDATAW nid = {};  // notification area icon, uID == id
    HICON icon = nullptr;
    bool ownsIcon = false;
    ULONGLONG nextRefreshAt = 0;
    int failures = 0;
};

HINSTANCE g_hInstance = nullptr;
HWND g_hwnd = nullptr;     // hidden: second-start message and message loop only
HWND g_hPalette = nullptr;
std::vector<Site> g_sites;
std::mutex g_sitesMutex;

// Icons the worker replaced. Destroying them there would race the palette, which
// paints them on the UI thread, so they are queued for it instead. Own mutex:
// this must not wait behind g_sitesMutex.
std::vector<HICON> g_retiredIcons;
std::mutex g_retiredMutex;

HANDLE g_hStop = nullptr;     // manual-reset; set on shutdown
HANDLE g_hRefresh = nullptr;  // auto-reset; set on network/config change
int g_promoteTriesLeft = 0;   // retries left for pulling icons out of the overflow

void resizePalette();
void refreshPalette();

bool networkAvailable() {
    return InternetGetConnectedState(nullptr, 0) != FALSE;
}

// True when `name` appears among the command-line arguments. `-d` is the one
// ShowFavicon reacts to: it turns on the console side of the logging.
bool hasFlag(int argc, wchar_t* const* argv, const wchar_t* name) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && lstrcmpiW(argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

ULONGLONG backoffMs(int failures) {
    if (failures <= 0) {
        return kHourMs;
    }
    if (failures > 6) {
        return kHourMs;  // stop retrying until the next trigger
    }
    return 30000ull * failures;  // 30s, 60s, ..., 180s
}

std::wstring cacheFileFor(const std::wstring& url) {
    std::wstring host = sf::hostFromUrl(url);
    if (host.empty()) {
        host = L"site";
    }
    return sf::iconCacheDir() + L"\\" + host + L".png";
}

void destroyOwnedIcon(Site& site) {
    if (site.ownsIcon && site.icon) {
        DestroyIcon(site.icon);
        site.icon = nullptr;
        site.ownsIcon = false;
    }
}

// Hand an icon over to the UI thread for destruction. Caller holds g_sitesMutex.
void retireIcon(HICON icon) {
    if (!icon) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_retiredMutex);
    g_retiredIcons.push_back(icon);
}

// Destroy the icons the worker retired. UI thread only.
void freeRetiredIcons() {
    std::vector<HICON> taken;
    {
        std::lock_guard<std::mutex> lk(g_retiredMutex);
        taken.swap(g_retiredIcons);
    }
    for (HICON icon : taken) {
        DestroyIcon(icon);
    }
}

// Add the notification-area icon for one site. Caller holds the lock, and the
// hidden window must exist: it owns the icon's callback messages.
void addTrayIcon(Site& site) {
    site.nid = {};
    site.nid.cbSize = sizeof(site.nid);
    site.nid.hWnd = g_hwnd;
    site.nid.uID = site.id;
    site.nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    site.nid.uCallbackMessage = kTrayCallbackMsg;
    site.nid.hIcon = site.icon;

    std::wstring tip = sf::hostFromUrl(site.url);
    if (tip.empty()) {
        tip = L"ShowFavicon";
    }
    lstrcpynW(site.nid.szTip, tip.c_str(),
              static_cast<int>(sizeof(site.nid.szTip) / sizeof(site.nid.szTip[0])));

    Shell_NotifyIconW(NIM_ADD, &site.nid);
    CPPDEBUG( Tools::wformat( L"tray: added icon %u for %s", site.nid.uID, tip ) );
}

// Drop every site. Caller holds the lock.
void destroyAllSites() {
    CPPDEBUG( Tools::format( "ui: dropping %d site(s)",
                             static_cast<int>(g_sites.size()) ) );
    for (auto& site : g_sites) {
        if (site.nid.cbSize) {
            Shell_NotifyIconW(NIM_DELETE, &site.nid);
        }
        destroyOwnedIcon(site);
    }
    g_sites.clear();
}

// Ask the shell to keep this executable's tray icons out of the overflow.
void promoteTrayIcons() {
    const int promoted = sf::promoteNotificationIcons();
    if (promoted > 0) {
        CPPDEBUG( Tools::format( "tray: promoted %d notification icon(s)",
                                 promoted ) );
    }

    if (--g_promoteTriesLeft <= 0) {
        KillTimer(g_hwnd, kPromoteTimerId);
    }
}

// Promotion is attempted once right away - after the first run the entries
// already exist - and then on a timer, because a brand new icon's entry only
// appears a moment after the icon was added.
void startTrayPromotion() {
    if (!g_hwnd) {
        return;
    }

    g_promoteTriesLeft = kPromoteTries;
    promoteTrayIcons();
    if (g_promoteTriesLeft > 0) {
        SetTimer(g_hwnd, kPromoteTimerId, kPromoteRetryMs, nullptr);
    }
}

// (Re)build the site list from a list of URLs. UI thread only, so the icons it
// frees cannot be the ones being painted.
void rebuildSites(const std::vector<std::wstring>& urls) {
    CPPDEBUG( Tools::format( "config: rebuilding the palette with %d site(s)",
                             static_cast<int>(urls.size()) ) );
    {
        std::lock_guard<std::mutex> lk(g_sitesMutex);
        destroyAllSites();
        g_sites.reserve(urls.size());
        for (size_t i = 0; i < urls.size(); ++i) {
            Site site;
            site.url = urls[i];
            site.cacheFile = cacheFileFor(urls[i]);
            site.id = static_cast<UINT>(i + 1);
            site.icon = LoadIcon(nullptr, IDI_APPLICATION);  // shared, not owned
            site.nextRefreshAt = 0;  // fetch immediately
            addTrayIcon(site);
            g_sites.push_back(std::move(site));
        }
    }
    resizePalette();
    refreshPalette();
    startTrayPromotion();
    SetEvent(g_hRefresh);
}

void refreshSite(size_t idx) {
    std::wstring url;
    std::wstring cacheFile;
    UINT id = 0;
    {
        std::lock_guard<std::mutex> lk(g_sitesMutex);
        if (idx >= g_sites.size()) {
            return;
        }
        url = g_sites[idx].url;
        cacheFile = g_sites[idx].cacheFile;
        id = g_sites[idx].id;
    }

    CPPDEBUG( Tools::wformat( L"refresh: site %u %s", id, url ) );

    if (!networkAvailable()) {
        CPPDEBUG( Tools::format( "refresh: no network, site %u deferred", id ) );
        std::lock_guard<std::mutex> lk(g_sitesMutex);
        if (idx < g_sites.size() && g_sites[idx].id == id) {
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
    if (!icon) {
        icon = LoadIcon(nullptr, IDI_APPLICATION);
    }

    // Copied out under the lock and handed to the shell outside it, because a
    // NIM_MODIFY call can wait on the tray window.
    NOTIFYICONDATAW nid = {};
    {
        std::lock_guard<std::mutex> lk(g_sitesMutex);
        if (idx < g_sites.size() && g_sites[idx].id == id) {
            Site& site = g_sites[idx];
            if (site.ownsIcon && site.icon) {
                retireIcon(site.icon);
            }
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
        } else if (owns) {
            // The list was rebuilt while fetching: nobody will paint this icon.
            DestroyIcon(icon);
        }
    }
    CPPDEBUG( Tools::format( "refresh: site %u %s", id,
                             ok ? "ok" : "failed" ) );

    if (nid.cbSize) {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    refreshPalette();
}

DWORD WINAPI workerProc(LPVOID) {
    sf::nameCurrentThread(L"worker");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    for (;;) {
        if (WaitForSingleObject(g_hStop, 0) == WAIT_OBJECT_0) {
            break;
        }

        ULONGLONG now = GetTickCount64();
        ULONGLONG earliest = ULLONG_MAX;
        std::vector<size_t> due;
        {
            std::lock_guard<std::mutex> lk(g_sitesMutex);
            for (size_t i = 0; i < g_sites.size(); ++i) {
                if (g_sites[i].nextRefreshAt <= now) {
                    due.push_back(i);
                } else if (g_sites[i].nextRefreshAt < earliest) {
                    earliest = g_sites[i].nextRefreshAt;
                }
            }
        }

        for (size_t i : due) {
            refreshSite(i);
        }

        DWORD waitMs = 1000;
        if (earliest != ULLONG_MAX && earliest > now) {
            waitMs = static_cast<DWORD>(std::min<ULONGLONG>(earliest - now, 60000));
        }

        HANDLE hs[2] = {g_hStop, g_hRefresh};
        DWORD w = WaitForMultipleObjects(2, hs, FALSE, waitMs);
        if (w == WAIT_OBJECT_0) {
            break;
        }
        if (w == WAIT_OBJECT_0 + 1) {
            std::lock_guard<std::mutex> lk(g_sitesMutex);
            for (auto& s : g_sites) {
                s.nextRefreshAt = 0;  // force immediate refresh
            }
        }
    }

    CoUninitialize();
    return 0;
}

DWORD WINAPI networkProc(LPVOID) {
    sf::nameCurrentThread(L"network");
    for (;;) {
        if (WaitForSingleObject(g_hStop, 0) == WAIT_OBJECT_0) {
            break;
        }

        HANDLE hNotify = nullptr;
        if (NotifyAddrChange(&hNotify, nullptr) == NO_ERROR && hNotify) {
            HANDLE hs[2] = {g_hStop, hNotify};
            DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
            CloseHandle(hNotify);
            if (w == WAIT_OBJECT_0) {
                break;
            }
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
        if (idx < g_sites.size()) {
            url = g_sites[idx].url;
        }
    }
    if (!url.empty()) {
        CPPDEBUG( Tools::wformat( L"site: opening %s", url ) );
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void openSettings(HWND hwnd) {
    // One dialog at a time. The modal loop still dispatches this thread's
    // messages, so a second request - another start, or the palette menu -
    // would otherwise nest a second dialog on top of the first.
    if (sf::settingsDialogOpen()) {
        CPPDEBUG( "settings: dialog already open, raising it" );
        sf::raiseSettingsDialog();
        return;
    }

    std::vector<std::wstring> urls;
    {
        std::lock_guard<std::mutex> lk(g_sitesMutex);
        for (const auto& s : g_sites) {
            urls.push_back(s.url);
        }
    }
    if (urls.empty()) {
        urls.push_back(kDefaultSite);
    }

    int res = sf::showSettingsDialog(g_hInstance, hwnd, urls);
    CPPDEBUG( Tools::format( "settings: dialog closed with %d", res ) );
    if (res == IDOK) {
        sf::saveSites(urls);
        rebuildSites(urls);
    }
}

// --- palette window ---------------------------------------------------------
//
// Windows 11 drops new tray icons into the overflow flyout and no API can
// promote one, so the app shows its own surface instead: one icon per site,
// always on top, click to open, right-click for the menu.

int paletteColumns(size_t count) {
    if (count == 0) {
        return 1;
    }
    return static_cast<int>(std::min<size_t>(count, static_cast<size_t>(kMaxColumns)));
}

SIZE paletteClientSize(size_t count) {
    const int columns = paletteColumns(count);
    const int rows = static_cast<int>(
        (count + static_cast<size_t>(columns) - 1) / static_cast<size_t>(columns));
    const int cell = kIconSize + kIconPad;

    SIZE size = {};
    size.cx = columns * cell + kIconPad;
    size.cy = rows * cell + kIconPad;
    return size;
}

size_t paletteCount() {
    std::lock_guard<std::mutex> lk(g_sitesMutex);
    return g_sites.size();
}

// Keeps the palette fully inside a monitor's work area. Needed because a
// remembered position can point at a screen that is no longer attached, and
// because growing near the right edge would otherwise push icons off the
// desktop.
void clampPaletteToWorkArea() {
    if (!g_hPalette) {
        return;
    }

    RECT window = {};
    GetWindowRect(g_hPalette, &window);
    const int width = window.right - window.left;
    const int height = window.bottom - window.top;

    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(g_hPalette, MONITOR_DEFAULTTONEAREST), &mi)) {
        return;
    }

    int x = window.left;
    int y = window.top;
    if (x + width > mi.rcWork.right) {
        x = mi.rcWork.right - width;
    }
    if (y + height > mi.rcWork.bottom) {
        y = mi.rcWork.bottom - height;
    }
    if (x < mi.rcWork.left) {
        x = mi.rcWork.left;
    }
    if (y < mi.rcWork.top) {
        y = mi.rcWork.top;
    }

    if (x != window.left || y != window.top) {
        SetWindowPos(g_hPalette, nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void resizePalette() {
    if (!g_hPalette) {
        return;
    }

    const SIZE size = paletteClientSize(paletteCount());
    RECT r = { 0, 0, size.cx, size.cy };
    AdjustWindowRectEx(&r, GetWindowLongW(g_hPalette, GWL_STYLE), FALSE,
                       GetWindowLongW(g_hPalette, GWL_EXSTYLE));

    SetWindowPos(g_hPalette, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    clampPaletteToWorkArea();
}

void refreshPalette() {
    if (g_hPalette) {
        PostMessageW(g_hPalette, kIconsChangedMsg, 0, 0);
    }
}

// Index of the cell under a client point, or -1.
int paletteIndexAt(int x, int y) {
    const int cell = kIconSize + kIconPad;
    const int column = (x - kIconPad) / cell;
    const int row = (y - kIconPad) / cell;
    if (column < 0 || row < 0 || column >= kMaxColumns) {
        return -1;
    }

    const size_t index = static_cast<size_t>(row) * kMaxColumns +
                         static_cast<size_t>(column);
    std::lock_guard<std::mutex> lk(g_sitesMutex);
    return index < g_sites.size() ? static_cast<int>(index) : -1;
}

void showPaletteMenu(HWND palette, int index) {
    const bool onIcon = index >= 0;

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (onIcon ? MF_ENABLED : MF_GRAYED), 1, L"Open");
    AppendMenuW(menu, MF_STRING, 4, L"Update now");
    AppendMenuW(menu, MF_STRING, 3, L"Configure...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2, L"Exit");

    POINT pt = {};
    GetCursorPos(&pt);
    SetForegroundWindow(palette);
    const UINT cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                    pt.x, pt.y, 0, palette, nullptr);
    DestroyMenu(menu);

    if (cmd == 1 && onIcon) {
        openSite(static_cast<size_t>(index));
    } else if (cmd == 4) {
        CPPDEBUG( "palette: refreshing every site on request" );
        SetEvent(g_hRefresh);  // forces an immediate refresh of all sites
    } else if (cmd == 3) {
        openSettings(palette);
    } else if (cmd == 2) {
        DestroyWindow(g_hwnd);
    }
}

LRESULT CALLBACK PaletteProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            HDC dc = BeginPaint(hwnd, &ps);

            RECT client = {};
            GetClientRect(hwnd, &client);
            FillRect(dc, &client, GetSysColorBrush(COLOR_BTNFACE));

            std::lock_guard<std::mutex> lk(g_sitesMutex);
            const int cell = kIconSize + kIconPad;
            for (size_t i = 0; i < g_sites.size(); ++i) {
                if (!g_sites[i].icon) {
                    continue;
                }
                const int column = static_cast<int>(i) % kMaxColumns;
                const int row = static_cast<int>(i) / kMaxColumns;
                DrawIconEx(dc, kIconPad + column * cell, kIconPad + row * cell,
                           g_sites[i].icon, kIconSize, kIconSize, 0, nullptr,
                           DI_NORMAL);
            }

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONUP: {
            const int index = paletteIndexAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (index >= 0) {
                openSite(static_cast<size_t>(index));
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            const int index = paletteIndexAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            showPaletteMenu(hwnd, index);
            return 0;
        }
        case kIconsChangedMsg:
            freeRetiredIcons();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_EXITSIZEMOVE: {
            RECT r = {};
            GetWindowRect(hwnd, &r);
            sf::savePalettePos(r.left, r.top);
            return 0;
        }
        case WM_CLOSE:
            // The palette is the only surface, so closing it exits: hiding it
            // would leave a process with nothing left to click.
            DestroyWindow(g_hwnd);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool createPalette(HINSTANCE hInstance) {
    WNDCLASSEXW pc = {};
    pc.cbSize = sizeof(pc);
    pc.lpfnWndProc = PaletteProc;
    pc.hInstance = hInstance;
    pc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pc.lpszClassName = kPaletteClassName;

    if (!RegisterClassExW(&pc)) {
        CPPDEBUG( "startup: RegisterClassExW failed for the palette" );
        return false;
    }

    // Topmost, so it stays reachable without the tray overflow. Deliberately
    // without WS_EX_TOOLWINDOW: the palette is the app's only window, so it
    // gets a taskbar button and an Alt+Tab entry like any other program, which
    // is also how it is found again if it ends up behind something.
    const DWORD exStyle = WS_EX_TOPMOST;
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;

    const SIZE size = paletteClientSize(paletteCount());
    RECT r = { 0, 0, size.cx, size.cy };
    AdjustWindowRectEx(&r, style, FALSE, exStyle);
    const int width = r.right - r.left;
    const int height = r.bottom - r.top;

    int x = 0;
    int y = 0;
    if (!sf::loadPalettePos(x, y)) {
        RECT work = {};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        x = work.right - width - 12;
        y = work.bottom - height - 12;
    }

    g_hPalette = CreateWindowExW(exStyle, kPaletteClassName, L"ShowFavicon", style,
                                 x, y, width, height, nullptr, nullptr, hInstance,
                                 nullptr);
    if (!g_hPalette) {
        CPPDEBUG( "startup: CreateWindowExW failed for the palette" );
        return false;
    }

    clampPaletteToWorkArea();
    ShowWindow(g_hPalette, SW_SHOWNOACTIVATE);
    CPPDEBUG( "startup: palette shown" );
    return true;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case kTrayCallbackMsg: {
            if (wParam < 1) {
                return 0;
            }
            const size_t idx = static_cast<size_t>(wParam) - 1;
            if (lParam == WM_LBUTTONUP) {
                openSite(idx);
            } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                showPaletteMenu(hwnd, static_cast<int>(idx));
            }
            return 0;
        }
        case WM_TIMER:
            if (wParam == kPromoteTimerId) {
                promoteTrayIcons();
                return 0;
            }
            break;
        case kShowSettingsMsg:
            // A second start of the app asks this instance, which owns the
            // palette, to show the dialog instead of showing its own.
            CPPDEBUG( "instance: another start asked for the settings dialog" );
            openSettings(hwnd);
            return 0;
        case WM_DESTROY:
            CPPDEBUG( "shutdown: window destroyed" );
            SetEvent(g_hStop);
            {
                std::lock_guard<std::mutex> lk(g_sitesMutex);
                destroyAllSites();
            }
            freeRetiredIcons();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                    PWSTR /*pCmdLine*/, int /*nCmdShow*/) {
    sf::nameCurrentThread(L"main");

    // A WIN32 GUI program has no console of its own, so `-d` is how the log
    // becomes visible there. Every message also goes to the log file.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const bool console = argv && hasFlag(argc, argv, L"-d");
    if (argv) {
        LocalFree(argv);
    }

    const std::wstring logFile = sf::appDataDir() + L"\\showfavicon.log";
    sf::logInit(logFile, console);

    CPPDEBUG( Tools::wformat( L"ShowFavicon starting (console=%s, log=%s)",
                              console ? L"yes" : L"no", logFile ) );

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_hInstance = hInstance;

    // One instance per session. A second start does not open its own palette:
    // it asks the running instance to show the settings dialog, then exits. The
    // mutex handle is deliberately never closed, so the object lives as long
    // as the process and the name stays taken.
    HANDLE hSingleInstance = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (hSingleInstance && GetLastError() == ERROR_ALREADY_EXISTS) {
        CPPDEBUG( "instance: already running, asking it for the settings dialog" );
        HWND running = FindWindowW(kWindowClassName, nullptr);
        if (running) {
            PostMessageW(running, kShowSettingsMsg, 0, 0);
        } else {
            CPPDEBUG( "instance: found no window to ask" );
        }
        sf::logShutdown();
        CoUninitialize();
        return 0;
    }

    // Nothing configured means a first run: seed the default site, then open
    // the settings dialog (below) so it can be changed straight away.
    std::vector<std::wstring> urls = sf::loadSites();
    const bool firstRun = urls.empty();
    if (firstRun) {
        urls.push_back(kDefaultSite);
        CPPDEBUG( "config: first run, seeding the default site" );
    }

    CPPDEBUG( Tools::format( "config: %d site(s)", static_cast<int>(urls.size()) ) );
    for (const auto& u : urls) {
        CPPDEBUG( Tools::wformat( L"config: site %s", u ) );
    }

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

    // Hidden top-level window: it exists for the second-start message and for
    // the message loop, and is never shown. The palette below is what is seen.
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

    // The sites come first so the palette is already the right size when it is
    // placed: a window that grows after being moved would hang off the screen
    // edge it was placed against.
    rebuildSites(urls);

    if (!createPalette(hInstance)) {
        sf::logShutdown();
        CoUninitialize();
        return 1;
    }

    HANDLE hWorker = CreateThread(nullptr, 0, workerProc, nullptr, 0, nullptr);
    HANDLE hNet = CreateThread(nullptr, 0, networkProc, nullptr, 0, nullptr);

    CPPDEBUG( "startup: running" );

    if (firstRun) {
        // Save the seed before showing the dialog: if it is cancelled the
        // default site is still configured, instead of coming back to an empty
        // configuration - and to this dialog - on every start.
        sf::saveSites(urls);
        openSettings(g_hwnd);
    }

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
