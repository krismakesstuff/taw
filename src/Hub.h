#pragma once

#include <juce_core/juce_core.h>

// The audio hub (docs/plan-audio-hub.md): one background taw that owns the audio interface and
// routes between it, taw sessions and other apps. On macOS it runs from "taw hub.app", started
// at login by a launchd agent, so it has its own microphone permission.
namespace taw::hub
{
// taw --hub: runs the hub until it's asked to stop. Returns the exit code.
int run (const juce::StringArray& args);

// The launchd agent that starts the hub at login and keeps it running (macOS). install
// replaces one already there and starts it; uninstall stops and removes it. Both return what
// happened, for the terminal.
juce::String install();
juce::String uninstall();

// The driver for the apps' devices (taw.driver, built beside taw), into or out of
// /Library/Audio/Plug-Ins/HAL: asks for an admin password, restarts the Mac's audio (every
// app's sound stops for a moment), and restarts the hub so it sees them.
juce::String installDriver();
juce::String uninstallDriver();

// Runs or stops it now; it still starts at login (start installs it if it isn't).
juce::String start();
juce::String stop();

// Where the hub writes what it's doing.
juce::File logFile();
} // namespace taw::hub
