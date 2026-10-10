#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <wininet.h>
#include <iphlpapi.h>

#include "diag.h"
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
constexpr UINT kShowSettingsMsg = WM_APP + 2;
constexpr UINT_PTR kPromoteTimerId = 1;
#ifdef LOG_TIMING
// Reports the UI thread's CPU once a minute: the message loop blocks in
// GetMessageW and therefore cannot produce a heartbeat of its own.
constexpr UINT_PTR kDiagTimerId = 2;
constexpr UINT kDiagIntervalMs = 60000;
#endif
// The shell writes its per-icon settings entry a moment after the icon is first
// added, so promoting is retried a few times instead of once.
constexpr int kPromoteTries = 4;
constexpr UINT kPromoteRetryMs = 1200;
constexpr wchar_t kInstanceMutex[] = L"ShowFavicon.SingleInstance";
// A second start waits this long before it decides the name is really taken.
// An instance that is shutting down keeps the name for a moment without owning
// a window any more, and a click in that window must not do nothing.
constexpr int kStartAttempts = 25;
constexpr DWORD kStartRetryMs = 200;
constexpr ULONGLONG kHourMs = 3600000ull;
constexpr wchar_t kDefaultSite[] = L"https://github.com/";

struct Site {
    std::wstring url;
    std::wstring cacheFile;
    UINT id = 0;  // slot identity, so a refresh that raced a rebuild is ignored
    NOTIFYICONDATAW nid = {};  // notification area icon, uID == id
    bool trayAdded = false;    // NIM_ADD succeeded; retried on the timer if not
    HICON icon = nullptr;
    bool ownsIcon = false;
    ULONGLONG nextRefreshAt = 0;
    int failures = 0;
};

HINSTANCE g_hInstance = nullptr;
HWND g_hwnd = nullptr;  // hidden: owns the tray callback messages, the promotion
                        // timer, the second-start message and the message loop
std::vector<Site> g_sites;
std::mutex g_sitesMutex;

HANDLE g_hStop = nullptr;     // manual-reset; set on shutdown
HANDLE g_hRefresh = nullptr;  // auto-reset; set on network/config change
int g_promoteTriesLeft = 0;   // retries left for pulling icons out of the overflow

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

    site.trayAdded = Shell_NotifyIconW(NIM_ADD, &site.nid) != FALSE;
    if (site.trayAdded) {
        CPPDEBUG( Tools::wformat( L"tray: added icon %u for %s", site.nid.uID, tip ) );
    } else {
        CPPDEBUG( Tools::wformat( L"tray: NIM_ADD failed for %u, retrying later",
                                  site.nid.uID ) );
    }
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

// Ask the shell to keep this executable's tray icons out of the overflow. An
// icon that could not be added yet is retried here too: NIM_ADD fails while the
// shell is still coming up, which is what starting from the Run key runs into.
void promoteTrayIcons() {
    const int promoted = sf::promoteNotificationIcons();
    if (promoted > 0) {
        CPPDEBUG( Tools::format( "tray: promoted %d notification icon(s)",
                                 promoted ) );
    }

    {
        std::lock_guard<std::mutex> lk(g_sitesMutex);
        for (auto& site : g_sites) {
            if (!site.trayAdded && site.nid.cbSize) {
                site.trayAdded = Shell_NotifyIconW(NIM_ADD, &site.nid) != FALSE;
                CPPDEBUG( Tools::format( "tray: retry for icon %u %s", site.nid.uID,
                                         site.trayAdded ? "ok" : "failed" ) );
            }
        }
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
    CPPDEBUG( Tools::format( "config: rebuilding %d tray icon(s)",
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

#ifdef LOG_TIMING
    // Timing around the whole pipeline (fetch, parse, decode, HICON), so a
    // refresh that eats CPU shows its cost and not just its result.
    const ULONGLONG startMs = GetTickCount64();
#endif

    sf::RgbaImage img;
    bool ok = sf::fetchOrCachedIcon(url, cacheFile, img);
    HICON icon = ok ? sf::imageToHicon(img) : nullptr;
    bool owns = (icon != nullptr);
    if (!icon) {
        icon = LoadIcon(nullptr, IDI_APPLICATION);
    }

    // Copied out under the lock and handed to the shell outside it, because a
    // NIM_MODIFY call can wait on the tray window. The shell takes its own copy
    // of the icon, so the one being replaced can be destroyed right away.
    NOTIFYICONDATAW nid = {};
    {
        std::lock_guard<std::mutex> lk(g_sitesMutex);
        if (idx < g_sites.size() && g_sites[idx].id == id) {
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
        } else if (owns) {
            // The list was rebuilt while fetching: nobody will paint this icon.
            DestroyIcon(icon);
        }
    }
#ifdef LOG_TIMING
    CPPDEBUG( Tools::format( "refresh: site %u %s in %d ms", id,
                             ok ? "ok" : "failed",
                             static_cast<int>(GetTickCount64() - startMs) ) );
#else
    CPPDEBUG( Tools::format( "refresh: site %u %s", id, ok ? "ok" : "failed" ) );
#endif

    if (nid.cbSize) {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

DWORD WINAPI workerProc(LPVOID) {
    sf::nameCurrentThread(L"worker");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Reports once a minute how often this loop turned and how much CPU it
    // used: a spin here is a rate in the thousands plus CPU that grows with
    // wall time, an idle loop is a handful of iterations and ~0 ms.
    sf::Heartbeat heartbeat("worker");

    for (;;) {
        heartbeat.tick();

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

#ifdef LOG_TIMING
        // A wait that is about to expire almost immediately is the shape a
        // near-spin takes; logging it makes that visible in the log.
        if (waitMs < 100) {
            CPPDEBUG( Tools::format( "diag: worker waits only %u ms, %d due",
                                     waitMs, static_cast<int>(due.size()) ) );
        }
#endif

        HANDLE hs[2] = {g_hStop, g_hRefresh};
        DWORD w = WaitForMultipleObjects(2, hs, FALSE, waitMs);
        if (w == WAIT_OBJECT_0) {
            break;
        }
        if (w == WAIT_OBJECT_0 + 1) {
#ifdef LOG_TIMING
            CPPDEBUG( "diag: worker woken by the refresh event" );
#endif
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
#ifdef LOG_TIMING
    CPPDEBUG( "diag: network thread started" );
#endif

    // This thread parks inside the synchronous form of NotifyAddrChange
    // (OVERLAPPED == NULL) until an address really changes, so it normally uses
    // no CPU at all and its heartbeat only appears after a change. It is logged
    // anyway: on a machine that keeps changing addresses the call returns over
    // and over and this loop would spin, and that has to be visible rather than
    // hidden behind a thread that looks idle.
    sf::Heartbeat heartbeat("network");

    for (;;) {
        heartbeat.tick();

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
#ifdef LOG_TIMING
            CPPDEBUG( "diag: NotifyAddrChange failed, retrying in 5 s" );
#endif
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
    // messages, so a second request - another start, or the tray menu - would
    // otherwise nest a second dialog on top of the first.
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

// --- tray menu --------------------------------------------------------------
//
// The notification area icons are the app's only visible surface: one per site,
// always visible (see startTrayPromotion), left click opens the site and right
// click opens this menu.

void showTrayMenu(HWND owner, int index) {
    const bool onIcon = index >= 0;

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (onIcon ? MF_ENABLED : MF_GRAYED), 1, L"Open");
    AppendMenuW(menu, MF_STRING, 4, L"Update now");
    AppendMenuW(menu, MF_STRING, 3, L"Configure...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2, L"Exit");

    POINT pt = {};
    GetCursorPos(&pt);

    // The tray icons sit on the bottom edge of the screen, so a menu that does
    // not fit below the cursor is flipped up - and a flipped-up menu puts the
    // cursor on its last row, which is Exit. A stray click there would end the
    // process. Placing the menu above the cursor by roughly one extra row keeps
    // the cursor outside it, so such a click only dismisses the menu.
    const int rowHeight = GetSystemMetrics(SM_CYMENU) + 4;
    const int height = (static_cast<int>(GetMenuItemCount(menu)) + 1) * rowHeight;
    const LONG top = std::max<LONG>(0, pt.y - height);

    SetForegroundWindow(owner);
    const UINT cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                    pt.x, top, 0, owner, nullptr);
    DestroyMenu(menu);
    // The owner is a hidden window, so it can never really become the foreground
    // one. Posting anything afterwards is what makes the menu close when the
    // user clicks somewhere else.
    PostMessageW(owner, WM_NULL, 0, 0);

    if (cmd == 1 && onIcon) {
        openSite(static_cast<size_t>(index));
    } else if (cmd == 4) {
        CPPDEBUG( "tray: refreshing every site on request" );
        SetEvent(g_hRefresh);  // forces an immediate refresh of all sites
    } else if (cmd == 3) {
        openSettings(owner);
    } else if (cmd == 2) {
        CPPDEBUG( "tray: exit requested from the menu" );
        DestroyWindow(g_hwnd);
    }
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
                showTrayMenu(hwnd, static_cast<int>(idx));
            }
            return 0;
        }
        case WM_TIMER:
            if (wParam == kPromoteTimerId) {
                promoteTrayIcons();
                return 0;
            }
#ifdef LOG_TIMING
            if (wParam == kDiagTimerId) {
                sf::logThreadCpu("ui");
                return 0;
            }
#endif
            break;
        case kShowSettingsMsg:
            // A second start asks this instance for the settings dialog instead
            // of starting a second set of tray icons.
            CPPDEBUG( "instance: another start asked for the settings dialog" );
            openSettings(hwnd);
            return 0;
        case WM_CLOSE:
            // Only the tray menu's Exit may end the process. Anything else that
            // closes this window (the taskbar's Close window, for instance)
            // would otherwise destroy it through DefWindowProc and take the
            // tray icons down with it, silently.
            CPPDEBUG( "ui: WM_CLOSE ignored - use Exit on the tray menu" );
            return 0;
        case WM_DESTROY:
            CPPDEBUG( "shutdown: window destroyed" );
            SetEvent(g_hStop);
            {
                std::lock_guard<std::mutex> lk(g_sitesMutex);
                destroyAllSites();
            }
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    return 0;
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

    // One instance per session. A second start hands over to the running one and
    // exits. The name is waited for rather than given up on: it is held for a
    // moment by an instance that is shutting down, and reporting "already
    // running" when there is no window left to ask would make the click look
    // like it did nothing and the icons would stay away.
    HANDLE hSingleInstance = nullptr;
    for (int attempt = 0; attempt < kStartAttempts; ++attempt) {
        hSingleInstance = CreateMutexW(nullptr, TRUE, kInstanceMutex);
        if (!hSingleInstance || GetLastError() != ERROR_ALREADY_EXISTS) {
            break;  // the name is ours: this is the instance that runs
        }

        HWND running = FindWindowW(kWindowClassName, nullptr);
        if (running) {
            CPPDEBUG( "instance: already running, asking it for the settings dialog" );
            PostMessageW(running, kShowSettingsMsg, 0, 0);
            sf::logShutdown();
            CoUninitialize();
            return 0;
        }

        CPPDEBUG( "instance: name is taken but no window yet, waiting" );
        CloseHandle(hSingleInstance);
        hSingleInstance = nullptr;
        Sleep(kStartRetryMs);
    }

    if (!hSingleInstance) {
        CPPDEBUG( "startup: the single-instance name stays taken, giving up" );
        sf::logShutdown();
        CoUninitialize();
        return 1;
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

    // Hidden top-level window: it owns the tray callback messages, the promotion
    // timer, the second-start message and the message loop. Nothing is shown.
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

    // Adds the tray icons and starts the promotion retries.
    rebuildSites(urls);

#ifdef LOG_TIMING
    // Reports the UI thread's CPU once a minute for the whole run; a message
    // loop blocked in GetMessageW emits nothing else.
    SetTimer(g_hwnd, kDiagTimerId, kDiagIntervalMs, nullptr);
#endif

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

    // The message loop blocks in GetMessageW, so this only ticks while messages
    // are actually being dispatched; a flooded queue shows up as the rate
    // rising. An idle UI thread still reports its CPU from kDiagTimerId.
    sf::Heartbeat heartbeat("ui");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        heartbeat.tick();
    }

    CPPDEBUG( "shutdown: message loop ended" );

    SetEvent(g_hStop);
    WaitForSingleObject(hWorker, 5000);
    WaitForSingleObject(hNet, 1000);
    CloseHandle(hWorker);
    CloseHandle(hNet);
    CloseHandle(g_hStop);
    CloseHandle(g_hRefresh);

    CoUninitialize();

    CPPDEBUG( "shutdown: done" );
    sf::logShutdown();

    return static_cast<int>(msg.wParam);
}
