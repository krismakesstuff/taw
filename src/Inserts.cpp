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

        auto* in = offline ? hostedEntry (node) : liveEntry (node);

        if (in == nullptr || in->slot == nullptr)
            continue;

        // Low latency mode: plugins with more latency than the limit sit out (never in a bounce).
        if (! offline && lowLatency && in->slot->latency() > (int) session->tree()[ids::lowLatencyMax])
            continue;

        const auto prefix = "fx " + Session::slotName (s).toLowerCase() + "/";

        if (! offline || handOffPlugins) // live, or closing (the bounce takes the session's own)
        {
            tp.inserts.push_back (in->slot);
            addParameterCurves (tp, track, in->slot, prefix);
            continue;
        }

        // A bounce plays its own copy, so the live one keeps running meanwhile.
        juce::String problem;

        if (auto copy = createPlugin (node, sampleRate(), 8192, problem))
        {
            copy->setState (in->slot->getState());
            copy->setNonRealtime (true);
            addParameterCurves (tp, track, copy, prefix);
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

bool App::insertSittingOut (const juce::ValueTree& node) const
{
    if (! lowLatency)
        return false;

    for (const auto& in : hosted)
        if (in.node == node)
            return in.slot != nullptr && in.slot->latency() > (int) session->tree()[ids::lowLatencyMax];

    return false;
}

juce::String App::insertProblem (const juce::ValueTree& node) const
{
    for (const auto& in : hosted)
        if (in.node == node)
            return in.slot == nullptr ? in.problem : juce::String();

    return {};
}

//==============================================================================
juce::String App::insertOwnerName (int track) const
{
    return track < 0 ? juce::String ("the mix") : "track " + trackLabel (track);
}

juce::String App::describeInserts (int track) const
{
    juce::StringArray parts;

    for (int s = 0; s < Session::numSends; ++s)
        if (const auto insert = Session::insert (automationOwner (track), s); insert.isValid())
            parts.add (Session::slotName (s) + " " + insert[ids::pluginName].toString() + ((bool) insert[ids::bypass] ? " (bypassed)" : ""));

    return parts.joinIntoString (juce::String (L" \x00b7 "));
}

std::vector<int> App::insertsMeant (int track, int slot, const juce::String& words, juce::String& error) const
{
    const auto owner = automationOwner (track);
    const auto prefix = routingPrefix (track);
    std::vector<int> all, matching;

    for (int s = 0; s < Session::numSends; ++s)
        if (const auto insert = Session::insert (owner, s); insert.isValid())
        {
            all.push_back (s);

            const auto name = insert[ids::pluginName].toString().toLowerCase();
            bool every = words.isNotEmpty();

            for (const auto& w : juce::StringArray::fromTokens (words.toLowerCase(), " ", ""))
                every = every && name.contains (w);

            if (every)
                matching.push_back (s);
        }

    if (slot >= 0)
    {
        if (std::find (all.begin(), all.end(), slot) == all.end())
            error = insertOwnerName (track) + " insert " + Session::slotName (slot) + " is empty";

        return error.isEmpty() ? std::vector<int> { slot } : std::vector<int>();
    }

    if (all.empty())
        error = insertOwnerName (track) + " has no inserts";
    else if (words.isNotEmpty() && matching.empty())
        error = "no insert on " + insertOwnerName (track) + " matches " + words.quoted() + L" \x00b7 " + describeInserts (track);
    else if (words.isEmpty() && all.size() > 1)
        error = L"which insert? " + describeInserts (track) + L" \x00b7 e.g. " + prefix + " ins " + Session::slotName (all.back()).toLowerCase() + " x";

    return error.isNotEmpty() ? std::vector<int>() : words.isNotEmpty() ? matching : all;
}

Result App::loadEffect (int index, int slot, const juce::PluginDescription& desc)
{
    if (session == nullptr || index < -1 || index >= session->numTracks())
        return Result::fail ("no such track");

    if (recording)
        return Result::fail ("stop recording first");

    const auto rate = engine.deviceSampleRate() > 0.0 ? engine.deviceSampleRate() : sampleRate();
    juce::String error;
    auto track = automationOwner (index);
    auto plugin = plugins.create (desc, rate, engine.maxBlockSize(), error, trackIsStereo (track) ? 2 : 1);

    if (plugin == nullptr)
        return Result::fail (error);

    beginEdit ("fx " + desc.name);

    // Replacing: the old one's state is kept on disk, so undo brings it back.
    if (auto old = Session::insert (track, slot); old.isValid())
    {
        dropPlugin (old);
        track.removeChild (old, um());
    }

    const auto base = (index < 0 ? juce::String ("mix") : juce::String (index + 1).paddedLeft ('0', 2)) + "-" + juce::File::createLegalFileName (desc.name).replaceCharacter (' ', '_');
    const auto stateFile = newPluginStateFile (base);

    juce::ValueTree node (ids::INSERT);
    node.setProperty (ids::slot, Session::slotName (slot).toLowerCase(), nullptr)
        .setProperty (ids::plugin, desc.createIdentifierString(), nullptr)
        .setProperty (ids::pluginName, desc.name, nullptr)
        .setProperty (ids::pluginState, stateFile.getRelativePathFrom (session->folder()).replaceCharacter ('\\', '/'), nullptr);
    track.appendChild (node, um());

    hosted.push_back ({ node, std::move (plugin), {}, {} });
    rebuildGraph();

    // Like Pro Tools: a new plugin opens its window, in place of any other (not when an agent
    // added it: the user didn't ask for it, and a plugin's own dialog would hold taw).
    if (console.isOpen() && ! agentEditing)
        toggleInsertEditor (index, slot);

    const auto cmd = routingPrefix (index) + " ins " + Session::slotName (slot).toLowerCase();
    return Result::good (insertOwnerName (index) + " insert " + Session::slotName (slot) + ": " + desc.name
                         + L" \x00b7 " + cmd + L" x removes it \x00b7 " + cmd + " off bypasses it");
}

Result App::removeInsert (int index, int slot)
{
    auto track = automationOwner (index);
    const auto node = Session::insert (track, slot);

    if (! node.isValid())
        return Result::fail ("insert " + Session::slotName (slot) + " is empty");

    const auto name = node[ids::pluginName].toString();
    dropPlugin (node);
    track.removeChild (node, um());
    return Result::good (insertOwnerName (index) + " insert " + Session::slotName (slot) + ": removed " + name
                         + L" \x00b7 ctrl+z brings it back");
}

Result App::toggleInsertEditor (int index, int slot)
{
    const auto track = automationOwner (index);
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
    closePluginWindows();
    in->touched = true; // whatever happens in there may differ from the save
    in->slot->showEditor ((index < 0 ? juce::String ("Mix") : trackLabel (index) + " " + track[ids::name].toString()) + L" \x00b7 " + Session::slotName (slot) + L" \x00b7 "
                          + in->slot->name() + L" \x00b7 taw");
    return Result::good (in->slot->name() + L" window open \x00b7 | again (or its close button) closes it");
}

void App::closePluginWindows()
{
    for (auto& in : hosted)
        if (in.slot != nullptr && in.slot->editorShowing())
        {
            in.slot->closeEditor();
            writePluginState (in);
        }
}

// | (shift+\): open or close the plugin under the cursor, playing or not. In the routing column,
// the slot there: an insert's window (empty: the effects picker), or the bus picker on a send or
// the output. On a track: its instrument, else its first insert, else the effects picker for
// insert A. On the mix, the same for its inserts.
Result App::togglePluginHere()
{
    if (session == nullptr || onTopLane())
        return Result::fail (L"| opens a track's plugin \x2014 \x2193 to one");

    // On the mix: its inserts.
    const auto who = ioTrack();
    const auto track = automationOwner (who);

    if (ioCell >= ioFirstSend && who >= 0)
    {
        openBusPicker (focused, ioCell == ioOutput ? 0 : ioCell - ioFirstSend + 1);
        return Result::good();
    }

    if (ioCell < 0 && who >= 0 && Session::isInstrumentTrack (track))
        return toggleEditor (focused);

    int slot = ioCell;

    for (int k = 0; slot < 0 && k < Session::numSends; ++k)
        if (Session::insert (track, k).isValid())
            slot = k;

    if (slot >= 0 && Session::insert (track, slot).isValid())
        return toggleInsertEditor (who, slot);

    openPluginPicker (who, std::max (0, slot));
    return Result::good();
}

void App::followWindow (int index, int slot)
{
    const bool anyOpen = std::any_of (hosted.begin(), hosted.end(), [] (const auto& in) { return in.slot != nullptr && in.slot->editorShowing(); });

    if (! anyOpen || slot < 0 || index < -1 || index >= session->numTracks())
        return;

    const auto node = Session::insert (automationOwner (index), slot);

    if (! node.isValid())
        return;

    if (auto* in = hostedEntry (node); in != nullptr && in->slot != nullptr && ! in->slot->editorShowing())
        toggleInsertEditor (index, slot);
}

Result App::swapInserts (int index, int slot, int other)
{
    if (other < 0 || other >= Session::numSends || other == slot)
        return Result::fail ("inserts run A to E");

    const auto track = automationOwner (index);
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

    return Result::good (insertOwnerName (index) + L" inserts: " + chain.joinIntoString (juce::String (L" \x2192 ")));
}

Result App::toggleBypass (int index, int slot)
{
    auto node = Session::insert (automationOwner (index), slot);

    if (! node.isValid())
        return Result::fail ("insert " + Session::slotName (slot) + " is empty");

    node.setProperty (ids::bypass, ! (bool) node[ids::bypass], um());
    return Result::good ("insert " + Session::slotName (slot) + " " + node[ids::pluginName].toString() + ((bool) node[ids::bypass] ? " bypassed" : " on"));
}

// ins                the effects picker, for the first free slot (and what's in the slots)
// ins valhalla       into the first free slot
// ins b | ins b valhalla | ins b>d
//                    open/close slot B's window (empty: the picker), load into it, swap with D
// ins b x | ins l3 x | ins x      remove it: by slot, by name, or the only one ("del",
// ins b off | ins l3 on           "remove" too); bypass it or back on, the same ways
// (fx: the old name, still understood.)
Result App::editInserts (const std::vector<int>& tracks, const juce::StringArray& argsIn)
{
    if (tracks.empty())
        return Result::fail (L"ins works on a track \x2014 \x2193 to one, or 2 ins");

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

    const auto first = tracks.front();

    // Removing or bypassing: the word anywhere, the insert by slot, name, or as the only one.
    bool remove = false;
    juce::String onOff;
    juce::StringArray words;

    for (const auto& a : args)
    {
        const auto lower = a.toLowerCase();

        if (lower == "x" || lower == "del" || lower == "remove")
            remove = true;
        else if (lower == "off" || lower == "on")
            onOff = lower;
        else
            words.add (a);
    }

    if (remove || onOff.isNotEmpty())
    {
        Result last;
        juce::StringArray done;

        for (auto t : tracks)
        {
            juce::String error;
            const auto meant = insertsMeant (t, slot, words.joinIntoString (" "), error);

            if (meant.empty())
                return Result::fail (error);

            for (auto s : meant)
            {
                if (remove)
                {
                    if (last = removeInsert (t, s); ! last.ok)
                        return last;

                    continue;
                }

                auto node = Session::insert (automationOwner (t), s);
                node.setProperty (ids::bypass, onOff == "off", um());
                done.add (insertOwnerName (t) + " insert " + Session::slotName (s) + " " + node[ids::pluginName].toString() + (onOff == "off" ? " bypassed" : " on"));
            }
        }

        return remove ? last : Result::good (done.joinIntoString (juce::String (L" \x00b7 ")));
    }

    const auto rest = args.joinIntoString (" ").trim();

    // No slot: the first free one (on the first track named).
    if (slot < 0)
    {
        for (int s = 0; s < Session::numSends && slot < 0; ++s)
            if (! Session::insert (automationOwner (first), s).isValid())
                slot = s;

        if (slot < 0)
            return Result::fail (insertOwnerName (first) + L"'s inserts are all used: " + describeInserts (first)
                                 + L" \x00b7 " + routingPrefix (first) + " ins e <name> replaces E, " + routingPrefix (first) + " ins e x removes it");
    }
    else if (rest.isEmpty())
    {
        if (Session::insert (automationOwner (first), slot).isValid())
            return toggleInsertEditor (first, slot);
    }

    if (rest.isEmpty())
    {
        openPluginPicker (first, slot);

        if (! plugins.hasScanned())
            return Result::good (juce::String (L"scanning plugins (first time)\x2026"));

        // What's there already, and how it comes out, under the picker.
        if (const auto there = describeInserts (first); there.isNotEmpty())
            return Result::good (insertOwnerName (first) + " inserts " + there + L" \x00b7 " + routingPrefix (first) + " ins <slot or name> x removes one");

        return Result::good();
    }

    if (! plugins.hasScanned())
        return Result::fail (L"no plugins scanned yet \x2014 type scan (takes a moment), then " + routingPrefix (first) + " ins " + rest);

    const auto desc = plugins.findEffect (rest, ! trackIsStereo (automationOwner (first)));

    if (! desc)
        return Result::fail ("no effect matches " + rest.quoted() + L" \x00b7 " + routingPrefix (first) + L" ins alone lists them \x00b7 scan finds new ones");

    Result last;

    for (auto t : tracks)
        if (last = loadEffect (t, slot, *desc); ! last.ok)
            return last;

    return last;
}
} // namespace taw
