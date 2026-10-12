#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace taw
{
class App;

// The sample browser's window (ctrl+B): over the terminal like the editors, mouse-forward like a
// plugin's, and as usable from the keyboard. Kind tabs across the top; the map fills the middle
// (drag over it to audition what's under the pointer, or hover, with that on); the list beside
// it; under the list, the 4x4 pads of the kit you're on (pick one, and what you audition goes
// onto it; drag a sample onto any pad). Typing filters. While it's in front it keeps the
// keyboard (App::handleBrowserKey has the keys; the rest go to taw); esc hands it back.
class BrowserWindow
{
public:
    explicit BrowserWindow (App&);
    ~BrowserWindow();

    void open (bool withKeys);
    void close();
    bool isOpen() const;
    bool isActive() const; // in front, with the keyboard
    void* nativeHandle() const;

    void poll(); // once a frame: its close button

    // The window's contents drawn into a picture, without a window (taw --exec ":browser f.png").
    static juce::Image snapshot (App&, int width, int height);

private:
    struct Panel;
    struct Window;

    App& app;
    std::unique_ptr<Window> window;
};

// A sample kind's colour, as the terminal draws it (Ui.cpp), for the window.
juce::Colour sampleKindColour (const juce::String& kind);
} // namespace taw
