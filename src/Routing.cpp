// Routing: bus tracks, each track's output (the mix or a bus) and its five sends (A to E), the
// routing column right of the timeline, and the bus picker.

#include "App.h"

namespace taw
{
namespace
{
float sendGain (const juce::ValueTree& send)
{
    return juce::Decibels::decibelsToGain ((float) (double) send[ids::db], -96.0f);
}

juce::String levelText (double db)
{
    return db <= -96.0 ? juce::String ("-inf") : formatDb (db);
}

// "-12", "+3", "0", "-6db", "-inf": a send level. A bare number other than 0 is a track number.
bool isLevel (const juce::String& token, double& db)
{
    auto t = token.trim().toLowerCase();
    const bool dbSuffix = t.endsWith ("db");

    if (dbSuffix)
        t = t.dropLastCharacters (2);

    if (t == "-inf")
    {
        db = -96.0;
        return true;
    }

    const bool sign = t.startsWithChar ('-') || t.startsWithChar ('+');
    const auto digits = sign ? t.substring (1) : t;

    if (digits.isEmpty() || ! digits.containsOnly ("0123456789."))
        return false;

    if (! sign && ! dbSuffix && digits.getDoubleValue() != 0.0)
        return false;

    db = juce::jlimit (-96.0, 12.0, t.getDoubleValue());
    return true;
}

int slotOf (const juce::String& token) // "b" -> 1, else -1
{
    const auto t = token.toLowerCase();
    return t.length() == 1 && t[0] >= 'a' && t[0] < 'a' + Session::numSends ? (int) (t[0] - 'a') : -1;
}
} // namespace

//==============================================================================
std::vector<int> App::routesFrom (int track, bool withMutedSends) const
{
    std::vector<int> to;
    const auto t = session->track (track);

    if (const auto out = session->findBus (t[ids::out]); out >= 0 && out != track)
        to.push_back (out);

    for (const auto& c : t)
        if (c.hasType (ids::SEND) && (withMutedSends || ! (bool) c[ids::mute]))
            if (const auto b = session->findBus (c[ids::to]); b >= 0 && b != track)
                to.push_back (b);

    return to;
}

bool App::feeds (int from, int to) const
{
    std::vector<bool> seen ((size_t) session->numTracks(), false);
    std::vector<int> stack { from };

    while (! stack.empty())
    {
        const auto t = stack.back();
        stack.pop_back();

        if (t == to)
            return true;

        if (seen[(size_t) t])
            continue;

        seen[(size_t) t] = true;

        for (auto next : routesFrom (t, true))
            stack.push_back (next);
    }

    return false;
}

std::vector<int> App::busFeeders (int busTrack) const
{
    std::vector<int> from;

    for (int t = 0; t < session->numTracks(); ++t)
        if (t != busTrack)
            for (auto to : routesFrom (t, true))
                if (to == busTrack)
                {
                    from.push_back (t);
                    break;
                }

    return from;
}

void App::applyRouting (Graph& g, bool offline)
{
    const auto n = (int) g.tracks.size();

    // Each bus track gets a buffer (the first maxBuses of them; makeBus stops there).
    for (int t = 0; t < n; ++t)
        if (Session::isBusTrack (session->track (t)) && g.numBuses < Graph::maxBuses)
            g.tracks[(size_t) t].busIndex = g.numBuses++;

    // The order tracks run in: a bus only once everything feeding it has run. Ties go in track
    // order. A loop (only possible in a hand-edited session) runs in track order, and the routes
    // that would feed back are dropped below.
    std::vector<std::vector<int>> edges ((size_t) n);
    std::vector<int> waiting ((size_t) n, 0), position ((size_t) n, -1);

    for (int t = 0; t < n; ++t)
        for (auto to : routesFrom (t, false))
        {
            edges[(size_t) t].push_back (to);
            ++waiting[(size_t) to];
        }

    while ((int) g.order.size() < n)
    {
        int next = -1;

        for (int t = 0; t < n && next < 0; ++t)
            if (position[(size_t) t] < 0 && waiting[(size_t) t] == 0)
                next = t;

        for (int t = 0; t < n && next < 0; ++t) // a loop: break it at the first track left
            if (position[(size_t) t] < 0)
                next = t;

        position[(size_t) next] = (int) g.order.size();
        g.order.push_back (next);

        for (auto to : edges[(size_t) next])
            --waiting[(size_t) to];
    }

    auto busFor = [&] (int from, int busTrack)
    {
        return busTrack >= 0 && position[(size_t) busTrack] > position[(size_t) from] ? g.tracks[(size_t) busTrack].busIndex : -1;
    };

    for (int t = 0; t < n; ++t)
    {
        const auto track = session->track (t);
        auto& tp = g.tracks[(size_t) t];

        if (const auto out = session->findBus (track[ids::out]); out >= 0 && out != t)
            tp.out = busFor (t, out); // a bus that can't take it: the mix

        for (int s = 0; s < Session::numSends; ++s)
        {
            const auto send = Session::send (track, s);

            if (! send.isValid() || (bool) send[ids::mute])
                continue;

            if (const auto bus = busFor (t, session->findBus (send[ids::to])); bus >= 0)
                tp.sends.push_back ({ bus, sendGain (send), (bool) send[ids::pre], automationCurve (track, "send " + Session::slotName (s).toLowerCase()), {} });
        }
    }

    compensateLatency (g, offline);

    // Solo follows the signal: a bus that a soloed track feeds stays audible (or you'd lose its
    // reverb), and so do the tracks whose output runs into a soloed bus (soloing the drum bus
    // plays the drums). Muting still mutes.
    std::vector<bool> soloed ((size_t) n, false);
    bool anySolo = false;

    for (int t = 0; t < n; ++t)
        anySolo = anySolo || (soloed[(size_t) t] = (bool) session->track (t)[ids::solo]);

    if (! anySolo)
        return;

    std::vector<bool> heard = soloed;

    for (auto t : g.order) // in run order, so a bus is reached after everything feeding it
        if (heard[(size_t) t])
            for (auto to : edges[(size_t) t])
                heard[(size_t) to] = true;

    // Upstream, whatever feeds a soloed bus keeps feeding it, but stays out of the mix itself:
    // soloing the reverb plays just the reverb, soloing the drum bus plays the drums.
    std::vector<bool> feeding ((size_t) n, false);

    for (auto it = g.order.rbegin(); it != g.order.rend(); ++it) // a bus before what feeds it
        for (auto to : edges[(size_t) *it])
            if (soloed[(size_t) to] || feeding[(size_t) to])
                feeding[(size_t) *it] = true;

    for (int t = 0; t < n; ++t)
    {
        auto& tp = g.tracks[(size_t) t];
        tp.soloAudible = heard[(size_t) t] || feeding[(size_t) t];
        tp.audible = ! (bool) session->track (t)[ids::mute] && tp.soloAudible;

        if (! heard[(size_t) t] && tp.out < 0)
            tp.out = TrackPlay::nowhere;
    }
}

// Delay compensation. Each track's plugins make it late by its latency; a bus starts as late as
// the latest thing feeding it. Every route (an output or a send) is delayed by what it takes to
// arrive as late as the latest route into the same place, so everything meets in time at each
// bus and at the mix. The mix's lateness is the graph's latency: recordings and bounces are
// shifted by it. Routes keep their delay lines across rebuilds (a fader move mustn't empty them);
// a bounce gets its own.
void App::compensateLatency (Graph& g, bool offline)
{
    const auto n = (int) g.tracks.size();
    std::vector<int> busTrack ((size_t) g.numBuses, -1), arrives ((size_t) n, 0), leaves ((size_t) n, 0);

    for (int t = 0; t < n; ++t)
        if (g.tracks[(size_t) t].busIndex >= 0)
            busTrack[(size_t) g.tracks[(size_t) t].busIndex] = t;

    auto destOf = [&] (int bus) { return bus >= 0 && bus < g.numBuses ? busTrack[(size_t) bus] : -1; }; // -1: the mix
    int mix = 0;

    for (auto t : g.order) // everything feeding a bus runs before it
    {
        auto& tp = g.tracks[(size_t) t];
        leaves[(size_t) t] = arrives[(size_t) t] + tp.latency;

        auto reach = [&] (int bus)
        {
            if (const auto d = destOf (bus); d >= 0)
                arrives[(size_t) d] = juce::jmax (arrives[(size_t) d], leaves[(size_t) t]);
            else
                mix = juce::jmax (mix, leaves[(size_t) t]);
        };

        reach (tp.out);

        for (const auto& s : tp.sends)
            reach (s.bus);
    }

    g.latency = mix + g.mixBus.latency; // the mix's own inserts come after

    std::vector<KeptDelay> kept;

    auto delayFor = [&] (int t, int edge, int bus) -> std::shared_ptr<RouteDelay>
    {
        const auto d = destOf (bus);
        const auto length = (d >= 0 ? arrives[(size_t) d] : mix) - leaves[(size_t) t];

        if (length <= 0)
            return {};

        if (offline)
            return std::make_shared<RouteDelay> (length);

        const auto track = session->track (t);

        for (const auto& k : routeDelays)
            if (k.track == track && k.edge == edge && k.line->length == length)
            {
                kept.push_back (k);
                return k.line;
            }

        auto line = std::make_shared<RouteDelay> (length);
        kept.push_back ({ track, edge, line });
        return line;
    };

    for (int t = 0; t < n; ++t)
    {
        auto& tp = g.tracks[(size_t) t];
        tp.outDelay = delayFor (t, -1, tp.out);

        for (auto& s : tp.sends)
            s.delay = delayFor (t, (int) (&s - tp.sends.data()), s.bus);
    }

    if (! offline)
    {
        routeDelays = std::move (kept);
        mixLatency = g.latency;
    }
}

//==============================================================================
// Width and pan

bool App::trackIsStereo (const juce::ValueTree& track)
{
    if (track.hasType (ids::SESSION)) // the mix bus
        return true;

    if (track.hasProperty (ids::channels))
        return (int) track[ids::channels] >= 2;

    if (Session::isInstrumentTrack (track) || Session::isBusTrack (track))
        return true;

    if (Session::isSampleTrack (track))
    {
        const auto* data = audioFor (track[ids::sample].toString());
        return data != nullptr && data->audio->getNumChannels() > 1;
    }

    for (const auto& c : Session::clips (track))
        if (const auto* data = clipFor (c); data != nullptr && data->audio->getNumChannels() > 1)
            return true;

    return false;
}

juce::String App::panText (double pan)
{
    const auto amount = juce::roundToInt (std::abs (pan) * 100.0);
    return amount == 0 ? juce::String ("C") : pan < 0.0 ? "<" + juce::String (amount) : juce::String (amount) + ">";
}

// "c" or "0" centre; "l30", "30l", "<30" or "-30" left; "r30", "30r", "30>", "+30" or "30" right.
Result App::setPan (const std::vector<int>& tracks, const juce::String& valueIn)
{
    auto v = valueIn.trim().toLowerCase().removeCharacters (" ");

    if (v.isEmpty())
        return Result::fail ("pan where? 2 pan l30, 2 pan c, 2 pan 45r");

    double sign = 1.0;

    if (v.startsWithChar ('l') || v.startsWithChar ('<') || v.endsWithChar ('l'))
        sign = -1.0;

    const auto digits = v.removeCharacters ("lr<>+c");

    if (v != "c" && v != "center" && v != "centre" && ! digits.containsOnly ("-0123456789.") )
        return Result::fail ("pan l30, pan c, pan 45r (or -100 to 100)");

    const auto pan = juce::jlimit (-1.0, 1.0, sign * digits.getDoubleValue() / 100.0);

    for (auto t : tracks)
        session->track (t).setProperty (ids::pan, pan, um());

    return Result::good (trackList (tracks) + " pan " + panText (pan));
}

Result App::nudgePan (const std::vector<int>& tracks, double delta)
{
    for (auto t : tracks)
    {
        auto track = session->track (t);
        track.setProperty (ids::pan, juce::jlimit (-1.0, 1.0, std::round (((double) track[ids::pan] + delta) * 100.0) / 100.0), um());
    }

    return Result::good (trackList (tracks) + " pan " + panText (session->track (tracks.front())[ids::pan]));
}

Result App::setWidth (const std::vector<int>& tracks, bool stereo)
{
    for (auto t : tracks)
    {
        const auto track = session->track (t);

        if (Session::isInstrumentTrack (track) || Session::isBusTrack (track))
            return Result::fail ("track " + juce::String (t + 1) + " is " + (Session::isBusTrack (track) ? "a bus" : "an instrument") + ": always stereo");
    }

    for (auto t : tracks)
    {
        auto track = session->track (t);
        track.setProperty (ids::channels, stereo ? 2 : 1, um());

        // Its effects get made again for the new width (their settings carry over).
        for (const auto& c : track)
            if (c.hasType (ids::INSERT))
                dropPlugin (c);
    }

    const int input = session->track (tracks.front())[ids::input];
    return Result::good (trackList (tracks) + (stereo ? " stereo" : " mono")
                         + (input > 0 ? (stereo ? L" \x00b7 records inputs " + juce::String (input) + "-" + juce::String (input + 1)
                                                : L" \x00b7 records input " + juce::String (input))
                                      : juce::String()));
}

//==============================================================================
int App::makeBus (const juce::String& nameIn, juce::String& error)
{
    int buses = 0;

    for (int t = 0; t < session->numTracks(); ++t)
        buses += Session::isBusTrack (session->track (t)) ? 1 : 0;

    if (buses >= Graph::maxBuses)
    {
        error = "a session has at most " + juce::String (Graph::maxBuses) + " buses";
        return 0;
    }

    if (session->numTracks() >= AudioEngine::maxTracks)
    {
        error = "a session has at most " + juce::String (AudioEngine::maxTracks) + " tracks";
        return 0;
    }

    auto name = nameIn.trim().unquoted();

    if (name.isEmpty())
        name = "Bus " + juce::String (buses + 1);
    else
        name = name.substring (0, 1).toUpperCase() + name.substring (1); // "verb" reads as Verb

    const auto id = session->nextBusId();
    auto t = session->addTrack (name, 0, um());
    t.setProperty (ids::kind, "bus", um()).setProperty (ids::busId, id, um());
    return id;
}

Result App::addBus (const juce::String& name)
{
    juce::String error;
    const auto id = makeBus (name, error);

    if (id == 0)
        return Result::fail (error);

    const auto lower = session->track (session->findBus (id))[ids::name].toString().toLowerCase();
    focusTrack (session->findBus (id));
    return Result::good ("added bus " + busName (id) + L" \x00b7 feed it: 1 send " + lower + " (a send) or 1 out " + lower
                         + L" (all of track 1) \x00b7 \\ on a track routes it");
}

juce::String App::busName (int busId) const
{
    const auto t = session->findBus (busId);
    return t < 0 ? juce::String ("Mix") : juce::String (t + 1) + " " + session->track (t)[ids::name].toString();
}

// A bus by track number or name: exact, then the start of a name, then anywhere in it. A name
// nothing matches makes a new bus (made says so).
int App::resolveBus (const juce::String& queryIn, juce::String& made, juce::String& error)
{
    const auto query = queryIn.trim().unquoted();

    if (query.containsOnly ("0123456789") && query.isNotEmpty())
    {
        const auto t = query.getIntValue() - 1;

        if (t < 0 || t >= session->numTracks())
            error = "no track " + query;
        else if (! Session::isBusTrack (session->track (t)))
            error = "track " + query + L" isn't a bus \x2014 bus <name> makes one";
        else
            return (int) session->track (t)[ids::busId];

        return 0;
    }

    for (int pass = 0; pass < 3; ++pass)
        for (int t = 0; t < session->numTracks(); ++t)
        {
            const auto track = session->track (t);
            const auto name = track[ids::name].toString();

            if (Session::isBusTrack (track)
                && (pass == 0 ? name.equalsIgnoreCase (query) : pass == 1 ? name.startsWithIgnoreCase (query) : name.containsIgnoreCase (query)))
                return (int) track[ids::busId];
        }

    const auto id = makeBus (query, error);

    if (id > 0)
        made = "new bus " + busName (id);

    return id;
}

Result App::checkRoute (int track, int busId) const
{
    const auto bus = session->findBus (busId);

    if (busId == 0)
        return Result::good();

    if (bus < 0)
        return Result::fail ("no such bus");

    if (bus == track)
        return Result::fail ("track " + juce::String (track + 1) + " can't feed itself");

    if (feeds (bus, track))
        return Result::fail (busName (busId) + " already feeds track " + juce::String (track + 1) + L" \x2014 that would loop");

    return Result::good();
}

Result App::setOutputBus (int track, int busId)
{
    if (auto r = checkRoute (track, busId); ! r.ok)
        return r;

    auto t = session->track (track);

    if (busId == 0)
        t.removeProperty (ids::out, um());
    else
        t.setProperty (ids::out, busId, um());

    return Result::good ("track " + juce::String (track + 1) + L" \x2192 " + busName (busId));
}

Result App::setSendBus (int track, int slot, int busId)
{
    if (busId == 0)
        return Result::fail ("sends go to buses (the mix is the output)");

    if (auto r = checkRoute (track, busId); ! r.ok)
        return r;

    auto t = session->track (track);
    auto send = Session::send (t, slot);

    if (! send.isValid())
    {
        send = juce::ValueTree (ids::SEND);
        send.setProperty (ids::slot, Session::slotName (slot).toLowerCase(), nullptr).setProperty (ids::db, 0.0, nullptr);
        send.setProperty (ids::to, busId, nullptr);
        t.appendChild (send, um());
    }
    else
    {
        send.setProperty (ids::to, busId, um());
    }

    return Result::good (describeIoCell (track, ioFirstSend + slot));
}

Result App::nudgeSend (int track, int slot, double deltaDb)
{
    auto send = Session::send (session->track (track), slot);

    if (! send.isValid())
        return Result::fail ("send " + Session::slotName (slot) + L" is empty \x2014 | picks a bus");

    const auto db = (double) send[ids::db];
    send.setProperty (ids::db, juce::jlimit (-96.0, 12.0, db <= -96.0 && deltaDb > 0 ? -60.0 : std::round (db + deltaDb)), um());
    return Result::good (describeIoCell (track, ioFirstSend + slot));
}

juce::String App::describeIoCell (int track, int cell) const
{
    const auto t = automationOwner (track);
    const auto label = insertOwnerName (track);

    if (cell == ioOutput)
        return label + L" output \x2192 " + busName (t[ids::out]) + L" \x00b7 | changes \x00b7 del: the mix";

    if (cell < ioFirstSend)
    {
        const auto insert = Session::insert (t, cell);
        const auto slot = Session::slotName (cell);

        if (! insert.isValid())
            return label + " insert " + slot + L" empty \x00b7 | picks an effect";

        const auto problem = insertProblem (insert);
        return label + " insert " + slot + ": " + insert[ids::pluginName].toString()
               + (problem.isNotEmpty() ? L" \x2014 " + problem : (bool) insert[ids::bypass] ? juce::String (" (bypassed)") : juce::String())
               + L" \x00b7 | opens it \x00b7 M bypass \x00b7 shift+\x2190\x2192 moves \x00b7 del removes";
    }

    const auto send = Session::send (t, cell - ioFirstSend);
    const auto slot = Session::slotName (cell - ioFirstSend);

    if (! send.isValid())
        return label + " send " + slot + L" empty \x00b7 | picks a bus";

    const bool missing = session->findBus (send[ids::to]) < 0;
    return label + " send " + slot + L" \x2192 " + (missing ? juce::String ("a bus that's gone") : busName (send[ids::to])) + " "
           + levelText (send[ids::db]) + " dB " + ((bool) send[ids::pre] ? "pre" : "post") + ((bool) send[ids::mute] ? " (off)" : "")
           + L" \x00b7 [ ] level \x00b7 M off \x00b7 P pre/post \x00b7 del removes";
}

juce::String App::describeSends (int track) const
{
    const auto t = session->track (track);
    juce::StringArray parts;

    for (int s = 0; s < Session::numSends; ++s)
        if (const auto send = Session::send (t, s); send.isValid())
            parts.add (Session::slotName (s) + " " + busName (send[ids::to]) + " " + levelText (send[ids::db])
                       + ((bool) send[ids::pre] ? " pre" : "") + ((bool) send[ids::mute] ? " off" : ""));

    juce::StringArray inserts;

    for (int s = 0; s < Session::numSends; ++s)
        if (const auto insert = Session::insert (t, s); insert.isValid())
            inserts.add (Session::slotName (s) + " " + insert[ids::pluginName].toString() + ((bool) insert[ids::bypass] ? " (bypassed)" : ""));

    return "track " + juce::String (track + 1) + (inserts.isEmpty() ? juce::String() : L" \x00b7 inserts " + inserts.joinIntoString (juce::String (L" \x00b7 ")))
           + L" \x2192 " + busName (t[ids::out])
           + (parts.isEmpty() ? juce::String (L" \x00b7 no sends") : L" \x00b7 sends " + parts.joinIntoString (juce::String (L" \x00b7 ")));
}

//==============================================================================
Result App::setOutput (const std::vector<int>& tracks, const juce::String& where)
{
    if (where.trim().isEmpty())
    {
        openBusPicker (tracks.front(), 0);
        return Result::good();
    }

    int busId = 0;
    juce::String made, error;

    if (! where.trim().equalsIgnoreCase ("mix"))
        if (busId = resolveBus (where, made, error); busId == 0)
            return Result::fail (error);

    Result last;

    for (auto t : tracks)
        if (last = setOutputBus (t, busId); ! last.ok)
            return last;

    const auto msg = trackList (tracks) + L" \x2192 " + busName (busId);
    return Result::good (made.isNotEmpty() ? made + L" \x00b7 " + msg : msg);
}

// send                       list the sends
// send verb                  a send to Verb (the slot already going there, else the first free)
// send b verb | send b 6     slot B to Verb / to bus track 6
// send b -12 | pre | post | off | on | x      level, before or after the fader, mute, remove
Result App::editSend (const std::vector<int>& tracks, const juce::StringArray& argsIn)
{
    auto args = argsIn;

    if (args.isEmpty())
        return Result::good (describeSends (tracks.front()));

    // "b-6" (from 2sendb-6): slot B, then a level.
    if (args[0].length() > 1 && slotOf (args[0].substring (0, 1)) >= 0 && juce::String ("+-0123456789").containsChar (args[0][1]))
    {
        args.insert (1, args[0].substring (1));
        args.set (0, args[0].substring (0, 1));
    }

    int slot = slotOf (args[0]);

    if (slot >= 0)
        args.remove (0);

    double db = 0.0;
    bool hasLevel = false, remove = false;
    juce::String pre, onOff; // "pre" / "post", "on" / "off"
    juce::StringArray words;

    for (const auto& a : args)
    {
        const auto lower = a.toLowerCase();

        if (isLevel (a, db))
            hasLevel = true;
        else if (lower == "pre" || lower == "post")
            pre = lower;
        else if (lower == "on" || lower == "off")
            onOff = lower;
        else if (lower == "x" || lower == "del")
            remove = true;
        else
            words.add (a);
    }

    const auto query = words.joinIntoString (" ");

    if (query.isEmpty() && slot < 0)
        return Result::fail (L"send to which bus? 2 send verb \x00b7 or a slot: 2 send b -6");

    int busId = 0;
    juce::String made;

    if (query.isNotEmpty())
    {
        juce::String error;

        if (busId = resolveBus (query, made, error); busId == 0)
            return Result::fail (error);
    }

    juce::StringArray done;

    for (auto t : tracks)
    {
        auto track = session->track (t);
        int s = slot;

        // No slot named: the one already going to this bus, else the first free one.
        for (int k = 0; k < Session::numSends && s < 0; ++k)
            if (const auto send = Session::send (track, k); send.isValid() && (int) send[ids::to] == busId)
                s = k;

        for (int k = 0; k < Session::numSends && s < 0; ++k)
            if (! Session::send (track, k).isValid())
                s = k;

        if (s < 0)
            return Result::fail ("track " + juce::String (t + 1) + L"'s sends are all used \x2014 2 send e verb replaces E");

        if (remove)
        {
            if (const auto send = Session::send (track, s); send.isValid())
                track.removeChild (send, um());

            done.add ("track " + juce::String (t + 1) + " send " + Session::slotName (s) + " removed");
            continue;
        }

        if (busId > 0)
        {
            if (auto r = setSendBus (t, s, busId); ! r.ok)
                return r;
        }

        auto send = Session::send (track, s);

        if (! send.isValid())
            return Result::fail ("send " + Session::slotName (s) + L" is empty \x2014 name a bus: 2 send " + Session::slotName (s).toLowerCase() + " verb");

        if (hasLevel)
            send.setProperty (ids::db, db, um());

        if (pre.isNotEmpty())
            send.setProperty (ids::pre, pre == "pre", um());

        if (onOff.isNotEmpty())
            send.setProperty (ids::mute, onOff == "off", um());

        done.add (describeIoCell (t, ioFirstSend + s).upToFirstOccurrenceOf (juce::String (L" \x00b7"), false, false));
    }

    const auto msg = done.joinIntoString (juce::String (L" \x00b7 "));
    return Result::good (made.isNotEmpty() ? made + L" \x00b7 " + msg : msg);
}

//==============================================================================
std::vector<App::BusChoice> App::busChoices() const
{
    std::vector<BusChoice> choices;

    if (session == nullptr || busPickTrack < 0 || busPickTrack >= session->numTracks())
        return choices;

    const auto filter = busFilter.trim();

    if (busPickCell == 0 && (filter.isEmpty() || juce::String ("mix").startsWithIgnoreCase (filter)))
        choices.push_back ({ "Mix", 0 });

    bool exact = false;

    for (int t = 0; t < session->numTracks(); ++t)
    {
        const auto track = session->track (t);
        const auto name = track[ids::name].toString();

        // Only buses this track can feed without a loop.
        if (! Session::isBusTrack (track) || t == busPickTrack || feeds (t, busPickTrack))
            continue;

        if (filter.isEmpty() || name.containsIgnoreCase (filter))
            choices.push_back ({ juce::String (t + 1) + " " + name, (int) track[ids::busId] });

        exact = exact || name.equalsIgnoreCase (filter);
    }

    if (! exact)
        choices.push_back ({ filter.isEmpty() ? juce::String ("new bus") : L"new bus \x201c" + filter + L"\x201d", -1 });

    return choices;
}

void App::openBusPicker (int track, int cell)
{
    busPickerVisible = true;
    busPickTrack = track;
    busPickCell = cell;
    busFilter.clear();
    busPickIndex = 0;

    // Start on where it goes now.
    const auto t = session->track (track);
    const int now = cell == 0 ? (int) t[ids::out] : (int) Session::send (t, cell - 1)[ids::to];
    const auto choices = busChoices();

    for (size_t i = 0; i < choices.size(); ++i)
        if (choices[i].busId == now && now != 0)
            busPickIndex = (int) i;
}

bool App::handleBusPickerKey (const Key& k)
{
    if (! busPickerVisible)
        return false;

    const auto choices = busChoices();
    const auto n = (int) choices.size();

    switch (k.code)
    {
        case Key::Up:     busPickIndex = juce::jmax (0, busPickIndex - 1); return true;
        case Key::Down:   busPickIndex = juce::jmax (0, juce::jmin (n - 1, busPickIndex + 1)); return true;
        case Key::Escape: busPickerVisible = false; return true;
        case Key::Backspace:
            busFilter = busFilter.dropLastCharacters (1);
            busPickIndex = 0;
            return true;
        case Key::Enter:
            if (busPickIndex < n && session != nullptr && busPickTrack < session->numTracks())
            {
                busPickerVisible = false;
                const auto& choice = choices[(size_t) busPickIndex];
                beginEdit (busPickCell == 0 ? "output" : "send");
                int busId = choice.busId;
                juce::String made;

                if (busId < 0)
                {
                    juce::String error;

                    if (busId = makeBus (busFilter, error); busId == 0)
                    {
                        say (Result::fail (error));
                        return true;
                    }

                    made = "new bus " + busName (busId) + L" \x00b7 ";
                }

                auto r = busPickCell == 0 ? setOutputBus (busPickTrack, busId) : setSendBus (busPickTrack, busPickCell - 1, busId);
                say (r.ok ? Result::good (made + r.message) : r);
            }
            return true;
        case Key::Char:
            if (k.ctrl || k.alt)
                return false;

            if (k.ch >= 32)
            {
                busFilter += juce::String::charToString ((juce::juce_wchar) k.ch);
                busPickIndex = 0;
            }
            return true;
        default:
            return true;
    }
}

// In the routing column (after \): left/right walk inserts A-E, sends A-E, then the output;
// up/down go row by row (inserts, sends, the next track's inserts). | opens what's under the
// cursor (togglePluginHere); enter keeps its one meaning, back to the start. On an insert: M
// bypasses, shift+left/right moves it along the chain, del removes it. On a send: [ ] ride its
// level, M turns it off, P flips pre/post, del removes it. On the output: del puts it back on
// the mix. Esc or \ leaves. An open plugin window follows you from insert to insert.
bool App::handleIoKey (const Key& k)
{
    if (ioCell < 0 || session == nullptr || ! commandLine.isEmpty())
        return false;

    if (onTopLane() || (! mixFocused() && focused >= session->numTracks()))
    {
        ioCell = -1;
        return false;
    }

    // The mix has inserts only (one row).
    const auto track = ioTrack();
    const auto lastCell = track < 0 ? ioFirstSend - 1 : ioOutput;
    const auto insertSlot = ioCell < ioFirstSend ? ioCell : -1;
    const auto sendSlot = ioCell >= ioFirstSend && ioCell < ioOutput ? ioCell - ioFirstSend : -1;

    switch (k.code)
    {
        case Key::Left:
        case Key::Right:
        {
            if (k.ctrl || k.alt)
                return false;

            const int step = k.code == Key::Right ? 1 : -1;

            // Shift moves the insert itself (and the cursor with it).
            if (k.shift)
            {
                if (insertSlot < 0 || insertSlot + step < 0 || insertSlot + step >= Session::numSends)
                    return true;

                beginEdit ("move insert");
                const auto r = swapInserts (track, insertSlot, insertSlot + step);
                ioCell += step;
                say (r);
                return true;
            }

            ioCell = juce::jlimit (0, lastCell, ioCell + step);
            followWindow (track, ioCell < ioFirstSend ? ioCell : -1);
            say (Result::good (describeIoCell (track, ioCell)));
            return true;
        }

        case Key::Up:
        case Key::Down:
        {
            if (k.ctrl || k.alt || k.shift)
                return false;

            // By row: a track's inserts, then its sends, then the next track's inserts. The
            // output only has the sends row, so it goes track to track. One row tall (no sends
            // row showing): track to track.
            const bool down = k.code == Key::Down;
            const bool rows = session->trackHeight() > 1;

            if (rows && insertSlot >= 0 && down && track >= 0)
                ioCell = ioFirstSend + insertSlot;
            else if (rows && sendSlot >= 0 && ! down)
                ioCell = ioFirstInsert + sendSlot;
            else
            {
                const auto before = focused;
                down ? focusDown (false) : focusUp (false);

                if (rows && focused != before && ! onTopLane())
                    ioCell = sendSlot >= 0 ? ioFirstInsert + sendSlot
                           : insertSlot >= 0 ? ioFirstSend + insertSlot
                                             : ioCell;

                // Onto the mix: the insert under the send (the output: its last insert).
                if (mixFocused() && ioCell >= ioFirstSend)
                    ioCell = ioCell == ioOutput ? ioFirstSend - 1 : ioCell - ioFirstSend;
            }

            if (! onTopLane())
            {
                followWindow (ioTrack(), ioCell < ioFirstSend ? ioCell : -1);
                say (Result::good (describeIoCell (ioTrack(), ioCell)));
            }
            return true;
        }

        case Key::Delete:
        case Key::Backspace:
            if (insertSlot >= 0)
            {
                beginEdit ("remove insert");
                say (removeInsert (track, insertSlot));
            }
            else if (ioCell == ioOutput)
            {
                beginEdit ("output");
                say (setOutputBus (track, 0));
            }
            else if (auto send = Session::send (session->track (track), sendSlot); send.isValid())
            {
                beginEdit ("remove send");
                session->track (track).removeChild (send, um());
                say (Result::good ("track " + juce::String (track + 1) + " send " + Session::slotName (sendSlot) + L" removed \x00b7 ctrl+z brings it back"));
            }
            return true;

        case Key::Escape:
            ioCell = -1;
            message.clear();
            return true;

        case Key::Char:
            if (k.ctrl || k.alt)
                return false;

            if (k.ch == '\\')
            {
                ioCell = -1;
                message.clear();
                return true;
            }

            if (insertSlot >= 0 && k.ch == 'M')
            {
                beginEdit ("bypass");
                say (toggleBypass (track, insertSlot));
                return true;
            }

            if (sendSlot >= 0 && (k.ch == '[' || k.ch == ']'))
            {
                beginEdit ("send level", "send:" + juce::String (track) + ":" + juce::String (sendSlot));
                say (nudgeSend (track, sendSlot, k.ch == ']' ? 1.0 : -1.0));
                return true;
            }

            if (sendSlot >= 0 && (k.ch == 'M' || k.ch == 'P'))
            {
                auto send = Session::send (session->track (track), sendSlot);

                if (! send.isValid())
                {
                    say (Result::fail ("send " + Session::slotName (sendSlot) + L" is empty \x2014 | picks a bus"));
                    return true;
                }

                beginEdit (k.ch == 'M' ? "send off" : "send pre");
                const auto& id = k.ch == 'M' ? ids::mute : ids::pre;
                send.setProperty (id, ! (bool) send[id], um());
                say (Result::good (describeIoCell (track, ioCell)));
                return true;
            }

            return false;

        default:
            return false;
    }
}
} // namespace taw
