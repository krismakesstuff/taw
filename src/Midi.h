#pragma once

#include "SpscQueue.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace taw
{
// A short MIDI message: notes, controllers, pitch bend, pressure. No sysex, no clock.
struct MidiMsg
{
    juce::uint8 status = 0, d1 = 0, d2 = 0;

    int type() const noexcept { return status & 0xf0; }
    int channel() const noexcept { return status & 0x0f; } // 0-15
    bool isNoteOn() const noexcept { return type() == 0x90 && d2 > 0; }
    bool isNoteOff() const noexcept { return type() == 0x80 || (type() == 0x90 && d2 == 0); }
    int size() const noexcept { return type() == 0xc0 || type() == 0xd0 ? 2 : 3; }

    static MidiMsg noteOff (int channel, int pitch) { return { (juce::uint8) (0x80 | channel), (juce::uint8) pitch, 0 }; }
    static MidiMsg noteOn (int channel, int pitch, int vel) { return { (juce::uint8) (0x90 | channel), (juce::uint8) pitch, (juce::uint8) vel }; }
    static MidiMsg controller (int channel, int number, int value) { return { (juce::uint8) (0xb0 | channel), (juce::uint8) number, (juce::uint8) value }; }
};

// A message the audio thread heard while recording, at the playhead it arrived on.
struct TimedMidi
{
    juce::int64 pos = 0;
    MidiMsg msg;
};

using MidiQueue = SpscQueue<MidiMsg, 1024>;

// Controllers mapped to automation (or any, while learning a mapping) go to the message thread
// instead of the instruments, each with the time it arrived.
struct ControlMsg
{
    MidiMsg msg;
    double ms = 0.0; // juce::Time::getMillisecondCounterHiRes when it arrived
};

using ControlQueue = SpscQueue<ControlMsg, 512>;

struct ControlRouting
{
    std::array<std::atomic<juce::uint32>, 16 * 128 / 32> mapped {}; // a bit per channel and controller
    std::atomic<bool> learning { false };

    bool isMapped (int channel, int cc) const noexcept
    {
        const auto bit = channel * 128 + cc;
        return (mapped[(size_t) (bit / 32)].load (std::memory_order_relaxed) >> (bit % 32) & 1u) != 0;
    }

    // Message thread: the controllers the session's lanes listen to (channel -1: any).
    void set (const std::vector<std::pair<int, int>>& channelAndCc)
    {
        std::array<juce::uint32, 16 * 128 / 32> bits {};

        for (const auto& [ch, cc] : channelAndCc)
            for (int c = 0; c < 16; ++c)
                if ((ch < 0 || ch == c) && cc >= 0 && cc < 128)
                    bits[(size_t) ((c * 128 + cc) / 32)] |= 1u << ((c * 128 + cc) % 32);

        for (size_t i = 0; i < bits.size(); ++i)
            mapped[i].store (bits[i], std::memory_order_relaxed);
    }
};

// An instrument track's notes and controllers in samples, ready to play. Built from the
// session tree (where they're in beats) with the playback graph.
struct Sequence
{
    struct Note
    {
        juce::int64 start = 0, end = 0;
        juce::uint8 channel = 0, pitch = 60, vel = 100;
    };

    struct Control
    {
        juce::int64 pos = 0;
        MidiMsg msg;
    };

    std::vector<Note> notes;       // by start
    std::vector<Control> controls; // by position
};

// What a plugin is told about the transport (tempo-synced arps and LFOs read it).
struct HostTransport
{
    double bpm = 120.0, sampleRate = 48000.0;
    int timeSigNum = 4, timeSigDen = 4;
    juce::int64 pos = 0;
    bool rolling = false, recording = false;
};

// The MIDI inputs on the system. Every one is opened unless switched off in the MIDI panel
// ("midi"; the off list is kept in settings.json by name). Inputs come and go as devices are
// plugged in. Each open port feeds its own queue (one producer per queue); the audio thread
// reads them all. Ports open on a thread of their own: a driver can take seconds to open one
// (an audio interface's MIDI ports wait while its driver changes sample rate), and the screen
// shouldn't wait with it. The rest is called from the message thread.
class MidiInputs
{
public:
    static constexpr int maxPorts = 32;

    MidiInputs (std::array<MidiQueue, maxPorts>& queues, juce::StringArray switchedOff);
    ~MidiInputs();

    void refresh(); // soon (on the ports' thread): opens new devices, closes ones that went away or were switched off
    bool opening() const noexcept { return busy.load(); } // ports are being opened right now

    struct Device
    {
        juce::String name;
        bool on = true;    // not switched off
        bool open = false; // false while on: another app has it, or too many ports
        int events = 0;    // messages received since it opened
        MidiMsg last;      // the latest of them
    };

    std::vector<Device> devices() const; // every input on the system, in the system's order
    void setOn (const juce::String& name, bool on);
    const juce::StringArray& switchedOff() const { return off; }

    juce::StringArray names() const; // the open ones
    int eventsReceived() const noexcept { return received.load (std::memory_order_relaxed); }

    // Automation's controllers: mapped ones (and every controller while learning) come here,
    // not to the instruments. Read from the message thread.
    ControlRouting& controlRouting() { return control; }
    bool popControl (ControlMsg& out) noexcept
    {
        for (auto& q : controlQueues)
            if (q.pop (out))
                return true;

        return false;
    }

private:
    struct Port;

    void update(); // on the ports' thread

    std::array<MidiQueue, maxPorts>& queues;
    std::array<ControlQueue, maxPorts> controlQueues; // one per port, like queues
    ControlRouting control;
    std::vector<std::unique_ptr<Port>> ports;     // changed only on the ports' thread, under lock
    juce::Array<juce::MidiDeviceInfo> available;  // likewise
    juce::StringArray off;                        // under lock
    std::atomic<int> received { 0 };
    std::atomic<bool> busy { true };

    mutable std::mutex lock;
    std::condition_variable wake;
    bool wanted = true, quitting = false; // under lock
    std::thread worker;
    juce::MidiDeviceListConnection deviceListConnection;
};

// "note C4 vel 90", "cc 64 = 127", "bend +512": what a message is, for the MIDI panel.
juce::String describe (const MidiMsg&);
} // namespace taw
