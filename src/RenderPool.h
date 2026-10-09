#pragma once

#include <atomic>
#include <memory>
#include <semaphore>
#include <thread>
#include <vector>

namespace taw
{
// Worker threads that help the audio thread render a block: the tracks are independent until
// they're mixed, so each one can render on its own core. Workers run at the audio thread's
// priority (Windows: MMCSS "Pro Audio") and sleep between blocks.
//
// run() hands out jobs 0..count-1, does some itself, and returns once every one is done. No
// allocation or locks on the way: an atomic counter for the jobs, a semaphore to wake workers.
class RenderPool
{
public:
    using Job = void (*) (void* context, int index);

    RenderPool() = default;
    ~RenderPool() { stop(); }

    void start (int numWorkers);
    void stop();
    int numWorkers() const noexcept { return (int) workers.size(); }

    // Audio thread only.
    void run (int count, Job job, void* context) noexcept;

private:
    void work() noexcept;
    void take() noexcept; // jobs until none are left

    std::vector<std::thread> workers;
    std::counting_semaphore<64> wake { 0 };
    std::atomic<bool> quit { false };

    Job job = nullptr;
    void* context = nullptr;
    int count = 0;
    std::atomic<int> next { 0 };
    std::atomic<int> active { 0 }; // workers woken for this block and not yet back
};
} // namespace taw
