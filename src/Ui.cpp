#include "Ui.h"
#include "App.h"

namespace taw
{
namespace colour
{
constexpr int text = 252, dim = 243, faint = 238, headerBg = 235, focusBg = 237;
constexpr int accent = 214, rec = 196, recBg = 52, play = 35, playBg = 22;
constexpr int clipAltBg = 239, hit = 179; // hit: sample-track steps (warm, unlike waveforms)
constexpr int selBg = 24, selClipBg = 31, loopBg = 28, loopOffBg = 239;
constexpr int clipBg = 236, wave = 110, liveWave = 210, marker = 170;
constexpr int ok = 114, error = 203, meterGreen = 35, meterYellow = 178, meterRed = 196, meterBg = 234;
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

    for (int x = 0; x < w; ++x)
    {
        const auto& c = cells[(size_t) (y * w + x)];

        if (c.fg != style.fg || c.bg != style.bg || c.bold != style.bold)
        {
            out += L"\x1b[0";
            if (c.bold) out += L";1";
            if (c.fg >= 0) out += L";38;5;" + std::to_wstring (c.fg);
            if (c.bg >= 0) out += L";48;5;" + std::to_wstring (c.bg);
            out += L"m";
            style = c;
        }

        out += c.ch;
    }

    return out + L"\x1b[0m";
}

//==============================================================================
static juce::String padRight (const juce::String& s, int width)
{
    return s.length() >= width ? s.substring (0, width) : s + juce::String::repeatedString (" ", width - s.length());
}

static float toDb (float gain) { return juce::Decibels::gainToDecibels (gain, -100.0f); }

void Ui::render (App& app)
{
    const auto w = app.console.width() - 1; // keep the last column free so nothing wraps
    const auto h = app.console.height();

    if (w != canvas.width() || h != canvas.height())
    {
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
//   4  ruler, then tracks
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
}

void Ui::drawTitle (App& app)
{
    const int x = canvas.text (1, 0, "taw", colour::accent, -1, true);

    if (auto* s = app.session.get())
        canvas.text (x, 0, L" \x25b8 " + s->name(), colour::text, -1, true);
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

    if (app.recording)
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
    const int laneX = leftPanel + 1, laneW = laneWidth (w + 1), meterX = laneX + laneW + 1;
    const auto spc = app.samplesPerColumn();
    const auto rolling = app.engine.isRolling();
    const auto playhead = app.engine.playhead();

    auto columnOf = [&] (juce::int64 pos) { return (int) std::floor (((double) pos - (double) app.viewStart) / spc); };
    const auto cursorCol = columnOf (app.cursor);
    const auto playCol = columnOf (playhead);

    // Ruler ---------------------------------------------------------------------
    const int rulerY = 4; // right below the transport bar
    const auto zoomText = s.gridOn() ? (app.zoomLevel == 0 ? juce::String ("1 beat") : juce::String (std::pow (2.0, app.zoomLevel), 2) + " beats")
                                     : juce::String (spc / app.sampleRate(), 2) + " s";
    canvas.text (1, rulerY, "zoom " + zoomText + "/col", colour::faint, -1, false, leftPanel - 1);
    canvas.put (laneX - 1, rulerY, L'\x2502', colour::faint);
    canvas.put (meterX - 1, rulerY, L'\x2502', colour::faint);
    canvas.text (meterX + 1, rulerY, "  vol", colour::faint);
    canvas.text (meterX + 7, rulerY, "level", colour::faint);
    canvas.text (meterX + 20, rulerY, " peak", colour::faint);

    // Time selection, as columns.
    const bool timeSel = app.hasTimeSelection();
    const auto selRange = app.timeSelection();
    const auto selTracks = app.selectedTracks();
    auto inTimeSelection = [&] (double t0, double t1) { return timeSel && t1 > (double) selRange.getStart() && t0 < (double) selRange.getEnd(); };

    // Loop range on the ruler: green when on, grey when set but off.
    {
        const auto loopStart = (double) (juce::int64) s.tree()[ids::loopStart];
        const auto loopEnd = (double) (juce::int64) s.tree()[ids::loopEnd];
        const int loopBg = (bool) s.tree()[ids::loop] ? colour::loopBg : colour::loopOffBg;

        if (loopEnd > loopStart)
            for (int c = 0; c < laneW; ++c)
                if (const auto t0 = (double) app.viewStart + c * spc; t0 + spc > loopStart && t0 < loopEnd)
                    canvas.put (laneX + c, rulerY, L' ', -1, loopBg);
    }

    if (timeSel)
        for (int c = 0; c < laneW; ++c)
            if (inTimeSelection ((double) app.viewStart + c * spc, (double) app.viewStart + (c + 1) * spc))
                canvas.put (laneX + c, rulerY, L' ', -1, colour::selBg);

    int nextFree = 0;

    for (int c = 0; c < laneW; ++c)
    {
        const auto t0 = (double) app.viewStart + c * spc, t1 = t0 + spc;
        juce::String label;

        if (s.gridOn())
        {
            const auto bar = std::ceil (t0 / s.samplesPerBar() - 1e-9);

            if (bar * s.samplesPerBar() < t1)
                label = juce::String ((int) bar + 1);
        }
        else
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
                canvas.put (laneX + c + j, rulerY, (wchar_t) label[j], colour::dim, canvas.at (laneX + c + j, rulerY).bg);
            nextFree = c + label.length() + 1;
        }
    }

    for (const auto& m : s.tree())
        if (m.hasType (ids::MARKER))
            if (const auto c = columnOf ((juce::int64) m[ids::position]); c >= 0 && c < laneW)
                canvas.text (laneX + c, rulerY, m[ids::name].toString().substring (0, 1), 231, colour::marker, true);

    if (cursorCol >= 0 && cursorCol < laneW)
        canvas.put (laneX + cursorCol, rulerY, L'\x25bc', colour::accent);

    // Tracks --------------------------------------------------------------------
    const int tracksTop = rulerY + 1, masterY = h - 6, tracksBottom = masterY - 1; // exclusive
    const int visible = std::max (1, tracksBottom - tracksTop);
    const int n = s.numTracks();

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

    if (n == 0)
        canvas.text (laneX + 1, tracksTop, L"no tracks \x00b7 type add", colour::dim);

    for (int row = 0; row < visible && app.trackScroll + row < n; ++row)
    {
        const int i = app.trackScroll + row, y = tracksTop + row;
        const auto t = s.track (i);
        const bool focusedRow = i == app.focused;
        const bool selectedRow = std::find (selTracks.begin(), selTracks.end(), i) != selTracks.end();
        const int bg = focusedRow ? colour::focusBg : (selectedRow ? colour::selBg : -1);
        // A time selection covers the selected tracks, or every track when none are selected.
        const bool rowInTimeSelection = timeSel && (selTracks.empty() || selectedRow);

        // Left panel: focus, number, name, flags, input
        canvas.fill (0, y, leftPanel, L' ', -1, bg);
        if (focusedRow)
            canvas.put (0, y, L'\x25b8', colour::accent, bg);

        canvas.text (1, y, juce::String (i + 1).paddedLeft (' ', 2), colour::text, bg, true);
        canvas.text (4, y, padRight (t[ids::name].toString(), 10), colour::text, bg);

        auto flag = [&] (int x, bool on, wchar_t ch, int fg) { canvas.put (x, y, on ? ch : L'\x00b7', on ? fg : colour::faint, bg, on); };
        flag (15, t[ids::armed], L'R', colour::rec);
        flag (16, t[ids::mute], L'M', colour::meterYellow);
        flag (17, t[ids::solo], L'S', colour::play);
        flag (18, t[ids::monitor], L'I', colour::wave);

        const bool sampleTrack = Session::isSampleTrack (t);
        const int input = t[ids::input];

        if (sampleTrack)
            canvas.text (20, y, "smp", colour::hit, bg);
        else
            canvas.text (20, y, input > 0 ? "in" + juce::String (input) : juce::String ("--"),
                         input > app.engine.numInputChannels() ? colour::error : colour::dim, bg);

        canvas.put (laneX - 1, y, L'\x2502', colour::faint);
        canvas.put (meterX - 1, y, L'\x2502', colour::faint);

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

        for (int c = 0; c < laneW; ++c)
        {
            const auto t0 = (double) app.viewStart + c * spc, t1 = t0 + spc;
            wchar_t ch = L' ';
            int fg = colour::wave, cellBg = -1;

            if (s.gridOn())
            {
                const auto bar = std::ceil (t0 / s.samplesPerBar() - 1e-9);
                if (bar * s.samplesPerBar() < t1)
                {
                    ch = L'\x00b7';
                    fg = colour::faint;
                }
            }

            // Sample tracks read as a step sequencer: every column is a step, a hit shows its
            // velocity as a shade, an empty step a faint dot (brighter on bar lines).
            if (sampleTrack)
            {
                const bool barLine = ch != L' ';
                ch = L'\x00b7';
                fg = barLine ? colour::dim : colour::faint;

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
                    ch = glyphFor (overviewPeak (lc.data->overview, fileZero, std::max (t0, start), std::min (t1, end)));
                    fg = colour::wave;
                    cellBg = k % 2 == 0 ? colour::clipBg : colour::clipAltBg;
                    break;
                }
            }

            if (liveTake != nullptr)
            {
                const auto end = (double) liveTake->start + (double) liveTake->overview.size() * Recorder::overviewBlock;

                if (t1 > (double) liveTake->start && t0 < end)
                {
                    ch = glyphFor (overviewPeak (liveTake->overview, liveTake->start, t0, t1));
                    fg = colour::liveWave;
                    cellBg = colour::recBg;
                }
            }

            if (rowInTimeSelection && inTimeSelection (t0, t1))
                cellBg = (cellBg == colour::clipBg || cellBg == colour::clipAltBg) ? colour::selClipBg : colour::selBg;

            if (rolling && c == playCol)
                cellBg = app.recording ? colour::rec : colour::play;
            else if (c == cursorCol)
                cellBg = rolling ? colour::faint : colour::accent, fg = rolling ? fg : 16;

            canvas.put (laneX + c, y, ch, fg, cellBg);
        }

        // Volume, then the meter: armed tracks show their input, others their playback
        const auto& m = app.trackMeters[(size_t) i];
        drawVolume (meterX + 1, y, t[ids::gainDb], bg);
        drawMeter (meterX + 7, y, 12, m.level, colour::meterBg);
        drawDb (meterX + 20, y, m.hold, m.clipped, bg);
    }

    // Mix ---------------------------------------------------------------------------
    const bool mixFocused = app.mixFocused();

    for (int ch = 0; ch < 2; ++ch)
    {
        const int y = masterY + ch;
        const int bg = mixFocused ? colour::focusBg : -1;
        canvas.fill (0, y, leftPanel, L' ', -1, bg);

        if (ch == 0)
        {
            if (mixFocused)
                canvas.put (0, y, L'\x25b8', colour::accent, bg);

            canvas.text (4, y, "Mix", colour::text, bg, true);
            drawVolume (meterX + 1, y, s.tree()[ids::mixDb], bg);
        }

        canvas.text (laneX - 3, y, ch == 0 ? "L" : "R", colour::dim, bg);
        canvas.put (laneX - 1, y, L'\x2502', colour::faint);
        canvas.put (meterX - 1, y, L'\x2502', colour::faint);

        if (ch == 0 && n > visible)
            canvas.text (laneX + 1, y, "tracks " + juce::String (app.trackScroll + 1) + "-" + juce::String (std::min (n, app.trackScroll + visible))
                                           + " of " + juce::String (n) + L" \x00b7 \x2191\x2193 to scroll",
                         colour::faint);

        const auto& m = app.masterMeters[(size_t) ch];
        drawMeter (meterX + 7, y, 12, m.level, colour::meterBg);
        drawDb (meterX + 20, y, m.hold, m.clipped);
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
        x = canvas.text (x + 1, cmdY, ": ", colour::accent, -1, true);
        x = canvas.text (x, cmdY, app.commandLine, colour::text, -1, false, w - 4);
        canvas.put (x, cmdY, L' ', -1, colour::text);

        // With an empty line, say what commands will act on.
        if (app.commandLine.isEmpty() && app.session != nullptr)
        {
            juce::StringArray parts;

            if (app.hasTimeSelection())
                parts.add (app.formatBars (app.timeSelection().getStart()) + L" \x2192 " + app.formatBars (app.timeSelection().getEnd()));

            if (const auto sel = app.selectedTracks(); ! sel.empty())
                parts.add (sel.size() == 1 ? "track " + juce::String (sel.front() + 1)
                                           : "tracks " + juce::String (sel.front() + 1) + "-" + juce::String (sel.back() + 1));

            if (! parts.isEmpty())
                canvas.text (x + 2, cmdY, "selected: " + parts.joinIntoString (juce::String (L" \x00b7 ")) + L" \x00b7 esc clears", colour::dim);
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
    else if (app.session != nullptr && ! app.mixFocused() && Session::isSampleTrack (app.session->track (app.focused)))
        hints = L"alt+/ toggles a hit \x00b7 alt \x2190\x2192 step, ctrl+alt 4 steps (grid 16 for 16ths) \x00b7 pat x...x... \x00b7 space play \x00b7 [ ] vol \x00b7 audio converts back \x00b7 ? help";
    else
        hints = L"enter run (empty: return) \x00b7 space play \x00b7 ctrl+space record \x00b7 1 r arms \x00b7 \x2190\x2192 bar \x00b7 ctrl 4 bars \x00b7 alt beat \x00b7 shift select \x00b7 [ ] vol ({ }: mix) \x00b7 add \x00b7 import / drop files \x00b7 ? help";

    canvas.text (1, hintY, hints, colour::dim, -1, false, w - 2);
}

void Ui::drawHelp (App&)
{
    // Two columns of "keys  what it does". Headings span a column; notes span the box.
    enum Kind { Pair, Heading, Note, Blank };
    struct Row { Kind kind; const wchar_t *k1, *d1, *k2, *d2; };

    static const Row rows[] = {
        { Heading, L"MOVE", nullptr, L"PLAY & RECORD", nullptr },
        { Pair, L"\x2190 \x2192", L"1 bar  (grid off: 1 s)", L"space", L"play / stop" },
        { Pair, L"ctrl \x2190 \x2192", L"4 bars", L"1 r  1-3 r", L"arm (+ hear input)" },
        { Pair, L"alt \x2190 \x2192", L"1 step (ctrl+alt: 4)", L"ctrl+space", L"record armed tracks" },
        { Pair, L"shift + any", L"select time / tracks", L"ctrl+space", L"...while playing: punch" },
        { Pair, L"home / end", L"start / end", L"2mon", L"hear input on playback too" },
        { Pair, L"\x2191 \x2193", L"choose track or Mix", L"ctrl k", L"marker at the playhead" },
        { Pair, L"@17  @1:30", L"bar 17 / 1 min 30 s", L"commands", L"work while playing too" },
        { Pair, L"+ -   enter", L"zoom / return to start", L"l", L"loop the selection / toggle loop" },
        { Heading, L"TRACKS", nullptr, L"VOLUME", nullptr },
        { Note, L"Numbers first, spaces optional: 2m, 1-3m, 2i3, 2v-6. No number = selection or \x25b8 track." },
        { Pair, L"add vox in2", L"new track on input 2", L"[  ]", L"track -1 / +1 dB" },
        { Pair, L"2a 2m 2s", L"arm / mute / solo", L"{ } ctrl [ ]", L"mix -1 / +1 dB" },
        { Pair, L"1 rec", L"arm 1 and record", L"2v-6", L"track volume to -6 dB" },
        { Pair, L"2i3", L"use input 3", L"mixv-3", L"mix volume to -3 dB" },
        { Pair, L"2n Vox", L"rename", L"2x", L"drop latest clip" },
        { Heading, L"SAMPLES", nullptr, L"EDIT", nullptr },
        { Pair, L"sample f", L"new sample track", L"ctrl c x v", L"copy / cut / paste" },
        { Pair, L"3 sample", L"convert (3 audio: back)", L"ctrl d", L"duplicate after itself" },
        { Pair, L"3 pat x...", L"pattern: X x o .", L"delete", L"clear the range" },
        { Pair, L"grid 16", L"steps of a 16th", L"ctrl z / y", L"undo / redo" },
        { Pair, L"alt+/", L"toggle a hit at cursor", L"sel 3 5", L"select bars 3 to 5" },
        { Heading, L"SESSION", nullptr, nullptr, nullptr },
        { Pair, L"click", L"click on / off", L"click rec", L"click only when recording" },
        { Pair, L"bpm 92", L"tempo  (grid: on/off)", L"dev  panel", L"audio device / driver" },
#if JUCE_WINDOWS
        { Pair, L"b  b mp3", L"bounce the selection", L"b wav D:\\x", L"...to a folder (remembered)" },
#else
        { Pair, L"b  b mp3", L"bounce the selection", L"b wav ~/x", L"...to a folder (remembered)" },
#endif
        { Pair, L"import f", L"track per file/folder", L"2 import f", L"onto track 2  (or drop files)" },
        { Pair, L"save  close", L"save / back to sessions", L"q", L"save and quit" },
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
