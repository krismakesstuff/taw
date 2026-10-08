#pragma once

#include "HubShared.h"
#include "SpscQueue.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <memory>
#include <vector>

// The hub's side: the engine (the routing, run each block on the interface's audio thread) and
// the server (sessions connecting, their shared memory, the routes between them).
namespace taw::hub
{
// What the audio thread runs: the clients and the routes between them. Built on other threads,
// handed over through a queue, and freed back there (clients' memory goes with the last plan
// that uses it).
struct Plan
{
    struct Client
    {
        std::shared_ptr<SharedRegion> region;
        RegionLayout layout;
        int firstSource = 0; // where its outputs are among the sources
    };

    // A source channel into a destination: the hardware outputs (client -1) or a client's inputs.
    struct Route
    {
        int source = 0, client = -1, channel = 0;
        float gain = 1.0f;
    };

    int hardwareInputs = 0; // sources 0.. are the interface's inputs, then each client's outputs
    std::vector<Client> clients;
    std::vector<Route> routes;

    std::vector<const float*> sources; // filled each block
    std::vector<float> zeros;          // silence, for whatever has none
    static constexpr int maxBlock = 8192;
};

class Engine final : public juce::AudioIODeviceCallback
{
public:
    Engine() = default;
    ~Engine() override;

    // From one thread at a time (the server holds its lock). freeRetired frees plans the audio
    // thread is done with (setPlan does too).
    void setPlan (std::unique_ptr<Plan>);
    void freeRetired();

    // One block: the interface's inputs in, its outputs out. The device callback calls this;
    // tests call it straight.
    void process (const float* const* in, int numIn, float* const* out, int numOut, int numSamples);

    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int n,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        process (in, numIn, out, numOut, n);
    }

    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override {}

    // What the interface runs at, for clients (set when it starts).
    std::atomic<double> sampleRate { 0.0 };
    std::atomic<int> blockSize { 0 }, inputLatency { 0 }, outputLatency { 0 };
    std::atomic<std::uint64_t> cycles { 0 };

private:
    SpscQueue<Plan*, 64> pending, retired;
    Plan* current = nullptr;
};

// A route as saved (hub.json) and sent: channels count from 1. Its ends are the interface
// (interfaceKey) or a session, by its folder; one to or from a session that isn't connected
// waits for it.
struct SavedRoute
{
    juce::String from;
    int fromChannel = 1;
    juce::String to;
    int toChannel = 1;
    double db = 0.0;

    juce::var toVar() const;
    static SavedRoute fromVar (const juce::var&);
};

inline constexpr const char* interfaceKey = "interface";
juce::File routesFile(); // <config>/hub.json

class Server final : private juce::InterprocessConnectionServer
{
public:
    explicit Server (Engine&); // with the routes saved in hub.json
    ~Server() override;

    // Listens on a free local port. Returns it (0: couldn't).
    int start();

    // The interface, when its device (re)starts.
    void setHardware (int inputs, int outputs, const juce::String& device);

    // Tells every client the hub is going, then lets them go.
    void stopAll();

    // Lets go of clients that disconnected. Call now and then (not from their threads).
    void tidy();

    struct ClientStatus
    {
        juce::String name;
        int inputs = 0, outputs = 0;
        std::uint64_t late = 0;
    };

    std::vector<ClientStatus> clients() const;

private:
    struct Connection;
    friend struct Connection;

    juce::InterprocessConnection* createConnectionObject() override;
    void received (Connection&, const juce::var&);
    void lost (Connection&);

    // Under lock.
    void rebuild();
    void remember (const Connection&);
    juce::var status() const;
    void load();
    void save() const;

    Engine& engine;
    juce::CriticalSection lock;
    std::vector<std::unique_ptr<Connection>> connections;
    int nextId = 1;
    int hardwareInputs = 0, hardwareOutputs = 0;
    juce::String deviceName;

    std::vector<SavedRoute> routes;
    juce::StringArray known;    // sessions seen (keys): they have their routes
    juce::StringPairArray names; // key -> the name it last had
};
} // namespace taw::hub
