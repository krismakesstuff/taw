#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <vector>

namespace taw
{
// A Pro Tools session (.ptx, Pro Tools 10 and later) read into a taw session, through ptformat
// (third_party/ptformat): its audio tracks with their clips and other playlists, its MIDI tracks'
// notes, and its tempo. What doesn't come across (plugins, the mixer, fades, stereo tracks
// recorded as .L/.R pairs, tempo changes) is listed in the report. The session and its files
// are only read. Experimental.
struct ProToolsSession
{
    juce::ValueTree session; // a SESSION tree, ready for Session::create
    double sampleRate = 44100.0;
    struct Copy
    {
        juce::File from;
        juce::String to; // in the session folder, e.g. "audio/Gtr 1_01.wav"
    };
    std::vector<Copy> copies;
    juce::StringArray report; // what didn't come across
    juce::String error;       // couldn't read it at all
};

ProToolsSession readProToolsSession (const juce::File& ptx);

// Makes the session folder (at the Pro Tools session's own rate): session.json, the audio copied
// in, import-report.txt. False (and error) if it couldn't.
bool writeProToolsSession (const ProToolsSession&, const juce::File& dir, juce::String& error);
} // namespace taw
