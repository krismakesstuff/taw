#pragma once

#include <juce_core/juce_core.h>

#include <atomic>

// What taw does when things go wrong: crash reports, a last-chance save, a clean shutdown when
// the console window is closed, and noticing on the next launch that the last run didn't end.
namespace taw::safety
{
// Unhandled crashes (any thread) write a minidump and a log line to %APPDATA%\taw\crashes,
// put the console back the way it was, and try one last save. Call once, first thing.
void installCrashHandler();
juce::File crashFolder();

// The crash handler calls this to save. Clear it (nullptr) before the target goes away.
void setEmergencySave (void (*save) (void*), void* context);

// So a crash can restore the console modes taw changed (Console::open records them).
void rememberConsole (void* in, unsigned long inMode, void* out, unsigned long outMode);

// Closing the console window, logging off or Ctrl+Break: ask the app to quit, then wait (the
// OS allows about five seconds) until main says shutdown is complete.
void installCloseHandler();
bool closeRequested();
void shutdownComplete();

// A marker per running taw (%APPDATA%\taw\running\<pid>.json) naming its session. A marker
// whose process is gone means that run didn't exit cleanly. Only interactive runs keep one:
// startRunMarker turns it on, markRunning updates the session it names.
void startRunMarker();
void markRunning (const juce::File& sessionFolder);
void markStopped();

struct UncleanExit
{
    bool found = false;
    juce::File session;  // what it had open, if anything
    juce::File crashLog; // newest crash report written since it started, if any
};

// Finds (and clears) markers left by runs that died. Call before markRunning.
UncleanExit findUncleanExit();

// A WAV whose writer never finished has stale sizes in its header. Rewrites them from the file
// size. Returns false if the file isn't a WAV it understands.
bool repairWavHeader (const juce::File&);
} // namespace taw::safety
