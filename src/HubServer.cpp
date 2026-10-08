#include "HubServer.h"
#include "Safety.h"
#include "Session.h"

#include <map>

namespace taw::hub
{
constexpr juce::uint32 connectionMagic = 0x74617763; // "tawc": both ends of the control channel

//==============================================================================
Engine::~Engine()
{
    for (Plan* p = nullptr; pending.pop (p);)
        delete p;

    freeRetired();
    delete current;
}

void Engine::freeRetired()
{
    for (Plan* p = nullptr; retired.pop (p);)
        delete p;
}

void Engine::setPlan (std::unique_ptr<Plan> p)
{
    freeRetired();

    if (pending.push (p.get()))
        p.release(); // the audio thread's now; it comes back through retired
}

void Engine::audioDeviceAboutToStart (juce::AudioIODevice* d)
{
    sampleRate = d->getCurrentSampleRate();
    blockSize = d->getCurrentBufferSizeSamples();
    inputLatency = d->getInputLatencyInSamples();
    outputLatency = d->getOutputLatencyInSamples();
}

void Engine::process (const float* const* in, int numIn, float* const* out, int numOut, int n)
{
    // retired can't fill: it's emptied before each plan is set, and fewer plans than it holds
    // can be waiting. (Were it full, the old plan would leak rather than free memory here.)
    for (Plan* p = nullptr; pending.pop (p);)
    {
        if (current != nullptr)
            retired.push (current);

        current = p;
    }

    const auto cycle = cycles.fetch_add (1) + 1;

    for (int c = 0; c < numOut; ++c)
        if (out[c] != nullptr)
            juce::FloatVectorOperations::clear (out[c], n);

    auto* p = current;

    if (p == nullptr || n > Plan::maxBlock)
        return;

    // Sources: the interface's inputs, and what each client finished last cycle.
    for (int c = 0; c < p->hardwareInputs; ++c)
        p->sources[(size_t) c] = c < numIn && in[c] != nullptr ? in[c] : p->zeros.data();

    for (auto& client : p->clients)
    {
        auto& h = client.region->header();
        const auto done = h.doneCycle.load (std::memory_order_acquire);
        const bool ready = done == cycle - 1 && client.layout.blockSize == n;

        if (! ready && done != 0) // it has started, and missed this one
            h.late.fetch_add (1, std::memory_order_relaxed);

        for (int k = 0; k < client.layout.numOutputs; ++k)
            p->sources[(size_t) (client.firstSource + k)] = ready ? client.layout.output (client.region->data(), cycle - 1, k) : p->zeros.data();
    }

    // Destinations: each client's inputs for this cycle, and the interface's outputs.
    for (auto& client : p->clients)
        if (client.layout.blockSize == n)
            for (int k = 0; k < client.layout.numInputs; ++k)
                juce::FloatVectorOperations::clear (client.layout.input (client.region->data(), cycle, k), n);

    for (const auto& r : p->routes)
    {
        float* dest = nullptr;

        if (r.client < 0)
            dest = r.channel < numOut ? out[r.channel] : nullptr;
        else if (auto& client = p->clients[(size_t) r.client]; client.layout.blockSize == n)
            dest = client.layout.input (client.region->data(), cycle, r.channel);

        if (dest != nullptr)
            juce::FloatVectorOperations::addWithMultiply (dest, p->sources[(size_t) r.source], r.gain, n);
    }

    // Their inputs are ready: wake them.
    for (auto& client : p->clients)
    {
        auto& h = client.region->header();
        h.hubCycle.store (cycle, std::memory_order_release);
        wakeWaiter (h.hubCycle);
    }
}

//==============================================================================
// One client's control channel. Its messages arrive on its own thread.
struct Server::Connection final : juce::InterprocessConnection
{
    Connection (Server& s, int i) : juce::InterprocessConnection (false, connectionMagic), server (s), id (i) {}
    ~Connection() override { disconnect(); }

    void connectionMade() override {}
    void connectionLost() override { server.lost (*this); }

    void messageReceived (const juce::MemoryBlock& m) override
    {
        server.received (*this, juce::JSON::parse (m.toString()));
    }

    void send (const juce::var& v)
    {
        const auto text = juce::JSON::toString (v, true);
        sendMessage (juce::MemoryBlock (text.toRawUTF8(), text.getNumBytesAsUTF8()));
    }

    Server& server;
    const int id;
    juce::String name, key; // key: the session's folder, what routes name it by
    std::shared_ptr<SharedRegion> region; // once it said hello
    RegionLayout layout;
    bool joined = false;                  // it has the memory mapped: in the plan
    bool dead = false;                    // gone: to delete (tidy)
};

Server::Server (Engine& e) : engine (e)
{
    load();
}

Server::~Server()
{
    stopAll();
}

int Server::start()
{
    return beginWaitingForSocket (0, "127.0.0.1") ? getBoundPort() : 0;
}

juce::InterprocessConnection* Server::createConnectionObject()
{
    const juce::ScopedLock sl (lock);
    connections.push_back (std::make_unique<Connection> (*this, nextId++));
    return connections.back().get();
}

void Server::received (Connection& c, const juce::var& m)
{
    const auto type = m["type"].toString();
    const juce::ScopedLock sl (lock);

    if (type == "hello")
    {
        // A region sized for the interface as it runs now.
        const int block = engine.blockSize.load();
        const double rate = engine.sampleRate.load();
        auto* reply = new juce::DynamicObject();

        if (block <= 0 || rate <= 0.0)
        {
            reply->setProperty ("type", "refused");
            reply->setProperty ("reason", "the hub has no audio device running");
            c.send (reply);
            return;
        }

        c.name = m["name"].toString();
        c.key = m["key"].toString(); // empty: a taw with no session open (no routes until it has one)
        c.layout.numInputs = juce::jlimit (0, 64, juce::jmin ((int) m["inputs"], sessionInputs (hardwareInputs)));
        c.layout.numOutputs = juce::jlimit (0, 16, (int) m["outputs"]);
        c.layout.blockSize = block;

        juce::String error;
        const auto shmName = "/tawhub." + juce::String (safety::thisProcess()) + "." + juce::String (c.id);
        c.region = SharedRegion::create (shmName, c.layout.bytes(), error);

        if (c.region == nullptr)
        {
            reply->setProperty ("type", "refused");
            reply->setProperty ("reason", error);
            c.send (reply);
            return;
        }

        auto& h = c.region->header();
        h.numInputs = (std::uint32_t) c.layout.numInputs;
        h.numOutputs = (std::uint32_t) c.layout.numOutputs;
        h.blockSize = (std::uint32_t) block;
        h.sampleRate = rate;

        reply->setProperty ("type", "welcome");
        reply->setProperty ("shm", shmName);
        reply->setProperty ("inputs", c.layout.numInputs);
        reply->setProperty ("outputs", c.layout.numOutputs);
        reply->setProperty ("blockSize", block);
        reply->setProperty ("sampleRate", rate);
        reply->setProperty ("inputLatency", engine.inputLatency.load());
        reply->setProperty ("outputLatency", engine.outputLatency.load() + block); // played a cycle later
        c.send (reply);
    }
    else if (type == "ready" && c.region != nullptr)
    {
        c.region->unlink(); // mapped at both ends: the name isn't needed
        c.joined = true;
        remember (c);
        rebuild();
    }
    else if (type == "name")
    {
        // The session it's playing changed (taw opens its device before the session).
        c.name = m["name"].toString();
        c.key = m["key"].toString(); // empty: a taw with no session open (no routes until it has one)

        if (c.joined)
        {
            remember (c);
            rebuild();
        }
    }
    else if (type == "bye")
    {
        c.joined = false;
        c.dead = true;
        rebuild();
    }
    else if (type == "status")
    {
        c.send (status());
    }
    else if (type == "forget")
    {
        // A session and its routes, gone from the hub (it gets the defaults if it comes back).
        const auto key = m["key"].toString();
        const auto before = routes.size();
        std::erase_if (routes, [&] (const SavedRoute& r) { return r.from == key || r.to == key; });
        known.removeString (key);
        names.remove (juce::StringRef (key));
        save();
        rebuild();
        auto reply = status();
        reply.getDynamicObject()->setProperty ("removed", (int) (before - routes.size()));
        c.send (reply);
    }
    else if (type == "route" || type == "unroute")
    {
        const auto from = m["from"].toString(), to = m["to"].toString();
        const int fromChannel = m["fromChannel"], toChannel = m["toChannel"];
        auto reply = status();

        if (from.isEmpty() || to.isEmpty())
        {
            reply.getDynamicObject()->setProperty ("error", "a route needs both ends");
        }
        else if (type == "route")
        {
            if (fromChannel < 1 || toChannel < 1)
            {
                reply.getDynamicObject()->setProperty ("error", "channels count from 1");
            }
            else
            {
                const double db = m.hasProperty ("db") ? (double) m["db"] : 0.0;
                auto it = std::find_if (routes.begin(), routes.end(), [&] (const SavedRoute& r)
                                        { return r.from == from && r.fromChannel == fromChannel && r.to == to && r.toChannel == toChannel; });

                if (it != routes.end())
                    it->db = db;
                else
                    routes.push_back ({ from, fromChannel, to, toChannel, db });

                save();
                rebuild();
                reply = status();
            }
        }
        else
        {
            // Channel 0 means all of them.
            const auto before = routes.size();
            std::erase_if (routes, [&] (const SavedRoute& r)
                           { return r.from == from && r.to == to && (fromChannel == 0 || r.fromChannel == fromChannel)
                                    && (toChannel == 0 || r.toChannel == toChannel); });
            save();
            rebuild();
            reply = status();
            reply.getDynamicObject()->setProperty ("removed", (int) (before - routes.size()));
        }

        c.send (reply);
    }
}

// A session seen for the first time gets the defaults, so it works as it would on the
// interface: every input in order, and its first two outputs to the interface's (summed with
// other sessions'). From then on its routes are whatever they've been made.
void Server::remember (const Connection& c)
{
    if (c.key.isEmpty())
        return;

    if (known.contains (c.key))
    {
        names.set (c.key, c.name);
        return;
    }

    known.add (c.key);
    names.set (c.key, c.name);

    for (int k = 1; k <= juce::jmin (c.layout.numInputs, hardwareInputs); ++k)
        routes.push_back ({ interfaceKey, k, c.key, k, 0.0 });

    for (int k = 1; k <= juce::jmin (2, c.layout.numOutputs, hardwareOutputs); ++k)
        routes.push_back ({ c.key, k, interfaceKey, k, 0.0 });

    save();
}

juce::var Server::status() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("type", "status");
    o->setProperty ("device", deviceName);
    o->setProperty ("inputs", hardwareInputs);
    o->setProperty ("outputs", hardwareOutputs);
    o->setProperty ("apps", appDevices);
    o->setProperty ("sampleRate", engine.sampleRate.load());
    o->setProperty ("blockSize", engine.blockSize.load());

    juce::Array<juce::var> sessions;

    for (const auto& key : known)
    {
        auto* s = new juce::DynamicObject();
        s->setProperty ("key", key);
        s->setProperty ("name", names[key]);
        s->setProperty ("connected", false);

        for (const auto& c : connections)
            if (c->joined && c->key == key)
            {
                s->setProperty ("connected", true);
                s->setProperty ("inputs", c->layout.numInputs);
                s->setProperty ("outputs", c->layout.numOutputs);
                s->setProperty ("late", (juce::int64) c->region->header().late.load());
            }

        sessions.add (s);
    }

    o->setProperty ("sessions", sessions);

    juce::Array<juce::var> list;

    for (const auto& r : routes)
        list.add (r.toVar());

    o->setProperty ("routes", list);
    return o;
}

juce::var SavedRoute::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("from", from);
    o->setProperty ("fromChannel", fromChannel);
    o->setProperty ("to", to);
    o->setProperty ("toChannel", toChannel);
    o->setProperty ("db", db);
    return o;
}

SavedRoute SavedRoute::fromVar (const juce::var& v)
{
    return { v["from"].toString(), (int) v["fromChannel"], v["to"].toString(), (int) v["toChannel"], (double) v["db"] };
}

void Server::load()
{
    const auto v = juce::JSON::parse (routesFile());

    if (auto* list = v["routes"].getArray())
        for (const auto& r : *list)
            routes.push_back (SavedRoute::fromVar (r));

    if (auto* list = v["sessions"].getArray())
        for (const auto& s : *list)
        {
            known.add (s["key"].toString());
            names.set (s["key"].toString(), s["name"].toString());
        }
}

void Server::save() const
{
    juce::Array<juce::var> list, sessions;

    for (const auto& r : routes)
        list.add (r.toVar());

    for (const auto& key : known)
    {
        auto* s = new juce::DynamicObject();
        s->setProperty ("key", key);
        s->setProperty ("name", names[key]);
        sessions.add (s);
    }

    auto* o = new juce::DynamicObject();
    o->setProperty ("sessions", sessions);
    o->setProperty ("routes", list);
    routesFile().replaceWithText (juce::JSON::toString (juce::var (o)));
}

juce::File routesFile() { return configFolder().getChildFile ("hub.json"); }

void Server::lost (Connection& c)
{
    const juce::ScopedLock sl (lock);
    c.joined = false;
    c.dead = true;
    rebuild();
}

void Server::setHardware (int inputs, int outputs, const juce::String& device, int apps)
{
    const juce::ScopedLock sl (lock);
    hardwareInputs = inputs;
    hardwareOutputs = outputs;
    deviceName = device;
    appDevices = apps;
    rebuild();
}

// "taw 3" -> 3; anything else 0.
int appNumber (const juce::String& key)
{
    if (! key.startsWith ("taw ") || key.length() != 5)
        return 0;

    const int n = key.getLastCharacter() - '0';
    return n >= 1 && n <= 9 ? n : 0;
}

// The saved routes, for the sessions here now: those to or from one that isn't wait for it.
void Server::rebuild()
{
    auto p = std::make_unique<Plan>();
    p->hardwareInputs = hardwareInputs + 2 * appDevices; // the device's inputs: the interface's, then the apps'
    int sources = p->hardwareInputs;
    std::map<juce::String, int> clientOf;

    for (auto& c : connections)
    {
        if (! c->joined)
            continue;

        if (c->key.isNotEmpty())
            clientOf[c->key] = (int) p->clients.size();

        p->clients.push_back ({ c->region, c->layout, sources });
        sources += c->layout.numOutputs;
    }

    for (const auto& r : routes)
    {
        Plan::Route route;
        route.gain = juce::Decibels::decibelsToGain ((float) r.db);

        // The device's channels: the interface's, then each app device's two.
        const int fromApp = appNumber (r.from), toApp = appNumber (r.to);
        route.channel = r.toChannel - 1;

        if (r.from == interfaceKey)
        {
            if (r.fromChannel > hardwareInputs)
                continue;

            route.source = r.fromChannel - 1;
        }
        else if (fromApp > 0)
        {
            if (fromApp > appDevices || r.fromChannel > 2)
                continue;

            route.source = hardwareInputs + 2 * (fromApp - 1) + r.fromChannel - 1;
        }
        else if (auto it = clientOf.find (r.from); it != clientOf.end() && r.fromChannel <= p->clients[(size_t) it->second].layout.numOutputs)
        {
            route.source = p->clients[(size_t) it->second].firstSource + r.fromChannel - 1;
        }
        else
        {
            continue;
        }

        if (r.to == interfaceKey)
        {
            if (r.toChannel > hardwareOutputs)
                continue;

            route.client = -1;
        }
        else if (toApp > 0)
        {
            if (toApp > appDevices || r.toChannel > 2)
                continue;

            route.client = -1;
            route.channel = hardwareOutputs + 2 * (toApp - 1) + r.toChannel - 1;
        }
        else if (auto it = clientOf.find (r.to); it != clientOf.end() && r.toChannel <= p->clients[(size_t) it->second].layout.numInputs)
        {
            route.client = it->second;
        }
        else
        {
            continue;
        }

        p->routes.push_back (route);
    }

    p->sources.resize ((size_t) sources);
    p->zeros.assign ((size_t) Plan::maxBlock, 0.0f);
    engine.setPlan (std::move (p));
}

void Server::tidy()
{
    std::vector<std::unique_ptr<Connection>> gone;

    {
        const juce::ScopedLock sl (lock);
        engine.freeRetired(); // departed clients' memory goes with their last plan

        for (auto it = connections.begin(); it != connections.end();)
        {
            if ((*it)->dead)
            {
                gone.push_back (std::move (*it));
                it = connections.erase (it);
            }
            else
            {
                ++it;
            }
        }
    }

    gone.clear(); // outside the lock: disconnecting waits for their threads, which may want it
}

void Server::stopAll()
{
    stop(); // no new connections

    {
        const juce::ScopedLock sl (lock);
        auto* bye = new juce::DynamicObject();
        bye->setProperty ("type", "stopping");
        const juce::var message (bye);

        for (auto& c : connections)
        {
            if (c->region != nullptr)
            {
                auto& h = c->region->header();
                h.hubStopping.store (1);
                h.hubCycle.fetch_add (1); // out of their wait
                wakeWaiter (h.hubCycle);
            }

            c->send (message);
            c->dead = true;
        }

        for (auto& c : connections)
            c->joined = false;

        rebuild();
    }

    tidy();
}

std::vector<Server::ClientStatus> Server::clients() const
{
    const juce::ScopedLock sl (lock);
    std::vector<ClientStatus> out;

    for (auto& c : connections)
        if (c->joined)
            out.push_back ({ c->name, c->layout.numInputs, c->layout.numOutputs, c->region->header().late.load() });

    return out;
}
} // namespace taw::hub
