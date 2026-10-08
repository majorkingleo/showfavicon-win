#include "logging.h"

// cpputils debug plumbing (vendored under src/cpputils, see src/tools_config.h).
#include "AsyncFileLogger.h"
#include "AsyncOutDebug.h"

#include "util.h"

#include <CpputilsDebug.h>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <thread>

namespace sf {
namespace {

// How long the logger thread idles when nothing arrives.
constexpr std::chrono::milliseconds kIdleTimeout{200};

// The file is flushed on a timer, not per line - same cadence the reference uses.
constexpr std::chrono::seconds kFlushInterval{3};

std::unique_ptr<AsyncOut::Debug> g_frontend;
std::unique_ptr<AsyncOut::FileLogger> g_file;
std::unique_ptr<AsyncOut::Logger> g_console;
std::thread g_thread;
std::atomic<bool> g_quit{false};

// A WIN32 GUI process has no console. Reuse the one that started us (so
// `showfavicon.exe -d` run from a shell prints into that shell) and only
// allocate a fresh one when there is none.
void attachConsole() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        // ERROR_ACCESS_DENIED means the process already owns a console, which
        // is the normal case for a test host; only a console-less process has
        // to allocate one.
        if (GetLastError() != ERROR_ACCESS_DENIED && !AllocConsole()) {
            return;
        }
    }

    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$", "r", stdin);

    SetConsoleOutputCP(CP_UTF8);

    // The colour helper writes whole strings; ANSI escapes are only there for
    // completeness, so failing to enable them is not fatal.
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(out, &mode)) {
        SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

// The backend loop. Every backend's semaphore is released by every message, so
// waiting on one of them is enough to know that anything arrived.
void run() {
    // Identifiable as "logger" in the debugger, next to main/worker/network.
    nameCurrentThread(L"logger");

    AsyncOut::Logger* primary = g_file
                                    ? static_cast<AsyncOut::Logger*>(g_file.get())
                                    : g_console.get();

    auto next_flush = std::chrono::steady_clock::now() + kFlushInterval;

    while (!g_quit) {
        if (g_console) {
            g_console->log();
        }
        if (g_file) {
            g_file->log();
        }

        if (std::chrono::steady_clock::now() > next_flush) {
            if (g_file) {
                g_file->flush();
            }
            next_flush = std::chrono::steady_clock::now() + kFlushInterval;
        }

        if (primary) {
            primary->wait_for(kIdleTimeout);
        } else {
            std::this_thread::sleep_for(kIdleTimeout);
        }
    }

    // Final drain, while the backends are still alive.
    if (g_console) {
        g_console->log();
    }
    if (g_file) {
        g_file->log();
    }
}

// std::ofstream::open takes a narrow path, which on Windows is interpreted in
// the ANSI code page - so convert with CP_ACP, not UTF-8. The messages written
// into the file stay UTF-8; only the path is affected.
std::string ansiPath(const std::wstring& wide) {
    int n = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), static_cast<int>(wide.size()),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, wide.c_str(), static_cast<int>(wide.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace

void logInit(const std::wstring& logFile, bool console) {
    if (console) {
        attachConsole();
    }

    if (!logFile.empty()) {
        // Backends before the frontend: a log file that cannot be opened (or a
        // missing time zone database) must not leave Tools::x_debug installed
        // pointing at a half-built object.
        try {
            g_file = std::make_unique<AsyncOut::FileLogger>(ansiPath(logFile));
        } catch (const std::exception&) {
            g_file.reset();
        }
    }

    if (console) {
        g_console = std::make_unique<AsyncOut::Logger>();
    }

    if (!g_file && !g_console) {
        return;  // nowhere to deliver to; CPPDEBUG stays a no-op
    }

    g_frontend = std::make_unique<AsyncOut::Debug>();

    if (g_file) {
        g_frontend->subscribe(g_file.get());
    }
    if (g_console) {
        g_frontend->subscribe(g_console.get());
    }

    // From here on CPPDEBUG has somewhere to go.
    Tools::x_debug = g_frontend.get();

    g_quit = false;
    g_thread = std::thread(run);
}

void logShutdown() {
    g_quit = true;

    if (g_thread.joinable()) {
        g_thread.join();
    }

    // Stop any further CPPDEBUG before the subscriber list is dismantled.
    Tools::x_debug = nullptr;

    // Destroying a backend unsubscribes it and closes (flushes) the log file.
    // The frontend must outlive both.
    g_file.reset();
    g_console.reset();
    g_frontend.reset();
}

}  // namespace sf
