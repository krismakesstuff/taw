// Playlists, as in Pro Tools: other versions of a track's clips. The track's own clips are its
// main playlist, the one that plays and records; the others are PLAYLIST children, each holding
// clips the way a track does, so whatever reads a track's clips reads one of them the same way.
// shift+P shows them as lanes under the track, where one can be soloed in the main's place, its
// selection copied to the main, or edited like a track. Loop recording puts each pass on one.

#include "App.h"

namespace taw
{
namespace
{
// The clips straight under a track or playlist (not its sends, inserts or automation).
std::vector<juce::ValueTree> clipsOf (const juce::ValueTree& holder)
{
    std::vector<juce::ValueTree> out;

    for (const auto& c : holder)
        if (Session::isClip (c))
            out.push_back (c);

    return out;
}

void moveClips (juce::ValueTree from, juce::ValueTree to, juce::UndoManager* um)
{
    for (auto c : clipsOf (from))
    {
        from.removeChild (c, um);
        to.appendChild (c, um);
    }
}
} // namespace

std::vector<juce::ValueTree> App::playlistsShown (int track) const
{
    if (session == nullptr || track < 0 || track >= session->numTracks())
        return {};

    const auto t = session->track (track);
    return (bool) t[ids::playlists] ? Session::playlists (t) : std::vector<juce::ValueTree>();
}

int App::lanesUnder (int track) const
{
    return (int) lanesShown (track).size() + (int) playlistsShown (track).size();
}

juce::ValueTree App::focusedPlaylist() const
{
    if (session == nullptr || onTopLane() || focusedLane < 0 || focused >= session->numTracks())
        return {};

    const auto shown = playlistsShown (focused);
    return focusedLane < (int) shown.size() ? shown[(size_t) focusedLane] : juce::ValueTree();
}

juce::ValueTree App::clipHolder (int track) const
{
    if (track == focused)
        if (const auto p = focusedPlaylist(); p.isValid())
            return p;

    return session->track (track);
}

void App::closePlaylistLanes (juce::ValueTree track)
{
    if (! track.hasType (ids::TRACK))
        return;

    track.removeProperty (ids::playlists, nullptr); // a view, not an edit

    for (auto p : Session::playlists (track))
        p.removeProperty (ids::solo, nullptr);
}

// ctrl+p: a new empty playlist becomes the main; ctrl+alt+p: a copy of the main does. Either
// way the old main joins the others, on the focused or selected tracks.
Result App::newPlaylist (bool copy)
{
    if (session == nullptr || onTopLane() || mixFocused() || session->numTracks() == 0)
        return Result::fail (L"playlists are a track's \x2014 \x2193 to one");

    if (recording)
        return Result::fail ("stop recording first");

    beginEdit (copy ? "copy playlist" : "new playlist");
    juce::StringArray made;

    for (auto i : defaultTargets())
    {
        auto track = session->track (i);

        if (Session::isBusTrack (track))
            continue;

        const auto name = Session::nextPlaylistName (track);
        juce::ValueTree old (ids::PLAYLIST, { { ids::name, Session::mainPlaylistName (track) } });
        track.appendChild (old, um());

        if (copy)
            for (const auto& c : clipsOf (track))
                old.appendChild (c.createCopy(), um());
        else
            moveClips (track, old, um());

        track.setProperty (ids::playlist, name, um());
        made.add (trackLabel (i) + " " + name);
    }

    if (made.isEmpty())
        return Result::fail ("a bus has no playlists");

    closeEditorsOnPlaylists();
    const bool shown = (bool) session->track (focused)[ids::playlists];
    return Result::good ((copy ? L"copied the main to " : L"new playlist ") + made.joinIntoString (", ")
                         + (copy ? L" \x00b7 the old main is a playlist now" : L" \x00b7 the old main is a playlist now, ready to record")
                         + (shown ? juce::String() : juce::String (L" \x00b7 shift+P shows them")));
}

// The playlist becomes the main and the main takes its place among the others.
Result App::makeMain (int trackIndex, juce::ValueTree playlist)
{
    auto track = session->track (trackIndex);

    if (! playlist.isValid() || playlist.getParent() != track)
        return Result::fail ("no such playlist");

    const auto name = playlist[ids::name].toString();
    juce::ValueTree old (ids::PLAYLIST, { { ids::name, Session::mainPlaylistName (track) } });
    track.addChild (old, track.indexOf (playlist), um());
    moveClips (track, old, um());
    moveClips (playlist, track, um());
    track.removeChild (playlist, um());

    if (name == track[ids::name].toString())
        track.removeProperty (ids::playlist, um());
    else
        track.setProperty (ids::playlist, name, um());

    closeEditorsOnPlaylists();
    return Result::good ("track " + trackLabel (trackIndex) + " plays " + name + L" \x00b7 " + old[ids::name].toString() + " is a playlist now");
}

// "pl": the focused track's playlists; "pl new", "pl copy"; "pl 2" or "pl Vox.02": that one
// becomes the main (numbered as the lanes are, from the top); "pl del [n]": a playlist goes
// (its files stay in audio\).
Result App::playlistCommand (const juce::StringArray& args)
{
    if (session == nullptr || onTopLane() || mixFocused() || session->numTracks() == 0)
        return Result::fail (L"playlists are a track's \x2014 \x2193 to one");

    const auto what = args[0].toLowerCase();

    if (what == "new" || what == "copy")
        return newPlaylist (what == "copy");

    auto track = session->track (focused);
    const auto list = Session::playlists (track);
    const auto label = "track " + trackLabel (focused);

    // A playlist by lane number or by name (whole, or its ending: "02").
    auto find = [&] (const juce::String& spec) -> juce::ValueTree
    {
        if (spec.containsOnly ("0123456789") && spec.isNotEmpty() && ! spec.startsWith ("0"))
        {
            const auto n = spec.getIntValue();
            return n >= 1 && n <= (int) list.size() ? list[(size_t) n - 1] : juce::ValueTree();
        }

        for (const auto& p : list)
            if (p[ids::name].toString().equalsIgnoreCase (spec) || p[ids::name].toString().endsWithIgnoreCase ("." + spec))
                return p;

        return {};
    };

    if (what.isEmpty())
    {
        juce::StringArray names;

        for (size_t k = 0; k < list.size(); ++k)
            names.add (juce::String ((int) k + 1) + " " + list[k][ids::name].toString() + ((bool) list[k][ids::solo] ? " (soloed)" : ""));

        return Result::good (label + " plays " + Session::mainPlaylistName (track)
                             + (names.isEmpty() ? juce::String (L" \x00b7 no other playlists: ctrl+p makes one, ctrl+alt+p copies the main")
                                                : L" \x00b7 " + names.joinIntoString (juce::String (L" \x00b7 ")) + L" \x00b7 pl <n> makes one the main"));
    }

    if (what == "del" || what == "delete")
    {
        auto p = args[1].isNotEmpty() ? find (args[1]) : focusedPlaylist();

        if (! p.isValid())
            return Result::fail (L"pl del <n>, or on a playlist lane (shift+P shows them)");

        beginEdit ("delete playlist");
        const auto name = p[ids::name].toString();
        track.removeChild (p, um());
        closeEditorsOnPlaylists();
        return Result::good ("deleted playlist " + name + L" (its audio stays in audio\\) \x00b7 ctrl+z brings it back");
    }

    if (recording)
        return Result::fail ("stop recording first");

    const auto p = find (args.joinIntoString (" "));

    if (! p.isValid())
        return Result::fail (label + " has no playlist " + args.joinIntoString (" ") + L" \x00b7 pl lists them");

    beginEdit ("choose playlist");
    return makeMain (focused, p);
}

// shift+P: the focused track's playlists as lanes under it (its automation lanes close).
Result App::togglePlaylistLanes()
{
    if (session == nullptr || onTopLane() || mixFocused() || focused >= session->numTracks())
        return Result::fail (L"shift+P shows a track's playlists \x2014 \x2193 to one");

    auto track = session->track (focused);

    if ((bool) track[ids::playlists])
    {
        closePlaylistLanes (track);
        focusedLane = -1;
        return Result::good (L"playlists closed \x00b7 shift+P shows them");
    }

    track.setProperty (ids::playlists, true, nullptr); // a view, not an edit
    track.setProperty (ids::lanes, false, nullptr);    // never with the automation lanes
    focusedLane = -1;
    const auto n = (int) Session::playlists (track).size();

    if (n == 0)
        return Result::good (Session::mainPlaylistName (track) + L" is the only playlist \x00b7 ctrl+p a new one, ctrl+alt+p a copy of it");

    return Result::good (juce::String (n) + (n == 1 ? " playlist" : " playlists") + L" under " + Session::mainPlaylistName (track)
                         + L" \x00b7 \x2193 onto one: shift+S solos it, alt+shift+\x2191 copies the selection to the main");
}

// shift+S on a playlist lane: it plays in the main's place (one per track), the rest of the
// session as it was. Again: the main plays.
Result App::soloPlaylistLane()
{
    auto p = focusedPlaylist();

    if (! p.isValid())
        return Result::fail (L"shift+S on a playlist lane solos it");

    const bool on = ! (bool) p[ids::solo];

    for (auto other : Session::playlists (p.getParent()))
        other.removeProperty (ids::solo, nullptr); // a monitoring choice, not an edit

    if (on)
        p.setProperty (ids::solo, true, nullptr);

    return Result::good (on ? p[ids::name].toString() + L" plays in the main's place \x00b7 shift+S again: the main"
                            : Session::mainPlaylistName (p.getParent()) + " (the main) plays again");
}

// alt+shift+up on a playlist lane: the selection on it replaces that range on the main
// (Pro Tools' Copy Selection to Main Playlist). With no selection the whole playlist becomes the main.
Result App::promoteToMain()
{
    auto p = focusedPlaylist();

    if (! p.isValid())
        return Result::fail (L"alt+shift+\x2191 on a playlist lane copies its selection to the main (shift+P shows them)");

    if (recording)
        return Result::fail ("stop recording first");

    if (! hasTimeSelection())
    {
        beginEdit ("choose playlist");
        auto r = makeMain (focused, p);
        return r.ok ? Result::good (r.message + L" (no selection: the whole of it)") : r;
    }

    const auto track = session->track (focused);
    const auto range = timeSelection();
    const auto lane = gatherLane (track, p, range);

    beginEdit ("copy to main");
    clearClips (track, track, range);
    pasteLane (track, track, lane, range.getStart(), snapBeats (range.getEnd()) - snapBeats (range.getStart()));

    return Result::good ("copied " + formatBars (range.getStart()) + L" \x2192 " + formatBars (range.getEnd()) + " of " + p[ids::name].toString()
                         + " to the main (" + Session::mainPlaylistName (track) + L") \x00b7 ctrl+z undoes");
}

// The editors show a track's clips from where they were opened: when playlists swap, they close
// rather than show clips that have moved.
void App::closeEditorsOnPlaylists()
{
    if (midiEditor != nullptr && midiEditor->isOpen())
        midiEditor->close();

    if (audioEditor != nullptr && audioEditor->isOpen())
        audioEditor->close();
}

//==============================================================================
// Loop recording

juce::ValueTree App::takePlaylist (juce::ValueTree track, juce::ValueTree take)
{
    juce::ValueTree p (ids::PLAYLIST, { { ids::name, Session::nextPlaylistName (track) } });

    for (const auto& c : clipsOf (track))
        p.appendChild (c.createCopy(), nullptr);

    p.appendChild (take, nullptr); // on top: it plays over what was there
    track.appendChild (p, um());
    return p;
}

void App::startLoopRecording()
{
    loopRecStart = (juce::int64) session->tree()[ids::loopStart];
    loopRecEnd = (juce::int64) session->tree()[ids::loopEnd];

    // Punched in past the loop: the engine won't go round it, so it's an ordinary take.
    if (engine.isRolling() && engine.playhead() >= loopRecEnd)
        return;

    loopRecording = true;
    loopWrapBase = engine.isRolling() ? engine.loopWrapCount() : 0;
    loopPassesDone = 0;
    loopPassTakes.clear();

    if (! engine.isRolling())
        midiPassStart = (double) loopRecStart / session->samplesPerBeat();

    for (const auto& l : live)
        if (! l.finishing)
            loopTakes[l.recorder->file.getRelativePathFrom (session->folder()).replaceCharacter ('\\', '/')] = { loopRecStart, loopRecEnd };
}

double App::recordedBeat (juce::int64 pos, int wraps)
{
    const auto spb = session->samplesPerBeat();
    const auto length = loopRecEnd - loopRecStart;

    if (! loopRecording || length <= 0)
        return juce::jmax (0.0, (double) heardPosition (pos) / spb);

    // Unfolded: as if the loop were laid end to end. Then folded back into the pass it was in.
    const auto heard = heardPosition (pos + (juce::int64) (wraps - loopWrapBase) * length);
    const int pass = juce::jmax (0, (int) std::floor ((double) (heard - loopRecStart) / (double) length));

    while (recording && loopPassesDone < pass)
        splitLoopPass();

    // Late for a pass already split off: the start of this one.
    const auto at = pass < loopPassesDone ? loopRecStart : heard - (juce::int64) pass * length;
    return juce::jmax (0.0, (double) at / spb);
}

void App::pollLoopPasses()
{
    if (session == nullptr || ! recording || ! loopRecording)
        return;

    const int passes = engine.loopWrapCount() - loopWrapBase;

    // A pass is over once its end has been heard: the playhead that far into the next.
    const auto lag = engine.playhead() - heardPosition (engine.playhead());

    if (passes > loopPassesDone && (passes > loopPassesDone + 1 || engine.playhead() - loopRecStart >= lag))
    {
        drainRecordedMidi();

        while (loopPassesDone < passes)
            splitLoopPass();
    }
}

void App::splitLoopPass()
{
    const auto spb = session->samplesPerBeat();
    const auto startBeat = (double) loopRecStart / spb, endBeat = (double) loopRecEnd / spb;

    // Keys held over the loop's end: the note ends there, and goes on from the loop's start.
    for (auto& [key, h] : heldNotes)
    {
        recordMidiNode (Session::makeNote (h.beat, endBeat - h.played, h.pitch, h.vel, h.channel + 1), h.beat, endBeat);
        h.beat = h.played = startBeat;
    }

    // Each track's take this pass: off the main, onto a new playlist of the main plus it.
    for (auto& [t, clip] : midiPassClips)
    {
        auto track = clip.getParent();

        if (! clip.isValid() || ! track.isValid())
            continue;

        Session::growMidiClip (clip, (double) clip[ids::beat], endBeat, nullptr);
        track.removeChild (clip, um());
        loopPassTakes[t].push_back ({ takePlaylist (track, clip), clip });
    }

    midiPassClips.clear();
    midiPassStart = startBeat;
    ++loopPassesDone;
}

// Stopped (or punched out): the pass cut short is dropped, as in Pro Tools, and the last whole
// one moves from its playlist to the main. With no whole pass, it's an ordinary take.
juce::String App::finishLoopRecording()
{
    drainRecordedMidi();

    while (loopPassesDone < engine.loopWrapCount() - loopWrapBase)
        splitLoopPass();

    juce::String note;

    if (loopPassesDone == 0)
    {
        finishHeldNotes (engine.playhead());
    }
    else
    {
        for (auto& [t, clip] : midiPassClips)
            if (auto parent = clip.getParent(); parent.isValid())
                parent.removeChild (clip, um());

        midiPassClips.clear();
        heldNotes.clear();
        juce::StringArray tracks;

        for (auto& [t, takes] : loopPassTakes)
        {
            if (takes.empty())
                continue;

            auto [playlist, take] = takes.back();
            auto track = playlist.getParent();

            if (! track.isValid())
                continue;

            playlist.removeChild (take, um());
            track.appendChild (take, um());
            track.removeChild (playlist, um());
            tracks.add (trackLabel (t));
        }

        if (! tracks.isEmpty())
            note = juce::String (loopPassesDone) + (loopPassesDone == 1 ? " pass" : " passes") + " on " + (tracks.size() == 1 ? "track " : "tracks ")
                   + tracks.joinIntoString (",") + L": the last on the main, the others in playlists (shift+P)";
    }

    loopPassTakes.clear();
    return note;
}

// An audio take recorded looping: one file through every pass, cut into a clip per pass. The
// last whole pass goes on the main, the ones before it onto new playlists; a pass cut short is
// dropped (its audio stays in the file). Recorded within one pass, it's an ordinary take.
juce::String App::placeLoopTake (int trackIndex, const juce::String& rel, juce::int64 start, juce::int64 length, const LoopTake& loop)
{
    auto track = session->track (trackIndex);
    const auto pass = loop.end - loop.start;
    const auto from = start - loop.start; // where the file's first sample is, from the loop's start, unfolded
    const auto whole = pass > 0 ? (int) ((from + length) / pass) : 0;

    if (whole == 0)
    {
        const auto offset = start < 0 ? -start : (juce::int64) 0;
        track.appendChild (Session::makeClip (rel, start + offset, offset, length - offset), um());
        return {};
    }

    for (int k = 0; k < whole; ++k)
    {
        const auto a = juce::jmax<juce::int64> (0, k * pass - from), b = juce::jmin (length, (k + 1) * pass - from);

        if (b <= a)
            continue;

        auto clip = Session::makeClip (rel, loop.start + from + a - k * pass, a, b - a);

        if (k < whole - 1)
            takePlaylist (track, clip);
        else
            track.appendChild (clip, um());
    }

    return juce::String (whole) + (whole == 1 ? " pass" : " passes") + L": the last on the main, the others in playlists (shift+P)";
}
} // namespace taw
