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
constexpr int leftPanel = 25;  // focus, number, name, flags, input
constexpr int meterPanel = 25; // volume, meter bar, peak readout

// Console width minus the one-column gutter, the panels and the two lane borders.
inline int laneWidth (int consoleWidth) { return std::max (10, consoleWidth - 1 - leftPanel - 2 - meterPanel); }
} // namespace layout

struct Cell
{
    wchar_t ch = L' ';
    int fg = -1, bg = -1; // 256-colour palette index, -1 = terminal default
    bool bold = false;

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

private:
    void drawMain (App&);
    void drawTitle (App&);
    void drawInterface (App&);
    void drawTransport (App&);
    void drawTimeline (App&);
    void drawBottom (App&);
    void drawHelp (App&);
    void drawDevicePicker (App&);
    void drawTemplatePicker (App&);
    void drawMeter (int x, int y, int width, float level, int bg = -1);
    void drawDb (int x, int y, float hold, bool clipped, int bg = -1);
    void drawVolume (int x, int y, double db, int bg = -1);

    Canvas canvas;
    std::vector<std::wstring> previous;
};
} // namespace taw
