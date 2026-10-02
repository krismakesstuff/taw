// The command language: [selector] verb [args]
//
//   selector   1 | 1-3 | 1,4 | * (tracks are 1-based on the surface, 0-based inside)
//   @pos       @17  @17.3  @1:30  @0
//
// See docs/design.md for the full table. Every key binding ends up here or in an App method
// with the same name, so anything a person can do, a script or agent can do too.

#include "App.h"

namespace taw
{
namespace
{
struct Parsed
{
    bool hasSelector = false;
    bool mix = false; // "mix v -3"
    std::vector<int> tracks;
    juce::String verb;
    juce::StringArray args;
    juce::String rest; // everything after the verb, verbatim (for names)
};

juce::String parseSelector (const juce::String& text, int numTracks, std::vector<int>& out)
{
    for (const auto& part : juce::StringArray::fromTokens (text, ",", {}))
    {
        if (part == "*")
        {
            for (int i = 0; i < numTracks; ++i)
                out.push_back (i);

            continue;
        }

        const auto dash = part.indexOfChar ('-');
        const auto from = dash > 0 ? part.substring (0, dash).getIntValue() : part.getIntValue();
        const auto to = dash > 0 ? part.substring (dash + 1).getIntValue() : from;

        if (from < 1 || to < from || part.containsOnly ("-"))
            return "bad selector '" + part + "'";

        if (to > numTracks)
            return "no track " + juce::String (to) + " (session has " + juce::String (numTracks) + ")";

        for (int i = from; i <= to; ++i)
            out.push_back (i - 1);
    }

    std::sort (out.begin(), out.end());
    out.erase (std::unique (out.begin(), out.end()), out.end());
    return {};
}

// Every verb, so a verb glued to its argument can be split off: "2i2", "2v-6", "bpm92".
const char* const verbs[] = { "r", "rec", "play", "stop", "mark", "click", "c", "l", "loop", "b", "bounce", "import", "imp", "undo", "redo", "dev", "panel",
                              "help", "q", "quit", "exit", "save", "bpm", "grid", "height", "light", "dark", "grey", "gray", "project", "settings", "sig", "snap", "lat", "add", "new",
                              "a", "arm", "m", "mute", "s", "solo", "mon", "x", "i", "in", "v", "vol", "n", "name",
                              "sample", "audio", "pat", "sel", "copy", "cut", "paste", "dup", "del", "close",
                              "inst", "ed", "scan", "midi", "fill",
                              "bus", "out", ">", "send", "io", "count" };

// Splits "i2" into verb "i" + argument "2". The longest known verb wins ("mon" over "m").
// Letters directly after a verb only count as an argument for names, click modes and bounce
// formats ("2nVox", "clickrec", "bmp3"); otherwise "mutr" is a typo, not "m" + "utr".
void splitVerb (const juce::String& token, juce::String& verb, juce::String& glued)
{
    verb = token;
    glued.clear();
    int best = 0;

    for (const auto* v : verbs)
    {
        const juce::String candidate (v);

        if (candidate.length() > best && candidate.length() < token.length() && token.startsWithIgnoreCase (candidate))
        {
            const auto next = token[candidate.length()];
            const bool takesWords = candidate == "n" || candidate == "name" || candidate == "click" || candidate == "b"
                                    || candidate == "pat" || candidate == "inst" || candidate == "bus" || candidate == "out"
                                    || candidate == ">" || candidate == "send";

            if (takesWords || ! juce::CharacterFunctions::isLetter (next))
                best = candidate.length();
        }
    }

    // An exact verb ("mon", "stop") is never split.
    for (const auto* v : verbs)
        if (token.equalsIgnoreCase (v))
            return;

    if (best > 0)
    {
        verb = token.substring (0, best);
        glued = token.substring (best);
    }
}

juce::String parse (const juce::String& line, int numTracks, Parsed& p)
{
    auto s = line.trim();
    auto first = s.upToFirstOccurrenceOf (" ", false, false);
    auto afterFirst = s.fromFirstOccurrenceOf (" ", false, false).trim();

    auto takeNextWord = [&afterFirst]
    {
        const auto w = afterFirst.upToFirstOccurrenceOf (" ", false, false);
        afterFirst = afterFirst.fromFirstOccurrenceOf (" ", false, false).trim();
        return w;
    };

    // Selector: "mix", or leading track numbers ("1r", "1-3 r", "*m").
    juce::String verbToken;
    int i = 0;

    while (i < first.length() && juce::String ("0123456789,-*").containsChar (first[i]))
        ++i;

    if (first.startsWithIgnoreCase ("mix") && (first.length() > 3 || afterFirst.isNotEmpty()))
    {
        p.hasSelector = p.mix = true;
        verbToken = first.length() > 3 ? first.substring (3) : takeNextWord();
    }
    else if (i > 0)
    {
        p.hasSelector = true;

        if (auto err = parseSelector (first.substring (0, i), numTracks, p.tracks); err.isNotEmpty())
            return err;

        verbToken = i < first.length() ? first.substring (i) : takeNextWord();
    }
    else
    {
        verbToken = first;
    }

    juce::String glued;
    splitVerb (verbToken, p.verb, glued);

    if (glued.isNotEmpty())
        afterFirst = (glued + " " + afterFirst).trim();

    p.verb = p.verb.toLowerCase();
    p.rest = afterFirst;
    p.args = juce::StringArray::fromTokens (afterFirst, " ", "\"");
    p.args.removeEmptyStrings();
    return {};
}
} // namespace

juce::String trackList (const std::vector<int>& tracks)
{
    juce::StringArray s;

    for (auto t : tracks)
        s.add (juce::String (t + 1));

    return (tracks.size() == 1 ? "track " : "tracks ") + s.joinIntoString (",");
}

juce::String explainCommand (const juce::String& line, int numTracks)
{
    Parsed p;

    if (auto err = parse (line, numTracks, p); err.isNotEmpty())
        return "error: " + err;

    juce::StringArray sel;

    for (auto t : p.tracks)
        sel.add (juce::String (t + 1));

    return (p.mix ? juce::String ("mix") : (p.hasSelector ? "tracks " + sel.joinIntoString (",") : juce::String ("(default)")))
           + " | verb " + p.verb.quoted() + " | args " + p.args.joinIntoString ("|").quoted();
}

Result App::gotoPosition (const juce::String& specIn)
{
    juce::int64 pos = 0;

    if (auto r = parsePosition (specIn, pos); r.failed())
        return Result::fail (r.getErrorMessage());

    if (recording)
        return Result::fail ("can't move while recording");

    engine.isRolling() ? engine.seek (pos) : setCursor (pos);
    return Result::good();
}

// "17" bar 17, "17.3" its third beat, "1:30" a time, "0" the very start.
juce::Result App::parsePosition (const juce::String& specIn, juce::int64& pos) const
{
    const auto spec = specIn.trim();

    if (spec.isEmpty())
        return juce::Result::fail ("go where? @17  @17.3  @1:30");

    if (spec.containsChar (':'))
    {
        const auto minutes = spec.upToFirstOccurrenceOf (":", false, false).getDoubleValue();
        const auto seconds = spec.fromFirstOccurrenceOf (":", false, false).getDoubleValue();
        pos = std::llround ((minutes * 60.0 + seconds) * sampleRate());
    }
    else if (spec.containsOnly ("0123456789."))
    {
        const auto bar = spec.upToFirstOccurrenceOf (".", false, false).getIntValue();
        const auto beat = spec.containsChar ('.') ? spec.fromFirstOccurrenceOf (".", false, false).getDoubleValue() : 1.0;

        if (bar == 0 && ! spec.containsChar ('.'))
            pos = 0;
        else if (bar < 1 || beat < 1.0 || beat >= session->beatsPerBar() + 1)
            return juce::Result::fail ("no such position: " + spec);
        else
            pos = std::llround ((bar - 1) * session->samplesPerBar() + (beat - 1.0) * session->samplesPerBeat());
    }
    else
    {
        return juce::Result::fail ("go where? @17  @17.3  @1:30");
    }

    return juce::Result::ok();
}

juce::String App::clickStatus() const
{
    const auto& s = session->tree();

    if (! (bool) s[ids::click])
        return "click off";

    return juce::String ("click on") + (s[ids::clickWhen].toString() == "rec" ? " (recording only)" : " (always)") + L" \x00b7 "
           + juce::String ((double) s[ids::clickDb], 1) + " dB";
}

Result App::setClick (const juce::String& argIn)
{
    auto s = session->tree();
    const auto arg = argIn.trim().toLowerCase();

    if (arg.isEmpty())
        s.setProperty (ids::click, ! (bool) s[ids::click], um());
    else if (arg == "on" || arg == "off")
        s.setProperty (ids::click, arg == "on", um());
    else if (arg == "rec" || arg == "always")
        s.setProperty (ids::clickWhen, arg, um()).setProperty (ids::click, true, um());
    else if (arg.containsOnly ("-+0123456789."))
        s.setProperty (ids::clickDb, juce::jlimit (-60.0, 6.0, arg.getDoubleValue()), um());
    else
        return Result::fail ("click [on|off|rec|always|-12]");

    return Result::good (clickStatus());
}

// count 4 | count off (or 0) | count rec (only when recording starts) | count always (play too)
Result App::setCountIn (const juce::String& argIn)
{
    auto s = session->tree();
    const auto arg = argIn.trim().toLowerCase();

    if (arg.isEmpty())
        return Result::good (countInStatus() + L" \x00b7 count 2, count off, count always");

    if (arg == "off")
        s.setProperty (ids::countIn, 0, um());
    else if (arg == "on")
        s.setProperty (ids::countIn, (int) s[ids::countIn] > 0 ? (int) s[ids::countIn] : 4, um());
    else if (arg == "rec" || arg == "always")
        s.setProperty (ids::countInWhen, arg, um());
    else if (arg.containsOnly ("0123456789"))
        s.setProperty (ids::countIn, juce::jlimit (0, 16, arg.getIntValue()), um());
    else
        return Result::fail ("count [4|off|rec|always]");

    return Result::good (countInStatus());
}

//==============================================================================
// Project settings panel ("project")

std::vector<App::Setting> App::projectSettings() const
{
    if (session == nullptr)
        return {};

    const auto& s = session->tree();
    const auto onOff = [] (bool on) { return juce::String (on ? "on" : "off"); };
    const int height = session->trackHeight();

    return {
        { "Tempo", juce::String (session->bpm(), 1) + " bpm", "bpm " },
        { "Time signature", s[ids::timeSigNum].toString() + "/" + s[ids::timeSigDen].toString(), "sig " },
        { "Grid", onOff (session->gridOn()), {} },
        { "Grid step (alt+arrows)", "1/" + s[ids::gridDiv].toString(), "grid " },
        { "Input quantize (pads)", onOff (s[ids::inputQuantize]), {} },
        { "Click", ! (bool) s[ids::click] ? juce::String ("off") : s[ids::clickWhen].toString() == "rec" ? juce::String ("recording only") : juce::String ("always"), {} },
        { "Click level", juce::String ((double) s[ids::clickDb], 1) + " dB", "click " },
        { "Count-in", (int) s[ids::countIn] <= 0 ? juce::String ("off") : s[ids::countIn].toString() + ((int) s[ids::countIn] == 1 ? " click" : " clicks"), "count " },
        { "Count-in when", s[ids::countInWhen].toString() == "always" ? juce::String ("recording and playback") : juce::String ("recording"), {} },
        { "Latency compensation", onOff (s[ids::latencyComp]), {} },
        { "Track height", juce::String (height) + (height == 1 ? " row" : " rows"), "height " },
        { "Bounce format", s[ids::bounceFormat].toString(), {} },
        { "Bounce folder", s[ids::bounceFolder].toString() + L"  (b wav <folder> changes it)", {}, false },
        { "Sample rate", juce::String (sampleRate() / 1000.0, 1) + L" kHz  (set when the session was made)", {}, false },
        { "Theme (all sessions)", theme, {} },
    };
}

// Left / right on a setting. Goes through the same commands you'd type, so undo names them.
Result App::stepSetting (int index, int direction)
{
    const auto& s = session->tree();
    const auto settings = projectSettings();

    if (index < 0 || index >= (int) settings.size() || ! settings[(size_t) index].editable)
        return Result::fail (L"this one is read-only");

    auto cycle = [direction] (const juce::StringArray& options, const juce::String& current)
    {
        const auto i = options.indexOf (current);
        return options[(juce::jmax (0, i) + direction + options.size()) % options.size()];
    };

    // By name, so rows can be added or moved in projectSettings without renumbering.
    const auto& name = settings[(size_t) index].name;

    if (name == "Tempo")          return execute ("bpm " + juce::String (juce::jlimit (20.0, 400.0, std::round (session->bpm()) + direction)));
    if (name == "Time signature") return execute ("sig " + juce::String (juce::jlimit (1, 16, (int) s[ids::timeSigNum] + direction)) + "/" + s[ids::timeSigDen].toString());
    if (name == "Grid")           return execute ("grid");
    if (name.startsWith ("Grid step")) return execute ("grid " + cycle ({ "4", "8", "16", "32" }, s[ids::gridDiv].toString()));
    if (name.startsWith ("Input quantize")) return execute ("snap input");
    if (name == "Click")          return execute ("click " + cycle ({ "off", "always", "rec" }, ! (bool) s[ids::click] ? juce::String ("off") : s[ids::clickWhen].toString()));
    if (name == "Click level")    return execute ("click " + juce::String (juce::jlimit (-60.0, 6.0, std::round ((double) s[ids::clickDb]) + direction), 1));
    if (name == "Count-in")       return execute ("count " + juce::String (juce::jlimit (0, 16, (int) s[ids::countIn] + direction)));
    if (name == "Count-in when")  return execute (juce::String ("count ") + (s[ids::countInWhen].toString() == "always" ? "rec" : "always"));
    if (name == "Latency compensation") return execute ("lat");
    if (name == "Track height")   return execute ("height " + juce::String (juce::jlimit (1, 4, session->trackHeight() + direction)));
    if (name.startsWith ("Theme")) return execute (cycle ({ "dark", "light", "grey" }, theme));

    if (name == "Bounce format")
    {
        beginEdit ("bounce format");
        const auto format = cycle ({ "wav", "mp3" }, s[ids::bounceFormat].toString());
        session->tree().setProperty (ids::bounceFormat, format, um());
        return Result::good ("bounces as " + format);
    }

    return Result::fail (L"this one is read-only");
}

bool App::handleSettingsKey (const Key& k)
{
    if (! settingsVisible || ! commandLine.isEmpty() || session == nullptr)
        return false; // while you type, keys go to the command line as usual

    const auto settings = projectSettings();
    settingsIndex = juce::jlimit (0, (int) settings.size() - 1, settingsIndex);

    switch (k.code)
    {
        case Key::Up:   settingsIndex = juce::jmax (0, settingsIndex - 1); return true;
        case Key::Down: settingsIndex = juce::jmin ((int) settings.size() - 1, settingsIndex + 1); return true;
        case Key::Left:
        case Key::Right:
            say (stepSetting (settingsIndex, k.code == Key::Right ? +1 : -1));
            return true;
        case Key::Enter:
            // A value to type: start the command for it (bpm 92 + enter). A switch: flip it.
            if (const auto& row = settings[(size_t) settingsIndex]; row.command.isNotEmpty())
                commandLine = row.command;
            else
                say (stepSetting (settingsIndex, +1));
            return true;
        case Key::Escape:
            settingsVisible = false;
            return true;
        default:
            return false;
    }
}

//==============================================================================
// Sample tracks

Result App::convertTrack (int index, bool toSample)
{
    auto track = session->track (index);
    const auto label = "track " + juce::String (index + 1);

    if (Session::isBusTrack (track))
        return Result::fail (label + " is a bus");

    if (Session::isInstrumentTrack (track))
        return Result::fail (label + L" is an instrument track \x2014 inst swaps its plugin");

    if (! toSample)
    {
        if (! Session::isSampleTrack (track))
            return Result::fail (label + " is already an audio track");

        track.setProperty (ids::kind, "audio", um());
        return Result::good (label + L" is audio again \x00b7 its hits are kept (" + juce::String (index + 1) + " sample switches back)");
    }

    if (Session::isSampleTrack (track))
        return Result::fail (label + " is already a sample track");

    // The sample is the track's latest clip (as trimmed), unless it had one from before.
    if (track[ids::sample].toString().isEmpty())
    {
        const auto clip = Session::lastClip (track);

        if (! clip.isValid())
            return Result::fail (label + L" has no audio to sample \x2014 sample <file> makes a new sample track");

        track.setProperty (ids::sample, clip[ids::file], um())
            .setProperty (ids::sampleOffset, clip[ids::offset], um())
            .setProperty (ids::sampleLength, clip[ids::length], um());
    }

    // So it sounds the same: a hit wherever a clip started (only if it has no hits yet).
    int added = 0;

    if (Session::hits (track).empty())
        for (const auto& c : Session::clips (track))
        {
            track.appendChild (Session::makeHit ((double) (juce::int64) c[ids::start] / session->samplesPerBeat(), 1.0f), um());
            ++added;
        }

    track.setProperty (ids::kind, "sample", um());
    return Result::good (label + " is a sample track" + (added > 0 ? " (" + juce::String (added) + " hits from its clips)" : juce::String())
                         + L" \x00b7 " + juce::String (index + 1) + " audio switches back");
}

// "x...x...x...x..." over the selection (repeating), or from the cursor for a bar.
// X = accent, x = normal, o = ghost, . or - = rest; spaces and | are ignored.
Result App::writePattern (const std::vector<int>& tracks, const juce::String& stepsIn, double stepBeats)
{
    juce::Array<float> steps;

    for (auto p = stepsIn.getCharPointer(); ! p.isEmpty();)
    {
        const auto c = p.getAndAdvance();

        if (c == 'X')                  steps.add (1.0f);
        else if (c == 'x')             steps.add (0.8f);
        else if (c == 'o' || c == 'O') steps.add (0.45f);
        else if (c == '.' || c == '-') steps.add (0.0f);
        else if (c != ' ' && c != '|')
            return Result::fail (juce::String ("pattern steps are X x o . (got '") + juce::String::charToString (c) + "')");
    }

    if (steps.isEmpty())
        return Result::fail ("pattern? e.g. 1 pat x...x...x...x...");

    if (tracks.empty())
        return Result::fail ("which track? e.g. 1 pat x...x...");

    for (auto t : tracks)
        if (! Session::isSampleTrack (session->track (t)))
            return Result::fail ("track " + juce::String (t + 1) + L" isn't a sample track \x2014 " + juce::String (t + 1) + " sample converts it");

    // Positions are whole samples, so at tempos like 99 bpm a step lands a fraction of a sample
    // off the exact beat. Anything within half a sample of a step is on it.
    const auto spb = session->samplesPerBeat();
    const auto halfSample = 0.5 / spb; // in beats
    auto toBeats = [&] (juce::int64 samples)
    {
        const auto b = (double) samples / spb;
        const auto onStep = std::round (b / stepBeats) * stepBeats;
        return std::abs (onStep - b) <= halfSample ? onStep : b;
    };

    double from, to;

    if (hasTimeSelection())
    {
        from = toBeats (timeSelection().getStart());
        to = toBeats (timeSelection().getEnd());
    }
    else
    {
        // From the cursor (snapped down to a step), for a bar or the whole pattern if longer.
        from = std::floor (toBeats (cursor) / stepBeats + halfSample) * stepBeats;
        to = from + juce::jmax ((double) session->beatsPerBar(), steps.size() * stepBeats);
    }

    int written = 0;

    for (auto t : tracks)
    {
        auto track = session->track (t);

        // The pattern replaces whatever hits were in the range.
        for (const auto& h : Session::hits (track))
            if (const double b = h[ids::beat]; b >= from - halfSample && b < to - halfSample)
                track.removeChild (h, um());

        for (int k = 0; from + k * stepBeats < to - halfSample; ++k)
            if (const auto vel = steps[k % steps.size()]; vel > 0.0f)
            {
                track.appendChild (Session::makeHit (from + k * stepBeats, vel), um());
                ++written;
            }
    }

    return Result::good (juce::String (written) + " hits on " + juce::String (tracks.size() == 1 ? "track " + juce::String (tracks.front() + 1) : juce::String ((int) tracks.size()) + " tracks")
                         + L" \x00b7 " + formatBars (std::llround (from * spb)) + L" \x2192 " + formatBars (std::llround (to * spb)));
}

Result App::toggleHitAtCursor()
{
    if (session == nullptr || mixFocused() || onTopLane() || ! Session::isSampleTrack (session->track (focused)))
        return Result::fail (L"alt+/ places hits on sample tracks \x2014 focus one with \x2191\x2193");

    auto track = session->track (focused);
    const auto step = hitStepBeats();
    const auto at = engine.isRolling() ? engine.playhead() : cursor;
    auto beat = (double) at / session->samplesPerBeat();

    for (const auto& h : Session::hits (track))
        if (std::abs ((double) h[ids::beat] - beat) < step * 0.5)
        {
            beginEdit ("remove hit");
            track.removeChild (h, um());
            return Result::good ("removed hit at " + formatBars (at));
        }

    if (session->gridOn())
        beat = std::round (beat / step) * step;

    beginEdit ("add hit");
    track.appendChild (Session::makeHit (beat, 0.8f), um());
    return Result::good ("hit at " + formatBars (std::llround (beat * session->samplesPerBeat())));
}

Result App::setGridDivision (int noteValue)
{
    if (noteValue != 4 && noteValue != 8 && noteValue != 16 && noteValue != 32)
        return Result::fail ("grid 4, 8, 16 or 32");

    session->tree().setProperty (ids::gridDiv, noteValue, um()).setProperty (ids::grid, true, um());

    // One column per step, so the lanes read as a step sequencer.
    zoomLevel = -(int) std::round (std::log2 (noteValue / 4.0));
    keepInView (cursor);
    return Result::good ("grid 1/" + juce::String (noteValue) + L" \x00b7 alt \x2190\x2192 moves a step \x00b7 alt+/ toggles a hit on a sample track");
}

Result App::setLoop (const juce::String& argIn)
{
    auto s = session->tree();
    const auto arg = argIn.trim().toLowerCase();
    const auto start = (juce::int64) s[ids::loopStart], end = (juce::int64) s[ids::loopEnd];
    const bool haveRange = end > start;
    auto describe = [this] (juce::int64 a, juce::int64 b) { return formatBars (a) + L" \x2192 " + formatBars (b); };

    if (arg == "off")
    {
        s.setProperty (ids::loop, false, um());
        return Result::good ("loop off");
    }

    // A fresh time selection always becomes the loop.
    if (arg.isEmpty() && hasTimeSelection())
    {
        const auto r = timeSelection();
        s.setProperty (ids::loopStart, r.getStart(), um())
            .setProperty (ids::loopEnd, r.getEnd(), um())
            .setProperty (ids::loop, true, um());
        clearTimeSelection();
        return Result::good ("loop " + describe (r.getStart(), r.getEnd()));
    }

    if (! haveRange)
        return Result::fail (L"select a time range first (shift + \x2190 \x2192), then l");

    if (arg == "on" || (arg.isEmpty() && ! (bool) s[ids::loop]))
    {
        s.setProperty (ids::loop, true, um());
        return Result::good ("loop " + describe (start, end) + (recording ? " (after this take)" : ""));
    }

    if (arg.isEmpty())
    {
        s.setProperty (ids::loop, false, um());
        return Result::good ("loop off");
    }

    return Result::fail ("loop [on|off]");
}

Result App::execute (const juce::String& line)
{
    if (session == nullptr)
        return Result::fail ("no session");

    const auto trimmed = line.trim();

    // Every command line is one undo step, named after itself ("undid 2v-6").
    beginEdit (trimmed);

    if (trimmed.startsWithChar ('@'))
        return gotoPosition (trimmed.substring (1));

    Parsed p;

    if (auto err = parse (trimmed, session->numTracks(), p); err.isNotEmpty())
        return Result::fail (err);

    const auto& v = p.verb;

    // A bare selector focuses: "3" then Enter.
    if (v.isEmpty())
    {
        if (p.tracks.empty())
            return Result::fail ("no tracks");

        focusTrack (p.tracks.front());
        return Result::good();
    }

    // The mix only has a volume (for now).
    const bool onMix = p.mix || (! p.hasSelector && selectedTracks().empty() && mixFocused());

    if (onMix && (v == "v" || v == "vol"))
    {
        if (p.args.isEmpty())
            return Result::fail ("mix volume in dB, e.g. mix v -3");

        return setMixVolume (p.args[0].getDoubleValue());
    }

    if (p.mix)
        return Result::fail ("the mix only takes v, e.g. mix v -3");

    // Global verbs ------------------------------------------------------------
    // rec records the armed tracks (ctrl+space does the same); "1 rec" arms exactly track 1
    // first. Arming itself is "r", below.
    if (v == "rec")                return record (p.hasSelector ? p.tracks : selectedTracks());
    if (v == "play")               return play();
    if (v == "stop")               return stop();
    if (v == "mark")               return addMarker (engine.isRolling() ? engine.playhead() : cursor, p.rest.trim());
    if (v == "click" || v == "c")  return setClick (p.args[0]);
    if (v == "count")              return setCountIn (p.args[0]);
    if (v == "l" || v == "loop")   return setLoop (p.args[0]);
    if (v == "b" || v == "bounce") return startBounce (p.args);
    if (v == "import" || v == "imp")
        return startImport (splitPaths (p.rest), p.hasSelector && ! p.tracks.empty() ? p.tracks.front() : -1);
    if (v == "undo")               return undoEdit();
    if (v == "redo")               return redoEdit();
    if ((v == "dev" || v == "panel") && engine.isRolling())
        return Result::fail ("stop playback before changing the audio device");
    if (v == "dev")                { openDevicePicker(); return Result::good(); }
    if (v == "panel")              return showControlPanel();
    if (v == "help" || v == "?")   { helpVisible = ! helpVisible; settingsVisible = false; return Result::good(); }
    if (v == "project" || v == "settings")
    {
        settingsVisible = ! settingsVisible;
        helpVisible = false;
        return Result::good (settingsVisible ? juce::String (L"\x2191\x2193 choose \x00b7 \x2190\x2192 change \x00b7 enter types a value \x00b7 esc closes") : juce::String());
    }
    if (v == "q" || v == "quit" || v == "exit") { quit(); return Result::good(); }

    // Instruments: inst <name> (a new instrument track; 3 inst <name> puts it on track 3), inst
    // alone for the picker, ed for the plugin's window, scan to find plugins, midi for inputs.
    if (v == "inst")                return loadInstrument (p.hasSelector ? p.tracks : std::vector<int> {}, p.rest);
    if (v == "scan")               return scanPlugins (p.args[0].equalsIgnoreCase ("retry"));
    if (v == "midi")               return toggleMidiPanel();
    if (v == "ed")
    {
        const auto targets = p.hasSelector ? p.tracks : defaultTargets();
        return toggleEditor ((onTopLane() && ! p.hasSelector) || targets.empty() ? -1 : targets.front());
    }

    // fill (the f key): the focused track takes the whole track area; on an instrument track
    // that's a piano roll. Up and down still move between tracks.
    if (v == "fill")
    {
        fillView = ! fillView;
        return Result::good (fillView ? juce::String (L"fill view \x00b7 \x2191\x2193 other tracks \x00b7 f again shows them all") : juce::String());
    }
    if (v == "close")              return closeSession();

    // Routing: bus <name> adds a bus track; io shows or hides the routing column (o steps into it).
    if (v == "bus" || ((v == "add" || v == "new") && p.args[0].equalsIgnoreCase ("bus")))
    {
        if (p.hasSelector)
            return Result::fail ("bus <name> adds a bus track; 2 out <bus> sends track 2 to it");

        return addBus (v == "bus" ? p.rest : p.rest.fromFirstOccurrenceOf (" ", false, false));
    }

    if (v == "io")
    {
        ioVisible = ! ioVisible;
        ioCell = ioVisible ? ioCell : -1;
        saveSettings();
        return Result::good (ioVisible ? juce::String (L"routing column on \x00b7 o steps into it") : juce::String ("routing column off"));
    }

    // Not in the help: crashes on purpose, to check what a crash leaves behind.
    if (v == "crashtest")
    {
        volatile int* nowhere = nullptr;
        *nowhere = 1;
    }

    // Editing over the time selection: ctrl+c / x / v / d and delete do the same.
    if (v == "sel")   return selectRange (p.args[0], p.args[1]);
    if (v == "copy")  return copySelection (p.hasSelector ? p.tracks : defaultTargets());
    if (v == "cut")   return cutSelection (p.hasSelector ? p.tracks : defaultTargets());
    if (v == "del")   return deleteSelection (p.hasSelector ? p.tracks : defaultTargets());
    if (v == "dup")   return duplicateSelection (p.hasSelector ? p.tracks : defaultTargets());
    if (v == "paste")
    {
        const auto targets = p.hasSelector ? p.tracks : defaultTargets();
        return paste (targets.empty() ? -1 : targets.front());
    }

    if (v == "save")
        return session->save() ? Result::good ("saved " + session->folder().getFullPathName())
                               : Result::fail ("couldn't write session.json");

    if (v == "bpm")
    {
        const auto bpm = p.args[0].getDoubleValue();

        if (bpm < 20.0 || bpm > 400.0)
            return Result::fail ("bpm between 20 and 400");

        session->tree().setProperty (ids::bpm, bpm, um());
        return Result::good ("tempo " + juce::String (bpm, 1));
    }

    if (v == "grid" && p.args[0].containsOnly ("0123456789") && p.args[0].isNotEmpty())
        return setGridDivision (p.args[0].getIntValue());

    if (v == "grid")
    {
        session->tree().setProperty (ids::grid, ! session->gridOn(), um());
        zoomLevel = 0;
        keepInView (cursor);
        return Result::good (session->gridOn() ? L"grid on \x00b7 arrows move by bars" : L"grid off \x00b7 arrows move by seconds");
    }

    // snap / snap 50 / snap input: quantize hits ("quantize" would start with q, a view key).
    if (v == "snap")
        return quantizeHits (p.rest, p.hasSelector ? p.tracks : defaultTargets());

    // sig 3/4: time signature (beats per bar / note value).
    if (v == "sig")
    {
        const auto num = p.args[0].upToFirstOccurrenceOf ("/", false, false).getIntValue();
        const auto den = p.args[0].containsChar ('/') ? p.args[0].fromFirstOccurrenceOf ("/", false, false).getIntValue()
                                                      : (int) session->tree()[ids::timeSigDen];

        if (num < 1 || num > 16 || (den != 2 && den != 4 && den != 8 && den != 16))
            return Result::fail ("sig 3/4 (1-16 beats over 2, 4, 8 or 16)");

        session->tree().setProperty (ids::timeSigNum, num, um()).setProperty (ids::timeSigDen, den, um());
        return Result::good ("time signature " + juce::String (num) + "/" + juce::String (den));
    }

    // height 3: rows per track, saved with the session.
    if (v == "height")
    {
        if (p.args[0].isEmpty())
            return Result::good ("tracks are " + juce::String (session->trackHeight()) + L" rows tall \x00b7 height 1-4 changes it");

        const auto rows = p.args[0].getIntValue();

        if (rows < 1 || rows > 4)
            return Result::fail ("height 1 to 4 (rows per track)");

        session->tree().setProperty (ids::trackHeight, rows, um());
        return Result::good ("tracks " + juce::String (rows) + (rows == 1 ? " row" : " rows") + " tall");
    }

    // dark / light / grey: the colour theme, for every session (saved in settings.json).
    if (v == "light" || v == "dark" || v == "grey" || v == "gray")
    {
        theme = v == "gray" ? juce::String ("grey") : v;
        saveSettings();
        return Result::good (theme + " theme");
    }

    if (v == "lat")
    {
        const bool on = ! (bool) session->tree()[ids::latencyComp];
        session->tree().setProperty (ids::latencyComp, on, um());
        return Result::good (on ? "latency compensation on (new takes)" : "latency compensation off (new takes)");
    }

    // add | add 3 | add Vox | add vox1 in2 | add 2 Gtr in3  (Gtr 1 on in3, Gtr 2 on in4).
    // Without an input, each new track takes the lowest input no other track is using.
    // sample <file|folder>  /  add sample <file>   new sample track(s)
    // 3 sample <file>                             give track 3 a new sample, keeping its hits
    // 3 sample                                    convert audio track 3 (its clip becomes the sample)
    if (v == "sample" || ((v == "add" || v == "new") && p.args[0].equalsIgnoreCase ("sample")))
    {
        const auto rest = v == "sample" ? p.rest : p.rest.fromFirstOccurrenceOf (" ", false, false).trim();
        const auto paths = splitPaths (rest);

        if (paths.isEmpty())
        {
            if (! p.hasSelector && selectedTracks().empty() && (mixFocused() || session->numTracks() == 0))
                return Result::fail ("sample <file> makes a sample track, 3 sample converts track 3");

            Result last;

            for (auto t : (p.hasSelector ? p.tracks : defaultTargets()))
                last = convertTrack (t, true);

            return last;
        }

        return startImport (paths, p.hasSelector && ! p.tracks.empty() ? p.tracks.front() : -1, ImportAs::sample);
    }

    if (v == "add" || v == "new")
    {
        auto args = p.args;
        int count = 1, firstInput = -1;

        if (! args.isEmpty() && args[0].containsOnly ("0123456789"))
        {
            count = juce::jlimit (1, 64, args[0].getIntValue());
            args.remove (0);
        }

        for (int a = args.size(); --a >= 0;)
        {
            const auto& arg = args[a];
            const auto digits = arg.startsWithIgnoreCase ("in") ? arg.substring (2) : juce::String();

            if (digits.isNotEmpty() && digits.containsOnly ("0123456789"))
            {
                firstInput = digits.getIntValue();
                args.remove (a);
            }
        }

        const auto baseName = args.joinIntoString (" ").unquoted();
        juce::StringArray added;

        for (int i = 0; i < count; ++i)
        {
            std::vector<bool> used ((size_t) engine.numInputChannels() + 1, false);

            for (int t = 0; t < session->numTracks(); ++t)
                if (const int in = session->track (t)[ids::input]; in > 0 && in < (int) used.size())
                    used[(size_t) in] = true;

            int input = 0;

            if (firstInput >= 0)
                input = firstInput == 0 ? 0 : firstInput + i;
            else
                for (int in = 1; in < (int) used.size() && input == 0; ++in)
                    if (! used[(size_t) in])
                        input = in;

            const auto n = session->numTracks() + 1;
            const auto name = baseName.isEmpty() ? "Track " + juce::String (n)
                                                 : (count == 1 ? baseName : baseName + " " + juce::String (i + 1));
            session->addTrack (name, input, um());
            added.add (juce::String (n) + " " + name + (input > 0 ? " (in" + juce::String (input) + ")" : juce::String (" (no input)")));
        }

        focusTrack (session->numTracks() - 1);

        if (firstInput > 0 && firstInput + count - 1 > engine.numInputChannels())
            return Result::fail ("added " + added.joinIntoString (", ") + L" \x00b7 device only has "
                                 + juce::String (engine.numInputChannels()) + " inputs");

        return Result::good ("added " + added.joinIntoString (", "));
    }

    // Track verbs: numbers, else the shift-selection, else the focused track -------
    std::vector<int> targets = p.hasSelector ? p.tracks : defaultTargets();

    if (session->numTracks() == 0)
        return Result::fail (L"no tracks \x2014 add");

    if (targets.empty())
        return Result::fail ("the mix only takes v, e.g. v -3");

    // From the bar or markers row, defaultTargets is every track (for edits); track settings
    // want a track named or focused.
    if (onTopLane() && ! p.hasSelector)
        return Result::fail (L"\x2193 to a track first, or name one: 2m");

    auto toggle = [&] (const juce::Identifier& id, const juce::String& label) -> Result
    {
        // A group toggles together: if any are off, all go on; if all are on, all go off.
        bool on = false;

        for (auto t : targets)
            on = on || ! (bool) session->track (t)[id];

        for (auto t : targets)
            session->track (t).setProperty (id, on, um());

        return Result::good (trackList (targets) + " " + (on ? "" : "un") + label);
    };

    // r arms (record-enable + input monitoring); ctrl+space then records.
    // Armed audio tracks monitor their input; armed sample tracks listen to the pad key (.).
    if (v == "r" || v == "a" || v == "arm")
    {
        for (auto t : targets)
            if (Session::isBusTrack (session->track (t)))
                return Result::fail ("track " + juce::String (t + 1) + L" is a bus \x2014 it plays what's sent to it, nothing to record");

        const auto r = toggle (ids::armed, "armed");

        if (! (bool) session->track (targets.front())[ids::armed])
            return r;

        if (Session::isInstrumentTrack (session->track (targets.front())))
            return Result::good (r.message + L" \x00b7 your MIDI controller plays it \x00b7 ctrl+space records what you play");

        return Session::isSampleTrack (session->track (targets.front()))
                   ? Result::good (r.message + L" \x00b7 . plays it (shift: accent) \x00b7 ctrl+space records what you play")
                   : Result::good (r.message + L" \x00b7 monitoring its input \x00b7 ctrl+space records");
    }

    if (v == "m" || v == "mute") return toggle (ids::mute, "muted");
    if (v == "s" || v == "solo") return toggle (ids::solo, "soloed");

    // mon: an armed track keeps its input in your ears during playback too (to audition a part
    // against the mix), instead of switching to its clips. Off = auto input.
    if (v == "mon")
    {
        toggle (ids::monitor, "monitoring input");
        const bool on = session->track (targets.front())[ids::monitor];
        return Result::good (trackList (targets) + (on ? L": input during playback too (while armed) \x00b7 mon again for auto"
                                                      : L": auto input (input when stopped or recording, clips on playback)"));
    }

    // 2 out verb / 2 > verb / 2 out mix: where the track goes after its fader. Alone: the picker.
    if (v == "out" || v == ">")
        return setOutput (targets, p.rest);

    // 2 send verb / 2 send b -6 / 2 send b pre|post|off|on|x / 2 send (lists them)
    if (v == "send")
        return editSend (targets, p.args);

    if (v == "audio")
    {
        Result last;

        for (auto t : targets)
            last = convertTrack (t, false);

        return last;
    }

    // pat x...x...   pat /8 x.x.x.x.   pat X..x..o.  (step = 1/16 unless /N given)
    if (v == "pat")
    {
        auto args = p.args;
        double stepBeats = 0.25;

        if (! args.isEmpty() && args[0].startsWithChar ('/'))
        {
            const auto digits = args[0].substring (1).initialSectionContainingOnly ("0123456789");
            const auto division = digits.getIntValue();

            if (division != 4 && division != 8 && division != 16 && division != 32)
                return Result::fail ("step size /4, /8, /16 or /32");

            stepBeats = 4.0 / division;
            args.set (0, args[0].substring (1 + digits.length())); // "/8x.x." keeps its steps
        }

        return writePattern (targets, args.joinIntoString (""), stepBeats);
    }

    if (v == "x")
    {
        Result last;

        for (auto t : targets)
            last = discardTake (t);

        return last;
    }

    if (v == "i" || v == "in")
    {
        if (p.args.isEmpty() || ! p.args[0].containsOnly ("0123456789"))
            return Result::fail ("which input? e.g. 1 i 2 (0 = none)");

        const auto input = p.args[0].getIntValue();

        for (auto t : targets)
            if (Session::isBusTrack (session->track (t)))
                return Result::fail ("track " + juce::String (t + 1) + L" is a bus \x2014 no input; 2 out " + juce::String (t + 1) + " feeds it");

        for (auto t : targets)
            session->track (t).setProperty (ids::input, input, um());

        auto msg = trackList (targets) + (input == 0 ? L" \x00b7 no input" : L" \x00b7 input " + juce::String (input));

        if (input > engine.numInputChannels())
            return Result::fail (msg + " (device only has " + juce::String (engine.numInputChannels()) + ")");

        return Result::good (msg);
    }

    if (v == "v" || v == "vol")
    {
        if (p.args.isEmpty())
            return Result::fail ("volume in dB, e.g. 2 v -6");

        const auto db = juce::jlimit (-96.0, 12.0, p.args[0].getDoubleValue());

        for (auto t : targets)
            session->track (t).setProperty (ids::gainDb, db, um());

        return Result::good (trackList (targets) + L" \x00b7 " + formatDb (db) + " dB");
    }

    if (v == "n" || v == "name")
    {
        if (p.rest.isEmpty())
            return Result::fail ("name what? e.g. 1 n Vox");

        for (auto t : targets)
            session->track (t).setProperty (ids::name, p.rest.unquoted(), um());

        return Result::good (trackList (targets) + " named " + p.rest.unquoted());
    }

    return Result::fail ("unknown command '" + v + L"' \x00b7 ? for help");
}
} // namespace taw
