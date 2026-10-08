#include "Ui.h"
#include "App.h"

namespace taw
{
// 256-colour palette indexes, set per frame from the theme ("light" / "dark").
namespace colour
{
int screenFg, screenBg; // what -1 (the terminal's default) means: dark keeps the terminal's own
int text, dim, faint, headerBg, focusBg;
int accent, rec, recBg, play, playBg;
int clipAltBg, hit; // hit: sample-track steps (warm, unlike waveforms)
int selBg, selClipBg, loopBg, loopOffBg;
int cursorBg, playheadBg; // behind what's under the cursor / playhead in a lane
int busColour, chipText, busText; // buses' own colour (dark green); text on a type label, and on the bus one
int gridBar; // bar lines; beats and finer use faint
int laneFocusBg, laneSelBg; // empty lane cells on the focused / selected tracks (apart from clips)
int clipBg, wave, liveWave, marker;
int videoBg, videoText; // the video row's bar
int note, blackKeyBg; // instrument notes; piano-roll rows of the black keys
int ok, error, meterGreen, meterYellow, meterRed, meterBg;
bool lightScreen = false; // light and grey: darker kind colours

// A sample kind's colour, the same on the browser's badges, map and list.
int kind (const juce::String& k)
{
    static const std::pair<const char*, std::pair<int, int>> table[] = {
        { "kick", { 203, 160 } }, { "snare", { 214, 166 } }, { "clap", { 150, 64 } }, { "rim", { 74, 31 } },
        { "hihat", { 111, 19 } }, { "open hat", { 141, 91 } }, { "cymbal", { 183, 133 } }, { "tom", { 137, 94 } },
        { "perc", { 43, 30 } },
    };

    for (const auto& [name, c] : table)
        if (k == name)
            return lightScreen ? c.second : c.first;

    return lightScreen ? 242 : 250;
}

// A palette index as the colour terminals show it (the xterm 256-colour table).
juce::Colour xtermColour (int index)
{
    static const juce::uint32 basic[16] = { 0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xc0c0c0,
                                            0x808080, 0xff0000, 0x00ff00, 0xffff00, 0x0000ff, 0xff00ff, 0x00ffff, 0xffffff };

    if (index < 0)
        return juce::Colour (0xff1c1c1c);

    if (index < 16)
        return juce::Colour (0xff000000 | basic[index]);

    if (index >= 232)
    {
        const auto v = (juce::uint8) (8 + (index - 232) * 10);
        return juce::Colour (v, v, v);
    }

    const int i = index - 16;
    auto level = [] (int n) { return (juce::uint8) (n == 0 ? 0 : 55 + n * 40); };
    return juce::Colour (level (i / 36), level ((i / 6) % 6), level (i % 6));
}

void apply (const juce::String& theme)
{
    lightScreen = theme == "grey" || theme == "light";

    if (theme == "grey")
    {
        // Mid-light grey: a softer screen than light, with darker clips and text to match.
        screenFg = 234, screenBg = 249;
        text = 233, dim = 238, faint = 244, headerBg = 247, focusBg = 246;
        accent = 166, rec = 160, recBg = 217, play = 28, playBg = 28;
        clipAltBg = 243, hit = 94;
        selBg = 110, selClipBg = 74, loopBg = 108, loopOffBg = 245;
        cursorBg = 180, playheadBg = 114;
        busColour = 22, chipText = 231, busText = 231;
        gridBar = 241;
        laneFocusBg = 248, laneSelBg = 146;
        clipBg = 245, wave = 18, liveWave = 124, marker = 90;
        videoBg = 103, videoText = 231;
        note = 23, blackKeyBg = 247;
        ok = 22, error = 124, meterGreen = 28, meterYellow = 136, meterRed = 160, meterBg = 246;
    }
    else if (theme == "light")
    {
        screenFg = 236, screenBg = 255;
        text = 235, dim = 242, faint = 248, headerBg = 253, focusBg = 252;
        accent = 166, rec = 160, recBg = 224, play = 28, playBg = 28;
        clipAltBg = 249, hit = 130;
        selBg = 153, selClipBg = 117, loopBg = 151, loopOffBg = 250;
        cursorBg = 223, playheadBg = 157;
        busColour = 22, chipText = 231, busText = 231;
        gridBar = 245;
        laneFocusBg = 254, laneSelBg = 189;
        clipBg = 251, wave = 25, liveWave = 160, marker = 133;
        videoBg = 146, videoText = 235;
        note = 30, blackKeyBg = 253;
        ok = 28, error = 160, meterGreen = 34, meterYellow = 172, meterRed = 160, meterBg = 252;
    }
    else
    {
        screenFg = -1, screenBg = -1;
        text = 252, dim = 243, faint = 238, headerBg = 235, focusBg = 237;
        accent = 214, rec = 196, recBg = 52, play = 35, playBg = 22;
        clipAltBg = 239, hit = 179;
        selBg = 24, selClipBg = 31, loopBg = 28, loopOffBg = 239;
        cursorBg = 94, playheadBg = 22;
        busColour = 28, chipText = 16, busText = 231;
        gridBar = 241;
        laneFocusBg = 234, laneSelBg = 17;
        clipBg = 236, wave = 110, liveWave = 210, marker = 170;
        videoBg = 60, videoText = 189;
        note = 79, blackKeyBg = 233;
        ok = 114, error = 203, meterGreen = 35, meterYellow = 178, meterRed = 196, meterBg = 234;
    }
}
} // namespace colour

//==============================================================================
void Canvas::resize (int newW, int newH)
{
    w = std::max (0, newW);
    h = std::max (0, newH);
    cells.assign ((size_t) (w * h), Cell {});
}

void Canvas::put (int x, int y, wchar_t ch, int fg, int bg, bool bold)
{
    if (x >= 0 && y >= 0 && x < w && y < h)
        at (x, y) = { ch, fg, bg, bold };
}

int Canvas::text (int x, int y, const juce::String& s, int fg, int bg, bool bold, int maxWidth)
{
    auto p = s.getCharPointer();

    for (int i = 0; ! p.isEmpty() && (maxWidth < 0 || i < maxWidth); ++i, ++x)
        put (x, y, (wchar_t) p.getAndAdvance(), fg, bg, bold);

    return x;
}

void Canvas::fill (int x, int y, int width, wchar_t ch, int fg, int bg)
{
    for (int i = 0; i < width; ++i)
        put (x + i, y, ch, fg, bg);
}

std::wstring Canvas::row (int y) const
{
    std::wstring out = L"\x1b[0m";
    Cell style;
    style.fg = style.bg = -2; // so the first cell always sets its colours

    for (int x = 0; x < w; ++x)
    {
        const auto& c = cells[(size_t) (y * w + x)];

        if (c.fg != style.fg || c.bg != style.bg || c.bold != style.bold || c.over != style.over || c.under != style.under)
        {
            out += L"\x1b[0";
            if (c.bold) out += L";1";
            if (c.under) out += L";4";
            if (c.over) out += L";53"; // terminals without overline skip it
            // -1 is the theme's screen colour: the terminal's own in dark, set explicitly in light.
            const auto fg = c.fg >= 0 ? c.fg : colour::screenFg, bg = c.bg >= 0 ? c.bg : colour::screenBg;
            if (fg >= 0) out += L";38;5;" + std::to_wstring (fg);
            if (bg >= 0) out += L";48;5;" + std::to_wstring (bg);
            out += L"m";
            style = c;
        }

        out += c.ch;
    }

    // In light, paint the rest of the line (the column kept free) the screen colour too.
    if (colour::screenBg >= 0)
        out += L"\x1b[0;48;5;" + std::to_wstring (colour::screenBg) + L"m\x1b[K";

    return out + L"\x1b[0m";
}

//==============================================================================
static juce::String padRight (const juce::String& s, int width)
{
    return s.length() >= width ? s.substring (0, width) : s + juce::String::repeatedString (" ", width - s.length());
}

static float toDb (float gain) { return juce::Decibels::gainToDecibels (gain, -100.0f); }

// The frame as plain text, w by h, without touching the console (taw --exec ":screen").
juce::String Ui::snapshot (App& app, int w, int h)
{
    colour::apply (app.theme);

    // Plugin windows' title bars in the same colours.
    if (app.theme != themeDrawn)
    {
        using colour::xtermColour;
        PluginSlot::setWindowColours ({ xtermColour (colour::headerBg), xtermColour (colour::text), xtermColour (colour::dim),
                                        xtermColour (colour::accent) });

        // The MIDI editor too: the lane's colours, the notes', the playhead's.
        const auto bg = xtermColour (colour::screenBg >= 0 ? colour::screenBg : -1);
        const bool lightTheme = bg.getPerceivedBrightness() > 0.5f;
        setEditorColours ({ bg, xtermColour (colour::headerBg), xtermColour (colour::text), xtermColour (colour::dim), xtermColour (colour::faint),
                            xtermColour (colour::gridBar), lightTheme ? juce::Colours::white : xtermColour (252),
                            lightTheme ? xtermColour (240) : xtermColour (236), xtermColour (colour::blackKeyBg), xtermColour (colour::note),
                            xtermColour (colour::accent).brighter (0.2f), xtermColour (colour::accent), xtermColour (colour::play), xtermColour (colour::rec),
                            lightTheme ? bg.darker (0.08f) : bg.darker (0.5f),
                            xtermColour (colour::wave), xtermColour (colour::clipBg), xtermColour (colour::selBg) });
    }
    canvas.resize (w, h);
    drawMain (app);

    juce::String out;

    for (int y = 0; y < h; ++y)
    {
        juce::String line;

        for (int x = 0; x < w; ++x)
            line << juce::String::charToString ((juce::juce_wchar) canvas.at (x, y).ch);

        out << line.trimEnd() << "\n";
    }

    previous.clear(); // the next real render repaints everything
    return out;
}

void Ui::render (App& app)
{
    const auto w = app.console.width() - 1; // keep the last column free so nothing wraps
    const auto h = app.console.height();

    colour::apply (app.theme);

    if (w != canvas.width() || h != canvas.height() || app.theme != themeDrawn)
    {
        themeDrawn = app.theme; // a new theme repaints everything
        previous.clear();
        app.console.write (L"\x1b[0m\x1b[2J");
    }

    canvas.resize (w, h);
    laneMap.valid = false; // until the timeline is drawn (a picker or a box over it: clicks pass it by)

    if (w < 70 || h < 16)
        canvas.text (1, 0, L"taw needs at least 70x16 \x2014 make the window bigger", colour::error);
    else if (app.mode == Mode::DevicePicker)
        drawDevicePicker (app);
    else if (app.mode == Mode::TemplatePicker)
    {
        drawTemplatePicker (app);

        if (app.opening != nullptr)
            drawOpening (app);
    }
    else
    {
        drawMain (app);

        if (app.closing != nullptr)
            drawClosing (app);
    }

    if (app.loadingPlugin.isNotEmpty() && w >= 70 && h >= 16)
        drawPluginLoading (app);

    previous.resize ((size_t) h);
    std::wstring frame;

    for (int y = 0; y < h; ++y)
    {
        auto r = canvas.row (y);

        if (r != previous[(size_t) y])
        {
            frame += L"\x1b[" + std::to_wstring (y + 1) + L";1H" + r;
            previous[(size_t) y] = std::move (r);
        }
    }

    if (! frame.empty())
        app.console.write (frame);
}

//==============================================================================
// Top of the screen:
//   0  taw ▸ project
//   1  interface · input meters
//   2  (blank)
//   3  transport bar, centred
//   4  markers
//   5  ruler, then tracks
void Ui::drawMain (App& app)
{
    // The docked browser: about a third of the screen, under the mix row.
    dockCols = layout::browserDock (canvas.width() + 1, app.browserVisible); // the screen being drawn (a snapshot has no console)
    app.browserGeometry.valid = false;

    drawTitle (app);
    drawInterface (app);
    drawTransport (app);

    if (app.session != nullptr)
        drawTimeline (app);

    if (dockCols > 0)
        drawBrowser (app);

    drawBottom (app);

    // A box over the timeline takes the clicks out of it.
    if (app.helpVisible || app.pluginPickerVisible || app.busPickerVisible || app.trackMenuVisible || app.autoPanelVisible || app.midiPanelVisible
        || app.settingsVisible)
        laneMap.valid = false;

    if (app.helpVisible)
        drawHelp (app);
    else if (app.pluginPickerVisible && app.session != nullptr)
        drawPluginPicker (app);
    else if (app.busPickerVisible && app.session != nullptr)
        drawBusPicker (app);
    else if (app.trackMenuVisible && app.session != nullptr)
        drawTrackMenu (app);
    else if (app.autoPanelVisible && app.session != nullptr)
        drawAutoPanel (app);
    else if (app.midiPanelVisible && app.midiInputs != nullptr)
        drawMidiPanel (app);
    else if (app.settingsVisible && app.session != nullptr)
        drawSettings (app);
}

void Ui::drawTitle (App& app)
{
    int x = canvas.text (1, 0, "taw", colour::accent, -1, true);

    if (auto* s = app.session.get())
        canvas.text (x, 0, L" \x25b8 " + s->name(), colour::text, -1, true);

    // MIDI inputs on the right, with a light that blinks as messages come in.
    if (app.midiInputs != nullptr)
    {
        const auto names = app.midiInputs->names();
        const auto now = juce::Time::getMillisecondCounterHiRes();

        if (const auto seen = app.midiInputs->eventsReceived(); seen != midiSeen)
        {
            midiSeen = seen;
            midiLitAt = now;
        }

        const auto label = names.isEmpty() ? juce::String ("no midi in")
                           : names.size() == 1 ? "midi " + names[0]
                                               : "midi " + juce::String (names.size()) + " in (midi: choose)";
        const int lx = canvas.width() - 3 - std::min (label.length(), 40);
        canvas.text (lx, 0, label, names.isEmpty() ? colour::faint : colour::dim, -1, false, 40);

        if (! names.isEmpty())
            canvas.put (canvas.width() - 2, 0, L'\x25cf', now - midiLitAt < 150.0 ? colour::note : colour::faint);
    }
}

void Ui::drawInterface (App& app)
{
    const int w = canvas.width(), y = 1;
    juce::String dev;
    int devColour = colour::dim;

    if (auto* d = app.engine.devices().getCurrentAudioDevice())
    {
        const auto latencyMs = (d->getInputLatencyInSamples() + d->getOutputLatencyInSamples()) * 1000.0 / d->getCurrentSampleRate();
        const auto xruns = d->getXRunCount();
        dev = d->getName() + L" \x00b7 " + d->getTypeName() + L" \x00b7 " + juce::String (d->getCurrentSampleRate() / 1000.0, 1) + "k/"
              + juce::String (d->getCurrentBufferSizeSamples()) + L" \x00b7 " + juce::String (latencyMs, 1) + "ms"
              + (xruns >= 0 ? L" \x00b7 xr " + juce::String (xruns) : juce::String());

        if (xruns > 0)
            devColour = colour::meterYellow;
    }
    else
    {
        dev = L"no audio device \x00b7 type dev";
        devColour = colour::error;
    }

    int x = canvas.text (1, y, dev, devColour, -1, false, w / 2);

    // Input meters share the rest of the line: each gets "N▕meter" with the meter as wide as
    // the space allows (3 to 6 cells), so all of a 16-input interface fits on a normal window.
    const auto n = app.engine.numInputChannels();
    x = canvas.text (x + 3, y, "in", colour::dim);

    if (n == 0)
    {
        canvas.text (x + 1, y, "no inputs", colour::faint);
        return;
    }

    const auto labelWidth = n >= 10 ? 2 : 1;
    const auto perInput = (w - 1 - x) / n;                  // label + space + meter + gap
    const auto meterWidth = juce::jlimit (3, 6, perInput - labelWidth - 2);

    for (int ch = 0; ch < n; ++ch)
    {
        const auto label = juce::String (ch + 1);

        if (x + 1 + labelWidth + 1 + meterWidth >= w)
        {
            canvas.text (x + 1, y, "+" + juce::String (n - ch), colour::dim);
            break;
        }

        const auto& m = app.inputMeters[(size_t) ch];
        canvas.text (x + 1 + labelWidth - label.length(), y, label, m.clipped ? colour::meterRed : colour::dim, -1, m.clipped);
        drawMeter (x + 2 + labelWidth, y, meterWidth, m.level, colour::meterBg);
        x += 2 + labelWidth + meterWidth;
    }
}

void Ui::drawTransport (App& app)
{
    auto* s = app.session.get();

    if (s == nullptr)
        return;

    const int w = canvas.width() - dockCols, y = 3, bg = colour::headerBg; // centred over the timeline
    const auto rolling = app.engine.isRolling();
    const auto pos = rolling ? app.heardPlayhead() : app.cursor;

    // Built as a list of coloured pieces, then centred as a whole.
    struct Piece { juce::String text; int fg, bg; bool bold; };
    std::vector<Piece> pieces;
    auto add = [&] (juce::String t, int fg, int pieceBg = colour::headerBg, bool bold = false) { pieces.push_back ({ std::move (t), fg, pieceBg, bold }); };
    auto gap = [&] { add ("   ", -1); };

    // Counting in: the clicks still to come, then it rolls.
    const auto countIn = app.engine.countInRemaining();
    const auto clicksLeft = countIn > 0 ? (int) std::ceil ((double) countIn / s->samplesPerBeat() - 1e-6) : 0;

    if (clicksLeft > 0)
        add (L" \x25cf COUNT " + juce::String (clicksLeft) + " ", 231, app.recording ? colour::rec : colour::playBg, true);
    else if (app.recording)
        add (L" \x25cf REC ", 231, colour::rec, true);
    else if (rolling)
        add (L" \x25ba PLAY ", 231, colour::playBg, true);
    else
        add (L" \x25a0 STOP ", colour::dim);

    gap();
    add (s->gridOn() ? app.formatBars (pos) : juce::String ("grid off"), s->gridOn() ? colour::text : colour::dim, bg, true);
    gap();
    add (app.formatTime (pos), colour::text, bg, true);
    gap();
    add (juce::String (s->bpm(), 1) + " bpm " + juce::String (s->beatsPerBar()) + "/" + s->tree()[ids::timeSigDen].toString(), colour::dim);
    gap();

    if ((bool) s->tree()[ids::click])
        add (s->tree()[ids::clickWhen].toString() == "rec" ? "click rec" : "click", colour::accent, bg, true);
    else
        add ("no click", colour::faint);

    if ((int) s->tree()[ids::countIn] > 0)
    {
        gap();
        add ("count " + s->tree()[ids::countIn].toString(), colour::dim);
    }

    // Automation write armed: mapped controllers moved while playing are recorded.
    if (app.writeOn)
    {
        gap();
        add (L" \x25cf WRITE ", 231, rolling ? colour::rec : colour::recBg, true);
    }

    // Plugins still loading after the session opened.
    if (! app.pluginQueue.empty())
    {
        gap();
        add ("loading " + juce::String ((int) app.pluginQueue.size()) + (app.pluginQueue.size() == 1 ? " plugin" : " plugins"), colour::accent);
    }

    // A plugin keeping the keyboard (its title double-clicked): keys aren't reaching taw.
    if (app.console.keyboardHeldBy() != nullptr)
    {
        gap();
        add (L" keys \x2192 plugin ", 16, colour::accent, true);
    }

    // Low latency mode changes what you hear, so it's shown while it's on.
    if (app.lowLatency)
    {
        gap();
        add (" LOW LAT ", 16, colour::meterYellow, true);
    }

    if ((bool) s->tree()[ids::loop])
    {
        gap();
        add ("loop", colour::play, bg, true);
    }

    int total = 0;

    for (const auto& p : pieces)
        total += p.text.length();

    // Pad one space either side so the bar reads as a block, then centre it.
    int x = std::max (0, (w - total - 2) / 2);
    canvas.put (x++, y, L' ', -1, bg);

    for (const auto& p : pieces)
        x = canvas.text (x, y, p.text, p.fg, p.bg, p.bold);

    canvas.put (x, y, L' ', -1, bg);
}

void Ui::drawMeter (int x, int y, int width, float level, int bg)
{
    // Half-cell resolution: Consolas (cmd's default font) has the half and full blocks but
    // not the finer eighth blocks.
    static const wchar_t halves[] = L" \x258c\x2588";
    constexpr float floorDb = -60.0f;

    const auto fraction = juce::jlimit (0.0f, 1.0f, (toDb (level) - floorDb) / -floorDb);
    const auto filled = fraction * (float) width * 2.0f;

    for (int i = 0; i < width; ++i)
    {
        const auto cellDb = floorDb + (float) (i + 1) / (float) width * -floorDb;
        const int fg = cellDb > -6.0f ? colour::meterRed : (cellDb > -18.0f ? colour::meterYellow : colour::meterGreen);
        const auto halvesHere = juce::jlimit (0, 2, (int) (filled - (float) i * 2.0f));
        canvas.put (x + i, y, halves[halvesHere], fg, bg);
    }
}

void Ui::drawDb (int x, int y, float hold, bool clipped, int bg)
{
    if (clipped)
    {
        canvas.text (x, y, " CLIP", 231, colour::meterRed, true);
        return;
    }

    const auto db = toDb (hold);
    const auto s = db <= -99.0f ? juce::String (" -inf") : juce::String (db, 1).paddedLeft (' ', 5);
    canvas.text (x, y, s, db > -6.0f ? colour::meterYellow : colour::dim, bg);
}

void Ui::drawTimeline (App& app)
{
    using namespace layout;
    auto& s = *app.session;
    const int w = canvas.width() - dockCols, h = canvas.height(); // the docked browser is on the right
    const bool io = app.ioVisible;
    const bool twoRows = s.trackHeight() > 1; // the flags go on a track's second row
    const int leftW = leftPanel (twoRows);
    const int ioX = leftW + 1, laneX = laneStart (io, twoRows), laneW = laneWidth (w + 1, io, twoRows), meterX = laneX + laneW + 1;

    // The borders between the panels and the timeline, and after it.
    auto borders = [&] (int y)
    {
        canvas.put (ioX - 1, y, L'\x2502', colour::faint);
        canvas.put (laneX - 1, y, L'\x2502', colour::faint);
        canvas.put (meterX - 1, y, L'\x2502', colour::faint);
    };
    const auto spc = app.samplesPerColumn();
    const auto rolling = app.engine.isRolling();
    const auto playhead = app.heardPlayhead(); // where you hear it (delay compensation makes the mix late)

    // Where column 0 starts. Grid lines can fall between samples (99 bpm: a beat is 26727.27
    // samples), so positions snapped to them are rounded either way; reading every sample from
    // its middle (half a sample in) puts a rounded position in the same column as its line.
    const double origin = (double) app.viewStart - 0.5;

    auto columnOf = [&] (juce::int64 pos) { return (int) std::floor (((double) pos - origin) / spc); };
    const auto cursorCol = columnOf (app.cursor);
    const auto playCol = columnOf (playhead);

    // Grid lines follow the zoom (App::gridSpacing, which the arrows step by too).
    const auto grid = app.gridSpacing();

    // A column covers [t0, t1), so a line at t0 belongs on the cell's left edge, where
    // selections and clips start. Fonts without a left-edge mark get centred ones.
    const bool edges = app.console.hasEdgeGlyphs();
    const wchar_t gridMark = edges ? L'\x258f' : L'\x00b7', cursorMark = edges ? L'\x258f' : L'\x2502';

    // 2: a bar line falls in [t0, t1), 1: a subdivision, 0: neither.
    auto gridLevel = [&] (double t0, double t1)
    {
        auto lineIn = [&] (double every) { return every > 0.0 && std::ceil (t0 / every - 1e-9) * every < t1; };
        return ! s.gridOn() ? 0 : lineIn (grid.bars) ? 2 : lineIn (grid.sub) ? 1 : 0;
    };

    // Time selection, as columns.
    const bool timeSel = app.hasTimeSelection();
    const auto selRange = app.timeSelection();
    const auto selTracks = app.selectedTracks();
    auto inTimeSelection = [&] (double t0, double t1) { return timeSel && t1 > (double) selRange.getStart() && t0 < (double) selRange.getEnd(); };

    // Markers and ruler rows -----------------------------------------------------------
    // Both can be focused (up from the first track); a selection made there covers every track.
    const int markerY = 4, rulerY = 5; // below the transport bar

    auto drawTopLane = [&] (int y, App::TopLane lane, const juce::String& label)
    {
        const bool focusedLane = app.topLane == lane;
        const int bg = focusedLane ? colour::focusBg : -1;
        canvas.fill (0, y, leftW, L' ', -1, bg);

        if (focusedLane)
            canvas.put (0, y, L'\x25b8', colour::accent, bg);

        canvas.text (1, y, label, focusedLane ? colour::dim : colour::faint, bg, false, leftW - 1);
        borders (y);

        if (focusedLane)
            for (int c = 0; c < laneW; ++c)
                canvas.put (laneX + c, y, L' ', -1,
                            inTimeSelection (origin + c * spc, origin + (c + 1) * spc) ? colour::selBg : colour::laneFocusBg);
    };

    const auto zoomText = s.gridOn() ? (app.zoomLevel == 0 ? juce::String ("1 beat") : juce::String (std::pow (2.0, app.zoomLevel), 2) + " beats")
                                     : juce::String (spc / app.sampleRate(), 2) + " s";
    drawTopLane (markerY, App::TopLane::markers, "markers");
    drawTopLane (rulerY, App::TopLane::ruler, "zoom " + zoomText + "/col");
    canvas.text (meterX, rulerY, "  vol", colour::faint); // pan under it
    canvas.text (meterX + 6, rulerY, "level", colour::faint);
    canvas.text (meterX + 12, rulerY, " peak", colour::faint);

    // Slot letters: inserts on a track's first row, sends on its second.
    if (io)
    {
        for (int k = 0; k < Session::numSends; ++k)
            canvas.text (ioX + ioSlots + k * ioSlotW, rulerY, Session::slotName (k), colour::faint);

        canvas.text (ioX + ioOut, rulerY, "out", colour::faint);
    }

    // Loop range on the ruler: green when on, grey when set but off.
    {
        const auto loopStart = (double) (juce::int64) s.tree()[ids::loopStart];
        const auto loopEnd = (double) (juce::int64) s.tree()[ids::loopEnd];
        const int loopBg = (bool) s.tree()[ids::loop] ? colour::loopBg : colour::loopOffBg;

        if (loopEnd > loopStart)
            for (int c = 0; c < laneW; ++c)
                if (const auto t0 = origin + c * spc; t0 + spc > loopStart && t0 < loopEnd)
                    canvas.put (laneX + c, rulerY, L' ', -1, loopBg);
    }

    // The selection shows on the ruler only while it's the focused lane (above): the tracks
    // show what a selection covers.
    if (timeSel && app.topLane == App::TopLane::ruler)
        for (int c = 0; c < laneW; ++c)
            if (inTimeSelection (origin + c * spc, origin + (c + 1) * spc))
                canvas.put (laneX + c, rulerY, L' ', -1, colour::selBg);

    int nextFree = 0;

    for (int c = 0; c < laneW; ++c)
    {
        const auto t0 = origin + c * spc, t1 = t0 + spc;
        juce::String label;
        int labelColour = colour::dim;

        if (const auto level = gridLevel (t0, t1); level == 2)
        {
            label = juce::String ((int) std::ceil (t0 / s.samplesPerBar() - 1e-9) + 1);
        }
        else if (level == 1 && c >= nextFree)
        {
            // Beats get numbers (2.3), finer lines the fraction of the beat as one character
            // (2.3  ¼  ½  ¾  2.4); a tick when even that's tight. Subdivisions stop at eighths of a
            // beat (App::gridSpacing), which all have glyphs.
            static const wchar_t* const eighths[] = { L"", L"\x215b", L"\x00bc", L"\x215c", L"\x00bd", L"\x215d", L"\x00be", L"\x215e" };
            const auto line = std::ceil (t0 / grid.sub - 1e-9) * grid.sub; // this line's position
            const auto beats = line / s.samplesPerBeat();
            const auto beat = (juce::int64) std::floor (beats + 1e-6);
            const auto eighth = (int) std::llround ((beats - (double) beat) * 8.0) % 8;
            const auto barBeat = juce::String (beat / s.beatsPerBar() + 1) + "." + juce::String (beat % s.beatsPerBar() + 1);
            const auto room = (int) (grid.sub / spc) - 1; // columns before the next line, less a gap

            label = eighth == 0 ? barBeat : juce::String (eighths[eighth]);

            if (eighth == 0 ? s.samplesPerBeat() < spc * 6.0 : label.length() > room)
            {
                label.clear();
                canvas.put (laneX + c, rulerY, gridMark, colour::faint, canvas.at (laneX + c, rulerY).bg);
            }

            labelColour = colour::faint;
        }
        else if (! s.gridOn())
        {
            const auto every = spc / app.sampleRate() < 0.5 ? 5.0 : 30.0; // seconds between labels
            const auto mark = std::ceil (t0 / (every * app.sampleRate()) - 1e-9);

            if (mark * every * app.sampleRate() < t1)
            {
                const auto secs = (int) (mark * every);
                label = juce::String (secs / 60) + ":" + juce::String (secs % 60).paddedLeft ('0', 2);
            }
        }

        if (label.isNotEmpty() && c >= nextFree)
        {
            for (int j = 0; j < label.length() && c + j < laneW; ++j)
                canvas.put (laneX + c + j, rulerY, (wchar_t) label[j], labelColour, canvas.at (laneX + c + j, rulerY).bg);
            nextFree = c + label.length() + 1;
        }
    }

    // Markers on their own row, left to right, each name cut short before the next marker.
    {
        struct Mark { int column; juce::String name; bool picked; };
        std::vector<Mark> marks;

        for (const auto& m : s.tree())
            if (m.hasType (ids::MARKER))
            {
                // The one being named shows what's typed so far.
                const bool naming = m == app.namingMarker && app.commandLine.isNotEmpty();
                const bool picked = m == app.namingMarker || (app.topLane == App::TopLane::markers && m == app.selectedMarker);
                marks.push_back ({ columnOf ((juce::int64) m[ids::position]), naming ? app.commandLine : m[ids::name].toString(), picked });
            }

        std::sort (marks.begin(), marks.end(), [] (const auto& a, const auto& b) { return a.column < b.column; });

        for (size_t i = 0; i < marks.size(); ++i)
        {
            const auto& [c, name, picked] = marks[i];

            if (c < 0 || c >= laneW)
                continue;

            // The picked marker (arrows on the markers row) shows in the cursor's colour.
            const int room = std::min (laneW - c, i + 1 < marks.size() ? std::max (1, marks[i + 1].column - c - 1) : laneW);
            canvas.text (laneX + c, markerY, name.isEmpty() ? juce::String ("?") : name, picked ? 16 : 231,
                         picked ? colour::accent : colour::marker, true, room);
        }
    }

    // Video row ---------------------------------------------------------------------
    // Under the ruler while there's a video: its length as a bar, with the video's own time
    // along it (frame ticks once a frame is wide enough), and the timecode at the cursor.
    const int videoY = app.videoLane() ? rulerY + 1 : -1;

    if (videoY >= 0)
    {
        const bool focusedLane = app.topLane == App::TopLane::video;
        const int bg = focusedLane ? colour::focusBg : -1;
        const auto start = (double) app.videoStartPos(), end = start + (double) app.videoLength();
        const auto spf = app.samplesPerFrame();
        const auto fps = app.sampleRate() / spf;

        canvas.fill (0, videoY, leftW, L' ', -1, bg);

        if (focusedLane)
            canvas.put (0, videoY, L'\x25b8', colour::accent, bg);

        canvas.text (1, videoY, "video", focusedLane ? colour::dim : colour::faint, bg);
        const auto tc = app.videoTimecode (rolling ? playhead : app.cursor);
        canvas.text (leftW - 1 - tc.length(), videoY, tc, focusedLane ? colour::text : colour::dim, bg);
        borders (videoY);

        for (int c = 0; c < laneW; ++c)
        {
            const auto t0 = origin + c * spc, t1 = t0 + spc;
            const bool inVideo = t1 > start && t0 < end;
            const bool selected = focusedLane && inTimeSelection (t0, t1);
            const int cellBg = selected ? (inVideo ? colour::selClipBg : colour::selBg) : inVideo ? colour::videoBg : focusedLane ? colour::laneFocusBg : -1;
            canvas.put (laneX + c, videoY, L' ', colour::videoText, cellBg);
        }

        // Labels every so many seconds of the video, as far apart as a label needs; frame
        // ticks between them when a frame is two columns or more.
        const auto columnsPerSecond = app.sampleRate() / spc;
        double every = 1.0;

        for (const double e : { 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0 })
            if ((every = e) * columnsPerSecond >= 10.0)
                break;

        if (spf / spc >= 2.0)
            for (int c = 0; c < laneW; ++c)
            {
                const auto t0 = origin + c * spc, t1 = t0 + spc;
                const auto k = std::ceil ((t0 - start) / spf - 1e-9);

                if (start + k * spf < t1 && start + k * spf >= start && start + k * spf < end)
                    canvas.put (laneX + c, videoY, gridMark, colour::videoText, canvas.at (laneX + c, videoY).bg);
            }

        // The name first, then the labels where they fit.
        const int startCol = juce::jmax (0, columnOf ((juce::int64) start));
        int videoFree = 0;

        if (startCol < laneW && end > origin)
        {
            const auto name = app.videoFile().getFileNameWithoutExtension();
            const int endCol = juce::jmin (laneW, columnOf ((juce::int64) end));
            const int room = juce::jmin (endCol - startCol - 1, 24);

            if (room > 2)
            {
                for (int j = 0; j < juce::jmin (room, name.length()); ++j)
                    canvas.put (laneX + startCol + 1 + j, videoY, (wchar_t) name[j], colour::videoText, canvas.at (laneX + startCol + 1 + j, videoY).bg, true);

                videoFree = startCol + 2 + juce::jmin (room, name.length());
            }
        }

        const auto firstLabel = std::ceil (juce::jmax (0.0, (origin - start) / app.sampleRate()) / every) * every;

        for (auto secs = firstLabel; start + secs * app.sampleRate() < juce::jmin (end, origin + laneW * spc); secs += every)
        {
            const int c = columnOf ((juce::int64) std::llround (start + secs * app.sampleRate()));
            const auto whole = (int) std::llround (secs);
            const auto label = juce::String (whole / 60) + ":" + juce::String (whole % 60).paddedLeft ('0', 2)
                               + (fps > 0.0 && spf / spc >= 2.0 ? ":00" : "");

            if (c < videoFree || c < 0 || c + label.length() >= laneW)
                continue;

            for (int j = 0; j < label.length(); ++j)
                canvas.put (laneX + c + j, videoY, (wchar_t) label[j], colour::videoText, canvas.at (laneX + c + j, videoY).bg);

            videoFree = c + label.length() + 1;
        }

        // The playhead and cursor, as on the lanes: a colour behind a label, a line in the clear.
        auto mark = [&] (int c, int markBg, int markFg)
        {
            if (c < 0 || c >= laneW)
                return;

            auto& cell = canvas.at (laneX + c, videoY);
            const bool clear = cell.ch == L' ' || cell.ch == gridMark;
            canvas.put (laneX + c, videoY, clear ? cursorMark : cell.ch, clear ? markFg : cell.fg, markBg, cell.bold);
        };

        if (rolling)
            mark (playCol, colour::playheadBg, app.recording ? colour::rec : colour::play);
        else
            mark (cursorCol, colour::cursorBg, colour::accent);
    }

    // Tracks --------------------------------------------------------------------
    const int n = s.numTracks();
    // Fill view (f): the focused track takes all the rows; the others are an arrow away.
    const bool fill = app.fillView && ! app.onTopLane() && ! app.mixFocused() && n > 0;

    // Open automation lanes go under their track, two rows each on tall tracks (fill hides them).
    // The mix's go under the Mix row, which moves up to make room (leaving the tracks a few rows).
    const int laneRows = s.trackHeight() > 1 ? 2 : 1;
    const int tracksTop = rulerY + 1 + (videoY >= 0 ? 1 : 0);
    auto mixLanes = fill ? std::vector<juce::String>() : app.lanesShown (-1);
    mixLanes.resize (std::min (mixLanes.size(), (size_t) std::max (0, (h - 6 - tracksTop - 4) / laneRows)));
    const int masterY = h - 6 - (int) mixLanes.size() * laneRows, tracksBottom = masterY - 1; // exclusive
    const int rowsPerTrack = fill ? std::max (1, tracksBottom - tracksTop) : s.trackHeight(); // "height", per session
    auto lanesOf = [&] (int track) { return fill ? std::vector<juce::String>() : app.lanesShown (track); };
    auto blockRows = [&] (int track) { return rowsPerTrack + (int) lanesOf (track).size() * laneRows; };

    // How many tracks fit from scroll on (at least one, cut short if it must be).
    auto fitFrom = [&] (int scroll)
    {
        int used = 0, count = 0;

        for (int i = scroll; i < n; ++i)
        {
            const auto rows = blockRows (i);

            if (count > 0 && used + rows > tracksBottom - tracksTop)
                break;

            used += rows;
            ++count;
        }

        return std::max (1, count);
    };

    const int focusRow = std::min (app.focused, n - 1); // the mix row lives below the list

    if (focusRow < app.trackScroll)
        app.trackScroll = std::max (0, focusRow);

    while (app.trackScroll < focusRow && app.trackScroll + fitFrom (app.trackScroll) <= focusRow)
        ++app.trackScroll;

    const int visible = fill ? 1 : fitFrom (app.trackScroll); // tracks that fit
    std::vector<int> trackY; // where each visible track starts

    for (int row = 0, y = tracksTop; row < visible && app.trackScroll + row < n; ++row)
    {
        trackY.push_back (y);
        y += blockRows (app.trackScroll + row);
    }

    const int tracksEnd = trackY.empty() ? tracksTop : trackY.back() + blockRows (app.trackScroll + (int) trackY.size() - 1);

    // Where things were drawn, for the mouse: which track (and lane) each row is.
    laneMap = { laneX, laneW, rulerY, tracksTop, rowsPerTrack, app.trackScroll, std::min (visible, n - app.trackScroll), videoY, {}, origin, spc, true };

    for (size_t row = 0; row < trackY.size(); ++row)
    {
        const int track = app.trackScroll + (int) row;

        for (int r = 0; r < rowsPerTrack; ++r)
            laneMap.rows.push_back ({ track, -1 });

        for (int lane = 0; lane < (int) lanesOf (track).size(); ++lane)
            for (int r = 0; r < laneRows; ++r)
                laneMap.rows.push_back ({ track, lane });
    }

    // Loudness as shade density; these four blocks exist in every console font.
    static const wchar_t levels[] = L" \x2591\x2592\x2593\x2588";
    auto glyphFor = [] (float peak)
    {
        const auto db = toDb (peak);
        return db < -54.0f ? levels[0] : levels[juce::jlimit (1, 4, 1 + (int) ((db + 54.0f) / 54.0f * 4.0f))];
    };

    // Loudness on the waveform's scale: 0 at -54 dB and below, 1 at full scale.
    auto waveLevel = [] (float gain)
    {
        const auto db = toDb (gain);
        return db < -54.0f ? 0.0 : juce::jlimit (0.0, 1.0, (db + 54.0) / 54.0);
    };

    // Taller tracks draw the waveform centred: the highest sample sets how far it reaches up from
    // the middle, the lowest how far down. Fonts with eighth blocks (Cascadia, the edge-glyph
    // fonts) draw that to an eighth of a row; below the middle the fill hangs from a cell's top,
    // which no font has, so it's the lower block for the gap drawn inverted (invert: swap the
    // cell's colours). Consolas keeps shades: a row the wave only partly reaches is lighter.
    static const wchar_t eighths[] = L" \x2581\x2582\x2583\x2584\x2585\x2586\x2587\x2588"; // filled from the bottom
    struct WaveCell { wchar_t ch; bool invert; };

    auto waveCell = [&] (WavePoint p, int r) -> WaveCell
    {
        const auto peak = std::max (-p.lo, p.hi);

        if (! edges)
        {
            if (rowsPerTrack == 1)
                return { glyphFor (peak), false };

            const auto level = waveLevel (peak);
            const auto reach = level * rowsPerTrack * 0.5 + 0.25;                              // rows from the middle
            const auto from = std::max (0.0, std::abs (r + 0.5 - rowsPerTrack * 0.5) - 0.5); // where this row starts
            const auto cover = juce::jlimit (0.0, 1.0, reach - from);
            return { level <= 0.0 ? levels[0] : levels[juce::jlimit (0, 4, (int) std::ceil (cover * 4.0 - 0.5))], false };
        }

        // One row: a bar rising from the bottom (on the same scale as below).
        if (rowsPerTrack == 1)
        {
            const auto level = waveLevel (peak);
            const auto n = level <= 0.0 ? 0 : juce::jlimit (1, 8, juce::roundToInt (level * 8.0));
            return { n == 8 ? L' ' : eighths[n], n == 8 };
        }

        // In eighths from the track's bottom: the fill spans [bottom, top) around the middle.
        const int middle = rowsPerTrack * 4;
        auto reach = [&] (float v)
        {
            const auto level = waveLevel (v);
            return level <= 0.0 ? 0 : std::max (1, juce::roundToInt (level * middle));
        };
        const int top = middle + reach (p.hi), bottom = middle - reach (-p.lo);
        const int cellBottom = (rowsPerTrack - 1 - r) * 8;
        const int a = juce::jlimit (0, 8, bottom - cellBottom), b = juce::jlimit (0, 8, top - cellBottom); // [a, b) of this cell

        // A full cell is a space in the wave's colour: block glyphs stop a pixel short of the
        // cell, which would leave a seam between rows.
        if (b <= a) return { L' ', false };
        if (a == 0 && b == 8) return { L' ', true };
        if (a == 0) return { eighths[b], false };
        if (b == 8) return { eighths[a], true };

        // Odd heights put the middle inside a row, where a wave can touch neither edge of it:
        // whichever glyph covers [a, b) closest, a block from an edge or a line through the middle.
        auto miss = [&] (double lo, double hi) { return (hi - lo) + (b - a) - 2.0 * std::max (0.0, std::min (hi, (double) b) - std::max (lo, (double) a)); };
        WaveCell best { L'\x2501', false };
        auto bestMiss = miss (3.5, 4.5);

        for (int k = 1; k <= 8; ++k)
        {
            if (miss (0.0, k) < bestMiss)
                best = k == 8 ? WaveCell { L' ', true } : WaveCell { eighths[k], false }, bestMiss = miss (0.0, k);

            if (k < 8 && miss (k, 8.0) < bestMiss)
                best = { eighths[k], true }, bestMiss = miss (k, 8.0);
        }

        return best;
    };

    // The lowest and highest sample over [t0, t1) of a clip or take whose first sample sits at
    // timeline position start: from its overview, or from the audio itself once a column is
    // shorter than an overview block (zoomed right in).
    auto waveRange = [] (const std::vector<WavePoint>& ov, const juce::AudioBuffer<float>* audio, juce::int64 start, double t0, double t1)
    {
        WavePoint p;

        if (audio != nullptr && t1 - t0 < Recorder::overviewBlock)
        {
            const auto a = std::max (0, (int) std::floor (t0 - (double) start));
            const auto b = std::min (audio->getNumSamples(), (int) std::ceil (t1 - (double) start));

            for (int ch = 0; ch < audio->getNumChannels() && b > a; ++ch)
            {
                const auto r = audio->findMinMax (ch, a, b - a);
                p.lo = std::min (p.lo, r.getStart());
                p.hi = std::max (p.hi, r.getEnd());
            }

            return p;
        }

        const auto a = std::max (0, (int) std::floor ((t0 - (double) start) / Recorder::overviewBlock));
        const auto b = std::min ((int) ov.size(), (int) std::ceil ((t1 - (double) start) / Recorder::overviewBlock));

        for (int i = a; i < b; ++i)
        {
            p.lo = std::min (p.lo, ov[(size_t) i].lo);
            p.hi = std::max (p.hi, ov[(size_t) i].hi);
        }

        return p;
    };

    // Instrument tracks: each note a bar at its pitch. The lane spans the pitches in use, one per
    // half row when they fit (an octave or so in a short track) and sharing rows when they
    // don't. In fill view each row is one pitch, named on the left: a piano roll.
    auto drawNotes = [&] (int i, const juce::ValueTree& t, int y, int laneBg, int panelBg, bool rowInTimeSelection)
    {
        struct LaneNote
        {
            double s0, s1;
            int pitch;
            bool live;
        };

        std::vector<LaneNote> notes;
        const auto spb = s.samplesPerBeat();

        for (const auto& p : Session::placedNotes (t))
            notes.push_back ({ p.start * spb, p.end * spb, (int) p.node[ids::pitch], false });

        // The clips, in samples. The one a pass is recording into reaches to the playhead.
        const bool recordingHere = std::find (app.midiRecordTracks.begin(), app.midiRecordTracks.end(), i) != app.midiRecordTracks.end();
        const auto passClip = recordingHere && app.midiPassClips.count (i) > 0 ? app.midiPassClips.at (i) : juce::ValueTree();
        std::vector<std::pair<double, double>> clipSpans;

        for (const auto& c : Session::midiClips (t))
            clipSpans.push_back ({ (double) c[ids::beat] * spb, std::max (Session::clipEnd (c) * spb, c == passClip ? (double) playhead : 0.0) });

        // Keys still down while recording reach to the playhead.
        if (recordingHere)
            for (const auto& held : app.heldNotes)
                notes.push_back ({ held.second.beat * spb, (double) playhead + 1.0, held.second.pitch, true });

        int lo = 60, hi = 60;

        if (! notes.empty())
        {
            lo = 127, hi = 0;

            for (const auto& note : notes)
                lo = std::min (lo, note.pitch), hi = std::max (hi, note.pitch);
        }

        const int halves = rowsPerTrack * 2;
        const int span = std::min (128, std::max (hi - lo + 1, fill ? rowsPerTrack : halves));
        const int bottom = juce::jlimit (0, 128 - span, lo - (span - (hi - lo + 1)) / 2);
        const int top = bottom + span - 1;
        const bool pianoRoll = fill && span == rowsPerTrack; // one pitch per row

        // The half rows a pitch covers, counted from the top of the lane.
        auto halvesOf = [&] (int pitch)
        {
            const int a = (top - pitch) * halves / span;
            return std::make_pair (a, std::max (a + 1, (top - pitch + 1) * halves / span));
        };

        auto isBlackKey = [] (int pitch) { const int k = pitch % 12; return k == 1 || k == 3 || k == 6 || k == 8 || k == 10; };

        // Pitch names down the left: every row in a piano roll, else where each C falls.
        if (fill)
            for (int p = bottom; p <= top; ++p)
            {
                if (! pianoRoll && p % 12 != 0)
                    continue;

                const int r = halvesOf (p).first / 2;

                if (r < 1 || r >= rowsPerTrack) // the first row has the track's name
                    continue;

                const auto name = juce::MidiMessage::getMidiNoteName (p, true, true, 4);
                canvas.text (leftW - 1 - name.length(), y + r, name, p % 12 == 0 ? colour::text : isBlackKey (p) ? colour::faint : colour::dim,
                             panelBg, p % 12 == 0);
            }

        std::vector<int> cover ((size_t) halves);

        for (int c = 0; c < laneW; ++c)
        {
            const auto t0 = origin + c * spc, t1 = t0 + spc;
            const auto line = gridLevel (t0, t1);
            std::fill (cover.begin(), cover.end(), 0); // 1: a note, 2: one being recorded

            for (const auto& note : notes)
                if (note.s1 > t0 && note.s0 < t1)
                {
                    const auto [a, b] = halvesOf (note.pitch);

                    for (int k = a; k < b && k < halves; ++k)
                        cover[(size_t) k] = std::max (cover[(size_t) k], note.live ? 2 : 1);
                }

            const bool selected = rowInTimeSelection && inTimeSelection (t0, t1);
            const bool playheadHere = rolling && c == playCol;
            const bool cursorHere = c == cursorCol && ! playheadHere;

            // The topmost clip here, shaded like audio clips (neighbours alternate), with an
            // edge on its first and last column when it's wide enough to have one.
            int inClip = -1;
            wchar_t edgeCh = 0;

            for (int k = (int) clipSpans.size(); --k >= 0;)
                if (const auto [start, end] = clipSpans[(size_t) k]; t1 > start && t0 < end)
                {
                    inClip = k;

                    if (end - start >= 3.0 * spc)
                    {
                        if (start >= t0 && start < t1)
                            edgeCh = edges ? L'\x258f' : L'\x258c';
                        else if (end > t0 && end <= t1)
                            edgeCh = edges ? L'\x2595' : L'\x2590';
                    }
                    break;
                }

            for (int r = 0; r < rowsPerTrack; ++r)
            {
                const int up = cover[(size_t) (r * 2)], down = cover[(size_t) (r * 2 + 1)];
                const int pitch = top - r;
                const int clipBg = inClip < 0 ? -1 : inClip % 2 == 0 ? colour::clipBg : colour::clipAltBg;
                int cellBg = selected ? (inClip >= 0 ? colour::selClipBg : colour::selBg)
                             : inClip >= 0 ? clipBg
                             : pianoRoll && isBlackKey (pitch) ? colour::blackKeyBg : laneBg;
                wchar_t ch = line > 0 ? gridMark : L' ';
                int fg = line == 2 ? colour::gridBar : colour::faint;

                if (up > 0 || down > 0)
                {
                    ch = up > 0 && down > 0 ? L'\x2588' : up > 0 ? L'\x2580' : L'\x2584';
                    fg = std::max (up, down) == 2 ? colour::liveWave : colour::note;
                }
                else if (edgeCh != 0)
                {
                    ch = edgeCh, fg = colour::note;
                }
                else if (inClip >= 0 && (r == 0 || r == rowsPerTrack - 1))
                {
                    ch = L' ', fg = colour::note; // the outline is drawn in the cell's colour: no grid marks on it
                }

                // As in audio lanes: a colour behind the notes, a line where there are none.
                const bool empty = up == 0 && down == 0 && edgeCh == 0;

                if (playheadHere)
                {
                    cellBg = colour::playheadBg;

                    if (empty)
                        ch = cursorMark, fg = app.recording ? colour::rec : colour::play;
                }
                else if (cursorHere && ! rolling)
                {
                    cellBg = colour::cursorBg;

                    if (empty)
                        ch = cursorMark, fg = colour::accent;
                }
                else if (cursorHere && empty)
                    ch = cursorMark, fg = colour::dim;

                canvas.put (laneX + c, y + r, ch, fg, cellBg);

                // A clip's outline: its top along the first row, its bottom along the last.
                if (inClip >= 0)
                {
                    auto& cell = canvas.at (laneX + c, y + r);
                    cell.over = r == 0;
                    cell.under = r == rowsPerTrack - 1;
                }
            }
        }
    };

    if (n == 0)
        canvas.text (laneX + 1, tracksTop, L"no tracks \x00b7 type add", colour::dim);

    for (int row = 0; row < visible && app.trackScroll + row < n; ++row)
    {
        const int i = app.trackScroll + row, y = trackY[(size_t) row];
        const auto t = s.track (i);
        const bool focusedRow = i == app.focused && ! app.onTopLane();
        const bool selectedRow = std::find (selTracks.begin(), selTracks.end(), i) != selTracks.end();
        const int bg = app.trackFlashing (t) ? colour::accent : focusedRow ? colour::focusBg : (selectedRow ? colour::selBg : -1);
        // A time selection covers the tracks edits will act on: the selected ones, else the focused
        // one, or every track from the bar or markers row (App::defaultTargets).
        const bool rowInTimeSelection = timeSel && (app.onTopLane() || (selTracks.empty() ? focusedRow : selectedRow));
        // The row stays lit all the way along the timeline, so you can see which track you're on.
        const int laneBg = selectedRow ? colour::laneSelBg : focusedRow ? colour::laneFocusBg : -1;

        // Header: focus, number, name and input on the first row; the R I S M flags on the second
        // (or after the name when tracks are one row tall); the rest is its colour.
        for (int r = 0; r < rowsPerTrack; ++r)
        {
            canvas.fill (0, y + r, leftW, L' ', -1, bg);
            canvas.fill (meterX, y + r, meterPanel, L' ', -1, bg);
            borders (y + r);
        }

        // The track's type at a glance, so an old session reads straight away: a strip of its
        // colour down the left edge (thicker on the focused track) and its label filled with it.
        // Audio blue like waveforms, sample warm like hits, instrument teal like notes, buses dark green.
        const int typeColour = Session::isBusTrack (t)        ? colour::busColour
                               : Session::isSampleTrack (t)     ? colour::hit
                               : Session::isInstrumentTrack (t) ? colour::note
                                                                : colour::wave;

        for (int r = 0; r < rowsPerTrack; ++r)
            canvas.put (0, y + r, focusedRow ? L'\x2588' : L'\x258c', typeColour, bg);

        // The type label: a chip of the type's colour (red when something's wrong, dim while loading).
        auto chip = [&] (const juce::String& text, int fill)
        {
            const int x = leftW - 6;
            canvas.fill (x, y, 6, L' ', -1, fill);
            canvas.text (x + 1, y, text, fill == colour::busColour ? colour::busText : colour::chipText, fill, true, 5);
        };

        canvas.text (1, y, juce::String (i + 1).paddedLeft (' ', 2), colour::text, bg, true);
        canvas.text (4, y, padRight (t[ids::name].toString(), 10), colour::text, bg);

        const int flagX = twoRows ? 4 : 15, flagY = twoRows ? y + 1 : y;
        auto flag = [&] (int x, bool on, wchar_t ch, int fg) { canvas.put (x, flagY, on ? ch : L'\x00b7', on ? fg : colour::faint, bg, on); };
        // R I S M: arm, input, solo, mute. A bus has nothing to arm or monitor: just solo and mute.
        const bool busRow = Session::isBusTrack (t);

        if (! busRow)
        {
            flag (flagX, t[ids::armed], L'R', colour::rec);
            flag (flagX + 1, t[ids::monitor], L'I', colour::wave);
        }

        flag (flagX + 2, t[ids::solo], L'S', colour::play);
        flag (flagX + 3, app.currentValue (t, "mute") >= 0.5, L'M', app.isAutomated (t, "mute") ? colour::note : colour::meterYellow);

        const bool sampleTrack = Session::isSampleTrack (t);
        const bool instrumentTrack = Session::isInstrumentTrack (t);
        const bool busTrack = Session::isBusTrack (t);
        const int input = t[ids::input];

        if (busTrack)
            chip ("bus", typeColour);
        else if (sampleTrack)
            chip ("smp", typeColour);
        else if (instrumentTrack)
        {
            // "inst", red when its plugin didn't load; the plugin's name after the flags.
            const auto problem = app.instrumentProblem (i);
            const bool pending = app.pluginPending (t);
            chip ("inst", problem.isNotEmpty() ? colour::error : pending ? colour::dim : typeColour);

            if (twoRows)
                canvas.text (9, y + 1, problem.isNotEmpty() ? problem : pending ? juce::String (L"loading\x2026") : t[ids::pluginName].toString(),
                             problem.isNotEmpty() ? colour::error : colour::dim, bg, false, fill ? 6 : leftW - 10);
        }
        else
        {
            // A stereo track records a pair: "i3-4" (past 9 just "11-12").
            const bool stereo = app.trackIsStereo (t);
            const auto text = input <= 0 ? juce::String ("--")
                              : stereo   ? (input < 10 ? "i" : "") + juce::String (input) + "-" + juce::String (input + 1)
                                         : "in" + juce::String (input);
            chip (text, input + (stereo ? 1 : 0) > app.engine.numInputChannels() ? colour::error : typeColour);
        }

        // Lane
        struct LaneClip
        {
            const ClipData* data;
            juce::int64 start, offset, length;
        };

        std::vector<LaneClip> laneClips;
        std::vector<std::pair<double, float>> laneHits; // sample tracks: (position in samples, velocity)
        std::vector<std::pair<double, double>> hitSpans;  // ...and their clips, in samples

        if (sampleTrack)
        {
            for (const auto& h : Session::placedHits (t))
                laneHits.push_back ({ h.beat * s.samplesPerBeat(), h.vel });

            std::sort (laneHits.begin(), laneHits.end());

            // The clip a pass is recording into reaches to the playhead.
            const auto passClip = app.recording && app.midiPassClips.count (i) > 0 ? app.midiPassClips.at (i) : juce::ValueTree();

            for (const auto& c : Session::hitClips (t))
                hitSpans.push_back ({ (double) c[ids::beat] * s.samplesPerBeat(),
                                      std::max (Session::clipEnd (c) * s.samplesPerBeat(), c == passClip ? (double) playhead : 0.0) });
        }
        else
        {
            for (const auto& c : Session::clips (t))
                if (auto it = app.clips.find (c[ids::file].toString()); it != app.clips.end())
                    laneClips.push_back ({ &it->second, (juce::int64) c[ids::start], (juce::int64) c[ids::offset], (juce::int64) c[ids::length] });
        }

        const LiveTake* liveTake = nullptr;

        for (const auto& l : app.live)
            if (l.recorder->trackIndex == i && l.start >= 0)
                liveTake = &l;

        if (instrumentTrack)
            drawNotes (i, t, y, laneBg, bg, rowInTimeSelection);
        else
        {
            for (int c = 0; c < laneW; ++c)
            {
                const auto t0 = origin + c * spc, t1 = t0 + spc;
                wchar_t ch = L' ';
                int fg = colour::wave, cellBg = laneBg;
                bool inClip = false;
                std::optional<WavePoint> wave; // a clip or take here: drawn per row by waveCell
                wchar_t edgeCh = 0;            // a clip's first or last column

                const auto line = gridLevel (t0, t1);

                if (line > 0)
                {
                    ch = gridMark;
                    fg = line == 2 ? colour::gridBar : colour::faint;
                }

                // Sample tracks read as a step sequencer: every column is a step, a hit shows its
                // velocity as a shade, an empty step a faint dot (a grid mark on grid lines).
                if (sampleTrack)
                {
                    ch = line > 0 ? gridMark : L'\x00b7';
                    fg = line == 2 ? colour::dim : line == 1 ? colour::gridBar : colour::faint;

                    // Inside a clip: its shade (neighbours alternate) and an outline, top and
                    // bottom, so a loop reads as one block.
                    for (int k = (int) hitSpans.size(); --k >= 0;)
                        if (t1 > hitSpans[(size_t) k].first + 0.5 && t0 < hitSpans[(size_t) k].second - 0.5)
                        {
                            cellBg = k % 2 == 0 ? colour::clipBg : colour::clipAltBg;
                            inClip = true;
                            break;
                        }

                    auto it = std::lower_bound (laneHits.begin(), laneHits.end(), std::make_pair (t0 - 0.5, 0.0f));
                    float vel = -1.0f;

                    for (; it != laneHits.end() && it->first < t1 - 0.5; ++it)
                        vel = std::max (vel, it->second);

                    if (vel >= 0.0f)
                    {
                        ch = vel >= 0.95f ? L'\x2588' : vel >= 0.7f ? L'\x2593' : vel >= 0.4f ? L'\x2592' : L'\x2591';
                        fg = colour::hit;

                        if (! inClip)
                            cellBg = colour::clipBg;
                    }
                }

                // The topmost clip in this column (later clips play over earlier ones). Neighbouring
                // clips alternate shades so each hit reads as its own block.
                for (int k = (int) laneClips.size(); --k >= 0;)
                {
                    const auto& lc = laneClips[(size_t) k];
                    const auto start = (double) lc.start, end = start + (double) lc.length;

                    if (t1 > start && t0 < end)
                    {
                        const auto fileZero = lc.start - lc.offset; // timeline position of the file's first sample
                        wave = waveRange (lc.data->overview, lc.data->audio.get(), fileZero, std::max (t0, start), std::min (t1, end));
                        fg = colour::wave;
                        cellBg = k % 2 == 0 ? colour::clipBg : colour::clipAltBg;
                        inClip = true;

                        // An outline (edges here, top and bottom lines below), so a clip's length
                        // shows against empty space even where it's silent. Clips only a column or
                        // two wide keep their waveform instead of edges.
                        if (end - start >= 3.0 * spc)
                        {
                            if (start >= t0 && start < t1)
                                edgeCh = edges ? L'\x258f' : L'\x258c';
                            else if (end > t0 && end <= t1)
                                edgeCh = edges ? L'\x2595' : L'\x2590';
                        }
                        break;
                    }
                }

                if (liveTake != nullptr)
                {
                    const auto end = (double) liveTake->start + (double) liveTake->overview.size() * Recorder::overviewBlock;

                    if (t1 > (double) liveTake->start && t0 < end)
                    {
                        wave = waveRange (liveTake->overview, nullptr, liveTake->start, t0, t1);
                        edgeCh = 0;
                        inClip = false;
                        fg = colour::liveWave;
                        cellBg = colour::recBg;
                    }
                }

                if (rowInTimeSelection && inTimeSelection (t0, t1))
                    cellBg = (cellBg == colour::clipBg || cellBg == colour::clipAltBg) ? colour::selClipBg : colour::selBg;

                // The playhead and the cursor colour the cell behind what's there, so a clip's edge
                // or a hit under them still shows; on an empty cell they're a thin line. The
                // playhead wins when they share a column.
                const bool playheadHere = rolling && c == playCol;
                const bool cursorHere = c == cursorCol && ! playheadHere;

                for (int r = 0; r < rowsPerTrack; ++r)
                {
                    const auto cell = edgeCh == 0 && wave ? waveCell (*wave, r) : WaveCell { edgeCh != 0 ? edgeCh : ch, false };
                    auto rowCh = cell.ch;
                    auto rowFg = fg;
                    auto rowBg = cellBg;
                    const bool empty = ! cell.invert && (rowCh == L' ' || rowCh == gridMark);

                    if (playheadHere)
                    {
                        rowBg = colour::playheadBg;

                        if (empty)
                            rowCh = cursorMark, rowFg = app.recording ? colour::rec : colour::play;
                    }
                    else if (cursorHere && ! rolling)
                    {
                        rowBg = colour::cursorBg;

                        if (empty)
                            rowCh = cursorMark, rowFg = colour::accent;
                    }
                    else if (cursorHere && empty)
                        rowCh = cursorMark, rowFg = colour::dim; // playing: the cursor is just a mark

                    if (cell.invert)
                        std::swap (rowFg, rowBg);

                    canvas.put (laneX + c, y + r, rowCh, rowFg, rowBg);

                    // A clip's outline: its top along the first row, its bottom along the last.
                    if (inClip)
                    {
                        canvas.at (laneX + c, y + r).over = r == 0;
                        canvas.at (laneX + c, y + r).under = r == rowsPerTrack - 1;
                    }
                }
            }
        }

        // A bus lane says what feeds it.
        if (busTrack)
        {
            juce::StringArray from;

            for (auto f : app.busFeeders (i))
                from.add (juce::String (f + 1) + " " + s.track (f)[ids::name].toString());

            canvas.text (laneX + 1, y, from.isEmpty() ? juce::String (L"bus \x00b7 nothing feeds it yet \x00b7 2 send " + t[ids::name].toString().toLowerCase())
                                                      : L"\x2190 " + from.joinIntoString (juce::String (L" \x00b7 ")),
                         colour::dim, laneBg, false, laneW - 2);
        }

        // Routing: inserts A-E on the first row; sends A-E and the output on the second, with
        // the send levels under them when there's a third. One row tall: inserts and the output.
        if (io)
        {
            const bool inColumn = focusedRow && app.ioCell >= 0;

            for (int r = 0; r < rowsPerTrack; ++r)
                canvas.fill (ioX, y + r, ioPanel, L' ', -1, bg);

            auto cellBg = [&] (int cell) { return inColumn && app.ioCell == cell ? colour::selBg : bg; };

            for (int k = 0; k < Session::numSends; ++k)
            {
                const int x = ioX + ioSlots + k * ioSlotW, cbg = cellBg (App::ioFirstInsert + k);
                const auto insert = Session::insert (t, k);
                canvas.fill (x - 1, y, ioSlotW, L' ', -1, cbg);

                if (! insert.isValid())
                    canvas.put (x, y, L'\x00b7', colour::faint, cbg);
                else
                    canvas.text (x, y, insert[ids::pluginName].toString(),
                                 app.insertProblem (insert).isNotEmpty() ? colour::error
                                 : (bool) insert[ids::bypass]           ? colour::faint
                                 : app.pluginPending (insert)           ? colour::dim // still loading
                                 : app.insertSittingOut (insert)        ? colour::meterYellow // low latency mode
                                                                         : colour::note,
                                 cbg, false, ioSlotW - 1);
            }

            if (rowsPerTrack > 1)
                for (int k = 0; k < Session::numSends; ++k)
                {
                    const int x = ioX + ioSlots + k * ioSlotW, cbg = cellBg (App::ioFirstSend + k);
                    const auto send = Session::send (t, k);
                    canvas.fill (x - 1, y + 1, ioSlotW, L' ', -1, cbg);

                    if (! send.isValid())
                    {
                        canvas.put (x, y + 1, L'\x00b7', colour::faint, cbg);
                        continue;
                    }

                    const int dest = s.findBus (send[ids::to]);
                    const bool off = send[ids::mute];
                    canvas.text (x, y + 1, dest < 0 ? juce::String ("?") : s.track (dest)[ids::name].toString(),
                                 dest < 0 ? colour::error : off ? colour::faint : colour::text, cbg, false, ioSlotW - 1);

                    // The level under the name; pre-fader sends in the accent colour.
                    if (rowsPerTrack > 2)
                    {
                        const auto sendParam = "send " + Session::slotName (k).toLowerCase();
                        const double db = app.currentValue (t, sendParam);
                        const auto level = db <= -95.9 ? juce::String (L"-\x221e") : juce::String (juce::roundToInt (db));
                        canvas.fill (x - 1, y + 2, ioSlotW, L' ', -1, cbg);
                        canvas.text (x, y + 2, level,
                                     off ? colour::faint : app.isAutomated (t, sendParam) ? colour::note : (bool) send[ids::pre] ? colour::accent : colour::dim,
                                     cbg, false, ioSlotW - 1);
                    }
                }

            const int outY = rowsPerTrack > 1 ? y + 1 : y;
            const int outBus = s.findBus (t[ids::out]);
            const auto outName = outBus < 0 ? juce::String ("Mix") : s.track (outBus)[ids::name].toString();
            canvas.fill (ioX + ioOut - 1, outY, 6, L' ', -1, cellBg (App::ioOutput));
            canvas.text (ioX + ioOut, outY, outName, outBus < 0 ? colour::dim : colour::accent, cellBg (App::ioOutput), outBus >= 0, 5);
        }

        // Volume and dB on the first row; the meter runs the track's full height. Armed tracks
        // show their input, others their playback.
        const auto& m = app.trackMeters[(size_t) i];
        drawVolume (meterX, y, app.currentValue (t, "vol"), bg, app.isAutomated (t, "vol"));
        drawDb (meterX + 12, y, m.hold, m.clipped, bg);

        // Pan under the volume (centre dim, like unity); "st" marks a stereo track.
        if (rowsPerTrack > 1)
        {
            const double pan = app.currentValue (t, "pan");
            canvas.text (meterX, y + 1, App::panText (pan).paddedLeft (' ', 5),
                         app.isAutomated (t, "pan") ? colour::note : std::abs (pan) < 0.005 ? colour::dim : colour::text, bg);

            if (app.trackIsStereo (t))
                canvas.text (meterX + 12, y + 1, "   st", colour::faint, bg);
        }

        for (int r = 0; r < rowsPerTrack; ++r)
            drawMeter (meterX + 6, y + r, 5, m.level, colour::meterBg);
    }

    // Automation lanes, under the tracks that have them open: the curve as a filled graph (8
    // steps a row), points in the accent colour, a lane with no points its setting as a faint
    // level; on the left its name, value at the cursor (or playhead), and controller.
    // i: the track (-1: the mix, its lanes under the Mix row); top: the first lane's row.
    const auto at = rolling ? playhead : app.cursor;

    auto drawLanes = [&] (int i, const juce::ValueTree& t, const std::vector<juce::String>& lanes, int top, bool selectedRow)
    {
        for (int k = 0; k < (int) lanes.size(); ++k)
        {
            const auto& param = lanes[(size_t) k];
            const int y = top + k * laneRows;
            const bool onOwner = i < 0 ? app.mixFocused() : i == app.focused && ! app.mixFocused();
            const bool focusedLane = onOwner && app.focusedLane == k && ! app.onTopLane();
            const int bg = focusedLane ? colour::focusBg : -1;
            const int laneBg = focusedLane ? colour::laneFocusBg : selectedRow ? colour::laneSelBg : -1;
            // The selection lights the lane you're on (a selection's edits go to it).
            const bool inSel = timeSel && (focusedLane || app.onTopLane());

            const auto target = AutomationTarget::parse (param);
            // Off (shift+O): its points drawn faint, its value the setting that plays instead.
            const auto node = Session::automation (t, param);
            const bool off = node.isValid() && App::laneOff (node);
            const auto curve = app.automationCurve (t, param, true);
            const auto setting = curve.pos.empty() || off ? app.staticValue (t, param, false) : 0.0;
            const int steps = laneRows * 8;
            const bool touched = node.isValid() && app.isTouched (node);

            auto height = [&] (double v)
            {
                if (target.isDecibels())
                    return v <= -95.9 ? 0.0 : juce::jlimit (0.0, 1.0, (v + 60.0) / 72.0); // -60 to +12 dB, like a fader
                if (target.kind == AutomationTarget::Kind::pan)
                    return (v + 1.0) / 2.0;
                return juce::jlimit (0.0, 1.0, v);
            };

            // Header: the track's colour strip carries on down; the lane's name, its value here,
            // its controller (lit while it moves). Indented, hanging off the track by a tree line.
            constexpr int indent = 4;
            const bool lastLane = k == (int) lanes.size() - 1;

            for (int r = 0; r < laneRows; ++r)
            {
                canvas.fill (0, y + r, leftW, L' ', -1, bg);
                canvas.fill (meterX, y + r, meterPanel, L' ', -1, bg);

                if (io)
                    canvas.fill (ioX, y + r, ioPanel, L' ', -1, bg);

                borders (y + r);
                canvas.put (0, y + r, focusedLane && r == 0 ? L'\x25b8' : L'\x2502', focusedLane ? colour::accent : colour::faint, bg);
                canvas.put (2, y + r, r == 0 ? (lastLane ? L'\x2514' : L'\x251c') : (lastLane ? L' ' : L'\x2502'), colour::faint, bg);
            }

            const auto nameEnd = canvas.text (indent, y, app.automationName (t, param, false), focusedLane ? colour::text : colour::dim, bg, focusedLane,
                                              leftW - indent - 1 - (off ? 4 : 0));

            if (off)
                canvas.text (nameEnd + 1, y, "OFF", colour::rec, bg, true);

            const auto value = app.automationText (t, param, (curve.pos.empty() || off) && ! touched ? setting : (double) curve.at (at), false);
            const auto control = app.controlText (node);

            if (laneRows > 1)
            {
                canvas.text (indent + 1, y + 1, value, colour::dim, bg, false, leftW - indent - 2);

                if (control.isNotEmpty())
                    canvas.text (std::max (indent + 1 + value.length() + 1, leftW - 1 - control.length()), y + 1, control, touched ? colour::accent : colour::faint, bg, touched);
            }
            else
            {
                canvas.text (std::max (nameEnd + (off ? 5 : 1), leftW - 1 - value.length()), y, value, colour::dim, bg);
            }

            if (io && control.isNotEmpty())
                canvas.text (ioX + 1, y, control + (touched ? juce::String (app.writeOn && rolling ? L" \x25cf writing" : L" moving") : juce::String()),
                             touched ? colour::accent : colour::faint, bg, touched, ioPanel - 2);

            for (int c = 0; c < laneW; ++c)
            {
                const auto t0 = origin + c * spc, t1 = t0 + spc;
                const auto mid = (juce::int64) std::llround ((t0 + t1) * 0.5);
                // Writing: what's been recorded this pass, up to the playhead, in the record colour.
                double written = 0.0;
                const bool wrote = touched && app.writtenValue (node, mid, written);
                const auto v = wrote ? written : curve.pos.empty() && ! touched ? setting : (double) curve.at (juce::jmax<juce::int64> (0, mid));
                const auto filled = juce::roundToInt (height (v) * steps);
                const auto firstPoint = std::lower_bound (curve.pos.begin(), curve.pos.end(), (juce::int64) std::ceil (t0));
                const bool point = firstPoint != curve.pos.end() && (double) *firstPoint < t1;
                const auto line = gridLevel (t0, t1);
                int cellBg = inSel && inTimeSelection (t0, t1) ? colour::selBg : laneBg;
                const bool playheadHere = rolling && c == playCol;
                const bool cursorHere = c == cursorCol && ! playheadHere;

                if (playheadHere)
                    cellBg = colour::playheadBg;
                else if (cursorHere && ! rolling)
                    cellBg = colour::cursorBg;

                for (int r = 0; r < laneRows; ++r)
                {
                    const int part = juce::jlimit (0, 8, filled - (laneRows - 1 - r) * 8);
                    wchar_t ch = part == 0 ? (line > 0 && r == laneRows - 1 ? gridMark : L' ') : (wchar_t) (0x2580 + part);
                    int fg = wrote ? colour::rec : curve.pos.empty() || off ? colour::faint : point ? colour::accent : colour::note;

                    if (part == 0 && line > 0)
                        fg = line == 2 ? colour::gridBar : colour::faint;

                    if (part == 0 && (playheadHere || (cursorHere && ! rolling)))
                        ch = cursorMark, fg = playheadHere ? (app.recording ? colour::rec : colour::play) : colour::accent;

                    canvas.put (laneX + c, y + r, ch, fg, cellBg);
                }
            }

            // A lane with nothing on it yet says how to start.
            if (curve.pos.empty() && ! touched && focusedLane)
                canvas.text (laneX + 1, y, L"no points: alt+\x2191\x2193 or pt <value> \x00b7 map: a knob runs it", colour::dim, canvas.at (laneX + 1, y).bg, false, laneW - 2);
        }
    };

    for (size_t row = 0; row < trackY.size(); ++row)
    {
        const int i = app.trackScroll + (int) row;

        if (const auto lanes = lanesOf (i); ! lanes.empty())
            drawLanes (i, s.track (i), lanes, trackY[row] + rowsPerTrack, std::find (selTracks.begin(), selTracks.end(), i) != selTracks.end());
    }

    // The cursor's handle sits under the last track, clear of the ruler's numbers.
    if (cursorCol >= 0 && cursorCol < laneW && n > 0 && tracksEnd < tracksBottom + 1)
        canvas.put (laneX + cursorCol, tracksEnd, L'\x25b2', colour::accent);

    // Mix ---------------------------------------------------------------------------
    const bool mixFocused = app.mixFocused();

    for (int ch = 0; ch < 2; ++ch)
    {
        const int y = masterY + ch;
        const int bg = mixFocused ? colour::focusBg : -1;
        canvas.fill (0, y, laneX - 1, L' ', -1, bg);
        canvas.fill (meterX, y, meterPanel, L' ', -1, bg);

        if (ch == 0)
        {
            if (mixFocused)
                canvas.put (0, y, L'\x25b8', colour::accent, bg);

            canvas.text (4, y, "Mix", colour::text, bg, true);
            drawVolume (meterX, y, app.currentValue (s.tree(), "vol"), bg, app.isAutomated (s.tree(), "vol"));
        }

        canvas.text (leftW - 2, y, ch == 0 ? "L" : "R", colour::dim, bg);

        // The mix's inserts A-E in the routing column (after the mix volume, as on a master).
        if (io && ch == 0)
        {
            const bool inColumn = mixFocused && app.ioCell >= 0;

            for (int k = 0; k < Session::numSends; ++k)
            {
                const int x = ioX + ioSlots + k * ioSlotW;
                const int cbg = inColumn && app.ioCell == App::ioFirstInsert + k ? colour::selBg : bg;
                const auto insert = Session::insert (s.tree(), k);
                canvas.fill (x - 1, y, ioSlotW, L' ', -1, cbg);

                if (! insert.isValid())
                    canvas.put (x, y, L'\x00b7', colour::faint, cbg);
                else
                    canvas.text (x, y, insert[ids::pluginName].toString(),
                                 app.insertProblem (insert).isNotEmpty() ? colour::error
                                 : (bool) insert[ids::bypass]           ? colour::faint
                                 : app.pluginPending (insert)           ? colour::dim
                                 : app.insertSittingOut (insert)        ? colour::meterYellow
                                                                         : colour::note,
                                 cbg, false, ioSlotW - 1);
            }
        }

        borders (y);

        if (ch == 0 && fill)
            canvas.text (laneX + 1, y, "track " + juce::String (app.focused + 1) + " of " + juce::String (n) + L" filling the view \x00b7 \x2191\x2193 others \x00b7 f shows all",
                         colour::faint);
        else if (ch == 0 && n > visible)
            canvas.text (laneX + 1, y, "tracks " + juce::String (app.trackScroll + 1) + "-" + juce::String (std::min (n, app.trackScroll + visible))
                                           + " of " + juce::String (n) + L" \x00b7 \x2191\x2193 to scroll",
                         colour::faint);

        const auto& m = app.masterMeters[(size_t) ch];
        drawMeter (meterX + 6, y, 5, m.level, colour::meterBg);
        drawDb (meterX + 12, y, m.hold, m.clipped, bg);
    }

    drawLanes (-1, s.tree(), mixLanes, masterY + 2, false);
}

void Ui::drawVolume (int x, int y, double db, int bg, bool automated)
{
    // Unity is dim so a changed fader stands out; automation (or a knob) moving it, its colour.
    const auto text = formatDb (db).paddedLeft (' ', 5);
    canvas.text (x, y, text, automated ? colour::note : std::abs (db) < 0.05 ? colour::dim : colour::text, bg, automated || std::abs (db) >= 0.05);
}

void Ui::drawBottom (App& app)
{
    const int w = canvas.width(), h = canvas.height();
    const int sepY = h - 4, msgY = h - 3, cmdY = h - 2, hintY = h - 1;

    canvas.fill (0, sepY, w, L'\x2500', colour::faint);

    if (app.message.isNotEmpty())
        canvas.text (1, msgY, app.message, app.messageIsError ? colour::error : colour::ok, -1, false, w - 2);

    const auto rolling = app.engine.isRolling();

    // The command line is always there; while rolling a status label sits in front of it.
    int x = 0;

    if (app.recording)
    {
        juce::StringArray tracks;

        for (const auto& l : app.live)
            if (! l.finishing)
                tracks.add (juce::String (l.recorder->trackIndex + 1));

        x = canvas.text (0, cmdY, L" \x25cf RECORDING  " + tracks.joinIntoString (" ") + "   " + app.formatTime (app.heardPlayhead() - app.playStart) + " ",
                         231, colour::rec, true);
    }
    else if (rolling)
    {
        x = canvas.text (0, cmdY, L" \x25ba PLAYING ", 231, colour::playBg, true);
    }

    // A bounce in progress: a bar, so nobody opens the file before it's written.
    if (const auto* b = app.currentBounce())
    {
        constexpr int barW = 20;
        const auto p = juce::jlimit (0.0f, 1.0f, b->progress());
        const auto filled = juce::roundToInt (p * barW);
        x = canvas.text (x, cmdY, L" \x2193 BOUNCING ", 231, colour::marker, true);
        x = canvas.text (x, cmdY, juce::String::repeatedString (juce::String (L"\x2588"), filled), colour::marker, -1);
        x = canvas.text (x, cmdY, juce::String::repeatedString (juce::String (L"\x2591"), barW - filled), colour::faint, -1);
        x = canvas.text (x + 1, cmdY, juce::String (juce::roundToInt (p * 100.0f)) + "%  " + b->file().getFileName() + L" \x00b7 esc cancels ",
                         colour::text, -1, false, std::max (0, w - x - 4));
    }

    {
        if (app.namingMarker.isValid())
            x = canvas.text (x, cmdY, " name marker " + app.namingMarker[ids::name].toString() + " ", 231, colour::marker, true);

        x = canvas.text (x + 1, cmdY, ": ", colour::accent, -1, true);
        x = canvas.text (x, cmdY, app.commandLine, colour::text, -1, false, w - 4);
        canvas.put (x, cmdY, L' ', -1, colour::text);

        if (app.tappingTempo())
            canvas.text (x + 2, cmdY, app.commandLine.endsWith (" ") ? L"t taps the tempo \x00b7 or type it" : L"t taps \x00b7 enter sets it",
                         colour::dim, -1, false, std::max (0, w - x - 3));

        // With an empty line, say what commands will act on.
        if (app.commandLine.isEmpty() && app.session != nullptr && ! app.namingMarker.isValid())
        {
            juce::StringArray parts;
            juce::String what = "selected";

            if (app.hasTimeSelection())
            {
                const auto sel = app.timeSelection();
                const auto& s = *app.session;
                parts.add (app.formatBars (sel.getStart()) + L" \x2192 " + app.formatBars (sel.getEnd()));

                // How long: bars and beats on the grid, and always seconds.
                // A sample of slack: a range in beats (a MIDI clip) rounds to whole samples at each end.
                const auto beats = (double) sel.getLength() / s.samplesPerBeat();
                const auto bars = (int) std::floor ((beats + 1.0 / s.samplesPerBeat()) / s.beatsPerBar());
                const auto restBeats = std::max (0.0, beats - bars * s.beatsPerBar());
                juce::StringArray length;

                if (s.gridOn())
                {
                    if (bars > 0)
                        length.add (juce::String (bars) + (bars == 1 ? " bar" : " bars"));

                    if (restBeats > 0.005 || bars == 0)
                        length.add (juce::String (restBeats, std::abs (restBeats - std::round (restBeats)) < 0.005 ? 0 : 2)
                                    + (std::abs (restBeats - 1.0) < 0.005 ? " beat" : " beats"));
                }

                length.add (juce::String ((double) sel.getLength() / app.sampleRate(), 2) + " s");
                parts.add (length.joinIntoString (" "));

                // A selection that is exactly one clip (tab, shift+tab) names it.
                if (const auto targets = app.defaultTargets(); targets.size() == 1)
                    for (const auto& c : Session::clips (s.track (targets.front())))
                        if ((juce::int64) c[ids::start] == sel.getStart() && (juce::int64) c[ids::start] + (juce::int64) c[ids::length] == sel.getEnd())
                            what = "clip " + c[ids::file].toString().replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false)
                                                 .upToLastOccurrenceOf (".", false, false) + " selected";

                // From one marker to the next (tab, shift+tab on the markers row): a section.
                if (app.topLane == App::TopLane::markers)
                {
                    juce::String from, to;

                    for (const auto& m : s.tree())
                        if (m.hasType (ids::MARKER))
                        {
                            if ((juce::int64) m[ids::position] == sel.getStart()) from = m[ids::name].toString();
                            if ((juce::int64) m[ids::position] == sel.getEnd())   to = m[ids::name].toString();
                        }

                    if (from.isNotEmpty())
                        what = "marker " + from + (to.isNotEmpty() ? L" \x2192 " + to : juce::String()) + " selected";
                }
            }

            if (app.onTopLane())
                parts.add ("all tracks");
            else if (const auto sel = app.selectedTracks(); ! sel.empty())
                parts.add (sel.size() == 1 ? "track " + juce::String (sel.front() + 1)
                                           : "tracks " + juce::String (sel.front() + 1) + "-" + juce::String (sel.back() + 1));

            if (! parts.isEmpty())
                canvas.text (x + 2, cmdY, what + ": " + parts.joinIntoString (juce::String (L" \x00b7 ")) + L" \x00b7 esc clears", colour::dim);
        }
    }

    juce::String hints;

    if (app.liveKeys.on())
        hints = L"KEYS  a w s e d f t g y h u j k o l p ; ' play " + app.liveKeys.range() + L" \x00b7 z x octave \x00b7 c v velocity "
                + juce::String (app.liveKeys.velocity()) + L" \x00b7 space play \x00b7 ctrl+space record \x00b7 esc or ctrl+k leaves";
    else if (app.recording)
        hints = L"space stop \x00b7 ctrl+space punch out \x00b7 ~ marker \x00b7 commands work: 2m 3s c \x00b7 [ ] volume";
    else if (rolling)
        hints = app.hasTimeSelection() ? L"ctrl c copy \x00b7 x cut \x00b7 d duplicate \x00b7 ctrl+v pastes at the cursor \x00b7 l loop \x00b7 shift \x2190\x2192 extend \x00b7 space stop"
                                       : L"space stop \x00b7 type commands (2m 3s l c) + enter \x00b7 enter alone returns \x00b7 ctrl+space punch in \x00b7 ~ marker \x00b7 [ ] volume \x00b7 \x2190\x2192 cursor, shift selects \x00b7 @17 seeks";
    else if (app.topLane == App::TopLane::video)
        hints = L"VIDEO  \x2190\x2192 a frame (hold to scrub) \x00b7 ctrl a second \x00b7 alt a beat \x00b7 shift selects \x00b7 tab its start / end \x00b7 click or drag scrubs \x00b7 bv bounces with picture \x00b7 video at moves it";
    else if (app.hasTimeSelection())
        hints = L"ctrl c copy \x00b7 x cut \x00b7 d duplicate \x00b7 del clear \x00b7 alt+shift+3 consolidate \x00b7 l loop \x00b7 b bounce (b mp3) \x00b7 pat x... fills it \x00b7 esc clears \x00b7 shift \x2190\x2192 extend";
    else if (app.ioCell >= 0 && app.session != nullptr && app.mixFocused())
        hints = L"MIX INSERTS (after the mix volume) \x00b7 \x2190\x2192 A-E \x00b7 | opens (empty: pick an effect) \x00b7 M bypass \x00b7 shift+\x2190\x2192 moves \x00b7 del removes \x00b7 esc or \\ leaves";
    else if (app.ioCell >= 0 && app.session != nullptr && ! app.onTopLane())
        hints = app.ioCell < App::ioFirstSend
                    ? L"\x2190\x2192 inserts, sends, output \x00b7 \x2191\x2193 rows \x00b7 | opens (empty: pick an effect) \x00b7 M bypass \x00b7 shift+\x2190\x2192 moves \x00b7 del removes \x00b7 esc or \\ leaves"
                : app.ioCell == App::ioOutput
                    ? L"\x2190\x2192 inserts, sends, output \x00b7 \x2191\x2193 rows \x00b7 | picks a bus \x00b7 del: back to the mix \x00b7 [ ] track volume \x00b7 esc or \\ leaves"
                    : L"\x2190\x2192 inserts, sends, output \x00b7 \x2191\x2193 rows \x00b7 | picks a bus \x00b7 [ ] send level \x00b7 M off \x00b7 P pre/post \x00b7 del removes \x00b7 esc or \\ leaves";
    else if (app.session != nullptr && ! app.mixFocused() && ! app.onTopLane() && Session::isBusTrack (app.session->track (app.focused)))
        hints = L"a bus plays what's sent to it \x00b7 2 send <name> or 2 out <name> feeds it \x00b7 \\ routes \x00b7 [ ] its volume \x00b7 shift+S solo (keeps what feeds it) \x00b7 ? help";
    else if (app.session != nullptr && ! app.mixFocused() && ! app.onTopLane() && Session::isInstrumentTrack (app.session->track (app.focused)))
        hints = L"shift+R arms (your controller plays it) \x00b7 ctrl+space records \x00b7 | plugin window \x00b7 f piano roll \x00b7 snap quantizes \x00b7 inst swaps the plugin \x00b7 ? help";
    else if (app.session != nullptr && ! app.mixFocused() && Session::isSampleTrack (app.session->track (app.focused)))
        hints = L"shift+R arms \x00b7 . plays armed (shift: accent), ctrl+space records it \x00b7 snap quantizes \x00b7 alt+/ toggles a hit \x00b7 pat x...x... \x00b7 ? help";
    else
        hints = L"enter run (empty: return) \x00b7 space play \x00b7 ctrl+space record \x00b7 shift+R arms \x00b7 r t zoom \x00b7 \x2190\x2192 bar \x00b7 ctrl 4 bars \x00b7 alt beat \x00b7 shift select \x00b7 [ ] vol ({ }: mix) \x00b7 add \x00b7 import / drop files \x00b7 ? help";

    canvas.text (1, hintY, hints, colour::dim, -1, false, w - 2);
}

void Ui::drawHelp (App& app)
{
    // Pages of two columns of "keys  what it does", so it fits a small screen (80 x 24). The
    // tabs run along the top; headings span a column, notes span the box.
    enum Kind { Pair, Heading, Note };
    struct Row { Kind kind; const wchar_t *k1, *d1, *k2, *d2; };
    struct Page { const wchar_t* name; std::vector<Row> rows; };

    static const std::vector<Page> pages = {
        { L"BASICS", {
            { Heading, L"MOVE", nullptr, L"PLAY & RECORD", nullptr },
            { Pair, L"\x2190 \x2192", L"next grid line (zoom)", L"space", L"play / stop" },
            { Pair, L"ctrl \x2190 \x2192", L"4 bars", L"shift+R  1r", L"arm (+ hear input)" },
            { Pair, L"alt \x2190 \x2192", L"1 step (ctrl+alt: 4)", L"ctrl+space", L"record armed tracks" },
            { Pair, L"shift + any", L"select time / tracks", L"ctrl+space", L"...playing: punch in/out" },
            { Pair, L"home / end", L"start / end", L"shift+I 2mon", L"input on playback too" },
            { Pair, L"\x2191 \x2193", L"track (ctrl: first/last)", L"~  `", L"add marker / markers row" },
            { Pair, L"@17  @1:30", L"bar 17 / 1 min 30 s", L"commands", L"work while playing too" },
            { Pair, L"r t   enter", L"zoom out, in / return", L"l", L"loop selection / toggle" },
            { Pair, L"q w", L"go to sel. start / end", L"tab", L"next clip edge / hit" },
            { Pair, L"ctrl+s / w", L"save / close session", L"ctrl+q  exit", L"save and quit" },
            { Note, L"esc clears the line, then the selection \x00b7 alt+C clears clip lights" },
        } },
        { L"TRACKS", {
            { Heading, L"TRACKS", nullptr, L"VOLUME", nullptr },
            { Note, L"Numbers first, spaces optional: 2m, 1-3m, 2i3, 2v-6. No number = selection or \x25b8 track." },
            { Pair, L"add vox in2", L"new track on input 2", L"[  ]", L"track -1 / +1 dB" },
            { Pair, L"shift+M S", L"mute / solo  (2m 2s)", L"{ } ctrl [ ]", L"mix -1 / +1 dB" },
            { Pair, L"1 rec", L"arm 1 and record", L"2v-6", L"track volume to -6 dB" },
            { Pair, L"2i3", L"use input 3", L"mixv-3", L"mix volume to -3 dB" },
            { Pair, L"2n Vox", L"rename", L"2x", L"drop latest clip" },
            { Pair, L"alt \x2191 \x2193", L"move tracks up / down", L"2 remove", L"delete track 2 (undo ok)" },
            { Pair, L"shift+T", L"track menu", L"2 stereo", L"stereo (inputs 3-4)" },
            { Pair, L"2 pan l30", L"pan (alt+[ ] nudges)", L"height 3", L"rows per track (1-4)" },
        } },
        { L"EDIT", {
            { Heading, L"EDIT", nullptr, L"SAMPLES", nullptr },
            { Pair, L"ctrl c x v", L"copy / cut / paste", L"sample f", L"new sample track" },
            { Pair, L"ctrl d", L"duplicate after itself", L".  >", L"play armed (> accent)" },
            { Pair, L"delete", L"clear the range", L"3 sample", L"convert (3 audio: back)" },
            { Pair, L"ctrl z / y", L"undo / redo", L"3 pat x...", L"pattern: X x o ." },
            { Pair, L"sel 3 5", L"select bars 3 to 5", L"grid 16", L"steps of a 16th" },
            { Pair, L"sel clip", L"select the clip here", L"snap 50", L"quantize hits (50%)" },
            { Pair, L"alt+shift+3", L"consolidate (cons)", L"alt+/", L"toggle a hit at cursor" },
            { Note, L"Automation goes with the clips: copy, cut, paste and duplicate carry it." },
        } },
        { L"PLUGINS", {
            { Heading, L"INSTRUMENTS", nullptr, L"EFFECTS & ROUTING", nullptr },
            { Pair, L"inst juno", L"instrument (alone: list)", L"ins  ins cla", L"effect picker / by name" },
            { Pair, L"shift+R  |", L"arm MIDI / plugin window", L"2 fx b  b x", L"its window / remove it" },
            { Pair, L"f  shift+E", L"piano roll / editor", L"2 fx b off", L"bypass (on); b>d swaps" },
            { Pair, L"scan  midi", L"find plugins / MIDI ins", L"\\   io", L"routing column / hide" },
            { Pair, L"ctrl+K", L"keys mode: type to play", L"bus Verb", L"new bus track" },
            { Pair, L"ctrl+\\", L"close plugin windows", L"2 send verb", L"send (new name: new bus)" },
            { Pair, L"lowlat", L"latent plugins sit out", L"2 send b -6", L"level; pre post off on x" },
            { Pair, L"lowlat 256", L"...over 256 samples", L"2 out drums", L"output to a bus" },
            { Pair, L"mix fx lim", L"an effect on the mix bus", L"\\ on Mix", L"its inserts (post-fader)" },
        } },
        { L"AUTOMATION", {
            { Heading, L"LANES", nullptr, L"KNOBS (MIDI CC)", nullptr },
            { Pair, L"shift+A", L"lanes (on Mix: the mix)", L"map", L"next knob moved runs it" },
            { Pair, L"auto", L"its parameters (adds)", L"map 74", L"...that controller" },
            { Pair, L"lane cutoff", L"a lane by name", L"unmap", L"all knobs off (74: one)" },
            { Pair, L"| on a lane", L"change what it shows", L"unmap lane", L"the knob on this lane" },
            { Pair, L"pt -6", L"a point (sel: a level)", L"shift+W  aw", L"write: record knobs" },
            { Pair, L"alt+\x2191\x2193", L"move points up / down", L"shift+O", L"lane off / on" },
            { Pair, L"del", L"remove selected points", L"", L"off: setting plays" },
            { Note, L"Points follow the tempo. A knob on a lane with no points is a fader: where it's left stays." },
        } },
        { L"SESSION", {
            { Heading, L"SESSION", nullptr, L"BOUNCE", nullptr },
            { Pair, L"click", L"click on / off", L"b  b mp3", L"bounce the selection" },
            { Pair, L"click rec", L"click only recording", L"bounces", L"open the bounce folder" },
            { Pair, L"count 4", L"count-in clicks (off)", L"bounces off", L"...don't open after" },
            { Pair, L"count always", L"...on play too", L"bv", L"bounce with video (mp4)" },
            { Pair, L"bpm 92", L"tempo  (grid: on/off)", L"bv cue 3", L"named (no sel: all)" },
            { Pair, L"dev  panel", L"audio device / driver", L"", L"" },
            { Pair, L"dark  light", L"themes (also: grey)", L"", L"" },
            { Pair, L"project", L"the session's settings", L"", L"" },
#if JUCE_WINDOWS
            { Note, L"b wav D:\\x bounces to a folder (remembered)." },
#else
            { Note, L"b wav ~/x bounces to a folder (remembered)." },
#endif
        } },
        { L"FILES", {
            { Heading, L"FILES & SAMPLES", nullptr, L"VIDEO", nullptr },
            { Pair, L"import f", L"track per file/folder", L"video f", L"picture window (or drop)" },
            { Pair, L"2 import f", L"onto track 2 (or drop)", L"video", L"show / hide it" },
            { Pair, L"ctrl+B", L"sample browser / hide", L"video at 5", L"move its start (-0:05)" },
            { Pair, L"index f", L"add a sample folder", L"video offset", L"2: picture 2 frames late" },
            { Pair, L"library", L"folders and samples", L"\x2191 video row", L"\x2190\x2192 frame, ctrl second" },
            { Pair, L"index remove", L"f: take a folder out", L"click / drag", L"on it: scrubs picture" },
            { Pair, L"import x.als", L"Live set: a new session", L"", L"" },
        } },
    };

    static_assert (App::helpPages == 7, "one page per tab");
    const auto& page = pages[(size_t) juce::jlimit (0, (int) pages.size() - 1, app.helpPage)];

    // The box is as tall as the tallest page, so turning pages doesn't make it jump.
    size_t tallest = 0;

    for (const auto& p : pages)
        tallest = std::max (tallest, p.rows.size());

    constexpr int keyW = 13, colW = 38; // key column width, width of each half
    const int w = canvas.width(), h = canvas.height();
    const int boxW = std::min (w - 4, 2 * colW + 4), boxH = std::min (h - 2, (int) tallest + 5);
    const int x0 = (w - boxW) / 2, y0 = 1, bg = colour::headerBg;

    for (int y = 0; y < boxH; ++y)
        canvas.fill (x0, y0 + y, boxW, L' ', -1, bg);

    // Tabs: the page you're on lit.
    int x = x0 + 2;

    for (int i = 0; i < (int) pages.size(); ++i)
    {
        const bool on = i == app.helpPage;
        const auto label = juce::String (" ") + pages[(size_t) i].name + " ";

        if (x + label.length() > x0 + boxW - 2)
            break;

        x = canvas.text (x, y0, label, on ? colour::text : colour::dim, on ? colour::selBg : bg, on) + 1;
    }

    auto pair = [&] (int px, int y, const wchar_t* k, const wchar_t* d)
    {
        if (k == nullptr)
            return;

        canvas.text (px, y, k, colour::accent, bg, false, keyW - 1);
        canvas.text (px + keyW, y, d, colour::text, bg, false, colW - keyW - 1);
    };

    for (int i = 0; i < (int) page.rows.size() && i < boxH - 4; ++i)
    {
        const auto& r = page.rows[(size_t) i];
        const int y = y0 + 2 + i, left = x0 + 2, right = left + colW;

        switch (r.kind)
        {
            case Heading:
                canvas.text (left, y, r.k1, colour::text, bg, true);
                if (r.k2 != nullptr)
                    canvas.text (right, y, r.k2, colour::text, bg, true);
                break;
            case Note:
                canvas.text (left, y, r.k1, colour::dim, bg, false, boxW - 4);
                break;
            case Pair:
                pair (left, y, r.k1, r.d1);
                pair (right, y, r.k2, r.d2);
                break;
        }
    }

    canvas.text (x0 + 2, y0 + boxH - 1, L"\x2190\x2192 or tab: pages \x00b7 1-7 jumps \x00b7 ? or esc closes", colour::faint, bg, false, boxW - 4);
}

// "project": the session's settings, in a box like help. The chosen row is lit; its value
// changes with left/right, or enter starts its command on the line below.
void Ui::drawSettings (App& app)
{
    const auto settings = app.projectSettings();
    const int w = canvas.width(), h = canvas.height();
    const int boxW = std::min (w - 4, 80), boxH = std::min (h - 2, (int) settings.size() + 6);
    const int x0 = (w - boxW) / 2, y0 = 1, bg = colour::headerBg;
    constexpr int nameW = 26;

    for (int y = 0; y < boxH; ++y)
        canvas.fill (x0, y0 + y, boxW, L' ', -1, bg);

    canvas.text (x0 + 2, y0 + 1, "PROJECT SETTINGS", colour::text, bg, true);
    canvas.text (x0 + 19, y0 + 1, app.session->tree()[ids::name].toString(), colour::dim, bg, false, boxW - 21);

    for (int i = 0; i < (int) settings.size() && i + 3 < boxH - 2; ++i)
    {
        const auto& row = settings[(size_t) i];
        const int y = y0 + 3 + i;
        const bool chosen = i == app.settingsIndex;
        const int rowBg = chosen ? colour::selBg : bg;

        canvas.fill (x0 + 1, y, boxW - 2, L' ', -1, rowBg);
        if (chosen)
            canvas.put (x0 + 1, y, L'\x25b8', colour::accent, rowBg);

        canvas.text (x0 + 3, y, row.name, colour::text, rowBg, false, nameW);
        canvas.text (x0 + 3 + nameW, y, row.value, row.editable ? colour::accent : colour::dim, rowBg, row.editable, boxW - nameW - 5);
    }

    canvas.text (x0 + 2, y0 + boxH - 2,
                 L"\x2191\x2193 choose \x00b7 \x2190\x2192 change \x00b7 enter: type a value or flip it \x00b7 commands work too \x00b7 esc closes",
                 colour::dim, bg, false, boxW - 4);
}

void Ui::drawPluginPicker (App& app)
{
    const auto choices = app.pickerChoices();
    const int w = canvas.width(), h = canvas.height();
    const int boxW = std::min (w - 4, 80), boxH = std::min (h - 2, 22);
    const int x0 = (w - boxW) / 2, y0 = 1, bg = colour::headerBg;
    const int listRows = boxH - 7;

    for (int y = 0; y < boxH; ++y)
        canvas.fill (x0, y0 + y, boxW, L' ', -1, bg);

    const bool effects = app.pluginPickSlot >= 0;
    canvas.text (x0 + 2, y0 + 1, effects ? "EFFECTS" : "INSTRUMENTS", colour::text, bg, true);
    canvas.text (x0 + 15, y0 + 1, effects ? "insert " + Session::slotName (app.pluginPickSlot) + " on " + app.insertOwnerName (app.pluginPickTarget)
                                  : app.pluginPickTarget >= 0 ? "onto track " + juce::String (app.pluginPickTarget + 1) : juce::String ("a new track"),
                 colour::dim, bg);

    // The filter, typed straight in.
    const int fx = canvas.text (x0 + 2, y0 + 2, L"\x203a ", colour::accent, bg);
    canvas.text (canvas.text (fx, y0 + 2, app.pluginFilter, colour::text, bg, true), y0 + 2, L"\x2581", colour::accent, bg);

    if (app.plugins.isScanning())
        canvas.text (x0 + 2, y0 + 4, L"scanning plugins " + juce::String (juce::roundToInt (app.plugins.scanProgress() * 100.0f)) + L"% \x00b7 "
                                         + app.plugins.scanningName(), colour::meterYellow, bg, false, boxW - 4);
    else if (choices.empty())
        canvas.text (x0 + 2, y0 + 4, app.pluginFilter.isEmpty() ? (effects ? L"no effects found \x00b7 esc, then scan looks again" : L"no instruments found \x00b7 esc, then scan looks again")
                                                                : L"nothing matches \x00b7 backspace", colour::dim, bg);

    // Keep the chosen row in view.
    const int first = juce::jlimit (0, std::max (0, (int) choices.size() - listRows), app.pluginPickIndex - listRows / 2);

    for (int row = 0; row < listRows && first + row < (int) choices.size(); ++row)
    {
        const int i = first + row, y = y0 + 4 + row;
        const auto& d = choices[(size_t) i];
        const bool chosen = i == app.pluginPickIndex;
        const int rowBg = chosen ? colour::selBg : bg;

        canvas.fill (x0 + 1, y, boxW - 2, L' ', -1, rowBg);

        if (chosen)
            canvas.put (x0 + 1, y, L'\x25b8', colour::accent, rowBg);

        const int nameW = std::min (40, boxW - 20);
        canvas.text (x0 + 3, y, d.name, colour::text, rowBg, chosen, nameW);
        canvas.text (x0 + 4 + nameW, y, d.manufacturerName, colour::dim, rowBg, false, boxW - nameW - 6);
    }

    canvas.text (x0 + 2, y0 + boxH - 2, L"type to filter \x00b7 \x2191\x2193 choose \x00b7 enter loads \x00b7 esc closes",
                 colour::dim, bg, false, boxW - 4);
}

// Where the output or a send goes: the mix (outputs only), the buses it can feed without a loop,
// and a new bus named by what's typed.
void Ui::drawBusPicker (App& app)
{
    const auto choices = app.busChoices();
    const int w = canvas.width(), h = canvas.height();
    const int boxW = std::min (w - 4, 56), boxH = std::min (h - 2, std::max (10, (int) choices.size() + 7));
    const int x0 = (w - boxW) / 2, y0 = 1, bg = colour::headerBg;
    const int listRows = boxH - 7;

    for (int y = 0; y < boxH; ++y)
        canvas.fill (x0, y0 + y, boxW, L' ', -1, bg);

    const auto title = app.busPickCell == 0 ? juce::String ("OUTPUT") : "SEND " + Session::slotName (app.busPickCell - 1);
    canvas.text (x0 + 2, y0 + 1, title, colour::text, bg, true);
    canvas.text (x0 + 3 + title.length(), y0 + 1, "of track " + juce::String (app.busPickTrack + 1) + " "
                                                       + app.session->track (app.busPickTrack)[ids::name].toString(), colour::dim, bg, false, boxW - title.length() - 5);

    const int fx = canvas.text (x0 + 2, y0 + 2, L"\x203a ", colour::accent, bg);
    canvas.text (canvas.text (fx, y0 + 2, app.busFilter, colour::text, bg, true), y0 + 2, L"\x2581", colour::accent, bg);

    const int first = juce::jlimit (0, std::max (0, (int) choices.size() - listRows), app.busPickIndex - listRows / 2);

    for (int row = 0; row < listRows && first + row < (int) choices.size(); ++row)
    {
        const int i = first + row, y = y0 + 4 + row;
        const auto& c = choices[(size_t) i];
        const bool chosen = i == app.busPickIndex;
        const int rowBg = chosen ? colour::selBg : bg;

        canvas.fill (x0 + 1, y, boxW - 2, L' ', -1, rowBg);

        if (chosen)
            canvas.put (x0 + 1, y, L'\x25b8', colour::accent, rowBg);

        canvas.text (x0 + 3, y, c.label, c.busId < 0 ? colour::accent : colour::text, rowBg, chosen, boxW - 5);
    }

    canvas.text (x0 + 2, y0 + boxH - 2, L"type to filter or name a new bus \x00b7 \x2191\x2193 \x00b7 enter \x00b7 esc", colour::dim, bg, false, boxW - 4);
}

// The track menu: a letter, what it does, and the command it stands for.
void Ui::drawTrackMenu (App& app)
{
    const auto items = app.trackMenuItems();
    const int w = canvas.width(), h = canvas.height();
    const int boxW = std::min (w - 4, 56), boxH = std::min (h - 2, (int) items.size() + 6);
    const int x0 = (w - boxW) / 2, y0 = 1, bg = colour::headerBg;

    for (int y = 0; y < boxH; ++y)
        canvas.fill (x0, y0 + y, boxW, L' ', -1, bg);

    const auto& tracks = app.trackMenuTracks;
    const auto title = tracks.size() == 1 ? juce::String ("TRACK ") + juce::String (tracks.front() + 1) : juce::String ("TRACKS");
    const auto what = tracks.size() == 1 ? app.session->track (tracks.front())[ids::name].toString() : trackList (tracks).fromFirstOccurrenceOf (" ", false, false);
    canvas.text (x0 + 2, y0 + 1, title, colour::text, bg, true);
    canvas.text (x0 + 3 + title.length(), y0 + 1, what, colour::dim, bg, false, boxW - title.length() - 5);

    for (int i = 0; i < (int) items.size() && i < boxH - 5; ++i)
    {
        const auto& item = items[(size_t) i];
        const int y = y0 + 3 + i;
        const bool chosen = i == app.trackMenuIndex;
        const int rowBg = chosen ? colour::selBg : bg;

        canvas.fill (x0 + 1, y, boxW - 2, L' ', -1, rowBg);

        if (chosen)
            canvas.put (x0 + 1, y, L'\x25b8', colour::accent, rowBg);

        canvas.put (x0 + 3, y, (wchar_t) item.key, colour::accent, rowBg, true);
        canvas.text (x0 + 5, y, item.label, item.danger ? colour::error : colour::text, rowBg, chosen, 22);
        canvas.text (x0 + 28, y, item.typed, colour::dim, rowBg, false, boxW - 30);
    }

    canvas.text (x0 + 2, y0 + boxH - 2, L"a letter, or \x2191\x2193 enter \x00b7 esc", colour::dim, bg, false, boxW - 4);
}

// The automation list: a track's parameters by section (what's automated, the mixer, each
// plugin), each with its value at the cursor, its points and its controller; the filter typed
// so far on top. Scrolls to keep the chosen row in view.
void Ui::drawAutoPanel (App& app)
{
    const auto& rows = app.autoPanelRows;
    const int w = canvas.width(), h = canvas.height();
    const int boxW = std::min (w - 4, 78), boxH = std::max (8, h - 2);
    const int x0 = (w - boxW) / 2, y0 = 1, bg = colour::headerBg;
    const int listTop = y0 + 4, listRows = boxH - 7;

    for (int y = 0; y < boxH; ++y)
        canvas.fill (x0, y0 + y, boxW, L' ', -1, bg);

    if (app.autoPanelTrack < -1 || app.autoPanelTrack >= app.session->numTracks())
        return;

    const auto track = app.automationOwner (app.autoPanelTrack); // -1: the mix
    const auto shownLanes = app.lanesShown (app.autoPanelTrack);
    int x = canvas.text (x0 + 2, y0 + 1, "AUTOMATION", colour::text, bg, true);
    const auto laneNow = app.autoPanelLane >= 0 && app.autoPanelLane < (int) shownLanes.size() ? shownLanes[(size_t) app.autoPanelLane] : juce::String();
    canvas.text (x + 1, y0 + 1, (app.autoPanelTrack < 0 ? juce::String ("the mix") : "track " + juce::String (app.autoPanelTrack + 1) + " " + track[ids::name].toString())
                                    + (laneNow.isNotEmpty() ? L" \x00b7 lane " + juce::String (app.autoPanelLane + 1) + " shows " + app.automationName (track, laneNow, false) : juce::String()),
                 colour::dim, bg, false, boxW - 16);

    // The filter, like a command line.
    x = canvas.text (x0 + 2, y0 + 2, "find: ", colour::dim, bg);
    x = canvas.text (x, y0 + 2, app.autoPanelFilter, colour::text, bg);
    canvas.put (x, y0 + 2, L' ', -1, colour::text);

    if (rows.empty())
        canvas.text (x0 + 3, listTop, app.autoPanelFilter.isEmpty() ? juce::String ("nothing to automate") : "nothing matches " + app.autoPanelFilter, colour::dim, bg);

    // Keep the chosen row in view, with its section's header when there's room.
    static int scroll = 0;
    scroll = juce::jlimit (0, juce::jmax (0, (int) rows.size() - listRows), scroll);

    if (app.autoPanelIndex < scroll + 1)
        scroll = juce::jmax (0, app.autoPanelIndex - 1);
    if (app.autoPanelIndex >= scroll + listRows)
        scroll = app.autoPanelIndex - listRows + 1;

    const auto at = app.engine.isRolling() ? app.heardPlayhead() : app.cursor;

    for (int r = 0; r < listRows && scroll + r < (int) rows.size(); ++r)
    {
        const auto& row = rows[(size_t) (scroll + r)];
        const int y = listTop + r;

        if (row.header)
        {
            canvas.text (x0 + 2, y, row.name, colour::accent, bg, true, boxW - 4);
            continue;
        }

        const bool chosen = scroll + r == app.autoPanelIndex;
        const int rowBg = chosen ? colour::selBg : bg;
        canvas.fill (x0 + 1, y, boxW - 2, L' ', -1, rowBg);

        if (chosen)
            canvas.put (x0 + 1, y, L'\x25b8', colour::accent, rowBg);

        // A lane open under the track: a dot.
        if (std::find (shownLanes.begin(), shownLanes.end(), row.param) != shownLanes.end())
            canvas.put (x0 + 3, y, L'\x25cf', colour::accent, rowBg);

        canvas.text (x0 + 5, y, row.name, colour::text, rowBg, chosen, 30);

        const auto lane = Session::automation (track, row.param);
        const auto points = lane.isValid() ? (int) Session::points (lane).size() : 0;
        const auto value = app.automationText (track, row.param, app.automationValue (track, row.param, at), false);
        canvas.text (x0 + 36, y, value, colour::dim, rowBg, false, 16);

        if (points > 0)
            canvas.text (x0 + 53, y, juce::String (points) + (points == 1 ? " pt" : " pts"), colour::note, rowBg, false, 9);

        if (lane.hasProperty (ids::num))
            canvas.text (x0 + 63, y, app.controlText (lane), colour::accent, rowBg, false, boxW - 65);
    }

    if ((int) rows.size() > listRows)
        canvas.text (x0 + boxW - 12, y0 + 1, juce::String (scroll + 1) + "-" + juce::String (juce::jmin ((int) rows.size(), scroll + listRows)) + "/"
                                                 + juce::String ((int) rows.size()), colour::faint, bg);

    canvas.text (x0 + 2, y0 + boxH - 2, laneNow.isNotEmpty() ? juce::String (L"type to find \x00b7 \x2191\x2193 pgup pgdn \x00b7 enter: the lane shows it \x00b7 del removes a lane \x00b7 esc")
                                                             : juce::String (L"type to find \x00b7 \x2191\x2193 pgup pgdn \x00b7 enter: a lane for it under the track \x00b7 del removes a lane \x00b7 esc"),
                 colour::dim, bg, false, boxW - 4);
}

// Every MIDI input: on or off, whether it opened, and what it last sent (lit while it's sending).
// The docked sample browser, a column right of the timeline from the transport row down to the
// input line. The map on top; everything you set at the bottom, like taw's own input line: the
// chosen sample, the axes, the kinds as badges (wrapping), the qualities the filter can use (with
// their range over what's shown), the hints, and the filter's input line last. Laid out from the
// bottom up.
void Ui::drawBrowser (App& app)
{
    auto& g = app.browserGeometry;
    const int h = canvas.height(), left = canvas.width() - dockCols, top = 2, rows = h - 4 - top;
    const int x0 = left + 2, cw = std::max (10, canvas.width() - x0 - 1); // content: after the border
    const bool active = app.browserActive;
    g = {};
    g.left = left;
    g.top = top;
    g.rows = rows;
    g.valid = true;

    for (int y = top; y < top + rows; ++y)
    {
        canvas.fill (left, y, dockCols, L' ');
        canvas.put (left, y, L'\x2502', active ? colour::dim : colour::faint);
    }

    // Header.
    int x = canvas.text (x0, top, "SAMPLES ", active ? colour::accent : colour::dim, -1, true);
    const auto* lib = app.library.get();
    const int total = lib != nullptr ? (int) lib->samples().size() : 0;
    x = canvas.text (x, top, juce::String (total) + L" \x00b7 audition " + formatDb (app.auditionDb) + " dB", colour::dim);

    if (app.isIndexing())
        canvas.text (x, top, L" \x00b7 indexing " + juce::String (app.indexDone.load()) + " / " + juce::String (app.indexTotal.load()), colour::meterYellow);

    // The bottom two rows: the hints, then the filter's input line.
    const int inputY = top + rows - 1, hintY = inputY - 1;
    canvas.text (x0, hintY, active ? L"alt+arrows axes \x00b7 ctrl jump \x00b7 enter on track \x00b7 shift+enter new track \x00b7 esc"
                                   : L"ctrl+B browse (again: hide)", colour::faint, -1, false, cw);

    if (lib == nullptr || total == 0)
    {
        canvas.text (x0, top + 2, app.isIndexing() ? juce::String (L"measuring samples\x2026") : juce::String (L"no samples yet \x2014 type"), colour::dim, -1, false, cw);
#if JUCE_WINDOWS
        canvas.text (x0 + 2, top + 3, L"index D:\\Samples\\Drums", colour::text, -1, false, cw - 2);
#else
        canvas.text (x0 + 2, top + 3, L"index ~/Samples/Drums", colour::text, -1, false, cw - 2);
#endif
        canvas.text (x0, top + 4, "or type index and drop a folder (it's only read)", colour::faint, -1, false, cw);
        return;
    }

    const auto shown = app.browserShown();

    // The filter's input line.
    x = canvas.text (x0, inputY, L"\x203a ", colour::accent);

    if (app.browserFilter.isEmpty())
        x = canvas.text (x, inputY, active ? L"\x2581 type a name or dec<200 bright>1k \x2026" : L"type a name or dec<200 bright>1k \x2026", colour::faint);
    else
    {
        x = canvas.text (x, inputY, app.browserFilter, colour::text, -1, true);

        if (active)
            x = canvas.text (x, inputY, L"\x2581", colour::accent);
    }

    const auto count = juce::String ((int) shown.size()) + " of " + juce::String (total);
    canvas.text (std::max (x + 2, x0 + cw - count.length()), inputY, count, colour::dim);

    // The selectors: what the axes show (alt+arrows step them).
    int axesY = 0; // set below, once the rows under the axes are counted
    auto selector = [&] (int row, const juce::String& label, const juce::String& value, App::BrowserFocus which, const juce::String& note, bool off)
    {
        const bool focus = active && app.browserFocus == which;
        const int y0 = axesY + row;
        g.axisRow[row] = y0;
        int sx = canvas.text (x0, y0, label.paddedRight (' ', 8), focus ? colour::accent : colour::dim, -1, focus);

        if (off)
        {
            canvas.text (sx + 2, y0, value, colour::faint);
            return;
        }

        sx = canvas.text (sx, y0, L"\x2039 ", focus ? colour::accent : colour::faint);
        sx = canvas.text (sx, y0, value, colour::text, focus ? colour::selBg : -1, true);
        sx = canvas.text (sx, y0, L" \x203a", focus ? colour::accent : colour::faint);
        canvas.text (sx + 2, y0, note, colour::faint, -1, false, x0 + cw - sx - 2);
    };

    // Badges, wrapping: the chosen one filled in its colour.
    const auto kinds = app.browserKinds();
    const int chosenKind = juce::jlimit (0, (int) kinds.size() - 1, app.browserKind);
    std::map<juce::String, int> counts;

    for (const auto& s : lib->samples())
        ++counts[s.kind().isEmpty() ? juce::String ("other") : s.kind()];

    auto kindName = [] (const SampleInfo& s) { return s.kind().isEmpty() ? juce::String ("other") : s.kind(); };

    // Where each badge goes (its row counted from the first), to know how many rows they need.
    struct BadgeAt { int x, row; juce::String label; };
    std::vector<BadgeAt> badgeAt;
    int badgeRows = 1;
    x = x0;

    for (const auto& k : kinds)
    {
        const auto label = " " + k + " " + juce::String (k == "all" ? total : counts[k]) + " ";

        if (x > x0 && x + label.length() > x0 + cw)
            x = x0, ++badgeRows;

        badgeAt.push_back ({ x, badgeRows - 1, label });
        x += label.length() + 1;
    }

    // The qualities the filter can use, each with its range over what's shown; the ones in the
    // filter now are lit. They wrap above the hints.
    struct Quality { const char* typed; const char* measure; int unit; }; // 0 Hz, 1 time, 2 dB, 3 plain, 4 count
    static const Quality qualities[] = { { "bright", "brightness", 0 }, { "att", "attack", 1 }, { "dec", "decay", 1 }, { "tail", "tail", 1 },
                                         { "len", "length", 1 }, { "pitch", "pitch", 0 }, { "loud", "loudness", 2 }, { "punch", "punch", 2 },
                                         { "noise", "noise", 3 }, { "low", "lowend", 2 }, { "hits", "hits", 4 } };
    auto compact = [] (double v, int unit) -> juce::String
    {
        switch (unit)
        {
            case 0:  return v >= 1000.0 ? juce::String (v / 1000.0, 1) + "k" : juce::String (juce::roundToInt (v));
            case 1:  return v >= 1000.0 ? juce::String (v / 1000.0, 1) + "s" : juce::String (juce::roundToInt (v)) + "ms";
            case 2:  return juce::String (juce::roundToInt (v));
            case 3:  return juce::String (v, 2);
            default: return juce::String (juce::roundToInt (v));
        }
    };

    const auto filterLower = app.browserFilter.toLowerCase();
    auto inFilter = [&] (const Quality& q)
    {
        for (const auto& t : juce::StringArray::fromTokens (filterLower, " ", ""))
        {
            const auto key = t.upToFirstOccurrenceOf ("<", false, false).upToFirstOccurrenceOf (">", false, false).upToFirstOccurrenceOf ("=", false, false);

            if (key != t && (key == q.typed || key == q.measure))
                return true;
        }

        return false;
    };

    struct Piece { juce::String text; bool lit; };
    std::vector<Piece> pieces;

    for (const auto& q : qualities)
    {
        double lo = 1.0e300, hi = -1.0e300;

        for (auto i : shown)
        {
            const auto v = Library::measure (lib->samples()[(size_t) i], q.measure);

            if (juce::String (q.measure) == "pitch" && v <= 0.0)
                continue; // unpitched

            lo = std::min (lo, v);
            hi = std::max (hi, v);
        }

        const auto units = q.unit == 0 ? juce::String ("Hz") : q.unit == 2 ? juce::String ("dB") : juce::String();
        pieces.push_back ({ juce::String (q.typed) + " " + (lo > hi ? juce::String (L"\x2014") : compact (lo, q.unit) + L"\x2026" + compact (hi, q.unit) + units),
                            inFilter (q) });
    }

    std::vector<std::vector<const Piece*>> qualityRows (1);
    int rowLength = 0;

    for (const auto& p : pieces)
    {
        if (rowLength > 0 && rowLength + 3 + p.text.length() > cw)
            qualityRows.push_back ({}), rowLength = 0;

        qualityRows.back().push_back (&p);
        rowLength += (rowLength > 0 ? 3 : 0) + p.text.length();
    }

    const int qualityY = hintY - (int) qualityRows.size();

    for (size_t r = 0; r < qualityRows.size(); ++r)
    {
        int qx = x0;

        for (size_t k = 0; k < qualityRows[r].size(); ++k)
        {
            if (k > 0)
                qx = canvas.text (qx, qualityY + (int) r, L" \x00b7 ", colour::faint);

            const auto* p = qualityRows[r][k];
            qx = canvas.text (qx, qualityY + (int) r, p->text, p->lit ? colour::accent : colour::dim, -1, p->lit);
        }
    }

    // Above the qualities: the badges, then the axes, then the chosen sample (2 rows) under the
    // map's x-axis rule; the map takes the rest, up to the header.
    const int badgesY = qualityY - badgeRows;
    axesY = badgesY - 2;
    const int infoY = axesY - 3, ruleY = infoY - 1, bodyY = top + 1;
    const int mapH = std::max (3, ruleY - bodyY);

    // A border between the map (and the chosen sample) and the settings and input line under it,
    // joined to the column's own border.
    canvas.put (left, axesY - 1, L'\x251c', active ? colour::dim : colour::faint);
    canvas.fill (left + 1, axesY - 1, canvas.width() - left - 1, L'\x2500', active ? colour::dim : colour::faint);

    for (int b = 0; b < (int) badgeAt.size(); ++b)
    {
        const auto& [bx, row, label] = badgeAt[(size_t) b];
        const auto& k = kinds[(size_t) b];
        const int c = k == "all" ? colour::text : colour::kind (k), by = badgesY + row;
        const bool chosen = b == chosenKind;

        if (chosen && active && app.browserFocus == App::BrowserFocus::kinds)
            canvas.put (bx - 1, by, L'\x25b8', colour::accent);

        g.badges.push_back ({ bx, by, label.length() });
        canvas.text (bx, by, label, chosen ? 16 : c, chosen ? c : -1, chosen, x0 + cw - bx);
    }

    const auto& axisNames = App::browserAxisNames();
    const bool similar = app.browserAxisX == 0;
    selector (0, similar ? juce::String ("map") : juce::String (L"x \x2192"), axisNames[app.browserAxisX], App::BrowserFocus::xAxis,
              similar ? juce::String ("close together = sounds alike") : app.browserAxisRange (false), false);
    selector (1, L"y \x2191", similar ? juce::String (L"\x2014") : axisNames[app.browserAxisY], App::BrowserFocus::yAxis,
              similar ? juce::String() : app.browserAxisRange (true), similar);


    canvas.fill (x0, ruleY, cw, L'\x2500', colour::faint);

    if (! similar)
    {
        const auto [low, high] = app.browserAxisEnds (false);
        const auto name = " " + axisNames[app.browserAxisX] + L" \x2192 ";
        canvas.text (x0, ruleY, low + " ", colour::text);
        canvas.text (x0 + (cw - name.length()) / 2, ruleY, name, colour::accent, -1, true);
        canvas.text (x0 + cw - high.length() - 1, ruleY, " " + high, colour::text);
    }

    // The map: half blocks, two square-ish pixels a cell (top and bottom), each in the colour
    // most of its samples have.
    const int mapX = x0, mapW = cw;
    g.mapX = mapX, g.mapY = bodyY, g.mapW = mapW, g.mapH = mapH;

    std::vector<std::map<juce::String, int>> pixelKinds ((size_t) (mapW * mapH * 2));
    auto dotOf = [&] (int i)
    {
        const auto [px, py] = app.browserPosition (i);
        return std::pair { juce::jlimit (0, mapW - 1, juce::roundToInt (px * (float) (mapW - 1))),
                           juce::jlimit (0, mapH * 2 - 1, juce::roundToInt (py * (float) (mapH * 2 - 1))) };
    };

    for (auto i : shown)
    {
        const auto [dx, dy] = dotOf (i);
        ++pixelKinds[(size_t) (dy * mapW + dx)][kindName (lib->samples()[(size_t) i])];
    }

    auto pixelColour = [&] (int px, int py)
    {
        const auto& k = pixelKinds[(size_t) (py * mapW + px)];

        if (k.empty())
            return -1;

        return colour::kind (std::max_element (k.begin(), k.end(), [] (const auto& a, const auto& b) { return a.second < b.second; })->first);
    };

    for (int cy = 0; cy < mapH; ++cy)
        for (int cx = 0; cx < mapW; ++cx)
        {
            const int upper = pixelColour (cx, cy * 2), lower = pixelColour (cx, cy * 2 + 1);

            if (upper >= 0)
                canvas.put (mapX + cx, bodyY + cy, L'\x2580', upper, lower);
            else if (lower >= 0)
                canvas.put (mapX + cx, bodyY + cy, L'\x2584', lower);
        }

    // The y axis in the map's left corners, on a plate so the dots don't hide it: its name and
    // high end at the top, its low end at the bottom.
    if (! similar)
    {
        const auto [low, high] = app.browserAxisEnds (true);
        int lx = canvas.text (mapX, bodyY, L"\x2191 " + axisNames[app.browserAxisY] + " ", colour::accent, colour::headerBg, true);
        canvas.text (lx, bodyY, high + " ", colour::text, colour::headerBg);
        canvas.text (mapX, bodyY + mapH - 1, "  " + low + " ", colour::text, colour::headerBg);
    }

    if (shown.empty())
        canvas.text (mapX + 2, bodyY + mapH / 2, app.browserFilter.isNotEmpty() ? L"nothing matches \x00b7 backspace" : L"none of these", colour::dim);

    // The chosen sample: a ring on the map with its name beside it; under the map, its file and
    // kind, then its numbers.
    const int at = (int) (std::find (shown.begin(), shown.end(), app.browserSample) - shown.begin());

    if (at < (int) shown.size())
    {
        const auto& s = lib->samples()[(size_t) app.browserSample];
        const auto kind = kindName (s);
        const auto [dx, dy] = dotOf (app.browserSample);
        const int cx = dx, cy = dy / 2;
        const bool mapFocus = active && app.browserFocus == App::BrowserFocus::map;
        canvas.put (mapX + cx, bodyY + cy, L'\x25c9', mapFocus ? 16 : colour::kind (kind), mapFocus ? colour::accent : colour::selBg, true);

        const auto name = juce::File (s.path).getFileNameWithoutExtension();
        const int nameX = cx + 2 + name.length() < mapW ? mapX + cx + 2 : std::max (mapX, mapX + cx - 1 - name.length());
        canvas.text (nameX, bodyY + cy, name, colour::text, -1, false, std::min (name.length(), mapX + mapW - nameX));

        canvas.put (x0, infoY, s.nameKind.isEmpty() ? L'\x25cb' : L'\x25cf', colour::kind (kind)); // hollow: kind by sound
        const int ix = canvas.text (x0 + 2, infoY, juce::File (s.path).getFileName(), colour::text, -1, true, cw - 2);
        canvas.text (ix, infoY, L" \x00b7 " + kind + (s.nameKind.isEmpty() ? " (by its sound)" : "") + L" \x00b7 "
                                    + juce::File (s.path).getParentDirectory().getFileName(), colour::dim, -1, false, x0 + cw - ix);

        juce::StringArray parts;
        parts.add (juce::String (s.seconds, 2) + " s");
        parts.add ("att " + juce::String (s.attackMs, 1) + " ms");
        parts.add ("dec " + juce::String (juce::roundToInt (s.decayMs)) + " ms");

        if (s.pitchConfidence >= 0.6 && s.pitchHz > 0.0)
        {
            const int note = juce::roundToInt (69.0 + 12.0 * std::log2 (s.pitchHz / 440.0));
            parts.add ("pitch " + juce::String (juce::roundToInt (s.pitchHz)) + " Hz " + juce::MidiMessage::getMidiNoteName (note, true, true, 4));
        }

        parts.add ("bright " + juce::String (juce::roundToInt (s.centroidHz)) + " Hz");
        parts.add ("loud " + formatDb (s.rmsDb) + " dB");
        canvas.text (x0 + 2, infoY + 1, parts.joinIntoString (juce::String (L" \x00b7 ")), colour::dim, -1, false, cw - 2);
    }
}

void Ui::drawMidiPanel (App& app)
{
    const auto devices = app.midiInputs->devices();
    const int w = canvas.width(), h = canvas.height();
    const int boxW = std::min (w - 4, 84), boxH = std::min (h - 2, (int) devices.size() + 7);
    const int x0 = (w - boxW) / 2, y0 = 1, bg = colour::headerBg;
    const int listRows = boxH - 6;
    const auto now = juce::Time::getMillisecondCounterHiRes();
    constexpr int nameW = 30;

    // When each input last sent something, to light its row.
    midiRowSeen.resize (devices.size());
    midiRowLitAt.resize (devices.size());

    for (size_t i = 0; i < devices.size(); ++i)
        if (devices[i].events != midiRowSeen[i])
        {
            midiRowSeen[i] = devices[i].events;
            midiRowLitAt[i] = now;
        }

    for (int y = 0; y < boxH; ++y)
        canvas.fill (x0, y0 + y, boxW, L' ', -1, bg);

    canvas.text (x0 + 2, y0 + 1, "MIDI INPUTS", colour::text, bg, true);
    canvas.text (x0 + 15, y0 + 1, L"armed instrument tracks hear every input that's on \x00b7 kept for all sessions", colour::dim, bg, false, boxW - 17);

    if (devices.empty())
        canvas.text (x0 + 2, y0 + 3, L"no MIDI inputs \x2014 plug a controller in, taw picks it up", colour::dim, bg);

    const int first = juce::jlimit (0, std::max (0, (int) devices.size() - listRows), app.midiPanelIndex - listRows / 2);

    for (int row = 0; row < listRows && first + row < (int) devices.size(); ++row)
    {
        const int i = first + row, y = y0 + 3 + row;
        const auto& d = devices[(size_t) i];
        const bool chosen = i == app.midiPanelIndex;
        const int rowBg = chosen ? colour::selBg : bg;
        const bool lit = d.events > 0 && now - midiRowLitAt[(size_t) i] < 300.0;

        canvas.fill (x0 + 1, y, boxW - 2, L' ', -1, rowBg);

        if (chosen)
            canvas.put (x0 + 1, y, L'\x25b8', colour::accent, rowBg);

        canvas.text (x0 + 3, y, d.on ? "on " : "off", d.on ? colour::accent : colour::faint, rowBg, d.on);
        canvas.put (x0 + 7, y, L'\x25cf', lit ? colour::note : colour::faint, rowBg);
        canvas.text (x0 + 9, y, d.name, d.on ? colour::text : colour::dim, rowBg, false, nameW);

        const auto status = ! d.on ? juce::String()
                            : ! d.open ? juce::String (L"won't open \x2014 another app has it")
                            : d.events == 0 ? juce::String ("open, nothing received yet")
                                            : describe (d.last) + L" \x00b7 " + juce::String (d.events);
        canvas.text (x0 + 10 + nameW, y, status, ! d.open && d.on ? colour::error : colour::dim, rowBg, false, boxW - nameW - 12);
    }

    canvas.text (x0 + 2, y0 + boxH - 2, L"\x2191\x2193 choose \x00b7 enter / space switches on or off (\x2190 off, \x2192 on) \x00b7 esc closes",
                 colour::dim, bg, false, boxW - 4);
}

//==============================================================================
void Ui::drawDevicePicker (App& app)
{
    const int w = canvas.width(), h = canvas.height();
    canvas.fill (0, 0, w, L' ', -1, colour::headerBg);
    canvas.text (1, 0, "taw", colour::accent, colour::headerBg, true);
    canvas.text (5, 0, L"\x25b8 audio device", colour::text, colour::headerBg);
#if JUCE_WINDOWS
    canvas.text (1, 2, "Choose the interface taw records and plays through. ASIO gives the lowest latency.", colour::dim);
#else
    canvas.text (1, 2, "Choose the interface taw records and plays through.", colour::dim);
#endif

    const int top = 4, visible = h - top - 4;
    const int first = std::max (0, std::min (app.pickIndex - visible / 2, (int) app.deviceChoices.size() - visible));

    if (app.deviceChoices.empty())
        canvas.text (3, top, L"no devices found \x00b7 plug in your interface and press r", colour::error);

    for (int row = 0; row < visible && first + row < (int) app.deviceChoices.size(); ++row)
    {
        const int i = first + row;
        const auto& c = app.deviceChoices[(size_t) i];
        const bool sel = i == app.pickIndex;
        const int bg = sel ? colour::focusBg : -1;

        canvas.fill (1, top + row, w - 2, L' ', -1, bg);
        if (sel)
            canvas.put (1, top + row, L'\x25b8', colour::accent, bg);

        canvas.text (3, top + row, juce::String (i + 1).paddedLeft (' ', 2), colour::dim, bg);
        canvas.text (7, top + row, padRight (c.type, 18), c.type == "ASIO" ? colour::accent : colour::dim, bg);
        canvas.text (26, top + row, c.output, colour::text, bg, sel);

        if (c.input != c.output && c.input.isNotEmpty())
            canvas.text (27 + c.output.length(), top + row, "(in: " + c.input + ")", colour::faint, bg);
    }

    drawBottom (app);
    canvas.fill (0, h - 2, w, L' ');
    canvas.fill (0, h - 1, w, L' ');
    canvas.text (1, h - 1, L"\x2191\x2193 select \x00b7 enter choose \x00b7 r rescan \x00b7 esc back", colour::dim);
}

// Opening or closing a session: what it's doing, and how far along.
void Ui::drawProgress (App& app, const juce::String& title, const juce::String& doing, size_t done, size_t total)
{
    const int w = canvas.width();
    const int boxW = std::min (w - 4, 64), x0 = (w - boxW) / 2, y0 = 3, bg = colour::headerBg;

    for (int y = 0; y < 6; ++y)
        canvas.fill (x0, y0 + y, boxW, L' ', -1, bg);

    canvas.text (x0 + 2, y0 + 1, title, colour::text, bg, true);
    canvas.text (x0 + 3 + title.length(), y0 + 1, app.session != nullptr ? app.session->name() : juce::String(), colour::accent, bg, true,
                 boxW - title.length() - 5);
    canvas.text (x0 + 2, y0 + 2, doing + L"\x2026", colour::dim, bg, false, boxW - 4);

    // A bar of steps done.
    const int barW = boxW - 12;
    const auto shown = juce::jmin (done, total);
    const int filled = total > 0 ? (int) ((double) barW * (double) shown / (double) total) : 0;

    for (int i = 0; i < barW; ++i)
        canvas.put (x0 + 2 + i, y0 + 4, i < filled ? L'\x2588' : L'\x2591', i < filled ? colour::accent : colour::faint, bg);

    canvas.text (x0 + 3 + barW, y0 + 4, juce::String ((int) shown) + "/" + juce::String ((int) total), colour::dim, bg);
}

// A plugin loading: taw can't draw again until it's in (plugins load on this thread), so this
// box is the last frame until then.
void Ui::drawPluginLoading (App& app)
{
    const int w = canvas.width();
    const int boxW = std::min (w - 4, 64), x0 = (w - boxW) / 2, y0 = 3, bg = colour::headerBg;

    for (int y = 0; y < 5; ++y)
        canvas.fill (x0, y0 + y, boxW, L' ', -1, bg);

    const auto title = app.loadingTitle.isNotEmpty() ? app.loadingTitle : juce::String ("LOADING");
    canvas.text (x0 + 2, y0 + 1, title, colour::text, bg, true);
    canvas.text (x0 + 3 + title.length(), y0 + 1, app.loadingPlugin, colour::accent, bg, true, boxW - title.length() - 5);
    canvas.text (x0 + 2, y0 + 3, app.loadingWhy, colour::dim, bg, false, boxW - 4);
}

void Ui::drawOpening (App& app)
{
    const auto& job = *app.opening;
    const auto done = juce::jmin (job.done, job.steps.size());
    const auto now = done < job.steps.size() ? job.steps[done].kind + " " + juce::File (job.steps[done].name).getFileName()
                                             : juce::String ("starting");
    drawProgress (app, "OPENING", "loading " + now, done, job.steps.size());
}

void Ui::drawClosing (App& app)
{
    const auto& job = *app.closing;
    drawProgress (app, job.thenQuit ? "SAVING AND QUITTING" : "CLOSING",
                  job.current.isNotEmpty() ? "saving " + job.current : juce::String ("saving the session"), job.done, job.total);
}

void Ui::drawTemplatePicker (App& app)
{
    const int w = canvas.width(), h = canvas.height();
    const int numRecent = (int) app.recent.size();
    canvas.fill (0, 0, w, L' ', -1, colour::headerBg);
    canvas.text (1, 0, "taw", colour::accent, colour::headerBg, true);
    canvas.text (5, 0, L"\x25b8 sessions", colour::text, colour::headerBg);

    // Which build, out of the way on the right: the version, the commit (+: changes on top of
    // it) and when it was built.
    const auto build = juce::String (TAW_VERSION) + " " + buildCommit() + " " + buildTime();
    canvas.text (std::max (16, w - 1 - build.length()), 0, build, colour::dim, colour::headerBg);

    int y = 2;

    // Recent sessions: name, how long ago, where.
    if (numRecent > 0)
    {
        canvas.text (1, y++, "recent", colour::dim);
        const auto now = juce::Time::getCurrentTime();
        const int maxRows = std::max (1, std::min (numRecent, h - 15));

        for (int i = 0; i < maxRows; ++i, ++y)
        {
            const auto& dir = app.recent[(size_t) i];
            const bool sel = i == app.pickIndex;
            const int bg = sel ? colour::focusBg : -1;

            canvas.fill (1, y, w - 2, L' ', -1, bg);
            if (sel)
                canvas.put (1, y, L'\x25b8', colour::accent, bg);

            const auto age = now - dir.getChildFile ("session.json").getLastModificationTime();
            const auto ago = age.inMinutes() < 60 ? juce::String ((int) age.inMinutes()) + " min ago"
                           : age.inHours() < 48   ? juce::String ((int) age.inHours()) + " h ago"
                                                  : juce::String ((int) age.inDays()) + " days ago";

            canvas.text (3, y, padRight (dir.getFileName(), 24), colour::text, bg, sel);
            canvas.text (28, y, padRight (ago, 12), colour::dim, bg);
            canvas.text (41, y, dir.getParentDirectory().getFullPathName(), colour::faint, bg, false, w - 43);
        }

        ++y;
    }

    canvas.text (1, y++, "new session", colour::dim);
    canvas.text (3, y, "name", colour::dim);

    const auto x = canvas.text (9, y, app.newSessionName, colour::text, -1, true);
    canvas.put (x, y, L' ', -1, colour::text);

    if (app.newSessionName.isEmpty())
        canvas.text (x + 2, y, "(type a name, or enter for a dated one)", colour::faint);
    else if (app.dateSessionNames && App::datedName (app.newSessionName) != app.newSessionName)
        canvas.text (x + 1, y, App::datedName ("").trim(), colour::faint); // the date it will get

    canvas.text (3, ++y, "in", colour::dim);
    canvas.text (9, y, app.newSessionParent.getFullPathName(), colour::faint, -1, false, w - 11);
    y += 2;
    canvas.text (1, y++, "template", colour::dim);

    for (int i = 0; i < (int) app.templates.size() && y < h - 4; ++i, ++y)
    {
        const auto& t = app.templates[(size_t) i];
        const bool sel = numRecent + i == app.pickIndex;
        const int bg = sel ? colour::focusBg : -1;

        canvas.fill (1, y, w - 2, L' ', -1, bg);
        if (sel)
            canvas.put (1, y, L'\x25b8', colour::accent, bg);

        canvas.text (3, y, padRight (t.name, 14), colour::text, bg, sel);

        juce::StringArray tracks;
        for (const auto& c : t.tree)
            if (c.hasType (ids::TRACK))
                tracks.add (c[ids::name].toString() + ((int) c[ids::input] > 0 ? L" \x2190 in" + c[ids::input].toString() : juce::String()));

        canvas.text (18, y, tracks.isEmpty() ? juce::String ("empty") : tracks.joinIntoString ("  "), colour::dim, bg, false, w - 20);
    }

    canvas.text (1, h - 2, "templates live in " + configFolder().getChildFile ("templates").getFullPathName() + L" \x00b7 add your own", colour::faint, -1, false, w - 2);
    canvas.text (1, h - 1, numRecent > 0 ? L"\x2191\x2193 choose \x00b7 enter open / create \x00b7 type a name for a new session \x00b7 esc quit"
                                         : L"type name \x00b7 \x2191\x2193 template \x00b7 enter create \x00b7 esc quit",
                 colour::dim);

    if (app.message.isNotEmpty())
        canvas.text (1, h - 3, app.message, app.messageIsError ? colour::error : colour::ok, -1, false, w - 2);
}
} // namespace taw
