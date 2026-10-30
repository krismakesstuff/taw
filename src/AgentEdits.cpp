// Agents in the open session, taw's side (docs/plan-taw-mcp.md): the tools an agent calls
// through taw --mcp, run here on the message thread one at a time. Reads describe the session;
// edits go through the state tree with um(), one undo step each, so the graph is rebuilt and
// the engine hears them exactly as it hears a person's edits.
//
// What's the user's stays theirs: an edit runs inside a guard that notes everything it touched,
// and if any of it was the focused track, inside the time selection, or something the user
// changed in the last few seconds, it's rolled back before the engine hears it and refused.

#include "App.h"

namespace taw
{
//==============================================================================
void agents::RecentEdits::watch (juce::ValueTree t)
{
    if (root.isValid())
        root.removeListener (this);

    root = std::move (t);
    times.clear();

    if (root.isValid())
        root.addListener (this);
}

juce::ValueTree agents::RecentEdits::ownerOf (const juce::ValueTree& node)
{
    auto n = node;

    while (n.isValid() && ! n.hasType (ids::TRACK) && n.getParent().isValid())
        n = n.getParent();

    return n; // a track, the session's tree, or the top of something already taken out
}

void agents::RecentEdits::touched (const juce::ValueTree& owner)
{
    if (agentEditing || ! owner.isValid())
        return;

    const auto now = juce::Time::getMillisecondCounterHiRes();
    times.erase (std::remove_if (times.begin(), times.end(), [&] (const auto& t)
                                 { return t.first == owner || now - t.second > window * 1000.0; }),
                 times.end());
    times.emplace_back (owner, now);
}

double agents::RecentEdits::recent (const juce::ValueTree& owner) const
{
    const auto now = juce::Time::getMillisecondCounterHiRes();

    for (const auto& [node, when] : times)
        if (node == owner && now - when <= window * 1000.0)
            return (now - when) / 1000.0;

    return -1.0;
}

namespace
{
// Everything an edit touches, while it runs: a node whose property changed, or a parent and
// the child it gained or lost.
struct Touches final : juce::ValueTree::Listener
{
    struct Touch
    {
        juce::ValueTree node, child;
        bool removed = false;
    };

    explicit Touches (juce::ValueTree t) : root (std::move (t)) { root.addListener (this); }
    ~Touches() override { root.removeListener (this); }

    void valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier&) override { list.push_back ({ t, {}, false }); }
    void valueTreeChildAdded (juce::ValueTree& p, juce::ValueTree& c) override { list.push_back ({ p, c, false }); }
    void valueTreeChildRemoved (juce::ValueTree& p, juce::ValueTree& c, int) override { list.push_back ({ p, c, true }); }
    void valueTreeChildOrderChanged (juce::ValueTree& p, int, int) override { list.push_back ({ p, {}, false }); }

    juce::ValueTree root;
    std::vector<Touch> list;
};

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();

    for (const auto& [k, v] : props)
        o->setProperty (k, v);

    return o;
}

// Beats as agents read them: no 3.9999999 from samples and back.
double tidy (double x) { return std::round (x * 1e6) / 1e6; }

juce::ValueTree clipOf (const juce::ValueTree& node)
{
    for (auto n = node; n.isValid() && ! n.hasType (ids::TRACK); n = n.getParent())
        if (Session::isClip (n))
            return n;

    return {};
}

// What the passthrough leaves to the user: their files, transport, history, windows,
// controllers and clipboard.
const juce::StringArray& usersOnly()
{
    static const juce::StringArray verbs { "save", "close", "q", "quit", "exit", "play", "stop", "rec", "undo", "redo", "dev", "panel",
                                           "b", "bounce", "bv", "bounces", "hub", "scan", "midi", "crashtest", "keycast", "video",
                                           "help", "?", "project", "settings", "stats", "browse", "index", "io", "fill", "light",
                                           "dark", "grey", "gray", "ed", "map", "unmap", "aw", "auto", "copy", "cut", "paste", "sel" };
    return verbs;
}
} // namespace

//==============================================================================
struct AgentTools
{
    App& app;
    const agents::Call& call;

    using Answer = agents::Answer;
    Session& session() const { return *app.session; }
    double spb() const { return session().samplesPerBeat(); }

    // What's the user's, noted before an edit --------------------------------------
    struct Locks
    {
        juce::ValueTree focused;              // invalid: the cursor is on the Mix
        std::vector<juce::ValueTree> covered; // the tracks the time selection is on
        juce::Range<juce::int64> range;       // the time selection (empty: none)
        std::vector<juce::ValueTree> inRange; // clips on those tracks overlapping it
        std::vector<juce::ValueTree> markersInRange;
    };

    juce::Range<juce::int64> span (const juce::ValueTree& clip) const
    {
        if (clip.hasType (ids::CLIP))
        {
            const auto start = (juce::int64) clip[ids::start];
            return { start, start + (juce::int64) clip[ids::length] };
        }

        const auto from = (juce::int64) std::llround ((double) clip[ids::beat] * spb());
        return { from, juce::jmax (from + 1, (juce::int64) std::llround (Session::clipEnd (clip) * spb())) };
    }

    static void allClips (const juce::ValueTree& holder, std::vector<juce::ValueTree>& out)
    {
        for (const auto& c : holder)
        {
            if (Session::isClip (c))
                out.push_back (c);
            else if (c.hasType (ids::PLAYLIST))
                allClips (c, out);
        }
    }

    Locks locks() const
    {
        Locks l;

        if (app.focused >= 0 && app.focused < session().numTracks())
            l.focused = session().track (app.focused);

        if (app.hasTimeSelection())
        {
            l.range = app.timeSelection();

            for (auto t : app.defaultTargets())
                if (t >= 0 && t < session().numTracks())
                {
                    const auto track = session().track (t);
                    l.covered.push_back (track);
                    std::vector<juce::ValueTree> clips;
                    allClips (track, clips);

                    for (const auto& c : clips)
                        if (span (c).intersects (l.range))
                            l.inRange.push_back (c);
                }

            for (const auto& m : session().tree())
                if (m.hasType (ids::MARKER) && l.range.contains ((juce::int64) m[ids::position]))
                    l.markersInRange.push_back (m);
        }

        return l;
    }

    static bool has (const std::vector<juce::ValueTree>& v, const juce::ValueTree& t) { return std::find (v.begin(), v.end(), t) != v.end(); }

    juce::String trackText (const juce::ValueTree& track) const
    {
        const auto index = session().tree().indexOf (track); // tracks come first in the tree
        const auto name = track[ids::name].toString();
        return index >= 0 && index < session().numTracks() ? "track " + app.trackLabel (index) + " " + name : "track " + name;
    }

    juce::String selectionText (const Locks& l) const
    {
        return "your time selection (" + app.formatBars (l.range.getStart()) + L" \x2192 " + app.formatBars (l.range.getEnd()) + ")";
    }

    // Why a track is the user's right now, or "".
    juce::String lockReason (const juce::ValueTree& track, const Locks& l) const
    {
        if (track == l.focused)
            return "your cursor is on it";

        if (const auto secs = app.recentEdits.recent (track); secs >= 0.0)
            return "you changed it " + juce::String (secs, 1) + " s ago";

        if (has (l.covered, track))
            return selectionText (l) + " is on it";

        return {};
    }

    // Whether an edit that touched these broke the rules: why, or "".
    juce::String violation (const Locks& l, const Touches& touches) const
    {
        const auto sessionTree = session().tree();

        for (const auto& t : touches.list)
        {
            // What it was about: the child added or removed, else the node itself.
            const auto subject = t.child.isValid() ? t.child : t.node;
            const auto owner = subject.hasType (ids::TRACK) ? subject : agents::RecentEdits::ownerOf (t.node);
            const auto clip = Session::isClip (subject) ? subject : clipOf (t.node);

            if (owner.hasType (ids::TRACK))
            {
                if (owner == l.focused)
                    return trackText (owner) + L" is where your cursor is \x2014 pick another track, or wait for the user to move";

                if (const auto secs = app.recentEdits.recent (owner); secs >= 0.0)
                    return "the user changed " + trackText (owner) + " " + juce::String (secs, 1) + " s ago (try again in a few seconds)";

                if (has (l.covered, owner))
                {
                    if (subject == owner && t.removed)
                        return trackText (owner) + " is in " + selectionText (l);

                    if (clip.isValid() && (has (l.inRange, clip) || (clip.getParent().isValid() && span (clip).intersects (l.range))))
                        return "that's inside " + selectionText (l) + " on " + trackText (owner);

                    for (auto n = t.node; n.isValid() && ! n.hasType (ids::TRACK); n = n.getParent())
                        if (n.hasType (ids::AUTO))
                            return "automation on " + trackText (owner) + " is in " + selectionText (l);
                }

                continue;
            }

            // The session's own settings and markers.
            const auto marker = subject.hasType (ids::MARKER) ? subject : juce::ValueTree();

            if (marker.isValid() && ! l.range.isEmpty() && (has (l.markersInRange, marker) || l.range.contains ((juce::int64) marker[ids::position])))
                return "that marker is inside " + selectionText (l);

            if (owner == sessionTree)
                if (const auto secs = app.recentEdits.recent (sessionTree); secs >= 0.0)
                    return "the user changed the session's settings " + juce::String (secs, 1) + " s ago (try again in a few seconds)";
        }

        return {};
    }

    // Where the user is, put back after an edit (commands focus what they make) -------
    struct View
    {
        juce::int64 cursor = 0, viewStart = 0, timeAnchor = -1, timeHead = -1;
        juce::ValueTree focusedTrack, anchorTrack;
        bool onMix = false;
        int focusedIndex = 0, focusedLane = -1, trackScroll = 0, zoomLevel = 0, browserPadNote = 0;
        App::TopLane topLane = App::TopLane::none;
        juce::String commandLine;
        juce::ValueTree namingMarker, selectedMarker;
        bool helpVisible = false, settingsVisible = false, fillView = false, pluginPickerVisible = false, busPickerVisible = false,
             autoPanelVisible = false;
        Mode mode = Mode::Main;
    };

    View keepView() const
    {
        View v;
        v.cursor = app.cursor;
        v.viewStart = app.viewStart;
        v.timeAnchor = app.timeAnchor;
        v.timeHead = app.timeHead;
        v.onMix = app.focused >= session().numTracks();
        v.focusedIndex = app.focused;
        v.focusedTrack = v.onMix ? juce::ValueTree() : session().track (app.focused);
        v.anchorTrack = app.trackAnchor >= 0 ? session().track (app.trackAnchor) : juce::ValueTree();
        v.focusedLane = app.focusedLane;
        v.trackScroll = app.trackScroll;
        v.zoomLevel = app.zoomLevel;
        v.browserPadNote = app.browserPadNote;
        v.topLane = app.topLane;
        v.commandLine = app.commandLine;
        v.namingMarker = app.namingMarker;
        v.selectedMarker = app.selectedMarker;
        v.helpVisible = app.helpVisible;
        v.settingsVisible = app.settingsVisible;
        v.fillView = app.fillView;
        v.pluginPickerVisible = app.pluginPickerVisible;
        v.busPickerVisible = app.busPickerVisible;
        v.autoPanelVisible = app.autoPanelVisible;
        v.mode = app.mode;
        return v;
    }

    void restoreView (const View& v) const
    {
        const auto count = session().numTracks();
        auto indexOf = [&] (const juce::ValueTree& t)
        {
            const auto i = session().tree().indexOf (t);
            return t.isValid() && i >= 0 && i < count ? i : -1;
        };

        app.cursor = v.cursor;
        app.viewStart = v.viewStart;
        app.timeAnchor = v.timeAnchor;
        app.timeHead = v.timeHead;
        app.focused = v.onMix ? count : (indexOf (v.focusedTrack) >= 0 ? indexOf (v.focusedTrack) : juce::jlimit (0, count, v.focusedIndex));
        app.trackAnchor = indexOf (v.anchorTrack);
        app.focusedLane = v.focusedLane;
        app.trackScroll = v.trackScroll;
        app.zoomLevel = v.zoomLevel;
        app.browserPadNote = v.browserPadNote;
        app.topLane = v.topLane;
        app.commandLine = v.commandLine;
        app.namingMarker = v.namingMarker;
        app.selectedMarker = v.selectedMarker;
        app.helpVisible = v.helpVisible;
        app.settingsVisible = v.settingsVisible;
        app.fillView = v.fillView;
        app.pluginPickerVisible = v.pluginPickerVisible;
        app.busPickerVisible = v.busPickerVisible;
        app.autoPanelVisible = v.autoPanelVisible;
        app.mode = v.mode;
    }

    // Whether an edit may run at all now.
    std::optional<Answer> cantEdit() const
    {
        if (app.session == nullptr)
            return Answer::fail ("no-session", "no session is open in taw");

        if (app.engine.isRolling() || app.recording)
            return Answer::fail ("playing", juce::String (app.recording ? "taw is recording" : "taw is playing") + L" \x00b7 try again when it stops");

        if (app.opening != nullptr || app.closing != nullptr || app.closePrompt.has_value())
            return Answer::fail ("busy", "the session is opening or closing");

        if (app.import != nullptr || app.bounce != nullptr)
            return Answer::fail ("busy", juce::String (app.import != nullptr ? "an import" : "a bounce") + L" is running \x00b7 try again in a moment");

        return {};
    }

    // An edit: one undo step named for the agent, refused and rolled back if it touched what's
    // the user's. structured: a tool of ours, all or nothing; else the passthrough, which keeps
    // what a command did even when it says no, as it does for a person.
    Answer edit (const juce::String& label, const std::function<Answer()>& change, bool structured = true)
    {
        if (auto no = cantEdit())
            return *no;

        const auto held = locks();
        const auto view = keepView();
        const bool importing = app.import != nullptr;
        auto* um = app.um();
        Answer a;

        {
            const juce::ScopedValueSetter<bool> mine (app.agentEditing, true), notUsers (app.recentEdits.agentEditing, true);
            um->beginNewTransaction (call.agent + ": " + label);
            bool rollBack = false;

            {
                Touches touches (session().tree());
                a = change();

                if (const auto why = violation (held, touches); why.isNotEmpty())
                {
                    a = Answer::fail ("hands-off", why);
                    rollBack = true;
                }
            }

            if (rollBack || (structured && ! a.ok))
                um->undoCurrentTransactionOnly(); // the user's redo history comes back too
            else
                um->setCurrentTransactionName (call.agent + ": " + label);

            // It started an import (add sample, import): answered once that's in (pollAgents).
            if (a.ok && ! importing && app.import != nullptr)
            {
                app.agentWaiting = call;
                app.agentWaitingLabel = label;
                a.code = "waiting";
            }
        }

        restoreView (view);
        app.lastEditKey.clear(); // the user's next [ ] doesn't merge into the agent's step

        if (a.ok && a.code != "waiting")
            app.say (Result::good (call.agent + ": " + a.text));

        return a;
    }

    // The import a call started is in: placed now, inside the guard. Nothing (keep waiting)
    // until it's done and taw is stopped.
    std::optional<Answer> finishImport()
    {
        if (app.import == nullptr || app.session == nullptr)
            return Answer::fail ("failed", "the import was cancelled");

        if (! app.import->isDone() || app.engine.isRolling() || app.recording)
            return {};

        const auto held = locks();
        const auto view = keepView();
        const auto before = session().numTracks();
        auto* um = app.um();
        Answer a;

        {
            const juce::ScopedValueSetter<bool> mine (app.agentEditing, true), notUsers (app.recentEdits.agentEditing, true);
            um->beginNewTransaction (call.agent + ": " + app.agentWaitingLabel);
            bool rollBack = false;

            {
                Touches touches (session().tree());
                app.pollImport();
                a = app.messageIsError ? Answer::fail ("failed", app.message) : Answer::good (app.message);

                // add_track's name, on the track the import made.
                if (const auto name = call.args["name"].toString().trim(); a.ok && call.tool == "add_track" && name.isNotEmpty() && session().numTracks() > before)
                {
                    session().track (session().numTracks() - 1).setProperty (ids::name, name, um);
                    a.text << ", named " << name;
                }

                if (session().numTracks() > before)
                    a.data = object ({ { "track", app.trackLabel (session().numTracks() - 1) } });

                if (const auto why = violation (held, touches); why.isNotEmpty())
                {
                    a = Answer::fail ("hands-off", why);
                    rollBack = true;
                }
            }

            if (rollBack)
                um->undoCurrentTransactionOnly();
            else
                um->setCurrentTransactionName (call.agent + ": " + app.agentWaitingLabel);
        }

        restoreView (view);
        app.lastEditKey.clear();

        if (a.ok)
            app.say (Result::good (call.agent + ": " + a.text));

        return a;
    }

    // Arguments ------------------------------------------------------------------------
    const juce::var& arg (const char* name) const { return call.args[name]; }

    // A track by its number as on screen ("3", "2.3") or its exact name.
    int findTrack (const juce::var& v, juce::String& error) const
    {
        const auto want = v.toString().trim();
        const auto numbers = app.numbering();

        if (want.isEmpty())
        {
            error = "which track? give its number as on screen (\"3\", \"2.3\") or its name";
            return -1;
        }

        for (int t = 0; t < session().numTracks(); ++t)
            if (numbers.label (t) == want)
                return t;

        for (int t = 0; t < session().numTracks(); ++t)
            if (session().track (t)[ids::name].toString().equalsIgnoreCase (want))
                return t;

        error = "no track " + want.quoted() + " (get_session lists them)";
        return -1;
    }

    // Reading ---------------------------------------------------------------------------
    juce::String busName (int busId) const
    {
        const auto bus = session().findBus (busId);
        return bus >= 0 ? session().track (bus)[ids::name].toString() : juce::String ("Mix");
    }

    juce::var describeTrack (int t, bool full, const Locks& l) const
    {
        const auto track = session().track (t);
        auto* o = new juce::DynamicObject();
        const auto numbers = app.numbering();
        const auto kind = Session::isKit (track) ? juce::String ("kit") : track[ids::kind].toString().isEmpty() ? juce::String ("audio")
                                                                                                                : track[ids::kind].toString();

        o->setProperty ("id", numbers.label (t));
        o->setProperty ("name", track[ids::name]);
        o->setProperty ("kind", kind);

        if (const auto why = lockReason (track, l); why.isNotEmpty())
            o->setProperty ("locked", why);

        o->setProperty ("armed", (bool) track[ids::armed]);
        o->setProperty ("mute", (bool) track[ids::mute]);
        o->setProperty ("solo", (bool) track[ids::solo]);
        o->setProperty ("volume_db", tidy ((double) track[ids::gainDb]));
        o->setProperty ("pan", tidy ((double) track.getProperty (ids::pan, 0.0)));
        o->setProperty ("output", busName ((int) track.getProperty (ids::out, 0)));

        if (Session::isInstrumentTrack (track))
            o->setProperty ("plugin", track[ids::pluginName].toString().isNotEmpty() ? juce::var (track[ids::pluginName]) : juce::var());

        if (Session::isSampleTrack (track))
            o->setProperty ("sample", track[ids::sample].toString().isNotEmpty() ? juce::var (track[ids::sample]) : juce::var());

        if (const auto kit = app.kitOf (t); kit >= 0)
        {
            o->setProperty ("kit", numbers.label (kit));
            o->setProperty ("note", (int) track.getProperty (ids::note, -1));

            if ((int) track[ids::choke] > 0)
                o->setProperty ("choke", (int) track[ids::choke]);
        }

        if (Session::isKit (track))
        {
            juce::Array<juce::var> members;

            for (int m = 0; m < session().numTracks(); ++m)
                if (app.kitOf (m) == t)
                    members.add (numbers.label (m));

            o->setProperty ("members", members);
        }

        juce::Array<juce::var> inserts, sends, clips;

        for (int slot = 0; slot < Session::numSends; ++slot)
        {
            if (const auto in = Session::insert (track, slot); in.isValid())
                inserts.add (object ({ { "slot", Session::slotName (slot) }, { "plugin", in[ids::pluginName] }, { "bypass", (bool) in[ids::bypass] } }));

            if (const auto s = Session::send (track, slot); s.isValid())
                sends.add (object ({ { "slot", Session::slotName (slot) }, { "to", busName ((int) s[ids::to]) }, { "db", tidy ((double) s[ids::db]) }, { "pre", (bool) s[ids::pre] } }));
        }

        o->setProperty ("inserts", inserts);
        o->setProperty ("sends", sends);

        const auto rate = session().sampleRate();
        int index = 0;

        if (Session::isInstrumentTrack (track))
        {
            for (const auto& c : Session::midiClips (track))
            {
                juce::Array<juce::var> notes;
                int count = 0;

                for (const auto& n : c)
                    if (n.hasType (ids::NOTE) && Session::inWindow (c, n[ids::beat]))
                    {
                        ++count;

                        if (full)
                            notes.add (object ({ { "beat", tidy ((double) n[ids::beat] - (double) c[ids::offset]) }, { "length", tidy ((double) n[ids::len]) }, { "pitch", (int) n[ids::pitch] }, { "vel", (int) n[ids::vel] }, { "ch", (int) n.getProperty (ids::ch, 1) } }));
                    }

                clips.add (object ({ { "index", index++ }, { "beat", tidy ((double) c[ids::beat]) }, { "length", tidy ((double) c[ids::len]) }, { full ? "notes" : "note_count", full ? juce::var (notes) : juce::var (count) } }));
            }
        }
        else if (Session::isSampleTrack (track))
        {
            for (const auto& c : Session::hitClips (track))
            {
                juce::Array<juce::var> hits;
                int count = 0;

                for (const auto& h : c)
                    if (h.hasType (ids::HIT) && Session::inWindow (c, h[ids::beat]))
                    {
                        ++count;

                        if (full)
                            hits.add (object ({ { "beat", tidy ((double) h[ids::beat] - (double) c[ids::offset]) }, { "vel", tidy ((double) h[ids::vel]) } }));
                    }

                clips.add (object ({ { "index", index++ }, { "beat", tidy ((double) c[ids::beat]) }, { "length", tidy ((double) c[ids::len]) }, { full ? "hits" : "hit_count", full ? juce::var (hits) : juce::var (count) } }));
            }
        }
        else if (! Session::isBusTrack (track))
        {
            for (const auto& c : Session::clips (track))
                clips.add (object ({ { "index", index++ }, { "beat", tidy ((double) (juce::int64) c[ids::start] / spb()) }, { "length_beats", tidy ((double) (juce::int64) c[ids::length] / spb()) }, { "seconds", tidy ((double) (juce::int64) c[ids::start] / rate) }, { "length_seconds", tidy ((double) (juce::int64) c[ids::length] / rate) }, { "file", c[ids::file] } }));
        }

        o->setProperty ("clips", clips);
        return o;
    }

    Answer getSession() const
    {
        if (app.session == nullptr)
            return Answer::fail ("no-session", "no session is open in taw");

        const auto l = locks();
        const auto& s = session().tree();
        const auto numbers = app.numbering();
        auto* o = new juce::DynamicObject();

        o->setProperty ("name", session().name());
        o->setProperty ("folder", session().folder().getFullPathName());
        o->setProperty ("bpm", tidy (session().bpm()));
        o->setProperty ("time_signature", juce::Array<juce::var> { (int) s[ids::timeSigNum], (int) s[ids::timeSigDen] });
        o->setProperty ("sample_rate", session().sampleRate());
        o->setProperty ("playing", app.engine.isRolling());
        o->setProperty ("recording", app.recording);
        o->setProperty ("unsaved", app.unsaved);
        o->setProperty ("cursor_beat", tidy ((double) app.cursor / spb()));
        o->setProperty ("focused", app.focused < session().numTracks() ? juce::var (numbers.label (app.focused)) : juce::var ("mix"));

        if (! l.range.isEmpty())
        {
            juce::Array<juce::var> on;

            for (const auto& t : l.covered)
                on.add (numbers.label (session().tree().indexOf (t)));

            o->setProperty ("selection", object ({ { "from_beat", tidy ((double) l.range.getStart() / spb()) },
                                                   { "to_beat", tidy ((double) l.range.getEnd() / spb()) },
                                                   { "tracks", on } }));
        }
        else
            o->setProperty ("selection", juce::var());

        o->setProperty ("loop", object ({ { "on", (bool) s[ids::loop] }, { "from_beat", tidy ((double) (juce::int64) s[ids::loopStart] / spb()) }, { "to_beat", tidy ((double) (juce::int64) s[ids::loopEnd] / spb()) } }));

        juce::Array<juce::var> markers, tracks;

        for (const auto& m : s)
            if (m.hasType (ids::MARKER))
                markers.add (object ({ { "name", m[ids::name] }, { "beat", tidy ((double) (juce::int64) m[ids::position] / spb()) } }));

        const bool notes = arg ("include_notes");

        for (int t = 0; t < session().numTracks(); ++t)
            tracks.add (describeTrack (t, notes, l));

        o->setProperty ("markers", markers);
        o->setProperty ("tracks", tracks);
        return Answer::good ({}, o);
    }

    Answer getTrack() const
    {
        if (app.session == nullptr)
            return Answer::fail ("no-session", "no session is open in taw");

        juce::String error;
        const auto t = findTrack (arg ("track"), error);
        return t < 0 ? Answer::fail ("invalid", error) : Answer::good ({}, describeTrack (t, true, locks()));
    }

    // Editing ---------------------------------------------------------------------------
    Answer runCommand()
    {
        if (app.session == nullptr)
            return Answer::fail ("no-session", "no session is open in taw");

        const auto line = arg ("command").toString().trim();

        if (line.isEmpty())
            return Answer::fail ("invalid", "which command? e.g. \"2 v -6\"");

        if (line.startsWithChar ('@'))
            return Answer::fail ("invalid", "the cursor is the user's (@ moves it)");

        const auto shape = commandShape (line, app.numbering());

        if (shape.error.isNotEmpty())
            return Answer::fail ("invalid", shape.error);

        if (shape.verb.isEmpty())
            return Answer::fail ("invalid", "a bare track number focuses that track, and the focus is the user's");

        if (usersOnly().contains (shape.verb))
            return Answer::fail ("invalid", shape.verb.quoted() + " is left to the user (agents can't save, play, undo, bounce, open windows or use the clipboard)");

        if ((shape.verb == "inst" || shape.verb == "fx" || shape.verb == "ins") && shape.args.isEmpty())
            return Answer::fail ("invalid", shape.verb + " alone opens a picker for the user: name the plugin (" + shape.verb + " vital)");

        return edit (line, [&]
                     {
                         const auto r = app.execute (line);
                         return r.ok ? Answer::good (r.message.isNotEmpty() ? r.message : line) : Answer::fail ("failed", r.message); }, false);
    }

    // The track an edit just made: the one that wasn't there before.
    juce::var newTrack (const std::vector<juce::ValueTree>& before) const
    {
        for (int t = session().numTracks(); --t >= 0;)
            if (! has (before, session().track (t)))
                return object ({ { "track", app.trackLabel (t) } });

        return {};
    }

    Answer addTrack()
    {
        const auto kind = arg ("kind").toString().trim().toLowerCase();
        const auto name = arg ("name").toString().removeCharacters ("\"").trim();
        const auto plugin = arg ("plugin").toString().trim();
        const auto sample = arg ("sample").toString().trim().unquoted();

        if (! juce::StringArray { "audio", "instrument", "sample", "bus", "kit" }.contains (kind))
            return Answer::fail ("invalid", "kind: audio, instrument, sample, bus or kit");

        if (kind == "sample" && sample.isNotEmpty() && ! juce::File::isAbsolutePath (sample))
            return Answer::fail ("invalid", "sample: a full path to an audio file");

        if (kind == "sample" && sample.isNotEmpty() && ! juce::File (sample).existsAsFile())
            return Answer::fail ("invalid", "no file " + sample);

        std::vector<juce::ValueTree> before;

        if (app.session != nullptr)
            for (int t = 0; t < session().numTracks(); ++t)
                before.push_back (session().track (t));

        return edit ("add " + kind + " track" + (name.isNotEmpty() ? " " + name : juce::String()), [&]
                     {
                         Result r;

                         // A plain track of a kind, with nothing to load: straight into the tree.
                         auto plain = [&] (const juce::String& k, const juce::String& base)
                         {
                             const auto n = name.isNotEmpty() ? name : base + " " + juce::String ((int) app.numbering().byNumber.size() + 1);
                             auto track = session().addTrack (n, 0, app.um());

                             if (k != "audio")
                                 track.setProperty (ids::kind, k, app.um());

                             return Result::good ("added " + k + " track " + app.trackLabel (session().numTracks() - 1) + " " + n);
                         };

                         if (kind == "audio")
                             r = app.execute (name.isNotEmpty() ? "add \"" + name + "\"" : juce::String ("add")); // the lowest free input, as for a person
                         else if (kind == "bus")
                             r = app.addBus (name.isNotEmpty() ? name : juce::String ("Bus"));
                         else if (kind == "kit")
                             r = app.addKit (name);
                         else if (kind == "instrument" && plugin.isNotEmpty())
                         {
                             r = app.loadInstrument (std::vector<int>(), plugin);

                             if (r.ok && session().numTracks() > (int) before.size())
                             {
                                 // Not armed: the user's controller keeps playing what it played.
                                 auto track = session().track (session().numTracks() - 1);
                                 track.setProperty (ids::armed, false, app.um());

                                 if (name.isNotEmpty())
                                     track.setProperty (ids::name, name, app.um());

                                 r.message = "added instrument track " + app.trackLabel (session().numTracks() - 1) + " " + track[ids::name].toString()
                                             + " playing " + track[ids::pluginName].toString();
                             }
                         }
                         else if (kind == "instrument")
                             r = plain ("instrument", "Inst");
                         else if (sample.isNotEmpty())
                             r = app.startImport ({ sample }, -1, App::ImportAs::sample);
                         else
                             r = plain ("sample", "Sample");

                         if (! r.ok)
                             return Answer::fail ("failed", r.message);

                         return Answer::good (r.message, newTrack (before)); });
    }

    Answer removeTrack()
    {
        if (auto no = cantEdit())
            return *no;

        juce::String error;
        const auto t = findTrack (arg ("track"), error);

        if (t < 0)
            return Answer::fail ("invalid", error);

        const auto label = trackText (session().track (t));
        return edit ("remove " + label, [&]
                     {
                         const auto r = app.removeTracks ({ t });
                         return r.ok ? Answer::good (r.message) : Answer::fail ("failed", r.message); });
    }

    // write_notes and write_hits: into clip `clip`, or a new one at `beat`.
    Answer writeEvents (bool notes)
    {
        if (auto no = cantEdit())
            return *no;

        juce::String error;
        const auto t = findTrack (arg ("track"), error);

        if (t < 0)
            return Answer::fail ("invalid", error);

        auto track = session().track (t);
        const auto what = trackText (track);

        if (notes && ! Session::isInstrumentTrack (track))
            return Answer::fail ("invalid", what + " isn't an instrument track: notes go on one (add_track kind instrument)");

        if (! notes && ! Session::isSampleTrack (track))
            return Answer::fail ("invalid", what + " isn't a sample track: hits go on one (add_track kind sample)");

        // The events, checked before anything changes.
        struct Event
        {
            double beat = 0.0, len = 0.0;
            int pitch = 60, vel = 100, ch = 1;
            float hitVel = 0.8f;
        };

        std::vector<Event> events;
        const auto* list = arg (notes ? "notes" : "hits").getArray();

        if (list == nullptr || list->isEmpty())
            return Answer::fail ("invalid", notes ? "notes: a list of { beat, length, pitch, vel?, ch? }" : "hits: a list of { beat, vel? }");

        for (int i = 0; i < list->size(); ++i)
        {
            const auto& v = list->getReference (i);
            Event e;
            const auto bad = [&] (const juce::String& why)
            { return Answer::fail ("invalid", (notes ? "note " : "hit ") + juce::String (i + 1) + ": " + why); };

            if (! v.hasProperty ("beat") || (double) v["beat"] < 0.0)
                return bad ("beat (from the clip's start) must be 0 or more");

            e.beat = v["beat"];

            if (notes)
            {
                e.len = v["length"];
                e.pitch = v.getProperty ("pitch", -1);
                e.vel = v.getProperty ("vel", 100);
                e.ch = v.getProperty ("ch", 1);

                if (e.len <= 0.0)
                    return bad ("length must be more than 0 beats");

                if (e.pitch < 0 || e.pitch > 127)
                    return bad ("pitch is a MIDI note, 0-127 (60 is C4)");

                if (e.vel < 1 || e.vel > 127)
                    return bad ("vel is 1-127");

                if (e.ch < 1 || e.ch > 16)
                    return bad ("ch is 1-16");
            }
            else
            {
                e.hitVel = (float) (double) v.getProperty ("vel", 0.8);

                if (e.hitVel < 0.0f || e.hitVel > 1.0f)
                    return bad ("vel is 0-1");
            }

            events.push_back (e);
        }

        auto clips = notes ? Session::midiClips (track) : Session::hitClips (track);
        const bool intoClip = arg ("clip").isInt() || arg ("clip").isInt64() || arg ("clip").isDouble();
        const int clipIndex = intoClip ? (int) arg ("clip") : -1;

        if (intoClip && (clipIndex < 0 || clipIndex >= (int) clips.size()))
            return Answer::fail ("invalid", what + " has no clip " + juce::String (clipIndex) + " (" + juce::String ((int) clips.size()) + " clips; get_track lists them)");

        // A new clip: at beat (by default the bar after the track's last clip), whole bars long.
        const auto bar = (double) juce::jmax (1, session().beatsPerBar());
        double reach = 0.0;

        for (const auto& e : events)
            reach = juce::jmax (reach, e.beat + juce::jmax (e.len, 1.0 / 64.0));

        double start = arg ("beat").isVoid() ? -1.0 : (double) arg ("beat");

        if (! intoClip && start < 0.0)
        {
            start = 0.0;

            for (const auto& c : clips)
                start = juce::jmax (start, std::ceil (Session::clipEnd (c) / bar - 1e-9) * bar);
        }

        const auto length = arg ("length").isVoid() ? juce::jmax (1.0, std::ceil (reach / bar - 1e-9)) * bar : (double) arg ("length");

        if (! intoClip && length <= 0.0)
            return Answer::fail ("invalid", "length must be more than 0 beats");

        const bool replace = arg ("replace");
        const auto kindText = juce::String (notes ? "note" : "hit") + (events.size() == 1 ? "" : "s");
        const auto label = (replace ? "replace " : "write ") + juce::String ((int) events.size()) + " " + kindText + " on " + what;

        return edit (label, [&]
                     {
                         auto* um = app.um();
                         juce::ValueTree clip;

                         if (intoClip)
                             clip = clips[(size_t) clipIndex];
                         else
                         {
                             clip = notes ? Session::makeMidiClip (start, 0.0, length) : Session::makeHitClip (start, 0.0, length);
                             track.appendChild (clip, um);
                         }

                         if (replace)
                             for (int i = clip.getNumChildren(); --i >= 0;)
                                 if (clip.getChild (i).hasType (notes ? ids::NOTE : ids::HIT))
                                     clip.removeChild (i, um);

                         // Beats from the clip's start, into its content (offset on).
                         const auto offset = (double) clip[ids::offset];

                         for (const auto& e : events)
                             clip.appendChild (notes ? Session::makeNote (offset + e.beat, e.len, e.pitch, e.vel, e.ch) : Session::makeHit (offset + e.beat, e.hitVel), um);

                         // An existing clip grows (never shrinks) to take them in.
                         Session::growMidiClip (clip, (double) clip[ids::beat], (double) clip[ids::beat] + reach, um);

                         const auto index = (int) (std::find (clips.begin(), clips.end(), clip) - clips.begin());
                         const auto at = app.formatBars ((juce::int64) std::llround ((double) clip[ids::beat] * spb()));
                         return Answer::good ((replace ? "replaced with " : "wrote ") + juce::String ((int) events.size()) + " " + kindText + " "
                                                  + (intoClip ? "in clip " + juce::String (index) : juce::String ("in a new clip")) + " on " + what + " at " + at,
                                              object ({ { "track", app.trackLabel (t) }, { "clip", intoClip ? index : (int) clips.size() } })); });
    }

    Answer removeClip()
    {
        if (auto no = cantEdit())
            return *no;

        juce::String error;
        const auto t = findTrack (arg ("track"), error);

        if (t < 0)
            return Answer::fail ("invalid", error);

        auto track = session().track (t);
        const auto clips = Session::isInstrumentTrack (track) ? Session::midiClips (track)
                           : Session::isSampleTrack (track)   ? Session::hitClips (track)
                                                              : Session::clips (track);
        const int index = arg ("clip");

        if (! arg ("clip").isInt() && ! arg ("clip").isInt64() && ! arg ("clip").isDouble())
            return Answer::fail ("invalid", "clip: its index from get_track");

        if (index < 0 || index >= (int) clips.size())
            return Answer::fail ("invalid", trackText (track) + " has no clip " + juce::String (index));

        const auto what = trackText (track);
        return edit ("remove clip " + juce::String (index) + " on " + what, [&]
                     {
                         track.removeChild (clips[(size_t) index], app.um());
                         return Answer::good ("removed clip " + juce::String (index) + " on " + what); });
    }

    Answer addMarker()
    {
        const auto name = arg ("name").toString().trim();
        const double beat = arg ("beat");

        if (name.isEmpty() || beat < 0.0)
            return Answer::fail ("invalid", "add_marker: { beat (0 or more), name }");

        return edit ("marker " + name, [&]
                     {
                         const auto r = app.addMarker ((juce::int64) std::llround (beat * spb()), name);
                         return r.ok ? Answer::good (r.message) : Answer::fail ("failed", r.message); });
    }

    Answer run()
    {
        const auto& tool = call.tool;

        if (tool == "get_session")
            return getSession();
        if (tool == "get_track")
            return getTrack();
        if (tool == "run_command")
            return runCommand();
        if (tool == "add_track")
            return addTrack();
        if (tool == "remove_track")
            return removeTrack();
        if (tool == "write_notes")
            return writeEvents (true);
        if (tool == "write_hits")
            return writeEvents (false);
        if (tool == "remove_clip")
            return removeClip();
        if (tool == "add_marker")
            return addMarker();

        return Answer::fail ("invalid", "taw has no tool " + tool.quoted());
    }
};

//==============================================================================
bool App::startAgents()
{
    if (agentServer != nullptr)
        return true;

    auto server = std::make_unique<agents::Server>();

    if (! server->start())
        return false;

    agentServer = std::move (server);
    return true;
}

agents::Answer App::agentCall (const agents::Call& call)
{
    return AgentTools { *this, call }.run();
}

void App::pollAgents()
{
    if (agentServer == nullptr)
        return;

    agentServer->setSession (session != nullptr ? session->folder() : juce::File(), session != nullptr ? session->name() : juce::String());

    if (session == nullptr)
        recentEdits.watch ({});
    else if (! recentEdits.watching (session->tree()))
        recentEdits.watch (session->tree());

    // A call waiting for its import goes first; the others queue behind it.
    if (agentWaiting.has_value())
    {
        const auto call = *agentWaiting;
        const auto done = AgentTools { *this, call }.finishImport();

        if (! done.has_value())
            return;

        agentWaiting.reset();
        agentServer->answer (call, *done);
    }

    // As many as fit in a few ms; each is quick (an import waits, as above).
    const auto until = juce::Time::getMillisecondCounterHiRes() + 20.0;

    while (! agentWaiting.has_value() && juce::Time::getMillisecondCounterHiRes() < until)
    {
        const auto call = agentServer->next();

        if (! call.has_value())
            break;

        const auto answer = agentCall (*call);

        if (answer.code != "waiting")
            agentServer->answer (*call, answer);
    }
}
} // namespace taw
