// Instrument tracks: a hosted plugin (VST3) per track, played by the MIDI controller while the
// track is armed, and the notes recorded on it.

#include "App.h"

namespace taw
{
namespace
{
// What a CC node stands for, as a MIDI message.
MidiMsg controlMessage (int num, int val, int channel)
{
    if (num == 128) // pitch bend, 0-16383
        return { (juce::uint8) (0xe0 | channel), (juce::uint8) (val & 0x7f), (juce::uint8) ((val >> 7) & 0x7f) };

    if (num == 129) // channel pressure
        return { (juce::uint8) (0xd0 | channel), (juce::uint8) juce::jlimit (0, 127, val), 0 };

    return MidiMsg::controller (channel, juce::jlimit (0, 127, num), juce::jlimit (0, 127, val));
}

int channelOf (const juce::ValueTree& node) { return juce::jlimit (1, 16, (int) node.getProperty (ids::ch, 1)) - 1; }
} // namespace

// Builds the track's notes and controllers in samples (the graph's copy of them).
Sequence buildSequence (const juce::ValueTree& track, double samplesPerBeat)
{
    Sequence seq;

    for (const auto& n : Session::notes (track))
    {
        Sequence::Note note;
        const double beat = n[ids::beat], len = n[ids::len];
        note.start = std::llround (beat * samplesPerBeat);
        note.end = juce::jmax (note.start + 1, (juce::int64) std::llround ((beat + len) * samplesPerBeat));
        note.channel = (juce::uint8) channelOf (n);
        note.pitch = (juce::uint8) juce::jlimit (0, 127, (int) n[ids::pitch]);
        note.vel = (juce::uint8) juce::jlimit (1, 127, (int) n[ids::vel]);
        seq.notes.push_back (note);
    }

    for (const auto& c : Session::controls (track))
        seq.controls.push_back ({ std::llround ((double) c[ids::beat] * samplesPerBeat), controlMessage (c[ids::num], c[ids::val], channelOf (c)) });

    std::stable_sort (seq.notes.begin(), seq.notes.end(), [] (const auto& a, const auto& b) { return a.start < b.start; });
    std::stable_sort (seq.controls.begin(), seq.controls.end(), [] (const auto& a, const auto& b) { return a.pos < b.pos; });
    return seq;
}

//==============================================================================
std::shared_ptr<PluginSlot> App::createInstrument (const juce::ValueTree& track, double rate, int block, juce::String& problem)
{
    const auto name = track[ids::pluginName].toString();
    auto desc = plugins.byIdentifier (track[ids::plugin].toString());

    if (! desc && name.isNotEmpty())
        desc = plugins.findInstrument (name);

    if (! desc)
    {
        problem = (name.isNotEmpty() ? name : juce::String ("its plugin"))
                  + (plugins.hasScanned() ? juce::String (" isn't installed") : juce::String (L" not found \x2014 type scan"));
        return {};
    }

    auto slot = plugins.create (*desc, rate, block, problem);

    if (slot == nullptr)
        return {};

    const auto stateFile = track[ids::pluginState].toString();

    if (stateFile.isNotEmpty())
    {
        juce::MemoryBlock state;

        if (session->folder().getChildFile (stateFile).loadFileAsData (state))
            slot->setState (state);
    }

    return slot;
}

App::Instrument* App::instrumentEntry (const juce::ValueTree& track)
{
    for (auto& in : instruments)
        if (in.track == track)
            return &in;

    if (! Session::isInstrumentTrack (track))
        return nullptr;

    // First time this track is played: load its plugin (and its saved state).
    Instrument in;
    in.track = track;
    const auto rate = engine.deviceSampleRate() > 0.0 ? engine.deviceSampleRate() : sampleRate();
    in.slot = createInstrument (track, rate, engine.maxBlockSize(), in.problem);

    if (in.slot != nullptr)
        in.savedState = in.slot->getState();

    instruments.push_back (std::move (in));
    return &instruments.back();
}

PluginSlot* App::instrumentFor (int track)
{
    if (session == nullptr || track < 0 || track >= session->numTracks())
        return nullptr;

    auto* in = instrumentEntry (session->track (track));
    return in != nullptr ? in->slot.get() : nullptr;
}

juce::String App::instrumentProblem (int track) const
{
    if (session == nullptr || track < 0 || track >= session->numTracks())
        return {};

    for (const auto& in : instruments)
        if (in.track == session->track (track))
            return in.problem;

    return {};
}

// Plugin state lives in its own file (plugins/…), not in session.json: it's opaque and can be
// megabytes. Written only when it changed.
void App::writeInstrumentState (Instrument& in)
{
    const auto sessionFolder = session->folder();
    const auto path = in.track[ids::pluginState].toString();

    if (in.slot == nullptr || path.isEmpty())
        return;

    auto state = in.slot->getState();

    if (state == in.savedState)
        return;

    const auto file = sessionFolder.getChildFile (path);
    file.getParentDirectory().createDirectory();

    if (file.replaceWithData (state.getData(), state.getSize()))
        in.savedState = std::move (state);
}

void App::saveInstrumentStates()
{
    if (session != nullptr)
        for (auto& in : instruments)
            writeInstrumentState (in);
}

void App::pollInstruments()
{
    if (session == nullptr)
        return;

    for (auto& in : instruments)
        if (in.slot != nullptr && in.slot->editorWantsToClose())
        {
            in.slot->closeEditor();
            writeInstrumentState (in);
        }

    // A new device (or new settings): every plugin gets prepared for its rate and block size.
    if (engine.isDeviceRunning() && engine.deviceGeneration() != preparedGeneration)
    {
        preparedGeneration = engine.deviceGeneration();

        for (auto& in : instruments)
            if (in.slot != nullptr)
                in.slot->prepare (engine.deviceSampleRate(), engine.maxBlockSize());
    }

    // Tweaks made in a plugin's window don't touch the session, so save states now and then.
    const auto now = juce::Time::getMillisecondCounterHiRes();

    if (now - lastStateSave > 15000.0)
    {
        lastStateSave = now;
        saveInstrumentStates();
    }

    if (plugins.isScanning())
    {
        if (plugins.scanStep())
        {
            message = L"scanning plugins " + juce::String (juce::roundToInt (plugins.scanProgress() * 100.0f)) + L"% \x00b7 "
                      + plugins.scanningName();
            messageIsError = false;
        }
        else
        {
            // Tracks whose plugin wasn't found get another go.
            instruments.erase (std::remove_if (instruments.begin(), instruments.end(), [] (const auto& in) { return in.slot == nullptr; }),
                               instruments.end());
            rebuildGraph();

            const auto n = (int) plugins.instruments().size();
            auto msg = "found " + juce::String (n) + (n == 1 ? " instrument" : " instruments");

            // Plugins that crashed or hung while being looked at are skipped from now on.
            if (const auto& failed = plugins.failedLastScan(); ! failed.isEmpty())
                msg << L" \x00b7 skipped " << failed.size() << " that crashed or hung (" << failed.joinIntoString (", ")
                    << L") \x00b7 scan retry tries them again";
            else if (! pluginPickerVisible)
                msg << L" \x00b7 inst <name> loads one, inst alone lists them";

            say (Result::good (msg));
        }
    }
}

//==============================================================================
Result App::scanPlugins (bool retryFailed)
{
    if (plugins.isScanning())
        return Result::fail ("already scanning");

    plugins.startScan (retryFailed);
    return Result::good (L"scanning plugins\x2026");
}

Result App::toggleMidiPanel()
{
    if (midiInputs == nullptr)
        return Result::fail ("MIDI is only open in the interactive app");

    midiPanelVisible = ! midiPanelVisible;

    if (! midiPanelVisible)
        return Result::good();

    midiInputs->refresh(); // tries busy ones again
    midiPanelIndex = 0;
    helpVisible = settingsVisible = pluginPickerVisible = false;
    return Result::good (midiInputs->devices().empty() ? juce::String (L"no MIDI inputs \x2014 plug a controller in (taw picks it up)")
                                                       : juce::String (L"play your controller: its row lights up"));
}

bool App::handleMidiPanelKey (const Key& k)
{
    if (! midiPanelVisible || midiInputs == nullptr || ! commandLine.isEmpty())
        return false;

    const auto devices = midiInputs->devices();
    const auto n = (int) devices.size();
    midiPanelIndex = juce::jlimit (0, juce::jmax (0, n - 1), midiPanelIndex);

    switch (k.code)
    {
        case Key::Up:     midiPanelIndex = juce::jmax (0, midiPanelIndex - 1); return true;
        case Key::Down:   midiPanelIndex = juce::jmin (n - 1, midiPanelIndex + 1); return true;
        case Key::Escape: midiPanelVisible = false; return true;
        case Key::Left:
        case Key::Right:
        case Key::Enter:
            if (n > 0)
            {
                // On / off, remembered for every session.
                const auto& d = devices[(size_t) midiPanelIndex];
                const bool on = k.code == Key::Left ? false : k.code == Key::Right ? true : ! d.on;
                midiInputs->setOn (d.name, on);
                midiOff = midiInputs->switchedOff();
                saveSettings();

                const auto now = midiInputs->devices();
                const bool opened = std::any_of (now.begin(), now.end(), [&] (const auto& x) { return x.name == d.name && x.open; });
                say (! on ? Result::good (d.name + " off")
                          : opened ? Result::good (d.name + " on")
                                   : Result::fail (d.name + L" is on but won't open \x2014 another app has it (close it, then midi again)"));
            }
            return true;
        case Key::Char:
            if (k.ch == ' ' && ! k.ctrl && ! k.alt)
                return handleMidiPanelKey ({ Key::Enter });

            return false; // typing starts a command as usual
        default:
            return false;
    }
}

void App::openPluginPicker (int targetTrack)
{
    // The first time, the list fills in while the picker is open.
    if (! plugins.hasScanned() && ! plugins.isScanning())
        plugins.startScan();

    pluginPickerVisible = true;
    pluginFilter.clear();
    pluginPickIndex = 0;
    pluginPickTarget = targetTrack;
    helpVisible = settingsVisible = false;
}

std::vector<juce::PluginDescription> App::pickerChoices() const
{
    std::vector<juce::PluginDescription> out;

    for (const auto& d : plugins.instruments())
        if (pluginFilter.isEmpty() || (d.manufacturerName + " " + d.name).containsIgnoreCase (pluginFilter))
            out.push_back (d);

    return out;
}

bool App::handlePluginPickerKey (const Key& k)
{
    if (! pluginPickerVisible)
        return false;

    const auto choices = pickerChoices();
    const auto n = (int) choices.size();

    switch (k.code)
    {
        case Key::Up:     pluginPickIndex = juce::jmax (0, pluginPickIndex - 1); return true;
        case Key::Down:   pluginPickIndex = juce::jmax (0, juce::jmin (n - 1, pluginPickIndex + 1)); return true;
        case Key::Escape: pluginPickerVisible = false; return true;
        case Key::Backspace:
            pluginFilter = pluginFilter.dropLastCharacters (1);
            pluginPickIndex = 0;
            return true;
        case Key::Enter:
            if (pluginPickIndex < n)
            {
                pluginPickerVisible = false;
                say (loadInstrument (pluginPickTarget, choices[(size_t) pluginPickIndex]));
            }
            return true;
        case Key::Char:
            if (k.ctrl || k.alt)
                return false; // ctrl+q and friends still work

            if (k.ch >= 32)
            {
                pluginFilter += juce::String::charToString ((juce::juce_wchar) k.ch);
                pluginPickIndex = 0;
            }
            return true;
        default:
            return true;
    }
}

Result App::loadInstrument (const std::vector<int>& tracks, const juce::String& query)
{
    if (query.trim().isEmpty())
    {
        openPluginPicker (tracks.empty() ? -1 : tracks.front());
        return Result::good (plugins.hasScanned() ? juce::String() : juce::String (L"scanning plugins (first time)\x2026"));
    }

    if (! plugins.hasScanned())
        return Result::fail (L"no plugins scanned yet \x2014 type scan (takes a moment), then inst " + query);

    const auto desc = plugins.findInstrument (query);

    if (! desc)
        return Result::fail ("no instrument matches " + query.quoted() + L" \x00b7 inst alone lists them \x00b7 scan finds new ones");

    if (tracks.empty())
        return loadInstrument (-1, *desc);

    Result last;

    for (auto t : tracks)
        if (last = loadInstrument (t, *desc); ! last.ok)
            return last;

    return last;
}

Result App::loadInstrument (int index, const juce::PluginDescription& desc)
{
    if (session == nullptr)
        return Result::fail ("no session");

    if (recording)
        return Result::fail ("stop recording first");

    juce::ValueTree track;

    if (index >= 0)
    {
        track = session->track (index);

        if (Session::isSampleTrack (track) || (Session::isAudioTrack (track) && ! Session::clips (track).empty()))
            return Result::fail ("track " + juce::String (index + 1) + L" has audio on it \x2014 inst alone makes a new instrument track");
    }

    const auto rate = engine.deviceSampleRate() > 0.0 ? engine.deviceSampleRate() : sampleRate();
    juce::String error;
    auto slot = plugins.create (desc, rate, engine.maxBlockSize(), error);

    if (slot == nullptr)
        return Result::fail (error);

    beginEdit ("inst " + desc.name);

    if (index < 0)
    {
        // A new track, armed so the controller plays it straight away.
        track = session->addTrack (desc.name, 0, um());
        index = session->numTracks() - 1;
        track.setProperty (ids::armed, true, um());
    }
    else
    {
        // Swapping plugins: the old one goes (its window too); the track keeps its notes.
        for (auto it = instruments.begin(); it != instruments.end(); ++it)
            if (it->track == track)
            {
                if (it->slot != nullptr)
                    it->slot->closeEditor();

                instruments.erase (it);
                break;
            }
    }

    const auto base = juce::String (index + 1).paddedLeft ('0', 2) + "-" + juce::File::createLegalFileName (desc.name).replaceCharacter (' ', '_');
    const auto stateFile = session->folder().getChildFile ("plugins").getNonexistentChildFile (base, ".state", false);

    track.setProperty (ids::kind, "instrument", um())
        .setProperty (ids::input, 0, um())
        .setProperty (ids::plugin, desc.createIdentifierString(), um())
        .setProperty (ids::pluginName, desc.name, um())
        .setProperty (ids::pluginState, stateFile.getRelativePathFrom (session->folder()).replaceCharacter ('\\', '/'), um());

    instruments.push_back ({ track, std::move (slot), {}, {} });
    focusTrack (index);
    rebuildGraph();

    auto msg = "track " + juce::String (index + 1) + " plays " + desc.name;
    const bool armed = track[ids::armed];

    if (midiInputs != nullptr && midiInputs->names().isEmpty())
        return Result::good (msg + L" \x00b7 no MIDI input found yet (plug one in) \x00b7 ed opens its window");

    return Result::good (msg + (armed ? L" \x00b7 armed: play your controller" : L" \x00b7 shift+R arms it for your controller")
                         + L" \x00b7 ed opens its window \x00b7 ctrl+space records");
}

Result App::toggleEditor (int index)
{
    if (session == nullptr || index < 0 || index >= session->numTracks() || ! Session::isInstrumentTrack (session->track (index)))
        return Result::fail (L"ed opens an instrument track's plugin \x2014 focus one (\x2191\x2193), or inst makes one");

    auto* slot = instrumentFor (index);

    if (slot == nullptr)
        return Result::fail ("track " + juce::String (index + 1) + ": " + instrumentProblem (index));

    if (slot->editorShowing())
    {
        if (auto* in = instrumentEntry (session->track (index)))
        {
            in->slot->closeEditor();
            writeInstrumentState (*in);
        }

        return Result::good ("closed " + slot->name());
    }

    const auto t = session->track (index);
    slot->showEditor (juce::String (index + 1) + " " + t[ids::name].toString() + L" \x00b7 " + slot->name() + L" \x00b7 taw");
    return Result::good (slot->name() + L" window open \x00b7 ed again (or its close button) closes it");
}

//==============================================================================
// Recording what the controller plays

juce::int64 App::heardPosition (juce::int64 playhead)
{
    // The playhead is what's being sent out now; you heard it an output latency later, and
    // that's what you played along to.
    if (session != nullptr && (bool) session->tree()[ids::latencyComp])
        if (auto* d = engine.devices().getCurrentAudioDevice())
            return playhead - d->getOutputLatencyInSamples();

    return playhead;
}

void App::drainRecordedMidi()
{
    for (TimedMidi m; engine.popRecordedMidi (m);)
    {
        if (session == nullptr || midiRecordTracks.empty())
            continue;

        const auto spb = session->samplesPerBeat();
        const auto played = juce::jmax (0.0, (double) heardPosition (m.pos) / spb);
        const auto& msg = m.msg;
        const int key = msg.channel() * 128 + msg.d1;

        auto finish = [&] (const HeldNote& h, double endBeat)
        {
            beginEdit ("record notes", "midi");

            for (auto t : midiRecordTracks)
                session->track (t).appendChild (Session::makeNote (h.beat, endBeat - h.played, h.pitch, h.vel, h.channel + 1), um());
        };

        if (msg.isNoteOn())
        {
            if (auto it = heldNotes.find (key); it != heldNotes.end())
                finish (it->second, played); // played again without letting go

            auto beat = played;

            // Input quantize moves where the note starts; it keeps the length you played.
            if ((bool) session->tree()[ids::inputQuantize])
            {
                const auto step = session->fineStepBeats();
                beat = std::round (beat / step) * step;
            }

            heldNotes[key] = { msg.channel(), msg.d1, msg.d2, beat, played };
        }
        else if (msg.isNoteOff())
        {
            if (auto it = heldNotes.find (key); it != heldNotes.end())
            {
                finish (it->second, played);
                heldNotes.erase (it);
            }
        }
        else
        {
            int num = -1, val = 0;

            if (msg.type() == 0xb0)      num = msg.d1, val = msg.d2;
            else if (msg.type() == 0xe0) num = 128, val = msg.d1 | (msg.d2 << 7);
            else if (msg.type() == 0xd0) num = 129, val = msg.d1;

            if (num < 0)
                continue;

            beginEdit ("record notes", "midi");

            for (auto t : midiRecordTracks)
                session->track (t).appendChild (Session::makeControl (played, num, val, msg.channel() + 1), um());
        }
    }
}

// Keys still down when recording stops end there.
void App::finishHeldNotes (juce::int64 at)
{
    drainRecordedMidi();

    if (session != nullptr && ! heldNotes.empty())
    {
        const auto endBeat = juce::jmax (0.0, (double) heardPosition (at) / session->samplesPerBeat());
        beginEdit ("record notes", "midi");

        for (const auto& [key, h] : heldNotes)
            for (auto t : midiRecordTracks)
                session->track (t).appendChild (Session::makeNote (h.beat, endBeat - h.played, h.pitch, h.vel, h.channel + 1), um());
    }

    heldNotes.clear();
}
} // namespace taw
