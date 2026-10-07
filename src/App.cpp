#include "App.h"
#include "Safety.h"
#include "Timing.h"

namespace taw
{
static juce::File deviceStateFile() { return configFolder().getChildFile ("audio-device.xml"); }
static juce::File settingsFile() { return configFolder().getChildFile ("settings.json"); }

static const double sessionRates[] = { 44100.0, 48000.0, 88200.0, 96000.0 };

double newSessionRate()
{
    const auto rate = (double) juce::JSON::parse (settingsFile()).getProperty ("newRate", 44100.0);
    return std::find (std::begin (sessionRates), std::end (sessionRates), rate) != std::end (sessionRates) ? rate : 44100.0;
}

void App::showWaiting (const juce::String& title, const juce::String& what, const juce::String& why)
{
    loadingTitle = title;
    loadingPlugin = what;
    loadingWhy = why;

    if (console.isOpen())
        ui.render (*this);
}

static juce::String kHz (double rate) { return juce::String (rate / 1000.0, rate == std::floor (rate / 1000.0) * 1000.0 ? 0 : 1) + " kHz"; }

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
    timing::mark ("taw starting");
    loadSettings();
    timing::mark ("settings read");

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

        timing::Scope t ("session read: " + openDir.getFileName());

        if (auto s = Session::load (openDir, error))
            attachSession (std::move (s));
        else
            say (Result::fail (error));
    }
}

App::~App()
{
    if (indexThread.joinable())
        indexThread.join(); // an index half written would be measured again anyway

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
        pluginQueue.clear();
        savePluginStates();
        midiEditor.reset();
        audioEditor.reset();

        for (auto& in : hosted)
            if (in.slot != nullptr)
                in.slot->closeEditor();

        hosted.clear();
        heldNotes.clear();
        midiRecordTracks.clear();
        midiPassClips.clear();
        fillView = pluginPickerVisible = false;

        // The video window's place, for next time (not an edit).
        if (video != nullptr)
        {
            session->tree().setProperty (ids::videoWindow, video->windowState(), nullptr);
            video.reset();
        }

        videoFailed.clear();
        videoClosed = false;
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

    // On screen: in steps, with the closing box (plugins can take a while to save and unload).
    if (console.isOpen())
    {
        startClosing (false);
        return Result::good();
    }

    return finishClosing();
}

// Closing (close, or quitting): recording stopped and the engine silent at once, then each
// plugin's state saved and the plugin let go of, a few per frame, under the closing box.
void App::startClosing (bool thenQuit)
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

    engine.setGraph (std::make_unique<Graph>()); // silence: nothing of the session plays on
    closing = std::make_unique<Closing>();
    closing->thenQuit = thenQuit;
    closing->total = hosted.size() + 1; // and the session file
    helpVisible = settingsVisible = pluginPickerVisible = busPickerVisible = trackMenuVisible = false;
    ioCell = -1;
}

void App::stepClosing()
{
    const auto until = juce::Time::getMillisecondCounterHiRes() + 40.0;

    while (! hosted.empty() && juce::Time::getMillisecondCounterHiRes() < until)
    {
        auto& in = hosted.back();
        closing->current = in.node[ids::pluginName].toString();
        writePluginState (in);

        if (in.slot != nullptr)
            in.slot->closeEditor();

        hosted.pop_back(); // the plugin goes once the silent graph has replaced the old one
        ++closing->done;
    }

    if (! hosted.empty())
        return;

    const bool thenQuit = closing->thenQuit;
    closing.reset();

    if (thenQuit)
    {
        releaseSession();
        quitRequested = true;
        return;
    }

    say (finishClosing());
}

Result App::finishClosing()
{
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

    timing::mark ("console open");
    interactive = true;
    startupDevice();

    // MIDI controllers, all of them, for armed instrument tracks. Only the interactive app opens
    // them (Windows MIDI inputs can't be shared).
    {
        timing::Scope t ("MIDI inputs opened");
        midiInputs = std::make_unique<MidiInputs> (engine.midiInputQueues(), midiOff);
    }

    auto last = juce::Time::getMillisecondCounterHiRes();
    bool firstFrame = true;
    bool openingShown = false; // the first frame of the session once an open finishes

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
        else if (mode == Mode::Main && ! liveKeys.on() && (dropPending || chars >= 4)) // a chord in keys mode isn't a drop
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

        // Keys the MIDI editor passed on (space, ctrl+z, ...).
        for (const auto& k : std::exchange (deferredKeys, {}))
            handleKey (k);

        // Keys mode: notes end when their keys come up; it ends with the session.
        if (liveKeys.on() && session == nullptr)
            liveKeys.setOn (false);

        liveKeys.poll (juce::Time::getMillisecondCounterHiRes());

        collectFinishedTakes();
        drainLiveOverviews();
        drainRecordedMidi();
        pollControls(); // controllers mapped to automation
        { timing::Scope t ("slow: plugin messages", 150.0); pollPlugins(); }
        pollBounce();
        pollImport();
        { timing::Scope t ("slow: library", 150.0); pollIndexing(); }

        if (opening != nullptr)
        {
            stepOpening();
            openingShown = opening == nullptr;
        }

        if (opening == nullptr && closing == nullptr && ! firstFrame) // the screen first, then plugins
            loadNextPlugin();

        if (closing != nullptr)
        {
            stepClosing();

            if (closing == nullptr && quitRequested)
                break;
        }

        if (treeDirty && session != nullptr && opening == nullptr && closing == nullptr)
        {
            treeDirty = false;

            // Undo can remove tracks out from under the focus or a selection.
            focused = juce::jlimit (0, session->numTracks(), focused);
            if (trackAnchor >= session->numTracks())
                trackAnchor = -1;

            timing::Scope t ("slow: rebuild and save", 150.0);
            rebuildGraph();
            session->save();
        }

        syncVideo();

        if (engine.isRolling())
        {
            if (juce::Time::getMillisecondCounterHiRes() - cursorMovedAt > 3000.0)
                keepInView (heardPlayhead());
        }

        const auto now = juce::Time::getMillisecondCounterHiRes();
        updateMeters ((now - last) / 1000.0);
        last = now;

        {
            timing::Scope t ("slow: drawing", 150.0);
            ui.render (*this);
        }

        if (std::exchange (firstFrame, false))
            timing::mark ("first screen drawn");

        if (std::exchange (openingShown, false))
            timing::mark ("session on screen");

        // Pumps JUCE's message queue (device notifications, driver panels) between frames.
        {
            const auto before = juce::Time::getMillisecondCounterHiRes();
            timing::Scope t ("slow: device messages", 150.0);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (15);
            const auto after = juce::Time::getMillisecondCounterHiRes();

            // The driver's restart after a rate change came and went (or never came): done.
            if (deviceSettleUntil > 0.0 && (after - before > 1000.0 || after > deviceSettleUntil))
            {
                deviceSettleUntil = 0.0;
                doneWaiting();
            }
        }
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

    timing::mark ("audio device opening");

    // Drivers can take seconds to open (more if the hardware has to change rate): say so first.
    if (saved != nullptr)
        showWaiting ("OPENING", saved->getStringAttribute ("audioOutputDeviceName", "the audio device"),
                     L"the audio device \x00b7 some drivers take a few seconds");

    const auto error = dm.initialise (AudioEngine::maxInputs, 2, saved.get(), false);
    doneWaiting();
    timing::mark ("audio device open: " + (dm.getCurrentAudioDevice() != nullptr ? dm.getCurrentAudioDevice()->getName() : juce::String ("none")));

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
        { timing::Scope t ("sample rate matched"); matchDeviceSampleRate(); }
        { timing::Scope t ("playback graph built"); rebuildGraph(); }
        return;
    }

    timing::Scope t ("session list read");
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

juce::String App::datedName (const juce::String& name)
{
    const auto last = name.fromLastOccurrenceOf (" ", false, false);
    const bool dated = last.length() == 8 && last[2] == '.' && last[5] == '.'
                       && last.removeCharacters (".").containsOnly ("0123456789");

    return dated ? name : name + juce::Time::getCurrentTime().formatted (" %m.%d.%y");
}

Result App::createSession (const Template& t)
{
    const auto folderFor = [this] (const juce::String& name) { return newSessionParent.getChildFile (juce::File::createLegalFileName (name.trim())); };

    if (newSessionName.trim().isEmpty())
        newSessionName = dateSessionNames ? juce::String ("session") : "session-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M");

    // A name you typed that's already a session opens it; a new one gets today's date (setting).
    if (dateSessionNames && ! Session::existsIn (folderFor (newSessionName)))
    {
        const auto base = datedName (newSessionName.trim());
        newSessionName = base;

        for (int n = 2; folderFor (newSessionName).exists(); ++n) // a second "session" today: never open or reuse it
            newSessionName = base + " " + juce::String (n);
    }

    const auto dir = folderFor (newSessionName);
    juce::String error;
    std::unique_ptr<Session> s;

    if (Session::existsIn (dir))
        s = Session::load (dir, error);
    else
        s = Session::create (dir, t.tree, newRate, error); // the device follows when it opens

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

    // Changing rate can take the driver several seconds (and the interface's MIDI ports wait
    // with it): a box says what's going on, and stays up while the driver settles.
    const auto from = device->getCurrentSampleRate();
    showWaiting ("SWITCHING", device->getName() + " to " + kHz (session->sampleRate()),
                 "this session is " + kHz (session->sampleRate()) + ", the device was at " + kHz (from)
                     + L" \x00b7 the driver takes a few seconds");

    auto setup = engine.devices().getAudioDeviceSetup();
    setup.sampleRate = session->sampleRate();
    engine.devices().setAudioDeviceSetup (setup, true);
    deviceSettleUntil = juce::Time::getMillisecondCounterHiRes() + 4000.0;

    // Remembered at the new rate: the next start opens the hardware as it is, without a change.
    if (auto xml = engine.devices().createStateXml())
        xml->writeTo (deviceStateFile());

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
    const auto from = loopActive() ? (juce::int64) session->tree()[ids::loopStart] : cursor;
    playFrom = from;
    engine.play (from, countInSamples (from, false));
    return Result::good();
}

// The count-in before the transport moves: the session's clicks on the beats just before
// from, so they lead in on the grid even from mid-beat. Recording counts in unless it's off;
// plain playback only with "count always".
juce::int64 App::countInSamples (juce::int64 from, bool forRecording) const
{
    const auto& s = session->tree();
    const int clicks = juce::jlimit (0, 16, (int) s[ids::countIn]);

    if (clicks == 0 || (! forRecording && s[ids::countInWhen].toString() != "always"))
        return 0;

    const auto spb = session->samplesPerBeat();
    const auto firstBeat = std::ceil ((double) from / spb - 1e-9) - clicks;
    return from - (juce::int64) std::llround (firstBeat * spb);
}

juce::String App::countInStatus() const
{
    const auto& s = session->tree();
    const int clicks = s[ids::countIn];
    return clicks <= 0 ? juce::String ("count-in off")
                       : "count-in " + juce::String (clicks) + (clicks == 1 ? " click" : " clicks")
                             + (s[ids::countInWhen].toString() == "always" ? " (recording and playback)" : " (recording)");
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

    releaseTouches(); // what a controller wrote this pass goes into its lanes
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

    // Ctrl+space again while recording punches out (and keeps playing).
    if (recording)
        return engine.isRolling() ? punch() : Result::fail ("already recording");

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

    const auto countIn = countInSamples (cursor, true);

    if (! engine.isRolling())
    {
        playStart = playFrom = cursor;
        engine.play (cursor, countIn);
    }

    juce::StringArray names;

    for (auto i : armed)
        names.add (juce::String (i + 1));

    return Result::good ((countIn > 0 ? "counting in, then recording " : "recording ") + names.joinIntoString (","));
}

Result App::punch()
{
    if (recording)
    {
        stopRecorders();
        return Result::good (L"punched out \x00b7 still playing \x00b7 ctrl+space punches in again");
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
        const int last = input + (trackIsStereo (session->track (i)) ? 1 : 0); // a stereo track records a pair

        if (input < 1 || last > numInputs)
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
        auto rec = engine.createRecorder (i, (int) t[ids::input] - 1, trackIsStereo (t) ? 2 : 1, file, sampleRate(), error);

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
    midiPassClips.clear();

    for (auto i : armedTracks)
        if (Session::isInstrumentTrack (session->track (i)))
            midiRecordTracks.push_back (i);

    // A punch starts where you are hearing; from a stop, at the cursor (after any count-in).
    midiPassStart = juce::jmax (0.0, (double) (engine.isRolling() ? heardPosition (engine.playhead()) : cursor) / session->samplesPerBeat());

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
        for (WavePoint v; l.recorder->overview.pop (v);)
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
    // What was captured at playhead P was played against audio heard (in + out latency) earlier,
    // and the mix you heard was the timeline's mixLatency before P (delay compensation always
    // counts: it's what the plugins did, not the interface).
    if (session != nullptr && (bool) session->tree()[ids::latencyComp])
        if (auto* d = engine.devices().getCurrentAudioDevice())
            return d->getInputLatencyInSamples() + d->getOutputLatencyInSamples() + mixLatency;

    return mixLatency;
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
    while (loadNextPlugin()) {}

    collectFinishedTakes();
    pollPlugins();
    pollBounce();
    pollImport();
    pollIndexing();

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

    if (Session::isInstrumentTrack (t))
    {
        const auto midi = Session::lastMidiClip (t);

        if (! midi.isValid())
            return Result::fail ("track " + juce::String (trackIndex + 1) + " has no clips");

        t.removeChild (midi, um());
        return Result::good ("dropped the latest clip on track " + juce::String (trackIndex + 1) + L" \x00b7 ctrl+z brings it back");
    }

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
        WavePoint p;

        for (int ch = 0; ch < channels; ++ch)
        {
            const auto r = buffer->findMinMax (ch, i, n);
            p.lo = juce::jmin (p.lo, r.getStart());
            p.hi = juce::jmax (p.hi, r.getEnd());
        }

        data.overview.push_back (p);
    }

    data.audio = std::move (buffer);
    return &clips.emplace (path, std::move (data)).first->second;
}

void App::rebuildGraph()
{
    if (session == nullptr)
        return;

    engine.setGraph (buildGraph());
    syncControlMap();
}

Curve App::automationCurve (const juce::ValueTree& owner, const juce::String& param) const
{
    Curve c;
    const auto lane = Session::automation (owner, param);

    if (! lane.isValid())
        return c;

    const auto target = AutomationTarget::parse (param);
    const auto spb = session->samplesPerBeat();
    c.steps = target.kind == AutomationTarget::Kind::mute;

    // A mapped controller can take it over while it moves; with no points, it otherwise plays
    // the mixer's setting.
    if (lane.hasProperty (ids::num))
    {
        c.live = liveValueFor (lane);

        switch (target.kind)
        {
            case AutomationTarget::Kind::volume: c.base = (float) (double) owner[owner.hasType (ids::SESSION) ? ids::mixDb : ids::gainDb]; break;
            case AutomationTarget::Kind::pan:    c.base = (float) (double) owner[ids::pan]; break;
            case AutomationTarget::Kind::mute:   c.base = (bool) owner[ids::mute] ? 1.0f : 0.0f; break;
            case AutomationTarget::Kind::send:
                if (const auto send = Session::send (owner, target.slot); send.isValid())
                    c.base = (float) (double) send[ids::db];
                break;
            default: break;
        }
    }

    for (const auto& p : Session::points (lane))
    {
        c.pos.push_back (std::llround ((double) p[ids::beat] * spb));
        c.val.push_back (target.clamp ((float) (double) p[ids::val]));
    }

    return c;
}

void App::addParameterCurves (TrackPlay& tp, const juce::ValueTree& track, const std::shared_ptr<PluginSlot>& plugin, const juce::String& prefix) const
{
    if (plugin == nullptr)
        return;

    for (const auto& lane : Session::automations (track))
    {
        const auto param = lane[ids::param].toString();

        if (! param.startsWith (prefix))
            continue;

        if (const auto index = plugin->findParameter (param.substring (prefix.length())); index >= 0)
            if (auto curve = automationCurve (track, param); ! curve.empty())
                tp.parameterCurves.push_back ({ plugin, index, std::move (curve) });
    }
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
        tp.soloAudible = ! anySolo || (bool) t[ids::solo];
        tp.audible = ! (bool) t[ids::mute] && tp.soloAudible;
        tp.gain = juce::Decibels::decibelsToGain ((float) (double) t[ids::gainDb], -96.0f);
        tp.volumeCurve = automationCurve (t, "vol");
        tp.panCurve = automationCurve (t, "pan");
        tp.muteCurve = automationCurve (t, "mute");

        if (Session::isInstrumentTrack (t))
        {
            // Armed means "listening to the MIDI controller", not to an audio input.
            tp.midiLive = tp.armed && ! offline;
            tp.armed = false;
            tp.stereo = true;
            tp.sequence = buildSequence (t, session->samplesPerBeat());

            // Live: a plugin not loaded yet is queued and the track is silent until it is.
            if (auto* in = offline ? hostedEntry (t) : liveEntry (t); in != nullptr && in->slot != nullptr)
            {
                if (offline)
                {
                    // A bounce plays its own copy, so the live one keeps playing meanwhile.
                    juce::String problem;
                    tp.plugin = createPlugin (t, sampleRate(), 8192, problem);

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

            addParameterCurves (tp, t, tp.plugin, "inst/");
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

                for (const auto& h : Session::placedHits (t))
                    tp.hits.push_back ({ std::llround (h.beat * session->samplesPerBeat()), h.vel });

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

    for (int i = 0; i < n; ++i)
        addInserts (g->tracks[(size_t) i], session->track (i), offline);

    // Width, pan and latency: once the inserts are known (a mono-to-stereo effect makes the
    // rest of the track stereo).
    for (int i = 0; i < n; ++i)
    {
        auto& tp = g->tracks[(size_t) i];
        const auto t = session->track (i);
        tp.stereo = trackIsStereo (t);
        tp.monoAtPan = ! tp.stereo;
        tp.latency = tp.plugin != nullptr ? tp.plugin->latency() : 0;

        for (const auto& fx : tp.inserts)
        {
            tp.monoAtPan = tp.monoAtPan && fx->numOutputs() < 2;
            tp.latency += fx->latency();
        }

        taw::setPan (tp, (float) (double) t[ids::pan]);
    }

    // The mix bus's inserts (their latency joins delay compensation's in applyRouting).
    addInserts (g->mixBus, session->tree(), offline);

    for (const auto& fx : g->mixBus.inserts)
        g->mixBus.latency += fx->latency();

    applyRouting (*g, offline);

    // Plugins that are gone (undo, a removed insert, a track no longer an instrument): state
    // saved, window closed. Undoing back brings the plugin back from that state.
    if (! offline)
        for (auto it = hosted.begin(); it != hosted.end();)
        {
            const auto owner = it->node.getParent(); // an insert's track, or the session (the mix's)
            const bool gone = it->node.hasType (ids::INSERT)
                                  ? ! owner.isValid() || (! owner.hasType (ids::SESSION) && ! owner.getParent().isValid())
                                  : ! it->node.getParent().isValid() || ! Session::isInstrumentTrack (it->node);

            if (gone)
            {
                writePluginState (*it);

                if (it->slot != nullptr)
                    it->slot->closeEditor();

                it = hosted.erase (it);
            }
            else
            {
                ++it;
            }
        }

    const auto& s = session->tree();
    g->mixGain = juce::Decibels::decibelsToGain ((float) (double) s[ids::mixDb], -96.0f);
    g->mixCurve = automationCurve (s, "vol");
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

    // MIDI files become instrument tracks straight away; the rest is audio for the importer.
    juce::StringArray audioPaths;
    Result midiResult;
    bool anyMidi = false;

    for (const auto& path : paths)
    {
        const auto f = juce::File::getCurrentWorkingDirectory().getChildFile (path.unquoted());

        if (f.hasFileExtension (".mid;.midi") && f.existsAsFile())
        {
            midiResult = importMidiFile (f, anyMidi ? -1 : targetTrack);
            anyMidi = true;
        }
        else
        {
            audioPaths.add (path);
        }
    }

    if (anyMidi && audioPaths.isEmpty())
        return midiResult;

    juce::StringArray notFound;
    auto files = Import::expand (audioPaths, juce::File::getCurrentWorkingDirectory(), notFound);

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

    juce::StringArray placed, failed, replaced;
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
            // keeps its hits, so a pattern can swap its sound. That says so, and the track's
            // header flashes; a track still named after its old sample takes the new one's name.
            if (Session::isSampleTrack (track) && track[ids::sample].toString().isNotEmpty())
            {
                const auto oldName = juce::File (track[ids::sample].toString()).getFileNameWithoutExtension();
                const auto newName = item.source.getFileNameWithoutExtension();
                // An imported sample's copy can carry a number after the original's name.
                const bool namedAfterIt = oldName.startsWithIgnoreCase (track[ids::name].toString()) || track[ids::name].toString().startsWithIgnoreCase (oldName);

                if (namedAfterIt)
                    track.setProperty (ids::name, newName.substring (0, 24), um());

                replaced.add ("track " + juce::String (session->tree().indexOf (track) + 1) + " " + track[ids::name].toString() + " now plays "
                              + newName + " (was " + juce::File (track[ids::sample].toString()).getFileName() + ")");
                flashTrack (track);
            }

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
    else if (! replaced.isEmpty() && replaced.size() == placed.size())
        say (Result::good (replaced.joinIntoString (juce::String (L" \x00b7 ")) + L" \x00b7 ctrl+z undoes"));
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

    // "index" (or "index remove") typed, then a folder dropped: indexed (or taken out) at once,
    // as a drop alone imports. "video", then a file: that's the picture.
    if (const auto line = commandLine.trim();
        line.equalsIgnoreCase ("index") || line.equalsIgnoreCase ("index remove") || line.equalsIgnoreCase ("video"))
    {
        commandLine.clear();
        say (execute (line + " " + text.trim()));
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

Result App::startBounce (const juce::StringArray& args, bool withVideo)
{
    if (bounce != nullptr)
        return Result::fail (L"already bouncing \x00b7 esc cancels");

    if (recording)
        return Result::fail ("stop recording first");

    auto s = session->tree();
    Bounce::Picture picture;
    juce::Range<juce::int64> range;

    if (withVideo)
    {
        // bv: the selection, else the whole video (from the session start, if it starts before).
        picture.file = videoFile();
        VideoDecoder probe;
        juce::String error;

        if (picture.file == juce::File())
            return Result::fail (L"no video \x00b7 video <file> adds one, then bv");

        if (! probe.open (picture.file, error))
            return Result::fail (error);

        const auto start = (juce::int64) s[ids::videoStart];
        range = hasTimeSelection() ? timeSelection()
                                   : juce::Range<juce::int64> (juce::jmax<juce::int64> (0, start), start + std::llround (probe.info().duration * sampleRate()));

        if (range.isEmpty())
            return Result::fail ("the video ends before the session starts");

        picture.from = (double) (range.getStart() - start) / sampleRate();
    }
    else if (! hasTimeSelection())
    {
        return Result::fail (L"select a time range first (shift + \x2190 \x2192), then b");
    }
    else
    {
        range = timeSelection();
    }

    juce::StringArray nameParts;

    // b | b mp3 | b wav D:\mixes | b mp3 "rough mix 3"  (format and folder are remembered)
    for (const auto& a : args)
    {
        if (! withVideo && (a.equalsIgnoreCase ("wav") || a.equalsIgnoreCase ("mp3")))
            s.setProperty (ids::bounceFormat, a.toLowerCase(), nullptr);
        else if (a.containsAnyOf ("\\/:"))
            s.setProperty (ids::bounceFolder, expandHome (a.unquoted()), nullptr);
        else
            nameParts.add (a.unquoted());
    }

    const auto format = withVideo ? Bounce::Format::video
                        : s[ids::bounceFormat].toString() == "mp3" ? Bounce::Format::mp3
                                                                   : Bounce::Format::wav;
    const auto folder = session->folder().getChildFile (s[ids::bounceFolder].toString());
    const auto name = nameParts.isEmpty() ? session->name() + "-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S")
                                          : nameParts.joinIntoString (" ");
    auto file = folder.getChildFile (juce::File::createLegalFileName (name) + Bounce::extension (format));

    if (file.exists())
        file = file.getNonexistentSibling (false);

    bounce = std::make_unique<Bounce> (buildGraph (true), range, sampleRate(), format, file, picture);
    bounce->start();
    return Result::good ("bouncing " + juce::String (withVideo ? "with picture " : "") + formatBars (range.getStart()) + L" \x2192 "
                         + formatBars (range.getEnd()));
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

        lastBounce = bounce->file();

        if (interactive && session != nullptr && (bool) session->tree()[ids::bounceReveal])
            lastBounce.revealToUser();
    }

    bounce.reset();
}

Result App::revealBounces (const juce::String& arg)
{
    if (session == nullptr)
        return Result::fail ("no session open");

    auto s = session->tree();

    if (arg == "on" || arg == "off")
    {
        beginEdit ("bounce reveal");
        s.setProperty (ids::bounceReveal, arg == "on", um());
        return Result::good (arg == "on" ? L"each bounce opens in the file explorer \x00b7 bounces off stops it"
                                         : L"bounces stay put \x00b7 bounces opens the folder");
    }

    if (arg.isNotEmpty())
        return Result::fail ("bounces [on|off]");

    const auto folder = session->folder().getChildFile (s[ids::bounceFolder].toString());

    // The latest bounce, picked out in its folder; else just the folder.
    if (lastBounce.existsAsFile() && lastBounce.getParentDirectory() == folder)
        lastBounce.revealToUser();
    else if (folder.isDirectory())
        folder.startAsProcess();
    else
        return Result::fail ("nothing bounced yet \x00b7 select a range, then b");

    return Result::good ("opened " + folder.getFullPathName());
}

//==============================================================================
// Video: the picture, in its own window, following the playhead

juce::File App::videoFile() const
{
    if (session == nullptr)
        return {};

    const auto path = session->tree()[ids::videoFile].toString();
    return path.isEmpty() ? juce::File() : session->folder().getChildFile (path);
}

// Runs on the video's decoder thread: reads only the engine's atomics and videoClock. Rolling,
// it's what you hear (the playhead less the mix's delay compensation and the device's output
// latency); stopped, the cursor.
double App::videoSeconds()
{
    const auto& c = videoClock;
    const auto pos = engine.isRolling() ? juce::jmax (c.playFrom.load(), engine.playhead() - c.lateBy.load())
                                        : engine.targetPosition();
    return (double) (pos - c.start.load()) / c.rate.load() - c.offset.load();
}

void App::syncVideo()
{
    const auto want = opening == nullptr && closing == nullptr ? videoFile() : juce::File();

    if (want == juce::File())
    {
        video.reset();
        videoFailed.clear();

        if (topLane == TopLane::video) // its row went with it
            topLane = TopLane::ruler;

        return;
    }

    auto s = session->tree();

    if (video == nullptr || video->file() != want)
    {
        if (videoFailed == want.getFullPathName())
            return;

        auto player = std::make_unique<VideoPlayer> ([this] { return videoSeconds(); });
        juce::String error;

        if (! player->open (want, error))
        {
            videoFailed = want.getFullPathName();
            video.reset();
            say (Result::fail (error));
            return;
        }

        player->restoreWindowState (s[ids::videoWindow].toString());
        video = std::move (player);
        videoFailed.clear();
    }

    auto& c = videoClock;
    const auto fps = juce::jmax (1.0, video->info().fps);
    int outputLatency = 0;

    if (auto* d = engine.devices().getCurrentAudioDevice())
        outputLatency = d->getOutputLatencyInSamples();

    c.rate = sampleRate();
    c.start = (juce::int64) s[ids::videoStart];
    c.offset = (int) s[ids::videoOffset] / fps;
    c.lateBy = mixLatency + outputLatency;
    c.playFrom = playFrom;

    // Its close button hides it until "video" or the next open. Not saved: Windows closes every
    // window this way on shutdown (and taskkill), which mustn't turn the picture off for good.
    if (video->takeCloseRequest())
        videoClosed = true;

    if (const bool shown = (bool) s[ids::videoShown] && ! videoClosed; video->isVisible() != shown)
        video->setVisible (shown);

    // It's only a view: the keyboard stays with taw.
    if (video->hasKeyboard())
        console.focusTerminal();
}

juce::int64 App::videoStartPos() const
{
    return session != nullptr ? (juce::int64) session->tree()[ids::videoStart] : 0;
}

juce::int64 App::videoLength() const
{
    return video != nullptr ? (juce::int64) std::llround (video->info().duration * sampleRate()) : 0;
}

double App::samplesPerFrame() const
{
    return sampleRate() / (video != nullptr && video->info().fps > 0.0 ? video->info().fps : 30.0);
}

// Frame k starts at start + k frames; its first sample is the one at or after that (the decoder
// shows a frame from its timestamp on, so that sample shows it).
juce::int64 App::frameStart (juce::int64 pos) const
{
    const auto spf = samplesPerFrame();
    const auto start = videoStartPos();
    auto k = std::floor ((double) (pos - start) / spf + 1.0e-9);

    // pos can sit before its frame's first sample when frames don't fall on whole samples.
    if (start + (juce::int64) std::ceil (k * spf - 1.0e-6) > pos)
        k -= 1.0;

    return start + (juce::int64) std::ceil (k * spf - 1.0e-6);
}

juce::String App::videoTimecode (juce::int64 pos) const
{
    if (video == nullptr)
        return {};

    const auto fps = video->info().fps;
    const auto nominal = juce::jmax (1, juce::roundToInt (fps)); // non-drop: 29.97 counts in 30s
    const auto frames = (juce::int64) std::floor ((double) (frameStart (pos) - videoStartPos()) / samplesPerFrame() + 0.5);
    const auto a = std::abs (frames);
    const auto secs = a / nominal;
    auto tc = juce::String (secs / 60) + ":" + juce::String (secs % 60).paddedLeft ('0', 2) + ":" + juce::String (a % nominal).paddedLeft ('0', 2);

    if (secs >= 3600)
        tc = juce::String (secs / 3600) + ":" + juce::String ((secs / 60) % 60).paddedLeft ('0', 2) + ":" + juce::String (secs % 60).paddedLeft ('0', 2) + ":"
             + juce::String (a % nominal).paddedLeft ('0', 2);

    return frames < 0 ? "-" + tc : tc;
}

// The video row: a frame at a time (a second with ctrl), from the cursor or the selection's
// moving end. Left from inside a frame goes to its start first.
Result App::stepFrame (int direction, bool second, bool extendSelection)
{
    if (video == nullptr)
        return Result::fail ("no video");

    const auto spf = samplesPerFrame();
    const auto start = videoStartPos();
    const auto from = extendSelection && timeAnchor >= 0 ? timeHead : cursor;
    const auto here = frameStart (from);
    const auto k = std::llround ((double) (here - start) / spf);
    const auto step = second ? (juce::int64) juce::jmax (1, juce::roundToInt (video->info().fps)) : (juce::int64) 1;
    const auto target = direction > 0 ? k + step : (from > here ? k - (step - 1) : k - step);
    const auto pos = start + (juce::int64) std::ceil ((double) target * spf - 1.0e-6);

    moveTo (juce::jmax<juce::int64> (0, frameStart (juce::jmax<juce::int64> (0, pos))), extendSelection); // not before the session
    return Result::good();
}

juce::String App::videoStartText() const
{
    const auto start = (juce::int64) session->tree()[ids::videoStart];

    if (start == 0)
        return "the session start";

    if (start < 0)
        return "-" + formatTime (-start) + " (before the session start)";

    return formatTime (start) + " (bar " + formatBars (start) + ")";
}

// video <file> | video (show/hide) | video at <position> | video offset <frames> | video remove
Result App::videoCommand (const juce::StringArray& args, const juce::String& rest)
{
    auto s = session->tree();
    const auto first = args[0].toLowerCase();
    const bool have = videoFile() != juce::File();

    if (rest.isEmpty())
    {
        if (! have)
            return Result::fail (L"no video \x00b7 video <file> adds one (mp4)");

        const bool show = video != nullptr ? ! video->isVisible() : ! (bool) s[ids::videoShown];
        s.setProperty (ids::videoShown, show, nullptr); // a window, not an edit
        videoClosed = false;
        return Result::good (show ? juce::String ("video shown") : juce::String (L"video hidden \x00b7 video shows it"));
    }

    if (first == "remove" || first == "rm")
    {
        if (! have)
            return Result::fail ("no video");

        const auto name = videoFile().getFileName();
        s.removeProperty (ids::videoFile, um());
        return Result::good (L"removed the video " + name + L" \x00b7 undo puts it back");
    }

    if (first == "at")
    {
        if (! have)
            return Result::fail (L"no video \x00b7 video <file> adds one (mp4)");

        // video at 5 | video at 1:30 | video at -0:05 (before the session: skips a slate)
        auto spec = rest.fromFirstOccurrenceOf (args[0], false, true).trim();
        const bool before = spec.startsWithChar ('-');
        juce::int64 pos = 0;

        if (before)
            spec = spec.substring (1).trim();

        if (auto r = parsePosition (spec, pos); r.failed())
            return Result::fail (L"video at 5 \x00b7 video at 1:30 \x00b7 video at -0:05");

        s.setProperty (ids::videoStart, before ? -pos : pos, um());
        return Result::good ("the video starts at " + videoStartText());
    }

    if (first == "offset")
    {
        if (args[1].isEmpty() || ! args[1].trimCharactersAtStart ("+-").containsOnly ("0123456789"))
            return Result::fail (L"video offset 2 shows the picture 2 frames late \x00b7 -2 early");

        const auto frames = juce::jlimit (-100, 100, args[1].getIntValue());
        s.setProperty (ids::videoOffset, frames, um());
        return Result::good (frames == 0 ? juce::String ("the picture is on time")
                                         : "the picture is shown " + juce::String (std::abs (frames)) + (std::abs (frames) == 1 ? " frame " : " frames ")
                                               + (frames > 0 ? "late" : "early"));
    }

    // A file: it replaces any video, starting at the session start.
    const auto file = Import::resolve (rest, session->folder());
    VideoDecoder probe;
    juce::String error;

    if (! probe.open (file, error))
        return Result::fail (error);

    const auto& info = probe.info();
    const auto path = file.isAChildOf (session->folder()) ? file.getRelativePathFrom (session->folder()) : file.getFullPathName();
    s.setProperty (ids::videoFile, path, um());
    s.setProperty (ids::videoStart, (juce::int64) 0, um());
    s.setProperty (ids::videoShown, true, nullptr);
    videoFailed.clear();

    const auto mins = (int) (info.duration / 60.0);
    const auto secs = juce::roundToInt (info.duration - mins * 60.0);
    return Result::good ("video " + file.getFileName() + L" \x00b7 " + juce::String (info.width) + "x" + juce::String (info.height) + " "
                         + juce::String (info.fps, info.fps == std::round (info.fps) ? 0 : 3) + " fps " + juce::String (mins) + ":"
                         + juce::String (secs).paddedLeft ('0', 2) + L" \x00b7 starts at the session start (video at <bar> moves it)");
}

//==============================================================================
// Navigation

double App::sampleRate() const
{
    if (session != nullptr)
        return session->sampleRate();

    if (auto* d = const_cast<App*> (this)->engine.devices().getCurrentAudioDevice())
        return d->getCurrentSampleRate();

    return 44100.0;
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
    const auto span = (juce::int64) (samplesPerColumn() * laneColumns());

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
        focusedLane = -1;
        return;
    }

    trackAnchor = -1;
    topLane = TopLane::none;
    selectedMarker = {};
    focused = juce::jlimit (0, n, index); // n = the mix row
    focusedLane = -1;
}

bool App::mixFocused() const
{
    return session != nullptr && ! onTopLane() && focused >= session->numTracks();
}

// Up from the first track: the video row (when there's a video), the bar row, then the markers
// row. Down comes back the same way. Shift only ever selects tracks, so it stops at the first one.
void App::focusUp (bool extendSelection)
{
    // Up through a track's lanes to its own row; from a track, onto the last lane of the one above.
    if (topLane == TopLane::none && focusedLane >= 0 && ! extendSelection)
    {
        --focusedLane;
        return;
    }

    if (topLane == TopLane::none && focused > 0 && ! extendSelection && ! lanesShown (focused - 1).empty())
    {
        const auto lanes = (int) lanesShown (focused - 1).size();
        focusTrack (focused - 1);
        focusedLane = lanes - 1;
        return;
    }

    if (topLane == TopLane::ruler)
        topLane = TopLane::markers;
    else if (topLane == TopLane::video)
        topLane = TopLane::ruler;
    else if (topLane == TopLane::none && focused == 0 && ! extendSelection)
        topLane = videoLane() ? TopLane::video : TopLane::ruler, trackAnchor = -1;
    else if (topLane == TopLane::none)
        focusTrack (focused - 1, extendSelection);
}

void App::focusDown (bool extendSelection)
{
    // Down from a track's row through its open lanes, then on to the next track (the Mix row:
    // through the mix's lanes under it).
    if (topLane == TopLane::none && ! extendSelection && session != nullptr && focused <= session->numTracks()
        && focusedLane + 1 < (int) lanesShown (ioTrack()).size())
    {
        ++focusedLane;
        trackAnchor = -1;
        return;
    }

    if (topLane == TopLane::markers)
        topLane = TopLane::ruler, selectedMarker = {};
    else if (topLane == TopLane::ruler && videoLane())
        topLane = TopLane::video;
    else if (topLane == TopLane::ruler || topLane == TopLane::video)
        topLane = TopLane::none, focused = 0;
    else
        focusTrack (focused + 1, extendSelection);
}

bool App::tappingTempo() const
{
    if (session == nullptr || ! commandLine.startsWithIgnoreCase ("bpm "))
        return false;

    const auto rest = commandLine.substring (4);
    return rest.isEmpty() || rest.containsOnly ("0123456789.");
}

// Each t is a beat. The tempo is the average gap over the last eight taps; a pause of two seconds
// starts over. It's written into the line ("bpm 121"), so enter sets it, as if typed.
void App::tapTempo()
{
    const auto now = juce::Time::getMillisecondCounterHiRes();

    // A pause, or a line the last tap didn't write (typed over, or a new bpm): start over.
    if (! tapTimes.empty() && (now - tapTimes.back() > 2000.0 || commandLine != tappedLine))
        tapTimes.clear();

    tapTimes.push_back (now);

    if (tapTimes.size() > 8)
        tapTimes.erase (tapTimes.begin());

    if (tapTimes.size() < 2)
    {
        commandLine = tappedLine = "bpm ";
        say (Result::good (L"tap \x00b7 keep tapping t on the beat"));
        return;
    }

    const auto gap = (tapTimes.back() - tapTimes.front()) / (double) (tapTimes.size() - 1);
    const auto bpm = juce::jlimit (20.0, 400.0, std::round (60000.0 / gap));
    commandLine = tappedLine = "bpm " + juce::String ((int) bpm);
    say (Result::good ("tap " + juce::String ((int) tapTimes.size()) + L" \x00b7 " + juce::String ((int) bpm)
                       + L" bpm \x00b7 enter sets it \x00b7 esc leaves it"));
}

bool App::playPads (float velocity, bool repeat)
{
    if (session == nullptr)
        return false;

    std::vector<int> armed;

    for (int i = 0; i < session->numTracks(); ++i)
        if (Session::isSampleTrack (session->track (i)) && (bool) session->track (i)[ids::armed])
            armed.push_back (i);

    if (armed.empty())
        return false;

    if (repeat)
        return true;

    // While recording, the hit goes where you heard the music when you pressed: the playhead
    // is what's being sent out now, which reaches your ears an output latency later.
    const bool rec = recording && engine.isRolling() && engine.countInRemaining() == 0; // not while counting in
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
            addHit (t, beat, velocity);
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
        return Result::fail (L"snap, snap 50 (percent) or snap input \x2014 the step is the grid's: grid 16");

    const auto step = session->fineStepBeats();
    const auto spb = session->samplesPerBeat();
    const bool ranged = hasTimeSelection();
    const auto range = timeSelection();
    int moved = 0;

    for (auto t : tracks)
    {
        auto track = session->track (t);

        // Note starts on instrument tracks (a note keeps its length), in the clips they play in;
        // one snapped back past its clip's start takes the clip with it.
        for (const auto& p : Session::placedNotes (track))
        {
            const auto pos = (juce::int64) std::llround (p.start * spb);

            if (ranged && ! range.contains (pos))
                continue;

            const auto target = std::round (p.start / step) * step;

            if (std::abs (target - p.start) > 1e-9)
            {
                const auto to = p.start + (target - p.start) * strength;
                Session::growMidiClip (p.clip, to, to, um());
                auto note = p.node;
                note.setProperty (ids::beat, (double) note[ids::beat] + (to - p.start), um());
                ++moved;
            }
        }

        // Hits on sample tracks.
        if (! Session::isSampleTrack (track))
            continue;

        // In their clips, as notes are: one snapped back past its clip's start takes the clip with it.
        for (const auto& h : Session::placedHits (track))
        {
            const auto pos = (juce::int64) std::llround (h.beat * spb);

            if (ranged && ! range.contains (pos))
                continue;

            const auto target = std::round (h.beat / step) * step;

            if (std::abs (target - h.beat) > 1e-9)
            {
                const auto to = h.beat + (target - h.beat) * strength;
                Session::growMidiClip (h.clip, to, to + 1.0 / 1024.0, um());
                auto node = h.node;
                node.setProperty (ids::beat, (double) node[ids::beat] + (to - h.beat), um());
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
    keepInView (engine.isRolling() ? heardPlayhead() : cursor);
}

void App::loadSettings()
{
    const auto json = juce::JSON::parse (settingsFile());
    if (const auto t = json["theme"].toString(); t == "light" || t == "grey")
        theme = t;

    if (json.hasProperty ("io"))
        ioVisible = (bool) json["io"];

    browserVisible = (bool) json.getProperty ("browser", false);
    browserAxisX = std::max (0, browserAxisNames().indexOf (json["browserX"].toString()));
    browserAxisY = std::max (0, browserAxisNames().indexOf (json["browserY"].toString()));

    if ((browserAxisX == 0) != (browserAxisY == 0) || (browserAxisX != 0 && browserAxisX == browserAxisY))
        browserAxisX = browserAxisY = 0; // a half-saved pair: the sound-alike map

    dateSessionNames = (bool) json.getProperty ("dateNames", true);
    newRate = newSessionRate();

    if (json.hasProperty ("auditionDb"))
        auditionDb = juce::jlimit (-60.0, 6.0, (double) json["auditionDb"]);

    if (const auto* off = json["midiOff"].getArray())
        for (const auto& name : *off)
            midiOff.add (name.toString());
}

void App::saveSettings() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("theme", theme);
    o->setProperty ("io", ioVisible);
    o->setProperty ("auditionDb", auditionDb);
    o->setProperty ("dateNames", dateSessionNames);
    o->setProperty ("newRate", newRate);
    o->setProperty ("browser", browserVisible);
    o->setProperty ("browserX", browserAxisNames()[browserAxisX]);
    o->setProperty ("browserY", browserAxisNames()[browserAxisY]);

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
    const auto columns = laneColumns();
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

    // On the video row: its start and end.
    if (topLane == TopLane::video && video != nullptr)
    {
        consider (juce::jmax<juce::int64> (0, videoStartPos()));
        consider (videoStartPos() + videoLength());

        if (next < 0)
            return Result::fail ("past the end of the video");

        moveTo (next, extendSelection);
        return Result::good();
    }

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
            for (const auto& h : Session::placedHits (track))
                consider (std::llround (h.beat * session->samplesPerBeat()));

        // MIDI and hit clip edges, and the notes in them.
        for (const auto& c : Session::beatClips (track))
        {
            consider (std::llround ((double) c[ids::beat] * session->samplesPerBeat()));
            consider (std::llround (Session::clipEnd (c) * session->samplesPerBeat()));
        }

        for (const auto& p : Session::placedNotes (track))
        {
            consider (std::llround (p.start * session->samplesPerBeat()));
            consider (std::llround (p.end * session->samplesPerBeat()));
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
    if (opening != nullptr || closing != nullptr)
        return; // keys wait until the session is open (or closed)

    if (k.code == Key::Mouse)
    {
        if (k.drag) // only scrubbing uses drags so far
        {
            if (mode == Mode::Main)
                handleMouse (k);

            return;
        }

        if (mode == Mode::Main && ! handleBrowserKey (k)) // the docked browser has its own clicks
            handleMouse (k);

        return;
    }

    switch (mode)
    {
        case Mode::Main:           handleMainKey (k); break;
        case Mode::DevicePicker:   handleDevicePickerKey (k); break;
        case Mode::TemplatePicker: handleTemplatePickerKey (k); break;
    }
}

// The mouse in the terminal. A click on a lane focuses its track and puts the cursor there (as
// in Pro Tools); shift-click stretches the time and track selections to it. A double-click on a
// clip selects it and opens it in its editor (MIDI or audio).
void App::handleMouse (const Key& k)
{
    const auto& map = ui.lanes();
    const int track = map.trackAt (k.y);

    // The video row: a click puts the cursor on that frame (the picture goes there); dragging
    // from it scrubs, wherever the mouse goes up or down.
    if (k.drag)
    {
        if (scrubbing && session != nullptr && map.valid)
        {
            const auto x = juce::jlimit (map.laneX, map.laneX + map.laneW - 1, k.x);
            moveTo (juce::jmax<juce::int64> (0, frameStart (juce::jmax<juce::int64> (0, map.positionAt (x)))), k.shift);
        }

        return;
    }

    scrubbing = session != nullptr && ! k.right && map.inLane (k.x) && k.y == map.videoY && videoLane();

    if (scrubbing)
    {
        topLane = TopLane::video;
        trackAnchor = -1;
        selectedMarker = {};
        moveTo (juce::jmax<juce::int64> (0, frameStart (juce::jmax<juce::int64> (0, map.positionAt (k.x)))), k.shift);
        return;
    }

    if (session == nullptr || k.right || ! map.inLane (k.x) || track < 0)
        return;

    // An automation lane: onto it, the cursor where it was clicked.
    if (const auto lane = map.laneAt (k.y); lane >= 0)
    {
        focusTrack (track);
        focusedLane = lane;
        moveTo (juce::jmax<juce::int64> (0, map.positionAt (k.x)), k.shift);
        return;
    }

    if (k.doubleClick)
    {
        const auto [t0, t1] = map.span (k.x);

        if (const auto clip = midiClipAt (track, t0, t1); clip.isValid())
        {
            const auto spb = session->samplesPerBeat();
            focusTrack (track);
            timeAnchor = std::llround ((double) clip[ids::beat] * spb);
            timeHead = std::llround (Session::clipEnd (clip) * spb);

            if (! engine.isRolling())
                setCursor (timeAnchor);

            // A hit clip has no editor: selected, for ctrl+c / ctrl+x / ctrl+d.
            say (clip.hasType (ids::HITCLIP) ? Result::good ("selected the clip " + formatBars (timeAnchor) + L" \x2192 " + formatBars (timeHead)
                                                             + L" \x00b7 ctrl+c copies, ctrl+x cuts, ctrl+d repeats it")
                                             : openMidiEditor (clip));
            return;
        }

        // An audio clip: the one on top there (later clips play over earlier ones).
        const auto audio = Session::clips (session->track (track));

        for (auto it = audio.rbegin(); it != audio.rend(); ++it)
        {
            const auto s = (double) (juce::int64) (*it)[ids::start], e = s + (double) (juce::int64) (*it)[ids::length];

            if (s < t1 && e > t0)
            {
                focusTrack (track);
                timeAnchor = (juce::int64) s;
                timeHead = (juce::int64) e;

                if (! engine.isRolling())
                    setCursor (timeAnchor);

                say (openAudioEditor (*it));
                break;
            }
        }

        return;
    }

    focusTrack (track, k.shift);
    moveTo (juce::jmax<juce::int64> (0, map.positionAt (k.x)), k.shift);
}

Result App::openMidiEditor (const juce::ValueTree& clip)
{
    if (! console.isOpen())
        return Result::fail ("the MIDI editor needs the screen");

    if (! clip.hasType (ids::MIDICLIP))
        return Result::fail ("that isn't a MIDI clip");

    if (midiEditor == nullptr)
        midiEditor = std::make_unique<MidiEditor> (*this);

    if (audioEditor != nullptr)
        audioEditor->close(); // one editor at a time

    midiEditor->open (clip);
    return Result::good (L"MIDI editor \x00b7 esc hands the keys back \x00b7 shift+E closes it");
}

Result App::openAudioEditor (const juce::ValueTree& clip)
{
    if (! console.isOpen())
        return Result::fail ("the audio editor needs the screen");

    if (! clip.hasType (ids::CLIP) || ! Session::isAudioTrack (clip.getParent()))
        return Result::fail ("that isn't an audio clip");

    if (audioEditor == nullptr)
        audioEditor = std::make_unique<AudioEditor> (*this);

    if (midiEditor != nullptr)
        midiEditor->close();

    audioEditor->open (clip);
    return Result::good (L"audio editor \x00b7 drag to select (taw's keys act on it) \x00b7 esc hands the keys back \x00b7 shift+E closes it");
}

void* App::editorWindow() const
{
    if (midiEditor != nullptr && midiEditor->isOpen())
        return midiEditor->nativeHandle();

    return audioEditor != nullptr ? audioEditor->nativeHandle() : nullptr;
}

// Shift+E / "edit": the editor for the focused track (open: closes it), on its clip at the
// cursor or else the nearest one. MIDI clips on instrument tracks, audio clips on audio tracks.
Result App::toggleEditor()
{
    if (midiEditor != nullptr && midiEditor->isOpen())
    {
        midiEditor->close();
        return Result::good ("MIDI editor closed");
    }

    if (audioEditor != nullptr && audioEditor->isOpen())
    {
        audioEditor->close();
        return Result::good ("audio editor closed");
    }

    if (session == nullptr || focused >= session->numTracks())
        return Result::fail (L"shift+E opens the focused track in an editor \x2014 focus a track (\x2191\x2193)");

    const auto track = session->track (focused);
    const auto label = "track " + juce::String (focused + 1);

    // The clip at the cursor, else the one nearest it (by its nearer edge).
    auto nearest = [this] (const std::vector<juce::ValueTree>& list, auto startOf, auto endOf)
    {
        juce::ValueTree best;
        double bestD = 0.0;

        for (auto it = list.rbegin(); it != list.rend(); ++it) // later ones play on top
        {
            const auto s = startOf (*it), e = endOf (*it), at = (double) cursor;
            const auto d = at < s ? s - at : at >= e ? at - e + 1.0 : -1.0;

            if (! best.isValid() || d < bestD)
                best = *it, bestD = d;
        }

        return best;
    };

    const auto spb = session->samplesPerBeat();

    if (Session::isInstrumentTrack (track))
    {
        const auto clip = nearest (Session::midiClips (track), [spb] (const juce::ValueTree& c) { return (double) c[ids::beat] * spb; },
                                   [spb] (const juce::ValueTree& c) { return Session::clipEnd (c) * spb; });

        return clip.isValid() ? openMidiEditor (clip)
                              : Result::fail (label + L" has no MIDI clips \x2014 record one (ctrl+space), or edit new for an empty one to draw into");
    }

    if (Session::isAudioTrack (track))
    {
        const auto clip = nearest (Session::clips (track), [] (const juce::ValueTree& c) { return (double) (juce::int64) c[ids::start]; },
                                   [] (const juce::ValueTree& c) { return (double) ((juce::int64) c[ids::start] + (juce::int64) c[ids::length]); });

        return clip.isValid() ? openAudioEditor (clip) : Result::fail (label + L" has no clips \x2014 record or import one");
    }

    return Result::fail (label + (Session::isSampleTrack (track) ? L" is a sample track \x2014 its hits are edited on the timeline (pat, alt+/; sel clip selects a clip)"
                                                                 : L" is a bus \x2014 it has no clips"));
}

// "edit new": an empty MIDI clip on the focused instrument track to draw into: over the time
// selection, else the bar the cursor is in.
Result App::newMidiClipForEditor()
{
    if (session == nullptr || focused >= session->numTracks() || ! Session::isInstrumentTrack (session->track (focused)))
        return Result::fail (L"edit new makes a MIDI clip on an instrument track \x2014 focus one (\x2191\x2193)");

    if (recording)
        return Result::fail ("stop recording first");

    if (! console.isOpen())
        return Result::fail ("the MIDI editor needs the screen");

    const auto spb = session->samplesPerBeat(), bar = (double) juce::jmax (1, session->beatsPerBar());
    const auto from = hasTimeSelection() ? snapBeats (timeSelection().getStart()) : std::floor ((double) cursor / spb / bar + 1e-9) * bar;
    const auto to = hasTimeSelection() ? snapBeats (timeSelection().getEnd()) : from + bar;
    beginEdit ("new MIDI clip");
    auto clip = Session::makeMidiClip (from, from, to - from);
    session->track (focused).appendChild (clip, um());

    if (auto r = openMidiEditor (clip); ! r.ok)
        return r;

    return Result::good (L"new MIDI clip \x00b7 double-click in it to draw notes \x00b7 ctrl+z takes it away");
}

juce::ValueTree App::midiClipAt (int track, double from, double to) const
{
    if (session == nullptr || track < 0 || track >= session->numTracks())
        return {};

    const auto midi = Session::beatClips (session->track (track));
    const auto spb = session->samplesPerBeat();

    for (auto it = midi.rbegin(); it != midi.rend(); ++it)
        if ((double) (*it)[ids::beat] * spb < to && Session::clipEnd (*it) * spb > from)
            return *it;

    return {};
}

Result App::toggleLiveKeys()
{
    if (liveKeys.on())
    {
        liveKeys.setOn (false);
        return Result::good ("keys mode off");
    }

    if (session == nullptr)
        return Result::fail ("open a session first, then ctrl+k plays the armed instrument from the keyboard");

    liveKeys.setOn (true);
    bool armed = false;

    for (int i = 0; i < session->numTracks(); ++i)
        armed = armed || (Session::isInstrumentTrack (session->track (i)) && (bool) session->track (i)[ids::armed]);

    return Result::good (L"keys mode: the keyboard plays " + liveKeys.range()
                         + (armed ? juce::String() : juce::String (L" \x00b7 arm an instrument track (shift+R) to hear it")));
}

void App::handleMainKey (const Key& k)
{
    // Arrows: plain = bar, ctrl = 4 bars, alt = 1 step, ctrl+alt = 4 steps. Shift never changes
    // the distance; it extends the selection (time with left/right, tracks with up/down).
    const auto step = k.alt ? (k.ctrl ? Step::FineLarge : Step::Fine) : (k.ctrl ? Step::Large : Step::Normal);

    // Keys mode: typing plays notes (space, arrows and ctrl keys still work); esc leaves it.
    if (liveKeys.on())
    {
        if (k.code == Key::Escape)
        {
            say (toggleLiveKeys());
            return;
        }

        if (liveKeys.handle (k, juce::Time::getMillisecondCounterHiRes()))
            return;
    }

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
    if (handleBrowserKey (k) || handlePluginPickerKey (k) || handleBusPickerKey (k) || handleTrackMenuKey (k) || handleAutoPanelKey (k) || handleMidiPanelKey (k)
        || handleSettingsKey (k) || handleIoKey (k))
        return;

    // The video row: arrows step frames (ctrl: seconds); holding one scrubs. Alt keeps the beat.
    if (topLane == TopLane::video && session != nullptr && (k.code == Key::Left || k.code == Key::Right) && ! k.alt)
    {
        say (stepFrame (k.code == Key::Right ? +1 : -1, k.ctrl, k.shift));
        return;
    }

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
        // Alt moves the selected (or focused) tracks up or down a place (on a lane, its points).
        case Key::Up:
        case Key::Down:
            if (k.alt && ! k.ctrl && session != nullptr && ! onTopLane() && selectedTracks().size() <= 1 && focusedLaneParam().isNotEmpty())
            {
                // On a lane: its points up or down (the selection's, or the one at the cursor).
                beginEdit ("automation", "auto:" + juce::String (focused) + ":" + focusedLaneParam());
                say (nudgeAutomation (k.code == Key::Up ? +1 : -1));
            }
            else if (k.alt && ! k.ctrl)
            {
                say (moveTracks (k.code == Key::Up ? -1 : +1));
            }
            else if (k.ctrl && session != nullptr && session->numTracks() > 0)
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
    // On a track showing automation, delete removes the selection's points instead.
    const bool onAutomation = session != nullptr && ! onTopLane() && selectedTracks().size() <= 1 && focusedLaneParam().isNotEmpty();

    if ((k.code == Key::Delete || (k.code == Key::Backspace && commandLine.isEmpty())) && hasTimeSelection() && onAutomation)
    {
        beginEdit ("delete points");
        say (clearAutomation());
        return;
    }

    if ((k.code == Key::Delete || (k.code == Key::Backspace && commandLine.isEmpty())) && hasTimeSelection())
    {
        beginEdit ("delete");
        say (deleteSelection (defaultTargets()));
        return;
    }

    // Alt+C clears the latched clip lights (inputs, tracks, the mix).
    if (k.code == Key::Char && k.alt && ! k.ctrl && (k.ch == 'c' || k.ch == 'C'))
    {
        for (auto& m : inputMeters) m.clipped = false;
        for (auto& m : trackMeters) m.clipped = false;
        for (auto& m : masterMeters) m.clipped = false;
        say (Result::good ("clip lights cleared"));
        return;
    }

    // Alt+shift+3 consolidates the selection into one clip per track, as in Pro Tools.
    if (k.code == Key::Char && k.alt && ! k.ctrl && k.ch == '#')
    {
        beginEdit ("consolidate");
        say (consolidate (defaultTargets()));
        return;
    }

    // Alt+/ toggles a hit at the cursor (at the playhead while rolling) on a sample track.
    if (k.code == Key::Char && k.alt && ! k.ctrl && k.ch == '/')
    {
        say (toggleHitAtCursor());
        return;
    }

    // Alt+[ ] pans the selected (or focused) tracks, 5 at a time.
    if (k.code == Key::Char && (k.ch == '[' || k.ch == ']') && k.alt && ! k.ctrl && commandLine.isEmpty() && session != nullptr)
    {
        if (onTopLane() || mixFocused())
            say (Result::fail (L"alt+[ ] pans a track \x2014 \x2193 to one"));
        else
        {
            juce::StringArray who;

            for (auto t : defaultTargets())
                who.add (juce::String (t));

            beginEdit ("pan", "pan:" + who.joinIntoString (","));
            say (nudgePan (defaultTargets(), k.ch == ']' ? 0.05 : -0.05));
        }

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
        else if (k.ch == 's' && session != nullptr)
            say (execute ("save"));
        else if (k.ch == 'w' && session != nullptr)
            say (execute ("close")); // saves, then back to the sessions
        else if (k.ch == '\\' && session != nullptr)
            say (execute ("ed close")); // every plugin window, from anywhere
        else if (k.ch == 'k')
            say (toggleLiveKeys());
        else if (k.ch == 'b') // into the sample browser (showing it); from inside it, hides it
            say (browserVisible && browserActive ? showBrowser (false) : focusBrowser());
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
                message.clear(); // (clip lights: alt+C)
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
            // Shift+. (>) is an accent. With none armed, . types as usual. Held, it's one hit
            // (the samples are one-shots), not a stream of them.
            else if (lineEmpty && (k.ch == '.' || k.ch == '>') && playPads (k.ch == '>' ? 1.0f : 0.8f, k.repeat))
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
            // ~ (shift+`): a marker here (the playhead while rolling). No ctrl+` for it: macOS
            // Terminal sends nothing for that.
            else if (lineEmpty && k.ch == '~' && session != nullptr)
            {
                beginEdit ("marker");
                say (addMarker (engine.isRolling() ? heardPlayhead() : cursor));
            }
            // `: to the markers row (pairs with ~, add a marker); again: back to the track.
            else if (lineEmpty && k.ch == '`' && session != nullptr)
            {
                if (topLane == TopLane::markers)
                    focusTrack (focused);
                else
                    topLane = TopLane::markers, trackAnchor = -1;
            }
            // \: into the routing column on the focused track (inserts, sends, then the output).
            else if (lineEmpty && k.ch == '\\' && session != nullptr)
            {
                if (onTopLane() || (session->numTracks() == 0 && ! mixFocused()))
                    say (Result::fail (L"\\ routes a track or the mix \x2014 \x2193 to one"));
                else
                {
                    if (! ioVisible)
                        ioVisible = true, saveSettings();

                    ioCell = ioFirstInsert;
                    say (Result::good (describeIoCell (ioTrack(), ioCell)));
                }
            }
            // | (shift+\): open or close the plugin under the cursor, playing or not.
            // On an automation lane: the parameters, to change what it shows.
            else if (lineEmpty && k.ch == '|' && session != nullptr && focusedLaneParam().isNotEmpty())
                say (openAutoPanel());
            else if (lineEmpty && k.ch == '|' && session != nullptr)
                say (togglePluginHere());
            // f: the focused track fills the track area (a piano roll on instrument tracks).
            else if (lineEmpty && k.ch == 'f' && session != nullptr)
                say (execute ("fill"));
            // shift+T: the track menu (rename, input, output, effects, width, delete...).
            else if (lineEmpty && k.ch == 'T' && session != nullptr)
                say (openTrackMenu());
            // shift+E: the focused track in its editor, MIDI or audio (or closes it).
            else if (lineEmpty && k.ch == 'E' && session != nullptr)
                say (toggleEditor());
            // shift+W: automation write (mapped controllers moved while playing are recorded).
            else if (lineEmpty && k.ch == 'W' && session != nullptr)
                say (setWrite ({}));
            // shift+A: the focused track's automation lanes open under it (or close).
            else if (lineEmpty && k.ch == 'A' && session != nullptr)
                say (toggleLanes());
            else if (lineEmpty && (k.ch == 'R' || k.ch == 'S' || k.ch == 'M' || k.ch == 'I'))
                say (execute (k.ch == 'R' ? "r" : k.ch == 'S' ? "s" : k.ch == 'M' ? "m" : "mon"));
            else if ((k.ch == 't' || k.ch == 'T') && tappingTempo())
                tapTempo();
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

// Opening a session from the menu: the session file, then its audio and plugins a few per
// frame, so the screen can say what it's loading (plugins can take seconds each).
void App::startOpening (const juce::File& dir)
{
    juce::String error;
    timing::mark ("opening " + dir.getFullPathName());
    auto s = Session::load (dir, error);
    timing::mark ("session.json read");

    if (s == nullptr)
    {
        say (Result::fail (error));
        return;
    }

    attachSession (std::move (s));
    timing::mark ("session attached");
    opening = std::make_unique<Opening>();

    for (int i = 0; i < session->numTracks(); ++i)
    {
        const auto t = session->track (i);

        if (Session::isSampleTrack (t))
            opening->steps.push_back ({ "audio", t[ids::sample].toString(), [this, t] { audioFor (t[ids::sample].toString()); } });

        for (const auto& c : Session::clips (t))
            opening->steps.push_back ({ "audio", c[ids::file].toString(), [this, c] { clipFor (c); } });

    }

    // Plugins aren't steps: they load after the session is up (loadNextPlugin).
}

void App::stepOpening()
{
    // About a frame's worth of work, then a frame drawn (a slow plugin still takes its time).
    const auto until = juce::Time::getMillisecondCounterHiRes() + 40.0;

    while (opening->done < opening->steps.size() && juce::Time::getMillisecondCounterHiRes() < until)
{
        const auto& step = opening->steps[opening->done++];
        timing::Scope t ("slow: " + step.kind + " " + step.name, 100.0);
        step.run();
    }

    if (opening->done < opening->steps.size())
        return;

    opening.reset();
    proceedToSession();
    say (Result::good ("opened " + session->name()
                       + (pluginQueue.empty() ? L" \x00b7 " + session->folder().getFullPathName()
                                              : L" \x00b7 loading " + juce::String ((int) pluginQueue.size()) + L" plugins (their tracks are silent until they're in)")));
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
            if (opening != nullptr)
                return; // already opening one

            if (pickIndex < numRecent)
                startOpening (recent[(size_t) pickIndex]);
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
