// The sample browser: docked right of the timeline (ctrl+B takes the keys into it, showing it;
// ctrl+B again hides it; Esc gives the keys back and leaves it showing). Selectors at the top choose the map's axes (the sound-alike
// map, or two measurements) and what its colours group by; badges pick a kind; the map shows that
// kind's samples, the list the same samples one by one, in the same colours; typing filters both
// by name. Every move auditions, at the browser's own level. Enter gives the focused sample track
// that sample (keeping its hits) or makes a new sample track.

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

                if (number.isEmpty() || ! juce::CharacterFunctions::isDigit (number[0]) && number[0] != '-' && number[0] != '.')
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
        && c.filter == browserFilter)
        return c;

    c.filter = browserFilter;

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

            while (hi + 1 < v.size() && v[hi + 1].first == v[lo].first)
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
        library = std::make_unique<Library> (libraryFile());
}

Result App::showBrowser (bool show)
{
    browserVisible = show;
    browserActive = show;
    saveSettings();

    if (! show)
    {
        engine.preview (nullptr);
        return Result::good();
    }

    return focusBrowser();
}

Result App::focusBrowser()
{
    loadLibrary();
    browserVisible = browserActive = true;
    browserFocus = BrowserFocus::map; // the map (and typing to filter) first
    saveSettings();

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

    return Result::good (juce::String ((int) library->samples().size()) + L" samples \x00b7 \x2190\x2192 kinds \x00b7 tab map / list \x00b7 type to filter \x00b7 esc back to the tracks");
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

    const auto kinds = browserKinds();
    const auto kind = kinds[(size_t) juce::jlimit (0, (int) kinds.size() - 1, browserKind)];
    const auto& all = library->samples();
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

    // Kind by kind (in the badges' order), then by name.
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

    return browserKind == 0 ? std::pair { s.mapX, s.mapY } : std::pair { s.kindX, s.kindY };
}

Result App::setAuditionDb (double db)
{
    auditionDb = juce::jlimit (-60.0, 6.0, std::round (db * 2.0) / 2.0);
    saveSettings();
    return Result::good ("audition " + formatDb (auditionDb) + " dB");
}

// Selects a sample and plays it from the top, resampled to the device's rate.
void App::browserPick (int sample)
{
    browserSample = sample;

    if (library == nullptr || sample < 0 || sample >= (int) library->samples().size())
        return;

    const auto& s = library->samples()[(size_t) sample];
    const juce::File file (s.path);
    message = file.getFileName() + L" \x00b7 " + kindOf (s) + (s.nameKind.isEmpty() ? " (by its sound)" : "");
    messageIsError = false;

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

    engine.preview (std::move (out), juce::Decibels::decibelsToGain ((float) auditionDb));
}

bool App::handleBrowserKey (const Key& k)
{
    if (! browserVisible)
        return false;

    const auto& g = browserGeometry;

    // A click in the pane takes the keys into it; elsewhere gives them back to the tracks.
    if (k.code == Key::Mouse)
    {
        if (! g.valid || k.x < g.left || k.y < g.top || k.y >= g.top + g.rows)
        {
            browserActive = false;
            return false;
        }

        loadLibrary();
        browserActive = true;
        const auto shown = browserShown();

        // A selector row: the click steps it (‹ on the left half back, else on).
        for (int r = 0; r < 2; ++r)
            if (k.y == g.axisRow[r])
            {
                browserFocus = r == 0 ? BrowserFocus::xAxis : BrowserFocus::yAxis;
                setBrowserAxis (r == 1, k.x < g.left + 14 ? -1 : 1);
                return true;
            }

        if (std::any_of (g.badges.begin(), g.badges.end(), [&] (const auto& b) { return b.y == k.y; }))
        {
            for (size_t b = 0; b < g.badges.size(); ++b)
                if (k.y == g.badges[b].y && k.x >= g.badges[b].x && k.x < g.badges[b].x + g.badges[b].w)
                {
                    browserKind = (int) b;
                    browserFocus = BrowserFocus::kinds;
                }

            const auto now = browserShown();

            if (std::find (now.begin(), now.end(), browserSample) == now.end())
                browserSample = now.empty() ? -1 : now.front();
        }
        else if (k.x >= g.listX && k.x < g.listX + g.listW && k.y >= g.listY && k.y < g.listY + g.listRows)
        {
            const int i = g.listFirst + k.y - g.listY;
            browserFocus = BrowserFocus::list;

            if (i < (int) shown.size())
                browserPick (shown[(size_t) i]);
        }
        else if (k.x >= g.mapX && k.x < g.mapX + g.mapW && k.y >= g.mapY && k.y < g.mapY + g.mapH && ! shown.empty())
        {
            // The sample nearest the clicked cell's centre.
            browserFocus = BrowserFocus::map;
            const float cx = (k.x - g.mapX + 0.5f) / (float) g.mapW, cy = (k.y - g.mapY + 0.5f) / (float) g.mapH;
            int best = -1;
            float bestD = 1.0e9f;

            for (auto i : shown)
            {
                const auto [x, y] = browserPosition (i);
                const float dx = (x - cx) * (float) g.mapW, dy = (y - cy) * (float) g.mapH * 2.0f;

                if (const float d = dx * dx + dy * dy; d < bestD)
                    bestD = d, best = i;
            }

            browserPick (best);
        }

        return true;
    }

    if (! browserActive)
        return false;

    // Ctrl shortcuts (ctrl+q, ctrl+b, ctrl+z, ctrl+space ...) and shift+space (the transport)
    // go on to taw.
    if ((k.code == Key::Char && (k.ctrl || k.alt)) || (k.code == Key::Char && k.ch == ' ' && k.shift))
        return false;

    loadLibrary();
    const auto kinds = browserKinds();
    auto shown = browserShown();
    const int at = (int) (std::find (shown.begin(), shown.end(), browserSample) - shown.begin());

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
        const float sx = (float) std::max (1, g.mapW), sy = (float) std::max (1, g.mapH) * 2.0f; // in pixels (half cells), about square
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

            const float jump = far ? 0.2f * (std::abs (ux) > 0.0f ? sx : sy) : 0.0f; // in pixels
            const float score = far ? std::sqrt ((along - jump) * (along - jump) + across * across) + across
                                    : std::sqrt (along * along + across * across) + across;

            if (score < bestScore)
                bestScore = score, best = i;
        }

        if (best >= 0)
            browserPick (best);

        return best >= 0;
    };

    // alt+arrows: the axes, from anywhere in the browser (left/right x, up/down y).
    if (k.alt && ! k.ctrl && (k.code == Key::Left || k.code == Key::Right || k.code == Key::Up || k.code == Key::Down))
    {
        const bool y = k.code == Key::Up || k.code == Key::Down;
        setBrowserAxis (y && browserAxisX != 0, k.code == Key::Right || k.code == Key::Up ? 1 : -1);
        return true;
    }

    switch (k.code)
    {
        case Key::Escape:
            if (browserFilter.isNotEmpty())
            {
                browserFilter.clear();
                keepSelection();
            }
            else
            {
                browserActive = false;
                engine.preview (nullptr);
                message = L"back to the tracks \x00b7 ctrl+B the browser again (and again hides it)";
                messageIsError = false;
            }
            return true;

        case Key::Tab:
            browserFocus = browserFocus == BrowserFocus::map ? BrowserFocus::list : BrowserFocus::map;
            return true;

        case Key::Left:
        case Key::Right:
        {
            const int step = k.code == Key::Left ? -1 : 1;

            if (browserFocus == BrowserFocus::xAxis || browserFocus == BrowserFocus::yAxis)
                setBrowserAxis (browserFocus == BrowserFocus::yAxis, step);
            else if (browserFocus == BrowserFocus::kinds)
            {
                browserKind = juce::jlimit (0, (int) kinds.size() - 1, browserKind + step);
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

            // The selectors and the badges, top to bottom: x, y, kinds, then the map.
            if (browserFocus == BrowserFocus::xAxis || browserFocus == BrowserFocus::yAxis
                || browserFocus == BrowserFocus::kinds)
            {
                // y is skipped while the map is the sound-alike one (it has no y of its own).
                auto next = (int) browserFocus + step;

                if (next == (int) BrowserFocus::yAxis && browserAxisX == 0)
                    next += step;

                browserFocus = (BrowserFocus) juce::jlimit ((int) BrowserFocus::xAxis, (int) BrowserFocus::map, next);
            }
            else if (browserFocus == BrowserFocus::map)
            {
                // Up past the top of the map goes to the badges.
                if (! moveOnMap (0.0f, (float) step, k.ctrl) && step < 0)
                    browserFocus = BrowserFocus::kinds;
            }
            else if (at + step < 0)
                browserFocus = BrowserFocus::kinds;
            else if (k.ctrl && ! shown.empty()) // ten at a time
                browserPick (shown[(size_t) juce::jlimit (0, (int) shown.size() - 1, at + step * 10)]);
            else if (at + step < (int) shown.size())
                browserPick (shown[(size_t) (at + step)]);

            return true;
        }

        case Key::PageUp:
        case Key::PageDown:
        case Key::Home:
        case Key::End:
            if (! shown.empty())
            {
                const int page = std::max (1, g.listRows - 1);
                const int to = k.code == Key::Home ? 0 : k.code == Key::End ? (int) shown.size() - 1
                             : at + (k.code == Key::PageUp ? -page : page);
                browserFocus = BrowserFocus::list;
                browserPick (shown[(size_t) juce::jlimit (0, (int) shown.size() - 1, to)]);
            }
            return true;

        case Key::Enter:
        {
            if (session == nullptr)
            {
                say (Result::fail ("open a session first, then enter puts the sample on a track"));
                return true;
            }

            if (browserSample < 0)
                return true;

            const int target = ! onTopLane() && ! mixFocused() && focused >= 0 && focused < session->numTracks()
                                       && Session::isSampleTrack (session->track (focused))
                                   ? focused : -1;
            say (startImport ({ library->samples()[(size_t) browserSample].path }, target, ImportAs::sample));
            return true;
        }

        case Key::Backspace:
            browserFilter = browserFilter.dropLastCharacters (1);
            keepSelection();
            return true;

        case Key::Char:
            if (k.ch == ' ' && browserFilter.isEmpty())
                browserPick (browserSample); // again
            else if (k.ch == ' ')
            {
                if (! browserFilter.endsWithChar (' '))
                    browserFilter += " "; // between filter terms
            }
            else if (k.ch >= 32)
            {
                browserFilter += juce::String::charToString ((juce::juce_wchar) k.ch);
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

    if (paths.isEmpty())
#if JUCE_WINDOWS
        return Result::fail (L"index what? index D:\\Samples\\Drums (it's only read; the index lives in taw's settings folder)");
#else
        return Result::fail (L"index what? index ~/Samples/Drums (it's only read; the index lives in taw's settings folder)");
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
        const auto agree = lib.classify();
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

void App::pollIndexing()
{
    // Docked from the start (it's remembered): the index loads with the first frame.
    if (browserVisible && library == nullptr)
        loadLibrary();

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
