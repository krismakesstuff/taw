// A take from the phone into the session: imported at the cursor on a new track and, when the
// phone asked for drums or for notes, listened to (Hum.h) and built as a part beside it.

#include "App.h"

#include <chrono>

namespace taw
{
namespace
{
juce::String titled (const juce::String& kind) { return kind.substring (0, 1).toUpperCase() + kind.substring (1); }

// The library's sample of a kind nearest in brightness to the mouth's version of it, or none.
juce::String sampleFor (const Library* library, const juce::String& kind, double brightnessHz)
{
    juce::String best;
    double nearest = 1.0e9;

    if (library != nullptr)
        for (const auto& s : library->samples())
        {
            if (s.kind() != kind || s.seconds > 3.0 || s.centroidHz <= 0.0)
                continue;

            if (const double off = std::abs (std::log2 (s.centroidHz / std::max (20.0, brightnessHz))); off < nearest && juce::File (s.path).existsAsFile())
            {
                nearest = off;
                best = s.path;
            }
        }

    return best;
}

// How bright the take's hits of a kind are: a kick or tom by its body, a snare or hat by its attack.
double brightnessOf (const Heard& heard, const juce::String& kind)
{
    std::vector<double> values;

    for (const auto& hit : heard.hits)
        if (hit.kind == kind)
            values.push_back (kind == "kick" || kind == "tom" ? hit.centroidHz : hit.attackHz);

    if (values.empty())
        return 0.0;

    std::nth_element (values.begin(), values.begin() + (std::ptrdiff_t) values.size() / 2, values.end());
    return values[values.size() / 2];
}
} // namespace

juce::String App::phoneStatus() const
{
    if (phoneJob == nullptr)
        return {};

    const auto name = phoneJob->take.file.getFileName();

    switch (phoneJob->stage)
    {
        case PhoneJob::Stage::importing: return "bringing in " + name;
        case PhoneJob::Stage::listening: return "listening to " + name + " for " + phoneJob->take.as;
        case PhoneJob::Stage::drums:     return "building drums from " + name;
    }

    return {};
}

void App::pollPhone()
{
    if (! phone.running())
        return;

    if (session != nullptr)
        phone.setSession (session->name(), session->bpm(), session->beatsPerBar(), session->numTracks() == 0 && phoneJob == nullptr, phoneKeyName());
    else
        phone.setSession ({});

    for (auto& take : phone.takeReceived())
        phoneWaiting.push_back (std::move (take));

    if (phoneJob != nullptr)
        return stepPhoneJob();

    if (phoneWaiting.empty() || session == nullptr || import != nullptr || recording)
        return;

    auto job = std::make_unique<PhoneJob>();
    job->take = phoneWaiting.front();
    phoneWaiting.erase (phoneWaiting.begin());
    job->tracksBefore = session->numTracks();

    if (const auto r = startImport ({ job->take.file.getFullPathName() }, -1, ImportAs::audio); ! r.ok)
        return say (r);

    // Listened to while it's imported (off this thread: a long take takes a second or two).
    if (job->take.as != "audio")
        job->heard = std::async (std::launch::async, [file = job->take.file]
        {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            return hear (file, formats);
        });

    say (Result::good ("from the phone: " + job->take.file.getFileName() + (job->take.as != "audio" ? ", to be made into " + job->take.as : juce::String())));
    phoneJob = std::move (job);
}

void App::writePhoneHits (int index, const juce::String& kind)
{
    auto track = session->track (index);
    track.setProperty (ids::name, titled (kind), um());
    auto clip = Session::makeHitClip (phoneJob->startBeat, 0.0, phoneJob->part.beats);
    track.appendChild (clip, um());

    for (const auto& hit : phoneJob->part.hits)
        if (hit.kind == kind)
            clip.appendChild (Session::makeHit (hit.beat, hit.vel), um());
}

void App::stepPhoneJob()
{
    auto& job = *phoneJob;
    const auto name = job.take.file.getFileName();
    const bool drums = job.take.as == "drums";

    if (session == nullptr)
        return phoneJob.reset(); // the session closed under it

    if (import != nullptr || recording)
        return;

    // Recorded on the phone to its click: the tempo and where beat one is in the take are known.
    const bool first = job.tracksBefore == 0;
    const bool clicked = job.take.downbeat >= 0.0 && job.take.bpm > 0.0;

    // Every part starts on the same bar line, so one take sits over the last: the first bar
    // line the first take's first beat could reach from the cursor ("phone here" moves it).
    // The recording moves along to sit under its part, losing what won't fit before it.
    const auto place = [this] (juce::ValueTree take, double leadSeconds)
    {
        const double spb = session->samplesPerBeat(), bar = juce::jmax (1, session->beatsPerBar());
        const double lead = leadSeconds * sampleRate();

        if (phoneStartBeat < 0.0 || phoneStartIn != session->folder())
        {
            phoneStartBeat = std::ceil (((double) (juce::int64) take[ids::start] + lead) / spb / bar - 1.0e-6) * bar;
            phoneStartIn = session->folder();
        }

        const auto from = (juce::int64) std::llround (phoneStartBeat * spb - lead);
        const auto cut = juce::jmax ((juce::int64) 0, -from);
        take.setProperty (ids::start, from + cut, um());
        take.setProperty (ids::offset, (juce::int64) take[ids::offset] + cut, um());
        take.setProperty (ids::length, juce::jmax ((juce::int64) 1, (juce::int64) take[ids::length] - cut), um());
        return phoneStartBeat;
    };

    if (job.stage == PhoneJob::Stage::importing)
    {
        // Its import is over: with a new track (the last), or not at all (the import said why).
        if (session->numTracks() <= job.tracksBefore)
            return phoneJob.reset();

        if (job.take.as == "audio")
        {
            // A plain recording made to the click: its beat one on the bar line, like a part's.
            if (clicked)
            {
                if (first)
                    session->tree().setProperty (ids::bpm, std::round (job.take.bpm), um());

                place (session->track (session->numTracks() - 1).getChildWithName (ids::CLIP), job.take.downbeat);
            }

            return phoneJob.reset();
        }

        // Muted: it's there to check the part against. (Still in the import's undo step.)
        session->track (session->numTracks() - 1).setProperty (ids::mute, true, um());
        job.stage = PhoneJob::Stage::listening;
    }

    if (job.stage == PhoneJob::Stage::listening)
    {
        if (job.heard.wait_for (std::chrono::seconds (0)) != std::future_status::ready)
            return;

        const auto heard = job.heard.get();

        if (! heard.has_value() || (drums ? heard->hits.empty() : heard->notes.empty()))
        {
            say (Result::fail ("from the phone: nothing to make " + job.take.as + " of was heard in " + name + L" \x00b7 the recording is in, muted"));
            return phoneJob.reset();
        }

        // One undo step from here (and no plugin window: nobody at the keys asked for one).
        const juce::ScopedValueSetter<bool> quiet (agentEditing, true);
        um()->beginNewTransaction ("phone: " + job.take.as + " from " + name);

        // The tempo: the click's if it was made to one. Else, in a session with nothing in it
        // yet the take sets it, and otherwise it was hummed to the session's.
        if (first && clicked)
            session->tree().setProperty (ids::bpm, std::round (job.take.bpm), um());

        job.part = clicked ? arrange (*heard, drums, session->bpm(), job.take.downbeat, job.take.onGrid)
                           : arrange (*heard, drums, first ? 0.0 : session->bpm(), -1.0, job.take.onGrid);
        job.heardTake = *heard;

        // Notes kept to a key, so one sung part agrees with the next: the one the phone named,
        // else the session's so far, else the one this take fits best (the session's from now).
        juce::String inKey;

        if (! drums && job.take.key != "off")
        {
            if (const auto named = HumKey::named (job.take.key))
                phoneKey = named;
            else if (phoneKeyName().isEmpty())
                phoneKey = keyOf (job.part);

            phoneKeyIn = session->folder();
            snapToKey (job.part, *phoneKey);
            inKey = " in " + phoneKey->name();
        }

        if (first && ! clicked)
            session->tree().setProperty (ids::bpm, std::round (job.part.bpm), um());

        job.startBeat = place (session->track (session->numTracks() - 1).getChildWithName (ids::CLIP), job.part.firstOnset);
        const auto at = formatBars ((juce::int64) std::llround (job.startBeat * session->samplesPerBeat()));

        if (drums)
        {
            for (const auto& hit : job.part.hits)
                job.kinds.addIfNotAlreadyThere (hit.kind);

            job.stage = PhoneJob::Stage::drums;
            return; // a sample track for each, from the next frame on
        }

        // Notes: on the instrument asked for, if taw has it; else on a track waiting for one.
        int index = -1;

        if (job.take.instrument.isNotEmpty() && plugins.hasScanned() && plugins.findInstrument (job.take.instrument).has_value()
            && loadInstrument (std::vector<int>(), job.take.instrument).ok)
        {
            index = session->numTracks() - 1;
            session->track (index).setProperty (ids::armed, false, um()); // the controller keeps playing what it played
        }
        else
        {
            session->addTrack ("Notes", 0, um()).setProperty (ids::kind, "instrument", um());
            index = session->numTracks() - 1;
        }

        auto clip = Session::makeMidiClip (job.startBeat, 0.0, job.part.beats);
        session->track (index).appendChild (clip, um());

        for (const auto& note : job.part.notes)
            clip.appendChild (Session::makeNote (note.beat, note.length, note.pitch, note.vel), um());

        const auto plugin = session->track (index)[ids::pluginName].toString();
        say (Result::good ("from the phone: " + juce::String ((int) job.part.notes.size()) + " notes" + inKey + " on track " + trackLabel (index)
                           + (plugin.isNotEmpty() ? " (" + plugin + ")" : juce::String()) + " at " + at
                           + (plugin.isEmpty() ? L" \x00b7 " + trackLabel (index) + " inst <name> gives it a sound" : juce::String())
                           + L" \x00b7 the recording is muted beside them"));
        return phoneJob.reset();
    }

    // Drums: a sample track for each drum heard, one at a time (a sample is imported, like any).
    if (job.sampleComing)
    {
        job.sampleComing = false;

        if (session->numTracks() > job.tracksBefore + 1 + job.kind) // its track is in (the import's undo step takes the hits too)
            writePhoneHits (session->numTracks() - 1, job.kinds[job.kind]);

        ++job.kind;
    }

    while (job.kind < job.kinds.size())
    {
        const auto kind = job.kinds[job.kind];
        loadLibrary();

        if (const auto sample = sampleFor (library.get(), kind, brightnessOf (job.heardTake, kind));
            sample.isNotEmpty() && startImport ({ sample }, -1, ImportAs::sample).ok)
        {
            job.sampleComing = true;
            return;
        }

        // The library has none of this kind: an empty sample track, for one to be given (3 sample <file>).
        const juce::ScopedValueSetter<bool> quiet (agentEditing, true);
        um()->beginNewTransaction ("phone: " + kind + " from " + name);
        session->addTrack (titled (kind), 0, um()).setProperty (ids::kind, "sample", um());
        writePhoneHits (session->numTracks() - 1, kind);
        ++job.kind;
    }

    say (Result::good ("from the phone: " + juce::String ((int) job.part.hits.size()) + " hits (" + job.kinds.joinIntoString (", ") + ") at "
                       + formatBars ((juce::int64) std::llround (job.startBeat * session->samplesPerBeat()))
                       + (job.tracksBefore == 0 ? ", tempo " + juce::String (session->bpm(), 0) : juce::String())
                       + L" \x00b7 the recording is muted beside them"));
    phoneJob.reset();
}
} // namespace taw
