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
// The track menu (shift+T): what you can do to the focused track, or the selected ones. Each
// item has a letter, and shows the command it stands for, so the menu teaches the typed way.

std::vector<App::TrackMenuItem> App::trackMenuItems()
{
    std::vector<TrackMenuItem> items;

    if (session == nullptr || trackMenuTracks.empty())
        return items;

    juce::StringArray numbers;

    for (auto t : trackMenuTracks)
        numbers.add (juce::String (t + 1));

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
        items.push_back ({ 'e', "Add an effect", sel + " fx <name>", [this, first]
                           {
                               const auto t = session->track (first);

                               for (int k = 0; k < Session::numSends; ++k)
                                   if (! Session::insert (t, k).isValid())
                                   {
                                       openPluginPicker (first, k);
                                       return Result::good();
                                   }

                               return Result::fail ("track " + juce::String (first + 1) + L"'s five inserts are full \x2014 \\ then del removes one");
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
        trackMenuVisible = false;

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
