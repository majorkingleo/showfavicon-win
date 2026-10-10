#pragma once

// Diagnostic instrumentation for hunting a process that burns CPU: a per-loop
// heartbeat (iteration rate plus the CPU time the calling thread spent) and a
// few scope timings on the refresh pipeline. All of it compiles out unless
// LOG_TIMING is defined, so the shipped binary carries none of it; enable it
// with -DLOG_TIMING=ON (the CMake option) or by defining it by hand while
// investigating, and nothing at the call sites has to change.
//
// Two questions have to be answerable from the log alone: which thread is busy,
// and whether it is spinning (turning its loop as fast as it can) or doing real
// work in one place. Every long-running loop calls Heartbeat::tick() once per
// iteration; the heartbeat turns that into one line carrying the iteration count
// and the CPU time the thread spent in the window. A spin shows up as an
// absurd iteration rate and CPU that grows with wall time; an idle loop shows a
// handful of iterations and ~0 ms. A message loop that blocks in GetMessage
// cannot tick, so its CPU is reported from a timer with logThreadCpu().

#ifdef LOG_TIMING

#include <CpputilsDebug.h>
#include <format.h>

#include <windows.h>

namespace sf {

// CPU time (kernel + user) consumed by the calling thread so far, in ms.
// Returns -1 when the query fails.
inline int threadCpuMs() {
    FILETIME creation = {}, exit = {}, kernel = {}, user = {};
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) {
        return -1;
    }
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return static_cast<int>((k.QuadPart + u.QuadPart) / 10000ull);  // 100 ns -> ms
}

// One line naming the calling thread and the CPU it has used in total.
inline void logThreadCpu(const char* name) {
    CPPDEBUG( Tools::format( "diag: %s thread cpu %d ms", name, threadCpuMs() ) );
}

// Per-loop heartbeat. Construct once per thread, call tick() once per iteration,
// and a line is written every `intervalSec`.
class Heartbeat {
public:
    explicit Heartbeat(const char* name, unsigned intervalSec = 60)
        : m_name(name),
          m_intervalMs(static_cast<ULONGLONG>(intervalSec) * 1000ull),
          m_windowStart(GetTickCount64()),
          m_windowCpuMs(threadCpuMs()),
          m_iters(0) {}

    void tick() {
        ++m_iters;
        if (GetTickCount64() - m_windowStart < m_intervalMs) {
            return;
        }
        report();
    }

    // Write the line now and start a fresh window. Used where a loop cannot
    // tick (a message loop blocked in GetMessage) or for a forced sample.
    void report() {
        const ULONGLONG now = GetTickCount64();
        const int cpu = threadCpuMs();
        CPPDEBUG( Tools::format(
            "diag: %s did %d iteration(s) in %d ms, used %d ms cpu", m_name,
            static_cast<int>(m_iters),
            static_cast<int>(now - m_windowStart), cpu - m_windowCpuMs ) );
        m_iters = 0;
        m_windowStart = now;
        m_windowCpuMs = cpu;
    }

private:
    const char* m_name;
    ULONGLONG m_intervalMs;
    ULONGLONG m_windowStart;
    int m_windowCpuMs;
    unsigned long long m_iters;
};

}  // namespace sf

#else  // !LOG_TIMING

namespace sf {

// No-op stand-ins: the call sites stay untouched and cost nothing.
inline void logThreadCpu(const char*) {}

class Heartbeat {
public:
    explicit Heartbeat(const char*, unsigned = 60) {}
    void tick() {}
    void report() {}
};

}  // namespace sf

#endif  // LOG_TIMING
