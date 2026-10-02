#include "App.h"
#include "Safety.h"

namespace taw
{
static juce::File deviceStateFile() { return configFolder().getChildFile ("audio-device.xml"); }
static juce::File settingsFile() { return configFolder().getChildFile ("settings.json"); }

//==============================================================================
void MeterState::update (float peak, double now, double dt)
{
    constexpr double fallDbPerSecond = 24.0, holdSeconds = 2.0;

    level = juce::jmax (peak, level * (float) std::pow (10.0, -fallDbPerSecond * dt / 20.0));

    if (peak >= hold || now - holdTime > holdSeconds)
    {
        hold = juce::jmax (peak, level);
        holdTime = now;
    }

    if (peak >= 0.999f)
        clipped = true;
}

//==============================================================================
App::App (juce::File openDir, juce::File newParent, juce::String newName, juce::String templateName)
    : newSessionName (std::move (newName)),
      newSessionParent (std::move (newParent)),
      pendingTemplate (std::move (templateName))
{
    configFolder().createDirectory();
    loadSettings();

    // If taw crashes, save what's open on the way down.
    safety::setEmergencySave ([] (void* app)
                              {
                                  if (auto& s = static_cast<App*> (app)->session; s != nullptr)
                                      s->save();
                              },
                              this);

    if (openDir != juce::File())
    {
        juce::String error;

        if (auto s = Session::load (openDir, error))
            attachSession (std::move (s));
        else
            say (Result::fail (error));
    }
}

App::~App()
{
    midiInputs.reset(); // its callbacks feed the engine's queues
    releaseSession();
    safety::setEmergencySave (nullptr, nullptr);
}

// Stops the transport (keeping any take), saves, and lets go of the session.
void App::releaseSession()
{
    if (session != nullptr)
    {
        if (recording || engine.isRolling())
        {
            stop();

            // Give the audio thread a moment to let go of the recorders so takes get saved.
            for (int i = 0; i < 50 && ! live.empty(); ++i)
            {
                juce::Thread::sleep (10);
                collectFinishedTakes();
            }
        }

        // Plugins: their state to disk, their windows closed. The instances themselves go with
        // the last graph that plays them.
        saveInstrumentStates();

        for (auto& in : instruments)
            if (in.slot != nullptr)
                in.slot->closeEditor();

        instruments.clear();
        heldNotes.clear();
        midiRecordTracks.clear();
        fillView = pluginPickerVisible = false;

        session->tree().removeListener (this);
        session->save();
        session.reset();
    }
}

Result App::closeSession()
{
    if (session == nullptr)
        return Result::fail ("no session open");

    if (bounce != nullptr || import != nullptr)
        return Result::fail (L"wait for the bounce or import to finish (esc cancels it)");

    const auto name = session->name();

    // Opened by folder, the start screen still lists the sessions next to this one.
    if (newSessionParent == juce::File())
        newSessionParent = session->folder().getParentDirectory();

    releaseSession();
    engine.setGraph (std::make_unique<Graph>()); // silence: nothing of the old session plays on

    clips.clear();
    clipboard = {};
    clearSelection();
    commandLine.clear();
    helpVisible = settingsVisible = false;
    newSessionName.clear();
    pendingTemplate.clear();
    safety::markRunning ({});

    proceedToSession(); // no session, so: the recent sessions and templates
    return Result::good ("closed " + name + " (saved)");
}

int App::run()
{
    if (! console.open())
    {
#if JUCE_WINDOWS
        std::fputs ("taw needs an interactive console (cmd, Windows Terminal).\n", stderr);
#else
        std::fputs ("taw needs an interactive terminal (Terminal, iTerm2).\n", stderr);
#endif
        return 1;
    }

    startupDevice();

    // MIDI controllers, all of them, for armed instrument tracks. Only the interactive app opens
    // them (Windows MIDI inputs can't be shared).
    midiInputs = std::make_unique<MidiInputs> (engine.midiInputQueues(), midiOff);

    auto last = juce::Time::getMillisecondCounterHiRes();

    // Closing the window (or Ctrl+Break) quits like q does: recording stops and takes are kept.
    while (! quitRequested && ! safety::closeRequested())
    {
        const auto keys = console.poll();
        int chars = 0;

        for (const auto& k : keys)
            chars += (k.code == Key::Char && ! k.ctrl && ! k.alt) ? 1 : 0;

        // A drop (or paste) arrives as a burst of typed characters, faster than anyone types.
        // Collect the burst instead of running it as keys (it must never fire hotkeys), and act
        // on it once it has finished arriving.
        if (dropPending && keys.empty())
            finishDrop();
        else if (mode == Mode::Main && (dropPending || chars >= 4))
        {
            for (const auto& k : keys)
                if (k.code == Key::Char)
                    dropBuffer += juce::String::charToString ((juce::juce_wchar) k.ch);

            dropPending = true;
        }
        else
        {
            for (const auto& k : keys)
                handleKey (k);
        }
        collectFinishedTakes();
        drainLiveOverviews();
        drainRecordedMidi();
        pollInstruments();
        pollBounce();
        pollImport();

        if (treeDirty && session != nullptr)
        {
            treeDirty = false;

            // Undo can remove tracks out from under the focus or a selection.
            focused = juce::jlimit (0, session->numTracks(), focused);
            if (trackAnchor >= session->numTracks())
                trackAnchor = -1;

            rebuildGraph();
            session->save();
        }

        if (engine.isRolling())
        {
            if (juce::Time::getMillisecondCounterHiRes() - cursorMovedAt > 3000.0)
                keepInView (engine.playhead());
        }

        const auto now = juce::Time::getMillisecondCounterHiRes();
        updateMeters ((now - last) / 1000.0);
        last = now;

        ui.render (*this);

        // Pumps JUCE's message queue (device notifications, driver panels) between frames.
        juce::MessageManager::getInstance()->runDispatchLoopUntil (15);
    }

    return 0;
}

//==============================================================================
// Startup: device first (the session needs its sample rate), then the session.

void App::startupDevice()
{
    auto& dm = engine.devices();
    std::unique_ptr<juce::XmlElement> saved;

    if (deviceStateFile().existsAsFile())
        saved = juce::parseXML (deviceStateFile());

    const auto error = dm.initialise (AudioEngine::maxInputs, 2, saved.get(), false);

    if (saved == nullptr || error.isNotEmpty() || dm.getCurrentAudioDevice() == nullptr)
    {
        if (saved != nullptr)
            say (Result::fail ("couldn't open the saved audio device" + (error.isNotEmpty() ? ": " + error : juce::String())));

        openDevicePicker();
        return;
    }

    proceedToSession();
}

void App::proceedToSession()
{
    mode = Mode::Main;

    if (session != nullptr)
    {
        matchDeviceSampleRate();
        rebuildGraph();
        return;
    }

    templates = loadTemplates();

    if (pendingTemplate.isNotEmpty())
    {
        for (const auto& t : templates)
            if (t.name.equalsIgnoreCase (pendingTemplate))
            {
                say (createSession (t));
                return;
            }

        say (Result::fail ("no template called " + pendingTemplate));
    }

    // One list: recent sessions first (the newest is selected, so Enter reopens it), then
    // templates for a new session.
    recent = loadRecentSessions();

    // Sessions sitting in the launch folder count too, even if never opened from this machine.
    for (const auto& dir : newSessionParent.findChildFiles (juce::File::findDirectories, false))
        if (Session::existsIn (dir) && std::find (recent.begin(), recent.end(), dir) == recent.end())
            recent.push_back (dir);

    std::stable_sort (recent.begin(), recent.end(), [] (const auto& a, const auto& b)
                      { return a.getChildFile ("session.json").getLastModificationTime() > b.getChildFile ("session.json").getLastModificationTime(); });

    if (recent.size() > 12)
        recent.resize (12);

    pickIndex = recent.empty() ? defaultTemplateRow() : 0;
    mode = Mode::TemplatePicker;
}

int App::defaultTemplateRow() const
{
    for (int i = 0; i < (int) templates.size(); ++i)
        if (templates[(size_t) i].name == "1mic")
            return (int) recent.size() + i;

    return (int) recent.size();
}

Result App::createSession (const Template& t)
{
    if (newSessionName.trim().isEmpty())
        newSessionName = "session-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M");

    const auto dir = newSessionParent.getChildFile (juce::File::createLegalFileName (newSessionName.trim()));
    juce::String error;
    std::unique_ptr<Session> s;

    if (Session::existsIn (dir))
        s = Session::load (dir, error);
    else
        s = Session::create (dir, t.tree, sampleRate() > 0 ? sampleRate() : 48000.0, error);

    if (s == nullptr)
        return Result::fail (error);

    attachSession (std::move (s));
    mode = Mode::Main;
    matchDeviceSampleRate();
    rebuildGraph();
    return Result::good ("session " + session->name() + L" \x00b7 " + juce::String (session->numTracks()) + L" tracks \x00b7 "
                         + dir.getFullPathName());
}

void App::attachSession (std::unique_ptr<Session> s)
{
    session = std::move (s);
    session->tree().addListener (this);
    rememberSession (session->folder());
    safety::markRunning (session->folder());
    focused = 0;
    topLane = TopLane::none;
    selectedMarker = {};
    cursor = playStart = viewStart = 0;

    if (session->loadNote.isNotEmpty())
        say (Result::fail (session->loadNote));

    recoverTakes();
}

void App::matchDeviceSampleRate()
{
    auto* device = engine.devices().getCurrentAudioDevice();

    if (device == nullptr || session == nullptr || device->getCurrentSampleRate() == session->sampleRate())
        return;

    auto setup = engine.devices().getAudioDeviceSetup();
    setup.sampleRate = session->sampleRate();
    engine.devices().setAudioDeviceSetup (setup, true);

    if (device = engine.devices().getCurrentAudioDevice(); device != nullptr && device->getCurrentSampleRate() != session->sampleRate())
        say (Result::fail ("device runs at " + juce::String (device->getCurrentSampleRate()) + " Hz but the session is "
                           + juce::String (session->sampleRate()) + L" Hz \x2014 playback will be off-speed"));
}

//==============================================================================
// Devices

void App::openDevicePicker()
{
    auto& dm = engine.devices();
    deviceChoices.clear();
    pickIndex = 0;

    const auto* current = dm.getCurrentAudioDevice();

    for (auto* type : dm.getAvailableDeviceTypes())
    {
        type->scanForDevices();
        const auto outputs = type->getDeviceNames (false);
        const auto inputs = type->getDeviceNames (true);
        const auto defaultInput = inputs[juce::jmax (0, type->getDefaultDeviceIndex (true))];

        for (const auto& o : outputs)
        {
            const auto in = inputs.contains (o) ? o : defaultInput;

            if (current != nullptr && current->getTypeName() == type->getTypeName() && current->getName() == o)
                pickIndex = (int) deviceChoices.size();

            deviceChoices.push_back ({ type->getTypeName(), o, in });
        }
    }

    // Put ASIO first: it's what a dedicated interface should be using. (macOS has only CoreAudio.)
    std::stable_partition (deviceChoices.begin(), deviceChoices.end(), [] (const auto& c) { return c.type == "ASIO"; });

    if (current != nullptr)
        for (int i = 0; i < (int) deviceChoices.size(); ++i)
            if (deviceChoices[(size_t) i].type == current->getTypeName() && deviceChoices[(size_t) i].output == current->getName())
                pickIndex = i;

    mode = Mode::DevicePicker;
}

Result App::chooseDevice (const DeviceChoice& c)
{
    if (recording || engine.isRolling())
        stop();

    auto& dm = engine.devices();
    dm.setCurrentAudioDeviceType (c.type, true);

    auto setup = dm.getAudioDeviceSetup();
    setup.outputDeviceName = c.output;
    setup.inputDeviceName = c.input;
    setup.useDefaultInputChannels = true;
    setup.useDefaultOutputChannels = true;
    setup.sampleRate = session != nullptr ? session->sampleRate() : 0.0;

    auto error = dm.setAudioDeviceSetup (setup, true);

    if (error.isNotEmpty() && setup.sampleRate > 0)
    {
        setup.sampleRate = 0; // fall back to whatever the device wants
        error = dm.setAudioDeviceSetup (setup, true);
    }

    auto* device = dm.getCurrentAudioDevice();

    if (error.isNotEmpty() || device == nullptr)
        return Result::fail ("couldn't open " + c.output + (error.isNotEmpty() ? ": " + error : juce::String()));

    // Open every input so all of them can be metered and recorded; outputs 1-2 are the mix.
    setup = dm.getAudioDeviceSetup();
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    setup.inputChannels.clear();
    setup.inputChannels.setRange (0, juce::jmin (AudioEngine::maxInputs, device->getInputChannelNames().size()), true);
    setup.outputChannels.clear();
    setup.outputChannels.setRange (0, juce::jmin (2, device->getOutputChannelNames().size()), true);
    error = dm.setAudioDeviceSetup (setup, true);

    if (error.isNotEmpty())
        return Result::fail (error);

    if (auto xml = dm.createStateXml())
        xml->writeTo (deviceStateFile());

    device = dm.getCurrentAudioDevice();
    return Result::good (c.output + L" \x00b7 " + juce::String (device->getActiveInputChannels().countNumberOfSetBits()) + " in / "
                         + juce::String (device->getActiveOutputChannels().countNumberOfSetBits()) + L" out \x00b7 "
                         + juce::String (device->getCurrentSampleRate() / 1000.0, 1) + L" kHz \x00b7 "
                         + juce::String (device->getCurrentBufferSizeSamples()) + " samples");
}

Result App::showControlPanel()
{
    auto* device = engine.devices().getCurrentAudioDevice();

    if (device == nullptr)
        return Result::fail (L"no audio device \x2014 type dev");

#if JUCE_MAC
    // CoreAudio devices have no driver panel; their settings live in Audio MIDI Setup.
    if (! device->hasControlPanel())
    {
        juce::ChildProcess open;
        return open.start (juce::StringArray { "open", "-a", "Audio MIDI Setup" }, 0)
                   ? Result::good ("opened Audio MIDI Setup")
                   : Result::fail ("couldn't open Audio MIDI Setup");
    }
#endif

    if (! device->hasControlPanel())
        return Result::fail (device->getTypeName() + " has no driver panel");

    // Buffer size / clock source may change underneath us; JUCE reopens the device.
    if (device->showControlPanel())
        engine.devices().restartLastAudioDevice();

    return Result::good ("driver panel closed");
}

//==============================================================================
// Transport and recording

Result App::play()
{
    if (engine.devices().getCurrentAudioDevice() == nullptr)
        return Result::fail (L"no audio device \x2014 type dev");

    if (engine.isRolling())
        return Result::good();

    // With the loop on, play always starts at the loop; the cursor stays where you left it.
    playStart = cursor;
    cursorMovedWhileRolling = false;
    engine.play (loopActive() ? (juce::int64) session->tree()[ids::loopStart] : cursor);
    return Result::good();
}

bool App::loopActive() const
{
    if (session == nullptr || ! (bool) session->tree()[ids::loop])
        return false;

    return (juce::int64) session->tree()[ids::loopEnd] > (juce::int64) session->tree()[ids::loopStart];
}

// Enter on an empty line: back to where play started. Stopped and already there, back to the
// session start. While rolling it keeps playing from there.
Result App::returnPlayhead()
{
    if (session == nullptr)
        return Result::good();

    if (recording)
        return Result::fail ("can't move while recording");

    if (engine.isRolling())
    {
        engine.seek (loopActive() ? (juce::int64) session->tree()[ids::loopStart] : playStart);
        return Result::good();
    }

    moveTo (cursor != playStart ? playStart : 0, false);
    return Result::good();
}

Result App::stop()
{
    const bool wasRecording = recording;

    if (recording)
        stopRecorders();

    engine.stop();
    // Back to where play started, unless you moved the cursor during playback.
    setCursor (cursorMovedWhileRolling ? cursor : playStart);
    cursorMovedWhileRolling = false;
    return Result::good (wasRecording ? juce::String (L"stopped \x00b7 saving take") : juce::String());
}

// Ctrl+Space / "rec": record the armed tracks. With tracks given ("1 rec", for scripts and
// agents), arm exactly those first. While playing, this punches in.
Result App::record (const std::vector<int>& tracks)
{
    if (session == nullptr)
        return Result::fail ("no session");

    if (recording)
        return Result::fail ("already recording");

    if (engine.devices().getCurrentAudioDevice() == nullptr)
        return Result::fail (L"no audio device \x2014 type dev");

    if (! tracks.empty())
        for (int i = 0; i < session->numTracks(); ++i)
            session->track (i).setProperty (ids::armed, std::find (tracks.begin(), tracks.end(), i) != tracks.end(), um());

    std::vector<int> armed;

    for (int i = 0; i < session->numTracks(); ++i)
        if ((bool) session->track (i)[ids::armed])
            armed.push_back (i);

    if (armed.empty())
        return Result::fail (L"nothing armed \x2014 shift+R arms the track, then ctrl+space records");

    if (engine.isRolling())
        return punch();

    if (auto r = startRecorders (armed); ! r.ok)
        return r;

    if (! engine.isRolling())
    {
        playStart = cursor;
        engine.play (cursor);
    }

    juce::StringArray names;

    for (auto i : armed)
        names.add (juce::String (i + 1));

    return Result::good ("recording " + names.joinIntoString (","));
}

Result App::punch()
{
    if (recording)
    {
        stopRecorders();
        return Result::good ("punched out");
    }

    std::vector<int> armed;

    for (int i = 0; i < session->numTracks(); ++i)
        if ((bool) session->track (i)[ids::armed])
            armed.push_back (i);

    if (armed.empty())
        return Result::fail ("nothing armed to punch into");

    auto r = startRecorders (armed);
    return r.ok ? Result::good ("punched in") : r;
}

Result App::startRecorders (const std::vector<int>& armedTracks)
{
    const auto numInputs = engine.numInputChannels();

    // Armed sample tracks record what you play on the pad key (playPads), armed instrument
    // tracks what you play on the controller (drainRecordedMidi); only audio tracks get files.
    std::vector<int> tracks;

    for (auto i : armedTracks)
        if (Session::isAudioTrack (session->track (i)))
            tracks.push_back (i);

    for (auto i : tracks)
    {
        const int input = session->track (i)[ids::input];

        if (input < 1 || input > numInputs)
            return Result::fail ("track " + juce::String (i + 1) + " has no valid input (device has " + juce::String (numInputs)
                                 + L") \x2014 try " + juce::String (i + 1) + " i 1");
    }

    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
    std::vector<LiveTake> started;

    for (auto i : tracks)
    {
        const auto t = session->track (i);
        const auto base = juce::String (i + 1).paddedLeft ('0', 2) + "-"
                          + juce::File::createLegalFileName (t[ids::name].toString()).replaceCharacter (' ', '_') + "-" + stamp;
        const auto file = session->audioFolder().getNonexistentChildFile (base, ".wav", false);

        juce::String error;
        auto rec = engine.createRecorder (i, (int) t[ids::input] - 1, file, sampleRate(), error);

        if (rec == nullptr)
        {
            for (auto& s : started)
                s.recorder->file.deleteFile();

            return Result::fail (error);
        }

        started.push_back ({ std::move (rec), {}, -1, false });
    }

    for (auto& s : started)
        live.push_back (std::move (s));

    midiRecordTracks.clear();
    heldNotes.clear();

    for (auto i : armedTracks)
        if (Session::isInstrumentTrack (session->track (i)))
            midiRecordTracks.push_back (i);

    recording = true;
    rebuildGraph();
    writeTakeJournal();
    return Result::good();
}

void App::stopRecorders()
{
    std::vector<std::shared_ptr<Recorder>> recs;

    for (auto& l : live)
        if (! l.finishing)
        {
            l.finishing = true;
            recs.push_back (l.recorder);
        }

    finishHeldNotes (engine.playhead());
    midiRecordTracks.clear();
    recording = false;
    rebuildGraph(); // a graph without the recorders; once it's live they get closed
    engine.finishRecorders (std::move (recs));
}

void App::drainLiveOverviews()
{
    bool started = false;

    for (auto& l : live)
    {
        for (float v; l.recorder->overview.pop (v);)
            l.overview.push_back (v);

        if (l.start < 0)
        {
            l.start = l.recorder->startSample.load();
            started = started || l.start >= 0;
        }
    }

    if (started)
        writeTakeJournal(); // now it knows where the take goes
}

juce::int64 App::latencyCompensation()
{
    // What was captured at playhead P was played against audio heard (in + out latency) earlier.
    if (session != nullptr && (bool) session->tree()[ids::latencyComp])
        if (auto* d = engine.devices().getCurrentAudioDevice())
            return d->getInputLatencyInSamples() + d->getOutputLatencyInSamples();

    return 0;
}

void App::writeTakeJournal()
{
    if (session == nullptr)
        return;

    const auto journal = session->folder().getChildFile ("recording.json");

    if (live.empty())
    {
        journal.deleteFile();
        return;
    }

    juce::Array<juce::var> takes;
    const auto comp = latencyCompensation();

    for (const auto& l : live)
    {
        auto* t = new juce::DynamicObject();
        t->setProperty ("file", l.recorder->file.getRelativePathFrom (session->folder()).replaceCharacter ('\\', '/'));
        t->setProperty ("track", l.recorder->trackIndex);
        t->setProperty ("start", l.start);
        t->setProperty ("comp", comp);
        takes.add (juce::var (t));
    }

    auto* o = new juce::DynamicObject();
    o->setProperty ("takes", takes);
    journal.replaceWithText (juce::JSON::toString (juce::var (o)));
}

void App::recoverTakes()
{
    const auto journal = session->folder().getChildFile ("recording.json");

    if (! journal.existsAsFile())
        return;

    const auto parsed = juce::JSON::parse (journal);
    juce::StringArray recovered;
    int lost = 0;

    if (const auto* takes = parsed["takes"].getArray())
    {
        for (const auto& t : *takes)
        {
            const auto rel = t["file"].toString();
            const auto file = session->folder().getChildFile (rel);
            const int track = t["track"];
            const auto start = (juce::int64) t["start"];

            if (! file.existsAsFile() || track < 0 || track >= session->numTracks())
            {
                ++lost;
                continue;
            }

            // The crash came after this take was already placed.
            bool placed = false;

            for (const auto& c : Session::clips (session->track (track)))
                placed = placed || c[ids::file].toString() == rel;

            if (placed)
                continue;

            juce::int64 length = 0;

            if (safety::repairWavHeader (file))
                if (std::unique_ptr<juce::AudioFormatReader> r (engine.formats().createReaderFor (file)); r != nullptr)
                    length = r->lengthInSamples;

            if (length <= 0 || start < 0)
            {
                ++lost; // nothing was captured; the file stays in audio\ in case
                continue;
            }

            beginEdit ("recover take");
            placeTake (track, file, start, length, (juce::int64) t["comp"]);
            recovered.add ("track " + juce::String (track + 1) + " (" + juce::String ((double) length / sampleRate(), 1) + " s)");
        }
    }

    journal.deleteFile();
    session->save();

    if (! recovered.isEmpty())
        say (Result::good (L"recovered the take that was recording: " + recovered.joinIntoString (", ")));
    else if (lost > 0)
        say (Result::fail ("a take was recording when taw stopped, but nothing in it could be recovered"));
}

void App::collectFinishedTakes()
{
    for (const auto& f : engine.poll())
    {
        // A take that finished after its session was closed: its journal stays behind, so the
        // next open of that session recovers it.
        if (session == nullptr)
        {
            live.clear();
            continue;
        }

        live.erase (std::remove_if (live.begin(), live.end(), [&] (const auto& l) { return l.recorder->file == f.file; }),
                    live.end());

        if (f.length <= 0 || f.start < 0)
        {
            f.file.deleteFile();
            writeTakeJournal();
            continue;
        }

        beginEdit ("record track " + juce::String (f.trackIndex + 1));
        placeTake (f.trackIndex, f.file, f.start, f.length, latencyCompensation());

        // Saved before the journal forgets the take, so a crash between the two can't lose it.
        session->save();
        writeTakeJournal();

        auto msg = L"take saved \x00b7 track " + juce::String (f.trackIndex + 1) + L" \x00b7 " + juce::String ((double) f.length / sampleRate(), 1) + " s";

        if (f.droppedBlocks > 0)
            say (Result::fail (msg + L" \x00b7 disk too slow, " + juce::String (f.droppedBlocks) + " blocks dropped"));
        else
            say (Result::good (msg));
    }
}

void App::placeTake (int trackIndex, const juce::File& file, juce::int64 start, juce::int64 length, juce::int64 comp)
{
    start -= comp;

    // A take recorded from the very start can land before zero once compensated: start the
    // clip at zero and skip into the file instead.
    const auto offset = start < 0 ? -start : (juce::int64) 0;
    const auto rel = file.getRelativePathFrom (session->folder()).replaceCharacter ('\\', '/');
    session->track (trackIndex).appendChild (Session::makeClip (rel, start + offset, offset, length - offset), um());
}

void App::beginEdit (const juce::String& name, const juce::String& coalesceKey)
{
    if (session == nullptr)
        return;

    const auto now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    const bool merge = coalesceKey.isNotEmpty() && coalesceKey == lastEditKey && now - lastEditTime < 2.0;

    if (! merge)
        session->undo()->beginNewTransaction (name);

    lastEditKey = coalesceKey;
    lastEditTime = now;
}

Result App::undoEdit()
{
    if (session == nullptr)
        return Result::fail ("no session");

    if (recording)
        return Result::fail ("stop recording first");

    auto* u = session->undo();
    const auto what = u->getUndoDescription();

    if (! u->undo())
        return Result::fail ("nothing to undo");

    lastEditKey.clear(); // never merge into an undone step
    return Result::good ("undid " + (what.isNotEmpty() ? what : juce::String ("last edit")) + L" \x00b7 ctrl+y redoes");
}

Result App::redoEdit()
{
    if (session == nullptr)
        return Result::fail ("no session");

    if (recording)
        return Result::fail ("stop recording first");

    auto* u = session->undo();
    const auto what = u->getRedoDescription();

    if (! u->redo())
        return Result::fail ("nothing to redo");

    lastEditKey.clear();
    return Result::good ("redid " + (what.isNotEmpty() ? what : juce::String ("last edit")));
}

void App::pumpBackgroundWork()
{
    collectFinishedTakes();
    pollInstruments();
    pollBounce();
    pollImport();

    if (treeDirty && session != nullptr)
    {
        treeDirty = false;
        rebuildGraph();
        session->save();
    }
}

Result App::addMarker (juce::int64 position, const juce::String& name)
{
    juce::ValueTree m (ids::MARKER);
    int n = 1;

    for (const auto& c : session->tree())
        n += c.hasType (ids::MARKER) ? 1 : 0;

    const auto label = name.isNotEmpty() ? name : juce::String (n);
    m.setProperty (ids::position, position, um()).setProperty (ids::name, label, um());
    session->tree().appendChild (m, um());

    if (name.isNotEmpty())
        return Result::good ("marker " + label + " at " + formatBars (position));

    namingMarker = m; // the command line asks for a name next
    commandLine.clear();
    return Result::good ("marker " + label + " at " + formatBars (position) + L" \x00b7 type a name + enter \x00b7 esc keeps " + label);
}

Result App::discardTake (int trackIndex)
{
    auto t = session->track (trackIndex);

    if (Session::isSampleTrack (t))
        return Result::fail ("track " + juce::String (trackIndex + 1) + L" is a sample track \x00b7 pat or enter edit its hits");

    auto clip = Session::lastClip (t);

    if (! clip.isValid())
        return Result::fail ("track " + juce::String (trackIndex + 1) + " has no clips");

    // Non-destructive: the file stays in audio/, only the clip goes, uncovering anything under it.
    t.removeChild (clip, um());
    return Result::good ("dropped the latest clip on track " + juce::String (trackIndex + 1) + " (file kept in audio/)");
}

//==============================================================================
// Graph

const ClipData* App::clipFor (const juce::ValueTree& clip)
{
    return audioFor (clip[ids::file].toString());
}

const ClipData* App::audioFor (const juce::String& path)
{
    if (path.isEmpty())
        return nullptr;

    if (auto it = clips.find (path); it != clips.end())
        return &it->second;

    const auto file = session->folder().getChildFile (path);
    std::unique_ptr<juce::AudioFormatReader> reader (engine.formats().createReaderFor (file));

    if (reader == nullptr)
    {
        say (Result::fail ("missing audio " + path));
        return nullptr;
    }

    // POC: takes live in memory. Streaming from disk comes with longer sessions.
    const auto channels = juce::jlimit (1, 2, (int) reader->numChannels); // tracks are mono or stereo
    auto buffer = std::make_shared<juce::AudioBuffer<float>> (channels, (int) reader->lengthInSamples);
    reader->read (buffer.get(), 0, (int) reader->lengthInSamples, 0, true, channels > 1);

    ClipData data;

    for (int i = 0; i < buffer->getNumSamples(); i += Recorder::overviewBlock)
    {
        const auto n = juce::jmin (Recorder::overviewBlock, buffer->getNumSamples() - i);
        data.overview.push_back (buffer->getMagnitude (i, n));
    }

    data.audio = std::move (buffer);
    return &clips.emplace (path, std::move (data)).first->second;
}

void App::rebuildGraph()
{
    if (session != nullptr)
        engine.setGraph (buildGraph());
}

std::unique_ptr<Graph> App::buildGraph (bool offline)
{
    auto g = std::make_unique<Graph>();
    const auto n = session->numTracks();
    bool anySolo = false;

    for (int i = 0; i < n; ++i)
        anySolo = anySolo || (bool) session->track (i)[ids::solo];

    for (int i = 0; i < n; ++i)
    {
        const auto t = session->track (i);
        TrackPlay tp;
        tp.inputChannel = (int) t[ids::input] - 1;
        tp.armed = t[ids::armed];
        tp.inputOnly = t[ids::monitor]; // "mon": hear the input during playback too
        tp.audible = ! (bool) t[ids::mute] && (! anySolo || (bool) t[ids::solo]);
        tp.gain = juce::Decibels::decibelsToGain ((float) (double) t[ids::gainDb], -96.0f);

        if (Session::isInstrumentTrack (t))
        {
            // Armed means "listening to the MIDI controller", not to an audio input.
            tp.midiLive = tp.armed && ! offline;
            tp.armed = false;
            tp.stereo = true;
            tp.sequence = buildSequence (t, session->samplesPerBeat());

            if (auto* in = instrumentEntry (t); in != nullptr && in->slot != nullptr)
            {
                if (offline)
                {
                    // A bounce plays its own copy, so the live one keeps playing meanwhile.
                    juce::String problem;
                    tp.plugin = createInstrument (t, sampleRate(), 8192, problem);

                    if (tp.plugin != nullptr)
                    {
                        tp.plugin->setState (in->slot->getState());
                        tp.plugin->setNonRealtime (true);
                    }
                }
                else
                {
                    tp.plugin = in->slot;
                }
            }

            g->tracks.push_back (std::move (tp));
            continue;
        }

        // A bus plays what's sent to it (applyRouting, below): no input, no clips, never armed.
        if (Session::isBusTrack (t))
        {
            tp.inputChannel = -1;
            tp.armed = tp.inputOnly = false;
            tp.stereo = true;
            g->tracks.push_back (std::move (tp));
            continue;
        }

        if (Session::isSampleTrack (t))
        {
            // Armed means "listening to the pad key", not to an input: its hits keep playing.
            tp.armed = false;

            // One sample, triggered by hits. Hits are in beats, so they follow the tempo.
            if (const auto* data = audioFor (t[ids::sample].toString()))
            {
                const auto fileLength = (juce::int64) data->audio->getNumSamples();
                tp.sample = data->audio;
                tp.sampleOffset = juce::jlimit<juce::int64> (0, fileLength, (juce::int64) t[ids::sampleOffset]);
                const auto wanted = (juce::int64) t[ids::sampleLength];
                tp.sampleLength = wanted > 0 ? juce::jmin (wanted, fileLength - tp.sampleOffset) : fileLength - tp.sampleOffset;
                tp.stereo = data->audio->getNumChannels() > 1;

                for (const auto& h : Session::hits (t))
                    tp.hits.push_back ({ std::llround ((double) h[ids::beat] * session->samplesPerBeat()), (float) (double) h[ids::vel] });

                std::sort (tp.hits.begin(), tp.hits.end(), [] (const auto& a, const auto& b) { return a.pos < b.pos; });
            }

            for (const auto& l : live)
                if (! l.finishing && l.recorder->trackIndex == i)
                    tp.recorder = l.recorder;

            g->tracks.push_back (std::move (tp));
            continue;
        }

        for (const auto& c : Session::clips (t))
        {
            const auto* data = clipFor (c);

            if (data == nullptr)
                continue;

            // Clamp to the file, in case session.json was edited by hand.
            const auto fileLength = (juce::int64) data->audio->getNumSamples();
            const auto offset = juce::jlimit<juce::int64> (0, fileLength, (juce::int64) c[ids::offset]);
            const auto length = juce::jlimit<juce::int64> (0, fileLength - offset, (juce::int64) c[ids::length]);

            if (length > 0)
            {
                tp.clips.push_back ({ data->audio, (juce::int64) c[ids::start], offset, length });
                tp.stereo = tp.stereo || data->audio->getNumChannels() > 1;
            }
        }

        for (const auto& l : live)
            if (! l.finishing && l.recorder->trackIndex == i)
                tp.recorder = l.recorder;

        g->tracks.push_back (std::move (tp));
    }

    applyRouting (*g);

    // Plugins of tracks that are gone (undo) or no longer instruments: state saved, window
    // closed. Undoing back brings the plugin back from that state.
    if (! offline)
        for (auto it = instruments.begin(); it != instruments.end();)
        {
            if (! it->track.getParent().isValid() || ! Session::isInstrumentTrack (it->track))
            {
                writeInstrumentState (*it);

                if (it->slot != nullptr)
                    it->slot->closeEditor();

                it = instruments.erase (it);
            }
            else
            {
                ++it;
            }
        }

    const auto& s = session->tree();
    g->mixGain = juce::Decibels::decibelsToGain ((float) (double) s[ids::mixDb], -96.0f);
    g->bpm = session->bpm();
    g->timeSigNum = session->beatsPerBar();
    g->timeSigDen = juce::jmax (1, (int) s[ids::timeSigDen]);
    g->recording = recording;
    g->loop = s[ids::loop];
    g->loopStart = (juce::int64) s[ids::loopStart];
    g->loopEnd = (juce::int64) s[ids::loopEnd];
    g->click.enabled = s[ids::click];
    g->click.recordOnly = s[ids::clickWhen].toString() == "rec";
    g->click.gain = juce::Decibels::decibelsToGain ((float) (double) s[ids::clickDb]);
    g->click.samplesPerBeat = session->samplesPerBeat();
    g->click.beatsPerBar = session->beatsPerBar();
    return g;
}

//==============================================================================
// Import

juce::String App::expandHome (const juce::String& path)
{
#if JUCE_WINDOWS
    return path;
#else
    if (path == "~" || path.startsWith ("~/"))
        return juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName() + path.substring (1);

    return path;
#endif
}

juce::StringArray App::splitPaths (const juce::String& text)
{
    const auto t = text.trim();
    juce::StringArray out;

#if ! JUCE_WINDOWS
    // macOS terminals drop files shell-style: My\ Song.wav, or 'quoted'. Split the way a shell
    // would, then expand ~.
    if (t.containsAnyOf ("\\'~"))
    {
        juce::String current;
        bool inToken = false;
        juce::juce_wchar quote = 0;

        for (auto p = t.getCharPointer(); ! p.isEmpty();)
        {
            const auto c = p.getAndAdvance();

            if (quote != 0)
            {
                if (c == quote)
                    quote = 0;
                else if (c == '\\' && quote == '"' && ! p.isEmpty())
                    current += juce::String::charToString (p.getAndAdvance());
                else
                    current += juce::String::charToString (c);
            }
            else if (c == '\\' && ! p.isEmpty())
            {
                current += juce::String::charToString (p.getAndAdvance());
                inToken = true;
            }
            else if (c == '\'' || c == '"')
            {
                quote = c;
                inToken = true;
            }
            else if (c == ' ' || c == '\t')
            {
                if (inToken)
                    out.add (expandHome (current));

                current.clear();
                inToken = false;
            }
            else
            {
                current += juce::String::charToString (c);
                inToken = true;
            }
        }

        if (inToken)
            out.add (expandHome (current));

        out.removeEmptyStrings();
        return out;
    }
#endif

    if (t.containsChar ('"'))
    {
        out = juce::StringArray::fromTokens (t, " ", "\"");

        for (auto& s : out)
            s = s.unquoted().trim();
    }
    else if (juce::File::isAbsolutePath (t) && juce::File (t).exists())
    {
        out.add (t); // one unquoted path with spaces in it
    }
    else
    {
        out = juce::StringArray::fromTokens (t, " ", {});
    }

    out.removeEmptyStrings();
    return out;
}

Result App::startImport (const juce::StringArray& paths, int targetTrack, ImportAs as)
{
    if (import != nullptr)
        return Result::fail (L"already importing \x00b7 esc cancels");

    if (recording)
        return Result::fail ("stop recording first");

    if (paths.isEmpty())
#if JUCE_WINDOWS
        return Result::fail (L"import what? import D:\\stems\\kick.wav \x00b7 or drop files here");
#else
        return Result::fail (L"import what? import ~/stems/kick.wav \x00b7 or drop files here");
#endif

    if (targetTrack >= 0 && targetTrack < session->numTracks() && Session::isBusTrack (session->track (targetTrack)))
        return Result::fail ("track " + juce::String (targetTrack + 1) + L" is a bus \x2014 import alone makes new tracks");

    juce::StringArray notFound;
    auto files = Import::expand (paths, juce::File::getCurrentWorkingDirectory(), notFound);

    if (files.empty())
        return Result::fail ("nothing to import" + (notFound.isEmpty() ? juce::String() : ": not found " + notFound.joinIntoString (", ")));

    importAt = cursor;
    importTarget = targetTrack;
    importAs = as;
    import = std::make_unique<Import> (std::move (files), session->audioFolder(), sampleRate());
    import->start();

    auto msg = "importing " + juce::String (import->total()) + (import->total() == 1 ? " file" : " files");
    return notFound.isEmpty() ? Result::good (msg) : Result::fail (msg + L" \x00b7 not found: " + notFound.joinIntoString (", "));
}

void App::pollImport()
{
    if (import == nullptr)
        return;

    if (! import->isDone())
    {
        message = "importing " + juce::String (juce::roundToInt (import->progress() * 100.0f)) + L"% \x00b7 esc cancels";
        messageIsError = false;
        return;
    }

    juce::StringArray placed, failed;
    beginEdit ("import"); // the whole import is one undo step

    for (const auto& item : import->results())
    {
        if (item.copy == juce::File())
        {
            if (item.error.isNotEmpty())
                failed.add (item.error);

            continue;
        }

        // Drops: one-shots (under 2 s) become sample tracks, anything longer stays audio.
        const bool asSample = importAs == ImportAs::sample
                              || (importAs == ImportAs::autoByLength && (double) item.length < 2.0 * sampleRate());

        auto track = importTarget >= 0 ? session->track (importTarget) : juce::ValueTree();

        if (! track.isValid())
            track = session->addTrack (item.source.getFileNameWithoutExtension().substring (0, 24), 0, um());

        const auto file = item.copy.getRelativePathFrom (session->folder()).replaceCharacter ('\\', '/');

        if (asSample)
        {
            // A new sample track starts with no hits; giving an existing track a new sample
            // keeps its hits, so a pattern can swap its sound.
            track.setProperty (ids::kind, "sample", um())
                .setProperty (ids::sample, file, um())
                .setProperty (ids::sampleOffset, 0, um())
                .setProperty (ids::sampleLength, 0, um());
        }
        else
        {
            track.appendChild (Session::makeClip (file, importAt, 0, item.length), um());
        }

        const auto index = session->tree().indexOf (track); // tracks come first in the tree
        placed.add (juce::String (index + 1) + " " + track[ids::name].toString() + (asSample ? " (sample)" : "")
                    + (item.channels > 1 ? " (stereo)" : ""));
        focusTrack (index);
    }

    if (import->wasCancelled())
        say (Result::fail ("import cancelled" + (placed.isEmpty() ? juce::String() : L" \x00b7 kept " + placed.joinIntoString (", "))));
    else if (! failed.isEmpty())
        say (Result::fail ((placed.isEmpty() ? juce::String() : "imported " + placed.joinIntoString (", ") + L" \x00b7 ")
                           + failed.joinIntoString (juce::String (L" \x00b7 "))));
    else
        say (Result::good ("imported " + placed.joinIntoString (", ") + " at " + formatBars (importAt)));

    import.reset();
}

// Files dropped on the console arrive as text: one or more (quoted) absolute paths. If that's
// all the burst was, import them straight away; otherwise it was an ordinary paste and goes
// onto the command line as typed text.
void App::pasteFromKey()
{
    const auto targets = defaultTargets();
    beginEdit ("paste");
    say (paste (targets.empty() ? -1 : targets.front()));
}

void App::finishDrop()
{
    dropPending = false;
    const auto text = dropBuffer;
    dropBuffer.clear();

    // The terminal turned Ctrl+V into the marker Ctrl+C left on the clipboard.
    if (text.trim() == pasteMarker)
    {
        pasteFromKey();
        return;
    }

    if (session != nullptr)
    {
        const auto paths = splitPaths (text);
        juce::StringArray resolved;

        for (const auto& p : paths)
            if (juce::File::isAbsolutePath (p.unquoted()))
                if (const auto f = Import::resolve (p, {}); f.exists())
                    resolved.add (f.getFullPathName());

        if (! resolved.isEmpty() && resolved.size() == paths.size())
        {
            say (startImport (resolved, -1, ImportAs::autoByLength));
            return;
        }
    }

    commandLine += text;
}

//==============================================================================
// Bounce

Result App::startBounce (const juce::StringArray& args)
{
    if (bounce != nullptr)
        return Result::fail (L"already bouncing \x00b7 esc cancels");

    if (recording)
        return Result::fail ("stop recording first");

    if (! hasTimeSelection())
        return Result::fail (L"select a time range first (shift + \x2190 \x2192), then b");

    auto s = session->tree();
    juce::StringArray nameParts;

    // b | b mp3 | b wav D:\mixes | b mp3 "rough mix 3"  (format and folder are remembered)
    for (const auto& a : args)
    {
        if (a.equalsIgnoreCase ("wav") || a.equalsIgnoreCase ("mp3"))
            s.setProperty (ids::bounceFormat, a.toLowerCase(), nullptr);
        else if (a.containsAnyOf ("\\/:"))
            s.setProperty (ids::bounceFolder, expandHome (a.unquoted()), nullptr);
        else
            nameParts.add (a.unquoted());
    }

    const auto format = s[ids::bounceFormat].toString() == "mp3" ? Bounce::Format::mp3 : Bounce::Format::wav;
    const auto folder = session->folder().getChildFile (s[ids::bounceFolder].toString());
    const auto name = nameParts.isEmpty() ? session->name() + "-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S")
                                          : nameParts.joinIntoString (" ");
    auto file = folder.getChildFile (juce::File::createLegalFileName (name) + Bounce::extension (format));

    if (file.exists())
        file = file.getNonexistentSibling (false);

    bounce = std::make_unique<Bounce> (buildGraph (true), timeSelection(), sampleRate(), format, file);
    bounce->start();
    return Result::good ("bouncing " + formatBars (timeSelection().getStart()) + L" \x2192 " + formatBars (timeSelection().getEnd()));
}

void App::pollBounce()
{
    if (bounce == nullptr)
        return;

    if (! bounce->isDone())
    {
        message = "bouncing " + juce::String (juce::roundToInt (bounce->progress() * 100.0f)) + L"% \x00b7 esc cancels";
        messageIsError = false;
        return;
    }

    if (bounce->wasCancelled())
        say (Result::good ("bounce cancelled"));
    else if (! bounce->succeeded())
        say (Result::fail ("bounce failed: " + bounce->errorMessage()));
    else
    {
        const auto peakDb = juce::Decibels::gainToDecibels (bounce->peak(), -100.0f);
        const auto summary = juce::String (bounce->seconds(), 1) + L" s \x2192 " + bounce->file().getFullPathName()
                             + L" \x00b7 peak " + formatDb (peakDb) + " dB";

        // Over 0 dBFS means the file is clipped (WAV and MP3 both store fixed point here).
        if (peakDb > 0.0f)
            say (Result::fail ("bounced, but it clips! " + summary));
        else
            say (Result::good ("bounced " + summary));
    }

    bounce.reset();
}

//==============================================================================
// Navigation

double App::sampleRate() const
{
    if (session != nullptr)
        return session->sampleRate();

    if (auto* d = const_cast<App*> (this)->engine.devices().getCurrentAudioDevice())
        return d->getCurrentSampleRate();

    return 48000.0;
}

double App::samplesPerColumn() const
{
    // Grid on: one column per beat. Grid off: half a second. Each zoom step doubles or halves.
    const auto base = (session != nullptr && session->gridOn()) ? session->samplesPerBeat() : sampleRate() * 0.5;
    return base * std::pow (2.0, zoomLevel);
}

void App::nudge (int direction, Step step, bool extendSelection)
{
    if (session == nullptr)
        return;

    const bool selecting = extendSelection && timeAnchor >= 0;

    // Selections grow from their moving end; otherwise the cursor moves. Never the playhead:
    // during playback the arrows place the cursor and select, while the music keeps going.
    const auto from = selecting ? timeHead : cursor;
    // Plain steps follow the grid lines on screen, so they shrink as you zoom in (a bar, a beat,
    // an 8th...). Fine (alt) steps follow the grid division: a beat, or a 16th after "grid 16".
    const bool fine = step == Step::Fine || step == Step::FineLarge;
    const auto times = (step == Step::Large || step == Step::FineLarge) ? 4.0 : 1.0;
    const auto unit = times * (session->gridOn() ? (fine ? session->samplesPerBeat() * hitStepBeats() : gridSpacing().finest())
                                                 : (fine ? 0.1 : 1.0) * sampleRate());
    double to;

    if (session->gridOn())
    {
        // Snap to the grid in the direction of travel.
        const auto units = (double) from / unit;
        // Grid lines can fall between samples (99 bpm: a beat is 26727.27 samples), so a position
        // within half a sample of a line counts as on it.
        const auto slack = 0.5 / unit;
        to = direction > 0 ? (std::floor (units + slack) + 1) * unit : (std::ceil (units - slack) - 1) * unit;
    }
    else
    {
        to = (double) from + direction * unit;
    }

    moveTo (juce::jmax<juce::int64> (0, std::llround (to)), extendSelection);
}

void App::moveTo (juce::int64 pos, bool extendSelection)
{
    // The cursor (and the selection) move; the playhead never does. Stopped, the cursor is where
    // play starts. Rolling, it's where pastes land, and stop leaves you there.
    if (extendSelection)
    {
        if (timeAnchor < 0)
            timeAnchor = cursor;

        timeHead = pos;
    }
    else
    {
        clearTimeSelection();
    }

    if (engine.isRolling())
    {
        cursorMovedWhileRolling = true;
        cursorMovedAt = juce::Time::getMillisecondCounterHiRes();
    }

    setCursor (pos); // rides the selection's moving end so it stays in view
}

void App::setCursor (juce::int64 pos)
{
    cursor = juce::jmax<juce::int64> (0, pos);

    if (! engine.isRolling())
        engine.seek (cursor);

    keepInView (cursor);
}

void App::keepInView (juce::int64 pos)
{
    const auto span = (juce::int64) (samplesPerColumn() * layout::laneWidth (console.width(), ioVisible));

    if (pos < viewStart || pos >= viewStart + span)
    {
        // Page so the position sits a quarter of the way in, aligned to a column.
        const auto spc = samplesPerColumn();
        viewStart = juce::jmax<juce::int64> (0, (juce::int64) (std::floor (((double) pos - span / 4.0) / spc) * spc));
    }
}

void App::focusTrack (int index, bool extendSelection)
{
    if (session == nullptr)
        return;

    const auto n = session->numTracks();

    if (extendSelection)
    {
        if (n == 0)
            return;

        // Track selections cover tracks only, never the mix.
        if (trackAnchor < 0)
            trackAnchor = juce::jmin (focused, n - 1);

        focused = juce::jlimit (0, n - 1, index);
        return;
    }

    trackAnchor = -1;
    topLane = TopLane::none;
    selectedMarker = {};
    focused = juce::jlimit (0, n, index); // n = the mix row
}

bool App::mixFocused() const
{
    return session != nullptr && ! onTopLane() && focused >= session->numTracks();
}

// Up from the first track: the bar row, then the markers row. Down comes back the same way.
// Shift only ever selects tracks, so it stops at the first one.
void App::focusUp (bool extendSelection)
{
    if (topLane == TopLane::ruler)
        topLane = TopLane::markers;
    else if (topLane == TopLane::none && focused == 0 && ! extendSelection)
        topLane = TopLane::ruler, trackAnchor = -1;
    else if (topLane == TopLane::none)
        focusTrack (focused - 1, extendSelection);
}

void App::focusDown (bool extendSelection)
{
    if (topLane == TopLane::markers)
        topLane = TopLane::ruler, selectedMarker = {};
    else if (topLane == TopLane::ruler)
        topLane = TopLane::none, focused = 0;
    else
        focusTrack (focused + 1, extendSelection);
}

bool App::playPads (float velocity)
{
    if (session == nullptr)
        return false;

    std::vector<int> armed;

    for (int i = 0; i < session->numTracks(); ++i)
        if (Session::isSampleTrack (session->track (i)) && (bool) session->track (i)[ids::armed])
            armed.push_back (i);

    if (armed.empty())
        return false;

    // While recording, the hit goes where you heard the music when you pressed: the playhead
    // is what's being sent out now, which reaches your ears an output latency later.
    const bool rec = recording && engine.isRolling();
    double beat = 0.0;
    bool landsAhead = false;

    if (rec)
    {
        beat = juce::jmax (0.0, (double) heardPosition (engine.playhead()) / session->samplesPerBeat());

        if ((bool) session->tree()[ids::inputQuantize])
        {
            const auto step = session->fineStepBeats();
            beat = std::round (beat / step) * step;
            // Snapped to a step that hasn't played yet: the timeline will play it on time.
            landsAhead = beat * session->samplesPerBeat() > (double) engine.playhead();
        }

        beginEdit ("record hits", "pads");
    }

    for (auto t : armed)
    {
        if (! landsAhead)
            engine.triggerPad (t, velocity);

        if (rec)
            session->track (t).appendChild (Session::makeHit (beat, velocity), um());
    }

    return true;
}

Result App::quantizeHits (const juce::String& argIn, const std::vector<int>& tracks)
{
    const auto arg = argIn.trim().toLowerCase();

    if (arg == "input" || arg == "in")
    {
        const bool on = ! (bool) session->tree()[ids::inputQuantize];
        session->tree().setProperty (ids::inputQuantize, on, um());
        return Result::good (on ? L"input quantize on \x00b7 recorded hits and notes snap to the grid step" : L"input quantize off \x00b7 hits and notes land as played");
    }

    // quantize [percent]: hits in the time selection (or the whole track) on the selected or
    // focused sample tracks move that far toward the nearest grid step.
    const auto strength = arg.isEmpty() ? 1.0 : juce::jlimit (0.0, 1.0, arg.getDoubleValue() / 100.0);

    if (arg.isNotEmpty() && ! arg.containsOnly ("0123456789."))
        return Result::fail ("quantize, quantize 50 (percent) or quantize input");

    const auto step = session->fineStepBeats();
    const auto spb = session->samplesPerBeat();
    const bool ranged = hasTimeSelection();
    const auto range = timeSelection();
    int moved = 0;

    for (auto t : tracks)
    {
        auto track = session->track (t);

        // Hits on sample tracks, note starts on instrument tracks (a note keeps its length).
        const auto items = Session::isSampleTrack (track) ? Session::hits (track) : Session::notes (track);

        for (auto h : items)
        {
            const auto beat = (double) h[ids::beat];
            const auto pos = (juce::int64) std::llround (beat * spb);

            if (ranged && ! range.contains (pos))
                continue;

            const auto target = std::round (beat / step) * step;

            if (std::abs (target - beat) > 1e-9)
            {
                h.setProperty (ids::beat, beat + (target - beat) * strength, um());
                ++moved;
            }
        }
    }

    if (moved == 0)
        return Result::good (L"nothing to move \x00b7 already on the grid (or no sample or instrument track here)");

    return Result::good ("quantized " + juce::String (moved) + (moved == 1 ? " hit or note" : " hits and notes") + " to 1/"
                         + session->tree()[ids::gridDiv].toString()
                         + (strength < 1.0 ? " at " + juce::String ((int) std::round (strength * 100)) + "%" : juce::String()));
}

double App::hitStepBeats() const
{
    const auto division = session->fineStepBeats();
    const auto shown = gridSpacing().finest() / session->samplesPerBeat();
    return shown > 0.0 ? juce::jmin (division, shown) : division;
}

App::GridSpacing App::gridSpacing() const
{
    GridSpacing g;

    if (session == nullptr || ! session->gridOn())
        return g;

    // Lines at least this many columns apart, so the view never fills with them.
    constexpr double minColumns = 4.0;
    const auto spc = samplesPerColumn();
    g.bars = session->samplesPerBar();

    while (g.bars < spc * minColumns)
        g.bars *= 2.0; // zoomed far out: every 2, 4, 8 bars

    if (g.bars == session->samplesPerBar())
        for (double step = session->samplesPerBeat(); step >= spc * minColumns && step > session->samplesPerBeat() / 16.0; step /= 2.0)
            g.sub = step; // beats, then halves, quarters, eighths of a beat

    return g;
}

juce::Range<juce::int64> App::timeSelection() const
{
    return juce::Range<juce::int64>::between (timeAnchor, timeHead);
}

std::vector<int> App::selectedTracks() const
{
    std::vector<int> out;

    if (trackAnchor >= 0 && session != nullptr)
        for (int i = juce::jmin (trackAnchor, focused); i <= juce::jmax (trackAnchor, focused) && i < session->numTracks(); ++i)
            out.push_back (i);

    return out;
}

std::vector<int> App::defaultTargets() const
{
    if (onTopLane() && session != nullptr)
    {
        std::vector<int> all ((size_t) session->numTracks());
        std::iota (all.begin(), all.end(), 0);
        return all;
    }

    if (auto sel = selectedTracks(); ! sel.empty())
        return sel;

    if (mixFocused())
        return {};

    return { focused };
}

juce::String formatDb (double db)
{
    return db <= -96.0 ? juce::String ("-inf") : (db > 0.0 ? "+" : "") + juce::String (db, 1);
}

Result App::setMixVolume (double db)
{
    db = juce::jlimit (-96.0, 12.0, db);
    session->tree().setProperty (ids::mixDb, db, um());
    return Result::good ("mix " + formatDb (db) + " dB");
}

Result App::nudgeVolume (double deltaDb)
{
    if (session == nullptr)
        return Result::fail ("no session");

    const auto targets = defaultTargets();

    if (targets.empty())
        return setMixVolume ((double) session->tree()[ids::mixDb] + deltaDb);

    for (auto t : targets)
    {
        auto track = session->track (t);
        track.setProperty (ids::gainDb, juce::jlimit (-96.0, 12.0, (double) track[ids::gainDb] + deltaDb), um());
    }

    const auto first = session->track (targets.front());
    return Result::good ((targets.size() == 1 ? first[ids::name].toString() : juce::String ((int) targets.size()) + " tracks") + " "
                         + formatDb (first[ids::gainDb]) + " dB");
}

void App::zoom (int delta)
{
    zoomLevel = juce::jlimit (-6, 8, zoomLevel + delta);
    keepInView (engine.isRolling() ? engine.playhead() : cursor);
}

void App::loadSettings()
{
    const auto json = juce::JSON::parse (settingsFile());
    if (const auto t = json["theme"].toString(); t == "light" || t == "grey")
        theme = t;

    if (json.hasProperty ("io"))
        ioVisible = (bool) json["io"];

    if (const auto* off = json["midiOff"].getArray())
        for (const auto& name : *off)
            midiOff.add (name.toString());
}

void App::saveSettings() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("theme", theme);
    o->setProperty ("io", ioVisible);

    juce::Array<juce::var> off;

    for (const auto& name : midiOff)
        off.add (name);

    o->setProperty ("midiOff", off);
    settingsFile().replaceWithText (juce::JSON::toString (juce::var (o)));
}

// The markers row: the arrows (and tab) go marker to marker, selecting the one they land on and
// taking the cursor with them; with shift the time selection stretches to it instead.
Result App::stepMarker (int direction, bool extendSelection)
{
    if (session == nullptr)
        return Result::good();

    const auto from = extendSelection && timeAnchor >= 0 ? timeHead : cursor;
    juce::ValueTree best;

    for (const auto& m : session->tree())
        if (m.hasType (ids::MARKER))
        {
            const auto pos = (juce::int64) m[ids::position];
            const auto bestPos = best.isValid() ? (juce::int64) best[ids::position] : 0;

            if (direction > 0 ? (pos > from && (! best.isValid() || pos < bestPos))
                              : (pos < from && (! best.isValid() || pos > bestPos)))
                best = m;
        }

    if (! best.isValid())
        return Result::fail (direction > 0 ? "no more markers" : "no markers before this");

    moveTo ((juce::int64) best[ids::position], extendSelection);
    selectedMarker = best;
    return Result::good (extendSelection ? juce::String()
                                         : "marker " + best[ids::name].toString() + L" \x00b7 enter renames \x00b7 del deletes \x00b7 \x2193 back to the tracks");
}

// q / w: the cursor to the selection's start or end, centred in the view; the selection stays,
// so you can select, snap, q, and play to hear it. (No selection: centres on the cursor.)
void App::showSelectionEdge (bool end)
{
    const auto pos = hasTimeSelection() ? (end ? timeSelection().getEnd() : timeSelection().getStart()) : cursor;

    if (engine.isRolling())
        cursorMovedWhileRolling = true; // stop leaves you there, as with the arrows

    setCursor (pos);

    const auto spc = samplesPerColumn();
    const auto columns = layout::laneWidth (console.width(), ioVisible);
    const auto first = std::floor ((double) pos / spc) - columns / 2;
    viewStart = juce::jmax<juce::int64> (0, (juce::int64) (first * spc));
}

// Tab: the cursor to the next clip start or end on the selected (or focused) tracks. With
// shift, the selection stretches to it, so tab then shift+tab selects a whole clip.
Result App::moveToNextClipEdge (bool extendSelection)
{
    if (session == nullptr)
        return Result::good();

    const auto from = cursor;
    juce::int64 next = -1;
    auto consider = [&] (juce::int64 edge) { if (edge > from && (next < 0 || edge < next)) next = edge; };

    // On the markers row it hops marker to marker instead, selecting sections across every track.
    if (topLane == TopLane::markers)
        return stepMarker (+1, extendSelection);

    // Clip starts and ends on audio tracks; each hit on sample tracks.
    for (auto t : defaultTargets())
    {
        const auto track = session->track (t);

        for (const auto& c : Session::clips (track))
        {
            consider ((juce::int64) c[ids::start]);
            consider ((juce::int64) c[ids::start] + (juce::int64) c[ids::length]);
        }

        if (Session::isSampleTrack (track))
            for (const auto& h : Session::hits (track))
                consider (std::llround ((double) h[ids::beat] * session->samplesPerBeat()));

        for (const auto& n : Session::notes (track))
        {
            consider (std::llround ((double) n[ids::beat] * session->samplesPerBeat()));
            consider (std::llround (((double) n[ids::beat] + (double) n[ids::len]) * session->samplesPerBeat()));
        }
    }

    if (next < 0)
        return Result::fail (onTopLane() ? "no more clips or hits" : "nothing more on this track");

    moveTo (next, extendSelection);
    return Result::good();
}

juce::String App::formatBars (juce::int64 pos) const
{
    if (session == nullptr)
        return {};

    // Half a sample of slack, so a grid line that falls between samples reads as itself
    // (2.1.00, not 1.4.99).
    const auto beats = ((double) pos + 0.5) / session->samplesPerBeat();
    const auto bpb = session->beatsPerBar();
    const auto bar = (int) std::floor (beats / bpb) + 1;
    const auto beat = (int) std::floor (std::fmod (beats, (double) bpb)) + 1;
    const auto hundredths = (int) std::floor ((beats - std::floor (beats)) * 100.0);
    return juce::String (bar) + "." + juce::String (beat) + "." + juce::String (hundredths).paddedLeft ('0', 2);
}

juce::String App::formatTime (juce::int64 pos) const
{
    const auto secs = (double) pos / sampleRate();
    const auto minutes = (int) (secs / 60.0);
    const auto rest = secs - minutes * 60.0;
    return juce::String (minutes).paddedLeft ('0', 2) + ":" + juce::String (rest, 3).paddedLeft ('0', 6);
}

//==============================================================================
// Input

void App::handleKey (const Key& k)
{
    switch (mode)
    {
        case Mode::Main:           handleMainKey (k); break;
        case Mode::DevicePicker:   handleDevicePickerKey (k); break;
        case Mode::TemplatePicker: handleTemplatePickerKey (k); break;
    }
}

void App::handleMainKey (const Key& k)
{
    // Arrows: plain = bar, ctrl = 4 bars, alt = 1 step, ctrl+alt = 4 steps. Shift never changes
    // the distance; it extends the selection (time with left/right, tracks with up/down).
    const auto step = k.alt ? (k.ctrl ? Step::FineLarge : Step::Fine) : (k.ctrl ? Step::Large : Step::Normal);

    // Naming a new marker: the command line is its name field. Enter keeps the name, esc keeps
    // the number; any other key (arrows, ctrl+...) leaves the number and does its usual job.
    if (namingMarker.isValid())
    {
        if (k.code == Key::Char && ! k.ctrl && ! k.alt && k.ch >= 32)
        {
            commandLine += juce::String::charToString ((juce::juce_wchar) k.ch);
            return;
        }

        if (k.code == Key::Backspace)
        {
            commandLine = commandLine.dropLastCharacters (1);
            return;
        }

        const auto name = commandLine.trim();
        auto marker = std::exchange (namingMarker, {});
        commandLine.clear();

        if (k.code == Key::Enter)
        {
            if (name.isNotEmpty() && marker.getParent().isValid())
            {
                marker.setProperty (ids::name, name, um()); // same undo step as the marker
                say (Result::good ("marker " + name + " at " + formatBars ((juce::int64) marker[ids::position])));
            }
            else
            {
                message.clear();
            }

            return;
        }

        if (k.code == Key::Escape)
        {
            message.clear();
            return;
        }
    }

    // The project settings panel takes the arrows, enter and esc while it's open; the
    // instrument picker takes typing too (it filters).
    if (handlePluginPickerKey (k) || handleBusPickerKey (k) || handleMidiPanelKey (k) || handleSettingsKey (k) || handleIoKey (k))
        return;

    // The markers row: arrows pick markers; the picked one is renamed with enter, removed with del.
    if (topLane == TopLane::markers && session != nullptr)
    {
        if (selectedMarker.isValid() && ! selectedMarker.getParent().isValid())
            selectedMarker = {}; // undone or deleted

        if ((k.code == Key::Left || k.code == Key::Right) && ! k.alt)
        {
            say (stepMarker (k.code == Key::Right ? +1 : -1, k.shift));
            return;
        }

        if (selectedMarker.isValid() && commandLine.isEmpty())
        {
            if (k.code == Key::Enter)
            {
                beginEdit ("rename marker");
                namingMarker = selectedMarker;
                commandLine = selectedMarker[ids::name].toString(); // edit it in place
                say (Result::good (L"rename: enter keeps \x00b7 esc cancels"));
                return;
            }

            if (k.code == Key::Delete || k.code == Key::Backspace)
            {
                beginEdit ("delete marker");
                const auto name = selectedMarker[ids::name].toString();
                session->tree().removeChild (selectedMarker, um());
                selectedMarker = {};
                say (Result::good ("deleted marker " + name + L" \x00b7 ctrl+z brings it back"));
                return;
            }
        }
    }

    // Navigation works the same whether stopped or rolling.
    switch (k.code)
    {
        case Key::Left:  nudge (-1, step, k.shift); return;
        case Key::Right: nudge (+1, step, k.shift); return;
        // Ctrl jumps to the first / last track; with shift it selects every track on the way.
        case Key::Up:
        case Key::Down:
            if (k.ctrl && session != nullptr && session->numTracks() > 0)
            {
                const auto from = onTopLane() ? 0 : juce::jmin (focused, session->numTracks() - 1);
                topLane = TopLane::none;
                selectedMarker = {};

                if (k.shift && trackAnchor < 0)
                    trackAnchor = from; // the selection grows from where you were

                focusTrack (k.code == Key::Up ? 0 : session->numTracks() - 1, k.shift);
            }
            else
            {
                k.code == Key::Up ? focusUp (k.shift) : focusDown (k.shift);
            }
            return;
        case Key::Home:  moveTo (0, k.shift); return;
        case Key::End:
            if (session != nullptr)
                moveTo (session->contentEnd(), k.shift);
            return;
        case Key::F1: helpVisible = ! helpVisible; settingsVisible = false; return;
        default: break;
    }

    // Delete clears the time selection on the selected (or focused) tracks. So does backspace
    // with nothing typed (a Mac keyboard's delete key sends backspace).
    if ((k.code == Key::Delete || (k.code == Key::Backspace && commandLine.isEmpty())) && hasTimeSelection())
    {
        beginEdit ("delete");
        say (deleteSelection (defaultTargets()));
        return;
    }

    // Alt+/ toggles a hit at the cursor (at the playhead while rolling) on a sample track.
    if (k.code == Key::Char && k.alt && ! k.ctrl && k.ch == '/')
    {
        say (toggleHitAtCursor());
        return;
    }

    // Volume rides act immediately, stopped or rolling, unless you're typing a command.
    // Ctrl (or shift: { }) rides the mix from anywhere, so you don't have to walk down to it.
    // Shift is there because macOS Terminal sends ctrl+[ as Escape.
    if (k.code == Key::Char && (k.ch == '[' || k.ch == ']' || k.ch == '{' || k.ch == '}') && ! k.alt && commandLine.isEmpty())
    {
        const auto delta = (k.ch == ']' || k.ch == '}') ? 1.0 : -1.0;
        const bool mixKey = k.ctrl || k.ch == '{' || k.ch == '}';
        const bool onMix = mixKey || (mixFocused() && selectedTracks().empty());

        if (onTopLane() && ! mixKey)
        {
            say (Result::fail (L"[ ] ride a track \x2014 \x2193 to one (or { } for the mix)"));
            return;
        }

        juce::StringArray who;

        for (auto t : defaultTargets())
            who.add (juce::String (t));

        beginEdit ("volume", onMix ? juce::String ("vol:mix") : "vol:" + who.joinIntoString (","));
        say (mixKey && session != nullptr ? setMixVolume ((double) session->tree()[ids::mixDb] + delta) : nudgeVolume (delta));
        return;
    }

    // Ctrl shortcuts (AltGr arrives as ctrl+alt and types characters, so it's excluded).
    if (k.code == Key::Char && k.ctrl && ! k.alt)
    {
        if (k.ch == 'q')
            quit(); // saves first, like exit
        else if (k.ch == 'z')
            say (undoEdit());
        else if (k.ch == 'y')
            say (redoEdit());
        else if (k.ch == ' ')
            say (record ({})); // record the armed tracks; while playing, punch in/out
        else if (k.ch == 'c' && hasTimeSelection())
            say (copySelection (defaultTargets()));
        else if (k.ch == 'c' && ! commandLine.isEmpty())
            commandLine.clear(); // like a shell; it never quits (q does)
        else if (k.ch == 'c')
            say (Result::fail (L"select a range to copy (shift + \x2190 \x2192) \x00b7 ctrl+q quits"));
        else if (k.ch == 'x')
        {
            beginEdit ("cut");
            say (cutSelection (defaultTargets()));
        }
        else if (k.ch == 'v')
            pasteFromKey();
        else if (k.ch == 'd')
        {
            beginEdit ("duplicate");
            say (duplicateSelection (defaultTargets()));
        }
        else if (k.ch == 'k' || k.ch == 'm') // k: macOS Terminal sends ctrl+m as Enter
        {
            beginEdit ("marker");
            say (addMarker (engine.isRolling() ? engine.playhead() : cursor));
        }


        return; // never type ctrl+letters into the command line
    }

    // The command line works the same stopped or rolling: type "2m" during playback, Enter.
    const bool lineEmpty = commandLine.isEmpty();

    switch (k.code)
    {
        case Key::Enter:
            if (! lineEmpty)
            {
                const auto line = commandLine;
                commandLine.clear();
                say (execute (line));
            }
            else
            {
                say (returnPlayhead());
            }
            return;

        case Key::Backspace:
            commandLine = commandLine.dropLastCharacters (1);
            return;

        case Key::Tab:
            say (moveToNextClipEdge (k.shift));
            return;

        case Key::Escape:
            if (! lineEmpty)
                commandLine.clear();
            else if (engine.isRolling())
                say (stop());
            else if (bounce != nullptr)
                bounce->cancel();
            else if (import != nullptr)
                import->cancel();
            else if (helpVisible)
                helpVisible = false;
            else if (timeAnchor >= 0 || trackAnchor >= 0)
                clearSelection();
            else
            {
                // Clear latched clip lights and the message.
                for (auto& m : inputMeters) m.clipped = false;
                for (auto& m : trackMeters) m.clipped = false;
                for (auto& m : masterMeters) m.clipped = false;
                message.clear();
            }
            return;

        case Key::Char:
            if (lineEmpty && k.ch == ' ')
                say (engine.isRolling() ? stop() : play());
            else if (lineEmpty && (k.ch == '+' || k.ch == '='))
                zoom (-1);
            else if (lineEmpty && k.ch == '-')
                zoom (+1);
            else if (lineEmpty && k.ch == '?')
                helpVisible = ! helpVisible, settingsVisible = false;
            // The pad key: plays (and while recording, records) every armed sample track.
            // Shift+. (>) is an accent. With none armed, . types as usual.
            else if (lineEmpty && (k.ch == '.' || k.ch == '>') && playPads (k.ch == '>' ? 1.0f : 0.8f))
                return;
            // Pro Tools keys on an empty line: r t zoom, q w show the selection's ends, and
            // shift+R S M I arm / solo / mute / monitor the selected (or focused) tracks. Typed
            // commands start with a number or another letter (2m, exit), so they still work.
            else if (lineEmpty && k.ch == 'r')
                zoom (+1);
            else if (lineEmpty && k.ch == 't')
                zoom (-1);
            else if (lineEmpty && (k.ch == 'q' || k.ch == 'w'))
                showSelectionEdge (k.ch == 'w');
            // k: to the markers row (pairs with ctrl+k, add a marker); again: back to the track.
            // (m would block typing mark, mute, mon and mix.)
            else if (lineEmpty && k.ch == 'k' && session != nullptr)
            {
                if (topLane == TopLane::markers)
                    focusTrack (focused);
                else
                    topLane = TopLane::markers, trackAnchor = -1;
            }
            // o: into the routing column on the focused track (the output, then sends A to E).
            else if (lineEmpty && k.ch == 'o' && session != nullptr)
            {
                if (mixFocused() || onTopLane() || session->numTracks() == 0)
                    say (Result::fail (L"o routes a track \x2014 \x2193 to one"));
                else
                {
                    if (! ioVisible)
                        ioVisible = true, saveSettings();

                    ioCell = 0;
                    say (Result::good (describeIoCell (focused, 0)));
                }
            }
            // f: the focused track fills the track area (a piano roll on instrument tracks).
            else if (lineEmpty && k.ch == 'f' && session != nullptr)
                say (execute ("fill"));
            else if (lineEmpty && (k.ch == 'R' || k.ch == 'S' || k.ch == 'M' || k.ch == 'I'))
                say (execute (k.ch == 'R' ? "r" : k.ch == 'S' ? "s" : k.ch == 'M' ? "m" : "mon"));
            else if (k.ch >= 32)
                commandLine += juce::String::charToString ((juce::juce_wchar) k.ch);
            return;

        default:
            return;
    }
}

void App::handleDevicePickerKey (const Key& k)
{
    const auto n = (int) deviceChoices.size();

    switch (k.code)
    {
        case Key::Up:   pickIndex = juce::jmax (0, pickIndex - 1); return;
        case Key::Down: pickIndex = juce::jmin (n - 1, pickIndex + 1); return;
        case Key::Escape:
            if (engine.devices().getCurrentAudioDevice() != nullptr)
                proceedToSession();
            else
                quit();
            return;
        case Key::Enter:
            if (n > 0)
            {
                const auto r = chooseDevice (deviceChoices[(size_t) pickIndex]);
                say (r);

                if (r.ok)
                    proceedToSession();
            }
            return;
        case Key::Char:
            if (k.ch == 'r' || k.ch == 'R')
                openDevicePicker(); // rescan, e.g. after plugging the interface in
            else if (k.ch >= '1' && k.ch <= '9' && k.ch - '1' < n)
                pickIndex = k.ch - '1';
            return;
        default:
            return;
    }
}

void App::handleTemplatePickerKey (const Key& k)
{
    const auto numRecent = (int) recent.size();
    const auto n = numRecent + (int) templates.size();

    switch (k.code)
    {
        case Key::Up:        pickIndex = juce::jmax (0, pickIndex - 1); return;
        case Key::Down:      pickIndex = juce::jmin (n - 1, pickIndex + 1); return;
        case Key::Backspace: newSessionName = newSessionName.dropLastCharacters (1); return;
        case Key::Escape:    quit(); return;
        case Key::Enter:
            if (pickIndex < numRecent)
            {
                juce::String error;

                if (auto s = Session::load (recent[(size_t) pickIndex], error))
                {
                    attachSession (std::move (s));
                    proceedToSession();
                    say (Result::good ("opened " + session->name() + L" \x00b7 " + session->folder().getFullPathName()));
                }
                else
                {
                    say (Result::fail (error));
                }
            }
            else if (pickIndex < n)
            {
                say (createSession (templates[(size_t) (pickIndex - numRecent)]));
            }
            return;
        case Key::Char:
            if (k.ch >= 32)
            {
                newSessionName += juce::String::charToString ((juce::juce_wchar) k.ch);

                // Typing a name means a new session: jump from the recent list to the templates.
                if (pickIndex < numRecent)
                    pickIndex = defaultTemplateRow();
            }
            return;
        default:
            return;
    }
}

//==============================================================================
void App::updateMeters (double dt)
{
    const auto now = juce::Time::getMillisecondCounterHiRes() / 1000.0;

    for (int i = 0; i < AudioEngine::maxInputs; ++i)
        inputMeters[(size_t) i].update (engine.takeInputPeak (i), now, dt);

    for (int i = 0; i < AudioEngine::maxTracks; ++i)
        trackMeters[(size_t) i].update (engine.takeTrackPeak (i), now, dt);

    for (int i = 0; i < 2; ++i)
        masterMeters[(size_t) i].update (engine.takeMasterPeak (i), now, dt);
}

void App::reportUncleanExit (const juce::File& crashLog, bool reopened)
{
    auto text = juce::String (crashLog.existsAsFile() ? "taw crashed last time" : "taw didn't close cleanly last time");

    const juce::String dot (L" \x00b7 ");

    if (reopened && session != nullptr)
        text << dot << "reopened " << session->name();

    if (crashLog.existsAsFile())
        text << dot << "report: " << crashLog.getFullPathName();

    if (message.isNotEmpty())
        text << dot << message;

    message = text;
    messageIsError = true;
}

void App::say (const Result& r)
{
    if (r.message.isEmpty() && r.ok)
        return;

    message = r.message;
    messageIsError = ! r.ok;
}
} // namespace taw
