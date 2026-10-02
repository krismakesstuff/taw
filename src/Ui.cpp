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
int gridBar; // bar lines; beats and finer use faint
int laneFocusBg, laneSelBg; // empty lane cells on the focused / selected tracks (apart from clips)
int clipBg, wave, liveWave, marker;
int note, blackKeyBg; // instrument notes; piano-roll rows of the black keys
int ok, error, meterGreen, meterYellow, meterRed, meterBg;

void apply (const juce::String& theme)
{
    if (theme == "grey")
    {
        // Mid-light grey: a softer screen than light, with darker clips and text to match.
        screenFg = 234, screenBg = 249;
        text = 233, dim = 238, faint = 244, headerBg = 247, focusBg = 246;
        accent = 166, rec = 160, recBg = 217, play = 28, playBg = 28;
        clipAltBg = 243, hit = 94;
        selBg = 110, selClipBg = 74, loopBg = 108, loopOffBg = 245;
        gridBar = 241;
        laneFocusBg = 248, laneSelBg = 146;
        clipBg = 245, wave = 18, liveWave = 124, marker = 90;
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
        gridBar = 245;
        laneFocusBg = 254, laneSelBg = 189;
        clipBg = 251, wave = 25, liveWave = 160, marker = 133;
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
        gridBar = 241;
        laneFocusBg = 234, laneSelBg = 17;
        clipBg = 236, wave = 110, liveWave = 210, marker = 170;
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

    if (w < 70 || h < 16)
        canvas.text (1, 0, L"taw needs at least 70x16 \x2014 make the window bigger", colour::error);
    else if (app.mode == Mode::DevicePicker)
        drawDevicePicker (app);
    else if (app.mode == Mode::TemplatePicker)
        drawTemplatePicker (app);
    else
        drawMain (app);

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
    drawTitle (app);
    drawInterface (app);
    drawTransport (app);

    if (app.session != nullptr)
        drawTimeline (app);

    drawBottom (app);

    if (app.helpVisible)
        drawHelp (app);
    else if (app.pluginPickerVisible && app.session != nullptr)
        drawPluginPicker (app);
    else if (app.busPickerVisible && app.session != nullptr)
        drawBusPicker (app);
    else if (app.midiPanelVisible && app.midiInputs != nullptr)
        drawMidiPanel (app);
    else if (app.settingsVisible && app.session != nullptr)
        drawSettings (app);
}

void Ui::drawTitle (App& app)
{
    const int x = canvas.text (1, 0, "taw", colour::accent, -1, true);

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

    const int w = canvas.width(), y = 3, bg = colour::headerBg;
    const auto rolling = app.engine.isRolling();
    const auto pos = rolling ? app.engine.playhead() : app.cursor;

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
    const int w = canvas.width(), h = canvas.height();
    const bool io = app.ioVisible;
    const int ioX = leftPanel + 1, meterX = ioX + ioWidth (io), laneX = laneStart (io), laneW = laneWidth (w + 1, io);

    // The borders between the panels and the timeline, and after it.
    auto borders = [&] (int y)
    {
        canvas.put (ioX - 1, y, L'\x2502', colour::faint);
        canvas.put (meterX - 1, y, L'\x2502', colour::faint);
        canvas.put (laneX - 1, y, L'\x2502', colour::faint);
        canvas.put (laneX + laneW, y, L'\x2502', colour::faint);
    };
    const auto spc = app.samplesPerColumn();
    const auto rolling = app.engine.isRolling();
    const auto playhead = app.engine.playhead();

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
        canvas.fill (0, y, leftPanel, L' ', -1, bg);

        if (focusedLane)
            canvas.put (0, y, L'\x25b8', colour::accent, bg);

        canvas.text (1, y, label, focusedLane ? colour::dim : colour::faint, bg, false, leftPanel - 1);
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

    // Tracks --------------------------------------------------------------------
    const int tracksTop = rulerY + 1, masterY = h - 6, tracksBottom = masterY - 1; // exclusive
    const int n = s.numTracks();
    // Fill view (f): the focused track takes all the rows; the others are an arrow away.
    const bool fill = app.fillView && ! app.onTopLane() && ! app.mixFocused() && n > 0;
    const int rowsPerTrack = fill ? std::max (1, tracksBottom - tracksTop) : s.trackHeight(); // "height", per session
    const int visible = fill ? 1 : std::max (1, (tracksBottom - tracksTop) / rowsPerTrack); // tracks that fit

    const int focusRow = std::min (app.focused, n - 1); // the mix row lives below the list

    if (focusRow < app.trackScroll)
        app.trackScroll = std::max (0, focusRow);
    if (focusRow >= app.trackScroll + visible)
        app.trackScroll = focusRow - visible + 1;

    // Loudness as shade density; these four blocks exist in every console font.
    static const wchar_t levels[] = L" \x2591\x2592\x2593\x2588";
    auto glyphFor = [] (float peak)
    {
        const auto db = toDb (peak);
        return db < -54.0f ? levels[0] : levels[juce::jlimit (1, 4, 1 + (int) ((db + 54.0f) / 54.0f * 4.0f))];
    };

    // Taller tracks draw the waveform centred: loudness sets how far it reaches up and down from
    // the middle, and a row it only partly reaches gets a lighter shade.
    auto waveGlyph = [&] (float peak, int r)
    {
        if (rowsPerTrack == 1)
            return glyphFor (peak);

        const auto db = toDb (peak);
        const auto level = db < -54.0f ? 0.0 : juce::jlimit (0.0, 1.0, (db + 54.0) / 54.0);
        const auto reach = level * rowsPerTrack * 0.5 + 0.25;                              // rows from the middle
        const auto from = std::max (0.0, std::abs (r + 0.5 - rowsPerTrack * 0.5) - 0.5); // where this row starts
        const auto cover = juce::jlimit (0.0, 1.0, reach - from);
        return level <= 0.0 ? levels[0] : levels[juce::jlimit (0, 4, (int) std::ceil (cover * 4.0 - 0.5))];
    };

    // Peak of an overview over [t0, t1) relative to a clip start.
    auto overviewPeak = [] (const std::vector<float>& ov, juce::int64 start, double t0, double t1)
    {
        const auto a = std::max (0, (int) std::floor ((t0 - (double) start) / Recorder::overviewBlock));
        const auto b = std::min ((int) ov.size(), (int) std::ceil ((t1 - (double) start) / Recorder::overviewBlock));
        float peak = 0.0f;

        for (int i = a; i < b; ++i)
            peak = std::max (peak, ov[(size_t) i]);

        return peak;
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

        for (const auto& node : Session::notes (t))
        {
            const double b = node[ids::beat], len = node[ids::len];
            notes.push_back ({ b * spb, (b + len) * spb, (int) node[ids::pitch], false });
        }

        // Keys still down while recording reach to the playhead.
        if (std::find (app.midiRecordTracks.begin(), app.midiRecordTracks.end(), i) != app.midiRecordTracks.end())
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
                canvas.text (leftPanel - 1 - name.length(), y + r, name, p % 12 == 0 ? colour::text : isBlackKey (p) ? colour::faint : colour::dim,
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

            for (int r = 0; r < rowsPerTrack; ++r)
            {
                const int up = cover[(size_t) (r * 2)], down = cover[(size_t) (r * 2 + 1)];
                const int pitch = top - r;
                int cellBg = selected ? colour::selBg : (pianoRoll && isBlackKey (pitch) ? colour::blackKeyBg : laneBg);
                wchar_t ch = line > 0 ? gridMark : L' ';
                int fg = line == 2 ? colour::gridBar : colour::faint;

                if (up > 0 || down > 0)
                {
                    ch = up > 0 && down > 0 ? L'\x2588' : up > 0 ? L'\x2580' : L'\x2584';
                    fg = std::max (up, down) == 2 ? colour::liveWave : colour::note;
                }

                if (playheadHere)
                    ch = cursorMark, fg = app.recording ? colour::rec : colour::play;
                else if (cursorHere)
                    ch = cursorMark, fg = rolling ? colour::dim : colour::accent;

                canvas.put (laneX + c, y + r, ch, fg, cellBg);
            }
        }
    };

    if (n == 0)
        canvas.text (laneX + 1, tracksTop, L"no tracks \x00b7 type add", colour::dim);

    for (int row = 0; row < visible && app.trackScroll + row < n; ++row)
    {
        const int i = app.trackScroll + row, y = tracksTop + row * rowsPerTrack;
        const auto t = s.track (i);
        const bool focusedRow = i == app.focused && ! app.onTopLane();
        const bool selectedRow = std::find (selTracks.begin(), selTracks.end(), i) != selTracks.end();
        const int bg = focusedRow ? colour::focusBg : (selectedRow ? colour::selBg : -1);
        // A time selection covers the tracks edits will act on: the selected ones, else the focused
        // one, or every track from the bar or markers row (App::defaultTargets).
        const bool rowInTimeSelection = timeSel && (app.onTopLane() || (selTracks.empty() ? focusedRow : selectedRow));
        // The row stays lit all the way along the timeline, so you can see which track you're on.
        const int laneBg = selectedRow ? colour::laneSelBg : focusedRow ? colour::laneFocusBg : -1;

        // Left panel: focus, number, name, flags, input (on the first row; the rest is its colour)
        for (int r = 0; r < rowsPerTrack; ++r)
        {
            canvas.fill (0, y + r, leftPanel, L' ', -1, bg);
            canvas.put (laneX - 1, y + r, L'\x2502', colour::faint);
            canvas.fill (meterX, y + r, meterPanel, L' ', -1, bg);
            borders (y + r);
        }

        if (focusedRow)
            canvas.put (0, y, L'\x25b8', colour::accent, bg);

        canvas.text (1, y, juce::String (i + 1).paddedLeft (' ', 2), colour::text, bg, true);
        canvas.text (4, y, padRight (t[ids::name].toString(), 10), colour::text, bg);

        auto flag = [&] (int x, bool on, wchar_t ch, int fg) { canvas.put (x, y, on ? ch : L'\x00b7', on ? fg : colour::faint, bg, on); };
        // A bus has nothing to arm or monitor: just mute and solo.
        const bool busRow = Session::isBusTrack (t);

        if (! busRow)
            flag (15, t[ids::armed], L'R', colour::rec);

        flag (16, t[ids::mute], L'M', colour::meterYellow);
        flag (17, t[ids::solo], L'S', colour::play);

        if (! busRow)
            flag (18, t[ids::monitor], L'I', colour::wave);

        const bool sampleTrack = Session::isSampleTrack (t);
        const bool instrumentTrack = Session::isInstrumentTrack (t);
        const bool busTrack = Session::isBusTrack (t);
        const int input = t[ids::input];

        if (busTrack)
            canvas.text (20, y, "bus", colour::accent, bg);
        else if (sampleTrack)
            canvas.text (20, y, "smp", colour::hit, bg);
        else if (instrumentTrack)
        {
            // "inst", red when its plugin didn't load; the plugin's name under the track's.
            const auto problem = app.instrumentProblem (i);
            canvas.text (20, y, "inst", problem.isNotEmpty() ? colour::error : colour::note, bg);

            if (rowsPerTrack > 1)
                canvas.text (4, y + 1, problem.isNotEmpty() ? problem : t[ids::pluginName].toString(),
                             problem.isNotEmpty() ? colour::error : colour::dim, bg, false, fill ? 15 : 19);
        }
        else
        {
            // A stereo track records a pair: "i3-4" (past 9 just "11-12").
            const bool stereo = app.trackIsStereo (t);
            const auto text = input <= 0 ? juce::String ("--")
                              : stereo   ? (input < 10 ? "i" : "") + juce::String (input) + "-" + juce::String (input + 1)
                                         : "in" + juce::String (input);
            canvas.text (20, y, text, input + (stereo ? 1 : 0) > app.engine.numInputChannels() ? colour::error : colour::dim, bg, false, 5);
        }

        // Lane
        struct LaneClip
        {
            const ClipData* data;
            juce::int64 start, offset, length;
        };

        std::vector<LaneClip> laneClips;
        std::vector<std::pair<double, float>> laneHits; // sample tracks: (position in samples, velocity)

        if (sampleTrack)
        {
            for (const auto& hitNode : Session::hits (t))
                laneHits.push_back ({ (double) hitNode[ids::beat] * s.samplesPerBeat(), (float) (double) hitNode[ids::vel] });

            std::sort (laneHits.begin(), laneHits.end());
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
                float wavePeak = -1.0f; // a clip or take here: drawn per row by waveGlyph
                wchar_t edgeCh = 0;     // a clip's first or last column

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

                    auto it = std::lower_bound (laneHits.begin(), laneHits.end(), std::make_pair (t0 - 0.5, 0.0f));
                    float vel = -1.0f;

                    for (; it != laneHits.end() && it->first < t1 - 0.5; ++it)
                        vel = std::max (vel, it->second);

                    if (vel >= 0.0f)
                    {
                        ch = vel >= 0.95f ? L'\x2588' : vel >= 0.7f ? L'\x2593' : vel >= 0.4f ? L'\x2592' : L'\x2591';
                        fg = colour::hit;
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
                        wavePeak = overviewPeak (lc.data->overview, fileZero, std::max (t0, start), std::min (t1, end));
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
                        wavePeak = overviewPeak (liveTake->overview, liveTake->start, t0, t1);
                        edgeCh = 0;
                        inClip = false;
                        fg = colour::liveWave;
                        cellBg = colour::recBg;
                    }
                }

                if (rowInTimeSelection && inTimeSelection (t0, t1))
                    cellBg = (cellBg == colour::clipBg || cellBg == colour::clipAltBg) ? colour::selClipBg : colour::selBg;

                // The playhead and the cursor are thin lines over whatever's there, so selections and
                // clips stay readable; the playhead wins when they share a column.
                const bool playheadHere = rolling && c == playCol;
                const bool cursorHere = c == cursorCol && ! playheadHere;

                for (int r = 0; r < rowsPerTrack; ++r)
                {
                    auto rowCh = edgeCh != 0 ? edgeCh : wavePeak >= 0.0f ? waveGlyph (wavePeak, r) : ch;
                    auto rowFg = fg;

                    if (playheadHere)
                        rowCh = cursorMark, rowFg = app.recording ? colour::rec : colour::play;
                    else if (cursorHere)
                        rowCh = cursorMark, rowFg = rolling ? colour::dim : colour::accent;

                    canvas.put (laneX + c, y + r, rowCh, rowFg, cellBg);

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
                                 app.insertProblem (insert).isNotEmpty() ? colour::error : (bool) insert[ids::bypass] ? colour::faint : colour::note, cbg,
                                 false, 3);
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
                                 dest < 0 ? colour::error : off ? colour::faint : colour::text, cbg, false, 3);

                    // The level under the name; pre-fader sends in the accent colour.
                    if (rowsPerTrack > 2)
                    {
                        const double db = send[ids::db];
                        const auto level = db <= -96.0 ? juce::String (L"-\x221e") : juce::String (juce::roundToInt (db));
                        canvas.fill (x - 1, y + 2, ioSlotW, L' ', -1, cbg);
                        canvas.text (x, y + 2, level, off ? colour::faint : (bool) send[ids::pre] ? colour::accent : colour::dim, cbg, false, 3);
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
        drawVolume (meterX, y, t[ids::gainDb], bg);
        drawDb (meterX + 12, y, m.hold, m.clipped, bg);

        // Pan under the volume (centre dim, like unity); "st" marks a stereo track.
        if (rowsPerTrack > 1)
        {
            const double pan = t[ids::pan];
            canvas.text (meterX, y + 1, App::panText (pan).paddedLeft (' ', 5), std::abs (pan) < 0.005 ? colour::dim : colour::text, bg);

            if (app.trackIsStereo (t))
                canvas.text (meterX + 12, y + 1, "   st", colour::faint, bg);
        }

        for (int r = 0; r < rowsPerTrack; ++r)
            drawMeter (meterX + 6, y + r, 5, m.level, colour::meterBg);
    }

    // The cursor's handle sits under the last track, clear of the ruler's numbers.
    if (cursorCol >= 0 && cursorCol < laneW && n > 0)
        canvas.put (laneX + cursorCol, tracksTop + std::min (visible, n - app.trackScroll) * rowsPerTrack, L'\x25b2', colour::accent);

    // Mix ---------------------------------------------------------------------------
    const bool mixFocused = app.mixFocused();

    for (int ch = 0; ch < 2; ++ch)
    {
        const int y = masterY + ch;
        const int bg = mixFocused ? colour::focusBg : -1;
        canvas.fill (0, y, meterX + meterPanel, L' ', -1, bg);

        if (ch == 0)
        {
            if (mixFocused)
                canvas.put (0, y, L'\x25b8', colour::accent, bg);

            canvas.text (4, y, "Mix", colour::text, bg, true);
            drawVolume (meterX, y, s.tree()[ids::mixDb], bg);
        }

        canvas.text (leftPanel - 2, y, ch == 0 ? "L" : "R", colour::dim, bg);
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
}

void Ui::drawVolume (int x, int y, double db, int bg)
{
    // Unity is dim so a changed fader stands out.
    const auto text = formatDb (db).paddedLeft (' ', 5);
    canvas.text (x, y, text, std::abs (db) < 0.05 ? colour::dim : colour::text, bg, std::abs (db) >= 0.05);
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

        x = canvas.text (0, cmdY, L" \x25cf RECORDING  " + tracks.joinIntoString (" ") + "   " + app.formatTime (app.engine.playhead() - app.playStart) + " ",
                         231, colour::rec, true);
    }
    else if (rolling)
    {
        x = canvas.text (0, cmdY, L" \x25ba PLAYING ", 231, colour::playBg, true);
    }

    {
        if (app.namingMarker.isValid())
            x = canvas.text (x, cmdY, " name marker " + app.namingMarker[ids::name].toString() + " ", 231, colour::marker, true);

        x = canvas.text (x + 1, cmdY, ": ", colour::accent, -1, true);
        x = canvas.text (x, cmdY, app.commandLine, colour::text, -1, false, w - 4);
        canvas.put (x, cmdY, L' ', -1, colour::text);

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
                const auto beats = (double) sel.getLength() / s.samplesPerBeat();
                const auto bars = (int) std::floor (beats / s.beatsPerBar() + 1e-6);
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

    if (app.recording)
        hints = L"space stop \x00b7 ctrl+space punch out \x00b7 ctrl+k marker \x00b7 commands work: 2m 3s c \x00b7 [ ] volume";
    else if (rolling)
        hints = app.hasTimeSelection() ? L"ctrl c copy \x00b7 x cut \x00b7 d duplicate \x00b7 ctrl+v pastes at the cursor \x00b7 l loop \x00b7 shift \x2190\x2192 extend \x00b7 space stop"
                                       : L"space stop \x00b7 type commands (2m 3s l c) + enter \x00b7 enter alone returns \x00b7 ctrl+space punch in \x00b7 ctrl+k marker \x00b7 [ ] volume \x00b7 \x2190\x2192 cursor, shift selects \x00b7 @17 seeks";
    else if (app.hasTimeSelection())
        hints = L"ctrl c copy \x00b7 x cut \x00b7 d duplicate \x00b7 del clear \x00b7 l loop \x00b7 b bounce (b mp3) \x00b7 pat x... fills it \x00b7 esc clears \x00b7 shift \x2190\x2192 extend";
    else if (app.ioCell >= 0 && app.session != nullptr && ! app.mixFocused() && ! app.onTopLane())
        hints = app.ioCell < App::ioFirstSend
                    ? L"\x2190\x2192 inserts, sends, output \x00b7 \x2191\x2193 tracks \x00b7 enter opens (empty: pick an effect) \x00b7 M bypass \x00b7 shift+\x2190\x2192 moves \x00b7 del removes \x00b7 esc or o leaves"
                : app.ioCell == App::ioOutput
                    ? L"\x2190\x2192 inserts, sends, output \x00b7 \x2191\x2193 tracks \x00b7 enter picks a bus \x00b7 del: back to the mix \x00b7 [ ] track volume \x00b7 esc or o leaves"
                    : L"\x2190\x2192 inserts, sends, output \x00b7 \x2191\x2193 tracks \x00b7 enter picks a bus \x00b7 [ ] send level \x00b7 M off \x00b7 P pre/post \x00b7 del removes \x00b7 esc or o leaves";
    else if (app.session != nullptr && ! app.mixFocused() && ! app.onTopLane() && Session::isBusTrack (app.session->track (app.focused)))
        hints = L"a bus plays what's sent to it \x00b7 2 send <name> or 2 out <name> feeds it \x00b7 o routes \x00b7 [ ] its volume \x00b7 shift+S solo (keeps what feeds it) \x00b7 ? help";
    else if (app.session != nullptr && ! app.mixFocused() && ! app.onTopLane() && Session::isInstrumentTrack (app.session->track (app.focused)))
        hints = L"shift+R arms (your controller plays it) \x00b7 ctrl+space records \x00b7 ed plugin window \x00b7 f piano roll \x00b7 snap quantizes \x00b7 inst swaps the plugin \x00b7 ? help";
    else if (app.session != nullptr && ! app.mixFocused() && Session::isSampleTrack (app.session->track (app.focused)))
        hints = L"shift+R arms \x00b7 . plays armed (shift: accent), ctrl+space records it \x00b7 snap quantizes \x00b7 alt+/ toggles a hit \x00b7 pat x...x... \x00b7 ? help";
    else
        hints = L"enter run (empty: return) \x00b7 space play \x00b7 ctrl+space record \x00b7 shift+R arms \x00b7 r t zoom \x00b7 \x2190\x2192 bar \x00b7 ctrl 4 bars \x00b7 alt beat \x00b7 shift select \x00b7 [ ] vol ({ }: mix) \x00b7 add \x00b7 import / drop files \x00b7 ? help";

    canvas.text (1, hintY, hints, colour::dim, -1, false, w - 2);
}

void Ui::drawHelp (App&)
{
    // Two columns of "keys  what it does". Headings span a column; notes span the box.
    enum Kind { Pair, Heading, Note, Blank };
    struct Row { Kind kind; const wchar_t *k1, *d1, *k2, *d2; };

    static const Row rows[] = {
        { Heading, L"MOVE", nullptr, L"PLAY & RECORD", nullptr },
        { Pair, L"\x2190 \x2192", L"next grid line (zoom)", L"space", L"play / stop" },
        { Pair, L"ctrl \x2190 \x2192", L"4 bars", L"shift+R  1r", L"arm (+ hear input)" },
        { Pair, L"alt \x2190 \x2192", L"1 step (ctrl+alt: 4)", L"ctrl+space", L"record armed tracks" },
        { Pair, L"shift + any", L"select time / tracks", L"ctrl+space", L"...while playing: punch in/out" },
        { Pair, L"home / end", L"start / end", L"shift+I 2mon", L"hear input on playback too" },
        { Pair, L"\x2191 \x2193", L"track (ctrl: first/last)", L"ctrl k  k", L"add marker / markers row" },
        { Pair, L"@17  @1:30", L"bar 17 / 1 min 30 s", L"commands", L"work while playing too" },
        { Pair, L"r t   enter", L"zoom out, in / return", L"l", L"loop the selection / toggle loop" },
        { Pair, L"q w", L"go to sel. start / end", L"tab", L"next clip edge / hit" },
        { Heading, L"TRACKS", nullptr, L"VOLUME", nullptr },
        { Note, L"Numbers first, spaces optional: 2m, 1-3m, 2i3, 2v-6. No number = selection or \x25b8 track." },
        { Pair, L"add vox in2", L"new track on input 2", L"[  ]", L"track -1 / +1 dB" },
        { Pair, L"shift+M S", L"mute / solo  (2m 2s)", L"{ } ctrl [ ]", L"mix -1 / +1 dB" },
        { Pair, L"1 rec", L"arm 1 and record", L"2v-6", L"track volume to -6 dB" },
        { Pair, L"2i3", L"use input 3", L"mixv-3", L"mix volume to -3 dB" },
        { Pair, L"2n Vox", L"rename", L"2x", L"drop latest clip" },
        { Pair, L"2 pan l30", L"pan (alt+[ ] nudges)", L"2 stereo", L"stereo: inputs 3-4 (2 i 3-4)" },
        { Heading, L"SAMPLES", nullptr, L"EDIT", nullptr },
        { Pair, L"sample f", L"new sample track", L"ctrl c x v", L"copy / cut / paste" },
        { Pair, L".  >", L"play armed (> accent)", L"snap  snap 50", L"quantize hits (%)" },
        { Pair, L"3 sample", L"convert (3 audio: back)", L"ctrl d", L"duplicate after itself" },
        { Pair, L"3 pat x...", L"pattern: X x o .", L"delete", L"clear the range" },
        { Pair, L"grid 16", L"steps of a 16th", L"ctrl z / y", L"undo / redo" },
        { Pair, L"alt+/", L"toggle a hit at cursor", L"sel 3 5", L"select bars 3 to 5" },
        { Heading, L"INSTRUMENTS", nullptr, L"ROUTING", nullptr },
        { Pair, L"inst  inst juno", L"instrument track (picker)", L"bus Verb", L"new bus track" },
        { Pair, L"shift+R  ed", L"arm for MIDI / its window", L"2 send verb", L"send A-E (a new name: a bus)" },
        { Pair, L"f", L"focused track fills: piano roll", L"2 send b -6", L"level; pre post off on x" },
        { Pair, L"scan  midi", L"find plugins / MIDI inputs on", L"2 out drums", L"output to a bus (out mix)" },
        { Pair, L"ins  ins cla", L"effect picker / by name", L"2 fx b  b x", L"its window / remove it" },
        { Pair, L"2 fx b off", L"bypass (on)  2 fx b>d swaps", L"o   io", L"inserts+sends column / hide" },
        { Heading, L"SESSION", nullptr, nullptr, nullptr },
        { Pair, L"click", L"click on / off", L"click rec", L"click only when recording" },
        { Pair, L"count 4", L"count-in clicks (off)", L"count always", L"...on play too (rec: not)" },
        { Pair, L"bpm 92", L"tempo  (grid: on/off)", L"dev  panel", L"audio device / driver" },
        { Pair, L"height 3", L"rows per track (1-4)", L"dark  light", L"themes (also: grey)" },
#if JUCE_WINDOWS
        { Pair, L"b  b mp3", L"bounce the selection", L"b wav D:\\x", L"...to a folder (remembered)" },
#else
        { Pair, L"b  b mp3", L"bounce the selection", L"b wav ~/x", L"...to a folder (remembered)" },
#endif
        { Pair, L"import f", L"track per file/folder", L"2 import f", L"onto track 2  (or drop files)" },
        { Pair, L"save  close", L"save / back to sessions", L"ctrl+q  exit", L"save and quit" },
        { Note, L"esc clears the line, the selection, then clip lights \x00b7 ? closes this" },
    };

    constexpr int keyW = 13, colW = 38; // key column width, width of each half
    const int w = canvas.width(), h = canvas.height();
    // Covers everything but the header and the hint line.
    const int boxW = std::min (w - 4, 2 * colW + 4), boxH = std::min (h - 2, (int) std::size (rows) + 2);
    const int x0 = (w - boxW) / 2, y0 = 1, bg = colour::headerBg;

    for (int y = 0; y < boxH; ++y)
        canvas.fill (x0, y0 + y, boxW, L' ', -1, bg);

    auto pair = [&] (int x, int y, const wchar_t* k, const wchar_t* d)
    {
        if (k == nullptr)
            return;

        canvas.text (x, y, k, colour::accent, bg, false, keyW - 1);
        canvas.text (x + keyW, y, d, colour::text, bg, false, colW - keyW - 1);
    };

    for (int i = 0; i < boxH - 2; ++i)
    {
        const auto& r = rows[i];
        const int y = y0 + 1 + i, left = x0 + 2, right = left + colW;

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
            case Blank:
                break;
        }
    }
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
    canvas.text (x0 + 15, y0 + 1, effects ? "insert " + Session::slotName (app.pluginPickSlot) + " on track " + juce::String (app.pluginPickTarget + 1)
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

// Every MIDI input: on or off, whether it opened, and what it last sent (lit while it's sending).
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

void Ui::drawTemplatePicker (App& app)
{
    const int w = canvas.width(), h = canvas.height();
    const int numRecent = (int) app.recent.size();
    canvas.fill (0, 0, w, L' ', -1, colour::headerBg);
    canvas.text (1, 0, "taw", colour::accent, colour::headerBg, true);
    canvas.text (5, 0, L"\x25b8 sessions", colour::text, colour::headerBg);

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
