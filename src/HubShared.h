#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>

// What the hub and its clients (taw sessions) share: a region of memory per client holding its
// audio, and a file saying where the hub listens.
//
// Each block the hub writes the client's inputs for cycle n into slot n % 2, publishes n in
// hubCycle and wakes the client. The client renders, writes its outputs into slot n % 2 and sets
// doneCycle to n. At cycle n + 1 the hub plays those outputs if doneCycle says they're there,
// and silence if not (a late block, counted). The hub never waits for a client; a client's
// outputs are heard one block later than straight from the device, which its device reports.
namespace taw::hub
{
struct RegionHeader
{
    static constexpr std::uint32_t magicValue = 0x74617768; // "tawh"
    static constexpr std::uint32_t currentVersion = 1;

    std::uint32_t magic = magicValue, version = currentVersion;
    std::uint32_t numInputs = 0, numOutputs = 0, blockSize = 0, reserved = 0;
    double sampleRate = 0.0;

    alignas (64) std::atomic<std::uint64_t> hubCycle { 0 };  // the cycle whose inputs are ready (the wait address)
    alignas (64) std::atomic<std::uint64_t> doneCycle { 0 }; // the cycle whose outputs the client has written
    alignas (64) std::atomic<std::uint64_t> late { 0 };      // cycles the client missed (the hub counts)
    std::atomic<std::uint32_t> hubStopping { 0 };            // the hub is going: stop waiting
};

static_assert (std::atomic<std::uint64_t>::is_always_lock_free, "the region's counters must work across processes");

// The region's audio, after the header: inputs[2][numInputs][blockSize], then
// outputs[2][numOutputs][blockSize].
struct RegionLayout
{
    int numInputs = 0, numOutputs = 0, blockSize = 0;

    static constexpr size_t audioOffset = (sizeof (RegionHeader) + 63) & ~(size_t) 63;
    size_t bytes() const { return audioOffset + sizeof (float) * (size_t) (2 * (numInputs + numOutputs) * blockSize); }

    float* input (void* base, std::uint64_t cycle, int channel) const
    {
        return audio (base) + ((size_t) (cycle & 1) * (size_t) numInputs + (size_t) channel) * (size_t) blockSize;
    }

    float* output (void* base, std::uint64_t cycle, int channel) const
    {
        return audio (base) + ((size_t) 2 * (size_t) numInputs + (size_t) (cycle & 1) * (size_t) numOutputs + (size_t) channel) * (size_t) blockSize;
    }

private:
    static float* audio (void* base) { return reinterpret_cast<float*> (static_cast<char*> (base) + audioOffset); }
};

// A named block of memory shared between processes (POSIX shm). The creator unlinks the name
// once the other side has it mapped, so nothing is left behind.
class SharedRegion
{
public:
    static std::unique_ptr<SharedRegion> create (const juce::String& name, size_t bytes, juce::String& error);
    static std::unique_ptr<SharedRegion> open (const juce::String& name, size_t bytes, juce::String& error);
    ~SharedRegion();

    void* data() const { return base; }
    RegionHeader& header() const { return *static_cast<RegionHeader*> (base); }
    void unlink(); // the name goes; the mappings stay

private:
    SharedRegion (juce::String n, void* b, size_t s) : name (std::move (n)), base (b), size (s) {}
    juce::String name;
    void* base = nullptr;
    size_t size = 0;
    bool linked = true;
};

// Waits until the counter isn't `seen` any more, or the timeout passes. Returns its value.
std::uint64_t waitForChange (std::atomic<std::uint64_t>&, std::uint64_t seen, int timeoutMs);
void wakeWaiter (std::atomic<std::uint64_t>&); // real-time safe: a non-blocking system call

// How many inputs a session gets: the interface's, and room for apps and other sessions to come
// in on even when it has few (a MacBook has one).
inline int sessionInputs (int hardwareInputs) { return std::max (hardwareInputs, 8); }

// Where the hub listens and what it runs at, for clients: <config>/hub-info.json. inputs: what
// a session gets (sessionInputs).
struct HubInfo
{
    int port = 0;
    juce::uint32 pid = 0;
    int inputs = 0, outputs = 0, blockSize = 0;
    double sampleRate = 0.0;
    juce::String device;

    juce::var toVar() const;
    static HubInfo fromVar (const juce::var&);
};

juce::File infoFile();
} // namespace taw::hub
