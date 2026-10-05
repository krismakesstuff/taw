#include "Midi.h"
#include "Timing.h"

namespace taw
{
struct MidiInputs::Port final : juce::MidiInputCallback
{
    juce::MidiDeviceInfo info;
    int slot = -1;
    MidiQueue* queue = nullptr;
    std::atomic<int>* received = nullptr;
    std::atomic<int> events { 0 };
    std::atomic<juce::uint32> last { 0 }; // the latest message, packed, for the panel
    std::unique_ptr<juce::MidiInput> input;

    // On the device's own thread.
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override
    {
        const auto* raw = m.getRawData();

        // Short channel messages only: sysex, clock and active sensing are of no use here.
        if (m.getRawDataSize() < 1 || raw[0] < 0x80 || raw[0] >= 0xf0)
            return;

        MidiMsg msg { raw[0], m.getRawDataSize() > 1 ? raw[1] : (juce::uint8) 0, m.getRawDataSize() > 2 ? raw[2] : (juce::uint8) 0 };

        if (msg.isNoteOff())
            msg = MidiMsg::noteOff (msg.channel(), msg.d1); // a note-on at velocity 0 is a note-off

        queue->push (msg); // a full queue drops it
        last.store ((juce::uint32) msg.status << 16 | (juce::uint32) msg.d1 << 8 | msg.d2, std::memory_order_relaxed);
        events.fetch_add (1, std::memory_order_relaxed);
        received->fetch_add (1, std::memory_order_relaxed);
    }
};

MidiInputs::MidiInputs (std::array<MidiQueue, maxPorts>& q, juce::StringArray switchedOff)
    : queues (q), off (std::move (switchedOff)),
      deviceListConnection (juce::MidiDeviceListConnection::make ([this] { refresh(); }))
{
    // The first update is already wanted: it opens everything.
    worker = std::thread ([this]
    {
        for (;;)
        {
            {
                std::unique_lock l (lock);
                wake.wait (l, [this] { return wanted || quitting; });

                if (quitting)
                    return;

                wanted = false;
            }

            busy = true;
            update();
            busy = false;
        }
    });
}

MidiInputs::~MidiInputs()
{
    {
        const std::scoped_lock l (lock);
        quitting = true;
    }

    wake.notify_one();
    worker.join(); // after any port it's opening

    for (auto& p : ports)
        p->input.reset(); // stops its callbacks before the port goes
}

void MidiInputs::refresh()
{
    {
        const std::scoped_lock l (lock);
        wanted = true;
    }

    wake.notify_one();
}

void MidiInputs::update()
{
    timing::mark ("MIDI: listing inputs");
    const auto found = juce::MidiInput::getAvailableDevices();
    timing::mark ("MIDI: " + juce::String (found.size()) + " inputs listed");
    juce::StringArray switchedOff;
    std::vector<std::unique_ptr<Port>> closing;

    // Close ports whose device is gone or was switched off. Stopping it first means its queue
    // has no producer left, so the slot can go to the next device.
    {
        const std::scoped_lock l (lock);
        available = found;
        switchedOff = off;

        for (auto it = ports.begin(); it != ports.end();)
        {
            if (! available.contains ((*it)->info) || off.contains ((*it)->info.name))
            {
                closing.push_back (std::move (*it));
                it = ports.erase (it);
            }
            else
            {
                ++it;
            }
        }
    }

    for (auto& p : closing)
        p->input.reset();

    closing.clear();

    // Only this thread changes ports, so it can read them without the lock.
    for (const auto& info : found)
    {
        if (switchedOff.contains (info.name) || std::any_of (ports.begin(), ports.end(), [&] (const auto& p) { return p->info == info; }))
            continue;

        int slot = 0;

        while (slot < maxPorts && std::any_of (ports.begin(), ports.end(), [&] (const auto& p) { return p->slot == slot; }))
            ++slot;

        if (slot == maxPorts)
            break;

        auto port = std::make_unique<Port>();
        port->info = info;
        port->slot = slot;
        port->queue = &queues[(size_t) slot];
        port->received = &received;
        port->input = juce::MidiInput::openDevice (info.identifier, port.get());
        timing::mark ("MIDI: opened " + info.name + (port->input == nullptr ? " (failed)" : ""));

        if (port->input == nullptr)
            continue; // another app has it open (Windows doesn't share MIDI inputs)

        port->input->start();
        const std::scoped_lock l (lock);
        ports.push_back (std::move (port));
    }
}

std::vector<MidiInputs::Device> MidiInputs::devices() const
{
    const std::scoped_lock l (lock);
    std::vector<Device> out;

    for (const auto& info : available)
    {
        Device d;
        d.name = info.name;
        d.on = ! off.contains (info.name);

        for (const auto& p : ports)
            if (p->info == info)
            {
                const auto packed = p->last.load (std::memory_order_relaxed);
                d.open = true;
                d.events = p->events.load (std::memory_order_relaxed);
                d.last = { (juce::uint8) (packed >> 16), (juce::uint8) (packed >> 8), (juce::uint8) packed };
            }

        out.push_back (d);
    }

    return out;
}

void MidiInputs::setOn (const juce::String& name, bool on)
{
    {
        const std::scoped_lock l (lock);

        if (on)
            off.removeString (name);
        else
            off.addIfNotAlreadyThere (name);
    }

    refresh();
}

juce::StringArray MidiInputs::names() const
{
    const std::scoped_lock l (lock);
    juce::StringArray s;

    for (const auto& p : ports)
        s.add (p->info.name);

    return s;
}

juce::String describe (const MidiMsg& m)
{
    const auto ch = m.channel() != 0 ? " ch" + juce::String (m.channel() + 1) : juce::String();

    switch (m.type())
    {
        case 0x90: return "note " + juce::MidiMessage::getMidiNoteName (m.d1, true, true, 4) + " vel " + juce::String (m.d2) + ch;
        case 0x80: return "note off " + juce::MidiMessage::getMidiNoteName (m.d1, true, true, 4) + ch;
        case 0xb0: return "cc " + juce::String (m.d1) + " = " + juce::String (m.d2) + ch;
        case 0xe0: return "bend " + juce::String ((m.d1 | (m.d2 << 7)) - 8192) + ch;
        case 0xd0: return "pressure " + juce::String (m.d1) + ch;
        case 0xa0: return "aftertouch " + juce::MidiMessage::getMidiNoteName (m.d1, true, true, 4) + ch;
        case 0xc0: return "program " + juce::String (m.d1 + 1) + ch;
        default:   return {};
    }
}
} // namespace taw
