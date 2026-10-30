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

    void valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier& id) override
    {
        if (! agents::RecentEdits::isBookkeeping (id))
            list.push_back ({ t, {}, false });
    }

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
                                           "dark", "grey", "gray", "ed", "map", "unmap", "aw", "auto", "copy", "cut", "paste", "sel",
                                           "agents" };
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
                    a.data = trackRef (session().numTracks() - 1);

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

    // A track by its id ("t7", what reads give and what stays put), else its number as on
    // screen ("3", "2.3", which shifts as tracks come and go), else its exact name.
    int findTrack (const juce::var& v, juce::String& error) const
    {
        const auto want = v.toString().trim();
        const auto numbers = app.numbering();

        if (want.isEmpty())
        {
            error = "which track? give its id from get_session (\"t3\")";
            return -1;
        }

        if (const auto t = session().trackById (want); t >= 0)
            return t;

        for (int t = 0; t < session().numTracks(); ++t)
            if (numbers.label (t) == want)
                return t;

        for (int t = 0; t < session().numTracks(); ++t)
            if (session().track (t)[ids::name].toString().equalsIgnoreCase (want))
                return t;

        error = "no track " + want.quoted() + " (get_session lists them)";
        return -1;
    }

    // What an edit answers with: the track's id and its number now.
    juce::var trackRef (int t) const { return object ({ { "track", session().track (t)[ids::uid] }, { "number", app.trackLabel (t) } }); }

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

        o->setProperty ("id", track[ids::uid]);
        o->setProperty ("number", numbers.label (t));
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
            o->setProperty ("kit", session().track (kit)[ids::uid]);
            o->setProperty ("note", (int) track.getProperty (ids::note, -1));

            if ((int) track[ids::choke] > 0)
                o->setProperty ("choke", (int) track[ids::choke]);
        }

        if (Session::isKit (track))
        {
            juce::Array<juce::var> members;

            for (int m = 0; m < session().numTracks(); ++m)
                if (app.kitOf (m) == t)
                    members.add (session().track (m)[ids::uid]);

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
        o->setProperty ("focused", app.focused < session().numTracks() ? juce::var (session().track (app.focused)[ids::uid]) : juce::var ("mix"));
        o->setProperty ("tempo_request", tempoRequest());
        o->setProperty ("session_request", sessionRequest());

        if (! l.range.isEmpty())
        {
            juce::Array<juce::var> on;

            for (const auto& t : l.covered)
                on.add (t[ids::uid]);

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

        // A Live set comes in as a session of its own, closing this one.
        if (shape.verb == "import" && shape.args.joinIntoString (" ").containsIgnoreCase (".als"))
            return Answer::fail ("invalid", "importing a Live set closes this session: the user does that (or new_session for a session of your own)");

        if ((shape.verb == "inst" || shape.verb == "fx" || shape.verb == "ins") && shape.args.isEmpty())
            return Answer::fail ("invalid", shape.verb + " alone opens a picker for the user: name the plugin (" + shape.verb + " vital)");

        // The tempo may move audio against the beats: set_tempo asks the user when it would.
        if (shape.verb == "bpm")
            return setTempo (shape.args[0]);

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
                return trackRef (t);

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

        if (intoClip)
            if (auto moved = clipMoved (clips[(size_t) clipIndex], clipIndex))
                return *moved;

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
                                              object ({ { "track", track[ids::uid] }, { "clip", intoClip ? index : (int) clips.size() } })); });
    }

    // A clip index is only good until clips change: clip_beat (its start, from the read) says
    // which clip the agent means, and a clip that isn't there any more is refused.
    std::optional<Answer> clipMoved (const juce::ValueTree& clip, int index) const
    {
        if (arg ("clip_beat").isVoid())
            return {};

        const auto at = clip.hasType (ids::CLIP) ? (double) (juce::int64) clip[ids::start] / spb() : (double) clip[ids::beat];

        if (std::abs (at - (double) arg ("clip_beat")) < 1e-4)
            return {};

        return Answer::fail ("invalid", "clip " + juce::String (index) + " starts at beat " + juce::String (tidy (at)) + ", not "
                                            + arg ("clip_beat").toString() + ": the clips have changed, read the track again");
    }

    // Tempo: MIDI and hits are in beats and follow it; audio clips are in samples and don't.
    // With none, the change just goes through; with some, the user says what happens to them
    // (App::answerTempoAsk), and the agent finds out by calling again or reading tempo_request.
    bool hasAudioClips() const
    {
        for (int t = 0; t < session().numTracks(); ++t)
        {
            std::vector<juce::ValueTree> clips;
            allClips (session().track (t), clips);

            for (const auto& c : clips)
                if (c.hasType (ids::CLIP))
                    return true;
        }

        return false;
    }

    juce::var tempoRequest() const
    {
        if (const auto& ask = app.tempoAsk)
            return object ({ { "status", "waiting for the user" }, { "agent", ask->agent }, { "from", ask->from }, { "to", ask->to } });

        if (const auto& done = app.tempoOutcome)
            return object ({ { "status", done->applied ? "applied" : "cancelled" }, { "agent", done->agent }, { "from", done->from }, { "to", done->to }, { "audio", done->audio } });

        return {};
    }

    Answer setTempo (const juce::var& value)
    {
        if (auto no = cantEdit())
            return *no;

        const auto bpm = value.toString().trim().getDoubleValue();
        const auto now = session().bpm();
        auto bpmText = [] (double b)
        { return juce::String (b, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."); };

        if (bpm < 20.0 || bpm > 400.0)
            return Answer::fail ("invalid", "bpm between 20 and 400");

        if (app.tempoAsk.has_value())
            return Answer::fail ("needs-user", "taw is asking the user about " + app.tempoAsk->agent + "'s change from " + bpmText (app.tempoAsk->from) + " to "
                                                   + bpmText (app.tempoAsk->to) + L" bpm \x00b7 call again in a moment, or read tempo_request in get_session");

        const auto& last = app.tempoOutcome;
        const bool askedThis = last.has_value() && juce::approximatelyEqual (last->to, bpm)
                               && juce::Time::getMillisecondCounterHiRes() - last->when < 120000.0;

        if (juce::approximatelyEqual (now, bpm))
            return Answer::good ("the tempo is " + bpmText (bpm) + (askedThis && last->applied ? " (the user chose: " + last->audio + ")" : juce::String()));

        if (askedThis && ! last->applied && last->agent == call.agent)
            return Answer::fail ("declined", "the user said no to " + bpmText (last->from) + L" \x2192 " + bpmText (bpm) + " bpm: ask them in the chat before trying again");

        if (! hasAudioClips())
            return edit ("bpm " + bpmText (bpm), [&]
                         {
                             session().tree().setProperty (ids::bpm, bpm, app.um());
                             return Answer::good ("tempo " + bpmText (now) + L" \x2192 " + bpmText (bpm) + " (MIDI and hits follow the beats)"); });

        if (const auto secs = app.recentEdits.recent (session().tree()); secs >= 0.0)
            return Answer::fail ("hands-off", "the user changed the session's settings " + juce::String (secs, 1) + " s ago (try again in a few seconds)");

        app.tempoAsk = App::TempoAsk { call.agent, now, bpm };
        return Answer::fail ("needs-user", "the session has audio clips, so taw is asking the user what to do with them (" + bpmText (now) + L" \x2192 "
                                               + bpmText (bpm) + L" bpm) \x00b7 call again in a moment, or read tempo_request in get_session");
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

        if (auto moved = clipMoved (clips[(size_t) index], index))
            return *moved;

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

    // Sessions: list, make, open. Switching away from an open session is the user's to say
    // (App::answerSessionAsk); on the session screen it just happens.
    // A taw opened straight to a session hasn't read its templates yet.
    const std::vector<Template>& templates() const
    {
        if (app.templates.empty())
            app.templates = loadTemplates();

        return app.templates;
    }

    std::vector<juce::File> knownSessions() const
    {
        auto all = loadRecentSessions();

        for (const auto& parent : { app.sessionsFolder, app.newSessionParent,
                                    app.session != nullptr ? app.session->folder().getParentDirectory() : juce::File() })
            if (parent != juce::File() && parent.isDirectory())
                for (const auto& dir : parent.findChildFiles (juce::File::findDirectories, false))
                    if (Session::existsIn (dir) && std::find (all.begin(), all.end(), dir) == all.end())
                        all.push_back (dir);

        std::stable_sort (all.begin(), all.end(), [] (const auto& a, const auto& b)
                          { return a.getChildFile ("session.json").getLastModificationTime() > b.getChildFile ("session.json").getLastModificationTime(); });
        return all;
    }

    juce::File findSession (const juce::String& what) const
    {
        if (juce::File::isAbsolutePath (what) && Session::existsIn (juce::File (what)))
            return juce::File (what);

        const auto all = knownSessions();

        for (const auto& dir : all)
            if (dir.getFileName().equalsIgnoreCase (what))
                return dir;

        for (const auto& dir : all) // the newest whose name has it
            if (dir.getFileName().containsIgnoreCase (what))
                return dir;

        return {};
    }

    juce::var sessionRequest() const
    {
        if (const auto& ask = app.sessionAsk)
            return object ({ { "status", "waiting for the user" }, { "agent", ask->agent }, { "session", ask->folder.getFullPathName() },
                             { "new", ask->create } });

        if (const auto& done = app.sessionOutcome)
            return object ({ { "status", done->applied ? "applied" : "declined" }, { "agent", done->agent },
                             { "session", done->folder.getFullPathName() }, { "new", done->create } });

        return {};
    }

    Answer listSessions() const
    {
        juce::Array<juce::var> sessions, templates;
        const auto open = app.session != nullptr ? app.session->folder() : juce::File();

        for (const auto& dir : knownSessions())
            sessions.add (object ({ { "name", dir.getFileName() }, { "folder", dir.getFullPathName() },
                                    { "saved", dir.getChildFile ("session.json").getLastModificationTime().toISO8601 (true) },
                                    { "open", dir == open } }));

        for (const auto& t : this->templates())
            templates.add (t.name);

        auto* o = new juce::DynamicObject();
        o->setProperty ("open", open != juce::File() ? juce::var (open.getFullPathName()) : juce::var());
        o->setProperty ("new_sessions_go_in", app.newSessionFolderFor ("x").getParentDirectory().getFullPathName());
        o->setProperty ("sessions", sessions);
        o->setProperty ("templates", templates);
        o->setProperty ("session_request", sessionRequest());
        const auto n = sessions.size();
        return Answer::good (juce::String (n) + (n == 1 ? " session" : " sessions") + (open != juce::File() ? ", " + open.getFileName() + " open" : juce::String (", none open")),
                             juce::var (o));
    }

    Answer switchTo (bool create)
    {
        const auto asked = arg (create ? "name" : "session").toString().trim();

        if (asked.isEmpty())
            return Answer::fail ("invalid", create ? "name the session" : "which session? its folder or name (list_sessions)");

        if (app.sessionAsk.has_value())
            return Answer::fail ("needs-user", "taw is asking the user about " + app.sessionAsk->agent + L"'s switch to " + app.sessionAsk->folder.getFileName()
                                                   + L" \x00b7 call again in a moment");

        // A call again after the user answered.
        const auto& last = app.sessionOutcome;
        const auto openFolder = app.session != nullptr ? app.session->folder() : juce::File();

        if (last.has_value() && last->agent == call.agent && last->asked == asked && last->create == create
            && juce::Time::getMillisecondCounterHiRes() - last->when < 120000.0)
        {
            if (! last->applied)
                return Answer::fail ("declined", "the user said no to opening " + last->folder.getFileName() + ": ask them in the chat before trying again");

            const auto data = object ({ { "session", last->folder.getFullPathName() } });

            if (app.opening != nullptr || app.closing != nullptr)
                return Answer::good ("opening " + last->folder.getFileName() + L" (the user said yes) \x00b7 edits work once it's in", data);

            if (openFolder == last->folder)
                return Answer::good (last->folder.getFileName() + " is open", data);
        }

        if (app.engine.isRolling() || app.recording)
            return Answer::fail ("playing", juce::String (app.recording ? "taw is recording" : "taw is playing") + L" \x00b7 try again when it stops");

        if (app.opening != nullptr || app.closing != nullptr || app.closePrompt.has_value() || app.tempoAsk.has_value())
            return Answer::fail ("busy", "the session is opening or closing, or taw is asking the user something");

        if (app.import != nullptr || app.bounce != nullptr)
            return Answer::fail ("busy", juce::String (app.import != nullptr ? "an import" : "a bounce") + L" is running \x00b7 try again in a moment");

        App::SessionAsk ask { call.agent, create, asked, {}, arg ("template").toString().trim() };

        if (create)
        {
            templates(); // makeSessionFolder starts from one

            if (ask.templateName.isNotEmpty()
                && std::none_of (templates().begin(), templates().end(), [&] (const auto& t) { return t.name.equalsIgnoreCase (ask.templateName); }))
            {
                juce::StringArray names;

                for (const auto& t : templates())
                    names.add (t.name);

                return Answer::fail ("invalid", "no template called " + ask.templateName.quoted() + " (there are: " + names.joinIntoString (", ") + ")");
            }

            ask.folder = app.newSessionFolderFor (asked);
        }
        else
        {
            ask.folder = findSession (asked);

            if (ask.folder == juce::File())
                return Answer::fail ("invalid", "no session called " + asked.quoted() + " (list_sessions lists them)");

            if (ask.folder == openFolder)
                return Answer::good (ask.folder.getFileName() + " is already open", object ({ { "session", ask.folder.getFullPathName() } }));
        }

        const bool now = app.session == nullptr;

        if (const auto r = app.switchSession (ask); ! r.ok)
            return Answer::fail ("failed", r.message);

        if (now)
            return Answer::good ((create ? "made " : "opening ") + ask.folder.getFileName() + (create ? ", opening it" : "")
                                     + L" \x00b7 edits work once it's in (busy until then)",
                                 object ({ { "session", ask.folder.getFullPathName() } }));

        return Answer::fail ("needs-user", "taw is asking the user whether to " + juce::String (create ? "make and open " : "switch to ") + ask.folder.getFileName()
                                               + " (" + openFolder.getFileName() + L" is open) \x00b7 call again with the same "
                                               + (create ? "name" : "session") + " in a moment to see what they chose");
    }

    Answer run()
    {
        const auto& tool = call.tool;

        if (tool == "list_sessions")
            return listSessions();
        if (tool == "new_session")
            return switchTo (true);
        if (tool == "open_session")
            return switchTo (false);

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
        if (tool == "set_tempo")
            return setTempo (arg ("bpm"));

        return Answer::fail ("invalid", "taw has no tool " + tool.quoted());
    }
};

//==============================================================================
// The tempo box: an agent wants a new tempo and the session has audio clips. The user's
// answer is the change (one undo step, the agent's), or none.
void App::answerTempoAsk (TempoChoice choice)
{
    if (! tempoAsk.has_value())
        return;

    const auto ask = *tempoAsk;
    tempoAsk.reset();

    if (session == nullptr)
        return;

    const auto from = session->bpm();
    auto bpmText = [] (double b)
    { return juce::String (b, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."); };
    TempoOutcome done { ask.agent, from, ask.to, {}, choice != TempoChoice::cancel, juce::Time::getMillisecondCounterHiRes() };

    if (choice == TempoChoice::cancel)
    {
        done.audio = "cancelled by the user";
        tempoOutcome = done;
        say (Result::good ("tempo stays " + bpmText (from) + " (" + ask.agent + " wanted " + bpmText (ask.to) + ")"));
        return;
    }

    {
        const juce::ScopedValueSetter<bool> mine (agentEditing, true), notUsers (recentEdits.agentEditing, true);
        um()->beginNewTransaction (ask.agent + ": bpm " + bpmText (ask.to));
        session->tree().setProperty (ids::bpm, ask.to, um());

        // With the beats: each clip starts on the beat it started on (its audio isn't stretched,
        // so it runs as long as it did). Markers and the loop stay in time, as when you change it.
        if (choice == TempoChoice::moveWithBeats)
        {
            std::function<void (juce::ValueTree)> move = [&] (juce::ValueTree holder)
            {
                for (auto c : holder)
                {
                    if (c.hasType (ids::CLIP))
                        c.setProperty (ids::start, (juce::int64) std::llround ((double) (juce::int64) c[ids::start] * from / ask.to), um());
                    else if (c.hasType (ids::PLAYLIST))
                        move (c);
                }
            };

            for (int t = 0; t < session->numTracks(); ++t)
                move (session->track (t));
        }

        um()->setCurrentTransactionName (ask.agent + ": bpm " + bpmText (ask.to));
    }

    done.audio = choice == TempoChoice::keepInTime ? "audio kept where it is in time" : "audio clips moved with the beats";
    tempoOutcome = done;
    lastEditKey.clear();
    say (Result::good (ask.agent + ": tempo " + bpmText (from) + L" \x2192 " + bpmText (ask.to) + L" \x00b7 " + done.audio));
}

bool App::handleTempoAskKey (const Key& k)
{
    if (! tempoAsk.has_value())
        return false;

    const auto c = k.code == Key::Char ? juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) k.ch) : 0;

    if (c == 't')
        answerTempoAsk (TempoChoice::keepInTime);
    else if (c == 'b')
        answerTempoAsk (TempoChoice::moveWithBeats);
    else if (k.code == Key::Escape || c == 'c' || c == 'n')
        answerTempoAsk (TempoChoice::cancel);
    else if (c == 's')
        say (Result::fail (L"stretching audio to the tempo isn't built yet \x2014 t, b or esc"));

    return true;
}

//==============================================================================
// An agent's new_session / open_session. Never reuses a folder: a second "bass idea" today is
// "bass idea 10.30.26 2".
juce::File App::newSessionFolderFor (const juce::String& name) const
{
    const auto parent = sessionsFolder != juce::File() && sessionsFolder.isDirectory() ? sessionsFolder
                        : newSessionParent != juce::File()                            ? newSessionParent
                        : session != nullptr                                          ? session->folder().getParentDirectory()
                                                                                      : juce::File::getCurrentWorkingDirectory();
    const auto base = juce::File::createLegalFileName (dateSessionNames ? datedName (name.trim()) : name.trim());
    auto dir = parent.getChildFile (base);

    for (int n = 2; dir.exists(); ++n)
        dir = parent.getChildFile (base + " " + juce::String (n));

    return dir;
}

Result App::makeSessionFolder (const SessionAsk& ask)
{
    const Template* from = nullptr;

    for (const auto& t : templates)
        if (ask.templateName.isNotEmpty() ? t.name.equalsIgnoreCase (ask.templateName) : &t == &templates[(size_t) defaultTemplateRow()])
            from = &t;

    if (ask.folder.exists())
        return Result::fail (ask.folder.getFullPathName() + " is there already");

    juce::String error;

    if (Session::create (ask.folder, from != nullptr ? from->tree : juce::ValueTree(), newRate, error) == nullptr)
        return Result::fail (error);

    return Result::good();
}

Result App::switchSession (const SessionAsk& ask)
{
    if (session != nullptr)
    {
        sessionAsk = ask; // the box asks
        return Result::good();
    }

    if (ask.create)
        if (auto r = makeSessionFolder (ask); ! r.ok)
            return r;

    sessionOutcome = SessionOutcome { ask.agent, ask.asked, ask.create, true, ask.folder, juce::Time::getMillisecondCounterHiRes() };
    startOpening (ask.folder);
    return Result::good();
}

// The session box: s save and switch (and bounce last-saved, as closing does), o save only,
// d switch without saving, 0 stay. With nothing unsaved, any yes just switches.
void App::answerSessionAsk (juce::juce_wchar choice)
{
    if (! sessionAsk.has_value())
        return;

    const auto ask = *sessionAsk;
    sessionAsk.reset();
    SessionOutcome done { ask.agent, ask.asked, ask.create, false, ask.folder, juce::Time::getMillisecondCounterHiRes() };
    sessionOutcome = done;

    if (choice == 0 || session == nullptr)
    {
        say (Result::good ("staying in " + (session != nullptr ? session->name() : juce::String ("taw")) + " (" + ask.agent + " wanted "
                           + ask.folder.getFileName() + ")"));
        return;
    }

    if (bounce != nullptr || import != nullptr)
    {
        say (Result::fail (L"wait for the bounce or import to finish (esc cancels it)"));
        return;
    }

    if (ask.create)
        if (auto r = makeSessionFolder (ask); ! r.ok)
        {
            say (Result::fail ("couldn't make " + ask.folder.getFileName() + ": " + r.message));
            return;
        }

    sessionOutcome->applied = true;
    openAfterClose = ask.folder;
    openAfterCloseNote = ask.agent + (ask.create ? ": made " : ": opening ") + ask.folder.getFileName();

    if (! hasUnsavedChanges())
        startClosing (false, false);
    else if (choice == 'o')
        startClosing (false, true, false);
    else
        startClosing (false, choice == 's');
}

bool App::handleSessionAskKey (const Key& k)
{
    if (! sessionAsk.has_value())
        return false;

    const auto c = k.code == Key::Char ? juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) k.ch) : 0;
    const bool changes = hasUnsavedChanges();

    if (k.code == Key::Escape || c == 'c')
        answerSessionAsk (0);
    else if (changes && (k.code == Key::Enter || c == 's' || c == 'y'))
        answerSessionAsk ('s');
    else if (changes && (c == 'o' || c == 'd' || c == 'n'))
        answerSessionAsk (c == 'n' ? 'd' : c);
    else if (! changes && (k.code == Key::Enter || c == 'y' || c == 'o'))
        answerSessionAsk ('o');
    else if (! changes && c == 'n')
        answerSessionAsk (0);

    return true;
}

// "agents" / "agents on" / "agents off": whether taw listens for agents (settings.json, every
// session). Off: no socket and no listing, so taw --mcp says agents are off.
Result App::agentsCommand (const juce::String& arg)
{
    const auto a = arg.trim().toLowerCase();

    if (a.isNotEmpty() && a != "on" && a != "off")
        return Result::fail ("agents on | agents off");

    agentsOn = a.isEmpty() ? ! agentsOn : a == "on";
    saveSettings();

    if (! agentsOn)
    {
        agentServer.reset();
        tempoAsk.reset();
        sessionAsk.reset();
        return Result::good (L"agents off \x00b7 taw --mcp can't reach this taw (agents on lets them in)");
    }

    if (! startAgents())
        return Result::fail ("agents on, but taw couldn't open its local socket");

    return Result::good (L"agents on \x00b7 taw --mcp reaches the session open here");
}

//==============================================================================
bool App::startAgents()
{
    if (agentServer != nullptr)
        return true;

    if (! agentsOn)
        return false;

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

void App::noteAgentCall (const agents::Call& call, const agents::Answer& answer)
{
    AgentSeen seen { call.agent, call.tool, answer.text, answer.ok ? juce::String() : answer.code,
                     ! call.tool.startsWith ("get_") && ! call.tool.startsWith ("list_"),
                     juce::Time::getMillisecondCounterHiRes() };

    if (answer.code == "waiting") // an import under way: noted again when it's answered
        seen.code = "working";

    if (seen.edit && answer.code == "playing")
    {
        ++agentHeld;
        agentHeldBy = call.agent;
    }

    agentLastCall = seen;

    if (seen.edit)
        agentLastEdit = seen;
}

void App::pollAgents()
{
    if (agentServer == nullptr)
        return;

    // Stopped after an agent was held off: say so once, so the user knows it can edit now.
    const bool rolling = engine.isRolling() || recording;

    if (agentRolling && ! rolling && agentHeld > 0)
    {
        say (Result::good (agentHeldBy + " tried to edit " + (agentHeld == 1 ? juce::String ("once") : juce::String (agentHeld) + " times")
                           + L" while it played \x00b7 it can edit now"));
        agentHeld = 0;
    }

    if (! agentRolling && rolling)
        agentHeld = 0;

    agentRolling = rolling;

    agentServer->setSession (session != nullptr ? session->folder() : juce::File(), session != nullptr ? session->name() : juce::String());

    if (session == nullptr)
    {
        recentEdits.watch ({});
        tempoAsk.reset();
        tempoOutcome.reset();
        sessionAsk.reset(); // nothing to switch away from (the outcome stays, for the agent's retry)
    }
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
        noteAgentCall (call, *done);
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

        noteAgentCall (*call, answer);

        if (answer.code != "waiting")
            agentServer->answer (*call, answer);
    }
}
} // namespace taw
