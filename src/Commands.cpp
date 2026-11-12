// The command language: [selector] verb [args]
//
//   selector   1 | 1-3 | 1,4 | * | 2.3 | 2.1-4 (tracks as numbered on screen, 0-based indexes
//              inside; a kit's pads aren't numbered as tracks: 2.3 is pad 3 of kit 2)
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

// Track numbers as they're shown (TrackNumbering): 3, 1-3, 1,4, * (every track, pads too), and a
// kit's pads by kit and pad: 2.3, 2.1-4.
juce::String parseSelector (const juce::String& text, const TrackNumbering& numbers, std::vector<int>& out)
{
    const auto numTracks = (int) numbers.byNumber.size();

    for (const auto& part : juce::StringArray::fromTokens (text, ",", {}))
    {
        if (part == "*")
        {
            for (int i = 0; i < (int) numbers.labels.size(); ++i)
                out.push_back (i);

            continue;
        }

        // Pads: kit.pad or kit.from-to.
        const auto dot = part.indexOfChar ('.');
        const auto range = dot >= 0 ? part.substring (dot + 1) : part;
        const auto dash = range.indexOfChar ('-');
        const auto from = dash > 0 ? range.substring (0, dash).getIntValue() : range.getIntValue();
        const auto to = dash > 0 ? range.substring (dash + 1).getIntValue() : from;

        if (from < 1 || to < from || range.containsOnly ("-") || range.isEmpty())
            return "bad selector '" + part + "'";

        if (dot >= 0)
        {
            const auto kit = part.substring (0, dot).getIntValue();
            const auto it = numbers.pads.find (kit);

            if (it == numbers.pads.end())
                return "no kit " + juce::String (kit) + L" \x2014 2.3 is pad 3 of kit 2";

            for (int pad = from; pad <= to; ++pad)
            {
                if (it->second.count (pad) == 0)
                {
                    if (from == to)
                        return "kit " + juce::String (kit) + " has no pad " + juce::String (pad);

                    continue; // a range takes the pads there are
                }

                out.push_back (it->second.at (pad));
            }

            continue;
        }

        if (to > numTracks)
            return "no track " + juce::String (to) + " (session has " + juce::String (numTracks) + ")";

        for (int i = from; i <= to; ++i)
            out.push_back (numbers.byNumber[(size_t) (i - 1)]);
    }

    std::sort (out.begin(), out.end());
    out.erase (std::unique (out.begin(), out.end()), out.end());
    return {};
}

// Every verb, so a verb glued to its argument can be split off: "2i2", "2v-6", "bpm92".
const char* const verbs[] = { "r", "rec", "play", "stop", "mark", "click", "c", "l", "loop", "b", "bounce", "bounces", "import", "imp", "undo", "redo", "dev", "panel",
                              "help", "q", "quit", "exit", "save", "cons", "consolidate", "sep", "separate", "split", "bpm", "grid", "height", "light", "dark", "grey", "gray", "project", "settings", "sig", "snap", "lat", "add", "new",
                              "a", "arm", "m", "mute", "s", "solo", "mon", "x", "i", "in", "v", "vol", "n", "name",
                              "sample", "audio", "remove", "rm", "pat", "sel", "copy", "cut", "paste", "dup", "del", "close",
                              "inst", "ed", "edit", "scan", "midi", "stats", "phone", "fill",
                              "bus", "out", ">", "send", "io", "count", "fx", "ins",
                              "pan", "mono", "stereo", "lowlat", "browse", "index", "library", "video", "keycast", "bv", "lane", "pt", "map", "unmap", "aw", "auto", "hub", "pl", "clip", "xfade", "declick", "kit", "note", "choke", "pad" };

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
                                    || candidate == ">" || candidate == "send" || candidate == "fx" || candidate == "ins"
                                    || candidate == "pan" || candidate == "index";

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

juce::String parse (const juce::String& line, const TrackNumbering& numbers, Parsed& p)
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

    while (i < first.length() && juce::String ("0123456789,-*.").containsChar (first[i]))
        ++i;

    if (first.startsWithIgnoreCase ("mix") && (first.length() > 3 || afterFirst.isNotEmpty()))
    {
        p.hasSelector = p.mix = true;
        verbToken = first.length() > 3 ? first.substring (3) : takeNextWord();
    }
    else if (i > 0)
    {
        p.hasSelector = true;

        if (auto err = parseSelector (first.substring (0, i), numbers, p.tracks); err.isNotEmpty())
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

juce::String explainCommand (const juce::String& line, int numTracks)
{
    Parsed p;
    TrackNumbering numbers;

    for (int t = 0; t < numTracks; ++t)
        numbers.byNumber.push_back (t);

    if (auto err = parse (line, numbers, p); err.isNotEmpty())
        return "error: " + err;

    juce::StringArray sel;

    for (auto t : p.tracks)
        sel.add (juce::String (t + 1));

    return (p.mix ? juce::String ("mix") : (p.hasSelector ? "tracks " + sel.joinIntoString (",") : juce::String ("(default)")))
           + " | verb " + p.verb.quoted() + " | args " + p.args.joinIntoString ("|").quoted();
}

CommandShape commandShape (const juce::String& line, const TrackNumbering& numbers)
{
    Parsed p;
    CommandShape s;
    s.error = parse (line, numbers, p);
    s.verb = p.verb;
    s.hasSelector = p.hasSelector;
    s.mix = p.mix;
    s.tracks = p.tracks;
    s.args = p.args;
    return s;
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

// "always" by any start of it: click a, count al.
static juce::String whenArg (const juce::String& arg)
{
    return arg.isNotEmpty() && juce::String ("always").startsWith (arg) ? juce::String ("always") : arg;
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
    const auto arg = whenArg (argIn.trim().toLowerCase());

    if (arg.isEmpty())
        s.setProperty (ids::click, ! (bool) s[ids::click], um());
    else if (arg == "on" || arg == "off")
        s.setProperty (ids::click, arg == "on", um());
    else if (arg == "rec" || arg == "always")
        s.setProperty (ids::clickWhen, arg, um()).setProperty (ids::click, true, um());
    else if (arg.containsOnly ("-+0123456789."))
        s.setProperty (ids::clickDb, juce::jlimit (-60.0, 6.0, arg.getDoubleValue()), um());
    else
        return Result::fail ("click [on|off|rec|always (a)|-12]");

    return Result::good (clickStatus());
}

// lowlat (toggle) | lowlat on | lowlat off | lowlat 256 (the limit, in samples)
Result App::setLowLatency (const juce::String& argIn)
{
    const auto arg = argIn.trim().toLowerCase();

    if (arg.isEmpty())
        lowLatency = ! lowLatency;
    else if (arg == "on" || arg == "off")
        lowLatency = arg == "on";
    else if (arg.containsOnly ("0123456789"))
        session->tree().setProperty (ids::lowLatencyMax, juce::jlimit (0, 8192, arg.getIntValue()), um());
    else
        return Result::fail ("lowlat [on|off|256]");

    rebuildGraph();
    const int max = session->tree()[ids::lowLatencyMax];
    const auto limit = juce::String (max) + " samples (" + juce::String (max * 1000.0 / sampleRate(), 1) + " ms)";

    if (! lowLatency)
        return Result::good ("low latency off \x00b7 every plugin back in" + juce::String (mixLatency > 0 ? L" \x00b7 the mix runs " + juce::String (mixLatency * 1000.0 / sampleRate(), 1) + " ms late" : ""));

    return Result::good (L"low latency on \x00b7 plugins over " + limit + L" bypassed (shown yellow) \x00b7 the mix now runs "
                         + juce::String (mixLatency * 1000.0 / sampleRate(), 1) + L" ms late \x00b7 lowlat again restores them");
}

// count 4 | count off (or 0) | count rec (only when recording starts) | count always / count a (play too)
Result App::setCountIn (const juce::String& argIn)
{
    auto s = session->tree();
    const auto arg = whenArg (argIn.trim().toLowerCase());

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
        return Result::fail ("count [4|off|rec|always (a)]");

    return Result::good (countInStatus());
}

//==============================================================================
// Project settings panel ("project")

static juce::String fileExplorer()
{
#if JUCE_WINDOWS
    return "Explorer";
#elif JUCE_MAC
    return "Finder";
#else
    return "file manager";
#endif
}

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
        { "Low latency mode", onOff (lowLatency) + (lowLatency ? juce::String (L" \x00b7 plugins over the limit bypassed") : juce::String()), {} },
        { "Low latency limit", s[ids::lowLatencyMax].toString() + " samples (" + juce::String ((double) s[ids::lowLatencyMax] * 1000.0 / sampleRate(), 1) + " ms)",
          "lowlat " },
        { "Track height", juce::String (height) + (height == 1 ? " row" : " rows"), "height " },
        { "Bounce format", s[ids::bounceFormat].toString(), {} },
        { "Bounce folder", s[ids::bounceFolder].toString() + L"  (b wav <folder> changes it)", {}, false },
        { "Show bounces in " + fileExplorer(), onOff (s[ids::bounceReveal]) + L"  (bounces opens it any time)", {} },
        { "Video", videoFile() == juce::File() ? juce::String ("none  (video <file> adds one)")
                                               : videoFile().getFileName() + ((bool) s[ids::videoShown] ? "" : "  (hidden)"), "video " },
        { "Video start", videoFile() == juce::File() ? juce::String ("-") : videoStartText(), "video at " },
        { "Video offset", (int) s[ids::videoOffset] == 0 ? juce::String ("0 frames")
                                                         : juce::String ((int) s[ids::videoOffset]) + ((int) s[ids::videoOffset] > 0 ? " frames (picture late)" : " frames (picture early)"),
          "video offset " },
        { "Sample rate", juce::String (sampleRate() / 1000.0, 1) + L" kHz  (set when the session was made)", {}, false },
        { "Theme (all sessions)", theme, {} },
        { "New session rate", juce::String (newRate / 1000.0, newRate == 44100.0 || newRate == 88200.0 ? 1 : 0) + L" kHz  (all sessions)", {} },
        { "Dated new sessions", dateSessionNames ? "on  (" + datedName ("name") + ", all sessions)" : juce::String ("off"), {} },
        { "Keycast (all sessions)", keycastShown && ! keycastClosed ? juce::String ("shown  (keycast font|color|bg|size|fade)") : juce::String ("hidden"), "keycast " },
        { "Agents (all sessions)", agentsOn ? juce::String ("on  (taw --mcp can edit the session open here)") : juce::String ("off"), {} },
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
    if (name == "Low latency mode") return execute ("lowlat");
    if (name == "Low latency limit") return execute ("lowlat " + juce::String (juce::jlimit (0, 8192, (int) s[ids::lowLatencyMax] + direction * 64)));
    if (name == "Track height")   return execute ("height " + juce::String (juce::jlimit (1, 4, session->trackHeight() + direction)));
    if (name.startsWith ("Theme")) return execute (cycle ({ "dark", "light", "grey" }, theme));

    if (name == "New session rate")
    {
        const juce::StringArray rates { "44100", "48000", "88200", "96000" };
        newRate = cycle (rates, juce::String ((int) newRate)).getDoubleValue();
        saveSettings();
        return Result::good ("new sessions are made at " + juce::String (newRate / 1000.0, 1) + " kHz");
    }

    if (name == "Dated new sessions")
    {
        dateSessionNames = ! dateSessionNames;
        saveSettings();
        return Result::good (dateSessionNames ? "new sessions are named " + datedName ("name") : juce::String ("new sessions keep the name you type"));
    }

    if (name.startsWith ("Keycast")) return execute ("keycast");
    if (name.startsWith ("Agents")) return agentsCommand ({});
    if (name.startsWith ("Show bounces")) return execute ((bool) s[ids::bounceReveal] ? "bounces off" : "bounces on");

    // The video: left/right shows or hides it, moves its start a frame, or its offset a frame.
    if (name == "Video") return execute ("video");
    if (name == "Video offset") return execute ("video offset " + juce::String (juce::jlimit (-100, 100, (int) s[ids::videoOffset] + direction)));

    if (name == "Video start")
    {
        if (video == nullptr)
            return Result::fail (L"no video \x00b7 video <file> adds one");

        beginEdit ("video start");
        const auto frame = sampleRate() / juce::jmax (1.0, video->info().fps);
        const auto frames = std::round ((double) (juce::int64) s[ids::videoStart] / frame) + direction; // onto a frame, then one more
        session->tree().setProperty (ids::videoStart, (juce::int64) std::llround (frames * frame), um());
        return Result::good ("the video starts at " + videoStartText());
    }

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
    const auto label = "track " + trackLabel (index);

    if (Session::isBusTrack (track))
        return Result::fail (label + " is a bus");

    if (Session::isInstrumentTrack (track))
        return Result::fail (label + L" is an instrument track \x2014 inst swaps its plugin");

    if (! toSample)
    {
        if (! Session::isSampleTrack (track))
            return Result::fail (label + " is already an audio track");

        track.setProperty (ids::kind, "audio", um());
        return Result::good (label + L" is audio again \x00b7 its hits are kept (" + trackLabel (index) + " sample switches back)");
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

    if (Session::hitClips (track).empty())
        for (const auto& c : Session::clips (track))
        {
            addHit (index, (double) (juce::int64) c[ids::start] / session->samplesPerBeat(), 1.0f);
            ++added;
        }

    track.setProperty (ids::kind, "sample", um());
    return Result::good (label + " is a sample track" + (added > 0 ? " (" + juce::String (added) + " hits from its clips)" : juce::String())
                         + L" \x00b7 " + trackLabel (index) + " audio switches back");
}

// "x...x...x...x..." over the selection (repeating), or from the cursor for a bar.
// X = accent, x = normal, o = ghost, . or - = rest; spaces and | are ignored.
Result App::writePattern (const std::vector<int>& tracks, const juce::String& stepsIn, double stepBeats, double swing)
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
            return Result::fail ("track " + trackLabel (t) + L" isn't a sample track \x2014 " + trackLabel (t) + " sample converts it");

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
        // The pattern is one clip over its range, replacing whatever was there.
        auto track = session->track (t);
        auto clip = Session::makeHitClip (from, 0.0, to - from);

        for (int k = 0; from + k * stepBeats < to - halfSample; ++k)
            if (const auto vel = steps[k % steps.size()]; vel > 0.0f)
            {
                // Swing: the second of each pair of steps moves later, toward the next pair.
                const auto late = (k % 2 == 1) ? stepBeats * (2.0 * swing / 100.0 - 1.0) : 0.0;
                clip.appendChild (Session::makeHit (k * stepBeats + late, vel), nullptr);
                ++written;
            }

        clearRange (t, { std::llround (from * spb), std::llround (to * spb) });
        track.appendChild (clip, um());
    }

    return Result::good (juce::String (written) + " hits" + (swing > 50.0 ? " (swing " + juce::String (swing, 0) + ")" : juce::String()) + " on " + juce::String (tracks.size() == 1 ? trackList (tracks) : juce::String ((int) tracks.size()) + " tracks")
                         + L" \x00b7 " + formatBars (std::llround (from * spb)) + L" \x2192 " + formatBars (std::llround (to * spb)));
}

Result App::toggleHitAtCursor()
{
    if (session == nullptr || mixFocused() || onTopLane() || ! Session::isSampleTrack (session->track (focused)))
        return Result::fail (L"alt+/ places hits on sample tracks \x2014 focus one with \x2191\x2193");

    const auto track = session->track (focused);
    const bool rolling = engine.isRolling();
    const auto at = rolling ? heardPlayhead() : cursor;
    auto beat = (double) at / session->samplesPerBeat();

    if (rolling)
    {
        // Played in: it lands where you heard the music, like the pad key. Only input quantize
        // snaps it, never the grid, and it never removes a hit already there.
        if ((bool) session->tree()[ids::inputQuantize])
        {
            const auto step = session->fineStepBeats();
            beat = std::round (beat / step) * step;
        }
    }
    else
    {
        // Stopped: step entry at the cursor, toggling the hit on the grid step.
        const auto step = hitStepBeats();

        for (const auto& h : Session::placedHits (track))
            if (std::abs (h.beat - beat) < step * 0.5)
            {
                beginEdit ("remove hit");
                auto clip = h.clip;
                clip.removeChild (h.node, um());
                return Result::good ("removed hit at " + formatBars (at));
            }

        if (session->gridOn())
            beat = std::round (beat / step) * step;
    }

    // You hear it as you place it, like a note played in. Snapped ahead of the playhead while
    // rolling, the timeline plays it on time instead.
    if (! rolling || beat * session->samplesPerBeat() <= (double) engine.playhead())
        engine.triggerPad (focused, 0.8f);

    beginEdit ("add hit");
    addHit (focused, beat, 0.8f);
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

// The session screen's command line: the commands that work with no session open. In a session
// these same verbs go through execute, below.
Result App::executeAppLevel (const juce::String& line)
{
    const auto trimmed = line.trim();
    const auto needsSession = Result::fail (trimmed + L" needs a session open \x00b7 here: help, index, library, scan, agents, hub, keycast, dark|light|grey, dev, q");
    Parsed p;

    // A track number ("3 v -6") fails to parse with no tracks: that's a session's command too.
    if (parse (trimmed, numbering(), p).isNotEmpty() || p.hasSelector || p.mix)
        return needsSession;

    const auto& v = p.verb;

    if (v == "help" || v == "?")   { helpVisible = true; return Result::good(); } // the screen's own page (Ui::drawHelp)
    if (v == "index")              return startIndexing (splitPaths (p.rest));
    if (v == "library")            return describeLibrary();
    if (v == "scan")               return scanPlugins (p.args[0].equalsIgnoreCase ("retry"));
    if (v == "agents")             return agentsCommand (p.args[0]);
    if (v == "hub")                return hubCommand (p.rest.trim());
    if (v == "keycast")            return keycastCommand (p.rest.trim());
    if (v == "q" || v == "quit" || v == "exit") { quit(); return Result::good(); }

    if (v == "light" || v == "dark" || v == "grey" || v == "gray")
    {
        theme = v == "gray" ? juce::String ("grey") : v;
        saveSettings();
        return Result::good (theme + " theme");
    }

    // As alt+d: the device the bounces play through, and back here after.
    if (v == "dev")
    {
        stopBounce();
        openDevicePicker();
        devicePickerFromSessions = true;
        return Result::good();
    }

    return needsSession;
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

    if (auto err = parse (trimmed, numbering(), p); err.isNotEmpty())
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

    // mix fx limiter / mix fx b (its window) / mix fx b x|off|on / mix fx b>d: the mix's inserts.
    if (onMix && (v == "fx" || v == "ins"))
        return editInserts ({ -1 }, p.args);

    // mix lane / mix lane ceiling / mix auto: the mix's automation, under the Mix row.
    if (p.mix && (v == "lane" || v == "auto"))
    {
        if (! mixFocused())
            focusTrack (session->numTracks());

        return v == "auto" ? openAutoPanel() : laneByName (-1, p.rest);
    }

    if (p.mix)
        return Result::fail ("the mix takes v, fx, lane and auto, e.g. mix v -3, mix fx limiter, mix lane vol");

    // Global verbs ------------------------------------------------------------
    // rec records the armed tracks (ctrl+space does the same); "1 rec" arms exactly track 1
    // first. Arming itself is "r", below.
    if (v == "rec")                return record (p.hasSelector ? p.tracks : selectedTracks());
    if (v == "play")               return play();
    if (v == "stop")               return stop();
    if (v == "mark")               return addMarker (engine.isRolling() ? heardPlayhead() : cursor, p.rest.trim());
    if (v == "click" || v == "c")  return setClick (p.args[0]);
    if (v == "count")              return setCountIn (p.args[0]);
    if (v == "lowlat")             return setLowLatency (p.args[0]);
    if (v == "l" || v == "loop")   return setLoop (p.args[0]);
    if (v == "b" || v == "bounce") return startBounce (p.args);
    if (v == "bv")                 return startBounce (p.args, true);

    // Automation: "2 lane cutoff" puts a lane for a parameter under track 2 (and goes to it);
    // "pt -6" puts a point at the cursor (or the selection at that level) on the lane you're on.
    // Playlists: "pl" lists the focused track's; "pl new", "pl copy"; "pl 2" makes one the main;
    // "pl del" deletes the one you're on. "3 pl ..." does it on track 3.
    if (v == "pl")
    {
        if (p.hasSelector && ! p.tracks.empty() && p.tracks.front() != focused)
            focusTrack (p.tracks.front());

        return playlistCommand (p.args);
    }

    if (v == "lane")
    {
        if (p.hasSelector && ! p.tracks.empty())
            return laneByName (p.tracks.front(), p.rest);

        return onTopLane() || mixFocused() ? laneByName (-1, p.rest) : laneByName (focused, p.rest);
    }

    // Controllers: "map" maps the next knob moved to the parameter the track shows ("map 74"
    // names it, and moves it off any other lane); "unmap" clears them all ("unmap 74" one, "unmap lane"
    // the lane you're on); "aw" (shift+W) records mapped controllers while playing
    // (touch). Not "write": w is a hotkey on an empty line.
    if (v == "auto")
    {
        if (p.hasSelector && ! p.tracks.empty())
            focusTrack (p.tracks.front());

        return openAutoPanel();
    }

    if (v == "map")    return learnControl (p.rest.trim());
    if (v == "unmap")  return unmapControl (p.rest.trim());
    if (v == "aw")     return setWrite (p.rest.trim());

    if (v == "pt")
    {
        if (p.hasSelector && ! p.tracks.empty())
            focusTrack (p.tracks.front());

        return setAutomation (p.rest);
    }
    if (v == "bounces")            return revealBounces (p.args[0].toLowerCase());
    if (v == "video")              return videoCommand (p.args, p.rest.trim());
    if (v == "keycast")            return keycastCommand (p.rest.trim());
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

    // Instruments: inst <name> swaps the plugin on the focused (or selected) instrument tracks,
    // else makes a new instrument track; 3 inst <name> puts it on track 3, add inst <name> always
    // makes a new track. inst alone for the picker, ed for the plugin's window, scan to find
    // plugins, midi for inputs.
    if (v == "inst")
    {
        std::vector<int> onto;

        if (p.hasSelector)
            onto = p.tracks;
        else if (const auto here = onTopLane() ? std::vector<int>() : defaultTargets(); ! here.empty() && std::all_of (here.begin(), here.end(), [this] (int t) { return Session::isInstrumentTrack (session->track (t)); }))
            onto = here;

        return loadInstrument (onto, p.rest);
    }

    // add 3 inst [plugin] / add 4 sample: several new tracks of a kind at once (add 3 audio is
    // with the plain add, below). With no plugin or sample they're empty, ready for one.
    if ((v == "add" || v == "new") && p.args[0].isNotEmpty() && p.args[0].containsOnly ("0123456789")
        && (p.args[1].equalsIgnoreCase ("inst") || p.args[1].equalsIgnoreCase ("sample")))
    {
        const int count = juce::jlimit (1, 64, p.args[0].getIntValue());
        const bool inst = p.args[1].equalsIgnoreCase ("inst");
        const auto what = p.rest.fromFirstOccurrenceOf (" ", false, false).trim().fromFirstOccurrenceOf (" ", false, false).trim();

        if (inst && what.isNotEmpty())
        {
            if (! plugins.hasScanned())
                return Result::fail (L"no plugins scanned yet \x2014 type scan (takes a moment), then add " + juce::String (count) + " inst " + what);

            const auto desc = plugins.findInstrument (what);

            if (! desc)
                return Result::fail ("no instrument matches " + what.quoted() + L" \x00b7 inst alone lists them \x00b7 scan finds new ones");

            const int first = session->numTracks();
            Result last;

            for (int i = 0; i < count; ++i)
                if (last = loadInstrument (-1, *desc); ! last.ok)
                    return last;

            if (count == 1)
                return last;

            // Numbered, and only the first armed: the controller plays one of them, not all at once.
            for (int i = 0; i < count; ++i)
                session->track (first + i).setProperty (ids::name, desc->name + " " + juce::String (i + 1), um())
                                         .setProperty (ids::armed, i == 0, um());

            focusTrack (first);
            return Result::good ("added " + juce::String (count) + " tracks playing " + desc->name + ": "
                                 + trackLabel (first) + "-" + trackLabel (first + count - 1) + L" \x00b7 " + trackLabel (first)
                                 + L" is armed for your controller (shift+R arms another)");
        }

        if (! inst && what.isNotEmpty())
        {
            if (count > 1)
                return Result::fail ("add " + juce::String (count) + L" sample makes empty sample tracks \x00b7 sample <files> makes one from each file");

            return startImport (splitPaths (what), -1, ImportAs::sample);
        }

        if (inst && count == 1)
            return loadInstrument ({}, {}); // the picker, for a new track

        juce::StringArray added;
        const int first = session->numTracks();

        for (int i = 0; i < count; ++i)
        {
            const auto n = (int) numbering().byNumber.size() + 1; // as numbered (a kit's pads aren't)
            const auto name = (inst ? "Inst " : "Sample ") + juce::String (n);
            session->addTrack (name, 0, um()).setProperty (ids::kind, inst ? "instrument" : "sample", um());
            added.add (juce::String (n));
        }

        focusTrack (first);
        const auto range = trackLabel (first) + (count > 1 ? "-" + trackLabel (first + count - 1) : juce::String());
        return Result::good ("added " + juce::String (count) + (inst ? " instrument tracks " : " sample tracks ") + range
                             + (inst ? L" \x00b7 " + range + " inst <name> loads a plugin on them (one at a time: 5 inst <name>)"
                                     : L" \x00b7 " + trackLabel (first) + L" sample <file> gives one its sample \x00b7 pat x... writes hits"));
    }

    if ((v == "add" || v == "new") && p.args[0].equalsIgnoreCase ("inst"))
        return loadInstrument ({}, p.rest.fromFirstOccurrenceOf (" ", false, false).trim());
    if (v == "scan")               return scanPlugins (p.args[0].equalsIgnoreCase ("retry"));
    if (v == "midi")               return toggleMidiPanel();
    if (v == "stats")              return toggleStats();
    if (v == "phone")              return phoneCommand (p.args[0]);
    if (v == "agents")             return agentsCommand (p.args[0]); // taw --mcp's way in (docs/plan-taw-mcp.md)
    if (v == "hub")                return hubCommand (p.rest.trim());

    // browse: show or hide the sample browser (ctrl+B goes into it); index <folder> adds a folder of samples to it.
    // browse vol -6: the audition's own level.
    if (v == "browse" && (p.args[0].equalsIgnoreCase ("vol") || p.args[0].equalsIgnoreCase ("v")))
        return p.args[1].isEmpty() ? Result::fail ("browse vol -6 sets the audition level") : setAuditionDb (p.args[1].getDoubleValue());
    if (v == "browse")             return showBrowser (p.args[0].isEmpty() ? ! browserOpen() : ! p.args[0].equalsIgnoreCase ("hide"));
    if (v == "index")              return startIndexing (splitPaths (p.rest));
    if (v == "library")            return describeLibrary();

    // edit (shift+E): the focused track's clip at the cursor (or the nearest) in its editor;
    // "9 edit" on track 9; "edit new" an empty MIDI clip to draw into.
    if (v == "edit")
    {
        if (p.hasSelector && ! p.tracks.empty())
            focusTrack (p.tracks.front());

        return p.args[0].equalsIgnoreCase ("new") ? newMidiClipForEditor() : toggleEditor();
    }

    if (v == "ed")
    {
        // ed close (ctrl+\): every plugin window, wherever you are.
        if (p.args[0].equalsIgnoreCase ("close") || p.args[0].equalsIgnoreCase ("x"))
        {
            int open = 0;

            for (const auto& in : hosted)
                open += in.slot != nullptr && in.slot->editorShowing() ? 1 : 0;

            closePluginWindows();
            return open == 0 ? Result::good ("no plugin windows open")
                             : Result::good ("closed " + juce::String (open) + (open == 1 ? " plugin window" : " plugin windows"));
        }

        // In the routing column on an insert: that plugin's window.
        if (! p.hasSelector && ioCell >= ioFirstInsert && ioCell < ioFirstSend && ! onTopLane())
            return toggleInsertEditor (ioTrack(), ioCell);

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

    // Kits: kit [name] makes one (on a kit: says what's on its pads; kit fold folds it); 3 kit
    // drums puts sample track 3 in it (3 kit off takes it out); note 38 and choke 1 on a member;
    // pad 3 <file> loads a sample onto pad 3 of the kit here (pad 3 alone: the browser's pad).
    if ((v == "add" || v == "new") && p.args[0].equalsIgnoreCase ("kit"))
        return addKit (p.rest.fromFirstOccurrenceOf (" ", false, false));

    if (v == "kit")
    {
        if (p.args[0].equalsIgnoreCase ("fold"))
            return toggleFold (p.hasSelector && ! p.tracks.empty() ? p.tracks.front() : kitHere());

        if (p.hasSelector && p.tracks.size() == 1 && Session::isKit (session->track (p.tracks.front())))
            return describeKit (p.tracks.front());

        if (p.hasSelector)
            return joinKit (p.tracks, p.rest);

        if (p.rest.isEmpty() && kitHere() >= 0)
            return describeKit (kitHere());

        return addKit (p.rest);
    }

    if (v == "note")  return setPadNote (p.hasSelector ? p.tracks : defaultTargets(), p.args[0]);
    if (v == "choke") return setChoke (p.hasSelector ? p.tracks : defaultTargets(), p.args[0]);

    if (v == "pad")
    {
        int kit = kitHere();

        if (p.hasSelector && ! p.tracks.empty())
            kit = Session::isKit (session->track (p.tracks.front())) ? p.tracks.front() : kitOf (p.tracks.front());

        const int pad = p.args[0].getIntValue();

        if (kit < 0)
            return Result::fail (L"pad loads onto a kit \x2014 kit makes one, \x2191\x2193 to it");

        if (pad < 1 || pad > 16 || ! p.args[0].containsOnly ("0123456789"))
            return Result::fail (L"pad 3 <file> loads a sample onto pad 3 (1-16) \x00b7 pad 3 alone picks it for the browser");

        const auto path = p.rest.fromFirstOccurrenceOf (" ", false, false).trim();

        if (path.isEmpty())
        {
            browserPadNote = firstPadNote + pad - 1;
            const auto member = kitMemberOnNote (kit, browserPadNote);
            return Result::good ("pad " + juce::String (pad) + (member >= 0 ? " (" + session->track (member)[ids::name].toString() + ")" : juce::String (" (empty)"))
                                 + L" \x00b7 ctrl+B: what you audition goes on it");
        }

        return loadPad (kit, firstPadNote + pad - 1, splitPaths (path)[0]);
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
    if (v == "sel" && p.args[0].equalsIgnoreCase ("clip")) return selectClipAtCursor();
    if (v == "sel")   return selectRange (p.args[0], p.args[1]);
    if (v == "cons" || v == "consolidate") return consolidate (p.hasSelector ? p.tracks : defaultTargets());
    if (v == "sep" || v == "separate" || v == "split") return separateClips (p.hasSelector ? p.tracks : defaultTargets());

    // Fades (ctrl+f makes them from the selection): "clip fade in 10ms", "xfade 20ms",
    // "declick 2". Not "fade": f is a hotkey on an empty line.
    if (v == "clip")    { if (p.hasSelector && ! p.tracks.empty()) focusTrack (p.tracks.front()); return clipCommand (p.args); }
    if (v == "xfade")   { if (p.hasSelector && ! p.tracks.empty()) focusTrack (p.tracks.front()); return crossfadeCommand (p.args); }
    if (v == "declick") return setDeclick (p.args[0]);
    if (v == "copy")  return copySelection (p.hasSelector ? p.tracks : defaultTargets());
    if (v == "cut")   return cutSelection (p.hasSelector ? p.tracks : defaultTargets());
    if (v == "del")   return deleteSelection (p.hasSelector ? p.tracks : defaultTargets());
    if (v == "dup" && p.args[0].equalsIgnoreCase ("track"))
        return duplicateTracks (p.hasSelector ? p.tracks : defaultTargets(), ! p.args[1].equalsIgnoreCase ("empty"));
    if (v == "dup")   return duplicateSelection (p.hasSelector ? p.tracks : defaultTargets());
    if (v == "paste")
    {
        const auto targets = p.hasSelector ? p.tracks : defaultTargets();
        return paste (targets.empty() ? -1 : targets.front());
    }

    if (v == "save")
        return saveSession();

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

        // "add 3 audio": the kind, said for clarity (audio is what add makes), not a name.
        if (! args.isEmpty() && args[0].equalsIgnoreCase ("audio"))
            args.remove (0);

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

        // "stereo" anywhere: stereo tracks, each on a pair of inputs.
        const bool stereo = args.contains ("stereo", true);
        args.removeString ("stereo", true);
        const int width = stereo ? 2 : 1;

        const auto baseName = args.joinIntoString (" ").unquoted();
        juce::StringArray added;

        for (int i = 0; i < count; ++i)
        {
            std::vector<bool> used ((size_t) engine.numInputChannels() + 2, false);

            for (int t = 0; t < session->numTracks(); ++t)
                if (const int in = session->track (t)[ids::input]; in > 0)
                    for (int k = in; k <= in + (trackIsStereo (session->track (t)) ? 1 : 0) && k < (int) used.size(); ++k)
                        used[(size_t) k] = true;

            int input = 0;

            if (firstInput >= 0)
                input = firstInput == 0 ? 0 : firstInput + i * width;
            else
                for (int in = 1; in + width - 1 < (int) used.size() - 1 && input == 0; ++in)
                    if (! used[(size_t) in] && (width == 1 || ! used[(size_t) in + 1]))
                        input = in;

            const auto n = (int) numbering().byNumber.size() + 1; // tracks as numbered (a kit's pads aren't)
            const auto name = baseName.isEmpty() ? "Track " + juce::String (n)
                                                 : (count == 1 ? baseName : baseName + " " + juce::String (i + 1));
            auto track = session->addTrack (name, input, um());

            if (stereo)
                track.setProperty (ids::channels, 2, um());

            added.add (juce::String (n) + " " + name
                       + (input <= 0 ? juce::String (" (no input)")
                                     : stereo ? " (in" + juce::String (input) + "-" + juce::String (input + 1) + ")" : " (in" + juce::String (input) + ")"));
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
            if (Session::isBusTrack (session->track (t)) && ! Session::isKit (session->track (t)))
                return Result::fail ("track " + trackLabel (t) + L" is a bus \x2014 it plays what's sent to it, nothing to record");

        const auto r = toggle (ids::armed, "armed");

        if (! (bool) session->track (targets.front())[ids::armed])
            return r;

        if (Session::isKit (session->track (targets.front())))
            return Result::good (r.message + L" \x00b7 your pads play it (ctrl+k: the keyboard's 4x4) \x00b7 ctrl+space records what you play");

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

    // 2 fx valhalla / 2 fx b valhalla / 2 fx b (its window) / 2 fx b x|off|on / 2 fx b>d / 2 fx (picker)
    if (v == "fx")
        return editInserts (targets, p.args);

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
        double stepBeats = 0.25, swing = 50.0;

        // ~58 or swing58 anywhere: every second step lands late (50 straight, 66 a triplet feel).
        for (int a = args.size(); --a >= 0;)
            if (const auto t = args[a].toLowerCase(); t.startsWithChar ('~') || t.startsWith ("swing"))
            {
                swing = t.trimCharactersAtStart ("~swing").getDoubleValue();

                if (swing < 50.0 || swing > 75.0)
                    return Result::fail ("swing between 50 (straight) and 75, e.g. pat /16 ~58 x.x.x.x.");

                args.remove (a);
            }

        if (! args.isEmpty() && args[0].startsWithChar ('/'))
        {
            const auto digits = args[0].substring (1).initialSectionContainingOnly ("0123456789");
            const auto division = digits.getIntValue();

            if (division != 4 && division != 8 && division != 16 && division != 32)
                return Result::fail ("step size /4, /8, /16 or /32");

            stepBeats = 4.0 / division;
            args.set (0, args[0].substring (1 + digits.length())); // "/8x.x." keeps its steps
        }

        return writePattern (targets, args.joinIntoString (""), stepBeats, swing);
    }

    if (v == "x")
    {
        Result last;

        for (auto t : targets)
            last = discardTake (t);

        return last;
    }

    // 2 i 3: input 3 (a stereo track takes 3 and 4); 2 i 3-4: a pair, which makes it stereo.
    if (v == "i" || v == "in")
    {
        const auto arg = p.args[0];
        const bool pair = arg.containsChar ('-');
        const auto input = arg.upToFirstOccurrenceOf ("-", false, false).getIntValue();

        if (arg.isEmpty() || ! arg.containsOnly ("0123456789-") || (pair && arg.fromFirstOccurrenceOf ("-", false, false).getIntValue() != input + 1))
            return Result::fail ("which input? 1 i 2, or a pair for a stereo track: 1 i 3-4 (0 = none)");

        for (auto t : targets)
            if (Session::isBusTrack (session->track (t)))
                return Result::fail ("track " + trackLabel (t) + L" is a bus \x2014 no input; 2 out " + trackLabel (t) + " feeds it");

        for (auto t : targets)
        {
            session->track (t).setProperty (ids::input, input, um());

            if (pair && ! Session::isInstrumentTrack (session->track (t)))
                session->track (t).setProperty (ids::channels, 2, um());
        }

        const bool stereo = trackIsStereo (session->track (targets.front()));
        const auto last = input + (stereo ? 1 : 0);
        auto msg = trackList (targets) + (input == 0 ? L" \x00b7 no input"
                                          : stereo ? L" \x00b7 inputs " + juce::String (input) + "-" + juce::String (last)
                                                   : L" \x00b7 input " + juce::String (input));

        if (last > engine.numInputChannels())
            return Result::fail (msg + " (device only has " + juce::String (engine.numInputChannels()) + ")");

        return Result::good (msg);
    }

    // 2 pan l30 / 2 pan c / 2 pan 45r (alt+[ ] nudges it); 2 mono / 2 stereo: the track's width.
    if (v == "pan")
        return p.args.isEmpty() ? Result::good (trackList (targets) + " pan " + panText (session->track (targets.front())[ids::pan])
                                                + L" \x00b7 2 pan l30, 2 pan c, 2 pan 45r")
                                : setPan (targets, p.rest);
    if (v == "mono" || v == "stereo")
        return setWidth (targets, v == "stereo");

    // ins: an effect on the focused track, like inst: alone, the picker; with a name, the best match.
    if (v == "ins")
        return editInserts (targets, p.args);

    if (v == "v" || v == "vol")
    {
        if (p.args.isEmpty())
            return Result::fail ("volume in dB, e.g. 2 v -6");

        const auto db = juce::jlimit (-96.0, 12.0, p.args[0].getDoubleValue());

        for (auto t : targets)
            session->track (t).setProperty (ids::gainDb, db, um());

        return Result::good (trackList (targets) + L" \x00b7 " + formatDb (db) + " dB");
    }

    // 2 remove, 2-4 remove: the tracks go (ctrl+z brings them back).
    if (v == "remove" || v == "rm")
        return removeTracks (targets);

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
