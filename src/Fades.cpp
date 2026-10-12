// Clip fades and crossfades on audio tracks. A fade is a property of its clip (fadeIn, fadeOut
// and their shapes, in samples); over another clip it's a crossfade with it (renderClips). So a
// crossfade is two clips overlapping, the one on top fading over the one under it: making one
// stretches both into their handles (the audio past their edges in their files) until they
// overlap across the selection.

#include "App.h"

namespace taw
{
namespace
{
const wchar_t* shapeWords (FadeShape s)
{
    switch (s)
    {
        case FadeShape::linear:      return L"linear";
        case FadeShape::sCurve:      return L"S-curve";
        case FadeShape::exponential: return L"exponential";
        case FadeShape::equalPower:  break;
    }

    return L"equal power";
}

FadeShape nextShape (FadeShape s) { return (FadeShape) (((int) s + 1) % 4); }

bool isShapeName (const juce::String& word)
{
    for (int s = 0; s < 4; ++s)
        if (word.equalsIgnoreCase (fadeShapeName ((FadeShape) s)))
            return true;

    return false;
}
} // namespace

struct App::FadePlan
{
    enum Kind { in, out, cross };

    struct Op
    {
        Kind kind;
        juce::ValueTree clip;  // in, out: the clip; cross: the earlier (left) clip
        juce::ValueTree right; // cross: the later (right) clip
        bool rightOnTop = true; // cross: which one fades over the other
        juce::int64 from = 0, to = 0; // the fade's span on the timeline
        int track = 0;
    };

    std::vector<Op> ops;
};

juce::int64 App::lengthInSamples (const juce::String& spec) const
{
    const auto s = spec.trim().toLowerCase();

    if (s.isEmpty())
        return -1;

    double seconds = -1.0;

    if (s.containsChar ('/'))
    {
        // A note value: 1/4 is a quarter note (a beat), 3/16 three sixteenths.
        const auto num = s.upToFirstOccurrenceOf ("/", false, false).getDoubleValue();
        const auto den = s.fromFirstOccurrenceOf ("/", false, false).getDoubleValue();

        if (num > 0.0 && den > 0.0)
            return std::llround (4.0 * num / den * session->samplesPerBeat());
    }
    else if (s.endsWith ("ms"))
        seconds = s.dropLastCharacters (2).getDoubleValue() * 0.001;
    else if (s.endsWith ("s"))
        seconds = s.dropLastCharacters (1).getDoubleValue();
    else if (s.containsOnly ("0123456789."))
        seconds = s.getDoubleValue() * 0.001;

    return seconds > 0.0 ? std::llround (seconds * sampleRate()) : -1;
}

juce::ValueTree App::clipUnderCursor() const
{
    if (session == nullptr || onTopLane() || mixFocused() || focused >= session->numTracks())
        return {};

    const auto at = engine.isRolling() ? heardPlayhead() : cursor;
    const auto laneClips = Session::clips (clipHolder (focused));

    for (auto it = laneClips.rbegin(); it != laneClips.rend(); ++it) // the one on top
        if (const auto s = (juce::int64) (*it)[ids::start]; s <= at && s + (juce::int64) (*it)[ids::length] > at)
            return *it;

    return {};
}

// What ctrl+f makes over range on one holder (a track, or one of its playlists):
//   - a crossfade where two clips meet (touch or overlap) inside it, one starting before the
//     range and the other ending after it. Both need enough handle to reach across.
//   - otherwise a fade in on a clip starting in it and ending after it, a fade out on a clip
//     starting before it and ending in it.
Result App::planFades (juce::ValueTree holder, juce::Range<juce::int64> range, FadePlan& plan)
{
    const auto laneClips = Session::clips (holder);
    const auto a = range.getStart(), b = range.getEnd();
    std::set<int> fadingIn, fadingOut; // clips whose start or end a crossfade already takes

    auto startOf = [&] (int i) { return (juce::int64) laneClips[(size_t) i][ids::start]; };
    auto endOf = [&] (int i) { return startOf (i) + (juce::int64) laneClips[(size_t) i][ids::length]; };

    for (int l = 0; l < (int) laneClips.size(); ++l)
        for (int r = 0; r < (int) laneClips.size(); ++r)
        {
            if (l == r || fadingOut.count (l) > 0 || fadingIn.count (r) > 0)
                continue;

            const auto sL = startOf (l), eL = endOf (l), sR = startOf (r), eR = endOf (r);

            if (! (sL < a && eR > b && eL > a && sR < b && sR <= eL && sL < sR && eL < eR))
                continue;

            // The left clip reaches on to b, the right one back to a.
            const auto& left = laneClips[(size_t) l];
            const auto& right = laneClips[(size_t) r];
            const auto* leftData = clipFor (left);
            const auto leftHandle = leftData == nullptr ? 0 : (juce::int64) leftData->audio->getNumSamples() - (juce::int64) left[ids::offset] - (eL - sL);
            const auto rightHandle = (juce::int64) right[ids::offset];
            const auto ms = [this] (juce::int64 n) { return juce::String (juce::jmax (0.0, (double) n / sampleRate() * 1000.0), 0) + " ms"; };

            if (b - eL > leftHandle)
                return Result::fail ("only " + ms (leftHandle) + " of audio past the left clip's end at " + formatBars (eL)
                                     + L" \x2014 select less after it");

            if (sR - a > rightHandle)
                return Result::fail ("only " + ms (rightHandle) + " of audio before the right clip's start at " + formatBars (sR)
                                     + L" \x2014 select less before it");

            plan.ops.push_back ({ FadePlan::cross, left, right, r > l, a, b, 0 });
            fadingOut.insert (l);
            fadingIn.insert (r);
        }

    for (int i = 0; i < (int) laneClips.size(); ++i)
    {
        const auto s = startOf (i), e = endOf (i);

        if (fadingIn.count (i) == 0 && s >= a && s < b && e > b)
            plan.ops.push_back ({ FadePlan::in, laneClips[(size_t) i], {}, true, s, b, 0 });
        else if (fadingOut.count (i) == 0 && s < a && e > a && e <= b)
            plan.ops.push_back ({ FadePlan::out, laneClips[(size_t) i], {}, true, a, e, 0 });
    }

    return Result::good();
}

// Makes the planned fades with shape, or with each fade's own shape (equal power for a new one).
// Without a shape, when every planned fade is already there just so, each moves on to its next
// curve instead: ctrl+f again on the same selection.
Result App::applyFades (FadePlan& plan, std::optional<FadeShape> shape)
{
    auto fadeOf = [] (const FadePlan::Op& op, bool& fadesIn) -> juce::ValueTree
    {
        fadesIn = op.kind == FadePlan::in || (op.kind == FadePlan::cross && op.rightOnTop);
        return op.kind == FadePlan::cross && op.rightOnTop ? op.right : op.clip;
    };

    auto isThere = [&] (const FadePlan::Op& op)
    {
        bool fadesIn = false;
        const auto c = fadeOf (op, fadesIn);
        const auto len = (juce::int64) c[fadesIn ? ids::fadeIn : ids::fadeOut];

        if (op.kind == FadePlan::cross)
            return len == op.to - op.from && (juce::int64) op.right[ids::start] == op.from
                   && (juce::int64) op.clip[ids::start] + (juce::int64) op.clip[ids::length] == op.to;

        return len == op.to - op.from;
    };

    const bool cycle = ! shape.has_value() && std::all_of (plan.ops.begin(), plan.ops.end(), isThere);
    FadeShape said = FadeShape::equalPower;
    int ins = 0, outs = 0, crosses = 0;
    juce::int64 lastLength = 0;
    std::set<int> on;

    for (auto& op : plan.ops)
    {
        if (op.kind == FadePlan::cross)
        {
            auto& left = op.clip;
            auto& right = op.right;
            const auto sL = (juce::int64) left[ids::start], sR = (juce::int64) right[ids::start];
            const auto eR = sR + (juce::int64) right[ids::length];
            left.setProperty (ids::length, op.to - sL, um());
            right.setProperty (ids::offset, (juce::int64) right[ids::offset] - (sR - op.from), um())
                .setProperty (ids::start, op.from, um())
                .setProperty (ids::length, eR - op.from, um());

            // Only the top clip fades; a fade on the one under would dip it twice.
            auto under = op.rightOnTop ? left : right;
            under.removeProperty (op.rightOnTop ? ids::fadeOut : ids::fadeIn, um());
            under.removeProperty (op.rightOnTop ? ids::fadeOutShape : ids::fadeInShape, um());
            ++crosses;
        }
        else
            ++(op.kind == FadePlan::in ? ins : outs);

        bool fadesIn = false;
        auto c = fadeOf (op, fadesIn);
        const auto& shapeId = fadesIn ? ids::fadeInShape : ids::fadeOutShape;
        const auto current = fadeShapeFromName (c[shapeId]);
        said = cycle ? nextShape (current) : shape.value_or (current);
        c.setProperty (fadesIn ? ids::fadeIn : ids::fadeOut, op.to - op.from, um());
        c.setProperty (shapeId, juce::String (fadeShapeName (said)), um());
        lastLength = op.to - op.from;
        on.insert (op.track);
    }

    // "crossfade 40 ms, equal power on track 2" or "2 fades in, 1 crossfade, linear on tracks 1,3".
    juce::StringArray parts;
    auto count = [&] (int n, const juce::String& one, const juce::String& many)
    {
        if (n > 0)
            parts.add (n == 1 ? one : juce::String (n) + " " + many);
    };

    count (ins, "fade in", "fades in");
    count (outs, "fade out", "fades out");
    count (crosses, "crossfade", "crossfades");
    juce::StringArray tracks;

    for (auto t : on)
        tracks.add (trackLabel (t));

    auto what = parts.joinIntoString (", ");

    if (plan.ops.size() == 1)
        what << " " << juce::String ((double) lastLength / sampleRate() * 1000.0, 0) << " ms";

    return Result::good (what + ", " + shapeWords (said) + " on " + (tracks.size() == 1 ? "track " : "tracks ") + tracks.joinIntoString (",")
                         + (shape.has_value() ? juce::String() : juce::String (L" \x00b7 ctrl+f again: ") + shapeWords (nextShape (said))));
}

Result App::fadeSelection (const std::vector<int>& tracks)
{
    if (auto r = checkEditable (tracks, true); ! r.ok)
        return Result::fail (hasTimeSelection() ? r.message : juce::String (L"select a range to fade (shift + \x2190 \x2192): from a clip's start, to its end, or across where two meet"));

    FadePlan plan;
    bool anyAudio = false;

    for (auto t : tracks)
    {
        const auto track = session->track (t);

        if (! Session::isAudioTrack (track))
            continue;

        anyAudio = true;
        const auto before = plan.ops.size();

        if (auto r = planFades (clipHolder (t), timeSelection(), plan); ! r.ok)
            return Result::fail ("track " + trackLabel (t) + ": " + r.message);

        for (auto i = before; i < plan.ops.size(); ++i)
            plan.ops[i].track = t;
    }

    if (! anyAudio)
        return Result::fail ("fades are for audio clips; choose an audio track");

    if (plan.ops.empty())
        return Result::fail ("nothing to fade: select from a clip's start into it, from inside it to its end, or across where two clips meet");

    return applyFades (plan, std::nullopt);
}

// clip fade in 10ms [shape] / clip fade out 1/4 [shape] / clip fade off: the clip under the cursor;
// clip fade alone: as ctrl+f.
Result App::clipCommand (const juce::StringArray& args)
{
    static const auto usage = L"clip fade in 10ms \x00b7 clip fade out 1/4 \x00b7 clip fade off (shapes: eqp lin s exp)";

    if (! args[0].equalsIgnoreCase ("fade"))
        return Result::fail (usage);

    if (args[1].isEmpty()) // as ctrl+f: from the selection
        return fadeSelection (defaultTargets());

    auto clip = clipUnderCursor();

    if (! clip.isValid())
        return Result::fail ("no audio clip under the cursor on the focused track");

    const auto which = args[1].toLowerCase();

    if (which == "off")
    {
        for (const auto& id : { ids::fadeIn, ids::fadeOut, ids::fadeInShape, ids::fadeOutShape })
            clip.removeProperty (id, um());

        return Result::good (L"fades off on the clip at " + formatBars ((juce::int64) clip[ids::start]) + L" \x00b7 its edges keep the declick");
    }

    const auto length = lengthInSamples (args[2]);

    if ((which != "in" && which != "out") || length <= 0 || (args[3].isNotEmpty() && ! isShapeName (args[3])))
        return Result::fail (usage);

    const auto s = (juce::int64) clip[ids::start], len = (juce::int64) clip[ids::length];
    const auto n = juce::jmin (length, len);
    const auto shape = args[3].isNotEmpty() ? fadeShapeFromName (args[3]) : fadeShapeFromName (clip[which == "in" ? ids::fadeInShape : ids::fadeOutShape]);

    FadePlan plan;
    plan.ops.push_back ({ which == "in" ? FadePlan::in : FadePlan::out, clip, {}, true, which == "in" ? s : s + len - n, which == "in" ? s + n : s + len, focused });
    return applyFades (plan, shape);
}

// xfade 20ms [shape]: a crossfade, centred where two clips meet nearest the cursor on the
// focused track (where they already overlap, the middle of the overlap).
Result App::crossfadeCommand (const juce::StringArray& args)
{
    const auto length = lengthInSamples (args[0]);

    if (length <= 0 || (args[1].isNotEmpty() && ! isShapeName (args[1])))
        return Result::fail ("xfade 20ms (or 1/16, 0.1s) [eqp|lin|s|exp]: where two clips meet nearest the cursor");

    if (session == nullptr || onTopLane() || mixFocused() || focused >= session->numTracks())
        return Result::fail (L"focus an audio track (\x2191\x2193)");

    const auto holder = clipHolder (focused);
    const auto laneClips = Session::clips (holder);
    const auto at = engine.isRolling() ? heardPlayhead() : cursor;
    juce::int64 best = -1;

    for (const auto& l : laneClips)
        for (const auto& r : laneClips)
        {
            const auto sL = (juce::int64) l[ids::start], eL = sL + (juce::int64) l[ids::length];
            const auto sR = (juce::int64) r[ids::start], eR = sR + (juce::int64) r[ids::length];

            if (sL < sR && sR <= eL && eL < eR)
                if (const auto meet = (sR + eL) / 2; best < 0 || std::abs (meet - at) < std::abs (best - at))
                    best = meet;
        }

    if (best < 0)
        return Result::fail ("no two clips meet on track " + trackLabel (focused));

    FadePlan plan;
    const auto from = best - length / 2;

    if (auto r = planFades (holder, { from, from + length }, plan); ! r.ok)
        return r;

    plan.ops.erase (std::remove_if (plan.ops.begin(), plan.ops.end(), [] (const auto& op) { return op.kind != FadePlan::cross; }), plan.ops.end());

    if (plan.ops.empty())
        return Result::fail ("a crossfade that long runs past the clips at " + formatBars (best) + "; try a shorter one");

    for (auto& op : plan.ops)
        op.track = focused;

    // The crossfade is left selected, so ctrl+f changes its curve.
    timeAnchor = from;
    timeHead = from + length;
    return applyFades (plan, args[1].isNotEmpty() ? std::optional<FadeShape> (fadeShapeFromName (args[1])) : std::nullopt);
}

// declick 2 (ms) / declick off: the fade on every audio clip edge that has none.
Result App::setDeclick (const juce::String& arg)
{
    if (arg.isEmpty())
    {
        const double ms = session->tree().getProperty (ids::declickMs, 2.0);
        return Result::good (ms > 0.0 ? "declick " + juce::String (ms, 1) + L" ms on clip edges \x00b7 declick off turns it off"
                                      : juce::String (L"declick off \x00b7 declick 2 puts 2 ms fades on clip edges"));
    }

    const auto off = arg.equalsIgnoreCase ("off") || arg == "0";
    const auto ms = arg.trim().trimCharactersAtEnd ("msMS").getDoubleValue();

    if (! off && (ms <= 0.0 || ms > 50.0))
        return Result::fail ("declick 2 (ms, up to 50) or declick off");

    session->tree().setProperty (ids::declickMs, off ? 0.0 : ms, um());
    return Result::good (off ? juce::String ("declick off: clip edges without a fade start and stop dead")
                             : "declick " + juce::String (ms, 1) + " ms on clip edges without a fade");
}
} // namespace taw
