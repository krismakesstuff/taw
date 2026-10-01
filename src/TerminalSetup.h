#pragma once

#include <juce_core/juce_core.h>

// macOS Terminal.app keeps some keys taw uses (shift+up/down, home/end, ctrl+option+arrows)
// and types symbols with Option instead of sending Alt. taw offers, once, to add a "taw"
// Terminal profile that fixes both; the user's own profiles are never touched.
namespace taw::terminalSetup
{
// Interactive run in Terminal.app that hasn't been asked yet: ask (plain y/n, before the screen
// is taken over), install on yes, and remember the answer either way.
void offerOnce();

// Writes the profile to the config folder and opens it: Terminal imports it and opens a
// window that uses it. Used by the prompt and by taw --terminal-profile.
juce::Result install();
} // namespace taw::terminalSetup
