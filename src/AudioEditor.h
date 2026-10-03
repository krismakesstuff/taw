#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace taw
{
class App;

// The audio editor: an audio track's clips in a window over the terminal, zoomable down to single
// samples, for the mouse. Shift+E (or "edit", or a double-click on a clip) opens it on the clip at
// the cursor or the nearest one.
//
// The time selection, the cursor and the playhead are taw's: drag across the waveform to select
// (to the sample, or the grid, or the nearest zero crossing), then taw's keys do the rest (delete,
// ctrl+x, ctrl+c / v, ctrl+d, l loops it, b bounces it), passed on from here. A clip's edges drag
// to trim it (nothing is lost: it's a window into its file) and its name bar drags to move it.
// One undo step per gesture, in taw's undo.
class AudioEditor
{
public:
    explicit AudioEditor (App&);
    ~AudioEditor();

    void open (const juce::ValueTree& clip); // a CLIP on an audio track
    void close();
    bool isOpen() const;
    juce::ValueTree track() const;
    void* nativeHandle() const;

    void poll(); // once a frame: its close button, the track going away

private:
    struct Wave;
    struct Window;

    App& app;
    std::unique_ptr<Window> window;
};
} // namespace taw
