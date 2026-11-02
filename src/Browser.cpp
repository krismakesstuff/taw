// The sample browser's state and keys; BrowserWindow.cpp draws it and has the mouse. ctrl+B
// opens the window with the keys (ctrl+B in it closes it; esc gives the keys back). Tabs pick a
// kind; the map shows that kind's samples where alike sit together (or on two measurements); the
// list the same samples one by one; typing filters both. Every move auditions, at the browser's
// own level, in time with the loop while playing. Enter puts the sample on the kit's chosen pad,
// or gives the focused sample track that sample (keeping its hits), or makes a new sample track.
// ? shows a guide to all of it over the map.

#include "App.h"

namespace taw
{
namespace
{
// The badges' order (the kinds Library sorts into); anything else comes after as "other".
const char* const kindOrder[] = { "kick", "snare", "clap", "rim", "hihat", "open hat", "cymbal", "tom", "perc" };

juce::String kindOf (const SampleInfo& s) { return s.kind().isEmpty() ? juce::String ("other") : s.kind(); }

juce::File libraryFile() { return configFolder().getChildFile ("library.json"); }

// The filter: words match file or folder names; "decay<200", "bright>1k", "pitch=60" match
// measurements (the --library names, or short ones: bright, att, dec, len, loud, low). Numbers
// take k (thousands) and s (seconds, for times). A term still being typed ("decay<") is skipped.
struct FilterTerm { juce::String word, measure; juce::juce_wchar op = 0; double value = 0.0; };

std::vector<FilterTerm> filterTerms (const juce::String& filter)
{
    static const std::pair<const char*, const char*> aliases[] = { { "bright", "brightness" }, { "att", "attack" }, { "dec", "decay" },
                                                                    { "len", "length" }, { "loud", "loudness" }, { "low", "lowend" },
                                                                    { "crest", "punch" }, { "flat", "noise" } };
    std::vector<FilterTerm> out;

    for (const auto& token : juce::StringArray::fromTokens (filter, " ", ""))
    {
        FilterTerm t;
        const int op = token.indexOfAnyOf ("<>=");

        if (op > 0)
        {
            auto key = token.substring (0, op).toLowerCase();

            for (const auto& [shortName, full] : aliases)
                if (key == shortName)
                    key = full;

            if (Library::measureNames().contains (key))
            {
                auto number = token.substring (op + 1).trim().toLowerCase();

                if (number.isEmpty() || (! juce::CharacterFunctions::isDigit (number[0]) && number[0] != '-' && number[0] != '.'))
                    continue; // still typing

                double scale = 1.0;
                const bool time = key == "attack" || key == "decay" || key == "tail" || key == "length";

                if (number.endsWith ("k") || number.endsWith ("khz"))
                    scale = 1000.0;
                else if (time && number.endsWith ("s") && ! number.endsWith ("ms"))
                    scale = 1000.0;

                t.measure = key;
                t.op = token[op];
                t.value = number.getDoubleValue() * scale;
                out.push_back (t);
                continue;
            }
        }

        t.word = token;
        out.push_back (t);
    }

    return out;
}

juce::String hz (double f) { return f >= 1000.0 ? juce::String (f / 1000.0, 1) + " kHz" : juce::String (juce::roundToInt (f)) + " Hz"; }
juce::String ms (double t) { return t >= 1000.0 ? juce::String (t / 1000.0, 2) + " s" : (t < 10.0 ? juce::String (t, 1) : juce::String (juce::roundToInt (t))) + " ms"; }

// What an axis can show: a measurement and how to write it. Index 0 is the sound-alike map.
struct Axis
{
    const char* name;
    double (*value) (const SampleInfo&);
    juce::String (*format) (double);
};

const Axis axisTable[] = {
    { "similar", nullptr, nullptr },
    { "brightness", [] (const SampleInfo& s) { return s.centroidHz; }, hz },
    { "attack", [] (const SampleInfo& s) { return s.attackMs; }, ms },
    { "decay", [] (const SampleInfo& s) { return s.decayMs; }, ms },
    { "tail", [] (const SampleInfo& s) { return s.tailMs; }, ms },
    { "length", [] (const SampleInfo& s) { return s.seconds * 1000.0; }, ms },
    { "pitch", [] (const SampleInfo& s) { return s.pitchConfidence >= 0.6 ? s.pitchHz : 0.0; },
      [] (double f) { return f <= 0.0 ? juce::String ("none") : hz (f) + " " + juce::MidiMessage::getMidiNoteName (juce::roundToInt (69.0 + 12.0 * std::log2 (f / 440.0)), true, true, 4); } },
    { "loudness", [] (const SampleInfo& s) { return s.rmsDb; }, [] (double db) { return formatDb (db) + " dB rms"; } },
    { "punch", [] (const SampleInfo& s) { return s.crestDb; }, [] (double db) { return juce::String (db, 1) + " dB crest"; } },
    { "noise", [] (const SampleInfo& s) { return s.flatness; }, [] (double f) { return juce::String (f, 2) + " flat"; } },
    { "low end", [] (const SampleInfo& s) { return 10.0 * std::log10 (std::pow (10.0, s.bandsDb[0] / 10.0) + std::pow (10.0, s.bandsDb[1] / 10.0)); },
      [] (double db) { return juce::String (db, 1) + " dB < 120 Hz"; } },
};
} // namespace

const juce::StringArray& App::browserAxisNames()
{
    static const juce::StringArray names = []
    {
        juce::StringArray n;

        for (const auto& a : axisTable)
            n.add (a.name);

        return n;
    }();
    return names;
}

// Steps one axis through the list. "similar" is both axes or neither: choosing it on one sets
// both, leaving it on one gives the other its own measurement.
void App::setBrowserAxis (bool y, int step)
{
    const int n = (int) std::size (axisTable);
    auto& axis = y ? browserAxisY : browserAxisX;
    auto& other = y ? browserAxisX : browserAxisY;

    if (axis == 0)
    {
        // Off the sound-alike map: brightness across, decay up, to start from.
        browserAxisX = 1;
        browserAxisY = 3;
    }
    else
    {
        axis = (axis + step + n) % n;

        if (axis == 0)
            other = 0;
        else if (axis == other)
            axis = (axis + step + n) % n == 0 ? 0 : (axis + step + n) % n; // never the same on both

        if (axis == 0)
            other = 0;
    }

    saveSettings();
    message = browserAxisX == 0 ? juce::String (L"map: close together = sounds alike")
                                : "map: " + juce::String (axisTable[browserAxisX].name) + L" \x2192  \x00b7  " + axisTable[browserAxisY].name + L" \x2191";
    messageIsError = false;
}

// Ranks (ties share one) within what's shown (the kind and the filter), so the map fills evenly
// whatever the spread of the values: filtering down to a few spreads those few over the map.
const App::AxisCache& App::axes() const
{
    auto& c = axisCache;

    if (library == nullptr || browserAxisX == 0)
        return c;

    const auto& all = library->samples();

    if (c.library == library.get() && c.count == all.size() && c.kind == browserKind && c.x == browserAxisX && c.y == browserAxisY
        && c.filter == browserFilter && c.like == browserLike)
        return c;

    c.filter = browserFilter;
    c.like = browserLike;

    c.library = library.get();
    c.count = all.size();
    c.kind = browserKind;
    c.x = browserAxisX;
    c.y = browserAxisY;
    c.rankX.assign (all.size(), 0.5f);
    c.rankY.assign (all.size(), 0.5f);

    const auto members = browserShown();

    auto rank = [&] (const Axis& a, std::vector<float>& out, juce::String& low, juce::String& high)
    {
        std::vector<std::pair<double, int>> v;

        for (auto i : members)
            v.push_back ({ a.value (all[(size_t) i]), i });

        std::sort (v.begin(), v.end());

        for (size_t lo = 0; lo < v.size();)
        {
            auto hi = lo;

            while (hi + 1 < v.size() && juce::exactlyEqual (v[hi + 1].first, v[lo].first))
                ++hi;

            const float r = v.size() > 1 ? (float) (lo + hi) / 2.0f / (float) (v.size() - 1) : 0.5f;

            for (auto k = lo; k <= hi; ++k)
                out[(size_t) v[k].second] = r;

            lo = hi + 1;
        }

        low = v.empty() ? juce::String() : a.format (v.front().first);
        high = v.empty() ? juce::String() : a.format (v.back().first);
    };

    rank (axisTable[browserAxisX], c.rankX, c.lowX, c.highX);
    rank (axisTable[browserAxisY], c.rankY, c.lowY, c.highY);
    return c;
}

juce::String App::browserAxisRange (bool y) const
{
    if (browserAxisX == 0)
        return {};

    const auto [low, high] = browserAxisEnds (y);
    return low + L" \x2192 " + high;
}

std::pair<juce::String, juce::String> App::browserAxisEnds (bool y) const
{
    if (browserAxisX == 0)
        return {};

    const auto& c = axes();
    return y ? std::pair { c.lowY, c.highY } : std::pair { c.lowX, c.highX };
}

void App::loadLibrary()
{
    if (library == nullptr)
    {
        timing::Scope t ("sample library read");
        library = std::make_unique<Library> (libraryFile());
    }
}

// The window: ctrl+B opens it in front with the keys (and from inside it closes it); browse
// opens or closes it. It needs the screen, so --exec has no browser (agents use --library).
Result App::showBrowser (bool show)
{
    if (! show)
    {
        engine.preview (nullptr);

        if (browserWindow != nullptr)
            browserWindow->close();

        return Result::good (L"browser closed \x00b7 ctrl+B opens it");
    }

    return focusBrowser();
}

bool App::browserOpen() const { return browserWindow != nullptr && browserWindow->isOpen(); }

Result App::focusBrowser()
{
    if (! console.isOpen())
        return Result::fail (L"the browser is a window \x2014 headless, taw --library finds samples");

    loadLibrary();

    if (browserWindow == nullptr)
        browserWindow = std::make_unique<BrowserWindow> (*this);

    browserWindow->open (true);
    browserFocus = BrowserFocus::map; // the map (and typing to filter) first

    if (library->samples().empty())
        return isIndexing() ? Result::good (L"indexing\x2026")
#if JUCE_WINDOWS
                            : Result::good (L"no samples yet \x2014 index D:\\Samples adds a folder (it's only read)");
#else
                            : Result::good (L"no samples yet \x2014 index ~/Samples adds a folder (it's only read)");
#endif

    const auto shown = browserShown();

    if (std::find (shown.begin(), shown.end(), browserSample) == shown.end())
        browserSample = shown.empty() ? -1 : shown.front();

    if (const auto kit = kitHere(); kit >= 0)
        return Result::good ("browsing for " + session->track (kit)[ids::name].toString() + " " + padName (browserPadNote)
                             + L" \x00b7 what you audition goes on it \x00b7 click a pad to pick another \x00b7 esc: back to taw");

    return Result::good (juce::String ((int) library->samples().size()) + L" samples \x00b7 ? keys and mouse \x00b7 type to filter \x00b7 esc: back to taw");
}

std::vector<juce::String> App::browserKinds() const
{
    std::vector<juce::String> out { "all" };

    if (library == nullptr)
        return out;

    std::set<juce::String> present;

    for (const auto& s : library->samples())
        present.insert (kindOf (s));

    for (auto* k : kindOrder)
        if (present.erase (k) > 0)
            out.push_back (k);

    for (const auto& k : present)
        out.push_back (k);

    return out;
}

std::vector<int> App::browserShown() const
{
    std::vector<int> out;

    if (library == nullptr)
        return out;

    const auto& all = library->samples();

    // Like this: the sample itself, then the ones nearest it, nearest first.
    if (browserLike >= 0 && browserLike < (int) all.size())
    {
        out.push_back (browserLike);

        for (auto i : library->nearest (browserLike, 40))
            out.push_back (i);

        return out;
    }

    const auto kinds = browserKinds();
    const auto kind = kinds[(size_t) juce::jlimit (0, (int) kinds.size() - 1, browserKind)];
    const auto terms = filterTerms (browserFilter);

    for (int i = 0; i < (int) all.size(); ++i)
    {
        const auto& s = all[(size_t) i];
        const juce::File f (s.path);
        const auto name = f.getParentDirectory().getFileName() + "/" + f.getFileName();

        if ((kind == "all" || kindOf (s) == kind)
            && std::all_of (terms.begin(), terms.end(), [&] (const FilterTerm& t)
               {
                   if (t.measure.isEmpty())
                       return name.containsIgnoreCase (t.word);

                   const auto m = Library::measure (s, t.measure);
                   return t.op == '<' ? m < t.value : t.op == '>' ? m > t.value : std::abs (m - t.value) <= std::abs (t.value) * 0.05;
               }))
            out.push_back (i);
    }

    // Kind by kind (in the tabs' order), then by name.
    auto rank = [] (const juce::String& k)
    {
        for (int r = 0; r < (int) std::size (kindOrder); ++r)
            if (k == kindOrder[r])
                return r;

        return (int) std::size (kindOrder);
    };

    std::sort (out.begin(), out.end(), [&] (int a, int b)
    {
        const auto& sa = all[(size_t) a];
        const auto& sb = all[(size_t) b];
        const int ra = rank (kindOf (sa)), rb = rank (kindOf (sb));

        if (ra != rb)
            return ra < rb;

        return juce::File (sa.path).getFileName().compareNatural (juce::File (sb.path).getFileName()) < 0;
    });

    return out;
}

std::pair<float, float> App::browserPosition (int sample) const
{
    const auto& s = library->samples()[(size_t) sample];

    if (browserAxisX != 0)
    {
        const auto& c = axes();
        return { c.rankX[(size_t) sample], 1.0f - c.rankY[(size_t) sample] }; // up is more
    }

    return browserKind == 0 || browserLike >= 0 ? std::pair { s.mapX, s.mapY } : std::pair { s.kindX, s.kindY };
}

Result App::setAuditionDb (double db)
{
    auditionDb = juce::jlimit (-60.0, 6.0, std::round (db * 2.0) / 2.0);
    saveSettings();
    return Result::good ("audition " + formatDb (auditionDb) + " dB");
}

// Selects a sample: plays it (audition), and on a kit with "audition -> pad" on, it goes onto the
// chosen pad at once (toPad), so what you hear is what the pad plays.
void App::browserPick (int sample, bool audition, bool toPad)
{
    browserSample = sample;

    if (library == nullptr || sample < 0 || sample >= (int) library->samples().size())
        return;

    const auto& s = library->samples()[(size_t) sample];
    message = juce::File (s.path).getFileName() + L" \x00b7 " + kindOf (s) + (s.nameKind.isEmpty() ? " (by its sound)" : "");
    messageIsError = false;

    if (audition)
        browserAudition (sample);

    if (toPad && browserToPad && session != nullptr && ! recording)
        if (const auto kit = kitHere(); kit >= 0)
        {
            // Already on it: nothing to load.
            const auto member = kitMemberOnNote (kit, browserPadNote);

            if (member < 0 || librarySampleFor (session->track (member)) != sample)
                say (loadPad (kit, browserPadNote, s.path));
        }
}

// Plays it from the top, resampled to the device's rate. While taw plays (and "in time" is on)
// it waits for the next grid step of what you're hearing, so you hear it in the groove.
void App::browserAudition (int sample)
{
    const auto& s = library->samples()[(size_t) sample];
    const juce::File file (s.path);
    std::unique_ptr<juce::AudioFormatReader> reader (engine.formats().createReaderFor (file));

    if (reader == nullptr)
    {
        say (Result::fail ("can't read " + file.getFileName()));
        return;
    }

    const double fileRate = reader->sampleRate, rate = engine.deviceSampleRate() > 0.0 ? engine.deviceSampleRate() : fileRate;
    const int n = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) (10.0 * fileRate));
    juce::AudioBuffer<float> in (2, n);
    reader->read (&in, 0, n, 0, true, true);

    if (reader->numChannels == 1)
        in.copyFrom (1, 0, in, 0, 0, n);

    auto out = std::make_shared<juce::AudioBuffer<float>> (2, (int) std::ceil (n * rate / fileRate) + 1);
    out->clear();

    if (std::abs (rate - fileRate) < 0.5)
    {
        out->copyFrom (0, 0, in, 0, 0, n);
        out->copyFrom (1, 0, in, 1, 0, n);
    }
    else
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            juce::LagrangeInterpolator resampler;
            resampler.process (fileRate / rate, in.getReadPointer (ch), out->getWritePointer (ch), out->getNumSamples() - 1, n, 0);
        }
    }

    juce::int64 at = -1;

    if (browserInTime && session != nullptr && engine.isRolling() && engine.countInRemaining() == 0)
    {
        // The playhead is ahead of what you hear by the mix's delay compensation: the next step
        // of what you hear, played that much later on the playhead's clock. Past the loop's end
        // it's the loop's start.
        const auto step = session->fineStepBeats() * session->samplesPerBeat();
        const auto heard = (double) (engine.playhead() - mixLatency);
        auto next = std::ceil (heard / step - 1.0e-6) * step;

        if (loopActive() && next >= (double) (juce::int64) session->tree()[ids::loopEnd])
            next = (double) (juce::int64) session->tree()[ids::loopStart];

        at = (juce::int64) std::llround (next) + mixLatency;
    }

    engine.preview (std::move (out), juce::Decibels::decibelsToGain ((float) auditionDb), 0, at);
}

// Enter: on a kit, onto its chosen pad; on a sample track, its new sample (it keeps its hits);
// else (or shift+enter) a new sample track.
Result App::browserLoad (bool newTrack)
{
    if (session == nullptr)
        return Result::fail ("open a session first, then enter puts the sample on a track");

    if (library == nullptr || browserSample < 0 || browserSample >= (int) library->samples().size())
        return Result::fail ("pick a sample first");

    const auto& path = library->samples()[(size_t) browserSample].path;

    if (! newTrack)
        if (const auto kit = kitHere(); kit >= 0)
            return loadPad (kit, browserPadNote, path);

    const int target = ! newTrack && ! onTopLane() && ! mixFocused() && focused >= 0 && focused < session->numTracks()
                               && Session::isSampleTrack (session->track (focused))
                           ? focused : -1;
    return startImport ({ path }, target, ImportAs::sample);
}

int App::librarySampleFor (const juce::ValueTree& track) const
{
    if (library == nullptr || ! track.isValid())
        return -1;

    const auto from = track[ids::sampleFrom].toString();
    const auto& all = library->samples();

    if (from.isNotEmpty())
        for (int i = 0; i < (int) all.size(); ++i)
            if (all[(size_t) i].path == from)
                return i;

    return -1;
}

// Like this: the samples that sound most like the chosen pad's sound (or, off a kit or on an
// empty pad, the chosen sample). Again: everything.
Result App::browserLikeThis()
{
    loadLibrary();

    if (browserLike >= 0)
    {
        browserLike = -1;
        return Result::good ("all samples again");
    }

    int from = browserSample;

    if (const auto kit = kitHere(); kit >= 0)
        if (const auto member = kitMemberOnNote (kit, browserPadNote); member >= 0)
            if (const auto i = librarySampleFor (session->track (member)); i >= 0)
                from = i;

    if (from < 0 || from >= (int) library->samples().size())
        return Result::fail ("pick a sample first, then like this shows the ones that sound like it");

    browserLike = from;
    browserSample = from;
    return Result::good (L"like " + juce::File (library->samples()[(size_t) from].path).getFileNameWithoutExtension()
                         + L", nearest first \x00b7 like this again: all");
}

void App::browserPlayPad (int note)
{
    browserPadNote = note;
    const auto kit = kitHere();

    if (kit < 0)
        return;

    if (const auto member = kitMemberOnNote (kit, note); member >= 0)
    {
        engine.triggerPad (member, 0.8f);
        message = padName (note) + " " + session->track (member)[ids::name].toString() + L" \x00b7 what you audition now goes on it";
    }
    else
        message = padName (note) + L" (empty) \x00b7 what you audition goes on it";

    messageIsError = false;
}

void App::setBrowserAxisTo (bool y, int axis)
{
    const int n = (int) std::size (axisTable);
    axis = juce::jlimit (0, n - 1, axis);

    if (axis == 0)
        browserAxisX = browserAxisY = 0;
    else if (y)
    {
        browserAxisY = axis;

        if (browserAxisX == 0 || browserAxisX == axis)
            browserAxisX = axis == 1 ? 3 : 1;
    }
    else
    {
        browserAxisX = axis;

        if (browserAxisY == 0 || browserAxisY == axis)
            browserAxisY = axis == 3 ? 1 : 3;
    }

    saveSettings();
}

// The window's keys. Tab goes round the tabs, the map, the list and (on a kit) the pads; arrows
// move inside each; typing filters; enter loads; esc clears the filter, then hands the keys back
// to taw. Ctrl and alt+letter keys, and shift+space, are taw's (false).
bool App::handleBrowserKey (const Key& k)
{
    if ((k.code == Key::Char && (k.ctrl || k.alt)) || (k.code == Key::Char && k.ch == ' ' && k.shift))
        return false;

    loadLibrary();
    const auto kinds = browserKinds();
    auto shown = browserShown();
    const int at = (int) (std::find (shown.begin(), shown.end(), browserSample) - shown.begin());
    const bool onKit = kitHere() >= 0;

    auto keepSelection = [&]
    {
        shown = browserShown();

        if (std::find (shown.begin(), shown.end(), browserSample) == shown.end())
            browserSample = shown.empty() ? -1 : shown.front();

        message = juce::String ((int) shown.size()) + " shown";
        messageIsError = false;
    };

    // The nearest sample on the map in an arrow's direction (within 60 degrees of it).
    // ctrl: a fifth of the map that way, to the sample nearest that spot (still that way).
    auto moveOnMap = [&] (float ux, float uy, bool far = false)
    {
        if (browserSample < 0)
        {
            if (! shown.empty())
                browserPick (shown.front());
            return true;
        }

        const auto [x0, y0] = browserPosition (browserSample);
        const float sx = 800.0f, sy = 500.0f; // about the window's map, in pixels
        int best = -1;
        float bestScore = 1.0e9f;

        for (auto i : shown)
        {
            if (i == browserSample)
                continue;

            const auto [x, y] = browserPosition (i);
            const float dx = (x - x0) * sx, dy = (y - y0) * sy;
            const float along = dx * ux + dy * uy, across = std::abs (dx * uy - dy * ux);

            if (along <= 0.0f || across > along * 1.7f)
                continue;

            const float jump = far ? 0.2f * (std::abs (ux) > 0.0f ? sx : sy) : 0.0f;
            const float score = far ? std::sqrt ((along - jump) * (along - jump) + across * across) + across
                                    : std::sqrt (along * along + across * across) + across;

            if (score < bestScore)
                bestScore = score, best = i;
        }

        if (best >= 0)
            browserPick (best);

        return best >= 0;
    };

    // ?: the guide to the keys and mouse, and again (or esc) to put it away. Any other key also
    // puts it away and does what it does.
    if (k.code == Key::Char && k.ch == '?')
    {
        browserGuide = ! browserGuide;
        return true;
    }

    if (browserGuide)
    {
        browserGuide = false;

        if (k.code == Key::Escape)
            return true;
    }

    // alt+arrows: the axes, from anywhere in the browser (left/right x, up/down y).
    if (k.alt && ! k.ctrl && (k.code == Key::Left || k.code == Key::Right || k.code == Key::Up || k.code == Key::Down))
    {
        const bool y = k.code == Key::Up || k.code == Key::Down;
        setBrowserAxis (y && browserAxisX != 0, k.code == Key::Right || k.code == Key::Up ? 1 : -1);
        return true;
    }

    // The pads: arrows move round the 4x4 (pad 1 bottom left), space plays the lit one.
    if (browserFocus == BrowserFocus::pads && onKit)
    {
        const int pad = juce::jlimit (0, 15, browserPadNote - firstPadNote);
        int col = pad % 4, row = pad / 4;

        switch (k.code)
        {
            case Key::Left:  col = std::max (0, col - 1); break;
            case Key::Right: col = std::min (3, col + 1); break;
            case Key::Up:    row = std::min (3, row + 1); break;
            case Key::Down:  row = std::max (0, row - 1); break;
            case Key::Char:
                if (k.ch == ' ')
                {
                    browserPlayPad (browserPadNote);
                    return true;
                }
                break;
            default: break;
        }

        if (k.code == Key::Left || k.code == Key::Right || k.code == Key::Up || k.code == Key::Down)
        {
            browserPlayPad (firstPadNote + row * 4 + col);
            return true;
        }
    }

    switch (k.code)
    {
        case Key::Escape:
            if (browserFilter.isNotEmpty())
            {
                browserFilter.clear();
                keepSelection();
            }
            else if (browserLike >= 0)
            {
                browserLike = -1;
                keepSelection();
            }
            else
            {
                engine.preview (nullptr);
                console.focusTerminal();
                message = L"back to the tracks \x00b7 ctrl+B the browser again (and again closes it)";
                messageIsError = false;
            }
            return true;

        case Key::Tab:
        {
            // Round the areas: tabs, map, list, and the pads on a kit.
            const int count = onKit ? 4 : 3;
            browserFocus = (BrowserFocus) (((int) browserFocus + (k.shift ? count - 1 : 1)) % count);
            return true;
        }

        case Key::Left:
        case Key::Right:
        {
            const int step = k.code == Key::Left ? -1 : 1;

            if (browserFocus == BrowserFocus::kinds)
            {
                browserKind = juce::jlimit (0, (int) kinds.size() - 1, browserKind + step);
                browserLike = -1;
                keepSelection();
            }
            else if (browserFocus == BrowserFocus::map)
                moveOnMap ((float) step, 0.0f, k.ctrl);
            else
                browserFocus = BrowserFocus::map;

            return true;
        }

        case Key::Up:
        case Key::Down:
        {
            const int step = k.code == Key::Up ? -1 : 1;

            if (k.shift)
            {
                say (setAuditionDb (auditionDb - step * 1.0));
                return true;
            }

            if (browserFocus == BrowserFocus::kinds)
            {
                if (step > 0)
                    browserFocus = BrowserFocus::map;
            }
            else if (browserFocus == BrowserFocus::map)
            {
                // Up past the top of the map goes to the tabs.
                if (! moveOnMap (0.0f, (float) step, k.ctrl) && step < 0)
                    browserFocus = BrowserFocus::kinds;
            }
            else if (k.ctrl && ! shown.empty()) // ten at a time
                browserPick (shown[(size_t) juce::jlimit (0, (int) shown.size() - 1, at + step * 10)]);
            else if (at + step >= 0 && at + step < (int) shown.size())
                browserPick (shown[(size_t) (at + step)]);

            return true;
        }

        case Key::PageUp:
        case Key::PageDown:
        case Key::Home:
        case Key::End:
            if (! shown.empty())
            {
                const int page = 10;
                const int to = k.code == Key::Home ? 0 : k.code == Key::End ? (int) shown.size() - 1
                             : at + (k.code == Key::PageUp ? -page : page);
                browserPick (shown[(size_t) juce::jlimit (0, (int) shown.size() - 1, to)]);
            }
            return true;

        case Key::Enter:
            say (browserLoad (k.shift));
            return true;

        case Key::Backspace:
            browserFilter = browserFilter.dropLastCharacters (1);
            keepSelection();
            return true;

        case Key::Char:
            if (k.ch == ' ' && browserFilter.isEmpty())
            {
                if (browserSample >= 0)
                    browserAudition (browserSample); // again
            }
            else if (k.ch == ' ')
            {
                if (! browserFilter.endsWithChar (' '))
                    browserFilter += " "; // between filter terms
            }
            else if (k.ch >= 32)
            {
                browserFilter += juce::String::charToString ((juce::juce_wchar) k.ch);
                browserLike = -1;
                keepSelection();
            }
            return true;

        default:
            return true;
    }
}

//==============================================================================
Result App::startIndexing (const juce::StringArray& paths)
{
    if (isIndexing())
        return Result::fail (L"already indexing \x00b7 it says when it's done");

    if (paths[0].equalsIgnoreCase ("remove"))
    {
        auto folders = paths;
        folders.remove (0);
        return removeFromLibrary (folders);
    }

    if (paths.isEmpty())
#if JUCE_WINDOWS
        return Result::fail (L"index what? index D:\\Samples\\Drums, or type index and drop a folder (it's only read)");
#else
        return Result::fail (L"index what? index ~/Samples/Drums, or type index and drop a folder (it's only read)");
#endif

    juce::Array<juce::File> folders;
    juce::StringArray missing;

    for (const auto& p : paths)
    {
        const auto f = Import::resolve (expandHome (p.unquoted()), juce::File::getCurrentWorkingDirectory());

        if (f.isDirectory())
            folders.add (f);
        else
            missing.add (p);
    }

    if (folders.isEmpty())
        return Result::fail ("no such folder: " + missing.joinIntoString (", "));

    indexDone = 0;
    indexTotal = 0;
    indexFinished = false;

    indexThread = std::thread ([this, folders]
    {
        Library lib (libraryFile());
        const auto started = juce::Time::getMillisecondCounterHiRes();
        lib.scan (folders, [this] (int done, int total) { indexDone = done; indexTotal = total; });
        const auto agree = lib.samples().empty() ? 0.0 : lib.classify();
        lib.layout();
        lib.save();

        indexSummary = "indexed " + juce::String ((int) lib.samples().size()) + L" samples in "
                       + juce::String ((juce::Time::getMillisecondCounterHiRes() - started) / 1000.0, 1) + L" s \x00b7 the sound agrees with the names "
                       + juce::String (juce::roundToInt (agree * 100.0)) + L"% of the time \x00b7 ctrl+B browses";
        indexFinished = true;
    });

    return Result::good ("indexing " + folders[0].getFileName() + (folders.size() > 1 ? " and " + juce::String (folders.size() - 1) + " more" : juce::String())
                         + (missing.isEmpty() ? juce::String() : L" \x00b7 not found: " + missing.joinIntoString (", ")));
}

namespace
{
// ~/Samples rather than /Users/you/Samples (Windows paths as they are).
juce::String shortPath (const juce::String& path)
{
#if JUCE_WINDOWS
    return path;
#else
    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName();
    return path.startsWith (home + "/") ? "~" + path.substring (home.length()) : path;
#endif
}

int samplesIn (const Library& lib, const juce::File& folder)
{
    int n = 0;

    for (const auto& s : lib.samples())
        n += juce::File (s.path).isAChildOf (folder) ? 1 : 0;

    return n;
}
} // namespace

Result App::removeFromLibrary (const juce::StringArray& paths)
{
    if (paths.isEmpty())
        return Result::fail ("index remove what? type index remove and drop the folder, or its path (library lists them)");

    loadLibrary();
    juce::Array<juce::File> folders;
    juce::StringArray unknown;

    // The folder may be gone (a drive unplugged): it only has to be in the index.
    for (const auto& p : paths)
    {
        const auto f = Import::resolve (expandHome (p.unquoted()), juce::File::getCurrentWorkingDirectory());
        bool listed = false;

        for (const auto& l : library->folders())
            listed = listed || juce::File (l) == f || juce::File (l).isAChildOf (f);

        if (listed || samplesIn (*library, f) > 0)
            folders.add (f);
        else
            unknown.add (shortPath (f.getFullPathName()));
    }

    if (folders.isEmpty())
        return Result::fail (L"not in the library: " + unknown.joinIntoString (", ") + L" \x00b7 library lists its folders");

    indexDone = 0;
    indexTotal = 0;
    indexFinished = false;

    indexThread = std::thread ([this, folders]
    {
        Library lib (libraryFile());
        int removed = 0;

        for (const auto& f : folders)
            removed += lib.remove (f);

        // What's left is sorted and mapped again without them.
        if (! lib.samples().empty())
        {
            lib.classify();
            lib.layout();
        }

        lib.save();
        indexSummary = "took " + juce::String (removed) + " samples out of the library \x00b7 " + juce::String ((int) lib.samples().size()) + " left";
        indexFinished = true;
    });

    return Result::good ("taking " + folders[0].getFileName() + (folders.size() > 1 ? " and " + juce::String (folders.size() - 1) + " more" : juce::String())
                         + " out of the library" + (unknown.isEmpty() ? juce::String() : L" \x00b7 not in it: " + unknown.joinIntoString (", ")));
}

Result App::describeLibrary()
{
    loadLibrary();
    const auto total = (int) library->samples().size();

    if (total == 0 && library->folders().isEmpty())
#if JUCE_WINDOWS
        return Result::good (L"the library is empty \x00b7 index D:\\Samples adds a folder, or type index and drop one");
#else
        return Result::good (L"the library is empty \x00b7 index ~/Samples adds a folder, or type index and drop one");
#endif

    juce::StringArray parts;
    int counted = 0;

    for (const auto& folder : library->folders())
    {
        const auto n = samplesIn (*library, juce::File (folder));
        counted += n;
        parts.add (shortPath (folder) + " " + juce::String (n));
    }

    // Indexed before the folders were kept: indexing them again lists them.
    if (counted < total)
        parts.add (juce::String (total - counted) + " from earlier (index their folder again to list it)");

    return Result::good ("library: " + juce::String (total) + L" samples \x00b7 " + parts.joinIntoString (juce::String (L" \x00b7 "))
                         + L" \x00b7 index remove <folder> takes one out");
}

void App::pollIndexing()
{
    if (! isIndexing())
        return;

    if (! indexFinished)
    {
        if (indexTotal > 0)
        {
            message = "indexing " + juce::String (indexDone.load()) + " / " + juce::String (indexTotal.load());
            messageIsError = false;
        }

        return;
    }

    indexThread.join();

    // The new index replaces the old; the selection follows its file.
    const auto selected = library != nullptr && browserSample >= 0 && browserSample < (int) library->samples().size()
                              ? library->samples()[(size_t) browserSample].path : juce::String();
    library = std::make_unique<Library> (libraryFile());
    browserSample = -1;

    for (int i = 0; i < (int) library->samples().size(); ++i)
        if (library->samples()[(size_t) i].path == selected)
            browserSample = i;

    browserKind = juce::jlimit (0, (int) browserKinds().size() - 1, browserKind);
    say (Result::good (indexSummary));
}
} // namespace taw
