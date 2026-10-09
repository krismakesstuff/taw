#include "HubShared.h"
#include "Session.h"

#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

#if JUCE_MAC
 #include <os/os_sync_wait_on_address.h>
#endif

namespace taw::hub
{
std::unique_ptr<SharedRegion> SharedRegion::create (const juce::String& name, size_t bytes, juce::String& error)
{
    shm_unlink (name.toRawUTF8()); // one left by a hub that crashed
    const int fd = shm_open (name.toRawUTF8(), O_CREAT | O_EXCL | O_RDWR, 0600);

    if (fd < 0)
    {
        error = "shared memory " + name + ": " + juce::String (strerror (errno));
        return {};
    }

    void* base = ftruncate (fd, (off_t) bytes) == 0 ? mmap (nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    close (fd);

    if (base == MAP_FAILED)
    {
        error = "shared memory " + name + ": " + juce::String (strerror (errno));
        shm_unlink (name.toRawUTF8());
        return {};
    }

    std::memset (base, 0, bytes);
    new (base) RegionHeader();
    return std::unique_ptr<SharedRegion> (new SharedRegion (name, base, bytes));
}

std::unique_ptr<SharedRegion> SharedRegion::open (const juce::String& name, size_t bytes, juce::String& error)
{
    const int fd = shm_open (name.toRawUTF8(), O_RDWR, 0600);

    if (fd < 0)
    {
        error = "shared memory " + name + ": " + juce::String (strerror (errno));
        return {};
    }

    void* base = mmap (nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close (fd);

    if (base == MAP_FAILED)
    {
        error = "shared memory " + name + ": " + juce::String (strerror (errno));
        return {};
    }

    std::unique_ptr<SharedRegion> r (new SharedRegion (name, base, bytes));
    r->linked = false; // the creator's to unlink

    if (r->header().magic != RegionHeader::magicValue || r->header().version != RegionHeader::currentVersion)
    {
        error = "the hub is a different version of taw: rebuild or restart it (taw --install-hub)";
        return {};
    }

    return r;
}

SharedRegion::~SharedRegion()
{
    unlink();
    munmap (base, size);
}

void SharedRegion::unlink()
{
    if (std::exchange (linked, false))
        shm_unlink (name.toRawUTF8());
}

//==============================================================================
std::uint64_t waitForChange (std::atomic<std::uint64_t>& counter, std::uint64_t seen, int timeoutMs)
{
#if JUCE_MAC
    if (const auto now = counter.load (std::memory_order_acquire); now != seen)
        return now;

    if (__builtin_available (macOS 14.4, *))
    {
        os_sync_wait_on_address_with_timeout (&counter, seen, sizeof (seen), OS_SYNC_WAIT_ON_ADDRESS_SHARED, OS_CLOCK_MACH_ABSOLUTE_TIME,
                                              (std::uint64_t) juce::jmax (1, timeoutMs) * 1000000ull);
        return counter.load (std::memory_order_acquire);
    }
#endif

    // No way to sleep on the address: look every 200 us.
    const auto until = juce::Time::getMillisecondCounterHiRes() + timeoutMs;

    for (auto now = counter.load (std::memory_order_acquire);; now = counter.load (std::memory_order_acquire))
    {
        if (now != seen || juce::Time::getMillisecondCounterHiRes() >= until)
            return now;

        std::this_thread::sleep_for (std::chrono::microseconds (200));
    }
}

void wakeWaiter (std::atomic<std::uint64_t>& counter)
{
#if JUCE_MAC
    if (__builtin_available (macOS 14.4, *))
        os_sync_wake_by_address_any (&counter, sizeof (std::uint64_t), OS_SYNC_WAKE_BY_ADDRESS_SHARED);
#else
    juce::ignoreUnused (counter);
#endif
}

//==============================================================================
juce::var HubInfo::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("port", port);
    o->setProperty ("pid", (juce::int64) pid);
    o->setProperty ("inputs", inputs);
    o->setProperty ("outputs", outputs);
    o->setProperty ("blockSize", blockSize);
    o->setProperty ("sampleRate", sampleRate);
    o->setProperty ("device", device);
    return o;
}

HubInfo HubInfo::fromVar (const juce::var& v)
{
    HubInfo i;
    i.port = v["port"];
    i.pid = (juce::uint32) (juce::int64) v["pid"];
    i.inputs = v["inputs"];
    i.outputs = v["outputs"];
    i.blockSize = v["blockSize"];
    i.sampleRate = v["sampleRate"];
    i.device = v["device"].toString();
    return i;
}

juce::File infoFile() { return configFolder().getChildFile ("hub-info.json"); }
} // namespace taw::hub
