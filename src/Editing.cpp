// Copy, cut, paste, duplicate and delete over a time selection. Audio tracks move clips (which
// only point into files, so nothing is copied on disk); sample tracks move hits. Each operation
// is one undo step: the caller opens it with beginEdit.

#include "App.h"

namespace taw
{
namespace
{
bool overlaps (juce::int64 s, juce::int64 e, juce::Range<juce::int64> r) { return s < r.getEnd() && e > r.getStart(); }
} // namespace

double App::snapBeats (juce::int64 samples) const
{
    // Positions are whole samples, so at tempos like 99 bpm a step lands a fraction of a sample
    // off the exact beat. Anything within half a sample of a step is on it.
    const auto spb = session->samplesPerBeat();
    const auto step = session->fineStepBeats();
    const auto b = (double) samples / spb;
    const auto onStep = std::round (b / step) * step;
    return std::abs (onStep - b) <= 0.5 / spb ? onStep : b;
}

bool App::hitInRange (double beat, double from, double to) const
{
    const auto halfSample = 0.5 / session->samplesPerBeat();
    return beat >= from - halfSample && beat < to - halfSample;
}

Clipboard App::gather (const std::vector<int>& tracks, juce::Range<juce::int64> range) const
{
    Clipboard c;
    c.length = range.getLength();
    const auto from = snapBeats (range.getStart()), to = snapBeats (range.getEnd());
    c.beats = to - from;

    for (auto t : tracks)
    {
        const auto track = session->track (t);
        Clipboard::Lane lane;
        lane.sample = Session::isSampleTrack (track);
        lane.instrument = Session::isInstrumentTrack (track);

        if (lane.instrument)
        {
            // Notes that start in the range (whole, even if they run past it), and controllers.
            for (const auto& n : track)
                if ((n.hasType (ids::NOTE) || n.hasType (ids::CC)) && hitInRange (n[ids::beat], from, to))
                {
                    auto copy = n.createCopy();
                    copy.setProperty (ids::beat, (double) n[ids::beat] - from, nullptr);
                    lane.items.push_back (copy);
                }
        }
        else if (lane.sample)
        {
            for (const auto& h : Session::hits (track))
                if (const double beat = h[ids::beat]; hitInRange (beat, from, to))
                    lane.items.push_back (Session::makeHit (beat - from, (float) h[ids::vel]));
        }
        else
        {
            // Clips trimmed to the range, in their original order so layering is kept.
            for (const auto& clip : Session::clips (track))
            {
                const auto s = (juce::int64) clip[ids::start], e = s + (juce::int64) clip[ids::length];

                if (! overlaps (s, e, range))
                    continue;

                const auto lo = juce::jmax (s, range.getStart()), hi = juce::jmin (e, range.getEnd());
                lane.items.push_back (Session::makeClip (clip[ids::file], lo - range.getStart(),
                                                         (juce::int64) clip[ids::offset] + (lo - s), hi - lo));
            }
        }

        c.lanes.push_back (std::move (lane));
    }

    return c;
}

int App::clearRange (int trackIndex, juce::Range<juce::int64> range)
{
    auto track = session->track (trackIndex);
    int changed = 0;

    if (Session::isInstrumentTrack (track))
    {
        const auto from = snapBeats (range.getStart()), to = snapBeats (range.getEnd());

        for (int i = track.getNumChildren(); --i >= 0;)
            if (const auto n = track.getChild (i); (n.hasType (ids::NOTE) || n.hasType (ids::CC)) && hitInRange (n[ids::beat], from, to))
            {
                track.removeChild (n, um());
                ++changed;
            }

        return changed;
    }

    if (Session::isSampleTrack (track))
    {
        const auto from = snapBeats (range.getStart()), to = snapBeats (range.getEnd());

        for (const auto& h : Session::hits (track))
            if (hitInRange (h[ids::beat], from, to))
            {
                track.removeChild (h, um());
                ++changed;
            }

        return changed;
    }

    for (auto clip : Session::clips (track))
    {
        const auto s = (juce::int64) clip[ids::start], len = (juce::int64) clip[ids::length], e = s + len;
        const auto offset = (juce::int64) clip[ids::offset];
        const auto a = range.getStart(), b = range.getEnd();

        if (! overlaps (s, e, range))
            continue;

        ++changed;

        if (s >= a && e <= b)
        {
            track.removeChild (clip, um());
        }
        else if (s < a && e > b)
        {
            // The range is inside the clip: keep the head, add the tail right above it.
            track.addChild (Session::makeClip (clip[ids::file], b, offset + (b - s), e - b), track.indexOf (clip) + 1, um());
            clip.setProperty (ids::length, a - s, um());
        }
        else if (s < a)
        {
            clip.setProperty (ids::length, a - s, um());
        }
        else
        {
            clip.setProperty (ids::start, b, um())
                .setProperty (ids::offset, offset + (b - s), um())
                .setProperty (ids::length, e - b, um());
        }
    }

    return changed;
}

Result App::pasteAt (const Clipboard& c, const std::vector<int>& dest, juce::int64 at)
{
    if (c.lanes.empty())
        return Result::fail (L"nothing copied \x2014 select a range (shift + \x2190 \x2192), then ctrl+c");

    const auto atBeats = snapBeats (at);
    const auto halfSample = 0.5 / session->samplesPerBeat();
    juce::StringArray pasted, skipped;

    for (size_t i = 0; i < c.lanes.size(); ++i)
    {
        const auto& lane = c.lanes[i];
        const int t = i < dest.size() ? dest[i] : session->numTracks();

        if (t >= session->numTracks())
        {
            skipped.add (juce::String ((int) (c.lanes.size() - i)) + " more track" + (c.lanes.size() - i == 1 ? "" : "s") + " (past the last track)");
            break;
        }

        auto track = session->track (t);

        if (lane.sample != Session::isSampleTrack (track) || lane.instrument != Session::isInstrumentTrack (track))
        {
            skipped.add ("track " + juce::String (t + 1) + " ("
                         + (lane.sample ? "hits need a sample track" : lane.instrument ? "notes need an instrument track" : "audio needs an audio track") + ")");
            continue;
        }

        for (const auto& item : lane.items)
        {
            if (lane.instrument)
            {
                // Notes add to what's there, like playing them in again.
                auto copy = item.createCopy();
                copy.setProperty (ids::beat, atBeats + (double) item[ids::beat], nullptr);
                track.appendChild (copy, um());
            }
            else if (lane.sample)
            {
                // Pasted hits win over any already on the same step.
                const auto beat = atBeats + (double) item[ids::beat];

                for (const auto& h : Session::hits (track))
                    if (std::abs ((double) h[ids::beat] - beat) <= halfSample)
                        track.removeChild (h, um());

                track.appendChild (Session::makeHit (beat, (float) item[ids::vel]), um());
            }
            else
            {
                // Appended clips play over what's already there (later wins).
                auto clip = item.createCopy();
                clip.setProperty (ids::start, at + (juce::int64) item[ids::start], nullptr);
                track.appendChild (clip, um());
            }
        }

        pasted.add (juce::String (t + 1));
    }

    if (pasted.isEmpty())
        return Result::fail ("nothing pasted: " + skipped.joinIntoString ("; "));

    auto msg = "pasted onto " + juce::String (pasted.size() == 1 ? "track " : "tracks ") + pasted.joinIntoString (",") + " at " + formatBars (at);

    if (! skipped.isEmpty())
        return Result::fail (msg + L" \x00b7 skipped " + skipped.joinIntoString ("; "));

    return Result::good (msg);
}

Result App::checkEditable (const std::vector<int>& tracks, bool needsRange) const
{
    if (session == nullptr)
        return Result::fail ("no session");

    if (recording)
        return Result::fail ("stop recording first");

    if (needsRange && ! hasTimeSelection())
        return Result::fail (L"select a range first (shift + \x2190 \x2192)");

    if (tracks.empty())
        return Result::fail (L"choose a track (\x2191 \x2193), not the mix");

    return Result::good();
}

Result App::copySelection (const std::vector<int>& tracks)
{
    if (auto r = checkEditable (tracks, true); ! r.ok)
        return r;

    clipboard = gather (tracks, timeSelection());
    Console::setClipboardText (pasteMarker);

    int items = 0;

    for (const auto& l : clipboard.lanes)
        items += (int) l.items.size();

    return Result::good ("copied " + formatBars (timeSelection().getStart()) + L" \x2192 " + formatBars (timeSelection().getEnd())
                         + " from " + trackList (tracks) + L" \x00b7 " + juce::String (items) + (items == 1 ? " item" : " items")
                         + L" \x00b7 ctrl+v pastes at the cursor");
}

Result App::cutSelection (const std::vector<int>& tracks)
{
    if (auto r = copySelection (tracks); ! r.ok)
        return r;

    for (auto t : tracks)
        clearRange (t, timeSelection());

    return Result::good ("cut " + formatBars (timeSelection().getStart()) + L" \x2192 " + formatBars (timeSelection().getEnd())
                         + " from " + trackList (tracks) + L" \x00b7 ctrl+v pastes it");
}

Result App::deleteSelection (const std::vector<int>& tracks)
{
    if (auto r = checkEditable (tracks, true); ! r.ok)
        return r;

    int n = 0;

    for (auto t : tracks)
        n += clearRange (t, timeSelection());

    return Result::good ("cleared " + formatBars (timeSelection().getStart()) + L" \x2192 " + formatBars (timeSelection().getEnd())
                         + " on " + trackList (tracks) + (n == 0 ? juce::String (" (it was empty)") : juce::String()));
}

Result App::paste (int firstTrack)
{
    if (auto r = checkEditable ({ firstTrack }, false); ! r.ok || firstTrack < 0)
        return r.ok ? Result::fail (L"choose a track (\x2191 \x2193), not the mix") : r;

    std::vector<int> dest;

    for (size_t i = 0; i < clipboard.lanes.size(); ++i)
        dest.push_back (firstTrack + (int) i);

    // At the cursor, playing or stopped (during playback the arrows move it, not the playhead).
    return pasteAt (clipboard, dest, cursor);
}

Result App::duplicateSelection (const std::vector<int>& tracks)
{
    if (auto r = checkEditable (tracks, true); ! r.ok)
        return r;

    // Right after itself, then the selection moves onto the copy so you can keep going.
    const auto range = timeSelection();
    const auto copy = gather (tracks, range);

    if (auto r = pasteAt (copy, tracks, range.getEnd()); ! r.ok)
        return r;

    timeAnchor = range.getEnd();
    timeHead = range.getEnd() + range.getLength();

    if (! engine.isRolling())
        setCursor (timeHead);

    return Result::good ("duplicated to " + formatBars (timeAnchor) + L" \x2192 " + formatBars (timeHead) + L" \x00b7 ctrl+d again repeats");
}
Result App::selectRange (const juce::String& fromSpec, const juce::String& toSpec)
{
    juce::int64 from = 0, to = 0;

    if (fromSpec.isEmpty() || toSpec.isEmpty())
        return Result::fail ("sel from to, e.g. sel 3 5 (bars 3 to 5)");

    if (auto r = parsePosition (fromSpec, from); r.failed())
        return Result::fail (r.getErrorMessage());

    if (auto r = parsePosition (toSpec, to); r.failed())
        return Result::fail (r.getErrorMessage());

    if (from == to)
        return Result::fail ("that range is empty");

    timeAnchor = from;
    timeHead = to;

    if (! engine.isRolling())
        setCursor (to);

    return Result::good ("selected " + formatBars (juce::jmin (from, to)) + L" \x2192 " + formatBars (juce::jmax (from, to)));
}
} // namespace taw
