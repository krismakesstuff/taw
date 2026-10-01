#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace taw
{
// Fixed-capacity, wait-free, single-producer / single-consumer queue.
// Used to pass things between the audio thread and the message thread without locks.
template <typename T, std::size_t Capacity>
class SpscQueue
{
public:
    bool push (const T& value) noexcept
    {
        const auto t = tail.load (std::memory_order_relaxed);
        const auto next = (t + 1) % Capacity;

        if (next == head.load (std::memory_order_acquire))
            return false;

        items[t] = value;
        tail.store (next, std::memory_order_release);
        return true;
    }

    bool pop (T& out) noexcept
    {
        const auto h = head.load (std::memory_order_relaxed);

        if (h == tail.load (std::memory_order_acquire))
            return false;

        out = items[h];
        head.store ((h + 1) % Capacity, std::memory_order_release);
        return true;
    }

private:
    std::array<T, Capacity> items {};
    std::atomic<std::size_t> head { 0 }, tail { 0 };
};
} // namespace taw
