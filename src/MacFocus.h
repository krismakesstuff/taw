#pragma once

#include <string>
#include <vector>

// macOS: the app taw's terminal runs in (Terminal, iTerm2, Ghostty...), brought to the front so
// keys go back to the terminal, as Console::focusTerminal does on Windows.
namespace taw::macFocus
{
void activateTerminal();

// The process id of the app in front (that has the keyboard), for tests.
int frontmostApp();

// A plugin window (its JUCE native handle, an NSView) gets the keyboard: taw comes to the front
// with that window key, so typing reaches the plugin (a login, a preset name).
void holdKeyboard (void* view);

// What has the keyboard, with taw in front, for handing it back to the terminal after a click
// in a plugin window (views: their JUCE native handles; keeper: one of taw's windows that keeps
// the keyboard while it's in front, like the MIDI editor).
enum class Front
{
    notTaw,  // another app (the terminal, or one you went to): nothing to do
    keeper,  // the keeper window: it keeps the keyboard
    other,   // a window of a plugin's own (a login, an alert, a file dialog): it keeps it
    menu,    // a menu is open
    button,  // a mouse button is down (dragging a knob)
    plugin   // a plugin window, the mouse done with it: the keyboard can go back
};

Front front (const std::vector<void*>& views, void* keeper);

// Who's in front: taw itself, or the terminal it runs in.
bool tawInFront();
bool terminalInFront();

// With TAW_FOCUS_LOG set: a line in $TMPDIR/taw-focus.log (repeats skipped), as on Windows.
void log (const std::string& line);
} // namespace taw::macFocus
