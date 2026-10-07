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

juce::String App::viewOf (int track) const
{
    return track >= 0 && session != nullptr && track < session->numTracks() ? session->track (track)[ids::view].toString() : juce::String();
}

// The plugin a lane's parameter belongs to (loading it if it isn't yet), or null.
PluginSlot* App::automationPlugin (const juce::ValueTree& owner, const AutomationTarget& target, bool load)
{
    if (! owner.hasType (ids::TRACK))
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

    if (points.empty())
    {
        if (lane.isValid())
            parent.removeChild (lane, um());

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
Result App::automationTrack (int& track, juce::String& param) const
{
    if (session == nullptr || onTopLane() || mixFocused() || focused >= session->numTracks())
        return Result::fail (L"automation is on a track \x2014 \x2193 to one");

    track = focused;
    param = viewOf (track);

    if (param.isEmpty())
        return Result::fail (L"the track shows its clips \x00b7 shift+A (or view vol) shows automation");

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
        return Result::fail ("pt -6 (dB) · pt l30 (pan) · pt on (mute) · pt 40 (plugins, %)");

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

// [ ] on a track showing automation: the points in the selection up or down (no points there:
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
            return setAutomation (automationText (owner, param, shift (valueIn (pts, a, steps, outside))));

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

// view clips | vol | pan | mute | send a | <plugin parameter name> : what the tracks' lanes show.
Result App::setView (const std::vector<int>& tracks, const juce::String& whatIn)
{
    if (tracks.empty())
        return Result::fail (L"view is for tracks \x2014 \x2193 to one");

    const auto what = whatIn.trim().toLowerCase();
    juce::String param;

    if (what.isEmpty() || what == "clips" || what == "notes" || what == "off")
        param = {};
    else if (what == "vol" || what == "volume")
        param = "vol";
    else if (what == "pan" || what == "mute")
        param = what;
    else if (what.startsWith ("send") && slotOf (what.fromLastOccurrenceOf (" ", false, false).trim()) >= 0)
        param = "send " + what.getLastCharacters (1);
    else
    {
        // A plugin parameter, by (part of) its name: the instrument's, then the inserts' in order.
        const auto track = session->track (tracks.front());
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

        // Not found (or "view ?"): what there is to show.
        if (param.isEmpty())
        {
            juce::StringArray names;

            for (const auto& [prefix, plugin] : found)
                if (plugin != nullptr)
                    for (const auto& p : plugin->parameters())
                        names.addIfNotAlreadyThere (p.name.toLowerCase());

            const auto shown = names.size() > 14 ? juce::StringArray (names.begin(), 14) : names;
            return Result::fail ((what == "?" ? juce::String ("view: vol pan mute send a") : "no parameter called " + whatIn.trim() + L" \x00b7 view vol pan mute send a")
                                 + (names.isEmpty() ? juce::String() : L" \x00b7 " + shown.joinIntoString (", ") + (names.size() > 14 ? L" \x2026 (" + juce::String (names.size()) + ")" : juce::String())));
        }
    }

    for (auto t : tracks)
    {
        auto track = session->track (t);

        if (param.isEmpty())
            track.removeProperty (ids::view, nullptr); // a view, not an edit
        else
            track.setProperty (ids::view, param, nullptr);
    }

    if (param.isEmpty())
        return Result::good (trackList (tracks) + (tracks.size() == 1 ? " shows its clips" : " show their clips"));

    const auto owner = automationOwner (tracks.front());
    const auto lane = Session::automation (owner, param);
    const auto count = lane.isValid() ? (int) Session::points (lane).size() : 0;
    return Result::good (trackList (tracks) + " shows " + automationName (owner, param)
                         + (count == 0 ? juce::String (L" \x00b7 no points yet: pt <value> or [ ] add them")
                                       : L" \x00b7 " + juce::String (count) + (count == 1 ? " point" : " points")));
}

// shift+A: the focused track's lane through clips, volume, pan, mute, its sends and the
// parameters it has automation for, and back to clips.
Result App::cycleView()
{
    if (session == nullptr || onTopLane() || mixFocused() || focused >= session->numTracks())
        return Result::fail (L"shift+A shows a track's automation \x2014 \x2193 to one");

    const auto track = session->track (focused);
    juce::StringArray views { "", "vol", "pan", "mute" };

    for (int s = 0; s < Session::numSends; ++s)
        if (Session::send (track, s).isValid())
            views.add ("send " + Session::slotName (s).toLowerCase());

    for (const auto& lane : Session::automations (track))
        views.addIfNotAlreadyThere (lane[ids::param].toString());

    const auto next = views[(views.indexOf (viewOf (focused)) + 1) % views.size()];
    auto t = session->track (focused);

    if (next.isEmpty())
    {
        t.removeProperty (ids::view, nullptr); // a view, not an edit
        return Result::good (trackList ({ focused }) + L" shows its clips \x00b7 shift+A again: its automation");
    }

    t.setProperty (ids::view, next, nullptr);
    const auto lane = Session::automation (t, next);
    const auto count = lane.isValid() ? (int) Session::points (lane).size() : 0;
    return Result::good (trackList ({ focused }) + " shows " + automationName (t, next)
                         + (count == 0 ? juce::String (L" \x00b7 no points yet: [ ] or pt <value> add them") : L" \x00b7 " + juce::String (count) + (count == 1 ? " point" : " points")));
}
} // namespace taw
