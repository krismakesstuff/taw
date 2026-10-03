#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace taw
{
class App;

// The editor's colours, from the theme (set with the plugin windows' title colours).
struct EditorColours
{
    juce::Colour bg, panel, text, dim, faint, bar, whiteKey, blackKey, blackRow, note, noteSelected, accent, play, rec, outside;
    juce::Colour wave, clip, selection; // the audio editor's waveforms, clips and the time selection
};
void setEditorColours (const EditorColours&);
const EditorColours& editorColours();

// The MIDI editor: one MIDI clip in a window over the terminal, for the mouse. Double-click a
// clip (or shift+E, or "edit") to open it. Notes move, resize, draw (double-click) and delete;
// a lane below sets velocity or draws a controller; a toolbar picks the grid and quantizes.
// Clicking a note, or the keyboard down the side, plays it on the track's instrument.
//
// It edits the session tree directly, one undo step per gesture, so taw's undo (ctrl+z, passed
// on from here) and everything on screen follow. The playhead and cursor are taw's: the ruler
// moves the cursor. While it's in front it has the keyboard; keys it doesn't use go to taw, and
// esc hands the keyboard back.
class MidiEditor
{
public:
    explicit MidiEditor (App&);
    ~MidiEditor();

    void open (const juce::ValueTree& clip); // a MIDICLIP on an instrument track: shows it (switching from another)
    void close();
    bool isOpen() const;
    juce::ValueTree clip() const;
    void* nativeHandle() const; // its window, which keeps the keyboard while it's in front

    // Once a frame: its close button, and the clip going away (undo) close it.
    void poll();

private:
    struct Window;
    struct Roll;

    App& app;
    std::unique_ptr<Window> window;
};
} // namespace taw
