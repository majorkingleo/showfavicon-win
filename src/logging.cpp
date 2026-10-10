#include "logging.h"

// cpputils debug plumbing (vendored under src/cpputils, see src/tools_config.h).
#include "AsyncFileLogger.h"
#include "AsyncOutDebug.h"

#include "diag.h"
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

// A backend must never be able to kill this thread: an exception escaping it
// would call std::terminate and take the whole process with it. Both backends
// convert wide text, which can throw, so swallowing here costs the batch the
// backend had popped - not the process, and not all further logging.
void drainConsole() {
    if (!g_console) {
        return;
    }
    try {
        g_console->log();
    } catch (...) {
    }
}

void drainFile() {
    if (!g_file) {
        return;
    }
    try {
        g_file->log();
    } catch (...) {
    }
}

void flushFile() {
    if (!g_file) {
        return;
    }
    try {
        g_file->flush();
    } catch (...) {
    }
}

// The backend loop. A backend's semaphore is released by every message, so on a
// platform whose std::counting_semaphore really blocks, waiting on one of them
// is enough to know that anything arrived. MinGW is the exception (see below).
void run() {
    // Identifiable as "logger" in the debugger, next to main/worker/network.
    nameCurrentThread(L"logger");

    // LOG_TIMING only: reports how often this loop turned and the CPU it used.
    sf::Heartbeat heartbeat("logger");

#ifndef __MINGW32__
    // The backend to wait on. Not needed under MinGW: there the semaphore wait
    // busy-spins, so the idle path below sleeps instead.
    AsyncOut::Logger* primary = g_file
                                    ? static_cast<AsyncOut::Logger*>(g_file.get())
                                    : g_console.get();
#endif

    auto next_flush = std::chrono::steady_clock::now() + kFlushInterval;

    while (!g_quit) {
        heartbeat.tick();

        drainConsole();
        drainFile();

        if (std::chrono::steady_clock::now() > next_flush) {
            flushFile();
            next_flush = std::chrono::steady_clock::now() + kFlushInterval;
        }

#ifdef __MINGW32__
        // MinGW's libstdc++ has no platform_wait (futex) on Windows, so
        // AsyncOut::Logger::wait_for() - a std::counting_semaphore::
        // try_acquire_for - is a sched_yield spin that burns a full core while
        // idle (measured at 59984 ms cpu in 60000 ms wall). The loop already
        // polls at this cadence, so a real sleep costs at most kIdleTimeout of
        // latency on a message - nothing against the 3 s flush interval - and
        // no CPU.
        std::this_thread::sleep_for(kIdleTimeout);
#else
        // The correct choice where the semaphore really blocks: the logger
        // wakes as soon as a backend is handed a message, instead of on the
        // next poll.
        if (primary) {
            primary->wait_for(kIdleTimeout);
        } else {
            std::this_thread::sleep_for(kIdleTimeout);
        }
#endif
    }

    // Final drain, while the backends are still alive.
    drainConsole();
    drainFile();
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
