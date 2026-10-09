#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <memory>
#include <semaphore>
#include <thread>
#include <vector>

namespace taw
{
// Worker threads that help the audio thread render a block: the tracks are independent until
// they're mixed, so each one can render on its own core. Workers run at the audio thread's
// priority (Windows: MMCSS "Pro Audio"; macOS: real time for the block's length, in the
// device's audio workgroup) and sleep between blocks.
//
// run() hands out jobs 0..count-1, does some itself, and returns once every one is done. No
// allocation or locks on the way: an atomic counter for the jobs, a semaphore to wake workers.
class RenderPool
{
public:
    using Job = void (*) (void* context, int index);

    RenderPool() = default;
    ~RenderPool() { stop(); }

    // The device's block, for the workers' scheduling on macOS: a block's length is their real-
    // time period, and they join its workgroup (performance cores, alongside the audio thread).
    // Workers start with them, so a new device means start() again.
    struct Timing
    {
        double rate = 0.0;
        int blockSize = 0;
        juce::AudioWorkgroup workgroup;
    };

    void start (int numWorkers, const Timing&);
    void start (int numWorkers) { start (numWorkers, Timing {}); }
    void stop();
    int numWorkers() const noexcept { return (int) workers.size(); }

    // Audio thread only.
    void run (int count, Job job, void* context) noexcept;

private:
    void work() noexcept;
    void take() noexcept; // jobs until none are left

    std::vector<std::thread> workers;
    Timing timing;
    std::counting_semaphore<64> wake { 0 };
    std::atomic<bool> quit { false };

    Job job = nullptr;
    void* context = nullptr;
    int count = 0;
    std::atomic<int> next { 0 };
    std::atomic<int> active { 0 }; // workers woken for this block and not yet back
};
} // namespace taw
