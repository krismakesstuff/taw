// Inserts: five effect slots per track (A to E), run after the track's source (and its
// instrument), before its sends and fader. Each is an INSERT in the track holding the same
// plugin, pluginName and pluginState an instrument track has.

#include "App.h"

namespace taw
{
namespace
{
int slotOf (const juce::String& token) // "b" -> 1, else -1
{
    const auto t = token.toLowerCase();
    return t.length() == 1 && t[0] >= 'a' && t[0] < 'a' + Session::numSends ? (int) (t[0] - 'a') : -1;
}
} // namespace

void App::addInserts (TrackPlay& tp, const juce::ValueTree& track, bool offline)
{
    for (int s = 0; s < Session::numSends; ++s)
    {
        const auto node = Session::insert (track, s);

        if (! node.isValid() || (bool) node[ids::bypass])
            continue;

        auto* in = hostedEntry (node);

        if (in == nullptr || in->slot == nullptr)
            continue;

        if (! offline)
        {
            tp.inserts.push_back (in->slot);
            continue;
        }

        // A bounce plays its own copy, so the live one keeps running meanwhile.
        juce::String problem;

        if (auto copy = createPlugin (node, sampleRate(), 8192, problem))
        {
            copy->setState (in->slot->getState());
            copy->setNonRealtime (true);
            tp.inserts.push_back (std::move (copy));
        }
    }
}

void App::dropPlugin (const juce::ValueTree& node)
{
    for (auto it = hosted.begin(); it != hosted.end(); ++it)
        if (it->node == node)
        {
            writePluginState (*it);

            if (it->slot != nullptr)
                it->slot->closeEditor();

            hosted.erase (it);
            return;
        }
}

juce::String App::insertProblem (const juce::ValueTree& node) const
{
    for (const auto& in : hosted)
        if (in.node == node)
            return in.slot == nullptr ? in.problem : juce::String();

    return {};
}

//==============================================================================
Result App::loadEffect (int index, int slot, const juce::PluginDescription& desc)
{
    if (session == nullptr || index < 0 || index >= session->numTracks())
        return Result::fail ("no such track");

    if (recording)
        return Result::fail ("stop recording first");

    const auto rate = engine.deviceSampleRate() > 0.0 ? engine.deviceSampleRate() : sampleRate();
    juce::String error;
    auto plugin = plugins.create (desc, rate, engine.maxBlockSize(), error, trackIsStereo (session->track (index)) ? 2 : 1);

    if (plugin == nullptr)
        return Result::fail (error);

    beginEdit ("fx " + desc.name);
    auto track = session->track (index);

    // Replacing: the old one's state is kept on disk, so undo brings it back.
    if (auto old = Session::insert (track, slot); old.isValid())
    {
        dropPlugin (old);
        track.removeChild (old, um());
    }

    const auto base = juce::String (index + 1).paddedLeft ('0', 2) + "-" + juce::File::createLegalFileName (desc.name).replaceCharacter (' ', '_');
    const auto stateFile = session->folder().getChildFile ("plugins").getNonexistentChildFile (base, ".state", false);

    juce::ValueTree node (ids::INSERT);
    node.setProperty (ids::slot, Session::slotName (slot).toLowerCase(), nullptr)
        .setProperty (ids::plugin, desc.createIdentifierString(), nullptr)
        .setProperty (ids::pluginName, desc.name, nullptr)
        .setProperty (ids::pluginState, stateFile.getRelativePathFrom (session->folder()).replaceCharacter ('\\', '/'), nullptr);
    track.appendChild (node, um());

    hosted.push_back ({ node, std::move (plugin), {}, {} });
    rebuildGraph();

    // Like Pro Tools: a new plugin opens its window, in place of any other.
    if (console.isOpen())
        toggleInsertEditor (index, slot);

    return Result::good ("track " + juce::String (index + 1) + " insert " + Session::slotName (slot) + ": " + desc.name
                         + L" \x00b7 enter on it opens its window \x00b7 M bypasses \x00b7 shift+\x2190\x2192 moves it");
}

Result App::removeInsert (int index, int slot)
{
    auto track = session->track (index);
    const auto node = Session::insert (track, slot);

    if (! node.isValid())
        return Result::fail ("insert " + Session::slotName (slot) + " is empty");

    const auto name = node[ids::pluginName].toString();
    dropPlugin (node);
    track.removeChild (node, um());
    return Result::good ("track " + juce::String (index + 1) + " insert " + Session::slotName (slot) + ": removed " + name
                         + L" \x00b7 ctrl+z brings it back");
}

Result App::toggleInsertEditor (int index, int slot)
{
    const auto track = session->track (index);
    const auto node = Session::insert (track, slot);

    if (! node.isValid())
        return Result::fail ("insert " + Session::slotName (slot) + " is empty");

    auto* in = hostedEntry (node);

    if (in == nullptr || in->slot == nullptr)
        return Result::fail ("insert " + Session::slotName (slot) + ": " + (in != nullptr ? in->problem : juce::String ("didn't load")));

    if (in->slot->editorShowing())
    {
        in->slot->closeEditor();
        writePluginState (*in);
        return Result::good ("closed " + in->slot->name());
    }

    // One plugin window at a time: it follows you from slot to slot.
    for (auto& other : hosted)
        if (other.slot != nullptr && other.slot->editorShowing())
        {
            other.slot->closeEditor();
            writePluginState (other);
        }

    in->slot->showEditor (juce::String (index + 1) + " " + track[ids::name].toString() + L" \x00b7 " + Session::slotName (slot) + L" \x00b7 "
                          + in->slot->name() + L" \x00b7 taw");
    return Result::good (in->slot->name() + L" window open \x00b7 enter again (or its close button) closes it");
}

void App::followWindow (int index, int slot)
{
    const bool anyOpen = std::any_of (hosted.begin(), hosted.end(), [] (const auto& in) { return in.slot != nullptr && in.slot->editorShowing(); });

    if (! anyOpen || slot < 0 || index < 0 || index >= session->numTracks())
        return;

    const auto node = Session::insert (session->track (index), slot);

    if (! node.isValid())
        return;

    if (auto* in = hostedEntry (node); in != nullptr && in->slot != nullptr && ! in->slot->editorShowing())
        toggleInsertEditor (index, slot);
}

Result App::swapInserts (int index, int slot, int other)
{
    if (other < 0 || other >= Session::numSends || other == slot)
        return Result::fail ("inserts run A to E");

    const auto track = session->track (index);
    auto a = Session::insert (track, slot), b = Session::insert (track, other);

    if (! a.isValid() && ! b.isValid())
        return Result::good();

    if (a.isValid())
        a.setProperty (ids::slot, Session::slotName (other).toLowerCase(), um());

    if (b.isValid())
        b.setProperty (ids::slot, Session::slotName (slot).toLowerCase(), um());

    juce::StringArray chain;

    for (int s = 0; s < Session::numSends; ++s)
        if (const auto n = Session::insert (track, s); n.isValid())
            chain.add (Session::slotName (s) + " " + n[ids::pluginName].toString());

    return Result::good ("track " + juce::String (index + 1) + L" inserts: " + chain.joinIntoString (juce::String (L" \x2192 ")));
}

Result App::toggleBypass (int index, int slot)
{
    auto node = Session::insert (session->track (index), slot);

    if (! node.isValid())
        return Result::fail ("insert " + Session::slotName (slot) + " is empty");

    node.setProperty (ids::bypass, ! (bool) node[ids::bypass], um());
    return Result::good ("insert " + Session::slotName (slot) + " " + node[ids::pluginName].toString() + ((bool) node[ids::bypass] ? " bypassed" : " on"));
}

// fx                 the effects picker, for the first free slot
// fx valhalla        into the first free slot
// fx b | fx b valhalla | fx b x | fx b off | fx b on | fx b>d
//                    open/close slot B's window (empty: the picker), load into it, remove it,
//                    bypass it, or swap it with D
Result App::editInserts (const std::vector<int>& tracks, const juce::StringArray& argsIn)
{
    if (tracks.empty())
        return Result::fail (L"fx works on a track \x2014 \x2193 to one, or 2 fx");

    auto args = argsIn;
    const auto joined = args.joinIntoString ("").removeCharacters (" ").toLowerCase();

    // b>d: swap.
    if (joined.length() == 3 && joined[1] == '>' && slotOf (joined.substring (0, 1)) >= 0 && slotOf (joined.substring (2)) >= 0)
    {
        Result last;

        for (auto t : tracks)
            last = swapInserts (t, slotOf (joined.substring (0, 1)), slotOf (joined.substring (2)));

        return last;
    }

    int slot = args.isEmpty() ? -1 : slotOf (args[0]);

    if (slot >= 0)
        args.remove (0);

    const auto rest = args.joinIntoString (" ").trim();
    const auto first = tracks.front();

    if (slot >= 0 && (rest == "x" || rest == "del"))
    {
        Result last;

        for (auto t : tracks)
            last = removeInsert (t, slot);

        return last;
    }

    if (slot >= 0 && (rest == "off" || rest == "on"))
    {
        for (auto t : tracks)
            if (auto node = Session::insert (session->track (t), slot); node.isValid())
                node.setProperty (ids::bypass, rest == "off", um());

        return Result::good (trackList (tracks) + " insert " + Session::slotName (slot) + (rest == "off" ? " bypassed" : " on"));
    }

    // No slot: the first free one (on the first track named).
    if (slot < 0)
    {
        for (int s = 0; s < Session::numSends && slot < 0; ++s)
            if (! Session::insert (session->track (first), s).isValid())
                slot = s;

        if (slot < 0)
            return Result::fail ("track " + juce::String (first + 1) + L"'s inserts are all used \x2014 2 fx e <name> replaces E");
    }
    else if (rest.isEmpty())
    {
        if (Session::insert (session->track (first), slot).isValid())
            return toggleInsertEditor (first, slot);
    }

    if (rest.isEmpty())
    {
        openPluginPicker (first, slot);
        return Result::good (plugins.hasScanned() ? juce::String() : juce::String (L"scanning plugins (first time)\x2026"));
    }

    if (! plugins.hasScanned())
        return Result::fail (L"no plugins scanned yet \x2014 type scan (takes a moment), then 2 fx " + rest);

    const auto desc = plugins.findEffect (rest, ! trackIsStereo (session->track (first)));

    if (! desc)
        return Result::fail ("no effect matches " + rest.quoted() + L" \x00b7 2 fx alone lists them \x00b7 scan finds new ones");

    Result last;

    for (auto t : tracks)
        if (last = loadEffect (t, slot, *desc); ! last.ok)
            return last;

    return last;
}
} // namespace taw
