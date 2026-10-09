#include "SystemStats.h"

#include <thread>

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <psapi.h>
 #pragma comment(lib, "psapi.lib")
#endif

namespace taw
{
#if defined(_WIN32)
static unsigned long long ticks (const FILETIME& f) { return ((unsigned long long) f.dwHighDateTime << 32) | f.dwLowDateTime; }

void SystemStats::sample()
{
    threads = (int) std::thread::hardware_concurrency();

    // The whole machine: busy time against all time, across every core.
    FILETIME idle, kernel, user;

    if (GetSystemTimes (&idle, &kernel, &user))
    {
        const auto i = ticks (idle), k = ticks (kernel), u = ticks (user);

        if (lastKernel != 0)
        {
            const auto total = (k - lastKernel) + (u - lastUser); // kernel time includes idle
            const auto busy = total - (i - lastIdle);
            cpu = total > 0 ? (float) busy / (float) total : 0.0f;
        }

        lastIdle = i;
        lastKernel = k;
        lastUser = u;
    }

    // taw: its threads' time against the wall clock times the number of cores.
    FILETIME created, exited, pk, pu, now;
    GetSystemTimeAsFileTime (&now);

    if (GetProcessTimes (GetCurrentProcess(), &created, &exited, &pk, &pu))
    {
        const auto process = ticks (pk) + ticks (pu), wall = ticks (now);

        if (lastWall != 0 && wall > lastWall && threads > 0)
            tawCpu = (float) (process - lastProcess) / (float) ((wall - lastWall) * (unsigned long long) threads);

        lastProcess = process;
        lastWall = wall;
    }

    PROCESS_MEMORY_COUNTERS_EX pmc {};

    if (GetProcessMemoryInfo (GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*) &pmc, sizeof (pmc)))
        tawMemoryMB = (double) pmc.PrivateUsage / (1024.0 * 1024.0);

    MEMORYSTATUSEX mem { sizeof (mem) };

    if (GlobalMemoryStatusEx (&mem))
    {
        freeMemoryMB = (double) mem.ullAvailPhys / (1024.0 * 1024.0);
        totalMemoryMB = (double) mem.ullTotalPhys / (1024.0 * 1024.0);
    }
}
#else
void SystemStats::sample()
{
    threads = (int) std::thread::hardware_concurrency();
}
#endif
} // namespace taw
