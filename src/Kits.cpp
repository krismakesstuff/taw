// Kits: a bus with kit set, its members the sample tracks playing into it, each on a MIDI note
// (pad 1 is note 36, what drum pads send) and maybe in a choke group. Each member keeps its own
// hits, so editing, mute, sends and inserts stay per sound. Armed, the controller's notes and
// keys mode's 4x4 pads play the members (on the audio thread: AudioEngine::process) and
// recording writes the hits onto each one, as the pad key does.

#include "App.h"

namespace taw
{
namespace
{
// "38", "d2", "f#2", "eb1": a note number, or a name with middle C as C4 (taw's names).
int parseNote (const juce::String& textIn)
{
    const auto text = textIn.trim().toLowerCase();

    if (text.isEmpty())
        return -1;

    if (text.containsOnly ("0123456789"))
        return text.getIntValue() <= 127 ? text.getIntValue() : -1;

    static const int semis[] = { 9, 11, 0, 2, 4, 5, 7 }; // a b c d e f g

    if (text[0] < 'a' || text[0] > 'g')
        return -1;

    int note = semis[text[0] - 'a'], i = 1;

    if (text[i] == '#')
        ++note, ++i;
    else if (text[i] == 'b' && text.length() > 2)
        --note, ++i;

    const auto octave = text.substring (i);

    if (octave.isEmpty() || ! octave.containsOnly ("-0123456789"))
        return -1;

    const auto n = note + (octave.getIntValue() + 1) * 12;
    return n >= 0 && n <= 127 ? n : -1;
}
} // namespace

int App::kitOf (const juce::ValueTree& track) const
{
    if (session == nullptr || ! Session::isSampleTrack (track))
        return -1;

    const int out = track[ids::out];

    if (out <= 0)
        return -1;

    const auto k = session->findBus (out);
    return k >= 0 && Session::isKit (session->track (k)) ? k : -1;
}

int App::kitHere() const
{
    if (session == nullptr || onTopLane() || focused < 0 || focused >= session->numTracks())
        return -1;

    return Session::isKit (session->track (focused)) ? focused : kitOf (focused);
}

std::vector<int> App::kitMembers (int kit) const
{
    std::vector<int> out;

    if (session == nullptr || kit < 0)
        return out;

    for (int t = 0; t < session->numTracks(); ++t)
        if (kitOf (t) == kit)
            out.push_back (t);

    std::stable_sort (out.begin(), out.end(), [this] (int a, int b)
                      { return (int) session->track (a).getProperty (ids::note, 999) < (int) session->track (b).getProperty (ids::note, 999); });
    return out;
}

int App::kitMemberOnNote (int kit, int note) const
{
    for (auto t : kitMembers (kit))
        if (session->track (t).hasProperty (ids::note) && (int) session->track (t)[ids::note] == note)
            return t;

    return -1;
}

bool App::trackHidden (int track) const
{
    const auto k = kitOf (track);
    return k >= 0 && (bool) session->track (k)[ids::folded];
}

juce::String App::padName (int note) const
{
    return note >= firstPadNote && note < firstPadNote + 16 ? "pad " + juce::String (note - firstPadNote + 1) : "note " + juce::String (note);
}

//==============================================================================
Result App::addKit (const juce::String& nameIn)
{
    int kits = 0;

    for (int t = 0; t < session->numTracks(); ++t)
        kits += Session::isKit (session->track (t)) ? 1 : 0;

    juce::String error;
    const auto id = makeBus (nameIn.trim().isEmpty() ? "Kit " + juce::String (kits + 1) : nameIn, error);

    if (id == 0)
        return Result::fail (error);

    const auto k = session->findBus (id);
    session->track (k).setProperty (ids::kit, true, um());
    focusTrack (k);
    browserPadNote = firstPadNote;
    return Result::good ("added kit " + busName (id) + L" \x00b7 ctrl+B: pick a pad, then every sound you audition goes on it \x00b7 "
                         + L"shift+R arms it for your pads (ctrl+k: the keyboard's 4x4)");
}

// 3 kit drums: tracks 3 into the kit (each on the next free pad unless it has a note);
// 3 kit off: out of it, back to the mix.
Result App::joinKit (const std::vector<int>& tracks, const juce::String& queryIn)
{
    const auto query = queryIn.trim().unquoted();

    if (tracks.empty())
        return Result::fail ("which tracks? 3 kit drums");

    if (query.equalsIgnoreCase ("off") || query.equalsIgnoreCase ("x") || query.equalsIgnoreCase ("mix"))
    {
        std::vector<int> left;

        for (auto t : tracks)
            if (kitOf (t) >= 0)
            {
                auto track = session->track (t);
                track.removeProperty (ids::out, um());
                track.removeProperty (ids::note, um());
                track.removeProperty (ids::choke, um());
                left.push_back (t);
            }

        return left.empty() ? Result::fail ("not in a kit") : Result::good (trackList (left) + " out of the kit, to the mix");
    }

    // Which kit: a number or a name, else the one here, else the only one.
    int kit = -1;

    if (query.containsOnly ("0123456789") && query.isNotEmpty())
        kit = query.getIntValue() - 1;
    else if (query.isNotEmpty())
    {
        for (int t = 0; t < session->numTracks() && kit < 0; ++t)
            if (Session::isKit (session->track (t)) && session->track (t)[ids::name].toString().startsWithIgnoreCase (query))
                kit = t;
    }
    else
    {
        for (int t = 0; t < session->numTracks(); ++t)
            if (Session::isKit (session->track (t)))
                kit = kit == -1 ? t : -2;
    }

    if (kit < 0 || kit >= session->numTracks() || ! Session::isKit (session->track (kit)))
        return Result::fail (query.isEmpty() ? L"which kit? 3 kit drums \x2014 kit makes one" : "no kit " + query + L" \x2014 kit " + query + " makes it");

    juce::StringArray placed;

    for (auto t : tracks)
    {
        auto track = session->track (t);

        if (! Session::isSampleTrack (track))
            return Result::fail ("track " + juce::String (t + 1) + L" isn't a sample track \x2014 kits are made of them (3 sample converts one)");

        track.setProperty (ids::out, session->track (kit)[ids::busId], um());

        if (! track.hasProperty (ids::note) || kitMemberOnNote (kit, (int) track[ids::note]) != t)
        {
            int note = firstPadNote;

            while (kitMemberOnNote (kit, note) >= 0 && note < 127)
                ++note;

            track.setProperty (ids::note, note, um());
        }

        placed.add (juce::String (t + 1) + " " + padName ((int) track[ids::note]));
    }

    return Result::good ("into kit " + busName ((int) session->track (kit)[ids::busId]) + ": " + placed.joinIntoString (", "));
}

Result App::setPadNote (const std::vector<int>& tracks, const juce::String& arg)
{
    const auto note = parseNote (arg);

    if (note < 0)
        return Result::fail (L"3 note 38 (or a name: 3 note d2, middle C is C4) \x2014 pad 1 is 36");

    if (tracks.size() != 1 || kitOf (tracks.front()) < 0)
        return Result::fail (L"note is for one kit member \x2014 3 kit drums puts track 3 in a kit");

    const auto t = tracks.front();
    const auto kit = kitOf (t);
    const auto other = kitMemberOnNote (kit, note);
    auto track = session->track (t);

    // The member already on that note swaps with this one, so two never share a pad.
    if (other >= 0 && other != t)
        session->track (other).setProperty (ids::note, track[ids::note], um());

    track.setProperty (ids::note, note, um());
    return Result::good ("track " + juce::String (t + 1) + " " + track[ids::name].toString() + " plays on " + padName (note) + " (note " + juce::String (note) + ")"
                         + (other >= 0 && other != t ? L" \x00b7 track " + juce::String (other + 1) + " took its old one" : juce::String()));
}

Result App::setChoke (const std::vector<int>& tracks, const juce::String& argIn)
{
    const auto arg = argIn.trim().toLowerCase();
    const bool off = arg == "off" || arg == "0" || arg == "x";
    const auto group = off ? 0 : arg.getIntValue();

    if (! off && (! arg.containsOnly ("0123456789") || group < 1 || group > 16))
        return Result::fail (L"3 choke 1 (a group, 1-16): a hit cuts off the others in its group, as a closed hat stops the open one \x00b7 3 choke off");

    for (auto t : tracks)
        if (kitOf (t) < 0)
            return Result::fail ("track " + juce::String (t + 1) + L" isn't in a kit \x2014 choke groups are a kit's (3 kit drums)");

    for (auto t : tracks)
    {
        if (off)
            session->track (t).removeProperty (ids::choke, um());
        else
            session->track (t).setProperty (ids::choke, group, um());
    }

    return Result::good (trackList (tracks) + (off ? juce::String (" in no choke group") : " in choke group " + juce::String (group)));
}

// A sample onto a pad: the member on that note gets it (keeping its hits), or a new member is
// made under the kit. The file is copied in by the import thread; pollImport finishes it.
Result App::loadPad (int kit, int note, const juce::String& path)
{
    if (session == nullptr || kit < 0 || kit >= session->numTracks() || ! Session::isKit (session->track (kit)))
        return Result::fail (L"no kit here \x2014 kit makes one");

    if (note < 0 || note > 127)
        return Result::fail ("pads are 1 to 16 (or note 0-127)");

    const auto member = kitMemberOnNote (kit, note);
    auto r = startImport ({ path }, member, ImportAs::sample);

    if (r.ok)
    {
        importKitBus = session->track (kit)[ids::busId];
        importNote = note;
    }

    return r;
}

// Called by pollImport for a pad load with no member on the note yet: a new sample track playing
// into the kit, under its last member.
juce::ValueTree App::newKitMember (const juce::String& name)
{
    const auto kit = session->findBus (importKitBus);

    if (kit < 0)
        return {};

    auto& tree = session->tree();
    const auto members = kitMembers (kit);
    int after = kit;

    for (auto m : members)
        after = std::max (after, m);

    auto track = session->addTrack (name, 0, um());
    track.setProperty (ids::out, importKitBus, um()).setProperty (ids::note, importNote, um());
    tree.moveChild (tree.indexOf (track), tree.indexOf (session->track (after)) + 1, um());
    return track;
}

Result App::toggleFold (int kit)
{
    if (kit < 0 || ! Session::isKit (session->track (kit)))
        return Result::fail (L"shift+F folds a kit \x2014 \x2191\x2193 to one");

    auto k = session->track (kit);
    const bool fold = ! (bool) k[ids::folded];
    k.setProperty (ids::folded, fold, nullptr); // a view, not an edit

    if (fold && kitOf (focused) == kit)
        focusTrack (kit);

    const auto n = (int) kitMembers (kit).size();
    return Result::good (k[ids::name].toString() + (fold ? " folded (" + juce::String (n) + " inside)" : juce::String (" open")) + L" \x00b7 shift+F again");
}

Result App::describeKit (int kit) const
{
    if (kit < 0)
        return Result::fail (L"no kit here \x2014 kit makes one");

    const auto k = session->track (kit);
    juce::StringArray pads;

    for (auto t : kitMembers (kit))
    {
        const auto m = session->track (t);
        pads.add (padName (m.getProperty (ids::note, -1)) + " " + m[ids::name].toString()
                  + ((int) m[ids::choke] > 0 ? " (choke " + m[ids::choke].toString() + ")" : juce::String()));
    }

    return Result::good ("kit " + juce::String (kit + 1) + " " + k[ids::name].toString() + ((bool) k[ids::armed] ? " (armed)" : "") + ": "
                         + (pads.isEmpty() ? juce::String (L"no pads yet \x00b7 pad 1 <file>, or ctrl+B and audition onto a pad") : pads.joinIntoString (juce::String (L" \x00b7 "))));
}

// Recording: a note on an armed kit's pad writes a hit onto the member playing it (the audio
// thread already played it).
void App::recordKitHit (int note, float velocity, double beat)
{
    for (int k = 0; k < session->numTracks(); ++k)
        if (Session::isKit (session->track (k)) && (bool) session->track (k)[ids::armed])
            if (const auto t = kitMemberOnNote (k, note); t >= 0)
            {
                beginEdit ("record hits", "pads");
                addHit (t, beat, velocity);
            }
}
} // namespace taw
