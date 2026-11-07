#include "App.h"

#include <algorithm>

namespace taw
{
//==============================================================================
// Removing tracks: "2 remove", or Delete in the track menu. One undo step brings them back,
// plugins and all (the instances are kept until then, only their windows close).

Result App::removeTracks (std::vector<int> tracks)
{
    if (session == nullptr)
        return Result::fail ("no session open");

    if (recording)
        return Result::fail (L"not while recording \x2014 stop first");

    std::sort (tracks.begin(), tracks.end());
    tracks.erase (std::unique (tracks.begin(), tracks.end()), tracks.end());
    tracks.erase (std::remove_if (tracks.begin(), tracks.end(), [this] (int t) { return t < 0 || t >= session->numTracks(); }), tracks.end());

    if (tracks.empty())
        return Result::fail (L"which track? shift+T on one (\x2191\x2193), or 2 remove");

    std::vector<juce::ValueTree> nodes;
    juce::StringArray names;

    for (auto t : tracks)
    {
        nodes.push_back (session->track (t));
        names.add (nodes.back()[ids::name].toString());
    }

    // A bus going away: what went into it goes back to the mix, and sends to it go.
    for (const auto& node : nodes)
    {
        if (! Session::isBusTrack (node))
            continue;

        const int id = node[ids::busId];

        for (int o = 0; o < session->numTracks(); ++o)
        {
            auto other = session->track (o);

            if ((int) other[ids::out] == id)
                other.removeProperty (ids::out, um());

            for (int c = other.getNumChildren(); --c >= 0;)
                if (other.getChild (c).hasType (ids::SEND) && (int) other.getChild (c)[ids::to] == id)
                    other.removeChild (c, um());
        }
    }

    for (auto& in : hosted)
        if (in.slot != nullptr && in.slot->editorShowing()
            && std::any_of (nodes.begin(), nodes.end(), [&] (const auto& n) { return in.node == n || in.node.getParent() == n; }))
        {
            in.slot->closeEditor();
            writePluginState (in);
        }

    for (const auto& node : nodes)
        session->tree().removeChild (node, um());

    clearSelection();
    ioCell = -1;
    focused = std::min (tracks.front(), std::max (0, session->numTracks() - 1));

    return Result::good ("removed " + trackList (tracks) + " " + names.joinIntoString (", ") + L" \x00b7 ctrl+z brings "
                         + (tracks.size() == 1 ? "it" : "them") + " back");
}

//==============================================================================
// Duplicating tracks: "2 dup track" (or "2 dup track empty"), or u in the track menu. Each copy
// goes right under its original with the same plugins (as they're set now, in state files of
// its own), inserts, sends, output, volume, pan and automation. Its main playlist is a copy of
// the original's, or empty; the other playlists stay with the original. One undo step.

Result App::duplicateTracks (std::vector<int> tracks, bool withClips)
{
    if (session == nullptr)
        return Result::fail ("no session open");

    if (recording)
        return Result::fail (L"not while recording \x2014 stop first");

    std::sort (tracks.begin(), tracks.end());
    tracks.erase (std::unique (tracks.begin(), tracks.end()), tracks.end());
    tracks.erase (std::remove_if (tracks.begin(), tracks.end(), [this] (int t) { return t < 0 || t >= session->numTracks(); }), tracks.end());

    if (tracks.empty())
        return Result::fail (L"which track? shift+T on one (\x2191\x2193), or 2 dup track");

    auto nameTaken = [this] (const juce::String& name)
    {
        for (int t = 0; t < session->numTracks(); ++t)
            if (session->track (t)[ids::name].toString().equalsIgnoreCase (name))
                return true;

        return false;
    };

    juce::StringArray reserved; // state files named in this go, not written yet
    std::vector<int> made;

    // A plugin's settings as they are now (its window may have changed them since the last
    // save) into a state file of the copy's own.
    auto copyState = [&] (juce::ValueTree node, const juce::ValueTree& from, int index)
    {
        juce::MemoryBlock state;
        bool have = false;

        for (auto& in : hosted)
            if (in.node == from && in.slot != nullptr)
            {
                state = in.slot->getState();
                have = true;
            }

        if (const auto path = from[ids::pluginState].toString(); ! have && path.isNotEmpty())
            have = pluginStateFile (path, false).loadFileAsData (state);

        const auto base = juce::String (index + 1).paddedLeft ('0', 2) + "-"
                          + juce::File::createLegalFileName (from[ids::pluginName].toString()).replaceCharacter (' ', '_');
        auto rel = juce::String();

        for (int n = 1; rel.isEmpty() || reserved.contains (rel); ++n)
            rel = newPluginStateFile (n > 1 ? base + "_" + juce::String (n) : base).getRelativePathFrom (session->folder()).replaceCharacter ('\\', '/');

        reserved.add (rel);
        node.setProperty (ids::pluginState, rel, nullptr);

        if (have)
        {
            const auto file = pluginStateFile (rel, true);
            file.getParentDirectory().createDirectory();
            file.replaceWithData (state.getData(), state.getSize());
        }
    };

    // Copies go in from the top, so each one moves the originals below it down a place.
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        const auto index = tracks[i] + (int) i;
        const auto original = session->track (index);
        auto copy = original.createCopy();

        for (int c = copy.getNumChildren(); --c >= 0;)
            if (const auto child = copy.getChild (c); child.hasType (ids::PLAYLIST) || (! withClips && Session::isClip (child)))
                copy.removeChild (c, nullptr);

        // Its main playlist takes its name; armed too it would record the same input twice.
        copy.removeProperty (ids::playlist, nullptr);
        copy.removeProperty (ids::playlists, nullptr);
        copy.setProperty (ids::armed, false, nullptr);

        // Pro Tools' names: Vox.dup1, Vox.dup2...
        const auto base = original[ids::name].toString().upToLastOccurrenceOf (".dup", false, false);
        auto name = juce::String();

        for (int n = 1; name.isEmpty() || nameTaken (name); ++n)
            name = base + ".dup" + juce::String (n);

        copy.setProperty (ids::name, name, nullptr);

        // A bus copy is a bus of its own: what goes into the original stays there.
        if (Session::isBusTrack (copy))
            copy.setProperty (ids::busId, session->nextBusId(), nullptr);

        if (Session::isInstrumentTrack (copy) && original[ids::pluginState].toString().isNotEmpty())
            copyState (copy, original, index + 1);

        for (int k = 0; k < Session::numSends; ++k)
            if (auto ins = Session::insert (copy, k); ins.isValid())
                copyState (ins, Session::insert (original, k), index + 1);

        session->tree().addChild (copy, session->tree().indexOf (original) + 1, um());
        made.push_back (index + 1);
    }

    clearSelection();
    ioCell = -1;
    focusTrack (made.front());

    const auto playlist = withClips ? juce::String ("its clips") : juce::String ("an empty playlist");
    return Result::good ("duplicated " + trackList (tracks) + L" \x2192 " + trackList (made) + " with " + playlist + L" \x00b7 ctrl+z undoes");
}

//==============================================================================
// Moving tracks: alt+up/down swaps the selected tracks (or the focused one) with the track
// next to them, so they move as a block. The focus and selection go with them.

Result App::moveTracks (int direction)
{
    if (session == nullptr)
        return Result::fail ("no session open");

    if (recording)
        return Result::fail (L"not while recording \x2014 stop first");

    if (onTopLane() || mixFocused())
        return Result::fail (L"alt+\x2191\x2193 moves a track \x2014 \x2193 to one");

    auto tracks = selectedTracks();

    if (tracks.empty())
        tracks = { focused };

    const int first = tracks.front(), last = tracks.back();
    const int other = direction < 0 ? first - 1 : last + 1;

    if (other < 0 || other >= session->numTracks())
        return Result::fail (direction < 0 ? "already at the top" : "already at the bottom");

    // The neighbour hops over the block, to just after it (moving up) or just before it.
    auto& tree = session->tree();
    const int from = tree.indexOf (session->track (other));
    const int to = tree.indexOf (session->track (direction < 0 ? last : first));

    beginEdit ("move tracks");
    tree.moveChild (from, to, um());

    focused += direction;

    if (trackAnchor >= 0)
        trackAnchor += direction;

    std::vector<int> now;

    for (auto t : tracks)
        now.push_back (t + direction);

    return Result::good ("moved " + trackList (tracks) + (direction < 0 ? " up" : " down") + L" \x2192 " + trackList (now).fromFirstOccurrenceOf (" ", false, false));
}

//==============================================================================
// The track menu (shift+T): what you can do to the focused track, or the selected ones. Each
// item has a letter, and shows the command it stands for, so the menu teaches the typed way.

std::vector<App::TrackMenuItem> App::trackMenuItems()
{
    std::vector<TrackMenuItem> items;

    if (session == nullptr || trackMenuTracks.empty())
        return items;

    juce::StringArray numbers;

    for (auto t : trackMenuTracks)
        numbers.add (trackLabel (t));

    const auto sel = numbers.joinIntoString (",");
    const bool one = trackMenuTracks.size() == 1;
    const int first = trackMenuTracks.front();
    const auto track = session->track (first);
    const bool bus = std::any_of (trackMenuTracks.begin(), trackMenuTracks.end(), [this] (int t) { return Session::isBusTrack (session->track (t)); });
    const bool audioOrSample = std::all_of (trackMenuTracks.begin(), trackMenuTracks.end(), [this] (int t)
                                            {
                                                const auto tr = session->track (t);
                                                return Session::isAudioTrack (tr) || Session::isSampleTrack (tr);
                                            });

    // Something to type: the menu closes and the command waits for the rest.
    auto typeIn = [this] (juce::String start)
    {
        return [this, start]
        {
            commandLine = start;
            return Result::good();
        };
    };

    // Duplicating: the copy's main playlist, the original's clips or none.
    if (trackMenuDuplicating)
    {
        items.push_back ({ 'c', "Copy the main playlist", sel + " dup track", [this, sel] { return execute (sel + " dup track"); } });
        items.push_back ({ 'e', "New empty playlist", sel + " dup track empty", [this, sel] { return execute (sel + " dup track empty"); } });
        return items;
    }

    items.push_back ({ 'n', one ? "Rename" : "Name them", sel + " n <name>", typeIn (sel + " n ") });

    if (! bus)
        items.push_back ({ 'i', "Input", sel + " i <input>", typeIn (sel + " i ") });

    if (one)
        items.push_back ({ 'o', "Output", sel + " out <bus>", [this, first]
                           {
                               openBusPicker (first, 0);
                               return Result::good();
                           } });

    if (one)
        items.push_back ({ 'e', "Add an effect", sel + " ins <name>", [this, first]
                           {
                               const auto t = session->track (first);

                               for (int k = 0; k < Session::numSends; ++k)
                                   if (! Session::insert (t, k).isValid())
                                   {
                                       openPluginPicker (first, k);
                                       return Result::good();
                                   }

                               return Result::fail ("track " + trackLabel (first) + L"'s five inserts are full \x2014 \\ then del removes one");
                           } });

    if (one && (Session::isInstrumentTrack (track) || Session::insert (track, 0).isValid() || Session::insert (track, 1).isValid()
                || Session::insert (track, 2).isValid() || Session::insert (track, 3).isValid() || Session::insert (track, 4).isValid()))
        items.push_back ({ 'p', "Plugin window", "|", [this, first]
                           {
                               focused = first;
                               ioCell = -1;
                               return togglePluginHere();
                           } });

    if (! bus)
    {
        const bool stereo = trackIsStereo (track);
        items.push_back ({ 'm', stereo ? "Make mono" : "Make stereo", sel + (stereo ? " mono" : " stereo"),
                           [this, sel, stereo] { return execute (sel + (stereo ? " mono" : " stereo")); } });
    }

    if (audioOrSample)
    {
        const bool sample = Session::isSampleTrack (track);
        items.push_back ({ 'c', sample ? "Convert to audio" : "Convert to sample", sel + (sample ? " audio" : " sample"),
                           [this, sel, sample] { return execute (sel + (sample ? " audio" : " sample")); } });
    }

    // The menu stays up and asks what the copy starts with.
    items.push_back ({ 'u', one ? L"Duplicate\x2026" : L"Duplicate them\x2026", sel + " dup track", [this]
                       {
                           trackMenuDuplicating = trackMenuVisible = true;
                           trackMenuIndex = 0;
                           return Result::good();
                       } });

    items.push_back ({ 'd', one ? "Delete track" : "Delete tracks", sel + " remove",
                       [this, sel] { return execute (sel + " remove"); }, true });

    return items;
}

Result App::openTrackMenu()
{
    if (session == nullptr || onTopLane() || mixFocused() || session->numTracks() == 0)
        return Result::fail (L"shift+T: a track's menu \x2014 \x2191\x2193 to one");

    trackMenuTracks = defaultTargets();
    trackMenuIndex = 0;
    trackMenuDuplicating = false;
    trackMenuVisible = ! trackMenuTracks.empty();
    return Result::good();
}

bool App::handleTrackMenuKey (const Key& k)
{
    if (! trackMenuVisible)
        return false;

    const auto items = trackMenuItems();
    const auto n = (int) items.size();

    auto run = [&] (int i)
    {
        trackMenuVisible = trackMenuDuplicating = false;

        if (i >= 0 && i < n)
            say (items[(size_t) i].run());
    };

    switch (k.code)
    {
        case Key::Up:     trackMenuIndex = juce::jmax (0, trackMenuIndex - 1); return true;
        case Key::Down:   trackMenuIndex = juce::jmax (0, juce::jmin (n - 1, trackMenuIndex + 1)); return true;
        case Key::Escape: trackMenuVisible = false; return true;
        case Key::Enter:  run (trackMenuIndex); return true;
        case Key::Delete:
            for (int i = 0; i < n; ++i)
                if (items[(size_t) i].danger)
                    run (i);
            return true;
        case Key::Char:
            if (k.ctrl || k.alt)
                return false;

            if (k.ch == 'T')
            {
                trackMenuVisible = false;
                return true;
            }

            for (int i = 0; i < n; ++i)
                if (items[(size_t) i].key == (juce::juce_wchar) k.ch)
                {
                    run (i);
                    return true;
                }
            return true;
        default:
            return true;
    }
}
} // namespace taw
