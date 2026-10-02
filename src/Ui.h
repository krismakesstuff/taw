#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <string>
#include <vector>

namespace taw
{
class App;

namespace layout
{
// Left to right, as in Pro Tools: the track panel, the routing column, the meter, then the
// timeline.
constexpr int leftPanel = 25;  // focus, number, name, flags, input
constexpr int ioPanel = 27;    // inserts A-E on a track's first row; sends A-E and the output on its second
constexpr int meterPanel = 18; // volume, meter bar, peak readout

inline int ioWidth (bool io) { return io ? ioPanel + 1 : 0; } // with its border
constexpr int ioSlots = 1, ioSlotW = 4, ioOut = 21;            // offsets in the panel, slot cell width

inline int laneStart (bool io) { return leftPanel + 1 + ioWidth (io) + meterPanel + 1; }

// Console width minus the one-column gutter, everything left of the lane and its right border.
inline int laneWidth (int consoleWidth, bool io) { return std::max (10, consoleWidth - 1 - laneStart (io) - 1); }
} // namespace layout

struct Cell
{
    wchar_t ch = L' ';
    int fg = -1, bg = -1; // 256-colour palette index, -1 = terminal default
    bool bold = false;
    bool over = false, under = false; // a line along the top / bottom (clip outlines), in fg

    bool operator== (const Cell&) const = default;
};

// A grid of cells rendered to VT sequences. Only rows that changed are rewritten.
class Canvas
{
public:
    void resize (int w, int h);
    int width() const { return w; }
    int height() const { return h; }

    void put (int x, int y, wchar_t ch, int fg = -1, int bg = -1, bool bold = false);
    int text (int x, int y, const juce::String&, int fg = -1, int bg = -1, bool bold = false, int maxWidth = -1);
    void fill (int x, int y, int width, wchar_t ch, int fg = -1, int bg = -1);
    Cell& at (int x, int y) { return cells[(size_t) (y * w + x)]; }

    std::wstring row (int y) const;

private:
    int w = 0, h = 0;
    std::vector<Cell> cells;
};

class Ui
{
public:
    void render (App&);
    juce::String snapshot (App&, int w, int h);

private:
    void drawMain (App&);
    void drawTitle (App&);
    void drawInterface (App&);
    void drawTransport (App&);
    void drawTimeline (App&);
    void drawBottom (App&);
    void drawHelp (App&);
    void drawSettings (App&);
    void drawPluginPicker (App&);
    void drawBusPicker (App&);
    void drawMidiPanel (App&);
    void drawDevicePicker (App&);
    void drawTemplatePicker (App&);
    void drawMeter (int x, int y, int width, float level, int bg = -1);
    void drawDb (int x, int y, float hold, bool clipped, int bg = -1);
    void drawVolume (int x, int y, double db, int bg = -1);

    Canvas canvas;
    std::vector<std::wstring> previous;
    juce::String themeDrawn; // the theme the screen was last painted in
    int midiSeen = 0;        // MIDI events counted at the last frame
    double midiLitAt = 0.0;  // when the last ones arrived (the activity light)
    std::vector<int> midiRowSeen;        // MIDI panel: each input's count at the last frame
    std::vector<double> midiRowLitAt;    // ...and when it last changed
};
} // namespace taw
