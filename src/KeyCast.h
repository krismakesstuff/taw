#pragma once

#include "Console.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <deque>

namespace taw
{
// Keycast: a window that shows the keys you press, for screen recordings of taw. Only taw's own
// keys (what the terminal hands it), so nothing typed in other apps shows up and no keyboard
// hook or permission is needed. The window never takes the keyboard; size it by dragging and
// the text grows with it.

// What the window shows: the recent presses, oldest first. A key held or pressed again in a
// row counts up ("→ ×6") and text typed on the command line reads as a word ("2v-6").
class KeyCastLog
{
public:
    struct Entry
    {
        juce::String text; // the key's name, or the word typed
        int count = 1;     // the same key again in a row
        bool word = false; // typed text: more typing adds to it
        double at = 0.0;   // ms, the last press it took

        juce::String shown() const { return count > 1 ? text + juce::String (L" \x00d7") + juce::String (count) : text; }
    };

    // typed: the key went onto the command line (taw had a line being typed after it).
    void press (const Key&, bool typed, double nowMs);
    // Drops what has faded away: entries untouched for fadeMs and then fadeOutMs more.
    // Returns whether anything is left (and so still needs drawing).
    bool expire (double nowMs, double fadeMs);
    void clear() { entries.clear(); }

    const std::deque<Entry>& recent() const { return entries; }

    // A key's name as the window shows it: "Ctrl+Space", "Shift+R", "←", "Enter".
    static juce::String name (const Key&);

    static constexpr int maxEntries = 6;
    static constexpr double fadeOutMs = 400.0; // after fade: how long it takes to disappear
    static constexpr double againMs = 1000.0;  // the same key within this counts up

private:
    std::deque<Entry> entries;
};

class KeyCast
{
public:
    struct Style
    {
        juce::String font;                         // empty: the default monospaced font
        juce::Colour text = juce::Colours::white;
        juce::Colour background = juce::Colours::black;
        double size = 1.0;                         // the text's scale, on top of the window's size
        double fade = 2.0;                         // seconds before a key starts to fade (0: never)
    };

    KeyCast();
    ~KeyCast();

    // The window is made the first time it's shown, so headless runs never make one.
    void setVisible (bool);
    bool isVisible() const;
    bool wasShown() const { return impl != nullptr; } // the window was made this run
    bool takeCloseRequest();  // its close button was pressed (it's hidden) since last asked
    bool hasKeyboard() const; // it was activated anyway (Windows can): give the keyboard back

    void press (const Key&, bool typed); // shown only while the window is
    void tick();                         // once a frame: fades

    const Style& style() const { return current; }
    void setStyle (const Style&);

    // Where the window is (JUCE's window state string), kept in settings.json.
    juce::String windowState() const;
    void restoreWindowState (const juce::String&);

    // "#ff8800", "ff8800", "#80ff8800" (with alpha) or a name ("yellow"); false if it's neither.
    static bool parseColour (const juce::String&, juce::Colour& out);
    static juce::String colourText (juce::Colour); // "#rrggbb" ("#aarrggbb" when not opaque)

    juce::Image snapshot(); // the window's picture as it is now, for tests (none: not shown yet)

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    Style current;
    juce::String savedState; // until the window is made
};
} // namespace taw
