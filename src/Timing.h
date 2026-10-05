#pragma once

#include <juce_core/juce_core.h>

namespace taw::timing
{
// Where the time goes while taw starts and opens sessions: each step, how long it took and the
// time since launch, in timing.log in the config folder (started afresh each run). For finding
// what makes a start or an open slow; cheap enough to leave on.
juce::File logFile();

// Appends "  1234 ms  +56 ms  what" and resets the step clock.
void mark (const juce::String& what);

// Times a scope: logs it when it took at least minMs.
struct Scope
{
    explicit Scope (juce::String w, double min = 0.0) : what (std::move (w)), minMs (min), start (juce::Time::getMillisecondCounterHiRes()) {}
    ~Scope();

    juce::String what;
    double minMs, start;
};
} // namespace taw::timing
