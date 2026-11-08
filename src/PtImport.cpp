// Reading a Pro Tools session through ptformat (third_party/ptformat), which unscrambles the .ptx
// and finds its audio files, regions, and which playlist plays which region where (in samples),
// plus MIDI regions and their notes (in ticks, 960000 to the quarter note).
//
// ptformat calls each playlist a track. Pro Tools names a track's playlists after it: "Gtr 1"
// has "Gtr 1.01", "Gtr 1.02"..., and the one playing carries the track's name. A duplicated
// track's are "Gtr 1.dup1", "Gtr 1.01.dup1.03"... So a playlist belongs to the track whose name
// matches it once the ".NN" numbers are dropped, and the one named exactly like the track is its
// main playlist; the others become taw playlists under it.

#include "PtImport.h"

#include "Ids.h"
#include "Session.h"

#include "ptformat/ptformat.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <cstring>
#include <map>

namespace taw
{
namespace
{
constexpr double ticksPerBeat = 960000.0;

// "Gtr 1.01.dup1.03" -> "Gtr 1.dup1": the name without its playlist numbers.
juce::String playlistFamily (const juce::String& name)
{
    juce::StringArray kept;

    for (const auto& part : juce::StringArray::fromTokens (name, ".", {}))
        if (! (part.isNotEmpty() && part.containsOnly ("0123456789")))
            kept.add (part);

    return kept.joinIntoString (".");
}

// The tempo: the first entry of the "( Tempo" block, a double after "Const". 0 if not found.
double readTempo (const unsigned char* data, size_t size)
{
    const auto find = [&] (const char* needle, size_t from) -> size_t
    {
        const auto n = std::strlen (needle);

        for (size_t i = from; i + n <= size; ++i)
            if (std::memcmp (data + i, needle, n) == 0)
                return i;

        return size;
    };

    const auto block = find ("( Tempo", 0);
    const auto konst = find ("Const", block);

    if (block >= size || konst >= size || konst - block > 64)
        return 0.0;

    for (size_t i = konst + 5; i + 8 <= size && i < konst + 96; ++i)
    {
        double v;
        std::memcpy (&v, data + i, 8); // little-endian on every platform taw builds for

        if (std::isfinite (v) && v >= 20.0 && v <= 999.0 && std::abs (v * 1000.0 - std::round (v * 1000.0)) < 1.0e-6)
            return v;
    }

    return 0.0;
}

double beatOf (int64_t ticks) { return std::round ((double) ticks / ticksPerBeat * 1.0e6) / 1.0e6; }
} // namespace

ProToolsSession readProToolsSession (const juce::File& ptx)
{
    ProToolsSession out;
    PTFFormat pt;
    const auto path = ptx.getFullPathName().toStdString();

    if (! ptx.existsAsFile())
    {
        out.error = "no such file: " + ptx.getFullPathName();
        return out;
    }

    // Positions come back scaled to the rate asked for; ask for the session's own.
    int rc = pt.load (path, 44100);

    if (rc == 0 && pt.sessionrate() > 0 && pt.sessionrate() != 44100)
        rc = pt.load (path, pt.sessionrate());

    if (rc != 0)
    {
        out.error = rc == -3 ? ptx.getFileName() + " is from a Pro Tools version taw can't read (10 to 2023 sessions are .ptx)"
                             : ptx.getFileName() + " isn't a Pro Tools session taw can read";
        return out;
    }

    out.sampleRate = (double) pt.sessionrate();
    const auto bpm = readTempo (pt.unxored_data(), (size_t) pt.unxored_size());

    out.session = juce::ValueTree (ids::SESSION);
    auto& s = out.session;
    s.setProperty (ids::bpm, bpm > 0.0 ? bpm : 120.0, nullptr)
        .setProperty (ids::timeSigNum, 4, nullptr)
        .setProperty (ids::timeSigDen, 4, nullptr);

    if (bpm <= 0.0)
        out.report.add ("couldn't find the tempo; the session is at 120");

    out.report.add ("the meter came across as 4/4, and only the first tempo (tempo and meter changes aren't read yet)");

    // The session's tracks, in order (ptformat sees the list twice).
    juce::StringArray trackNames;

    for (const auto& n : pt.alltracks())
        trackNames.addIfNotAlreadyThere (juce::String::fromUTF8 (n.c_str()));

    // Audio: ptformat's "tracks" are playlists, one entry per region placed on one.
    const auto audioFolder = ptx.getParentDirectory().getChildFile ("Audio Files");
    std::map<juce::String, juce::String> copied; // file name in Pro Tools -> its path in the session
    std::map<juce::String, std::vector<juce::ValueTree>> clipsOfPlaylist;
    juce::StringArray playlistOrder, missing, stereoHalves;

    for (const auto& t : pt.tracks())
    {
        const auto playlist = juce::String::fromUTF8 (t.name.c_str());
        const auto file = juce::String::fromUTF8 (t.reg.wave.filename.c_str());
        playlistOrder.addIfNotAlreadyThere (playlist);

        if (file.endsWithIgnoreCase (".L.wav") || file.endsWithIgnoreCase (".R.wav"))
            stereoHalves.addIfNotAlreadyThere (playlist);

        const auto source = audioFolder.getChildFile (file);

        if (! source.existsAsFile())
        {
            missing.addIfNotAlreadyThere (file);
            continue;
        }

        auto& to = copied[file];

        if (to.isEmpty())
        {
            to = "audio/" + juce::File::createLegalFileName (file);
            out.copies.push_back ({ source, to });
        }

        clipsOfPlaylist[playlist].push_back (Session::makeClip (to, t.reg.startpos, t.reg.sampleoffset, t.reg.length));
    }

    // MIDI, by the MIDI playlist each region sits on.
    std::map<juce::String, std::vector<juce::ValueTree>> midiOf;
    juce::StringArray midiOrder;

    for (const auto& t : pt.miditracks())
    {
        const auto name = juce::String::fromUTF8 (t.name.c_str());
        midiOrder.addIfNotAlreadyThere (name);
        auto clip = Session::makeMidiClip (beatOf (t.reg.startpos), 0.0, beatOf (t.reg.length));

        for (const auto& n : t.reg.midi)
            clip.appendChild (Session::makeNote (beatOf ((int64_t) n.pos), beatOf ((int64_t) n.length), n.note,
                                                 juce::jlimit (1, 127, (int) n.velocity)),
                              nullptr);

        midiOf[name].push_back (clip);
    }

    // Each playlist's track: the session track of the same family, else a track of its own.
    std::map<juce::String, juce::String> trackOfPlaylist;

    for (const auto& p : playlistOrder)
    {
        juce::String owner = p;

        for (const auto& t : trackNames)
            if (playlistFamily (t) == playlistFamily (p))
            {
                owner = t;
                break;
            }

        trackOfPlaylist[p] = owner;
    }

    juce::StringArray order; // tracks with something on them, in the session's order, then the rest

    for (const auto& t : trackNames)
        if (midiOrder.contains (t) || std::any_of (trackOfPlaylist.begin(), trackOfPlaylist.end(), [&] (const auto& kv) { return kv.second == t; }))
            order.add (t);

    for (const auto& kv : trackOfPlaylist)
        order.addIfNotAlreadyThere (kv.second);

    for (const auto& m : midiOrder)
        order.addIfNotAlreadyThere (m);

    int playlistsKept = 0;

    for (const auto& name : order)
    {
        juce::ValueTree track (ids::TRACK);
        track.setProperty (ids::name, name, nullptr);

        if (midiOf.count (name) > 0)
        {
            track.setProperty (ids::kind, "instrument", nullptr);

            for (auto& c : midiOf[name])
                track.appendChild (c, nullptr);

            out.report.add (name + L": its instrument didn't come across \x2014 inst <name> picks one");
        }
        else
        {
            track.setProperty (ids::kind, "audio", nullptr);

            // Its own playlist is the main one; the others, in Pro Tools' order, under it.
            for (const auto& p : playlistOrder)
            {
                if (trackOfPlaylist[p] != name)
                    continue;

                if (p == name)
                {
                    for (auto& c : clipsOfPlaylist[p])
                        track.appendChild (c, nullptr);
                }
                else
                {
                    juce::ValueTree alt (ids::PLAYLIST);
                    alt.setProperty (ids::name, p, nullptr);

                    for (auto& c : clipsOfPlaylist[p])
                        alt.appendChild (c, nullptr);

                    track.appendChild (alt, nullptr);
                    ++playlistsKept;
                }
            }
        }

        s.appendChild (track, nullptr);
    }

    if (playlistsKept > 0)
        out.report.add (juce::String (playlistsKept) + " other playlist" + (playlistsKept == 1 ? "" : "s")
                        + L" came across under their tracks (shift+P shows them)");

    for (const auto& p : stereoHalves)
        out.report.add (p + L": a stereo track (.L/.R files) \x2014 its halves came in as mono clips on one track, which isn't right yet");

    for (const auto& f : missing)
        out.report.add ("missing from Audio Files: " + f);

    juce::StringArray empty;

    for (const auto& t : trackNames)
        if (! order.contains (t))
            empty.add (t);

    if (! empty.isEmpty())
        out.report.add ("left out (nothing on them, or aux, master and VCA tracks, which aren't read yet): " + empty.joinIntoString (", "));

    out.report.add ("plugins, the mixer (volume, pan, sends, routing), automation, clip gain, fades and markers aren't read yet");
    return out;
}

bool writeProToolsSession (const ProToolsSession& pts, const juce::File& dir, juce::String& error)
{
    auto session = Session::create (dir, pts.session, pts.sampleRate, error);

    if (session == nullptr)
        return false;

    for (const auto& c : pts.copies)
    {
        const auto file = dir.getChildFile (c.to);
        file.getParentDirectory().createDirectory();

        if (! c.from.copyFileTo (file))
        {
            error = "couldn't copy " + c.from.getFullPathName();
            return false;
        }
    }

    juce::StringArray lines;
    lines.add ("Imported from Pro Tools: what didn't come across");
    lines.add ({});
    lines.addArray (pts.report);
    dir.getChildFile ("import-report.txt").replaceWithText (lines.joinIntoString ("\n") + "\n");
    return true;
}
} // namespace taw
