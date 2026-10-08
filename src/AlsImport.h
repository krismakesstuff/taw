#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <vector>

namespace taw
{
// An Ableton Live set (.als) read into a taw session: tempo, tracks and their clips and notes,
// the mixer (volume, pan, mute, sends to the returns, which become buses), VST3 plugins with
// their saved state (instruments, inserts, the master's on the mix) and automation as lanes.
// What doesn't come across (Live's own devices, time-stretched audio, clip envelopes) is listed
// in the report. The set and its files are only read.
struct AbletonSet
{
    juce::ValueTree session; // a SESSION tree, ready for Session::create
    struct File
    {
        juce::String path; // in the session folder, e.g. "plugins/01-ZENOLOGY.state"
        juce::MemoryBlock data;
    };
    std::vector<File> files;
    struct Copy
    {
        juce::File from;
        juce::String to; // in the session folder, e.g. "audio/Guitar.wav"
    };
    std::vector<Copy> copies;
    juce::StringArray report; // what came across, and what didn't
    juce::String error;       // couldn't read it at all
};

AbletonSet readAbletonSet (const juce::File& als, double sampleRate);

// Makes the session folder: session.json, the plugin states, the audio copied in. False (and
// error) if it couldn't; the report goes in import-report.txt beside it.
bool writeAbletonSession (const AbletonSet&, const juce::File& dir, double sampleRate, juce::String& error);
} // namespace taw
