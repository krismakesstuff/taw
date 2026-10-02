#pragma once

#include "SpscQueue.h"

#include <juce_audio_devices/juce_audio_devices.h>

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
// reads them all. Message thread.
class MidiInputs
{
public:
    static constexpr int maxPorts = 32;

    MidiInputs (std::array<MidiQueue, maxPorts>& queues, juce::StringArray switchedOff);
    ~MidiInputs();

    void refresh(); // opens new devices, closes ones that went away or were switched off

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

private:
    struct Port;

    std::array<MidiQueue, maxPorts>& queues;
    std::vector<std::unique_ptr<Port>> ports;
    juce::Array<juce::MidiDeviceInfo> available;
    juce::StringArray off;
    std::atomic<int> received { 0 };
    juce::MidiDeviceListConnection deviceListConnection;
};

// "note C4 vel 90", "cc 64 = 127", "bend +512": what a message is, for the MIDI panel.
juce::String describe (const MidiMsg&);
} // namespace taw
