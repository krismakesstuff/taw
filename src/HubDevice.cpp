#include "HubDevice.h"
#include "HubShared.h"
#include "Safety.h"

#if JUCE_MAC
 #include <unistd.h>
#endif

namespace taw::hub
{
namespace
{
constexpr juce::uint32 connectionMagic = 0x74617763; // "tawc", as the hub's
constexpr int outputsWanted = 2;

// Who this taw is to the hub: the session's name, and its folder (what routes name it by).
struct Identity
{
    juce::String name = "taw", key;
};

Identity& identity()
{
    static Identity id;
    return id;
}

void addIdentity (juce::DynamicObject& m, const Identity& id)
{
    m.setProperty ("name", id.name);
    m.setProperty ("key", id.key);
}

// The devices there are now, to tell the hub when the session changes. Message thread.
std::vector<std::function<void()>*>& renamers()
{
    static std::vector<std::function<void()>*> list;
    return list;
}

HubInfo readInfo()
{
    return HubInfo::fromVar (juce::JSON::parse (infoFile()));
}

bool hubRunning (const HubInfo& info)
{
    return info.port > 0 && info.pid != 0 && safety::tawRunning (info.pid);
}

// The hub is a launchd agent: ask launchd to start it if it isn't running.
juce::String startHub()
{
#if JUCE_MAC
    juce::ChildProcess p;

    if (! p.start (juce::StringArray { "/bin/launchctl", "kickstart", "gui/" + juce::String ((int) getuid()) + "/com.taw.hub" }))
        return "couldn't run launchctl";

    p.waitForProcessToFinish (5000);

    if (p.getExitCode() != 0)
        return "the hub isn't installed: taw --install-hub";

    return {};
#else
    return "the hub runs on macOS only so far";
#endif
}

class HubIODevice final : public juce::AudioIODevice,
                          private juce::InterprocessConnection,
                          private juce::Thread
{
public:
    HubIODevice()
        : juce::AudioIODevice (deviceTypeName, deviceTypeName),
          juce::InterprocessConnection (false, connectionMagic),
          juce::Thread ("taw hub client"),
          info (readInfo())
    {
        rename = [this]
        {
            auto* m = new juce::DynamicObject();
            m->setProperty ("type", "name");
            me = identity();
            addIdentity (*m, me);
            send (m);
        };

        renamers().push_back (&rename);
    }

    ~HubIODevice() override
    {
        close();
        std::erase (renamers(), &rename);
    }

    // Tells the hub which session this is now, if connected.
    std::function<void()> rename;

    juce::StringArray getOutputChannelNames() override { return names ("out", outputsWanted); }
    juce::StringArray getInputChannelNames() override { return names ("in", info.inputs); }
    juce::Array<double> getAvailableSampleRates() override { return { info.sampleRate > 0.0 ? info.sampleRate : 48000.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { getDefaultBufferSize() }; }
    int getDefaultBufferSize() override { return info.blockSize > 0 ? info.blockSize : 512; }

    juce::String open (const juce::BigInteger&, const juce::BigInteger&, double, int) override
    {
        close();
        me = identity();
        lastError = connect (true);

        if (lastError.isNotEmpty())
            close();

        return lastError;
    }

    void close() override
    {
        stop();

        if (isConnected())
        {
            auto* bye = new juce::DynamicObject();
            bye->setProperty ("type", "bye");
            send (bye);
            disconnect();
        }

        region.reset();
    }

    bool isOpen() override { return region != nullptr; }

    void start (juce::AudioIODeviceCallback* cb) override
    {
        if (region == nullptr || cb == nullptr || callback != nullptr)
            return;

        cb->audioDeviceAboutToStart (this);
        callback = cb;
        startRealtimeThread (RealtimeOptions().withApproximateAudioProcessingTime (layout.blockSize, sampleRate));
    }

    void stop() override
    {
        if (callback == nullptr)
            return;

        stopThread (2000);
        auto* cb = std::exchange (callback, nullptr);
        cb->audioDeviceStopped();
    }

    bool isPlaying() override { return callback != nullptr; }
    juce::String getLastError() override { return lastError; }
    int getCurrentBufferSizeSamples() override { return layout.blockSize; }
    double getCurrentSampleRate() override { return sampleRate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return bits (layout.numOutputs); }
    juce::BigInteger getActiveInputChannels() const override { return bits (layout.numInputs); }
    int getOutputLatencyInSamples() override { return outputLatency; }
    int getInputLatencyInSamples() override { return inputLatency; }
    int getXRunCount() const noexcept override { return lateBlocks.load(); }

private:
    static juce::StringArray names (const char* prefix, int n)
    {
        juce::StringArray out;

        for (int i = 1; i <= n; ++i)
            out.add (juce::String (prefix) + " " + juce::String (i));

        return out;
    }

    static juce::BigInteger bits (int n)
    {
        juce::BigInteger b;
        b.setRange (0, n, true);
        return b;
    }

    // To the hub (starting it if need be and allowed), hello, welcome, its memory mapped, ready.
    juce::String connect (bool startIfNeeded)
    {
        hubGone = false;
        info = readInfo();

        if (! hubRunning (info) && ! startIfNeeded)
            return "the hub isn't running";

        if (! hubRunning (info))
        {
            if (const auto error = startHub(); error.isNotEmpty())
                return error;

            for (int i = 0; i < 50 && ! hubRunning (info = readInfo()); ++i)
                juce::Thread::sleep (100);

            if (! hubRunning (info))
                return "the hub didn't start (its log: hub.log in taw's settings folder)";
        }

        if (! connectToSocket ("127.0.0.1", info.port, 2000))
            return "couldn't reach the hub on port " + juce::String (info.port);

        reply = juce::var();
        replied.reset();
        auto* hello = new juce::DynamicObject();
        hello->setProperty ("type", "hello");
        addIdentity (*hello, me);
        hello->setProperty ("pid", (juce::int64) safety::thisProcess());
        hello->setProperty ("inputs", info.inputs);
        hello->setProperty ("outputs", outputsWanted);
        send (hello);

        if (! replied.wait (3000))
            return "the hub didn't answer";

        const auto r = [this] { const juce::ScopedLock sl (replyLock); return reply; }();

        if (r["type"].toString() != "welcome")
            return "the hub said no: " + r["reason"].toString();

        layout.numInputs = r["inputs"];
        layout.numOutputs = r["outputs"];
        layout.blockSize = r["blockSize"];
        sampleRate = r["sampleRate"];
        inputLatency = r["inputLatency"];
        outputLatency = r["outputLatency"];

        juce::String error;
        region = SharedRegion::open (r["shm"].toString(), layout.bytes(), error);

        if (region == nullptr)
            return error;

        auto* ready = new juce::DynamicObject();
        ready->setProperty ("type", "ready");
        send (ready);
        return {};
    }

    void send (const juce::var& v)
    {
        if (! isConnected())
            return;

        const auto text = juce::JSON::toString (v, true);
        sendMessage (juce::MemoryBlock (text.toRawUTF8(), text.getNumBytesAsUTF8()));
    }

    void connectionMade() override {}
    void connectionLost() override { hubGone = true; }

    void messageReceived (const juce::MemoryBlock& m) override
    {
        const auto v = juce::JSON::parse (m.toString());

        if (v["type"].toString() == "stopping")
        {
            hubGone = true;
            return;
        }

        {
            const juce::ScopedLock sl (replyLock);
            reply = v;
        }

        replied.signal();
    }

    // The audio thread. If the hub goes (restarted, stopped), it waits for it to come back and
    // carries on, so the session needn't choose its device again; only a hub that comes back at
    // another rate or block size needs that.
    void run() override
    {
        while (render())
        {
            callback->audioDeviceError ("the hub stopped: waiting for it to come back");
            const auto before = layout;
            const auto rate = sampleRate;
            disconnect();
            region.reset();

            while (! threadShouldExit() && region == nullptr)
            {
                wait (500);

                if (threadShouldExit() || ! hubRunning (readInfo()))
                    continue;

                if (connect (false).isEmpty())
                {
                    if (layout.numInputs == before.numInputs && layout.numOutputs == before.numOutputs
                        && layout.blockSize == before.blockSize && sampleRate == rate)
                        break;

                    callback->audioDeviceError ("the hub came back at another rate or block size: choose the device again (dev)");
                    return;
                }

                disconnect();
                region.reset();
            }
        }
    }

    // Each cycle the hub publishes, rendered. Returns true if the hub went, false when stopped.
    bool render()
    {
        auto& h = region->header();
        auto last = h.hubCycle.load (std::memory_order_acquire);
        std::vector<const float*> in ((size_t) layout.numInputs);
        std::vector<float*> out ((size_t) layout.numOutputs);
        const juce::AudioIODeviceCallbackContext context;

        while (! threadShouldExit())
        {
            const auto cycle = waitForChange (h.hubCycle, last, 100);

            if (hubGone || h.hubStopping.load() != 0)
                return true;

            if (cycle == last)
                continue; // nothing yet: look again

            for (int k = 0; k < layout.numInputs; ++k)
                in[(size_t) k] = layout.input (region->data(), cycle, k);

            for (int k = 0; k < layout.numOutputs; ++k)
                out[(size_t) k] = layout.output (region->data(), cycle, k);

            callback->audioDeviceIOCallbackWithContext (in.data(), layout.numInputs, out.data(), layout.numOutputs, layout.blockSize, context);
            h.doneCycle.store (cycle, std::memory_order_release);
            lateBlocks = (int) h.late.load (std::memory_order_relaxed);
            last = cycle;
        }

        return false;
    }

    HubInfo info;
    Identity me; // the session it says it is (taken when opened, and when renamed)
    juce::String lastError;
    std::unique_ptr<SharedRegion> region;
    RegionLayout layout;
    double sampleRate = 0.0;
    int inputLatency = 0, outputLatency = 0;
    juce::AudioIODeviceCallback* callback = nullptr;
    std::atomic<bool> hubGone { false };
    std::atomic<int> lateBlocks { 0 }; // the hub's count, as of the last block (region can go meanwhile)

    juce::CriticalSection replyLock;
    juce::var reply;
    juce::WaitableEvent replied;
};

class HubDeviceType final : public juce::AudioIODeviceType
{
public:
    HubDeviceType() : juce::AudioIODeviceType (deviceTypeName) {}

    void scanForDevices() override {}
    juce::StringArray getDeviceNames (bool) const override { return { deviceTypeName }; }
    int getDefaultDeviceIndex (bool) const override { return 0; }
    int getIndexOfDevice (juce::AudioIODevice* d, bool) const override { return d != nullptr ? 0 : -1; }
    bool hasSeparateInputsAndOutputs() const override { return false; }

    juce::AudioIODevice* createDevice (const juce::String&, const juce::String&) override { return new HubIODevice(); }
};
} // namespace

void setClientName (const juce::String& name, const juce::String& key, bool tellConnected)
{
    identity() = { name, key };

    if (tellConnected)
        for (auto* r : renamers())
            (*r)();
}

std::unique_ptr<juce::AudioIODeviceType> makeDeviceType() { return std::make_unique<HubDeviceType>(); }

juce::var request (const juce::var& message, juce::String& error)
{
    struct Asker final : juce::InterprocessConnection
    {
        Asker() : juce::InterprocessConnection (false, connectionMagic) {}
        ~Asker() override { disconnect(); }

        void connectionMade() override {}
        void connectionLost() override { answered.signal(); }

        void messageReceived (const juce::MemoryBlock& m) override
        {
            {
                const juce::ScopedLock sl (lock);
                answer = juce::JSON::parse (m.toString());
            }

            answered.signal();
        }

        juce::CriticalSection lock;
        juce::var answer;
        juce::WaitableEvent answered;
    } asker;

    const auto info = readInfo();

    if (! hubRunning (info))
    {
        error = "the hub isn't running (hub start, or taw --install-hub)";
        return {};
    }

    if (! asker.connectToSocket ("127.0.0.1", info.port, 1000))
    {
        error = "couldn't reach the hub on port " + juce::String (info.port);
        return {};
    }

    const auto text = juce::JSON::toString (message, true);
    asker.sendMessage (juce::MemoryBlock (text.toRawUTF8(), text.getNumBytesAsUTF8()));

    if (! asker.answered.wait (1000))
    {
        error = "the hub didn't answer";
        return {};
    }

    const juce::ScopedLock sl (asker.lock);

    if (asker.answer.isVoid())
        error = "the hub hung up";

    return asker.answer;
}
} // namespace taw::hub
