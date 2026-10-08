// Automation on the message thread: which parameter a track's lane shows (its view), reading
// and naming values, and editing points. AUTO lanes hold POINTs (beat, val); the graph turns
// them into Curves (App::automationCurve) for the audio thread.

#include "App.h"

namespace taw
{
namespace
{
using Kind = AutomationTarget::Kind;

int slotOf (const juce::String& token) // "b" -> 1, else -1
{
    const auto t = token.toLowerCase();
    return t.length() == 1 && t[0] >= 'a' && t[0] < 'a' + Session::numSends ? (int) (t[0] - 'a') : -1;
}

struct Point
{
    double beat;
    double val;
};

std::vector<Point> readPoints (const juce::ValueTree& lane)
{
    std::vector<Point> out;

    for (const auto& p : Session::points (lane))
        out.push_back ({ (double) p[ids::beat], (double) p[ids::val] });

    return out;
}

// The curve between points, as Curve::at does it (in beats).
double valueIn (const std::vector<Point>& pts, double beat, bool steps, double otherwise)
{
    if (pts.empty())
        return otherwise;

    auto it = std::upper_bound (pts.begin(), pts.end(), beat, [] (double b, const Point& p) { return b < p.beat; });

    if (it == pts.begin())
        return pts.front().val;

    if (it == pts.end())
        return pts.back().val;

    const auto& a = *(it - 1);
    const auto& b = *it;
    return steps || b.beat == a.beat ? a.val : a.val + (b.val - a.val) * (beat - a.beat) / (b.beat - a.beat);
}

constexpr double sameBeat = 1.0e-6;
} // namespace

juce::ValueTree App::automationOwner (int track) const
{
    return session == nullptr ? juce::ValueTree() : track < 0 ? session->tree() : session->track (track);
}

// The plugin a lane's parameter belongs to (loading it if it isn't yet), or null.
PluginSlot* App::automationPlugin (const juce::ValueTree& owner, const AutomationTarget& target, bool load)
{
    if (! owner.hasType (ids::TRACK) && ! owner.hasType (ids::SESSION)) // the mix has inserts too
        return nullptr;

    const auto node = target.kind == Kind::instrument ? (Session::isInstrumentTrack (owner) ? owner : juce::ValueTree())
                      : target.kind == Kind::effect   ? Session::insert (owner, target.slot)
                                                      : juce::ValueTree();

    if (! node.isValid())
        return nullptr;

    auto* in = load ? hostedEntry (node) : liveEntry (node);
    return in != nullptr ? in->slot.get() : nullptr;
}

// What the parameter is when there's no automation: the fader, pan, mute or send as set, or
// the plugin's current value.
double App::staticValue (const juce::ValueTree& owner, const juce::String& param, bool load)
{
    const auto target = AutomationTarget::parse (param);

    switch (target.kind)
    {
        case Kind::volume: return owner.hasType (ids::SESSION) ? (double) owner[ids::mixDb] : (double) owner[ids::gainDb];
        case Kind::pan:    return owner[ids::pan];
        case Kind::mute:   return (bool) owner[ids::mute] ? 1.0 : 0.0;
        case Kind::send:
        {
            const auto send = Session::send (owner, target.slot);
            return send.isValid() ? (double) send[ids::db] : -96.0;
        }
        case Kind::instrument:
        case Kind::effect:
            if (auto* plugin = automationPlugin (owner, target, load))
                if (const auto index = plugin->findParameter (target.paramId); index >= 0)
                    return plugin->getParameter (index);

            return 0.5;
        case Kind::none:
        default:
            return 0.0;
    }
}

// The value right now, for the readouts: a controller moving it, else its automation where
// you are (what you hear while playing, the cursor when stopped), else its setting.
double App::currentValue (const juce::ValueTree& owner, const juce::String& param)
{
    if (session == nullptr)
        return 0.0;

    if (const auto lane = Session::automation (owner, param); lane.isValid())
    {
        for (const auto& t : touches)
            if (t.lane == lane)
                return t.live->value.load();

        if (lane.getNumChildren() > 0 && ! laneOff (lane))
            return automationValue (owner, param, engine.isRolling() ? heardPlayhead() : cursor);
    }

    return staticValue (owner, param, false);
}

bool App::isAutomated (const juce::ValueTree& owner, const juce::String& param) const
{
    const auto lane = Session::automation (owner, param);
    return lane.isValid() && ((lane.getNumChildren() > 0 && ! laneOff (lane)) || isTouched (lane));
}

double App::automationValue (const juce::ValueTree& owner, const juce::String& param, juce::int64 pos)
{
    const auto lane = Session::automation (owner, param);
    const auto points = readPoints (lane);
    return valueIn (points, (double) pos / session->samplesPerBeat(), AutomationTarget::parse (param).kind == Kind::mute,
                    points.empty() ? staticValue (owner, param) : 0.0);
}

juce::String App::automationName (const juce::ValueTree& owner, const juce::String& param, bool load)
{
    const auto target = AutomationTarget::parse (param);

    switch (target.kind)
    {
        case Kind::volume: return "volume";
        case Kind::pan:    return "pan";
        case Kind::mute:   return "mute";
        case Kind::send:   return "send " + Session::slotName (target.slot);
        case Kind::instrument:
        case Kind::effect:
        {
            const auto where = target.kind == Kind::instrument ? juce::String() : " (fx " + Session::slotName (target.slot) + ")";

            if (auto* plugin = automationPlugin (owner, target, load))
                for (const auto& p : plugin->parameters())
                    if (p.id == target.paramId)
                        return p.name + where;

            return "param " + target.paramId + where;
        }
        case Kind::none:
        default:
            return param;
    }
}

juce::String App::automationText (const juce::ValueTree& owner, const juce::String& param, double value, bool load)
{
    const auto target = AutomationTarget::parse (param);

    switch (target.kind)
    {
        case Kind::volume:
        case Kind::send:   return formatDb (value) + " dB";
        case Kind::pan:    return panText (value);
        case Kind::mute:   return value >= 0.5 ? "muted" : "on";
        case Kind::instrument:
        case Kind::effect:
            if (auto* plugin = automationPlugin (owner, target, load))
                if (const auto index = plugin->findParameter (target.paramId); index >= 0)
                    if (const auto text = plugin->parameterText (index, (float) value); text.isNotEmpty())
                        return text;

            return juce::String (juce::roundToInt (value * 100.0)) + "%";
        case Kind::none:
        default:
            return juce::String (value, 2);
    }
}

// "-6" "-inf" (dB) · "l30" "c" (pan) · "on" "off" (mute) · "40" "40%" "0.4" (plugins: 0-100%, or 0-1 with a point)
bool App::parseAutomationValue (const juce::String& param, const juce::String& textIn, double& out) const
{
    const auto target = AutomationTarget::parse (param);
    const auto text = textIn.trim().toLowerCase().removeCharacters (" ");

    if (text.isEmpty())
        return false;

    switch (target.kind)
    {
        case Kind::volume:
        case Kind::send:
            if (text == "-inf" || text == "inf")
                return (out = -96.0), true;

            if (! text.removeCharacters ("db").containsOnly ("+-0123456789."))
                return false;

            out = text.removeCharacters ("db").getDoubleValue();
            break;

        case Kind::pan:
        {
            const double sign = text.startsWithChar ('l') || text.startsWithChar ('<') || text.endsWithChar ('l') ? -1.0 : 1.0;
            const auto digits = text.removeCharacters ("lr<>+c");

            if (text != "c" && ! digits.containsOnly ("-0123456789."))
                return false;

            out = text == "c" ? 0.0 : sign * digits.getDoubleValue() / 100.0;
            break;
        }

        case Kind::mute:
            if (text == "on" || text == "1" || text == "muted" || text == "mute")
                out = 1.0;
            else if (text == "off" || text == "0" || text == "unmuted")
                out = 0.0;
            else
                return false;
            break;

        case Kind::instrument:
        case Kind::effect:
        {
            const auto digits = text.removeCharacters ("%");

            if (! digits.containsOnly ("0123456789."))
                return false;

            const auto v = digits.getDoubleValue();
            out = text.endsWithChar ('%') || ! text.containsChar ('.') ? v / 100.0 : v;
            break;
        }

        case Kind::none:
        default:
            return false;
    }

    out = target.clamp ((float) out);
    return true;
}

// The lane's points replaced by these (kept in beat order, so ties stay in the order given).
void App::writeLane (const juce::ValueTree& owner, const juce::String& param, std::vector<std::pair<double, double>> points)
{
    auto lane = Session::automation (owner, param);
    auto parent = owner;

    std::stable_sort (points.begin(), points.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });

    // An emptied lane stays under its track (and keeps its controller) until it's removed.
    if (points.empty())
    {
        if (lane.isValid())
            lane.removeAllChildren (um());

        return;
    }

    if (! lane.isValid())
    {
        lane = juce::ValueTree (ids::AUTO, { { ids::param, param } });
        parent.appendChild (lane, um());
    }

    lane.removeAllChildren (um());

    for (const auto& [beat, val] : points)
        lane.appendChild (Session::makePoint (beat, val), um());
}

// The track whose lane edits go to: the focused one (its view says which parameter).
Result App::automationTrack (int& track, juce::String& param)
{
    if (session == nullptr || onTopLane() || (! mixFocused() && focused >= session->numTracks()))
        return Result::fail (L"automation is on a track \x2014 \x2193 to one");

    track = ioTrack(); // -1: the mix
    param = focusedLaneParam();

    if (param.isEmpty())
        return Result::fail (L"\x2193 onto one of the track's lanes (shift+A opens them)");

    return Result::good();
}

// pt <value>: at the cursor, or the whole selection at that value (the curve outside it kept).
Result App::setAutomation (const juce::String& valueText)
{
    int track = -1;
    juce::String param;

    if (auto r = automationTrack (track, param); ! r.ok)
        return r;

    double value = 0.0;

    if (! parseAutomationValue (param, valueText, value))
        return Result::fail (L"pt -6 (dB) \x00b7 pt l30 (pan) \x00b7 pt on (mute) \x00b7 pt 40 (plugins, %)");

    return setAutomationValue (track, param, value);
}

// The point (or the selection's level) at a value, as pt does once it has parsed it.
Result App::setAutomationValue (int track, const juce::String& param, double value)
{
    const auto owner = automationOwner (track);
    const auto steps = AutomationTarget::parse (param).kind == Kind::mute;
    auto pts = readPoints (Session::automation (owner, param));
    const auto outside = pts.empty() ? staticValue (owner, param) : 0.0;
    const auto spb = session->samplesPerBeat();
    std::vector<std::pair<double, double>> out;

    if (hasTimeSelection())
    {
        const auto a = (double) timeSelection().getStart() / spb, b = (double) timeSelection().getEnd() / spb;
        const auto before = valueIn (pts, a, steps, outside), after = valueIn (pts, b, steps, outside);

        for (const auto& p : pts)
            if (p.beat < a - sameBeat || p.beat > b + sameBeat)
                out.push_back ({ p.beat, p.val });

        // The curve keeps its value up to the selection, jumps to the new one, and back after.
        if (std::abs (before - value) > 1.0e-9)
            out.push_back ({ a, before });

        out.push_back ({ a, value });
        out.push_back ({ b, value });

        if (std::abs (after - value) > 1.0e-9)
            out.push_back ({ b, after });

        writeLane (owner, param, out);
        return Result::good (automationName (owner, param) + " " + automationText (owner, param, value) + " from " + formatBars (timeSelection().getStart())
                             + L" \x2192 " + formatBars (timeSelection().getEnd()));
    }

    const auto at = (double) cursor / spb;

    for (const auto& p : pts)
        if (std::abs (p.beat - at) > sameBeat)
            out.push_back ({ p.beat, p.val });

    // A new lane starts where the parameter was set, at the session start, and goes to the
    // point from there (or before the point it would hold the point's value all along).
    if (pts.empty() && at > sameBeat)
        out.push_back ({ 0.0, outside });

    out.push_back ({ at, value });
    writeLane (owner, param, out);
    return Result::good (automationName (owner, param) + " " + automationText (owner, param, value) + " at " + formatBars (cursor));
}

// alt+up/down on a lane: the points in the selection up or down (no points there:
// the selection becomes one level, a step from where it was); no selection, the point at the
// cursor (made there if there isn't one).
Result App::nudgeAutomation (int direction)
{
    int track = -1;
    juce::String param;

    if (auto r = automationTrack (track, param); ! r.ok)
        return r;

    const auto owner = automationOwner (track);
    const auto target = AutomationTarget::parse (param);
    const auto steps = target.kind == Kind::mute;
    const auto step = target.isDecibels() ? 1.0 : target.kind == Kind::pan ? 0.05 : 0.01;
    auto pts = readPoints (Session::automation (owner, param));
    const auto outside = pts.empty() ? staticValue (owner, param) : 0.0;
    const auto spb = session->samplesPerBeat();
    auto shift = [&] (double v) { return steps ? (direction > 0 ? 1.0 : 0.0) : (double) target.clamp ((float) (v + direction * step)); };

    if (hasTimeSelection())
    {
        const auto a = (double) timeSelection().getStart() / spb, b = (double) timeSelection().getEnd() / spb;
        bool any = false;

        for (auto& p : pts)
            if (p.beat >= a - sameBeat && p.beat <= b + sameBeat)
                p.val = shift (p.val), any = true;

        if (! any)
            return setAutomationValue (track, param, shift (valueIn (pts, a, steps, outside)));

        std::vector<std::pair<double, double>> out;

        for (const auto& p : pts)
            out.push_back ({ p.beat, p.val });

        writeLane (owner, param, out);
        return Result::good (automationName (owner, param) + L" \x00b7 the points in the selection " + (direction > 0 ? "up" : "down"));
    }

    const auto at = (double) cursor / spb;

    for (auto& p : pts)
        if (std::abs (p.beat - at) <= sameBeat)
        {
            p.val = shift (p.val);
            std::vector<std::pair<double, double>> out;

            for (const auto& q : pts)
                out.push_back ({ q.beat, q.val });

            writeLane (owner, param, out);
            return Result::good (automationName (owner, param) + " " + automationText (owner, param, p.val) + " at " + formatBars (cursor));
        }

    const auto value = shift (valueIn (pts, at, steps, outside));
    std::vector<std::pair<double, double>> out;

    for (const auto& p : pts)
        out.push_back ({ p.beat, p.val });

    if (pts.empty() && at > sameBeat) // a new lane starts at the setting (as in setAutomation)
        out.push_back ({ 0.0, outside });

    out.push_back ({ at, value });
    writeLane (owner, param, out);
    return Result::good (automationName (owner, param) + " " + automationText (owner, param, value) + " at " + formatBars (cursor));
}

// Delete on a track showing automation: the points in the selection.
Result App::clearAutomation()
{
    int track = -1;
    juce::String param;

    if (auto r = automationTrack (track, param); ! r.ok)
        return r;

    if (! hasTimeSelection())
        return Result::fail (L"select the points to remove (shift + \x2190 \x2192)");

    const auto owner = automationOwner (track);
    const auto spb = session->samplesPerBeat();
    const auto a = (double) timeSelection().getStart() / spb, b = (double) timeSelection().getEnd() / spb;
    std::vector<std::pair<double, double>> out;
    int removed = 0;

    for (const auto& p : readPoints (Session::automation (owner, param)))
    {
        if (p.beat >= a - sameBeat && p.beat <= b + sameBeat)
            ++removed;
        else
            out.push_back ({ p.beat, p.val });
    }

    writeLane (owner, param, out);
    return Result::good ("removed " + juce::String (removed) + (removed == 1 ? " point" : " points") + " of " + automationName (owner, param)
                         + (out.empty() ? juce::String (L" \x00b7 no automation left: it's back to its setting") : juce::String()));
}

namespace
{
// The plugin a plugin parameter's lane belongs to on this track ("" for the mixer's).
juce::String pluginOf (const juce::ValueTree& track, const AutomationTarget& target)
{
    if (target.kind == Kind::instrument)
        return track[ids::plugin].toString();

    if (target.kind == Kind::effect)
        return Session::insert (track, target.slot)[ids::plugin].toString();

    return {};
}
} // namespace

// Each automated lane's curve from one beat to another: its value at the start, the points in
// between and its value at the end, relative to the start.
std::vector<Clipboard::Automation> App::gatherAutomation (const juce::ValueTree& track, double from, double to) const
{
    std::vector<Clipboard::Automation> out;

    for (const auto& lane : Session::automations (track))
    {
        const auto pts = readPoints (lane);

        if (pts.empty() || to - from <= sameBeat)
            continue;

        const auto param = lane[ids::param].toString();
        const auto target = AutomationTarget::parse (param);
        const auto steps = target.kind == Kind::mute;
        Clipboard::Automation a { param, pluginOf (track, target), {} };
        a.points.push_back ({ 0.0, valueIn (pts, from, steps, 0.0) });

        for (const auto& p : pts)
            if (p.beat > from + sameBeat && p.beat < to - sameBeat)
                a.points.push_back ({ p.beat - from, p.val });

        a.points.push_back ({ to - from, valueIn (pts, to - sameBeat, steps, 0.0) });
        out.push_back (std::move (a));
    }

    return out;
}

// The points from one beat to another gone; the curve either side stays as it was, joined
// across the gap.
void App::clearAutomationRange (const juce::ValueTree& track, double from, double to)
{
    for (const auto& lane : Session::automations (track))
    {
        const auto pts = readPoints (lane);

        if (std::none_of (pts.begin(), pts.end(), [&] (const Point& p) { return p.beat > from - sameBeat && p.beat < to + sameBeat; }))
            continue;

        const auto param = lane[ids::param].toString();
        const auto steps = AutomationTarget::parse (param).kind == Kind::mute;
        const auto before = valueIn (pts, from - sameBeat, steps, 0.0), after = valueIn (pts, to, steps, 0.0);
        std::vector<std::pair<double, double>> out;

        for (const auto& p : pts)
            if (p.beat <= from - sameBeat || p.beat >= to + sameBeat)
                out.push_back ({ p.beat, p.val });

        out.push_back ({ from, before });
        out.push_back ({ to, after });
        writeLane (track, param, out);
    }
}

// Copied curves put down at a beat, replacing what the lanes had there (and making lanes for
// them where there were none). The curve either side of the paste stays as it was.
void App::pasteAutomation (const juce::ValueTree& track, const std::vector<Clipboard::Automation>& autos, double at, double length)
{
    for (const auto& a : autos)
    {
        const auto target = AutomationTarget::parse (a.param);

        if (a.points.empty() || (target.isPlugin() && pluginOf (track, target) != a.plugin))
            continue;

        const auto steps = target.kind == Kind::mute;
        const auto old = readPoints (Session::automation (track, a.param));
        const auto outside = old.empty() ? staticValue (track, a.param) : 0.0;
        const auto before = valueIn (old, at - sameBeat, steps, outside), after = valueIn (old, at + length, steps, outside);
        std::vector<std::pair<double, double>> out;

        for (const auto& p : old)
            if (p.beat <= at - sameBeat || p.beat >= at + length + sameBeat)
                out.push_back ({ p.beat, p.val });

        // Ties keep this order: the old value up to the paste, the pasted curve, the old value after.
        if (at > sameBeat && std::abs (before - a.points.front().second) > 1.0e-4)
            out.push_back ({ at, before });

        for (const auto& [beat, val] : a.points)
            out.push_back ({ at + beat, val });

        if (std::abs (after - a.points.back().second) > 1.0e-4)
            out.push_back ({ at + length, after });

        writeLane (track, a.param, out);
    }
}

void App::moveAutomation (const juce::ValueTree& track, double from, double length, double to)
{
    if (std::abs (to - from) <= sameBeat)
        return;

    const auto autos = gatherAutomation (track, from, from + length);
    clearAutomationRange (track, from, from + length);
    pasteAutomation (track, autos, to, length);
}

// What's been written so far this pass, from its first point to the playhead (the controller
// holds its last value until it moves again), so the lane draws it as it records.
bool App::writtenValue (const juce::ValueTree& lane, juce::int64 pos, double& value)
{
    for (const auto& t : touches)
    {
        if (t.lane != lane || t.written.empty())
            continue;

        const auto& w = t.written;
        const auto end = writeOn && engine.isRolling() ? juce::jmax (w.back().first, heardPlayhead()) : w.back().first;

        if (pos < w.front().first || pos > end)
            return false;

        const auto it = std::upper_bound (w.begin(), w.end(), pos, [] (juce::int64 p, const auto& e) { return p < e.first; });

        if (it == w.end())
            value = w.back().second;
        else if (it == w.begin())
            value = w.front().second;
        else
        {
            const auto& [p0, v0] = *(it - 1);
            const auto& [p1, v1] = *it;
            const bool steps = AutomationTarget::parse (lane[ids::param].toString()).kind == Kind::mute;
            value = p1 == p0 || steps ? v0 : v0 + (v1 - v0) * (double) (pos - p0) / (double) (p1 - p0);
        }

        return true;
    }

    return false;
}

// lane vol | pan | mute | send a | <plugin parameter name>: a lane for it under the track
// (opened, and focused); "lane ?" lists the plugins' parameters.
Result App::laneByName (int trackIndex, const juce::String& whatIn)
{
    if (session == nullptr || trackIndex < -1 || trackIndex >= session->numTracks())
        return Result::fail (L"lanes are on tracks \x2014 \x2193 to one");

    const auto what = whatIn.trim().toLowerCase();
    juce::String param;

    if (what.isEmpty())
        return toggleLanes();

    if (what == "vol" || what == "volume")
        param = "vol";
    else if (trackIndex < 0 && (what == "pan" || what == "mute" || what.startsWith ("send")))
        return Result::fail ("the mix automates its volume and its inserts' parameters");
    else if (what == "pan" || what == "mute")
        param = what;
    else if (what.startsWith ("send") && slotOf (what.fromLastOccurrenceOf (" ", false, false).trim()) >= 0)
        param = "send " + what.getLastCharacters (1);
    else
    {
        // A plugin parameter, by (part of) its name: the instrument's, then the inserts' in order.
        const auto track = automationOwner (trackIndex);
        std::vector<std::pair<juce::String, PluginSlot*>> found;

        if (Session::isInstrumentTrack (track))
            found.push_back ({ "inst/", automationPlugin (track, AutomationTarget::parse ("inst/x")) });

        for (int s = 0; s < Session::numSends; ++s)
            if (Session::insert (track, s).isValid())
                found.push_back ({ "fx " + Session::slotName (s).toLowerCase() + "/",
                                     automationPlugin (track, AutomationTarget::parse ("fx " + Session::slotName (s).toLowerCase() + "/x")) });

        juce::String partial;

        for (const auto& [prefix, plugin] : found)
            if (plugin != nullptr)
                for (const auto& p : plugin->parameters())
                {
                    if (p.name.equalsIgnoreCase (what) && param.isEmpty())
                        param = prefix + p.id;
                    else if (partial.isEmpty() && p.name.toLowerCase().contains (what))
                        partial = prefix + p.id;
                }

        if (param.isEmpty() && what != "?")
            param = partial;

        // Not found (or "lane ?"): what there is.
        if (param.isEmpty())
        {
            juce::StringArray names;

            for (const auto& [prefix, plugin] : found)
                if (plugin != nullptr)
                    for (const auto& p : plugin->parameters())
                        names.addIfNotAlreadyThere (p.name.toLowerCase());

            const auto shown = names.size() > 14 ? juce::StringArray (names.begin(), 14) : names;
            const juce::String basics (trackIndex < 0 ? "lane vol" : "lane vol pan mute send a");
            return Result::fail ((what == "?" ? basics : "no parameter called " + whatIn.trim() + L" \x00b7 " + basics)
                                 + (names.isEmpty() ? juce::String() : L" \x00b7 " + shown.joinIntoString (", ") + (names.size() > 14 ? L" \x2026 (" + juce::String (names.size()) + ") \x00b7 auto lists them all" : juce::String())));
        }
    }

    // On one of the track's lanes, that lane shows it; from the track's row, a new lane.
    if (trackIndex == ioTrack() && focusedLaneParam().isNotEmpty())
        return changeLane (trackIndex, focusedLane, param);

    return showLane (trackIndex, param);
}

// The lanes under a track while they're open: its AUTO lanes in order, or a volume lane to
// start with when it has none.
std::vector<juce::String> App::lanesShown (int trackIndex) const
{
    if (session == nullptr || trackIndex < -1 || trackIndex >= session->numTracks())
        return {};

    const auto track = automationOwner (trackIndex); // -1: the mix's, under the Mix row

    if (! (bool) track[ids::lanes])
        return {};

    return laneList (track);
}

// What a track's lanes show (open or not): the list it keeps once a lane was added or changed,
// else its AUTO lanes in order, else a volume lane.
std::vector<juce::String> App::laneList (const juce::ValueTree& track) const
{
    std::vector<juce::String> out;

    if (track.hasProperty (ids::laneParams))
    {
        for (const auto& p : juce::StringArray::fromLines (track[ids::laneParams].toString()))
            if (p.isNotEmpty() && std::find (out.begin(), out.end(), p) == out.end())
                out.push_back (p);
    }
    else
    {
        for (const auto& lane : Session::automations (track))
            out.push_back (lane[ids::param].toString());
    }

    if (out.empty())
        out.push_back ("vol");

    return out;
}

void App::setLaneList (juce::ValueTree track, const std::vector<juce::String>& params)
{
    juce::StringArray lines;

    for (const auto& p : params)
        lines.add (p);

    track.setProperty (ids::laneParams, lines.joinIntoString ("\n"), nullptr); // a view, not an edit
}

// The lane at index shows param instead (its old parameter's automation stays, and plays);
// a lane already showing param swaps places with it.
Result App::changeLane (int trackIndex, int index, const juce::String& param)
{
    auto track = automationOwner (trackIndex);
    auto lanes = laneList (track);

    if (index < 0 || index >= (int) lanes.size())
        return showLane (trackIndex, param);

    const auto old = lanes[(size_t) index];

    if (const auto there = std::find (lanes.begin(), lanes.end(), param); there != lanes.end())
        *there = old;

    lanes[(size_t) index] = param;
    setLaneList (track, lanes);
    track.setProperty (ids::lanes, true, nullptr);
    focusTrack (trackIndex < 0 ? session->numTracks() : trackIndex);
    focusedLane = index;

    const auto lane = Session::automation (track, param);
    const auto count = lane.isValid() ? (int) Session::points (lane).size() : 0;
    const auto oldLane = Session::automation (track, old);
    const auto oldCount = oldLane.isValid() ? (int) Session::points (oldLane).size() : 0;
    return Result::good ("the lane shows " + automationName (track, param)
                         + (count > 0 ? L" \x00b7 " + juce::String (count) + (count == 1 ? " point" : " points") : juce::String (L" \x00b7 no points yet"))
                         + (old != param && oldCount > 0 ? L" \x00b7 " + automationName (track, old) + "'s automation still plays (auto lists it)" : juce::String()));
}

// The lane under the focused track that edits go to (empty: on the track's own row).
juce::String App::focusedLaneParam()
{
    if (session == nullptr || onTopLane() || focusedLane < 0)
        return {};

    const auto lanes = lanesShown (ioTrack());

    if (lanes.empty())
        return (focusedLane = -1), juce::String();

    focusedLane = juce::jmin (focusedLane, (int) lanes.size() - 1); // undo can take lanes away
    return lanes[(size_t) focusedLane];
}

// A lane for param under the track (made if it isn't there), the lanes open, the focus on it.
Result App::showLane (int trackIndex, const juce::String& param)
{
    auto track = automationOwner (trackIndex);
    auto lanes = laneList (track);

    // A track opening on its starting volume lane with nothing on it: that lane becomes this one.
    if (! (bool) track[ids::lanes] && lanes.size() == 1 && lanes.front() == "vol" && ! Session::automation (track, "vol").isValid())
        lanes.clear();

    if (std::find (lanes.begin(), lanes.end(), param) == lanes.end())
        lanes.push_back (param);

    setLaneList (track, lanes);
    track.setProperty (ids::lanes, true, nullptr); // open: a view, not an edit
    focusTrack (trackIndex < 0 ? session->numTracks() : trackIndex);
    focusedLane = (int) (std::find (lanes.begin(), lanes.end(), param) - lanes.begin());

    const auto lane = Session::automation (track, param);
    const auto count = lane.isValid() ? (int) Session::points (lane).size() : 0;
    return Result::good (automationName (track, param) + " lane on " + insertOwnerName (trackIndex)
                         + (count > 0 ? L" \x00b7 " + juce::String (count) + (count == 1 ? " point" : " points") : juce::String (L" \x00b7 no points yet: alt+\x2191\x2193 or pt <value>"))
                         + (lane.hasProperty (ids::num) ? L" \x00b7 " + controlText (lane) : juce::String (L" \x00b7 map: a knob runs it")));
}

// A lane off the track, its points and controller with it (undo brings it back).
Result App::removeLane (int trackIndex, const juce::String& param)
{
    auto track = automationOwner (trackIndex);
    const auto lane = Session::automation (track, param);
    auto lanes = laneList (track);
    const auto shown = std::find (lanes.begin(), lanes.end(), param);

    if (! lane.isValid() && shown == lanes.end())
        return Result::fail ("no " + automationName (track, param) + " lane");

    const auto name = automationName (track, param);

    if (shown != lanes.end())
    {
        lanes.erase (shown);
        setLaneList (track, lanes);
    }

    if (! lane.isValid())
        return Result::good ("closed the " + name + " lane");

    beginEdit ("remove lane");
    track.removeChild (lane, um());
    return Result::good ("removed the " + name + L" lane and its automation \x00b7 ctrl+z brings it back");
}

// shift+O on a lane: its automation off or on. Off, its points stay but don't play (nor
// bounce); the setting does, a mapped knob moves the setting, and writing passes it by. For
// trying values before writing again.
Result App::toggleLaneOff()
{
    int track = -1;
    juce::String param;

    if (auto r = automationTrack (track, param); ! r.ok)
        return r;

    auto owner = automationOwner (track);
    auto lane = Session::automation (owner, param);
    beginEdit ("automation off");

    if (! lane.isValid())
    {
        lane = juce::ValueTree (ids::AUTO, { { ids::param, param } });
        owner.appendChild (lane, um());
    }

    const bool off = ! laneOff (lane);

    if (off)
        lane.setProperty (ids::bypass, true, um());
    else
        lane.removeProperty (ids::bypass, um());

    return Result::good (automationName (owner, param) + (off ? juce::String (" automation OFF: plays its setting, isn't written") : juce::String (" automation on"))
                         + " (shift+O)");
}

// shift+A: the focused track's automation lanes, open under it or closed.
Result App::toggleLanes()
{
    if (session == nullptr || onTopLane() || (! mixFocused() && focused >= session->numTracks()))
        return Result::fail (L"shift+A opens a track's automation lanes \x2014 \x2193 to one");

    const auto who = ioTrack(); // the mix's lanes go under the Mix row
    auto track = automationOwner (who);
    const bool open = ! (bool) track[ids::lanes];
    track.setProperty (ids::lanes, open, nullptr); // a view, not an edit

    if (! open)
    {
        focusedLane = -1;
        return Result::good (insertOwnerName (who) + L" lanes closed \x00b7 shift+A opens them");
    }

    const auto lanes = lanesShown (who);
    return Result::good (insertOwnerName (who) + (lanes.size() == 1 ? " has 1 lane" : " has " + juce::String ((int) lanes.size()) + " lanes")
                         + L" \x00b7 \x2193 onto one \x00b7 auto adds more \x00b7 shift+A closes them");
}

//==============================================================================
// The automation list

Result App::openAutoPanel()
{
    if (session == nullptr || onTopLane() || (! mixFocused() && focused >= session->numTracks()))
        return Result::fail (L"auto lists a track's parameters \x2014 \x2193 to one");

    autoPanelTrack = ioTrack(); // -1: the mix
    autoPanelLane = focusedLaneParam().isNotEmpty() ? focusedLane : -1; // from a lane: enter changes it
    autoPanelFilter.clear();
    buildAutoPanelRows();
    autoPanelVisible = true;

    // Start on the lane you're on.
    const auto view = focusedLaneParam();
    autoPanelIndex = 0;

    for (int i = 0; i < (int) autoPanelRows.size(); ++i)
        if (! autoPanelRows[(size_t) i].header && autoPanelRows[(size_t) i].param == view)
        {
            autoPanelIndex = i;
            break;
        }

    while (autoPanelIndex < (int) autoPanelRows.size() && autoPanelRows[(size_t) autoPanelIndex].header)
        ++autoPanelIndex;

    return Result::good();
}

void App::buildAutoPanelRows()
{
    autoPanelRows.clear();

    if (session == nullptr || autoPanelTrack < -1 || autoPanelTrack >= session->numTracks())
        return;

    const auto track = automationOwner (autoPanelTrack);
    const bool mix = autoPanelTrack < 0;
    const auto filter = autoPanelFilter.trim().toLowerCase();
    auto matches = [&] (const juce::String& name, const juce::String& group)
    {
        return filter.isEmpty() || name.toLowerCase().contains (filter) || group.toLowerCase().contains (filter);
    };

    auto section = [&] (const juce::String& title, const std::vector<std::pair<juce::String, juce::String>>& params)
    {
        std::vector<AutoRow> rows;

        for (const auto& [param, name] : params)
            if (matches (name, title))
                rows.push_back ({ param, name, false });

        if (rows.empty())
            return;

        autoPanelRows.push_back ({ {}, title, true });
        autoPanelRows.insert (autoPanelRows.end(), rows.begin(), rows.end());
    };

    // What already has points or a controller, first.
    {
        std::vector<std::pair<juce::String, juce::String>> automated;

        for (const auto& lane : Session::automations (track))
            automated.push_back ({ lane[ids::param].toString(), automationName (track, lane[ids::param].toString()) });

        section (mix ? "ON THE MIX" : "ON THIS TRACK", automated);
    }

    // The mix: its volume only.
    std::vector<std::pair<juce::String, juce::String>> mixer { { "vol", "volume" } };

    if (! mix)
        mixer.insert (mixer.end(), { { "pan", "pan" }, { "mute", "mute" } });

    for (int s = 0; s < Session::numSends; ++s)
        if (Session::send (track, s).isValid())
            mixer.push_back ({ "send " + Session::slotName (s).toLowerCase(), "send " + Session::slotName (s) });

    section ("MIXER", mixer);

    auto pluginSection = [&] (const juce::String& prefix, const juce::String& title, const AutomationTarget& target)
    {
        if (auto* plugin = automationPlugin (track, target))
        {
            std::vector<std::pair<juce::String, juce::String>> params;

            for (const auto& p : plugin->parameters())
                params.push_back ({ prefix + p.id, p.name });

            section (title + " " + plugin->name().toUpperCase(), params);
        }
    };

    if (Session::isInstrumentTrack (track))
        pluginSection ("inst/", "INSTRUMENT", AutomationTarget::parse ("inst/x"));

    for (int s = 0; s < Session::numSends; ++s)
        if (Session::insert (track, s).isValid())
        {
            const auto letter = Session::slotName (s).toLowerCase();
            pluginSection ("fx " + letter + "/", "FX " + Session::slotName (s), AutomationTarget::parse ("fx " + letter + "/x"));
        }
}

bool App::handleAutoPanelKey (const Key& k)
{
    if (! autoPanelVisible)
        return false;

    const auto n = (int) autoPanelRows.size();

    // Up and down skip the headers.
    auto step = [&] (int direction, int times)
    {
        for (int t = 0; t < times; ++t)
        {
            auto i = autoPanelIndex + direction;

            while (i >= 0 && i < n && autoPanelRows[(size_t) i].header)
                i += direction;

            if (i < 0 || i >= n)
                break;

            autoPanelIndex = i;
        }
    };

    auto refilter = [&]
    {
        buildAutoPanelRows();
        autoPanelIndex = 0;
        step (+1, autoPanelRows.empty() || ! autoPanelRows.front().header ? 0 : 1);
    };

    switch (k.code)
    {
        case Key::Up:       step (-1, 1); return true;
        case Key::Down:     step (+1, 1); return true;
        case Key::PageUp:   step (-1, 10); return true;
        case Key::PageDown: step (+1, 10); return true;
        case Key::Escape:
            if (autoPanelFilter.isNotEmpty())
                autoPanelFilter.clear(), refilter();
            else
                autoPanelVisible = false;
            return true;
        case Key::Backspace:
            autoPanelFilter = autoPanelFilter.dropLastCharacters (1);
            refilter();
            return true;
        case Key::Enter:
            if (autoPanelIndex >= 0 && autoPanelIndex < n && ! autoPanelRows[(size_t) autoPanelIndex].header)
            {
                autoPanelVisible = false;
                const auto& param = autoPanelRows[(size_t) autoPanelIndex].param;
                say (autoPanelLane >= 0 ? changeLane (autoPanelTrack, autoPanelLane, param) : showLane (autoPanelTrack, param));
            }
            return true;
        case Key::Delete:
            // A lane the track has: off it (points, controller and all).
            if (autoPanelIndex >= 0 && autoPanelIndex < n && ! autoPanelRows[(size_t) autoPanelIndex].header
                && Session::automation (automationOwner (autoPanelTrack), autoPanelRows[(size_t) autoPanelIndex].param).isValid())
            {
                say (removeLane (autoPanelTrack, autoPanelRows[(size_t) autoPanelIndex].param));
                const auto keep = autoPanelIndex;
                buildAutoPanelRows();
                autoPanelIndex = juce::jlimit (0, juce::jmax (0, (int) autoPanelRows.size() - 1), keep);
                step (+1, autoPanelIndex < (int) autoPanelRows.size() && autoPanelRows[(size_t) autoPanelIndex].header ? 1 : 0);
            }
            return true;
        case Key::Char:
            if (k.ctrl || k.alt || k.ch < 32)
                return false;

            autoPanelFilter += juce::String::charToString ((juce::juce_wchar) k.ch);
            refilter();
            return true;
        default:
            return true;
    }
}

//==============================================================================
// Controllers

namespace
{
constexpr double releaseMs = 600.0;  // a knob can't say when it's let go: this long still, it is
constexpr double glideSeconds = 0.25; // after a touch, the curve comes back over this long
constexpr juce::int64 minPointGap = 441; // samples between written points (10 ms at 44.1k)
} // namespace

std::shared_ptr<LiveValue> App::liveValueFor (const juce::ValueTree& lane) const
{
    for (const auto& [l, value] : liveValues)
        if (l == lane)
            return value;

    liveValues.push_back ({ lane, std::make_shared<LiveValue>() });
    return liveValues.back().second;
}

bool App::isTouched (const juce::ValueTree& lane) const
{
    for (const auto& t : touches)
        if (t.lane == lane)
            return true;

    return false;
}

juce::String App::controlText (const juce::ValueTree& lane) const
{
    if (! lane.hasProperty (ids::num))
        return {};

    const auto ch = (int) lane[ids::ch];
    return "CC " + lane[ids::num].toString() + (ch > 0 ? " ch " + juce::String (ch) : juce::String());
}

// A controller's 0-127 as the parameter's value, on the same scale the lane draws.
double App::controlToValue (const juce::String& param, int cc) const
{
    const auto target = AutomationTarget::parse (param);
    const auto h = cc / 127.0;

    if (target.isDecibels())
        return cc == 0 ? -96.0 : h * 72.0 - 60.0; // -60 to +12 dB (0 dB at 106)

    if (target.kind == Kind::pan)
        return juce::jlimit (-1.0, 1.0, (cc - 64) / 63.0);

    if (target.kind == Kind::mute)
        return cc >= 64 ? 1.0 : 0.0;

    return h;
}

// The controllers the session's lanes listen to, for the MIDI inputs to send here.
void App::syncControlMap()
{
    if (midiInputs == nullptr || session == nullptr)
        return;

    std::vector<std::pair<int, int>> mapped;
    auto collect = [&] (const juce::ValueTree& owner)
    {
        for (const auto& lane : Session::automations (owner))
            if (lane.hasProperty (ids::num))
                mapped.push_back ({ (int) lane[ids::ch] - 1, (int) lane[ids::num] });
    };

    collect (session->tree());

    for (int t = 0; t < session->numTracks(); ++t)
        collect (session->track (t));

    midiInputs->controlRouting().set (mapped);

    // Values of lanes that went away (undo, a removed track) aren't needed any more.
    liveValues.erase (std::remove_if (liveValues.begin(), liveValues.end(), [] (const auto& e) { return ! e.first.getParent().isValid(); }),
                      liveValues.end());
}

void App::pollControls()
{
    if (midiInputs == nullptr || session == nullptr)
        return;

    for (ControlMsg m; midiInputs->popControl (m);)
        handleControl (m);

    releaseTouches (true);
}

void App::handleControl (const ControlMsg& m)
{
    const int channel = m.msg.channel() + 1, cc = m.msg.d1;

    // Learning: the first controller moved is the one.
    if (learning.track > -2)
    {
        const auto owner = automationOwner (learning.track);
        auto lane = Session::automation (owner, learning.param);
        auto parent = owner;
        beginEdit ("map");

        if (! lane.isValid())
        {
            lane = juce::ValueTree (ids::AUTO, { { ids::param, learning.param } });
            parent.appendChild (lane, um());
        }

        // A knob runs one parameter: learning it here takes it off the others.
        const auto moved = freeControl (cc, channel, lane);
        lane.setProperty (ids::num, cc, um());
        lane.setProperty (ids::ch, channel, um());
        midiInputs->controlRouting().learning = false;
        say (Result::good ("CC " + juce::String (cc) + " ch " + juce::String (channel) + L" \x2192 " + automationName (owner, learning.param)
                           + (learning.track >= 0 ? " on track " + juce::String (learning.track + 1) : juce::String (" on the mix"))
                           + (moved.isEmpty() ? juce::String() : " (was " + moved.joinIntoString (", ") + ")")
                           + L" \x00b7 shift+W arms writing, then play records it"));
        learning = {};
        syncControlMap(); // right away: the knob is probably still moving
        return;
    }

    const bool rolling = engine.isRolling();
    const auto now = juce::Time::getMillisecondCounterHiRes();
    // Where it was heard: the playhead as it sounded when the message arrived.
    const auto pos = juce::jmax<juce::int64> (0, heardPlayhead() - (juce::int64) ((now - m.ms) / 1000.0 * sampleRate()));

    auto apply = [&] (const juce::ValueTree& owner)
    {
        for (const auto& lane : Session::automations (owner))
        {
            if (! lane.hasProperty (ids::num) || (int) lane[ids::num] != cc || ((int) lane[ids::ch] > 0 && (int) lane[ids::ch] != channel))
                continue;

            const auto param = lane[ids::param].toString();
            const auto value = (float) controlToValue (param, m.msg.d2);
            auto moving = liveValueFor (lane);
            moving->value = value;
            moving->on = true;

            auto it = std::find_if (touches.begin(), touches.end(), [&] (const Touch& t) { return t.lane == lane; });

            if (it == touches.end())
            {
                touches.push_back ({ lane, owner, moving, now, {} });
                it = touches.end() - 1;
            }

            it->lastMs = now;

            // A lane that's off isn't written (the knob just moves the setting).
            if (writeOn && rolling && ! laneOff (lane))
            {
                auto& w = it->written;

                if (! w.empty() && pos - w.back().first < minPointGap && pos >= w.back().first)
                    w.back().second = value;
                else
                    w.push_back ({ pos, value });
            }
        }
    };

    apply (session->tree());

    for (int t = 0; t < session->numTracks(); ++t)
        apply (session->track (t));
}

// Touches let go of (or all of them, at a stop): their written moves into the lane, and the
// parameter back to its curve.
void App::releaseTouches (bool idleOnly)
{
    const auto now = juce::Time::getMillisecondCounterHiRes();

    for (auto it = touches.begin(); it != touches.end();)
    {
        if (idleOnly && now - it->lastMs < releaseMs)
        {
            ++it;
            continue;
        }

        if (it->written.empty())
            keepAsSetting (*it);
        else
            commitTouch (*it);

        it->live->on = false;
        it = touches.erase (it);
    }
}

// A controller moved on a lane with no points, not writing: like a fader, where you leave it
// is the setting (volume, pan, mute, the send; a plugin's parameter stays where it was put).
void App::keepAsSetting (const Touch& t)
{
    if (! t.lane.getParent().isValid() || (! Session::points (t.lane).empty() && ! laneOff (t.lane)))
        return;

    const auto param = t.lane[ids::param].toString();
    const auto target = AutomationTarget::parse (param);
    const auto value = (double) t.live->value.load();
    auto owner = t.owner;
    beginEdit ("controller", "control:" + param);

    switch (target.kind)
    {
        case Kind::volume: owner.setProperty (owner.hasType (ids::SESSION) ? ids::mixDb : ids::gainDb, value, um()); break;
        case Kind::pan:    owner.setProperty (ids::pan, value, um()); break;
        case Kind::mute:   owner.setProperty (ids::mute, value >= 0.5, um()); break;
        case Kind::send:
            if (auto send = Session::send (owner, target.slot); send.isValid())
                send.setProperty (ids::db, value, um());
            break;
        case Kind::instrument:
        case Kind::effect:
            if (auto* plugin = automationPlugin (owner, target, false))
                if (const auto index = plugin->findParameter (target.paramId); index >= 0)
                    plugin->setParameter (index, (float) value);
            break;
        case Kind::none:
        default:
            break;
    }
}

// Touch: the written stretch replaces the lane's points there, then the curve glides back to
// what it was.
void App::commitTouch (Touch& t)
{
    if (t.written.empty() || ! t.lane.getParent().isValid())
        return;

    const auto param = t.lane[ids::param].toString();
    const auto spb = session->samplesPerBeat();
    const auto steps = AutomationTarget::parse (param).kind == Kind::mute;
    const auto old = readPoints (t.lane);
    const auto outside = old.empty() ? staticValue (t.owner, param) : 0.0;
    const auto from = (double) t.written.front().first / spb;
    const auto to = (double) t.written.back().first / spb;
    const auto back = to + glideSeconds * sampleRate() / spb;
    std::vector<std::pair<double, double>> out;

    for (const auto& p : old)
        if (p.beat < from - sameBeat || p.beat > back + sameBeat)
            out.push_back ({ p.beat, p.val });

    // Where it was before the touch, so the curve up to it is unchanged.
    if (from > sameBeat)
        out.push_back ({ from - sameBeat, valueIn (old, from, steps, outside) });

    for (const auto& [pos, value] : t.written)
        out.push_back ({ (double) pos / spb, value });

    out.push_back ({ back, valueIn (old, back, steps, outside) });

    beginEdit ("automation write");
    writeLane (t.owner, param, out);
    say (Result::good ("wrote " + automationName (t.owner, param, false) + " " + formatBars (t.written.front().first) + L" \x2192 "
                       + formatBars (t.written.back().first) + L" \x00b7 ctrl+z undoes"));
}

// map: the next controller moved controls the parameter the focused track shows.
Result App::learnControl (const juce::String& arg)
{
    if (arg.equalsIgnoreCase ("off") || learning.track > -2)
    {
        const bool was = learning.track > -2;
        learning = {};

        if (midiInputs != nullptr)
            midiInputs->controlRouting().learning = false;

        return Result::good (was ? "stopped mapping" : "not mapping");
    }

    int track = -1;
    juce::String param;

    if (auto r = automationTrack (track, param); ! r.ok)
        return r;

    // map 74 (or map 74 2 for channel 2): that controller, without moving it.
    if (arg.containsOnly ("0123456789 ") && arg.trim().isNotEmpty())
    {
        const auto parts = juce::StringArray::fromTokens (arg.trim(), " ", {});
        const auto cc = parts[0].getIntValue(), ch = parts.size() > 1 ? parts[1].getIntValue() : 0;

        if (cc < 0 || cc > 127 || ch < 0 || ch > 16)
            return Result::fail ("map 74 (controller 0-127), map 74 2 (on channel 2)");

        auto owner = automationOwner (track);
        auto lane = Session::automation (owner, param);

        if (! lane.isValid())
        {
            lane = juce::ValueTree (ids::AUTO, { { ids::param, param } });
            owner.appendChild (lane, um());
        }

        const auto moved = freeControl (cc, ch, lane);
        lane.setProperty (ids::num, cc, um());

        if (ch > 0)
            lane.setProperty (ids::ch, ch, um());
        else
            lane.removeProperty (ids::ch, um());

        return Result::good ("CC " + juce::String (cc) + (ch > 0 ? " ch " + juce::String (ch) : juce::String()) + L" \x2192 " + automationName (owner, param)
                             + (moved.isEmpty() ? juce::String() : " (was " + moved.joinIntoString (", ") + ")"));
    }

    if (midiInputs == nullptr)
        return Result::fail ("no MIDI inputs here: map 74 names the controller instead");

    learning = { track, param };
    midiInputs->controlRouting().learning = true;
    return Result::good (L"move a knob or fader on your controller for " + automationName (automationOwner (track), param) + L" \x00b7 map again cancels");
}

// A controller off every lane it runs but keep (ch 0: any channel; a lane on any channel
// clashes with every channel). Returns what it ran: "pan on track 2".
juce::StringArray App::freeControl (int cc, int ch, const juce::ValueTree& keep)
{
    juce::StringArray freed;

    auto free = [&] (const juce::ValueTree& owner, const juce::String& where)
    {
        for (auto lane : Session::automations (owner))
        {
            const int laneCh = lane[ids::ch];

            if (lane == keep || ! lane.hasProperty (ids::num) || (int) lane[ids::num] != cc || (ch > 0 && laneCh > 0 && laneCh != ch))
                continue;

            lane.removeProperty (ids::num, um());
            lane.removeProperty (ids::ch, um());
            freed.add (automationName (owner, lane[ids::param].toString(), false) + where);
        }
    };

    free (session->tree(), " on the mix");

    for (int t = 0; t < session->numTracks(); ++t)
        free (session->track (t), " on track " + juce::String (t + 1));

    return freed;
}

// unmap: every controller off every lane. unmap 74 (unmap 74 2): that controller, wherever
// it's mapped. unmap lane: the lane you're on lets its controller go.
Result App::unmapControl (const juce::String& argIn)
{
    const auto arg = argIn.trim();

    if (session != nullptr && (arg.isEmpty() || arg.equalsIgnoreCase ("all")))
    {
        int n = 0;

        for (int cc = 0; cc < 128; ++cc)
            n += freeControl (cc, 0, {}).size();

        return n == 0 ? Result::fail ("no controllers are mapped") : Result::good ("unmapped " + juce::String (n) + (n == 1 ? " lane" : " lanes"));
    }

    if (session != nullptr && ! arg.equalsIgnoreCase ("lane"))
    {
        const auto parts = juce::StringArray::fromTokens (arg, " ", {});
        const auto cc = parts[0].getIntValue(), ch = parts.size() > 1 ? parts[1].getIntValue() : 0;

        if (! arg.containsOnly ("0123456789 ") || cc < 0 || cc > 127 || ch < 0 || ch > 16)
            return Result::fail ("unmap (every controller) | unmap 74 (that controller) | unmap lane (the lane you're on)");

        const auto freed = freeControl (cc, ch, {});
        return freed.isEmpty() ? Result::fail ("CC " + juce::String (cc) + " isn't mapped")
                               : Result::good ("CC " + juce::String (cc) + " no longer moves " + freed.joinIntoString (", "));
    }

    int track = -1;
    juce::String param;

    if (auto r = automationTrack (track, param); ! r.ok)
        return r;

    auto owner = automationOwner (track);
    auto lane = Session::automation (owner, param);

    if (! lane.isValid() || ! lane.hasProperty (ids::num))
        return Result::fail ("no controller on " + automationName (owner, param));

    const auto was = controlText (lane);
    lane.removeProperty (ids::num, um());
    lane.removeProperty (ids::ch, um());
    return Result::good (was + " no longer moves " + automationName (owner, param));
}

Result App::setWrite (const juce::String& arg)
{
    if (arg.equalsIgnoreCase ("on"))
        writeOn = true;
    else if (arg.equalsIgnoreCase ("off"))
        writeOn = false;
    else if (arg.isEmpty())
        writeOn = ! writeOn;
    else
        return Result::fail ("aw | aw on | aw off (or shift+W)");

    if (! writeOn)
        releaseTouches();

    return Result::good (writeOn ? juce::String (L"automation write on \x00b7 playing, a mapped controller you move is recorded (touch) \x00b7 shift+W again: off")
                                 : juce::String (L"automation write off \x00b7 controllers move their parameters without recording"));
}
} // namespace taw
