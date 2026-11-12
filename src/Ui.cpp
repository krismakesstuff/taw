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
int agent, agentText; // what an agent just changed (taw --mcp): its colour, and text on it
bool lightScreen = false; // light and grey: darker kind colours

// A sample kind's colour, the same on the browser's badges, map and list.
static int kind (const juce::String& k)
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
static juce::Colour xtermColour (int index)
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

static void apply (const juce::String& theme)
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
        agent = 91, agentText = 231;
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
        agent = 91, agentText = 231;
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
        agent = 141, agentText = 16;
    }
}
} // namespace colour

// An agent's change to this lately: 2 just now (a flash), 1 in the last few seconds (the
// agents' colour), 0 none. what: as App::markAgentEdit ("": anything on it).
static int agentLook (const App& app, const juce::ValueTree& owner, const juce::String& what = {})
{
    const auto age = app.agentMarkAge (owner, what);
    return age < 0.0 ? 0 : age < App::agentFlashMs ? 2 : 1;
}

juce::Colour sampleKindColour (const juce::String& kind) { return colour::xtermColour (colour::kind (kind)); }

//==============================================================================
void Canvas::resize (int newW, int newH)
{
    w = std::max (0, newW);
    h = std::max (0, newH);
    cells.assign ((size_t) (w * h), Cell {});
}

void Canvas::put (int x, int y, wchar_t ch, int fg, int bg, bool bold)
{
    if (x >= 0 && y >= std::max (0, clipTop) && x < w && y < std::min (h, clipBottom))
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

    if (app.mode == Mode::TemplatePicker)
    {
        drawTemplatePicker (app);

        if (app.helpVisible)
            drawHelp (app);
    }
    else
    {
        drawMain (app);

        if (app.closePrompt.has_value())
            drawClosePrompt (app);
        else if (app.sessionAsk.has_value())
            drawSessionPrompt (app);
        else if (app.tempoAsk.has_value())
            drawTempoPrompt (app);
    }

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
    faderMap.valid = false;
    laneMap.valid = false; // until the timeline is drawn (a picker or a box over it: clicks pass it by)

    if (w < 70 || h < 16)
        canvas.text (1, 0, L"taw needs at least 70x16 \x2014 make the window bigger", colour::error);
    else if (app.mode == Mode::DevicePicker)
        drawDevicePicker (app);
    else if (app.mode == Mode::Hub)
        drawHubPage (app);
    else if (app.mode == Mode::TemplatePicker)
    {
        drawTemplatePicker (app);

        if (app.opening != nullptr)
            drawOpening (app);
        else if (app.helpVisible)
            drawHelp (app);
    }
    else
    {
        drawMain (app);

        if (app.closing != nullptr)
            drawClosing (app);
        else if (app.closePrompt.has_value())
            drawClosePrompt (app);
        else if (app.sessionAsk.has_value())
            drawSessionPrompt (app);
        else if (app.tempoAsk.has_value())
            drawTempoPrompt (app);
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

    // The cursor hidden again with every frame: the console shows it again by itself (a resize,
    // another window taking focus), and it would sit wherever the last row drawn ended.
    if (! frame.empty())
        app.console.write (L"\x1b[?25l" + frame);
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
    focusFrame.valid = false;
    scrollBar.valid = false;

    drawTitle (app);
    drawInterface (app);
    drawTransport (app);

    if (app.session != nullptr)
        drawTimeline (app);

    drawBottom (app);
    drawFocusFrame();
    drawScrollBar();
    drawTyping (app);
    drawKeysWarning (app);

    // A box over the timeline takes the clicks out of it.
    if (app.helpVisible || app.pluginPickerVisible || app.busPickerVisible || app.sendFaderVisible || app.trackMenuVisible || app.autoPanelVisible || app.midiPanelVisible
        || app.settingsVisible || app.statsVisible || app.phoneVisible)
        laneMap.valid = false;

    if (app.helpVisible)
        drawHelp (app);
    else if (app.pluginPickerVisible && app.session != nullptr)
        drawPluginPicker (app);
    else if (app.busPickerVisible && app.session != nullptr)
        drawBusPicker (app);
    else if (app.sendFaderVisible && app.session != nullptr)
        drawSendFader (app);
    else if (app.trackMenuVisible && app.session != nullptr)
        drawTrackMenu (app);
    else if (app.autoPanelVisible && app.session != nullptr)
        drawAutoPanel (app);
    else if (app.midiPanelVisible && app.midiInputs != nullptr)
        drawMidiPanel (app);
    else if (app.settingsVisible && app.session != nullptr)
        drawSettings (app);
    else if (app.statsVisible && app.session != nullptr)
        drawStats (app);
    else if (app.phoneVisible && app.phone.running())
        drawPhone (app);
}

// "stats": the audio's load, the machine's, and each track's share, heaviest first.
void Ui::drawStats (App& app)
{
    const auto& s = *app.session;
    auto& e = app.engine;
    const auto& sys = app.system;
    const int w = canvas.width(), h = canvas.height();
    // Below the transport when there's room, so its dsp stays in sight.
    const int boxW = std::min (w - 4, 88), x0 = (w - boxW) / 2, y0 = h >= 24 ? 4 : 1, bg = colour::headerBg;
    const int vx = x0 + 11; // where the values start, after the labels
    const int numTracks = s.numTracks();
    const auto blockMs = e.blockMs();

    std::vector<int> order;
    for (int t = 0; t < numTracks; ++t)
        if (e.trackPeakMs (t) > 0.01f)
            order.push_back (t);

    std::sort (order.begin(), order.end(), [&] (int a, int b) { return e.trackMs (a) > e.trackMs (b); });

    const int maxRows = juce::jmax (1, h - y0 - 12);
    const int rows = juce::jmin ((int) order.size(), maxRows);
    const int boxH = 10 + juce::jmax (1, rows) + 1;

    for (int y = 0; y < boxH; ++y)
        canvas.fill (x0, y0 + y, boxW, L' ', -1, bg);

    const auto pct = [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; };
    const auto loadColour = [] (float v) { return v > 0.8f ? colour::error : v > 0.5f ? colour::meterYellow : colour::text; };
    const juce::String dot (L" \x00b7 ");

    canvas.text (x0 + 2, y0 + 1, "STATS", colour::text, bg, true);
    canvas.text (x0 + 9, y0 + 1, "how hard taw and the machine are working", colour::dim, bg, false, boxW - 11);

    // The audio: this block's share of its time, the high point lately, this pass's worst.
    int y = y0 + 3;
    canvas.text (x0 + 2, y, "audio", colour::dim, bg);
    int x = canvas.text (vx, y, "dsp " + pct (e.dspLoad()), loadColour (e.dspLoad()), bg, true);
    x = canvas.text (x, y, dot + "peak " + pct (e.dspPeak()) + " (3 s)", loadColour (e.dspPeak()), bg);

    if (const auto worst = e.worstBlockLoad(); worst > 0.0f)
    {
        x = canvas.text (x, y, dot + "this pass worst " + pct (worst), loadColour (worst), bg);
        const auto late = e.lateBlockCount();
        canvas.text (x, y, dot + juce::String (late) + (late == 1 ? " dropout" : " dropouts"), late > 0 ? colour::error : colour::dim, bg, late > 0);
    }

    ++y;
    juce::String block;

    if (auto* d = e.devices().getCurrentAudioDevice())
        block << juce::String (d->getCurrentSampleRate() / 1000.0, 1) << "k / " << d->getCurrentBufferSizeSamples() << " = ";

    block << juce::String (blockMs, 1) << " ms a block" << dot << "tracks on " << (1 + e.helperThreads()) << " cores";
    canvas.text (vx, y, block, colour::dim, bg, false, boxW - 13);

    // The machine.
    y += 2;
    canvas.text (x0 + 2, y, "machine", colour::dim, bg);

    if (sys.cpu >= 0.0f)
    {
        x = canvas.text (vx, y, "cpu " + pct (sys.cpu), loadColour (sys.cpu), bg, true);
        x = canvas.text (x, y, " (" + juce::String (sys.threads) + " threads)" + dot + "taw " + pct (sys.tawCpu), colour::dim, bg);
        x = canvas.text (x, y, dot + "taw memory " + juce::String (sys.tawMemoryMB / 1024.0, 1) + " GB", colour::dim, bg);
        canvas.text (x, y, dot + juce::String (sys.freeMemoryMB / 1024.0, 1) + " of " + juce::String (juce::roundToInt (sys.totalMemoryMB / 1024.0)) + " GB free",
                     sys.freeMemoryMB < 2048.0 ? colour::meterYellow : colour::dim, bg, false, x0 + boxW - x - 2);
    }
    else
    {
        canvas.text (vx, y, L"measuring\x2026", colour::faint, bg);
    }

    // Each track's time in a block, heaviest first: the bar is its share of one block.
    y += 2;
    const int nameW = juce::jmin (40, boxW - 36), barW = 14;
    canvas.text (x0 + 2, y, "tracks", colour::dim, bg);
    canvas.text (x0 + 4 + nameW + barW + 2, y, "  avg   peak ms", colour::dim, bg);
    ++y;

    if (order.empty())
        canvas.text (x0 + 4, y, "nothing taking time yet", colour::faint, bg);

    for (int i = 0; i < rows; ++i, ++y)
    {
        const auto t = order[(size_t) i];
        const auto tr = s.track (t);
        const auto plugin = tr[ids::pluginName].toString();
        auto name = juce::String (t + 1) + " " + tr[ids::name].toString() + (plugin.isNotEmpty() ? " [" + plugin + "]" : juce::String());
        int fx = 0;

        for (int slot = 0; slot < Session::numSends; ++slot)
            fx += Session::insert (tr, slot).isValid() && ! (bool) Session::insert (tr, slot)[ids::bypass] ? 1 : 0;

        if (fx > 0)
            name << " +" << fx << " fx";

        const auto avg = e.trackMs (t), peak = e.trackPeakMs (t);
        const auto share = blockMs > 0.0 ? (float) (avg / blockMs) : 0.0f;
        const auto peakShare = blockMs > 0.0 ? (float) (peak / blockMs) : 0.0f;
        canvas.text (x0 + 4, y, padRight (name, nameW), colour::text, bg, false, nameW);

        const int filled = juce::jlimit (0, barW, juce::roundToInt (share * barW));
        const int peakAt = juce::jlimit (0, barW - 1, juce::roundToInt (peakShare * barW));

        for (int b = 0; b < barW; ++b)
            canvas.put (x0 + 5 + nameW + b, y, b < filled ? L'\x2588' : b == peakAt ? L'\x2502' : L'\x2591',
                        b < filled ? loadColour (share) : b == peakAt ? loadColour (peakShare) : colour::faint, bg);

        canvas.text (x0 + 6 + nameW + barW, y, juce::String (avg, 2).paddedLeft (' ', 6) + juce::String (peak, 2).paddedLeft (' ', 7),
                     colour::dim, bg);
    }

    if ((int) order.size() > rows)
        canvas.text (x0 + 4, y++, "+" + juce::String ((int) order.size() - rows) + " more", colour::faint, bg);

    canvas.text (x0 + 4, y, padRight ("mix inserts", nameW), colour::dim, bg, false, nameW);
    canvas.text (x0 + 6 + nameW + barW, y, juce::String (e.mixMs(), 2).paddedLeft (' ', 6), colour::dim, bg);

    canvas.text (x0 + 2, y0 + boxH - 1, L"a bar is a track's share of one block (they share the cores) \x00b7 esc or stats closes", colour::faint, bg, false,
                 boxW - 4);
}

// "phone": the phone link's address as a QR code (two modules to a row: half blocks), and what
// has come over it.
void Ui::drawPhone (App& app)
{
    const auto url = app.phone.url();
    const auto code = PhoneLink::qr (url);
    const int w = canvas.width(), h = canvas.height(), bg = colour::headerBg;
    constexpr int quiet = 3, white = 231, black = 16;
    const int size = (int) code.size() + 2 * quiet, codeRows = (size + 1) / 2;
    const bool fits = ! code.empty() && h >= codeRows + 9 && w >= size + 8;
    const int boxW = std::min (w - 4, std::max (76, size + 6)), boxH = (fits ? codeRows + 1 : 0) + 8;
    const int x0 = (w - boxW) / 2, y0 = h >= boxH + 6 ? 4 : 1;

    for (int y = 0; y < boxH; ++y)
        canvas.fill (x0, y0 + y, boxW, L' ', -1, bg);

    canvas.text (x0 + 2, y0 + 1, "PHONE", colour::text, bg, true);
    canvas.text (x0 + 9, y0 + 1, "send recordings from a phone on this network", colour::dim, bg, false, boxW - 11);

    int y = y0 + 3;

    if (fits)
    {
        auto dark = [&] (int mx, int my)
        {
            mx -= quiet;
            my -= quiet;
            return mx >= 0 && my >= 0 && my < (int) code.size() && mx < (int) code.size() && code[(size_t) my][(size_t) mx];
        };

        const int cx = x0 + (boxW - size) / 2;

        for (int r = 0; r < codeRows; ++r, ++y)
            for (int c = 0; c < size; ++c)
                canvas.put (cx + c, y, L'\x2580', dark (c, 2 * r) ? black : white, 2 * r + 1 < size && dark (c, 2 * r + 1) ? black : white);

        ++y;
    }

    canvas.text (x0 + 2, y, fits ? "scan it, or open" : "on the phone, open", colour::dim, bg);
    canvas.text (x0 + (fits ? 19 : 21), y, url, colour::text, bg, true, boxW - 23);
    ++y;

    const auto making = app.phoneStatus();
    const auto status = making.isNotEmpty() ? making : app.phone.status();
    const int waiting = (int) app.phoneWaiting.size();
    auto line = status.isNotEmpty() ? status : juce::String ("nothing sent yet");

    if (waiting > 0)
        line << L" \x00b7 " << waiting << " waiting to go in";

    canvas.text (x0 + 2, y, line, status.isNotEmpty() ? colour::ok : colour::faint, bg, false, boxW - 4);
    canvas.text (x0 + 2, y0 + boxH - 2, L"same wifi \x00b7 the phone warns once (taw's own certificate): go on \x00b7 firewall: let taw through", colour::faint, bg, false, boxW - 4);
    canvas.text (x0 + 2, y0 + boxH - 1, L"lands at the cursor on a new track \x00b7 esc hides \x00b7 phone off ends it", colour::faint, bg, false, boxW - 4);
}

void Ui::drawTitle (App& app)
{
    int x = canvas.text (1, 0, "taw", colour::accent, -1, true);

    if (auto* s = app.session.get())
    {
        x = canvas.text (x, 0, L" \x25b8 " + s->name(), colour::text, -1, true);

        if (app.unsaved)
            x = canvas.text (x + 1, 0, "unsaved", colour::dim); // ctrl+s saves
    }

    int right = canvas.width() - 1; // where the MIDI readout starts

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
        right = lx;

        if (! names.isEmpty())
            canvas.put (canvas.width() - 2, 0, L'\x25cf', now - midiLitAt < 150.0 ? colour::note : colour::faint);
    }

    // Agents (taw --mcp), left of the MIDI: who's connected, a light that blinks with each call,
    // and what their last edit did for a while after (held off by playing, hands off, in).
    {
        struct Piece { juce::String text; int fg; };
        std::vector<Piece> pieces;
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const int connected = app.agentServer != nullptr ? app.agentServer->connections() : 0;

        if (! app.agentsOn)
            pieces.push_back ({ "agents off", colour::faint });
        else if (app.agentServer == nullptr)
        {
            if (app.interactive) // headless taws don't listen
                pieces.push_back ({ "agents: no socket", colour::error });
        }
        else if (! app.agentLastCall.has_value())
            pieces.push_back ({ connected > 0 ? "agent connected" : "agents on", connected > 0 ? colour::dim : colour::faint });
        else
        {
            const auto& last = *app.agentLastCall;
            pieces.push_back ({ L"\x25cf ", now - last.when < 150.0 ? colour::note : connected > 0 ? colour::accent : colour::faint });
            pieces.push_back ({ last.agent, connected > 0 ? colour::dim : colour::faint });

            if (app.agentLastEdit.has_value() && now - app.agentLastEdit->when < 10000.0)
            {
                const auto& e = *app.agentLastEdit;
                const auto rolling = app.engine.isRolling() || app.recording;

                if (e.code.isEmpty())
                    pieces.push_back ({ L"  \x2713 " + e.text, colour::dim });
                else if (e.code == "playing")
                    pieces.push_back ({ rolling ? L"  held: waiting for stop" : L"  held while playing \x00b7 it can edit now", colour::meterYellow });
                else if (e.code == "working")
                    pieces.push_back ({ L"  working\x2026", colour::accent });
                else if (e.code == "hands-off")
                    pieces.push_back ({ L"  hands off: " + e.text, colour::meterYellow });
                else if (e.code == "needs-user")
                    pieces.push_back ({ L"  asks you", colour::accent });
                else
                    pieces.push_back ({ L"  \x2717 " + e.text, colour::error });
            }
        }

        int total = 0;

        for (const auto& p : pieces)
            total += p.text.length();

        const int room = right - 3 - (x + 3);

        if (room >= 10)
        {
            int ax = right - 3 - std::min (total, room);
            const int end = right - 3;

            for (const auto& p : pieces)
                if (ax < end)
                    ax = canvas.text (ax, 0, p.text, p.fg, -1, false, end - ax);
        }
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

    const int w = canvas.width(), y = 3, bg = colour::headerBg; // centred over the timeline
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
    {
        const int byAgent = agentLook (app, s->tree(), "bpm"); // an agent's tempo change: lit, then in their colour
        add (juce::String (s->bpm(), 1) + " bpm " + juce::String (s->beatsPerBar()) + "/" + s->tree()[ids::timeSigDen].toString(),
             byAgent == 2 ? colour::agentText : byAgent == 1 ? colour::agent : colour::dim, byAgent == 2 ? colour::agent : bg, byAgent > 0);
    }
    gap();

    // The beat, blinking: lit for the first 100 ms of each (half a beat when they're quicker),
    // the downbeat brightest. Counting in, the clicks before the playhead, as the engine plays them.
    {
        const auto spb = s->samplesPerBeat();
        const auto at = clicksLeft > 0 ? (double) (app.engine.playhead() - countIn) : (double) pos;
        const auto beat = spb > 0.0 ? std::floor (at / spb) : 0.0;
        const bool lit = (rolling || clicksLeft > 0) && spb > 0.0 && at - beat * spb < std::min (0.1 * s->sampleRate(), 0.5 * spb);
        const bool downbeat = s->beatsPerBar() > 0 && (juce::int64) beat % s->beatsPerBar() == 0;
        add (L"\x25cf ", lit ? (downbeat ? colour::accent : colour::text) : colour::faint, bg, lit);
    }

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

    // An agent tried to edit while this plays: it gets in once it stops.
    if (app.agentHeld > 0 && (rolling || app.recording))
    {
        gap();
        add (L" \x25cf " + app.agentHeldBy + " waiting" + (app.agentHeld > 1 ? L" \x00d7" + juce::String (app.agentHeld) : juce::String()) + " ",
             16, colour::meterYellow, true);
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

    // How hard the audio is working, off the bar on the right in plain grey: its share of each
    // block's time, always (plugins run stopped too). Playing, the slowest block this pass joins
    // it; over 100% is a dropout, counted in red. stats has the rest.
    if (app.engine.isDeviceRunning())
    {
        const auto late = app.engine.lateBlockCount();
        const auto worst = rolling ? app.engine.worstBlockLoad() : 0.0f;
        const auto text = "dsp " + juce::String (juce::roundToInt (app.engine.dspLoad() * 100.0f)) + "%"
                          + (worst > 0.0f ? " peak " + juce::String (juce::roundToInt (worst * 100.0f)) + "%" : juce::String());
        const auto drops = late > 0 ? " " + juce::String (late) + (late == 1 ? " dropout" : " dropouts") : juce::String();
        const int dx = w - 1 - text.length() - drops.length();

        if (dx > x + 2) // only where the bar leaves room
        {
            const int end = canvas.text (dx, y, text, colour::dim);
            canvas.text (end, y, drops, colour::error, -1, true);
        }
    }
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
        {
            for (int c = 0; c < laneW; ++c)
                canvas.put (laneX + c, y, L' ', -1,
                            inTimeSelection (origin + c * spc, origin + (c + 1) * spc) ? colour::selBg : colour::laneFocusBg);

            focusFrame = { 0, y, w - 1, y, {}, colour::accent, true };
        }
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
        {
            canvas.put (0, videoY, L'\x25b8', colour::accent, bg);
            focusFrame = { 0, videoY, w - 1, videoY, {}, colour::accent, true };
        }

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
    const auto numbers = app.numbering(); // a kit's pads aren't numbered as tracks
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
    // The focused track is framed: a row above it and one below (its lanes and playlists inside).
    const int framedTrack = ! app.onTopLane() && ! app.mixFocused() && n > 0 ? std::min (app.focused, n - 1) : -1;
    const int rowsPerTrack = fill ? std::max (1, tracksBottom - tracksTop - 2) : s.trackHeight(); // "height", per session
    auto lanesOf = [&] (int track) { return fill ? std::vector<juce::String>() : app.lanesShown (track); };
    // Open playlists go under their track instead, each as tall as the track.
    auto playlistsOf = [&] (int track) { return fill ? std::vector<juce::ValueTree>() : app.playlistsShown (track); };
    auto blockRows = [&] (int track)
    {
        if (app.trackHidden (track) && track != app.focused) // a folded kit's members
            return 0;

        return rowsPerTrack + (int) lanesOf (track).size() * laneRows + (int) playlistsOf (track).size() * rowsPerTrack + (track == framedTrack ? 2 : 0);
    };

    // Every track's block in the whole list, top to bottom, in rows: the view scrolls by rows,
    // so a track taller than the view (many lanes, an open kit's pads under it) shows part way.
    std::vector<int> blockTop ((size_t) n + 1, 0);

    for (int i = 0; i < n; ++i)
        blockTop[(size_t) i + 1] = blockTop[(size_t) i] + (fill && i != app.focused ? 0 : blockRows (i));

    const int rowsTotal = blockTop[(size_t) n];
    const int area = std::max (1, tracksBottom - tracksTop);
    const int focusRow = std::min (app.focused, n - 1); // the mix row lives below the list

    // The focused track stays in view: all of it if it fits, else the rows you're on (its own,
    // or the lane you're on), clear of its frame's top edge, which stays on the first row.
    if (focusRow >= 0 && ! app.onTopLane() && ! app.mixFocused())
    {
        const int top = blockTop[(size_t) focusRow], rows = blockTop[(size_t) focusRow + 1] - top;
        const int frame = focusRow == framedTrack ? 1 : 0;
        int a = top, b = top + rows;

        if (rows > area)
        {
            const int lane = app.focusedLane, laneH = lanesOf (focusRow).empty() ? rowsPerTrack : laneRows;
            a = lane < 0 ? top : top + frame + rowsPerTrack + lane * laneH - 1; // the row above: the frame's edge
            b = lane < 0 ? top + frame + rowsPerTrack : a + 1 + laneH;

            if (lane >= 0 && lane == app.lanesUnder (focusRow) - 1)
                b += frame; // the last lane: its frame's bottom edge too
        }

        if (a < app.rowScroll)
            app.rowScroll = a;

        if (b > app.rowScroll + area)
            app.rowScroll = b - area;
    }

    app.rowScroll = juce::jlimit (0, std::max (0, rowsTotal - area), app.rowScroll);

    // The tracks with a row in view, the first and last perhaps cut.
    int firstShown = 0, lastShown = 0; // lastShown: exclusive

    while (firstShown < n && blockTop[(size_t) firstShown + 1] <= app.rowScroll)
        ++firstShown;

    for (lastShown = firstShown; lastShown < n && blockTop[(size_t) lastShown] < app.rowScroll + area; ++lastShown) {}

    const int visible = lastShown - firstShown;
    std::vector<int> trackY; // where each shown track's own rows start (inside its frame), above tracksTop when cut

    for (int track = firstShown; track < lastShown; ++track)
        trackY.push_back (tracksTop + blockTop[(size_t) track] - app.rowScroll + (track == framedTrack ? 1 : 0));

    const int tracksEnd = tracksTop + rowsTotal - app.rowScroll; // past the bottom while there's more

    // Out of sight: whole tracks each way (shown when scrolled to; a folded kit's pads aren't),
    // and rows.
    int tracksAbove = 0, tracksBelow = 0;

    for (int i = 0; i < n; ++i)
        if (blockTop[(size_t) i + 1] > blockTop[(size_t) i])
        {
            if (blockTop[(size_t) i + 1] <= app.rowScroll)
                ++tracksAbove;
            else if (blockTop[(size_t) i] >= app.rowScroll + area)
                ++tracksBelow;
        }

    const int rowsAbove = app.rowScroll, rowsBelow = std::max (0, rowsTotal - app.rowScroll - area);

    // The bar: in the border after the timeline, a thumb as long as the share of rows in view,
    // in half rows. It keeps off an end while there's more past it, so a glance says which way.
    // Always there: all of it in view is a thumb the whole height.
    if (! fill && area >= 2 && n > 0 && rowsTotal <= area)
        scrollBar = { meterX - 1, tracksTop, tracksBottom, 0, 0, true, true };
    else if (! fill && area >= 2 && rowsTotal > area)
    {
        const int halves = area * 2;
        int len = std::max (2, (int) std::lround ((double) halves * area / rowsTotal));
        int from = (int) std::lround ((double) halves * rowsAbove / rowsTotal);
        len = std::min (len, halves - (rowsAbove > 0 ? 1 : 0) - (rowsBelow > 0 ? 1 : 0));
        from = juce::jlimit (rowsAbove > 0 ? 1 : 0, halves - len - (rowsBelow > 0 ? 1 : 0), from);
        scrollBar = { meterX - 1, tracksTop, tracksBottom, from, from + len, true };
    }

    // Where things were drawn, for the mouse: which track (and lane) each row is.
    laneMap = { laneX, laneW, rulerY, tracksTop, rowsPerTrack, firstShown, visible, videoY, {}, origin, spc, true };
    laneMap.ioX = io ? ioX : -1;
    laneMap.mixY = masterY;
    laneMap.headerW = leftW;

    for (int track = firstShown; track < lastShown; ++track)
    {
        if (blockTop[(size_t) track + 1] == blockTop[(size_t) track])
            continue;

        if (track == framedTrack) // a click on its frame is a click on the track
            laneMap.rows.push_back ({ track, -1 });

        laneMap.rowIn.resize (laneMap.rows.size(), -1);

        for (int r = 0; r < rowsPerTrack; ++r)
        {
            laneMap.rows.push_back ({ track, -1 });
            laneMap.rowIn.push_back (r);
        }

        for (int lane = 0; lane < (int) lanesOf (track).size(); ++lane)
            for (int r = 0; r < laneRows; ++r)
                laneMap.rows.push_back ({ track, lane });

        for (int lane = 0; lane < (int) playlistsOf (track).size(); ++lane)
            for (int r = 0; r < rowsPerTrack; ++r)
                laneMap.rows.push_back ({ track, lane });

        if (track == framedTrack)
            laneMap.rows.push_back ({ track, -1 });
    }

    // From the first row in view, as many as there are.
    if (n > 0)
    {
        auto& rows = laneMap.rows;
        auto& rowIn = laneMap.rowIn;
        rowIn.resize (rows.size(), -1);
        const auto cut = std::min ((int) rows.size(), app.rowScroll - blockTop[(size_t) firstShown]);
        rows.erase (rows.begin(), rows.begin() + cut);
        rowIn.erase (rowIn.begin(), rowIn.begin() + cut);
        rows.resize (std::min (rows.size(), (size_t) area));
        rowIn.resize (rows.size(), -1);
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
            const auto filled = level <= 0.0 ? 0 : juce::jlimit (1, 8, juce::roundToInt (level * 8.0));
            return { filled == 8 ? L' ' : eighths[filled], filled == 8 };
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
    auto drawNotes = [&] (int i, const juce::ValueTree& t, int y, int laneBg, int panelBg, bool rowInTimeSelection, bool live)
    {
        struct LaneNote
        {
            double s0, s1;
            int pitch;
            bool live;
        };

        std::vector<LaneNote> notes;
        const auto spb = s.samplesPerBeat();
        const bool kitRow = Session::isKit (t);
        const int noteColour = kitRow ? colour::hit : colour::note;

        // The clips, in samples. The one a pass is recording into reaches to the playhead.
        const bool recordingHere = live && std::find (app.midiRecordTracks.begin(), app.midiRecordTracks.end(), i) != app.midiRecordTracks.end();
        const auto passClip = recordingHere && app.midiPassClips.count (i) > 0 ? app.midiPassClips.at (i) : juce::ValueTree();
        std::vector<std::pair<double, double>> clipSpans;

        if (kitRow)
        {
            // A kit: every pad's hits together, like a drum part in a MIDI clip, each a step long
            // on its pad's row (the pads in use, in note order, one per half row when they fit).
            const auto members = app.kitMembers (i);
            std::vector<int> used;

            for (auto m : members)
                if (! Session::placedHits (s.track (m)).empty())
                    used.push_back ((int) s.track (m).getProperty (ids::note, 0));

            std::sort (used.begin(), used.end());
            used.erase (std::unique (used.begin(), used.end()), used.end());
            for (auto m : members)
            {
                const auto member = s.track (m);
                const auto row = (int) (std::lower_bound (used.begin(), used.end(), (int) member.getProperty (ids::note, 0)) - used.begin());
                const auto memberPass = live && app.recording && app.midiPassClips.count (m) > 0 ? app.midiPassClips.at (m) : juce::ValueTree();

                for (const auto& hit : Session::placedHits (member))
                    notes.push_back ({ hit.beat * spb, hit.beat * spb + 1.0, row, false }); // a mark in its column; higher pads higher, as pitches

                for (const auto& c : Session::hitClips (member))
                    clipSpans.push_back ({ (double) c[ids::beat] * spb, std::max (Session::clipEnd (c) * spb, c == memberPass ? (double) playhead : 0.0) });
            }

            // One span per stretch the members' clips cover together.
            std::sort (clipSpans.begin(), clipSpans.end());
            std::vector<std::pair<double, double>> merged;

            for (const auto& span : clipSpans)
            {
                if (! merged.empty() && span.first <= merged.back().second)
                    merged.back().second = std::max (merged.back().second, span.second);
                else
                    merged.push_back (span);
            }

            clipSpans = std::move (merged);
        }
        else
        {
            for (const auto& p : Session::placedNotes (t))
                notes.push_back ({ p.start * spb, p.end * spb, (int) p.node[ids::pitch], false });

            for (const auto& c : Session::midiClips (t))
                clipSpans.push_back ({ (double) c[ids::beat] * spb, std::max (Session::clipEnd (c) * spb, c == passClip ? (double) playhead : 0.0) });
        }

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
        if (fill && ! kitRow)
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
                    fg = std::max (up, down) == 2 ? colour::liveWave : noteColour;
                }
                else if (edgeCh != 0)
                {
                    ch = edgeCh, fg = noteColour;
                }
                else if (inClip >= 0 && (r == 0 || r == rowsPerTrack - 1))
                {
                    ch = L' ', fg = noteColour; // the outline is drawn in the cell's colour: no grid marks on it
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

    // The tracks draw only on their own rows: one cut by the scroll draws its rows out of view
    // into nothing (the clip ends after their lanes, below).
    canvas.clip (tracksTop, tracksBottom);

    for (int row = 0; row < visible; ++row)
    {
        const int i = firstShown + row, y = trackY[(size_t) row];

        if (blockTop[(size_t) i + 1] == blockTop[(size_t) i]) // a folded kit's pad (or, filling, another track)
            continue;

        const auto t = s.track (i);
        const bool focusedRow = i == app.focused && ! app.onTopLane();
        const bool selectedRow = std::find (selTracks.begin(), selTracks.end(), i) != selTracks.end();
        const int byAgent = agentLook (app, t); // an agent just changed it: a flash, then its name in their colour
        const int bg = byAgent == 2 ? colour::agent : app.trackFlashing (t) ? colour::accent : focusedRow ? colour::focusBg : (selectedRow ? colour::selBg : -1);
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
        auto chip = [&] (const juce::String& text, int chipBg)
        {
            const int x = leftW - 6;
            canvas.fill (x, y, 6, L' ', -1, chipBg);
            canvas.text (x + 1, y, text, chipBg == colour::busColour ? colour::busText : colour::chipText, chipBg, true, 5);
        };

        // A pad shows no number: it's a lane of its kit, the kit's line down its left.
        if (app.kitOf (t) < 0)
            canvas.text (1, y, numbers.label (i).paddedLeft (' ', 2), colour::text, bg, true);
        // The main playlist's name, as Pro Tools shows it (the track's own until there are others).
        canvas.text (4, y, padRight (Session::mainPlaylistName (t), 10), byAgent == 2 ? colour::agentText : byAgent == 1 ? colour::agent : colour::text, bg, byAgent > 0);

        const int flagX = twoRows ? 4 : 15, flagY = twoRows ? y + 1 : y;
        auto flag = [&] (int x, bool on, wchar_t ch, int fg) { canvas.put (x, flagY, on ? ch : L'\x00b7', on ? fg : colour::faint, bg, on); };
        // R I S M: arm, input, solo, mute. A bus has nothing to arm or monitor: just solo and mute.
        const bool busRow = Session::isBusTrack (t);

        if (! busRow)
        {
            flag (flagX, t[ids::armed], L'R', colour::rec);
            flag (flagX + 1, t[ids::monitor], L'I', colour::wave);
        }
        else if (Session::isKit (t)) // a kit arms for its pads
            flag (flagX, t[ids::armed], L'R', colour::rec);

        flag (flagX + 2, t[ids::solo], L'S', colour::play);
        flag (flagX + 3, app.currentValue (t, "mute") >= 0.5, L'M', app.isAutomated (t, "mute") ? colour::note : colour::meterYellow);

        const bool sampleTrack = Session::isSampleTrack (t);
        const bool instrumentTrack = Session::isInstrumentTrack (t);
        const bool busTrack = Session::isBusTrack (t);
        const int input = t[ids::input];

        // A kit: its fold left of the name; its members a line down from it and their pad.
        if (Session::isKit (t))
            canvas.put (3, y, (bool) t[ids::folded] ? L'\x25b8' : L'\x25be', colour::hit, bg, true);
        else if (const auto kit = app.kitOf (t); kit >= 0)
            for (int r = 0; r < rowsPerTrack; ++r)
                canvas.put (3, y + r, L'\x2502', colour::faint, bg);

        if (Session::isKit (t))
            chip ("kit", colour::hit);
        else if (busTrack)
            chip ("bus", typeColour);
        else if (sampleTrack && app.kitOf (t) >= 0 && t.hasProperty (ids::note))
        {
            const int note = t[ids::note];
            chip (note >= App::firstPadNote && note < App::firstPadNote + 16 ? "pad" + juce::String (note - App::firstPadNote + 1) : "n" + juce::String (note), typeColour);
        }
        else if (sampleTrack)
            chip ("smp", typeColour);
        else if (instrumentTrack)
        {
            // "inst", red when its plugin didn't load; the plugin's name after the flags. While its
            // window is open both light up in the plugin colour (as "keys → plugin" does), so with
            // several of the same plugin it's plain which one the window belongs to.
            const auto problem = app.instrumentProblem (i);
            const bool pending = app.pluginPending (t);
            const bool open = problem.isEmpty() && app.editorOpen (t);
            chip ("inst", problem.isNotEmpty() ? colour::error : pending ? colour::dim : open ? colour::accent : typeColour);

            if (twoRows && open)
                canvas.text (9, y + 1, L"\x25b8 " + t[ids::pluginName].toString(), 16, colour::accent, true, fill ? 6 : leftW - 10);
            else if (twoRows)
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

        // Lane: the track's clips at rowY, or (live false) one of its playlists in a lane under it,
        // without what's recording.
        auto drawContent = [&] (const juce::ValueTree& source, int rowY, int contentBg, bool rowSelected, bool live)
        {
            struct LaneClip
            {
                const ClipData* data;
                juce::int64 start, offset, length;
                juce::int64 fadeIn, fadeOut; // as set (the declick isn't drawn)
            };

            std::vector<LaneClip> laneClips;
            std::vector<std::pair<double, float>> laneHits; // sample tracks: (position in samples, velocity)
            std::vector<std::pair<double, double>> hitSpans;  // ...and their clips, in samples

            if (sampleTrack)
            {
                for (const auto& hit : Session::placedHits (source))
                    laneHits.push_back ({ hit.beat * s.samplesPerBeat(), hit.vel });

                std::sort (laneHits.begin(), laneHits.end());

                // The clip a pass is recording into reaches to the playhead.
                const auto passClip = live && app.recording && app.midiPassClips.count (i) > 0 ? app.midiPassClips.at (i) : juce::ValueTree();

                for (const auto& c : Session::hitClips (source))
                    hitSpans.push_back ({ (double) c[ids::beat] * s.samplesPerBeat(),
                                          std::max (Session::clipEnd (c) * s.samplesPerBeat(), c == passClip ? (double) playhead : 0.0) });
            }
            else
            {
                for (const auto& c : Session::clips (source))
                    if (auto it = app.clips.find (c[ids::file].toString()); it != app.clips.end())
                        laneClips.push_back ({ &it->second, (juce::int64) c[ids::start], (juce::int64) c[ids::offset], (juce::int64) c[ids::length],
                                               (juce::int64) c[ids::fadeIn], (juce::int64) c[ids::fadeOut] });
            }

            const LiveTake* liveTake = nullptr;

            for (const auto& l : app.live)
                if (live && l.recorder->trackIndex == i && l.start >= 0)
                    liveTake = &l;

            if (instrumentTrack || Session::isKit (source))
                drawNotes (i, source, rowY, contentBg, bg, rowSelected, live);
            else
            {
                for (int c = 0; c < laneW; ++c)
                {
                    const auto t0 = origin + c * spc, t1 = t0 + spc;
                    wchar_t ch = L' ';
                    int fg = colour::wave, cellBg = contentBg;
                    bool inClip = false;
                    std::optional<WavePoint> wave; // a clip or take here: drawn per row by waveCell
                    wchar_t edgeCh = 0;            // a clip's first or last column
                    wchar_t fadeCh = 0;            // over a fade, on the top row: in, out or crossfade

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

                            const bool fadingIn = lc.fadeIn > 0 && t0 < start + (double) lc.fadeIn;
                            const bool fadingOut = lc.fadeOut > 0 && t1 > end - (double) lc.fadeOut;

                            if (fadingIn || fadingOut)
                            {
                                const bool over = std::any_of (laneClips.begin(), laneClips.begin() + k, [&] (const LaneClip& u)
                                                               { return t1 > (double) u.start && t0 < (double) (u.start + u.length); });
                                fadeCh = over ? L'\x2573' : fadingIn ? L'\x2571' : L'\x2572';
                            }
                            break;
                        }
                    }

                    if (liveTake != nullptr)
                    {
                        // Loop recording: this pass's part, folded back into the loop.
                        auto takeStart = liveTake->start;
                        auto from = (double) takeStart;
                        const auto recorded = (juce::int64) liveTake->overview.size() * Recorder::overviewBlock;

                        if (const auto pass = app.loopRecEnd - app.loopRecStart; app.loopRecording && pass > 0)
                        {
                            takeStart -= (takeStart - app.loopRecStart + recorded) / pass * pass;
                            from = std::max ((double) takeStart, (double) app.loopRecStart);
                        }

                        const auto end = (double) (takeStart + recorded);

                        if (t1 > from && t0 < end)
                        {
                            wave = waveRange (liveTake->overview, nullptr, takeStart, std::max (t0, from), t1);
                            edgeCh = fadeCh = 0;
                            inClip = false;
                            fg = colour::liveWave;
                            cellBg = colour::recBg;
                        }
                    }

                    if (rowSelected && inTimeSelection (t0, t1))
                        cellBg = (cellBg == colour::clipBg || cellBg == colour::clipAltBg) ? colour::selClipBg : colour::selBg;

                    // The playhead and the cursor colour the cell behind what's there, so a clip's edge
                    // or a hit under them still shows; on an empty cell they're a thin line. The
                    // playhead wins when they share a column.
                    const bool playheadHere = rolling && c == playCol;
                    const bool cursorHere = c == cursorCol && ! playheadHere;

                    for (int r = 0; r < rowsPerTrack; ++r)
                    {
                        const auto cell = edgeCh == 0 && wave ? waveCell (*wave, r) : WaveCell { edgeCh != 0 ? edgeCh : ch, false };
                        auto rowCh = r == 0 && fadeCh != 0 ? fadeCh : cell.ch;
                        auto rowFg = r == 0 && fadeCh != 0 ? colour::accent : fg;
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

                        canvas.put (laneX + c, rowY + r, rowCh, rowFg, rowBg);

                        // A clip's outline: its top along the first row, its bottom along the last.
                        if (inClip)
                        {
                            canvas.at (laneX + c, rowY + r).over = r == 0;
                            canvas.at (laneX + c, rowY + r).under = r == rowsPerTrack - 1;
                        }
                    }
                }
            }
        };

        drawContent (t, y, laneBg, rowInTimeSelection, true);

        // A bus lane says what feeds it (a kit's shows its pads' hits; an empty one says how to fill it).
        if (Session::isKit (t) && app.kitMembers (i).empty())
            canvas.text (laneX + 1, y, L"kit \x00b7 no pads yet \x00b7 ctrl+B: pick a pad and audition onto it \x00b7 pad 1 <file>", colour::dim, laneBg, false, laneW - 2);
        else if (busTrack && ! Session::isKit (t))
        {
            juce::StringArray from;

            for (auto f : app.busFeeders (i))
                from.add (numbers.label (f) + " " + s.track (f)[ids::name].toString());

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
                const auto insert = Session::insert (t, k);
                const bool open = insert.isValid() && app.editorOpen (insert); // lit like an open instrument
                const int x = ioX + ioSlots + k * ioSlotW, cbg = open ? colour::accent : cellBg (App::ioFirstInsert + k);
                canvas.fill (x - 1, y, ioSlotW, L' ', -1, cbg);

                if (! insert.isValid())
                    canvas.put (x, y, L'\x00b7', colour::faint, cbg);
                else if (open)
                    canvas.text (x, y, insert[ids::pluginName].toString(), 16, cbg, true, ioSlotW - 1);
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
                    // Its level just nudged by a key: the cell lit (name and level).
                    // Changed by an agent: lit in their colour, then the name in it.
                    const int sendAgent = agentLook (app, t, "send " + Session::slotName (k).toLowerCase());
                    const bool flash = sendAgent == 0 && app.valueFlashing (t, "send " + Session::slotName (k).toLowerCase());
                    const int x = ioX + ioSlots + k * ioSlotW, cbg = sendAgent == 2 ? colour::agent : flash ? colour::accent : cellBg (App::ioFirstSend + k);
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
                                 sendAgent == 2 ? colour::agentText : sendAgent == 1 ? colour::agent
                                 : flash ? 16 : dest < 0 ? colour::error : off ? colour::faint : colour::text,
                                 cbg, flash || sendAgent > 0, ioSlotW - 1);

                    // The level under the name; pre-fader sends in the accent colour.
                    if (rowsPerTrack > 2)
                    {
                        const auto sendParam = "send " + Session::slotName (k).toLowerCase();
                        const double db = app.currentValue (t, sendParam);
                        const auto level = db <= -95.9 ? juce::String (L"-\x221e") : juce::String (juce::roundToInt (db));
                        canvas.fill (x - 1, y + 2, ioSlotW, L' ', -1, cbg);
                        canvas.text (x, y + 2, level,
                                     sendAgent == 2 ? colour::agentText : sendAgent == 1 ? colour::agent
                                     : flash ? 16 : off ? colour::faint : app.isAutomated (t, sendParam) ? colour::note : (bool) send[ids::pre] ? colour::accent : colour::dim,
                                     cbg, flash || sendAgent > 0, ioSlotW - 1);
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
        drawVolume (meterX, y, app.currentValue (t, "vol"), bg, app.isAutomated (t, "vol"), app.valueFlashing (t, "vol"), agentLook (app, t, "vol"));
        drawDb (meterX + 12, y, m.hold, m.clipped, bg);

        // Pan under the volume (centre dim, like unity); "st" marks a stereo track.
        if (rowsPerTrack > 1)
        {
            const double pan = app.currentValue (t, "pan");
            const bool flash = app.valueFlashing (t, "pan");
            const int panAgent = agentLook (app, t, "pan");
            canvas.text (meterX, y + 1, App::panText (pan).paddedLeft (' ', 5),
                         panAgent == 2 ? colour::agentText : panAgent == 1 ? colour::agent
                         : flash ? 16 : app.isAutomated (t, "pan") ? colour::note : std::abs (pan) < 0.005 ? colour::dim : colour::text,
                         panAgent == 2 ? colour::agent : flash && panAgent == 0 ? colour::accent : bg, flash || panAgent > 0);

            if (app.trackIsStereo (t))
                canvas.text (meterX + 12, y + 1, "   st", colour::faint, bg);
        }

        for (int r = 0; r < rowsPerTrack; ++r)
            drawMeter (meterX + 6, y + r, 5, m.level, colour::meterBg);

        // Playlists (shift+P), under the track as tall as it: each one's clips, and on the left
        // its number (pl <n>), its name, and S lit while it's soloed in the main's place.
        const auto playlists = playlistsOf (i);

        for (int k = 0; k < (int) playlists.size(); ++k)
        {
            const auto& p = playlists[(size_t) k];
            const int py = y + rowsPerTrack * (k + 1);
            const bool focusedLane = focusedRow && app.focusedLane == k;
            const int pbg = focusedLane ? colour::focusBg : -1;
            const bool lastLane = k == (int) playlists.size() - 1;
            const bool soloed = p[ids::solo];

            for (int r = 0; r < rowsPerTrack; ++r)
            {
                canvas.fill (0, py + r, leftW, L' ', -1, pbg);
                canvas.fill (meterX, py + r, meterPanel, L' ', -1, pbg);

                if (io)
                    canvas.fill (ioX, py + r, ioPanel, L' ', -1, pbg);

                borders (py + r);
                canvas.put (0, py + r, focusedLane && r == 0 ? L'\x25b8' : L'\x2502', focusedLane ? colour::accent : colour::faint, pbg);
                canvas.put (2, py + r, r == 0 ? (lastLane ? L'\x2514' : L'\x251c') : (lastLane ? L' ' : L'\x2502'), colour::faint, pbg);
            }

            canvas.text (4, py, juce::String (k + 1), colour::dim, pbg);
            canvas.text (4 + juce::String (k + 1).length() + 1, py, p[ids::name].toString(), focusedLane ? colour::text : colour::dim, pbg, focusedLane,
                         leftW - 10 - juce::String (k + 1).length());
            canvas.put (leftW - 3, py, soloed ? L'S' : L'\x00b7', soloed ? colour::play : colour::faint, pbg, soloed);

            // The selection lights the lane you're on (its edits go to it).
            drawContent (p, py, focusedLane ? colour::laneFocusBg : selectedRow ? colour::laneSelBg : -1, timeSel && (focusedLane || app.onTopLane()), false);
        }
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

            const int laneAgent = agentLook (app, t, "auto " + param); // an agent just wrote it
            const auto nameEnd = canvas.text (indent, y, app.automationName (t, param, false),
                                              laneAgent == 2 ? colour::agentText : laneAgent == 1 ? colour::agent : focusedLane ? colour::text : colour::dim,
                                              laneAgent == 2 ? colour::agent : bg, focusedLane || laneAgent > 0, leftW - indent - 1 - (off ? 4 : 0));

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
        const int i = firstShown + (int) row;

        if (blockTop[(size_t) i + 1] == blockTop[(size_t) i]) // a folded kit's pad (or, filling, another track)
            continue;

        if (const auto lanes = lanesOf (i); ! lanes.empty())
            drawLanes (i, s.track (i), lanes, trackY[row] + rowsPerTrack, std::find (selTracks.begin(), selTracks.end(), i) != selTracks.end());

        // The focused track's frame, its name in the top edge: record-red while it's armed, so
        // a glance says you're on the track that records.
        if (i == framedTrack)
        {
            const auto t = s.track (i);
            const bool armed = (bool) t[ids::armed];
            const auto state = ! armed ? juce::String() : app.recording ? juce::String (L" \x00b7 \x25cf recording") : juce::String (L" \x00b7 armed");
            const int top = trackY[row] - 1;
            // A pad's frame names its kit and pad: "2.3 808SD (pad 3 of Kit 1)".
            const auto kit = app.kitOf (t);
            const auto where = kit < 0 ? juce::String() : " (" + app.padName (t.getProperty (ids::note, -1)) + " of " + s.track (kit)[ids::name].toString() + ")";

            // What's open under it, and the key that folds it away.
            juce::String fold;
            const auto openLanes = (int) lanesOf (i).size(), openPlaylists = (int) playlistsOf (i).size();
            const auto pads = Session::isKit (t) && ! (bool) t[ids::folded] ? (int) app.kitMembers (i).size() : 0;

            if (openLanes > 0)
                fold = juce::String (openLanes) + (openLanes == 1 ? " lane" : " lanes") + L": shift+A folds";
            else if (openPlaylists > 0)
                fold = juce::String (openPlaylists) + (openPlaylists == 1 ? " playlist" : " playlists") + L": shift+P folds";

            if (pads > 0)
                fold << (fold.isEmpty() ? juce::String() : juce::String (L" \x00b7 ")) << juce::String (pads) << (pads == 1 ? " pad" : " pads") << L": shift+F folds";
            else if (kit >= 0)
                fold << (fold.isEmpty() ? juce::String() : juce::String (L" \x00b7 ")) << L"shift+F folds the kit";

            // Cut by the scroll: the top edge stays on the first row, with the name; the
            // bottom is left open, there's more of it below.
            const int bottom = top + blockRows (i) - 1;
            focusFrame = { 0, std::max (top, tracksTop), w - 1, std::min (bottom, tracksBottom),
                           numbers.label (i) + " " + t[ids::name].toString() + where + state + (fold.isEmpty() ? juce::String() : L" \x00b7 " + fold),
                           armed ? colour::rec : colour::accent, true, bottom >= tracksBottom };
        }
    }

    canvas.unclip();

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
            drawVolume (meterX, y, app.currentValue (s.tree(), "vol"), bg, app.isAutomated (s.tree(), "vol"), app.valueFlashing (s.tree(), "vol"), agentLook (app, s.tree(), "vol"));
        }

        canvas.text (leftW - 2, y, ch == 0 ? "L" : "R", colour::dim, bg);

        // The mix's inserts A-E in the routing column (after the mix volume, as on a master).
        if (io && ch == 0)
        {
            const bool inColumn = mixFocused && app.ioCell >= 0;

            for (int k = 0; k < Session::numSends; ++k)
            {
                const int x = ioX + ioSlots + k * ioSlotW;
                const auto insert = Session::insert (s.tree(), k);
                const bool open = insert.isValid() && app.editorOpen (insert);
                const int cbg = open ? colour::accent : inColumn && app.ioCell == App::ioFirstInsert + k ? colour::selBg : bg;
                canvas.fill (x - 1, y, ioSlotW, L' ', -1, cbg);

                if (! insert.isValid())
                    canvas.put (x, y, L'\x00b7', colour::faint, cbg);
                else if (open)
                    canvas.text (x, y, insert[ids::pluginName].toString(), 16, cbg, true, ioSlotW - 1);
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
            canvas.text (laneX + 1, y, "track " + numbers.label (app.focused) + " of " + juce::String ((int) numbers.byNumber.size()) + L" filling the view \x00b7 \x2191\x2193 others \x00b7 f shows all",
                         colour::faint);
        else if (ch == 0 && n > 0 && rowsTotal <= area)
            canvas.text (laneX + 1, y, (numbers.byNumber.size() == 1 ? juce::String ("the track") : "all " + juce::String ((int) numbers.byNumber.size()) + " tracks") + " in view",
                         colour::faint, -1, false, laneW - 1);
        else if (ch == 0 && rowsTotal > area)
        {
            // What's in view against the whole list: the tracks (first and last shown, a folded
            // kit's pads not counted), how many are out of sight each way, and the rows.
            int firstLabel = -1, lastLabel = -1;

            for (int i = firstShown; i < lastShown; ++i)
                if (blockTop[(size_t) i + 1] > blockTop[(size_t) i])
                    lastLabel = i, firstLabel = firstLabel < 0 ? i : firstLabel;

            juce::StringArray hidden;

            if (tracksAbove > 0)
                hidden.add (juce::String (L"\x2191") + juce::String (tracksAbove));

            if (tracksBelow > 0)
                hidden.add (juce::String (L"\x2193") + juce::String (tracksBelow));

            const auto range = firstLabel < 0 ? juce::String()
                             : firstLabel == lastLabel ? "track " + numbers.label (firstLabel)
                                                       : "tracks " + numbers.label (firstLabel) + "-" + numbers.label (lastLabel);
            const int shownTo = std::min (rowsTotal, app.rowScroll + area);
            canvas.text (laneX + 1, y, range + " of " + juce::String ((int) numbers.byNumber.size())
                                           + (hidden.isEmpty() ? juce::String() : " (" + hidden.joinIntoString (" ") + ")")
                                           + L" \x00b7 rows " + juce::String (app.rowScroll + 1) + "-" + juce::String (shownTo) + " of " + juce::String (rowsTotal)
                                           + (rowsAbove > 0 && rowsBelow > 0 ? L" \x00b7 \x2191\x2193 to scroll" : rowsBelow > 0 ? L" \x00b7 \x2193 for more" : L" \x00b7 \x2191 for more"),
                         colour::dim, -1, false, laneW - 1);
        }

        const auto& m = app.masterMeters[(size_t) ch];
        drawMeter (meterX + 6, y, 5, m.level, colour::meterBg);
        drawDb (meterX + 12, y, m.hold, m.clipped, bg);
    }

    drawLanes (-1, s.tree(), mixLanes, masterY + 2, false);

    // The Mix's frame: the free row above it, and the separator row under its lanes.
    if (mixFocused)
        focusFrame = { 0, masterY - 1, w - 1, masterY + 2 + (int) mixLanes.size() * laneRows, "Mix", colour::accent, true };
}

void Ui::drawFocusFrame()
{
    const auto& f = focusFrame;

    if (! f.valid || f.y0 < 0 || f.y1 >= canvas.height() || f.x1 >= canvas.width())
        return;

    auto side = [&] (int x, int y) { canvas.put (x, y, L'\x2503', f.colour, canvas.at (x, y).bg, true); };

    // A top lane is one row with neighbours above and below: its sides only.
    if (f.y0 == f.y1)
    {
        side (f.x0, f.y0);
        side (f.x1, f.y0);
        return;
    }

    for (int x = f.x0 + 1; x < f.x1; ++x)
    {
        canvas.put (x, f.y0, L'\x2501', f.colour);

        if (! f.openBottom)
            canvas.put (x, f.y1, L'\x2501', f.colour);
    }

    for (int y = f.y0 + 1; y < f.y1; ++y)
    {
        side (f.x0, y);
        side (f.x1, y);
    }

    canvas.put (f.x0, f.y0, L'\x250f', f.colour);
    canvas.put (f.x1, f.y0, L'\x2513', f.colour);

    if (! f.openBottom)
    {
        canvas.put (f.x0, f.y1, L'\x2517', f.colour);
        canvas.put (f.x1, f.y1, L'\x251b', f.colour);
    }

    if (f.title.isNotEmpty())
        canvas.text (f.x0 + 2, f.y0, " " + f.title + " ", f.colour, -1, true, f.x1 - f.x0 - 3);
}

// The track list's scroll bar: a grey track down the border, always there; with more rows than
// fit, a thumb in the accent colour over it, in half blocks (greyed: just the track).
void Ui::drawScrollBar()
{
    const auto& b = scrollBar;

    if (! b.valid || b.x < 0 || b.x >= canvas.width() || b.y1 > canvas.height())
        return;

    for (int y = b.y0; y < b.y1; ++y)
    {
        const int top = (y - b.y0) * 2;
        const bool upper = ! b.greyed && top >= b.from && top < b.to, lower = ! b.greyed && top + 1 >= b.from && top + 1 < b.to;

        if (upper && lower)
            canvas.put (b.x, y, L'\x2588', colour::accent);
        else if (upper || lower)
            canvas.put (b.x, y, upper ? L'\x2580' : L'\x2584', colour::accent, colour::faint);
        else
            canvas.put (b.x, y, L'\x2588', colour::faint);
    }
}

// Text in the command line changes what keys do (space and letters type instead of playing),
// so it's plain at a glance: the line over it lit and saying so, the line itself lit, and a
// chip at its end with the way out.
void Ui::drawTyping (App& app)
{
    if (app.commandLine.isEmpty() || app.namingMarker.isValid())
        return; // naming a marker has its own label

    const int w = canvas.width(), h = canvas.height(), sepY = h - 4, cmdY = h - 2;

    canvas.fill (0, sepY, w, L'\x2501', colour::accent);
    canvas.text (2, sepY, L" TYPING \x00b7 space and letters go to the command line ", colour::accent, -1, true, w - 4);

    int used = 0; // past what's on the row: a status label, the prompt, the text and its cursor

    for (int x = 0; x < w; ++x)
    {
        auto& cell = canvas.at (x, cmdY);

        if (cell.ch != L' ' || cell.bg >= 0)
            used = x + 1;

        if (cell.bg < 0)
            cell.bg = colour::focusBg;
    }

    const juce::String chip (L" enter runs it \x00b7 esc clears ");

    if (const int chipX = w - 1 - chip.length(); chipX > used + 1)
        canvas.text (chipX, cmdY, chip, 16, colour::accent, true);
}

// A shortcut typed in keys mode does nothing (the letters are notes there), so for a couple of
// seconds the line over the command line says so in red, as the typing one does, with the way out.
void Ui::drawKeysWarning (App& app)
{
    if (! app.liveKeys.on() || juce::Time::getMillisecondCounterHiRes() - app.keysWarnedAt > 2000.0)
        return;

    const int w = canvas.width(), sepY = canvas.height() - 4;
    canvas.fill (0, sepY, w, L'\x2501', colour::error);
    canvas.text (2, sepY, L" KEYS MODE \x00b7 letters play notes, not shortcuts \x00b7 esc or ctrl+k leaves it ", 231, colour::error, true, w - 4);
}

void Ui::drawVolume (int x, int y, double db, int bg, bool automated, bool flash, int byAgent)
{
    // Unity is dim so a changed fader stands out; automation (or a knob) moving it, its colour;
    // just nudged by a key, lit; just changed by an agent, lit in theirs, then in their colour.
    const auto text = formatDb (db).paddedLeft (' ', 5);

    if (byAgent == 2)
        canvas.text (x, y, text, colour::agentText, colour::agent, true);
    else if (byAgent == 1)
        canvas.text (x, y, text, colour::agent, bg, true);
    else if (flash)
        canvas.text (x, y, text, 16, colour::accent, true);
    else
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

    if (app.liveKeys.on() && app.keysPlayKit())
        hints = L"PADS  z x c v 1-4 \x00b7 a s d f 5-8 \x00b7 q w e r 9-12 \x00b7 1 2 3 4 13-16 \x00b7 shift accent \x00b7 space play \x00b7 ctrl+space record \x00b7 esc or ctrl+k leaves";
    else if (app.liveKeys.on())
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
        hints = L"ctrl c copy \x00b7 x cut \x00b7 d duplicate \x00b7 e separate \x00b7 del clear \x00b7 alt+shift+3 consolidate \x00b7 l loop \x00b7 b bounce (b mp3) \x00b7 pat x... fills it \x00b7 esc clears \x00b7 shift \x2190\x2192 extend";
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
    // Pages of two columns of sections, each line "what it does   keys", so it fits a small
    // screen (80 x 24): the action in plain text, what you press or type in yellow. The tabs run
    // along the top; a page's note spans the box under both columns.
    struct Entry { const wchar_t* action; const wchar_t* keys; };
    struct Section { const wchar_t* title; std::vector<Entry> entries; };
    struct Page { const wchar_t* name; std::vector<Section> left, right; const wchar_t* note = nullptr; };

    static const std::vector<Page> pages = {
        { L"MOVE",
          { { L"MOVE", {
                { L"next grid line (zoom)", L"\x2190 \x2192" },
                { L"4 bars", L"ctrl \x2190 \x2192" },
                { L"1 step (ctrl+alt: 4)", L"alt \x2190 \x2192" },
                { L"start / end", L"home / end" },
                { L"track; ctrl: first/last", L"\x2191 \x2193" },
                { L"bar 17 / 1 min 30 s", L"@17  @1:30" },
                { L"selection start / end", L"q w" },
                { L"next clip edge / hit", L"tab" },
                { L"zoom out / in", L"r t" },
                { L"zoom back", L"enter" } } } },
          { { L"PLAY", {
                { L"play / stop", L"space" },
                { L"loop selection / toggle", L"l" },
                { L"add marker", L"~" },
                { L"markers row", L"`" } } },
            { L"SELECT", {
                { L"select time / tracks", L"shift + any" },
                { L"select bars 3 to 5", L"sel 3 5" },
                { L"select the clip here", L"sel clip" },
                { L"clear line, then sel.", L"esc" },
                { L"clear clip lights", L"alt+C" } } } },
          L"Commands work while playing too." },

        { L"REC",
          { { L"RECORD", {
                { L"arm (+ hear input)", L"shift+R  1r" },
                { L"arm 1 and record", L"1 rec" },
                { L"record armed tracks", L"ctrl+space" },
                { L"punch in/out (playing)", L"ctrl+space" },
                { L"input on playback too", L"shift+I 2mon" } } },
            { L"CLICK & COUNT-IN", {
                { L"click on / off", L"click" },
                { L"only recording / always", L"click rec/a" },
                { L"count-in clicks (off)", L"count 4" },
                { L"count-in on play too", L"count a" } } } },
          { { L"PLAYLISTS (TAKES)", {
                { L"new playlist (empty)", L"ctrl+p" },
                { L"copy of the main", L"ctrl+alt+p" },
                { L"playlists as lanes", L"shift+P" },
                { L"lane plays as the main", L"shift+S lane" },
                { L"list / make it the main", L"pl  pl 2" },
                { L"lane selection to main", L"alt+shift+\x2191" },
                { L"previous / next main", L"ctrl+alt+\x2191\x2193" } } } },
          L"Loop on, recording: each pass a take, the last on the main." },

        { L"TRACKS",
          { { L"TRACKS", {
                { L"new track on input 2", L"add vox in2" },
                { L"3 audio, inst or sample", L"add 3 inst" },
                { L"rename", L"2n Vox" },
                { L"use input 3", L"2i3" },
                { L"stereo (inputs 3-4)", L"2 stereo" },
                { L"mute / solo (2m 2s)", L"shift+M  S" },
                { L"move up / down", L"alt \x2191 \x2193" },
                { L"duplicate (+ empty)", L"2 dup track" },
                { L"delete (undo ok)", L"2 remove" },
                { L"drop latest clip", L"2x" },
                { L"track menu", L"shift+T" },
                { L"rows per track (1-4)", L"height 3" } } } },
          { { L"VOLUME & PAN", {
                { L"track -1 / +1 dB", L"[  ]" },
                { L"track volume to -6 dB", L"2v-6" },
                { L"mix -1 / +1 dB", L"{ } ctrl [ ]" },
                { L"mix volume to -3 dB", L"mixv-3" },
                { L"pan (alt+[ ] nudges)", L"2 pan l30" } } } },
          L"Track numbers first: 2m, 1-3m, 2v-6. None: the selection or the \x25b8 track." },

        { L"EDIT",
          { { L"EDIT", {
                { L"copy / cut / paste", L"ctrl c x v" },
                { L"duplicate after itself", L"ctrl d" },
                { L"clear the range", L"delete" },
                { L"undo / redo", L"ctrl z / y" },
                { L"consolidate (cons)", L"alt+shift+3" },
                { L"cut clips here (sep)", L"ctrl e" } } } },
          { { L"FADES", {
                { L"fade / crossfade sel.", L"ctrl f" },
                { L"crossfade nearest here", L"xfade 20ms" },
                { L"in 10ms, out 1/4, off", L"clip fade" } } },
            { L"MIDI", {
                { L"piano roll", L"f" },
                { L"MIDI / audio editor", L"shift+E" } } } },
          L"Automation goes with the clips: copy, cut, paste and duplicate carry it." },

        { L"SAMPLES",
          { { L"SAMPLES", {
                { L"new sample track", L"sample f" },
                { L"convert (audio: back)", L"3 sample" },
                { L"play armed (> accent)", L".  >" },
                { L"pattern: X x o .", L"3 pat x..." },
                { L"steps of a 16th", L"grid 16" },
                { L"quantize hits (50%)", L"snap 50" },
                { L"toggle a hit at cursor", L"alt+/" } } },
            { L"LIBRARY", {
                { L"sample browser window", L"ctrl+B" },
                { L"add a sample folder", L"index f" },
                { L"take a folder out", L"index remove" },
                { L"folders and samples", L"library" } } } },
          { { L"KITS", {
                { L"new kit (on one: pads)", L"kit" },
                { L"sample track into kit", L"3 kit drums" },
                { L"a sample onto pad 3", L"pad 3 f" },
                { L"fold / open a kit", L"shift+F" },
                { L"pad 3 of kit 2: choke", L"2.3 choke 1" },
                { L"the note that plays it", L"2.3 note 38" } } } },
          L"Armed (shift+R), pads play a kit; ctrl+k keys: zxcv asdf qwer 1234." },

        { L"PLUGINS",
          { { L"INSTRUMENTS", {
                { L"load/swap (alone: list)", L"inst juno" },
                { L"arm for MIDI", L"shift+R" },
                { L"plugin window", L"|" },
                { L"close plugin windows", L"ctrl+\\" },
                { L"keys mode: type to play", L"ctrl+K" },
                { L"find plugins / MIDI ins", L"scan  midi" },
                { L"latent plugins sit out", L"lowlat" },
                { L"...over 256 samples", L"lowlat 256" } } } },
          { { L"INSERTS", {
                { L"effect picker / by name", L"ins  ins cla" },
                { L"its window; b>d swaps", L"2 ins b" },
                { L"remove (or by name)", L"2 ins b x" },
                { L"bypass / back on", L"2 ins b off" },
                { L"on the mix bus", L"mix ins lim" } } },
            { L"ROUTING", {
                { L"routing column / hide", L"\\   io" },
                { L"new bus track", L"bus Verb" },
                { L"send; new name: new bus", L"2 send verb" },
                { L"level, pre/post, off", L"2 send b -6" },
                { L"remove (or by bus)", L"2 send verb x" },
                { L"output to a bus", L"2 out drums" },
                { L"Mix inserts, post-fader", L"\\ on Mix" } } } },
          L"On an insert or send (\\ gets there): del removes it, M bypasses or mutes it." },

        { L"AUTO",
          { { L"LANES", {
                { L"lanes (on Mix: the mix)", L"shift+A" },
                { L"its parameters (adds)", L"auto" },
                { L"a lane by name", L"lane cutoff" },
                { L"change what it shows", L"| on a lane" },
                { L"a point (sel: a level)", L"pt -6" },
                { L"move points up / down", L"alt+\x2191\x2193" },
                { L"remove selected points", L"del" },
                { L"lane off: setting plays", L"shift+O" } } } },
          { { L"KNOBS (MIDI CC)", {
                { L"next knob moved runs it", L"map" },
                { L"...that controller", L"map 74" },
                { L"all knobs off (74: one)", L"unmap" },
                { L"the knob on this lane", L"unmap lane" },
                { L"write: record knobs", L"shift+W  aw" } } } },
          L"Points follow the tempo. A knob on a lane with no points works as a fader." },

        { L"SESSION",
          { { L"SESSION", {
                { L"save / close session", L"ctrl+s / w" },
                { L"quit (asks to save)", L"ctrl+q  exit" },
                { L"tempo (grid: on/off)", L"bpm 92" },
                { L"the session's settings", L"project" },
                { L"let agents edit (MCP)", L"agents on/off" } } } },
          { { L"SETUP", {
                { L"audio device / driver", L"dev  panel" },
                { L"themes (also: grey)", L"dark  light" },
                { L"load: dsp, cpu, tracks", L"stats" },
        { L"recordings from a phone", L"phone" },
                { L"keys window (recording)", L"keycast" } } },
#if TAW_HUB
            { L"HUB", {
                { L"hub: routes, sessions", L"hub" },
                { L"in 1 > Song  Song > out", L"hub route" },
                { L"(start) run the hub", L"hub stop" } } },
#endif
          } },

        { L"FILES",
          { { L"IMPORT", {
                { L"track per file / folder", L"import f" },
                { L"onto track 2 (or drop)", L"2 import f" },
                { L"Live set: a new session", L"import x.als" } } },
            { L"BOUNCE", {
                { L"bounce the selection", L"b  b mp3" },
                { L"open the bounce folder", L"bounces" },
                { L"...don't open after", L"bounces off" },
                { L"bounce with video (mp4)", L"bv" },
                { L"named (no sel: all)", L"bv cue 3" } } } },
          { { L"VIDEO", {
                { L"picture window; or drop", L"video f" },
                { L"show / hide it", L"video" },
                { L"move its start (-0:05)", L"video at 5" },
                { L"picture N frames late", L"video offset" },
                { L"video row: frame/second", L"\x2190\x2192  ctrl" },
                { L"scrub the picture", L"click / drag" } } } },
#if JUCE_WINDOWS
          L"b wav D:\\x bounces to a folder (remembered)." },
#else
          L"b wav ~/x bounces to a folder (remembered)." },
#endif
    };

    // On the session screen (":help"), only what works there: one page of its own.
    static const std::vector<Page> screenPages = {
        { L"SESSIONS",
          { { L"NEW SESSION", {
                { L"name it", L"type" },
                { L"cursor (ctrl: a word)", L"\x2190 \x2192" },
                { L"choose a template", L"\x2191 \x2193" },
                { L"create it", L"enter" } } },
            { L"RECENT SESSIONS", {
                { L"choose one", L"\x2191 \x2193" },
                { L"open it", L"enter" } } },
            { L"BOUNCES", {
                { L"play / stop", L"enter  space" },
                { L"back / ahead 5 s (30 s)", L"(alt) \x2190 \x2192" },
                { L"from the start", L"home" },
                { L"open its session", L"o" },
                { L"show the file", L"r" } } } },
          { { L"THE SCREEN", {
                { L"next / previous area", L"tab shift+tab" },
                { L"sessions folder: pick", L"ctrl+o" },
                { L"sessions folder: type", L"ctrl+l" },
                { L"audio device", L"alt+d  :dev" },
#if TAW_HUB
                { L"the hub", L"alt+h  :hub" },
#endif
                { L"quit", L"esc  ctrl+q" } } },
            { L"COMMANDS  (: opens the line)", {
                { L"new session + phone link", L":phone" },
                { L"add a sample folder", L":index f" },
                { L"folders and samples", L":library" },
                { L"scan plugins (again)", L":scan (retry)" },
                { L"agents (MCP) on / off", L":agents on" },
                { L"show keys pressed", L":keycast" },
                { L"theme: dark light grey", L":light" },
                { L"this help", L":help  :?" } } } },
          L"Everything else needs a session: open or create one, then ? shows the rest." },
    };

    static_assert (App::helpPages == 9, "one page per tab");
    const bool onScreen = app.mode == Mode::TemplatePicker;
    const auto& shown = onScreen ? screenPages : pages;
    const auto& page = shown[(size_t) juce::jlimit (0, (int) shown.size() - 1, onScreen ? 0 : app.helpPage)];

    // A column's lines: each section's title and entries, a blank line between sections.
    auto lines = [] (const std::vector<Section>& column)
    {
        int n = 0;

        for (const auto& s : column)
            n += (n > 0 ? 1 : 0) + 1 + (int) s.entries.size();

        return n;
    };

    // The box is as tall as the tallest page, so turning pages doesn't make it jump.
    int tallest = 0;

    for (const auto& p : shown)
        tallest = std::max (tallest, std::max (lines (p.left), lines (p.right)) + (p.note != nullptr ? 2 : 0));

    constexpr int actionW = 24, colW = 38; // action width, width of each half (the keys get the rest)
    const int w = canvas.width(), h = canvas.height();
    const int boxW = std::min (w - 4, 2 * colW + 4), boxH = std::min (h - 2, tallest + 4);
    const int x0 = (w - boxW) / 2, y0 = 1, bg = colour::headerBg;

    for (int y = 0; y < boxH; ++y)
        canvas.fill (x0, y0 + y, boxW, L' ', -1, bg);

    // Tabs: the page you're on lit.
    int x = x0 + 2;

    for (int i = 0; i < (int) shown.size(); ++i)
    {
        const bool on = i == (onScreen ? 0 : app.helpPage);
        const auto label = juce::String (" ") + shown[(size_t) i].name + " ";

        if (x + label.length() > x0 + boxW - 2)
            break;

        x = canvas.text (x, y0, label, on ? colour::text : colour::dim, on ? colour::selBg : bg, on) + 1;
    }

    const int top = y0 + 2, bottom = y0 + boxH - 2; // the rows the columns and note may use

    auto column = [&] (int px, const std::vector<Section>& sections)
    {
        int y = top;

        for (const auto& s : sections)
        {
            if (y > top)
                ++y;

            if (y < bottom)
                canvas.text (px, y++, s.title, colour::text, bg, true, colW - 2);

            for (const auto& e : s.entries)
                if (y < bottom)
                {
                    canvas.text (px, y, e.action, colour::text, bg, false, actionW - 1);
                    canvas.text (px + actionW, y++, e.keys, colour::meterYellow, bg, false, colW - actionW - 1);
                }
        }
    };

    column (x0 + 2, page.left);
    column (x0 + 2 + colW, page.right);

    if (page.note != nullptr)
        canvas.text (x0 + 2, std::min (bottom - 1, top + std::max (lines (page.left), lines (page.right)) + 1), page.note, colour::dim, bg, false, boxW - 4);

    canvas.text (x0 + 2, y0 + boxH - 1,
                 onScreen ? juce::String (L"? or esc closes")
                          : L"\x2190\x2192 or tab: pages \x00b7 1-" + juce::String (App::helpPages) + L" jumps \x00b7 ? or esc closes",
                 colour::faint, bg, false, boxW - 4);
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

// The send fader: one send's level along a line, for the mouse and the arrows.
void Ui::drawSendFader (App& app)
{
    const auto& s = *app.session;

    if (app.sendFaderTrack < 0 || app.sendFaderTrack >= s.numTracks())
        return;

    const auto t = s.track (app.sendFaderTrack);
    const auto send = Session::send (t, app.sendFaderSlot);

    if (! send.isValid())
        return;

    const int w = canvas.width();
    const int boxW = std::min (w - 4, 72), boxH = 9, x0 = (w - boxW) / 2, y0 = 1, bg = colour::headerBg;

    for (int y = 0; y < boxH; ++y)
        canvas.fill (x0, y0 + y, boxW, L' ', -1, bg);

    const int dest = s.findBus (send[ids::to]);
    const auto title = "SEND " + Session::slotName (app.sendFaderSlot);
    canvas.text (x0 + 2, y0 + 1, title, colour::text, bg, true);
    canvas.text (x0 + 3 + title.length(), y0 + 1,
                 "of track " + juce::String (app.sendFaderTrack + 1) + " " + t[ids::name].toString() + L" \x2192 "
                     + (dest < 0 ? juce::String ("a bus that's gone") : s.track (dest)[ids::name].toString()),
                 colour::dim, bg, false, boxW - title.length() - 5);

    // The fader: off at its left end, then -60 to +12 dB; lit up to the knob.
    const double db = send[ids::db];
    const bool off = send[ids::mute];
    const int fx = x0 + 5, cols = boxW - 10, fy = y0 + 3, knob = App::faderCol (db, cols);
    faderMap = { fx, fy, cols, x0, y0, boxW, boxH, true };

    canvas.text (x0 + 2, fy, L"-\x221e", colour::dim, bg);
    canvas.text (fx + cols + 1, fy, "+12", colour::dim, bg);

    for (int c = 0; c < cols; ++c)
        canvas.put (fx + c, fy, c == knob ? L'\x25cf' : c < knob ? L'\x2501' : L'\x2500',
                    c == knob ? (off ? colour::dim : colour::text) : c < knob ? (off ? colour::faint : colour::accent) : colour::faint, bg);

    // Marks under it: where unity and a few levels are.
    for (const int mark : { -48, -24, -12, -6, 0, 6 })
    {
        const auto label = juce::String (mark);
        canvas.text (fx + App::faderCol (mark, cols) - (mark < 0 ? 1 : 0), fy + 1, label, mark == 0 ? colour::text : colour::faint, bg);
    }

    canvas.text (x0 + 2, fy + 3, (db <= -96.0 ? juce::String (L"-\x221e") : juce::String (juce::roundToInt (db))) + " dB", colour::text, bg, true);
    canvas.text (x0 + 12, fy + 3, juce::String ((bool) send[ids::pre] ? "pre-fader" : "post-fader") + (off ? L" \x00b7 off" : L""), off ? colour::faint : colour::dim, bg);
    canvas.text (x0 + 2, y0 + boxH - 2, L"drag, or \x2190\x2192 (shift: 6 dB) \x00b7 0 unity \x00b7 m off \x00b7 p pre/post \x00b7 b bus \x00b7 del removes \x00b7 esc",
                 colour::dim, bg, false, boxW - 4);
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
    const auto title = juce::String (app.trackMenuDuplicating ? "DUPLICATE " : "")
                       + (tracks.size() == 1 ? juce::String ("TRACK ") + juce::String (tracks.front() + 1) : juce::String ("TRACKS"));
    const auto what = tracks.size() == 1 ? app.session->track (tracks.front())[ids::name].toString() : app.trackList (tracks).fromFirstOccurrenceOf (" ", false, false);
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

// The hub's page, a screen of its own (with a session open or not): its interface, then the
// matrix of where sound goes. Rows are where it comes from (the interface's inputs, the app
// devices, sessions' outputs); columns where it goes (the interface's outputs, the app devices'
// mics, sessions' inputs). A cell is ● on, · off, the inputs it lands on in a session's column,
// ~ when typed routes don't fit the usual shape. It scrolls both ways, so it fits 80x24.
void Ui::drawHubPage (App& app)
{
    if (app.hubPicker.open)
    {
        drawHubInterfacePicker (app);
        return;
    }

    const auto& s = app.hubStatus;
    const int w = canvas.width(), h = canvas.height();

    canvas.fill (0, 0, w, L' ', -1, colour::headerBg);
    canvas.text (1, 0, "taw", colour::accent, colour::headerBg, true);
    int x = canvas.text (5, 0, L"\x25b8 hub", colour::text, colour::headerBg);

    if (app.session != nullptr && ! s.isVoid() && ! app.sessionOnHub())
        canvas.text (x + 2, 0, L"(" + app.session->name() + L" isn't on the hub \x00b7 its dev \x2192 taw hub puts it here)", colour::error, colour::headerBg);
    else if (app.session != nullptr)
        canvas.text (x + 2, 0, L"(" + app.session->name() + L" plays on \x00b7 esc goes back to it)", colour::dim, colour::headerBg);

    int y = 2;

    if (s.isVoid())
    {
        canvas.text (1, y++, app.hubError, colour::error, -1, false, w - 2);
        canvas.text (1, ++y, L"start runs the hub (it then starts at login) \x00b7 it owns the interface and routes between it, sessions and apps",
                     colour::dim, -1, false, w - 2);
    }
    else
    {
        canvas.text (1, y, "interface", colour::dim);
        // The rate and block first: the device's name can be long (two devices).
        x = canvas.text (12, y, juce::String ((double) s["sampleRate"] / 1000.0, 1) + "k/" + juce::String ((int) s["blockSize"]), colour::dim);
        canvas.text (x + 2, y++, s["device"].toString() + L" \x00b7 i changes it", colour::text, -1, false, w - x - 3);

        if ((int) s["apps"] == 0)
            canvas.text (12, y, L"no app devices \x2014 driver install adds taw 1 to taw 4 for Zoom, a browser...", colour::faint, -1, false, w - 13);
        else
            canvas.text (1, y + 1, L"rows come in, columns go out \x00b7 an app on taw 1 plays to taw1in, hears taw1out",
                         colour::dim, -1, false, w - 2);

        y += 3;

        const auto rows = app.hubRows();
        const auto cols = app.hubCols();
        constexpr int colW = 8;
        // The names take what the columns leave, so who plays into an app device shows.
        const int labelW = juce::jlimit (20, 34, w - 2 - (int) cols.size() * colW);
        const int top = y, firstRowY = top + 2;
        const int visibleCols = std::max (1, (w - 2 - labelW) / colW);
        const int visibleRows = std::max (1, h - 5 - firstRowY);
        const int firstCol = juce::jlimit (0, std::max (0, (int) cols.size() - visibleCols), app.hubCol - visibleCols / 2);
        const int firstRow = juce::jlimit (0, std::max (0, (int) rows.size() - visibleRows), app.hubRow - visibleRows / 2);

        // "to →" beside the columns' names, "from ↓" over the rows'.
        canvas.text (labelW - 5, top, L"to \x2192", colour::faint);
        canvas.text (1, top + 1, L"from \x2193", colour::faint);

        // The columns' heads: a name, and under it what part of it ("out 1-2", "as mic", "inputs").
        for (int i = 0; i < visibleCols && firstCol + i < (int) cols.size(); ++i)
        {
            const auto& c = cols[(size_t) (firstCol + i)];
            const bool chosen = firstCol + i == app.hubCol;
            const int cx = 1 + labelW + i * colW;
            canvas.text (cx, top, c.name, chosen ? colour::accent : (c.live ? colour::text : colour::faint), -1, chosen, colW - 1);
            canvas.text (cx, top + 1, c.sub, chosen ? colour::accent : colour::faint, -1, false, colW - 1);
        }

        if (firstCol > 0)
            canvas.put (labelW, top, L'\x25c2', colour::dim);

        if (firstCol + visibleCols < (int) cols.size())
            canvas.put (w - 2, top, L'\x25b8', colour::dim);

        for (int r = 0; r < visibleRows && firstRow + r < (int) rows.size(); ++r)
        {
            const auto& row = rows[(size_t) (firstRow + r)];
            const int ry = firstRowY + r;
            const bool rowChosen = firstRow + r == app.hubRow;

            if (rowChosen)
                canvas.put (1, ry, L'\x25b8', colour::accent);

            x = canvas.text (3, ry, row.name, rowChosen ? colour::accent : (row.live ? colour::text : colour::faint), -1, rowChosen, labelW - 3);

            if (row.sub.isNotEmpty() && x + 1 < labelW)
                canvas.text (x + 1, ry, row.sub, colour::faint, -1, false, labelW - x - 1);
            else if (! row.live && x + 1 < labelW)
                canvas.text (x + 1, ry, "(closed)", colour::faint, -1, false, labelW - x - 1);

            for (int i = 0; i < visibleCols && firstCol + i < (int) cols.size(); ++i)
            {
                const auto& col = cols[(size_t) (firstCol + i)];
                const auto cell = app.hubCell (row, col);
                const bool chosen = rowChosen && firstCol + i == app.hubCol;
                const int bg = chosen ? colour::focusBg : -1;
                const bool live = row.live && col.live;
                const int cx = 1 + labelW + i * colW;

                juce::String mark;
                int fg = colour::faint;

                if (cell.self)
                    mark = L"\x2500";
                else if (! cell.on)
                    mark = L"\x00b7";
                else
                {
                    mark = cell.odd ? juce::String ("~")
                           : col.session ? juce::String (cell.land) + (cell.landLast > cell.land ? "-" + juce::String (cell.landLast) : juce::String())
                                         : juce::String (L"\x25cf");
                    fg = live ? colour::accent : colour::dim;
                }

                if (chosen && fg == colour::faint)
                    fg = colour::dim; // faint doesn't read on the highlight

                canvas.fill (cx, ry, colW - 1, L' ', -1, bg);
                canvas.text (cx + std::max (0, (colW - 1 - (int) mark.length()) / 2), ry, mark, fg, bg, cell.on);
            }
        }

        // The chosen cell, in words.
        if (! rows.empty() && ! cols.empty())
        {
            const auto& row = rows[(size_t) juce::jlimit (0, (int) rows.size() - 1, app.hubRow)];
            const auto& col = cols[(size_t) juce::jlimit (0, (int) cols.size() - 1, app.hubCol)];
            const auto cell = app.hubCell (row, col);
            juce::String words;

            if (cell.self)
            {
                words = row.name + L" can't feed itself";
            }
            else if (! cell.on)
            {
                // An app device in full: who plays into it, who hears it.
                auto users = [&app] (const App::HubEnd& e, bool source) -> juce::String
                {
                    const auto it = app.hubUsers.find (e.key);
                    const auto list = it == app.hubUsers.end() ? juce::String()
                                      : (source ? it->second.playing : it->second.listening).joinIntoString (", ");

                    if (source)
                        return L" (" + (list.isEmpty() ? juce::String ("no app plays into it yet") : list + " into it") + ")";

                    return L"'s mic (" + (list.isEmpty() ? juce::String ("no app hears it yet") : list + " hear it") + ")";
                };

                const bool appRow = ! row.session && row.key != "interface";
                const bool appCol = ! col.session && col.key != "interface";
                words = row.name + (appRow ? users (row, true) : juce::String()) + L" \x2192 "
                        + col.name + (appCol ? users (col, false) : col.sub.isNotEmpty() ? " " + col.sub : juce::String()) + L": off \x00b7 space routes it";
            }
            else
            {
                juce::StringArray parts;

                for (const auto& r : cell.routes)
                    parts.add (app.hubEndName (r["from"].toString(), r["fromChannel"], true) + L" \x2192 "
                               + app.hubEndName (r["to"].toString(), r["toChannel"], false));

                words = parts.joinIntoString (", ") + L" \x00b7 " + formatDb (cell.db) + " dB"
                        + (col.session ? juce::String (L" \x00b7 shift+\x2190\x2192 other inputs") : juce::String());
            }

            canvas.text (1, h - 4, words, colour::dim, -1, false, w - 2);
        }
    }

    // The message, the command line, the keys.
    if (app.message.isNotEmpty())
        canvas.text (1, h - 3, app.message, app.messageIsError ? colour::error : colour::ok, -1, false, w - 2);

    x = canvas.text (1, h - 2, L"hub \x25b8 ", colour::accent);
    x = canvas.text (x, h - 2, app.hubLine, colour::text, -1, true);
    canvas.put (x, h - 2, L' ', -1, colour::text);

    if (app.hubLine.isEmpty())
        canvas.text (x + 2, h - 2, L"or type: route in 1 > Song 3 \x00b7 name taw 1 Zoom \x00b7 forget Song \x00b7 start \x00b7 stop",
                     colour::faint, -1, false, w - x - 3);

    canvas.text (1, h - 1, L"\x2190\x2191\x2193\x2192 move \x00b7 space on/off \x00b7 [ ] dB \x00b7 shift+\x2190\x2192 inputs \x00b7 i interface \x00b7 n name \x00b7 esc back",
                 colour::dim, -1, false, w - 2);
}

// The hub's interface: what it plays through on the left, what it hears on the right; the
// chosen side's heading and pick in the accent. "current" marks what the hub has.
void Ui::drawHubInterfacePicker (App& app)
{
    const auto& p = app.hubPicker;
    const int w = canvas.width(), h = canvas.height();

    canvas.fill (0, 0, w, L' ', -1, colour::headerBg);
    canvas.text (1, 0, "taw", colour::accent, colour::headerBg, true);
    canvas.text (5, 0, L"\x25b8 hub \x25b8 interface", colour::text, colour::headerBg);
    canvas.text (1, 2, L"The hardware the hub plays through and hears. Enter restarts the hub on it.", colour::dim, -1, false, w - 2);

    const int colW = (w - 4) / 2, top = 4, visible = h - top - 4;

    auto list = [&] (int x, const juce::String& heading, const juce::StringArray& names, int chosen, bool active, const juce::String& now)
    {
        canvas.text (x, top, heading, active ? colour::accent : colour::dim, -1, active);

        const int first = juce::jlimit (0, juce::jmax (0, names.size() - visible), chosen - visible / 2);

        if (names.isEmpty())
            canvas.text (x + 2, top + 2, "none found", colour::faint);

        for (int r = 0; r < visible && first + r < names.size(); ++r)
        {
            const int i = first + r;
            const bool sel = i == chosen;
            const int y = top + 2 + r;
            const int bg = sel && active ? colour::focusBg : -1;

            canvas.fill (x, y, colW - 1, L' ', -1, bg);

            if (sel)
                canvas.put (x, y, L'\x25b8', active ? colour::accent : colour::dim, bg);

            const int nx = canvas.text (x + 2, y, names[i], sel ? (active ? colour::accent : colour::text) : colour::text, bg, sel, colW - 10);

            if (names[i] == now)
                canvas.text (nx + 1, y, L"\x25cf current", colour::faint, bg);
        }
    };

    list (2, "output", p.outputs, p.output, ! p.onInputs, app.hubHardware (true));
    list (2 + colW + 1, "input", p.inputs, p.input, p.onInputs, app.hubHardware (false));

    // What enter would do, in words.
    if (! p.outputs.isEmpty())
        canvas.text (1, h - 3, L"enter: " + p.outputs[p.output] + (p.inputs.isEmpty() ? juce::String() : L" + " + p.inputs[p.input]),
                     colour::text, -1, false, w - 2);

    canvas.text (1, h - 1, L"\x2190\x2192 output / input \x00b7 \x2191\x2193 choose \x00b7 enter use both \x00b7 r rescan \x00b7 esc back", colour::dim, -1, false, w - 2);
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
        int x = canvas.text (26, top + row, c.output, colour::text, bg, sel);

        // Its input: the same device unless it says otherwise; ←→ on the chosen row change it.
        const auto input = c.input.isEmpty() ? juce::String ("no input") : c.input == c.output ? juce::String ("its own input") : "in: " + c.input;

        if (sel && ! c.inputs.isEmpty())
            canvas.text (x + 2, top + row, L"\x2190 " + input + L" \x2192", colour::dim, bg);
        else if (c.input != c.output)
            canvas.text (x + 1, top + row, "(" + input + ")", colour::faint, bg);
    }

    drawBottom (app);
    canvas.fill (0, h - 2, w, L' ');
    canvas.fill (0, h - 1, w, L' ');
    canvas.text (1, h - 1, L"\x2191\x2193 select \x00b7 \x2190\x2192 input \x00b7 enter choose \x00b7 r rescan \x00b7 esc back", colour::dim);
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
    const auto title = juce::String (job.save ? "SAVING AND " : "") + (job.thenQuit ? "QUITTING" : "CLOSING");
    const auto doing = job.current.isNotEmpty() ? (job.save ? "saving " : "closing ") + job.current
                                                : juce::String (job.save ? "saving the session" : "closing the session");
    drawProgress (app, title, doing, job.done, job.total);
}

// Closing with unsaved changes: save them (and bounce last-saved), or drop them.
void Ui::drawClosePrompt (App& app)
{
    const int w = canvas.width();
    const int boxW = std::min (w - 4, 64), x0 = (w - boxW) / 2, y0 = 3, bg = colour::headerBg;

    for (int y = 0; y < 8; ++y)
        canvas.fill (x0, y0 + y, boxW, L' ', -1, bg);

    const juce::String title (app.closePrompt->thenQuit ? "QUIT" : "CLOSE");
    canvas.text (x0 + 2, y0 + 1, title, colour::text, bg, true);
    canvas.text (x0 + 3 + title.length(), y0 + 1, app.session->name(), colour::accent, bg, true, boxW - title.length() - 5);
    canvas.text (x0 + 2, y0 + 2, "has changes you haven't saved", colour::dim, bg, false, boxW - 4);

    int x = canvas.text (x0 + 2, y0 + 4, "s", colour::accent, bg, true);
    x = canvas.text (x + 1, y0 + 4, "save, and bounce last-saved", colour::text, bg, false, boxW - 4);
    x = canvas.text (x0 + 2, y0 + 5, "o", colour::accent, bg, true);
    canvas.text (x + 1, y0 + 5, "save only, no bounce", colour::text, bg);
    x = canvas.text (x0 + 2, y0 + 6, "d", colour::accent, bg, true);
    canvas.text (x + 1, y0 + 6, "don't save", colour::text, bg);
    x = canvas.text (x0 + 26, y0 + 6, "esc", colour::accent, bg, true);
    canvas.text (x + 1, y0 + 6, "stay", colour::text, bg);
}

// An agent wants another session open (new_session / open_session): this one closes first, so
// it's the close prompt's choices, or plain yes / stay when nothing's unsaved.
void Ui::drawSessionPrompt (App& app)
{
    const auto& ask = *app.sessionAsk;
    const bool changes = app.hasUnsavedChanges();
    const int w = canvas.width();
    const int boxW = std::min (w - 4, 68), x0 = (w - boxW) / 2, y0 = 3, bg = colour::headerBg;
    const int rows = changes ? 9 : 7;

    for (int y = 0; y < rows; ++y)
        canvas.fill (x0, y0 + y, boxW, L' ', -1, bg);

    int x = canvas.text (x0 + 2, y0 + 1, ask.create ? "NEW SESSION" : "OPEN", colour::text, bg, true);
    x = canvas.text (x + 1, y0 + 1, ask.agent, colour::accent, bg, true, boxW / 3);
    canvas.text (x + 1, y0 + 1, ask.create ? "wants to make one and open it" : "wants to open another session", colour::text, bg, false, x0 + boxW - x - 3);
    x = canvas.text (x0 + 2, y0 + 2, ask.folder.getFileName(), colour::accent, bg, true, boxW - 4);

    if (ask.create && ask.templateName.isNotEmpty())
        canvas.text (x + 1, y0 + 2, "from " + ask.templateName, colour::dim, bg, false, x0 + boxW - x - 3);

    canvas.text (x0 + 2, y0 + 3, app.session->name() + (changes ? " closes first, and has changes you haven't saved" : " closes first (nothing unsaved)"),
                 colour::dim, bg, false, boxW - 4);

    auto option = [&] (int y, const juce::String& key, const juce::String& what)
    {
        canvas.text (x0 + 2, y, key, colour::accent, bg, true);
        canvas.text (x0 + 9, y, what, colour::text, bg, false, boxW - 11);
    };

    if (changes)
    {
        option (y0 + 5, "s", "save (and bounce last-saved), then open it");
        option (y0 + 6, "o", "save only, then open it");
        option (y0 + 7, "d", "don't save, open it");
        option (y0 + 8, "esc", "stay here");
    }
    else
    {
        option (y0 + 5, "enter", "open it");
        option (y0 + 6, "esc", "stay here");
    }
}

// An agent wants another tempo and the session has audio clips, which are placed in samples:
// MIDI and hits follow the beats, the audio does whatever the user says.
void Ui::drawTempoPrompt (App& app)
{
    const auto& ask = *app.tempoAsk;
    const int w = canvas.width();
    const int boxW = std::min (w - 4, 76), x0 = (w - boxW) / 2, y0 = 3, bg = colour::headerBg;
    auto bpm = [] (double b) { return juce::String (b, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."); };

    for (int y = 0; y < 10; ++y)
        canvas.fill (x0, y0 + y, boxW, L' ', -1, bg);

    int x = canvas.text (x0 + 2, y0 + 1, "TEMPO", colour::text, bg, true);
    x = canvas.text (x + 1, y0 + 1, ask.agent, colour::accent, bg, true, boxW / 2);
    canvas.text (x + 1, y0 + 1, "wants " + bpm (ask.from) + L" \x2192 " + bpm (ask.to) + " bpm", colour::text, bg, false, x0 + boxW - x - 3);
    canvas.text (x0 + 2, y0 + 2, "MIDI and hits follow the beats. Your audio clips:", colour::dim, bg, false, boxW - 4);

    x = canvas.text (x0 + 2, y0 + 4, "t", colour::accent, bg, true);
    canvas.text (x + 1, y0 + 4, "keep them where they are in time (they stay at their seconds)", colour::text, bg, false, boxW - 6);
    x = canvas.text (x0 + 2, y0 + 5, "b", colour::accent, bg, true);
    canvas.text (x + 1, y0 + 5, "move them with the beats (each starts on its beat; not stretched)", colour::text, bg, false, boxW - 6);
    x = canvas.text (x0 + 2, y0 + 6, "s", colour::faint, bg, true);
    canvas.text (x + 1, y0 + 6, "stretch them to the new tempo (coming, not built yet)", colour::faint, bg, false, boxW - 6);
    x = canvas.text (x0 + 2, y0 + 8, "esc", colour::accent, bg, true);
    canvas.text (x + 1, y0 + 8, "cancel: the tempo stays " + bpm (ask.from), colour::text, bg, false, boxW - 8);
}

// The session screen, top to bottom: a new session (its name, where it goes, the templates),
// then two columns: recent sessions on the left, the bounces taw made on the right (enter
// plays one, without opening anything). Each is framed, the focused one lit. Up and down move
// within the focused area; tab and shift+tab, and left and right, between areas.
static juce::String agoText (juce::Time t)
{
    const auto age = juce::Time::getCurrentTime() - t;
    return age.inMinutes() < 60 ? juce::String ((int) age.inMinutes()) + " min ago"
         : age.inHours() < 48   ? juce::String ((int) age.inHours()) + " h ago"
                                : juce::String ((int) age.inDays()) + " days ago";
}

static juce::String clockText (double s)
{
    const auto t = juce::jmax (0, (int) s);
    return juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2);
}

// An area's frame, its name set into the top edge: heavy and lit when it has the keys, light
// and faint when it doesn't.
static void areaBox (Canvas& canvas, int x0, int y0, int x1, int y1, const juce::String& title, bool focused)
{
    const int c = focused ? colour::accent : colour::faint;
    const wchar_t across = focused ? L'\x2501' : L'\x2500', down = focused ? L'\x2503' : L'\x2502';

    for (int x = x0 + 1; x < x1; ++x)
    {
        canvas.put (x, y0, across, c);
        canvas.put (x, y1, across, c);
    }

    for (int y = y0 + 1; y < y1; ++y)
    {
        canvas.put (x0, y, down, c);
        canvas.put (x1, y, down, c);
    }

    canvas.put (x0, y0, focused ? L'\x250f' : L'\x250c', c);
    canvas.put (x1, y0, focused ? L'\x2513' : L'\x2510', c);
    canvas.put (x0, y1, focused ? L'\x2517' : L'\x2514', c);
    canvas.put (x1, y1, focused ? L'\x251b' : L'\x2518', c);
    canvas.text (x0 + 2, y0, " " + title + " ", focused ? colour::accent : colour::dim, -1, focused, x1 - x0 - 3);
}

// A line across a frame, joined to its sides.
static void areaRule (Canvas& canvas, int x0, int x1, int y, bool focused)
{
    const int c = focused ? colour::accent : colour::faint;

    for (int x = x0 + 1; x < x1; ++x)
        canvas.put (x, y, focused ? L'\x2501' : L'\x2500', c);

    canvas.put (x0, y, focused ? L'\x2523' : L'\x251c', c);
    canvas.put (x1, y, focused ? L'\x252b' : L'\x2524', c);
}

// The first row to show so the selected one stays in view.
static int firstShown (int selected, int count, int rows)
{
    return juce::jlimit (0, juce::jmax (0, count - rows), selected - rows / 2);
}

void Ui::drawTemplatePicker (App& app)
{
    using Area = App::PickArea;
    const int w = canvas.width(), h = canvas.height();
    const int numRecent = (int) app.recent.size(), numBounces = (int) app.recentBounces.size();
    const int numTemplates = (int) app.templates.size();

    canvas.fill (0, 0, w, L' ', -1, colour::headerBg);
    canvas.text (1, 0, "taw", colour::accent, colour::headerBg, true);
    const auto afterTitle = canvas.text (5, 0, L"\x25b8 sessions", colour::text, colour::headerBg);

    // The audio device the bounces play through (a saved one that's gone leaves none).
    {
        auto* device = app.engine.devices().getCurrentAudioDevice();
        const auto x = canvas.text (afterTitle + 3, 0, device != nullptr ? L"\x266a " + device->getName() : juce::String (L"\x266a no audio device"),
                                    device != nullptr ? colour::text : colour::error, colour::headerBg, device == nullptr, w / 2);

        if (device != nullptr)
            canvas.text (x + 1, 0, juce::String (device->getCurrentSampleRate() / 1000.0, 1) + "k", colour::dim, colour::headerBg);

        canvas.text (x + (device != nullptr ? 8 : 1), 0, L"alt+d change", colour::faint, colour::headerBg);
    }

    // Which build, out of the way on the right: the version, the commit (+: changes on top of
    // it) and when it was built.
    const auto build = juce::String (TAW_VERSION) + " " + buildCommit() + " " + buildTime();
    canvas.text (std::max (16, w - 1 - build.length()), 0, build, colour::dim, colour::headerBg);

    // New session, in its frame: its name, where it goes, then the template to start from.
    const bool newFocused = app.pickArea == Area::templates;
    const int newTop = 2;
    const int templateRows = juce::jlimit (1, juce::jmax (1, numTemplates), (h - 14) / 2);
    areaBox (canvas, 0, newTop, w - 1, newTop + 3 + templateRows, "new session", newFocused);
    int y = newTop + 1;
    canvas.text (2, y, "name", colour::dim);

    {
        // A text field: the cursor shows where typing goes while the area has the keys.
        const auto& name = app.newSessionName;
        const auto x = canvas.text (7, y, name, colour::text, -1, true);

        if (newFocused)
        {
            const auto at = juce::jlimit (0, name.length(), app.newSessionCursor);
            canvas.put (7 + at, y, at < name.length() ? (wchar_t) name[at] : L' ', colour::headerBg, colour::text, true);
        }

        if (name.isEmpty())
            canvas.text (x + 2, y, "(type a name, or enter for a dated one)", colour::faint, -1, false, w - x - 3);
        else if (app.dateSessionNames && App::datedName (name) != name)
            canvas.text (x + 2, y, App::datedName ("").trim(), colour::faint, -1, false, w - x - 3); // the date it will get
    }

    // Where they go: ctrl+l types a new place over it, ctrl+o picks one.
    canvas.text (2, ++y, "in", colour::dim);

    if (app.editingFolder)
    {
        const auto x = canvas.text (7, y, app.folderEdit, app.folderEditFresh ? colour::dim : colour::text, app.folderEditFresh ? colour::selBg : -1,
                                    true, w - 40);
        canvas.put (x, y, L' ', -1, colour::text);
        canvas.text (x + 2, y, L"enter sets it \x00b7 esc leaves it", colour::faint, -1, false, w - x - 3);
    }
    else
    {
        const auto x = canvas.text (7, y, app.newSessionParent.getFullPathName(), colour::faint, -1, false, w - 40);
        canvas.text (x + 2, y, L"ctrl+o choose \x00b7 ctrl+l type", colour::faint, -1, false, w - x - 3);
    }

    ++y;

    // Templates: as many rows as fit, keeping room below for the lists.
    {
        const int rows = templateRows;
        const int first = firstShown (app.templateIndex, numTemplates, rows);
        const bool focused = newFocused;

        for (int i = first; i < juce::jmin (numTemplates, first + rows); ++i, ++y)
        {
            const auto& t = app.templates[(size_t) i];
            const bool sel = i == app.templateIndex;
            const int bg = sel && focused ? colour::focusBg : -1;

            canvas.fill (1, y, w - 2, L' ', -1, bg);

            if (sel)
                canvas.put (3, y, L'\x25b8', focused ? colour::accent : colour::faint, bg);

            canvas.text (5, y, padRight (t.name, 14), sel ? colour::text : colour::dim, bg, sel && focused);

            juce::StringArray tracks;
            for (const auto& c : t.tree)
                if (c.hasType (ids::TRACK))
                    tracks.add (c[ids::name].toString() + ((int) c[ids::input] > 0 ? L" \x2190 in" + c[ids::input].toString() : juce::String()));

            canvas.text (20, y, tracks.isEmpty() ? juce::String ("empty") : tracks.joinIntoString ("  "), sel && focused ? colour::dim : colour::faint, bg, false, w - 22);
        }
    }

    // Two frames side by side: recent sessions | bounces, down to the message rows.
    y = newTop + 4 + templateRows;
    const int mid = w / 2, listTop = y, listBottom = juce::jmax (listTop + 2, h - 4);
    const int leftX = 1, colW = mid - 2;           // inside the left frame
    const int rightX = mid + 1, rightW = w - 2 - mid; // inside the right one
    const int listRows = listBottom - listTop - 1;
    const auto* player = app.bouncePlayer.get();
    const auto at = app.bouncePlaySeconds();
    const bool bouncesFocused = app.pickArea == Area::bounces;

    // The player sits at the foot of the bounces: there whenever they have the keys (so it's
    // plain enter plays one), and while one plays.
    const bool showPlayer = (bouncesFocused || player != nullptr) && listRows >= 3;
    const int bounceRows = showPlayer ? listRows - 2 : listRows;

    areaBox (canvas, 0, listTop, mid - 1, listBottom, "recent sessions", app.pickArea == Area::sessions);
    areaBox (canvas, mid, listTop, w - 1, listBottom, "recent bounces", bouncesFocused);

    ++y;

    if (numRecent == 0)
        canvas.text (leftX + 2, y, "none yet", colour::faint);

    {
        const bool focused = app.pickArea == Area::sessions;
        const int first = firstShown (app.sessionIndex, numRecent, listRows);

        for (int i = first, row = y; i < juce::jmin (numRecent, first + listRows); ++i, ++row)
        {
            const auto& dir = app.recent[(size_t) i];
            const bool sel = i == app.sessionIndex;
            const int bg = sel && focused ? colour::focusBg : -1;
            const int ageW = 11, nameW = juce::jlimit (8, 22, colW - ageW - 4 - 18), metaW = juce::jmax (0, colW - nameW - ageW - 4);

            canvas.fill (leftX, row, colW - 1, L' ', -1, bg);

            if (sel)
                canvas.put (leftX, row, L'\x25b8', focused ? colour::accent : colour::faint, bg);

            canvas.text (leftX + 2, row, padRight (dir.getFileName(), nameW), colour::text, bg, sel && focused, nameW);

            // Its size at a glance: tracks, tempo, plugins, length.
            if (i < (int) app.recentSummaries.size() && app.recentSummaries[(size_t) i].ok && metaW > 0)
            {
                const auto& m = app.recentSummaries[(size_t) i];
                const auto secs = juce::roundToInt (m.seconds);
                const juce::String pieces[] = { juce::String (m.tracks) + " tr", juce::String (juce::roundToInt (m.bpm)) + " bpm",
                                                juce::String (m.plugins) + " fx",
                                                secs > 0 ? juce::String (secs / 60) + ":" + juce::String (secs % 60).paddedLeft ('0', 2) : juce::String() };
                juce::String meta;

                // As many as fit, in that order.
                for (const auto& piece : pieces)
                {
                    if (piece.isEmpty())
                        continue;

                    const auto next = meta.isEmpty() ? piece : meta + "  " + piece;

                    if (next.length() > metaW - 1)
                        break;

                    meta = next;
                }

                canvas.text (leftX + 3 + nameW, row, meta, sel && focused ? colour::dim : colour::faint, bg, false, metaW);
            }

            canvas.text (leftX + 3 + nameW + metaW, row, agoText (dir.getChildFile ("session.json").getLastModificationTime()), colour::dim, bg, false,
                         ageW);
        }
    }

    if (numBounces == 0)
        canvas.text (rightX + 2, y, L"none yet \x00b7 b bounces, saving as you close bounces last-saved", colour::faint, -1, false, rightW - 2);

    {
        const bool focused = bouncesFocused;
        const int first = firstShown (app.bounceIndex, numBounces, bounceRows);
        const int ageW = 11, nameW = juce::jmax (8, (rightW - ageW - 5) * 3 / 5), sessionW = juce::jmax (0, rightW - ageW - nameW - 5);

        for (int i = first, row = y; i < juce::jmin (numBounces, first + bounceRows); ++i, ++row)
        {
            const auto& b = app.recentBounces[(size_t) i];
            const bool sel = i == app.bounceIndex;
            const bool playing = player != nullptr && player->file == b.file && at >= 0.0;
            const int bg = sel && focused ? colour::focusBg : -1;

            canvas.fill (rightX, row, w - 1 - rightX, L' ', -1, bg);

            if (sel)
                canvas.put (rightX, row, L'\x25b8', focused ? colour::accent : colour::faint, bg);

            if (playing)
                canvas.put (rightX + 1, row, L'\x266a', colour::accent, bg);

            canvas.text (rightX + 2, row, padRight (b.file.getFileNameWithoutExtension(), nameW), playing ? colour::accent : colour::text, bg,
                         sel && focused, nameW);
            canvas.text (rightX + 3 + nameW, row, padRight (b.session, sessionW), colour::dim, bg, false, sessionW);
            canvas.text (rightX + 4 + nameW + sessionW, row, agoText (b.time), sel && focused ? colour::dim : colour::faint, bg, false, ageW);
        }
    }

    // The player: the bounce playing (where, and how far through), else the one picked, ready.
    if (showPlayer)
    {
        const int py = listBottom - 1, right = w - 2;
        areaRule (canvas, mid, w - 1, py - 1, bouncesFocused);

        const bool playing = player != nullptr && at >= 0.0;
        const auto* picked = app.bounceIndex < numBounces ? &app.recentBounces[(size_t) app.bounceIndex] : nullptr;
        const auto name = player != nullptr ? player->file.getFileNameWithoutExtension()
                        : picked != nullptr ? picked->file.getFileNameWithoutExtension()
                                            : juce::String ("nothing to play yet");
        int x = canvas.text (rightX + 1, py, playing ? L"\x25b6 " : L"\x25a0 ", playing ? colour::accent : colour::dim);
        x = canvas.text (x, py, name, player != nullptr ? colour::text : colour::dim, -1, player != nullptr, juce::jmax (8, rightW / 2));

        if (player != nullptr && ! player->started)
            canvas.text (x + 2, py, L"loading\x2026", colour::dim, -1, false, right - x - 1);
        else
        {
            const auto len = app.bounceLengthSeconds();
            const auto clock = player != nullptr ? clockText (juce::jmax (0.0, at)) + " / " + clockText (len) : juce::String (L"enter plays");
            x = canvas.text (x + 2, py, clock, colour::dim, -1, false, right - x - 1);
            const int barW = juce::jmax (0, right - (x + 2) + 1);
            const int filled = len > 0.0 && at >= 0.0 ? (int) (barW * at / len) : 0;

            for (int i = 0; i < barW; ++i)
                canvas.put (x + 2 + i, py, i < filled ? L'\x2501' : L'\x2500', i < filled ? colour::accent : colour::faint);
        }
    }

    // Bounces still going in the background (a session saved and closed): a lit label and a
    // bar, so it's plain it's working (and, quitting, that taw waits for it). Else the last word.
    if (! app.backgroundBounces.empty())
    {
        const auto& b = *app.backgroundBounces.front();
        const auto progress = juce::jlimit (0.0f, 1.0f, b.progress());
        int x = canvas.text (1, h - 3, L" \x25cf BOUNCING ", 231, colour::marker, true); // as a bounce in a session
        x = canvas.text (x + 1, h - 3, app.backgroundBounceNames.front() + L" \x00b7 " + b.file().getFileName(), colour::text, -1, true, w / 2);

        if (app.backgroundBounces.size() > 1)
            x = canvas.text (x, h - 3, " (+" + juce::String ((int) app.backgroundBounces.size() - 1) + " more)", colour::dim);

        const auto pct = " " + juce::String (juce::roundToInt (progress * 100.0f)) + "%";
        const int barW = juce::jmax (0, w - 2 - (x + 1) - pct.length());
        const int filled = (int) ((float) barW * progress);

        for (int i = 0; i < barW; ++i)
            canvas.put (x + 1 + i, h - 3, i < filled ? L'\x2588' : L'\x2591', i < filled ? colour::marker : colour::faint);

        canvas.text (x + 1 + barW, h - 3, pct, colour::marker, -1, true);

        if (app.quitAfterBounces)
            canvas.text (1, h - 2, L"taw quits when it's done \x00b7 esc or ctrl+q quits now (the bounce is dropped) \x00b7 any other key stays", colour::text, -1, false, w - 2);
    }
    else if (app.message.isNotEmpty())
    {
        canvas.text (1, h - 3, app.message, app.messageIsError ? colour::error : colour::ok, -1, false, w - 2);
    }

    // ":" the command line, as in a session; only the commands that need no session work here.
    if (app.screenCommandOpen)
    {
        auto x = canvas.text (1, h - 2, ": ", colour::accent, -1, true);
        x = canvas.text (x, h - 2, app.screenCommand, colour::text, -1, false, w - 4);
        canvas.put (x, h - 2, L' ', -1, colour::text);

        if (app.screenCommand.isEmpty())
            canvas.text (x + 2, h - 2, L"help \x00b7 phone \x00b7 index <folder> \x00b7 library \x00b7 scan \x00b7 agents on|off \x00b7 hub \x00b7 keycast \x00b7 dark|light|grey \x00b7 dev \x00b7 q",
                         colour::faint, -1, false, w - x - 3);

        canvas.text (1, h - 1, L"enter runs it \x00b7 esc closes \x00b7 a folder can be dropped after index", colour::dim, -1, false, w - 2);
        return;
    }

#if TAW_HUB
    const juce::String areas (L"tab \x2190\x2192 area \x00b7 alt+d device \x00b7 alt+h hub \x00b7 : commands");
#else
    const juce::String areas (L"tab \x2190\x2192 area \x00b7 alt+d device \x00b7 : commands");
#endif
    const bool bouncePlaying = app.bouncePlaySeconds() >= 0.0;
    const auto hint = app.pickArea == Area::bounces
                          ? (bouncePlaying ? L"enter play/stop \x00b7 \x2190\x2192 seek (alt: 30 s) \x00b7 o session \x00b7 r reveal \x00b7 tab area"
                                           : L"enter play/stop \x00b7 o session \x00b7 r reveal \x00b7 " + areas)
                      : app.pickArea == Area::sessions
                          ? L"\x2191\x2193 choose \x00b7 enter open \x00b7 type: new session \x00b7 " + areas + L" \x00b7 esc quit"
                          : L"type a name (ctrl \x2190\x2192 by word) \x00b7 \x2191\x2193 template \x00b7 enter create \x00b7 " + areas + L" \x00b7 esc quit";
    canvas.text (1, h - 1, hint, colour::dim, -1, false, w - 2);
}
} // namespace taw
