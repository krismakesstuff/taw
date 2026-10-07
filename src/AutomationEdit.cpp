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
        // A lane a controller is mapped to stays (empty), so the mapping does.
        if (lane.isValid() && lane.hasProperty (ids::num))
            lane.removeAllChildren (um());
        else if (lane.isValid())
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
        return Result::fail (L"pt -6 (dB) \x00b7 pt l30 (pan) \x00b7 pt on (mute) \x00b7 pt 40 (plugins, %)");

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

//==============================================================================
// The automation list

Result App::openAutoPanel()
{
    if (session == nullptr || onTopLane() || mixFocused() || focused >= session->numTracks())
        return Result::fail (L"auto lists a track's parameters \x2014 \x2193 to one");

    autoPanelTrack = focused;
    autoPanelFilter.clear();
    buildAutoPanelRows();
    autoPanelVisible = true;

    // Start on what the track shows now.
    const auto view = viewOf (focused);
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

    if (session == nullptr || autoPanelTrack < 0 || autoPanelTrack >= session->numTracks())
        return;

    const auto track = session->track (autoPanelTrack);
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

        section ("ON THIS TRACK", automated);
    }

    std::vector<std::pair<juce::String, juce::String>> mixer { { "vol", "volume" }, { "pan", "pan" }, { "mute", "mute" } };

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
                const auto param = autoPanelRows[(size_t) autoPanelIndex].param;
                auto track = session->track (autoPanelTrack);
                track.setProperty (ids::view, param, nullptr); // a view, not an edit
                autoPanelVisible = false;
                const auto lane = Session::automation (track, param);
                const auto count = lane.isValid() ? (int) Session::points (lane).size() : 0;
                say (Result::good (trackList ({ autoPanelTrack }) + " shows " + automationName (track, param)
                                   + (count > 0 ? L" \x00b7 " + juce::String (count) + (count == 1 ? " point" : " points") : juce::String (L" \x00b7 no points yet"))
                                   + (lane.hasProperty (ids::num) ? L" \x00b7 " + controlText (lane) : juce::String (L" \x00b7 learn maps a knob to it"))));
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
        beginEdit ("learn");

        if (! lane.isValid())
        {
            lane = juce::ValueTree (ids::AUTO, { { ids::param, learning.param } });
            parent.appendChild (lane, um());
        }

        lane.setProperty (ids::num, cc, um());
        lane.setProperty (ids::ch, channel, um());
        midiInputs->controlRouting().learning = false;
        say (Result::good ("CC " + juce::String (cc) + " ch " + juce::String (channel) + L" \x2192 " + automationName (owner, learning.param)
                           + (learning.track >= 0 ? " on track " + juce::String (learning.track + 1) : juce::String (" on the mix"))
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

            if (writeOn && rolling)
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
    if (! t.lane.getParent().isValid() || ! Session::points (t.lane).empty())
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

// learn: the next controller moved controls the parameter the focused track shows.
Result App::learnControl (const juce::String& arg)
{
    if (arg.equalsIgnoreCase ("off") || learning.track > -2)
    {
        const bool was = learning.track > -2;
        learning = {};

        if (midiInputs != nullptr)
            midiInputs->controlRouting().learning = false;

        return Result::good (was ? "stopped learning" : "not learning");
    }

    int track = -1;
    juce::String param;

    if (auto r = automationTrack (track, param); ! r.ok)
        return r;

    // learn 74 (or learn 74 2 for channel 2): that controller, without moving it.
    if (arg.containsOnly ("0123456789 ") && arg.trim().isNotEmpty())
    {
        const auto parts = juce::StringArray::fromTokens (arg.trim(), " ", {});
        const auto cc = parts[0].getIntValue(), ch = parts.size() > 1 ? parts[1].getIntValue() : 0;

        if (cc < 0 || cc > 127 || ch < 0 || ch > 16)
            return Result::fail ("learn 74 (controller 0-127), learn 74 2 (on channel 2)");

        auto owner = automationOwner (track);
        auto lane = Session::automation (owner, param);

        if (! lane.isValid())
        {
            lane = juce::ValueTree (ids::AUTO, { { ids::param, param } });
            owner.appendChild (lane, um());
        }

        lane.setProperty (ids::num, cc, um());

        if (ch > 0)
            lane.setProperty (ids::ch, ch, um());
        else
            lane.removeProperty (ids::ch, um());

        return Result::good ("CC " + juce::String (cc) + (ch > 0 ? " ch " + juce::String (ch) : juce::String()) + L" \x2192 " + automationName (owner, param));
    }

    if (midiInputs == nullptr)
        return Result::fail ("no MIDI inputs here: learn 74 names the controller instead");

    learning = { track, param };
    midiInputs->controlRouting().learning = true;
    return Result::good (L"move a knob or fader on your controller for " + automationName (automationOwner (track), param) + L" \x00b7 learn again cancels");
}

Result App::unmapControl()
{
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

    if (Session::points (lane).empty())
        owner.removeChild (lane, um());

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
