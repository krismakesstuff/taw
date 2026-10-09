#include "SystemStats.h"

#include <thread>

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <psapi.h>
 #pragma comment(lib, "psapi.lib")
#elif defined(__APPLE__)
 #include <chrono>
 #include <mach/mach.h>
 #include <sys/resource.h>
 #include <sys/sysctl.h>
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
#elif defined(__APPLE__)
void SystemStats::sample()
{
    threads = (int) std::thread::hardware_concurrency();

    // The whole machine: every core's ticks, busy against all (lastKernel holds all of them).
    host_cpu_load_info_data_t load {};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;

    if (host_statistics (mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t) &load, &count) == KERN_SUCCESS)
    {
        unsigned long long all = 0;

        for (auto t : load.cpu_ticks)
            all += t;

        const unsigned long long idle = load.cpu_ticks[CPU_STATE_IDLE];

        if (lastKernel != 0 && all > lastKernel)
        {
            const auto total = all - lastKernel;
            cpu = (float) (total - (idle - lastIdle)) / (float) total;
        }

        lastIdle = idle;
        lastKernel = all;
    }

    // taw: its threads' time against the wall clock times the number of cores (microseconds).
    rusage usage {};

    if (getrusage (RUSAGE_SELF, &usage) == 0)
    {
        const auto micros = [] (const timeval& t) { return (unsigned long long) t.tv_sec * 1000000ull + (unsigned long long) t.tv_usec; };
        const auto process = micros (usage.ru_utime) + micros (usage.ru_stime);
        const auto wall = (unsigned long long) std::chrono::duration_cast<std::chrono::microseconds> (
                              std::chrono::steady_clock::now().time_since_epoch()).count();

        if (lastWall != 0 && wall > lastWall && threads > 0)
            tawCpu = (float) (process - lastProcess) / (float) ((wall - lastWall) * (unsigned long long) threads);

        lastProcess = process;
        lastWall = wall;
    }

    // taw's memory as Activity Monitor counts it (its footprint).
    task_vm_info_data_t vm {};
    mach_msg_type_number_t vmCount = TASK_VM_INFO_COUNT;

    if (task_info (mach_task_self(), TASK_VM_INFO, (task_info_t) &vm, &vmCount) == KERN_SUCCESS)
        tawMemoryMB = (double) vm.phys_footprint / (1024.0 * 1024.0);

    // The machine's: all of it, and what's free for the taking (free, and inactive pages the
    // system hands out as needed).
    uint64_t total = 0;
    size_t size = sizeof (total);

    if (sysctlbyname ("hw.memsize", &total, &size, nullptr, 0) == 0)
        totalMemoryMB = (double) total / (1024.0 * 1024.0);

    vm_statistics64_data_t pages {};
    mach_msg_type_number_t pagesCount = HOST_VM_INFO64_COUNT;
    vm_size_t pageSize = 0;

    if (host_page_size (mach_host_self(), &pageSize) == KERN_SUCCESS
        && host_statistics64 (mach_host_self(), HOST_VM_INFO64, (host_info64_t) &pages, &pagesCount) == KERN_SUCCESS)
        freeMemoryMB = (double) ((unsigned long long) pages.free_count + pages.inactive_count) * (double) pageSize / (1024.0 * 1024.0);
}
#else
void SystemStats::sample()
{
    threads = (int) std::thread::hardware_concurrency();
}
#endif
} // namespace taw
