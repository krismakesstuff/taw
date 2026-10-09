#include "RenderPool.h"

#include <algorithm>

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <avrt.h>
 #pragma comment(lib, "avrt.lib")
#endif

#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
 #include <immintrin.h>
 #define TAW_PAUSE() _mm_pause()
#else
 #define TAW_PAUSE() std::this_thread::yield()
#endif

namespace taw
{
void RenderPool::start (int numWorkers)
{
    stop();
    quit.store (false);

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
#endif

#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
    _mm_setcsr (_mm_getcsr() | 0x8040); // denormals flushed to zero, like the audio thread
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
