// Instrument tracks: a hosted plugin (VST3, or an Audio Unit on macOS) per track, played by the MIDI controller while the
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

// Builds the track's notes and controllers in samples (the graph's copy of them): what its MIDI
// clips show, each note cut off at its clip's end.
Sequence buildSequence (const juce::ValueTree& track, double samplesPerBeat)
{
    Sequence seq;

    for (const auto& p : Session::placedNotes (track))
    {
        Sequence::Note note;
        note.start = std::llround (p.start * samplesPerBeat);
        note.end = juce::jmax (note.start + 1, (juce::int64) std::llround (p.end * samplesPerBeat));
        note.channel = (juce::uint8) channelOf (p.node);
        note.pitch = (juce::uint8) juce::jlimit (0, 127, (int) p.node[ids::pitch]);
        note.vel = (juce::uint8) juce::jlimit (1, 127, (int) p.node[ids::vel]);
        seq.notes.push_back (note);
    }

    for (const auto& p : Session::placedControls (track))
        seq.controls.push_back ({ std::llround (p.at * samplesPerBeat), controlMessage (p.node[ids::num], p.node[ids::val], channelOf (p.node)) });

    std::stable_sort (seq.notes.begin(), seq.notes.end(), [] (const auto& a, const auto& b) { return a.start < b.start; });
    std::stable_sort (seq.controls.begin(), seq.controls.end(), [] (const auto& a, const auto& b) { return a.pos < b.pos; });
    return seq;
}

//==============================================================================
// node: an instrument track or an INSERT; both keep plugin, pluginName and pluginState.
std::shared_ptr<PluginSlot> App::createPlugin (const juce::ValueTree& node, double rate, int block, juce::String& problem)
{
    const auto name = node[ids::pluginName].toString();
    auto desc = plugins.byIdentifier (node[ids::plugin].toString());

    if (! desc && name.isNotEmpty())
        desc = plugins.find (name, ! node.hasType (ids::INSERT));

    if (! desc && name.isEmpty() && node[ids::plugin].toString().isEmpty())
    {
        problem = L"no instrument yet \x2014 inst <name> picks one";
        return {};
    }

    if (! desc)
    {
        problem = (name.isNotEmpty() ? name : juce::String ("its plugin"))
                  + (plugins.hasScanned() ? juce::String (" isn't installed") : juce::String (L" not found \x2014 type scan"));
        return {};
    }

    // Loading can take seconds (Roland Cloud, big sample libraries) and nothing redraws until
    // it's done, plugins load on this thread, so put up a box saying what's loading first.
    const auto track = node.hasType (ids::INSERT) ? node.getParent() : node;
    loadingPlugin = desc->name;
    loadingWhy = "for " + (track.hasType (ids::SESSION) ? juce::String ("the mix") : track[ids::name].toString())
                 + (node.hasType (ids::INSERT) ? " (insert " + node[ids::slot].toString().toUpperCase() + ")" : juce::String())
                 + L" \x00b7 big ones take a few seconds; taw waits";

    if (console.isOpen())
        ui.render (*this);

    const auto channels = node.hasType (ids::INSERT) && ! trackIsStereo (node.getParent()) ? 1 : 2;
    auto slot = plugins.create (*desc, rate, block, problem, channels);

    if (slot != nullptr)
    {
        const auto stateFile = node[ids::pluginState].toString();

        if (stateFile.isNotEmpty())
        {
            juce::MemoryBlock state;

            if (pluginStateFile (stateFile, false).loadFileAsData (state))
                slot->setState (state);
        }
    }

    loadingPlugin.clear();
    return slot;
}

App::Hosted* App::hostedEntry (const juce::ValueTree& node)
{
    for (auto& in : hosted)
        if (in.node == node)
            return &in;

    if (! Session::isInstrumentTrack (node) && ! node.hasType (ids::INSERT))
        return nullptr;

    // First time it's played: load the plugin (and its saved state).
    Hosted in;
    in.node = node;
    const auto rate = engine.deviceSampleRate() > 0.0 ? engine.deviceSampleRate() : sampleRate();
    in.slot = createPlugin (node, rate, engine.maxBlockSize(), in.problem);

    if (in.slot != nullptr)
        in.savedState = in.slot->getState();

    hosted.push_back (std::move (in));
    return &hosted.back();
}

// Plugins load after a session opens, one a frame, most wanted first: an armed instrument, other
// instruments, inserts that are on, then bypassed ones. Until then a track is silent; opening a
// plugin's window or bouncing loads what it needs at once.
App::Hosted* App::liveEntry (const juce::ValueTree& node)
{
    for (auto& in : hosted)
        if (in.node == node)
            return &in;

    if ((Session::isInstrumentTrack (node) || node.hasType (ids::INSERT))
        && std::find (pluginQueue.begin(), pluginQueue.end(), node) == pluginQueue.end())
        pluginQueue.push_back (node);

    return nullptr;
}

bool App::pluginPending (const juce::ValueTree& node) const
{
    return std::find (pluginQueue.begin(), pluginQueue.end(), node) != pluginQueue.end();
}

bool App::loadNextPlugin()
{
    // Gone (undo) or loaded some other way meanwhile: nothing to do for those.
    pluginQueue.erase (std::remove_if (pluginQueue.begin(), pluginQueue.end(),
                                       [this] (const juce::ValueTree& node)
                                       {
                                           const auto track = node.hasType (ids::INSERT) ? node.getParent() : node;
                                           return ! track.isValid() || (! track.hasType (ids::SESSION) && ! track.getParent().isValid())
                                                  || std::any_of (hosted.begin(), hosted.end(), [&] (const auto& in) { return in.node == node; });
                                       }),
                       pluginQueue.end());

    if (pluginQueue.empty() || session == nullptr)
        return false;

    auto rank = [] (const juce::ValueTree& node)
    {
        if (! node.hasType (ids::INSERT))
            return (bool) node[ids::armed] ? 0 : 1;

        return (bool) node[ids::bypass] ? 3 : 2;
    };

    const auto next = std::min_element (pluginQueue.begin(), pluginQueue.end(),
                                        [&] (const auto& a, const auto& b) { return rank (a) < rank (b); });
    const auto node = *next;
    pluginQueue.erase (next);
    {
        timing::Scope t ("plugin loaded: " + node[ids::pluginName].toString());
        hostedEntry (node);
    }

    if (pluginQueue.empty() && console.isOpen())
        say (Result::good ("plugins loaded"));

    rebuildGraph(); // the track plays now
    return true;
}

PluginSlot* App::instrumentFor (int track)
{
    if (session == nullptr || track < 0 || track >= session->numTracks())
        return nullptr;

    auto* in = hostedEntry (session->track (track));
    return in != nullptr ? in->slot.get() : nullptr;
}

juce::String App::instrumentProblem (int track) const
{
    if (session == nullptr || track < 0 || track >= session->numTracks())
        return {};

    for (const auto& in : hosted)
        if (in.node == session->track (track))
            return in.problem;

    return {};
}

// Plugin state lives in its own file (plugins/…), not in session.json: it's opaque and can be
// megabytes. Written only when it changed.
void App::writePluginState (Hosted& in)
{
    const auto path = in.node[ids::pluginState].toString();

    if (in.slot == nullptr || path.isEmpty())
        return;

    auto state = in.slot->getState();

    if (state == in.savedState)
        return;

    const auto file = pluginStateFile (path, true);
    file.getParentDirectory().createDirectory();

    if (file.replaceWithData (state.getData(), state.getSize()))
        in.savedState = std::move (state);
}

void App::savePluginStates()
{
    if (session != nullptr)
        for (auto& in : hosted)
            writePluginState (in);
}

void App::pollPlugins()
{
    if (session == nullptr)
        return;

    std::vector<void*> windows;

    for (auto& in : hosted)
    {
        if (in.slot != nullptr && in.slot->editorWantsToClose())
        {
            in.slot->closeEditor();
            writePluginState (in);
        }

        if (in.slot != nullptr)
            if (auto* w = in.slot->editorHandle())
            {
                windows.push_back (w);

                // Its title bar double-clicked: it keeps the keyboard (or gives it back).
                if (in.slot->takeTitleDoubleClick())
                    console.toggleKeyboardHold (w);
            }
    }

    // Clicking in a plugin's window takes the keyboard; it comes back to taw once the mouse is
    // done, unless its title was double-clicked (then it keeps it, and says so). The MIDI
    // editor keeps it while it's in front.
    if (midiEditor != nullptr)
        midiEditor->poll();

    if (audioEditor != nullptr)
        audioEditor->poll();

    console.reclaimKeyboard (windows, editorWindow());

    for (auto& in : hosted)
        if (in.slot != nullptr)
            in.slot->markKeyboard (in.slot->editorHandle() != nullptr && in.slot->editorHandle() == console.keyboardHeldBy());

    // A new device (or new settings): every plugin gets prepared for its rate and block size.
    if (engine.isDeviceRunning() && engine.deviceGeneration() != preparedGeneration)
    {
        preparedGeneration = engine.deviceGeneration();

        for (auto& in : hosted)
            if (in.slot != nullptr)
                in.slot->prepare (engine.deviceSampleRate(), engine.maxBlockSize());
    }

    // A plugin whose latency changed (a lookahead setting, say): delay compensation follows.
    int latencies = 0;

    for (const auto& in : hosted)
        latencies += in.slot != nullptr ? in.slot->latency() * (int) ((&in - hosted.data()) + 1) : 0;

    if (latencies != lastLatencies)
    {
        lastLatencies = latencies;
        rebuildGraph();
    }

    // Tweaks made in a plugin's window don't touch the session, so save states now and then.
    const auto now = juce::Time::getMillisecondCounterHiRes();

    // Not while playing: building a big plugin's state (a ZENOLOGY's is 200 KB) can hold up its
    // audio and click. Stopping saves them.
    if (now - lastStateSave > 15000.0 && ! engine.isRolling())
    {
        lastStateSave = now;
        savePluginStates();
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
            hosted.erase (std::remove_if (hosted.begin(), hosted.end(), [] (const auto& in) { return in.slot == nullptr; }),
                               hosted.end());
            rebuildGraph();

            const auto n = (int) plugins.instruments().size();
            const auto fx = (int) plugins.effects().size();
            auto msg = "found " + juce::String (n) + (n == 1 ? " instrument" : " instruments") + " and " + juce::String (fx)
                       + (fx == 1 ? " effect" : " effects");

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
    return Result::good (midiInputs->opening()           ? juce::String (L"opening the MIDI inputs\x2026 rows show open as they come in")
                         : midiInputs->devices().empty() ? juce::String (L"no MIDI inputs \x2014 plug a controller in (taw picks it up)")
                                                         : juce::String (L"play your controller: its row lights up"));
}

Result App::toggleStats()
{
    statsVisible = ! statsVisible;

    if (statsVisible)
    {
        midiPanelVisible = helpVisible = settingsVisible = pluginPickerVisible = false;
        system.sample();
        systemSampledAt = juce::Time::getMillisecondCounterHiRes();
    }

    return Result::good (statsVisible ? juce::String (L"stats \x00b7 keys still work (space plays) \x00b7 esc or stats closes") : juce::String());
}

bool App::handleStatsKey (const Key& k)
{
    if (! statsVisible || ! commandLine.isEmpty() || k.code != Key::Escape)
        return false;

    statsVisible = false;
    return true;
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
                midiInputs->setOn (d.name, on); // opens or closes it in the background
                midiOff = midiInputs->switchedOff();
                saveSettings();
                say (Result::good (! on ? d.name + " off"
                                        : d.name + L" on \x00b7 its row says open once it's in (if not, another app has it: close it, then midi again)"));
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

void App::openPluginPicker (int targetTrack, int effectSlot)
{
    pluginPickSlot = effectSlot;
    pluginPickMono = effectSlot >= 0 && targetTrack >= 0 && ! trackIsStereo (session->track (targetTrack));

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

    for (const auto& d : pluginPickSlot >= 0 ? plugins.effectsFor (pluginPickMono) : plugins.instruments())
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
                say (pluginPickSlot >= 0 ? loadEffect (pluginPickTarget, pluginPickSlot, choices[(size_t) pluginPickIndex])
                                         : loadInstrument (pluginPickTarget, choices[(size_t) pluginPickIndex]));
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

        if (Session::isBusTrack (track))
            return Result::fail ("track " + juce::String (index + 1) + L" is a bus \x2014 inst alone makes a new instrument track");

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
        for (auto it = hosted.begin(); it != hosted.end(); ++it)
            if (it->node == track)
            {
                if (it->slot != nullptr)
                    it->slot->closeEditor();

                hosted.erase (it);
                break;
            }
    }

    const auto base = juce::String (index + 1).paddedLeft ('0', 2) + "-" + juce::File::createLegalFileName (desc.name).replaceCharacter (' ', '_');
    const auto stateFile = newPluginStateFile (base);

    track.setProperty (ids::kind, "instrument", um())
        .setProperty (ids::input, 0, um())
        .setProperty (ids::plugin, desc.createIdentifierString(), um())
        .setProperty (ids::pluginName, desc.name, um())
        .setProperty (ids::pluginState, stateFile.getRelativePathFrom (session->folder()).replaceCharacter ('\\', '/'), um());

    hosted.push_back ({ track, std::move (slot), {}, {} });
    focusTrack (index);
    rebuildGraph();

    auto msg = "track " + juce::String (index + 1) + " plays " + desc.name;
    const bool armed = track[ids::armed];

    if (midiInputs != nullptr && ! midiInputs->opening() && midiInputs->names().isEmpty())
        return Result::good (msg + L" \x00b7 no MIDI input found yet (plug one in) \x00b7 ed opens its window");

    return Result::good (msg + (armed ? L" \x00b7 armed: play your controller" : L" \x00b7 shift+R arms it for your controller")
                         + L" \x00b7 ed opens its window \x00b7 ctrl+space records");
}

// import song.mid: each of the file's tracks (each channel, in a one-track file) becomes an
// instrument track holding one MIDI clip at the cursor, whole bars long, named after the
// track; inst on it then picks what plays it. "3 import x.mid" puts the first part on track 3.
// The file's tempo is taken when the session has nothing in it yet; a tempo map isn't.
Result App::importMidiFile (const juce::File& file, int targetTrack)
{
    juce::FileInputStream in (file);
    juce::MidiFile midi;

    if (! in.openedOk() || ! midi.readFrom (in))
        return Result::fail ("couldn't read " + file.getFileName() + " as a MIDI file");

    // Beats from the file's ticks (or from seconds, for an SMPTE file, at the session's tempo).
    const int format = midi.getTimeFormat();

    if (format <= 0)
        midi.convertTimestampTicksToSeconds();

    double fileBpm = 0.0;
    juce::MidiMessageSequence tempos;
    midi.findAllTempoEvents (tempos);

    if (tempos.getNumEvents() > 0)
        fileBpm = 60.0 / tempos.getEventPointer (0)->message.getTempoSecondsPerQuarterNote();

    const bool emptySession = session->contentEnd() <= 0;

    if (emptySession && fileBpm >= 20.0 && fileBpm <= 400.0)
        session->tree().setProperty (ids::bpm, std::round (fileBpm * 100.0) / 100.0, um());

    auto beatOf = [&] (double t) { return format > 0 ? t / format : t * session->bpm() / 60.0; };

    struct Part { juce::String name; int channel; std::vector<juce::ValueTree> nodes; double end = 0.0; };
    std::vector<Part> parts;

    for (int t = 0; t < midi.getNumTracks(); ++t)
    {
        juce::MidiMessageSequence seq (*midi.getTrack (t));
        seq.updateMatchedPairs();
        juce::String name;
        std::map<int, Part> byChannel;

        for (int e = 0; e < seq.getNumEvents(); ++e)
        {
            const auto* ev = seq.getEventPointer (e);
            const auto& m = ev->message;
            const auto beat = beatOf (m.getTimeStamp());

            if (m.isTrackNameEvent() && name.isEmpty())
                name = m.getTextFromTextMetaEvent().trim();

            if (m.getChannel() <= 0)
                continue;

            auto& part = byChannel[m.getChannel()];
            part.channel = m.getChannel();

            if (m.isNoteOn())
            {
                const auto end = ev->noteOffObject != nullptr ? beatOf (ev->noteOffObject->message.getTimeStamp()) : beat + 0.25;
                part.nodes.push_back (Session::makeNote (beat, end - beat, m.getNoteNumber(), m.getVelocity(), m.getChannel()));
                part.end = std::max (part.end, end);
            }
            else if (m.isController() || m.isPitchWheel() || m.isChannelPressure())
            {
                const int num = m.isController() ? m.getControllerNumber() : m.isPitchWheel() ? 128 : 129;
                const int val = m.isController() ? m.getControllerValue() : m.isPitchWheel() ? m.getPitchWheelValue() : m.getChannelPressureValue();
                part.nodes.push_back (Session::makeControl (beat, num, val, m.getChannel()));
                part.end = std::max (part.end, beat);
            }
        }

        // A part needs a note; controllers alone (a file's setup track) aren't one.
        for (auto& [ch, part] : byChannel)
        {
            const bool hasNotes = std::any_of (part.nodes.begin(), part.nodes.end(), [] (const auto& n) { return n.hasType (ids::NOTE); });

            if (! hasNotes)
                continue;

            part.name = name.isNotEmpty() ? (byChannel.size() > 1 ? name + " " + juce::String (ch) : name)
                                          : file.getFileNameWithoutExtension() + " " + juce::String ((int) parts.size() + 1);
            parts.push_back (std::move (part));
        }
    }

    if (parts.empty())
        return Result::fail (file.getFileName() + " has no notes");

    beginEdit ("import " + file.getFileName());
    const auto at = (double) cursor / session->samplesPerBeat();
    const auto bar = (double) session->beatsPerBar();
    int first = -1;

    for (size_t i = 0; i < parts.size(); ++i)
    {
        auto& part = parts[i];
        juce::ValueTree track;
        const int target = i == 0 ? targetTrack : -1;

        if (target >= 0 && target < session->numTracks()
            && (Session::isInstrumentTrack (session->track (target)) || (Session::isAudioTrack (session->track (target)) && Session::clips (session->track (target)).empty())))
        {
            track = session->track (target);
        }
        else
        {
            track = session->addTrack (part.name, 0, um());
        }

        track.setProperty (ids::kind, "instrument", um()).setProperty (ids::input, 0, um());

        auto clip = Session::makeMidiClip (at, 0.0, std::max (bar, std::ceil (part.end / bar - 1.0e-9) * bar));

        for (auto& n : part.nodes)
            clip.appendChild (n, nullptr);

        track.appendChild (clip, um());
        first = first < 0 ? session->tree().indexOf (track) : first;
    }

    focusTrack (first);
    rebuildGraph();

    return Result::good ("imported " + file.getFileName() + ": " + juce::String ((int) parts.size()) + (parts.size() == 1 ? " part" : " parts")
                         + (emptySession && fileBpm > 0.0 ? " at " + juce::String (session->bpm(), 1) + " bpm" : juce::String())
                         + L" \x00b7 inst <name> on each track picks what plays it");
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
        if (auto* in = hostedEntry (session->track (index)))
        {
            in->slot->closeEditor();
            writePluginState (*in);
        }

        return Result::good ("closed " + slot->name());
    }

    const auto t = session->track (index);
    closePluginWindows(); // one at a time

    if (auto* in = hostedEntry (t))
        in->touched = true; // whatever happens in there may differ from the save

    slot->showEditor (juce::String (index + 1) + " " + t[ids::name].toString() + L" \x00b7 " + slot->name() + L" \x00b7 taw");
    return Result::good (slot->name() + L" window open \x00b7 | again (or its close button) closes it");
}

//==============================================================================
// Recording what the controller plays

juce::int64 App::heardPosition (juce::int64 playhead)
{
    // The playhead is what's being sent out now; you heard it an output latency later, and
    // that's what you played along to.
    // Delay compensation makes the whole mix later still.
    if (session != nullptr && (bool) session->tree()[ids::latencyComp])
        if (auto* d = engine.devices().getCurrentAudioDevice())
            return playhead - d->getOutputLatencyInSamples() - mixLatency;

    return playhead - mixLatency;
}

void App::drainRecordedMidi()
{
    for (TimedMidi m; engine.popRecordedMidi (m);)
    {
        if (session == nullptr || midiRecordTracks.empty())
            continue;

        const auto played = recordedBeat (m.pos, m.wraps);
        const auto& msg = m.msg;
        const int key = msg.channel() * 128 + msg.d1;

        auto finish = [&] (const HeldNote& h, double endBeat)
        {
            beginEdit ("record notes", "midi");
            const auto len = endBeat - h.played;
            recordMidiNode (Session::makeNote (h.beat, len, h.pitch, h.vel, h.channel + 1), h.beat, h.beat + len);
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
            recordMidiNode (Session::makeControl (played, num, val, msg.channel() + 1), played, played);
        }
    }
}

// Each pass records into a new clip on each track, made with its first note or controller: from
// where the pass began (or earlier, for a note quantized back past it), its content beats the
// timeline's, so everything lands where it was played. Nothing played, no clip.
void App::recordMidiNode (juce::ValueTree node, double from, double to)
{
    for (auto t : midiRecordTracks)
    {
        auto& clip = midiPassClips[t];

        if (! clip.isValid())
        {
            const auto start = std::min (midiPassStart, from);
            clip = Session::makeMidiClip (start, start, std::max (to, start) - start);
            session->track (t).appendChild (clip, um());
        }

        Session::growMidiClip (clip, from, to, um());
        clip.appendChild (node.createCopy(), um());
    }
}

// Keys still down when recording stops end there, and so do the pass's clips.
void App::finishHeldNotes (juce::int64 at)
{
    drainRecordedMidi();

    if (session != nullptr)
    {
        const auto endBeat = recordedBeat (at, engine.loopWrapCount());

        if (! heldNotes.empty())
        {
            beginEdit ("record notes", "midi");

            for (const auto& [key, h] : heldNotes)
                recordMidiNode (Session::makeNote (h.beat, endBeat - h.played, h.pitch, h.vel, h.channel + 1), h.beat, h.beat + endBeat - h.played);
        }

        // Out to where you stopped. Not its own undo step: undoing the pass takes the clip.
        for (auto& [t, clip] : midiPassClips)
            if (clip.getParent().isValid())
                Session::growMidiClip (clip, (double) clip[ids::beat], endBeat, nullptr);
    }

    heldNotes.clear();
    midiPassClips.clear();
}
} // namespace taw
