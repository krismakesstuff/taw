#include "RenderPool.h"

#include <algorithm>

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <avrt.h>
 #pragma comment(lib, "avrt.lib")
#elif defined(__APPLE__)
 #include <mach/mach.h>
 #include <mach/mach_time.h>
 #include <mach/thread_policy.h>
 #include <pthread.h>
#endif

#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
 #include <immintrin.h>
 #define TAW_PAUSE() _mm_pause()
#else
 #define TAW_PAUSE() std::this_thread::yield()
#endif

namespace taw
{
void RenderPool::start (int numWorkers, const Timing& t)
{
    stop();
    quit.store (false);
    timing = t;

    for (int i = 0; i < std::clamp (numWorkers, 0, 32); ++i)
        workers.emplace_back ([this] { work(); });
}

void RenderPool::stop()
{
    if (workers.empty())
        return;

    quit.store (true);
    wake.release ((std::ptrdiff_t) workers.size());

    for (auto& w : workers)
        w.join();

    workers.clear();
}

void RenderPool::work() noexcept
{
#if defined(_WIN32)
    // As the audio thread: the multimedia scheduler's pro audio class.
    DWORD task = 0;
    if (auto h = AvSetMmThreadCharacteristicsW (L"Pro Audio", &task))
        AvSetMmThreadPriority (h, AVRT_PRIORITY_HIGH);
    else
        SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#elif defined(__APPLE__)
    // As the audio thread: real time with a block's length as the period (as JUCE's real-time
    // threads, with only a period), then in its workgroup, as Apple asks of an audio thread's
    // helpers. Without a device (tests), normal priority.
    juce::WorkgroupToken token;

    if (timing.rate > 0.0 && timing.blockSize > 0)
    {
        mach_timebase_info_data_t base {};
        mach_timebase_info (&base);
        const auto period = (uint32_t) ((double) timing.blockSize / timing.rate * 1.0e9 * base.denom / base.numer);

        thread_time_constraint_policy_data_t policy { period, period, period, true };
        thread_policy_set (pthread_mach_thread_np (pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                           (thread_policy_t) &policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT);

        if (timing.workgroup)
            timing.workgroup.join (token);
    }
#endif

#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
    _mm_setcsr (_mm_getcsr() | 0x8040); // denormals flushed to zero, like the audio thread
#elif defined(__aarch64__) || defined(_M_ARM64)
    juce::FloatVectorOperations::disableDenormalisedNumberSupport(); // FPCR's flush to zero
#endif

    for (;;)
    {
        wake.acquire();

        if (quit.load())
            return;

        take();
        active.fetch_sub (1, std::memory_order_acq_rel);
    }
}

void RenderPool::take() noexcept
{
    for (int i; (i = next.fetch_add (1, std::memory_order_acq_rel)) < count;)
        job (context, i);
}

void RenderPool::run (int jobs, Job j, void* ctx) noexcept
{
    if (jobs <= 0)
        return;

    // Each woken worker stays in this block until it's back (active), so none is left holding
    // a job from this block when the next one starts.
    const int helpers = std::min ((int) workers.size(), jobs - 1);

    job = j;
    context = ctx;
    count = jobs;
    active.store (helpers, std::memory_order_relaxed);
    next.store (0, std::memory_order_release);

    if (helpers > 0)
        wake.release (helpers);

    take();

    while (active.load (std::memory_order_acquire) > 0)
        TAW_PAUSE();
}
} // namespace taw
