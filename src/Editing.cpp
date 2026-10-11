// Copy, cut, paste, duplicate, delete and consolidate over a time selection. Audio tracks move
// clips (which only point into files, so nothing is copied on disk); instrument and sample tracks
// move their MIDI and hit clips, trimmed to the selection. Each operation is one undo step: the
// caller opens it with beginEdit.

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
        c.lanes.push_back (gatherLane (session->track (t), clipHolder (t), range));

    return c;
}

// One track's clips over the range, from holder: the track, or one of its playlists (whose
// clips leave the track's automation where it is).
Clipboard::Lane App::gatherLane (const juce::ValueTree& track, const juce::ValueTree& holder, juce::Range<juce::int64> range) const
{
    const auto from = snapBeats (range.getStart()), to = snapBeats (range.getEnd());
    Clipboard::Lane lane;
    lane.sample = Session::isSampleTrack (track);
    lane.instrument = Session::isInstrumentTrack (track);

    if (lane.instrument || lane.sample)
    {
        // MIDI or hit clips trimmed to the range, with the notes or hits that start in what's left.
        for (const auto& clip : Session::beatClips (holder))
            if (auto piece = Session::trimmedMidiClip (clip, from, to); piece.isValid())
            {
                piece.setProperty (ids::beat, (double) piece[ids::beat] - from, nullptr);
                lane.items.push_back (piece);
            }
    }
    else
    {
        // Clips trimmed to the range, in their original order so layering is kept.
        for (const auto& clip : Session::clips (holder))
        {
            const auto s = (juce::int64) clip[ids::start], e = s + (juce::int64) clip[ids::length];

            if (! overlaps (s, e, range))
                continue;

            const auto lo = juce::jmax (s, range.getStart()), hi = juce::jmin (e, range.getEnd());
            lane.items.push_back (Session::makeClip (clip[ids::file], lo - range.getStart(),
                                                     (juce::int64) clip[ids::offset] + (lo - s), hi - lo));
        }
    }

    // The automation over the range goes with the clips (the main's: it's the track's).
    if (holder == track)
        lane.automation = gatherAutomation (track, from, to);

    return lane;
}

int App::clearRange (int trackIndex, juce::Range<juce::int64> range)
{
    return clearClips (session->track (trackIndex), clipHolder (trackIndex), range);
}

int App::clearClips (const juce::ValueTree& kindOf, juce::ValueTree track, juce::Range<juce::int64> range)
{
    int changed = 0;

    if (Session::isInstrumentTrack (kindOf) || Session::isSampleTrack (kindOf))
    {
        // As with audio clips: trimmed, split or removed. Trimming only hides notes and hits; the
        // tail of a split keeps just the ones that start in it.
        const auto a = snapBeats (range.getStart()), b = snapBeats (range.getEnd());
        const auto slack = 0.5 / session->samplesPerBeat();

        for (auto clip : Session::beatClips (track))
        {
            const double s = clip[ids::beat], e = Session::clipEnd (clip), offset = clip[ids::offset];

            if (s >= b - slack || e <= a + slack)
                continue;

            ++changed;

            if (s >= a - slack && e <= b + slack)
            {
                track.removeChild (clip, um());
            }
            else if (s < a && e > b)
            {
                track.addChild (Session::trimmedMidiClip (clip, b, e), track.indexOf (clip) + 1, um());
                clip.setProperty (ids::len, a - s, um());
            }
            else if (s < a)
            {
                clip.setProperty (ids::len, a - s, um());
            }
            else
            {
                clip.setProperty (ids::beat, b, um()).setProperty (ids::offset, offset + (b - s), um()).setProperty (ids::len, e - b, um());
            }
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

        if (Session::isBusTrack (track))
        {
            skipped.add ("track " + juce::String (t + 1) + " (a bus)");
            continue;
        }

        if (lane.sample != Session::isSampleTrack (track) || lane.instrument != Session::isInstrumentTrack (track))
        {
            skipped.add ("track " + juce::String (t + 1) + " ("
                         + (lane.sample ? "hits need a sample track" : lane.instrument ? "notes need an instrument track" : "audio needs an audio track") + ")");
            continue;
        }

        pasteLane (track, clipHolder (t), lane, at, c.beats);
        pasted.add (juce::String (t + 1));
    }

    if (pasted.isEmpty())
        return Result::fail ("nothing pasted: " + skipped.joinIntoString ("; "));

    auto msg = "pasted onto " + juce::String (pasted.size() == 1 ? "track " : "tracks ") + pasted.joinIntoString (",") + " at " + formatBars (at);

    if (! skipped.isEmpty())
        return Result::fail (msg + L" \x00b7 skipped " + skipped.joinIntoString ("; "));

    return Result::good (msg);
}

// One lane of a clipboard onto holder (the track, or one of its playlists) at a position.
void App::pasteLane (const juce::ValueTree& track, juce::ValueTree holder, const Clipboard::Lane& lane, juce::int64 at, double beats)
{
    const auto atBeats = snapBeats (at);

    // A pasted loop of hits replaces what was under it (two loops at once would double
    // every hit); MIDI clips add to what's there (overlapping ones both play).
    if (lane.sample)
        clearClips (track, holder, { at, at + std::llround (beats * session->samplesPerBeat()) });

    for (const auto& item : lane.items)
    {
        if (lane.instrument || lane.sample)
        {
            auto copy = item.createCopy();
            copy.setProperty (ids::beat, atBeats + (double) item[ids::beat], nullptr);
            holder.appendChild (copy, um());
        }
        else
        {
            // Appended clips play over what's already there (later wins).
            auto clip = item.createCopy();
            clip.setProperty (ids::start, at + (juce::int64) item[ids::start], nullptr);
            holder.appendChild (clip, um());
        }
    }

    if (holder == track)
        pasteAutomation (track, lane.automation, atBeats, beats);
}

// Cut and delete: the clips in the range, and the automation under them (on a playlist lane,
// only its clips: the automation is the track's).
int App::clearTracks (const std::vector<int>& tracks, juce::Range<juce::int64> range)
{
    int n = 0;

    for (auto t : tracks)
    {
        n += clearRange (t, range);

        if (clipHolder (t) == session->track (t))
            clearAutomationRange (session->track (t), snapBeats (range.getStart()), snapBeats (range.getEnd()));
    }

    return n;
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

    clearTracks (tracks, timeSelection());

    return Result::good ("cut " + formatBars (timeSelection().getStart()) + L" \x2192 " + formatBars (timeSelection().getEnd())
                         + " from " + trackList (tracks) + L" \x00b7 ctrl+v pastes it");
}

Result App::deleteSelection (const std::vector<int>& tracks)
{
    if (auto r = checkEditable (tracks, true); ! r.ok)
        return r;

    const auto n = clearTracks (tracks, timeSelection());

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
Result App::selectClipAtCursor()
{
    if (session == nullptr || onTopLane() || mixFocused() || focused >= session->numTracks())
        return Result::fail (L"sel clip selects the clip under the cursor \x2014 focus a track (\x2191\x2193)");

    const auto track = clipHolder (focused);
    const auto at = engine.isRolling() ? heardPlayhead() : cursor;
    juce::int64 from = -1, to = -1;

    if (const auto clip = midiClipAt (focused, (double) at, (double) at + 1.0); clip.isValid())
    {
        from = std::llround ((double) clip[ids::beat] * session->samplesPerBeat());
        to = std::llround (Session::clipEnd (clip) * session->samplesPerBeat());
    }
    else
    {
        const auto audio = Session::clips (track);

        for (auto it = audio.rbegin(); it != audio.rend(); ++it) // the one on top
            if (const auto s = (juce::int64) (*it)[ids::start], e = s + (juce::int64) (*it)[ids::length]; s <= at && e > at)
            {
                from = s, to = e;
                break;
            }
    }

    if (from < 0)
        return Result::fail ("no clip under the cursor on track " + juce::String (focused + 1));

    timeAnchor = from;
    timeHead = to;

    if (! engine.isRolling())
        setCursor (from);

    return Result::good ("selected the clip " + formatBars (from) + L" \x2192 " + formatBars (to) + L" \x00b7 ctrl+c copies, ctrl+x cuts, ctrl+d repeats it");
}

void App::addHit (int t, double beat, float vel)
{
    auto track = session->track (t);
    const auto bar = (double) juce::jmax (1, session->beatsPerBar());
    const auto past = beat + 1.0 / 1024.0; // a clip's end is open: reach just past the hit
    juce::ValueTree clip;
    double reach = past;

    if (recording)
    {
        // Into this pass's clip, made with its first hit from where the pass began.
        auto& pass = midiPassClips[t];

        if (! pass.isValid() || ! pass.getParent().isValid())
        {
            const auto start = std::min (midiPassStart, beat);
            pass = Session::makeHitClip (start, start, past - start);
            track.appendChild (pass, um());
        }

        clip = pass;
    }
    else
    {
        const auto list = Session::hitClips (track);

        // The clip on top here.
        for (auto it = list.rbegin(); it != list.rend() && ! clip.isValid(); ++it)
            if ((double) (*it)[ids::beat] <= beat + 1.0e-9 && Session::clipEnd (*it) > beat + 1.0e-9)
                clip = *it;

        if (! clip.isValid())
        {
            // Else one ending in this bar before the hit grows to the bar's end, so entering
            // steps bar after bar keeps one clip; else a new clip the length of the bar.
            const auto barStart = std::floor (beat / bar + 1.0e-9) * bar;
            reach = barStart + bar;

            for (auto it = list.rbegin(); it != list.rend() && ! clip.isValid(); ++it)
                if (const auto e = Session::clipEnd (*it); e >= barStart - 1.0e-9 && e <= beat + 1.0e-9)
                    clip = *it;

            if (! clip.isValid())
            {
                clip = Session::makeHitClip (barStart, barStart, bar);
                track.appendChild (clip, um());
            }
        }
    }

    Session::growMidiClip (clip, beat, reach, um());
    clip.appendChild (Session::makeHit (beat - Session::contentZero (clip), vel), um());
}

Result App::consolidate (const std::vector<int>& tracks)
{
    if (auto r = checkEditable (tracks, true); ! r.ok)
        return r;

    const auto range = timeSelection();
    const auto from = snapBeats (range.getStart()), to = snapBeats (range.getEnd());
    juce::StringArray made, skipped;

    for (auto t : tracks)
    {
        const auto track = session->track (t);
        auto holder = clipHolder (t); // the playlist lane you're on, else the track
        const auto label = juce::String (t + 1);

        if (Session::isBusTrack (track))
        {
            skipped.add (label + " (a bus)");
            continue;
        }

        if (Session::isInstrumentTrack (track) || Session::isSampleTrack (track))
        {
            // Everything that plays in the range, in one clip of exactly the range. Notes keep
            // the length they played for (cut off where their clip or the range ended).
            auto clip = Session::isSampleTrack (track) ? Session::makeHitClip (from, 0.0, to - from) : Session::makeMidiClip (from, 0.0, to - from);

            if (Session::isSampleTrack (track))
            {
                for (const auto& h : Session::placedHits (holder))
                    if (hitInRange (h.beat, from, to))
                        clip.appendChild (Session::makeHit (h.beat - from, h.vel), nullptr);
            }
            else
            {
                for (const auto& n : Session::placedNotes (holder))
                    if (hitInRange (n.start, from, to))
                    {
                        auto copy = n.node.createCopy();
                        copy.setProperty (ids::beat, n.start - from, nullptr).setProperty (ids::len, std::min (n.end, to) - n.start, nullptr);
                        clip.appendChild (copy, nullptr);
                    }

                for (const auto& c : Session::placedControls (holder))
                    if (hitInRange (c.at, from, to))
                        clip.appendChild (c.node.createCopy().setProperty (ids::beat, c.at - from, nullptr), nullptr);
            }

            const auto holderClips = Session::beatClips (holder);

            if (std::none_of (holderClips.begin(), holderClips.end(), [&] (const juce::ValueTree& c) { return (double) c[ids::beat] < to && Session::clipEnd (c) > from; }))
            {
                skipped.add (label + " (empty)");
                continue;
            }

            clearRange (t, range);
            holder.appendChild (clip, um());
            made.add (label);
            continue;
        }

        // Audio: what plays in the range rendered to a new file (silence where nothing does),
        // later clips over earlier ones as they play. The old files stay in audio/.
        const auto a = range.getStart(), b = range.getEnd();
        struct Piece { const ClipData* data; juce::int64 start, offset, length; };
        std::vector<Piece> pieces;
        int channels = 1;

        for (const auto& c : Session::clips (holder))
            if (const auto s = (juce::int64) c[ids::start], e = s + (juce::int64) c[ids::length]; s < b && e > a)
                if (const auto* data = clipFor (c))
                {
                    pieces.push_back ({ data, s, (juce::int64) c[ids::offset], e - s });
                    channels = std::max (channels, data->audio->getNumChannels());
                }

        if (pieces.empty())
        {
            skipped.add (label + " (no audio there)");
            continue;
        }

        juce::AudioBuffer<float> out (channels, (int) (b - a));
        out.clear();

        for (const auto& pc : pieces)
        {
            const auto& src = *pc.data->audio;
            const auto lo = std::max (pc.start, a), hi = std::min (pc.start + pc.length, b);
            const auto srcAt = pc.offset + (lo - pc.start);
            const auto n = (int) std::min<juce::int64> (hi - lo, (juce::int64) src.getNumSamples() - srcAt);

            if (n <= 0 || srcAt < 0)
                continue;

            for (int ch = 0; ch < channels; ++ch)
                out.copyFrom (ch, (int) (lo - a), src, std::min (ch, src.getNumChannels() - 1), (int) srcAt, n);
        }

        const auto base = juce::String (t + 1).paddedLeft ('0', 2) + "-"
                          + juce::File::createLegalFileName (track[ids::name].toString()).replaceCharacter (' ', '_') + "-cons";
        const auto file = session->audioFolder().getNonexistentChildFile (base, ".wav", false);
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto writer = juce::WavAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                                           .withSampleRate (sampleRate())
                                                                           .withNumChannels (channels)
                                                                           .withBitsPerSample (32)
                                                                           .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));

        if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (out, 0, out.getNumSamples()))
        {
            writer.reset();
            file.deleteFile();
            skipped.add (label + " (couldn't write " + file.getFileName() + ")");
            continue;
        }

        writer.reset();
        clearRange (t, range);
        holder.appendChild (Session::makeClip (file.getRelativePathFrom (session->folder()).replaceCharacter ('\\', '/'), a, 0, b - a), um());
        made.add (label);
    }

    if (made.isEmpty())
        return Result::fail ("nothing consolidated: " + skipped.joinIntoString ("; "));

    auto msg = "consolidated " + formatBars (range.getStart()) + L" \x2192 " + formatBars (range.getEnd()) + " into one clip on "
               + (made.size() == 1 ? "track " : "tracks ") + made.joinIntoString (",") + L" \x00b7 ctrl+z undoes";

    if (! skipped.isEmpty())
        return Result::fail (msg + L" \x00b7 skipped " + skipped.joinIntoString ("; "));

    return Result::good (msg);
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
