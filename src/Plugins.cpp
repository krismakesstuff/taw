#include "Plugins.h"
#include "Session.h"

namespace taw
{
//==============================================================================
// Plugin windows get taw's own title bar: the theme's colours, the track and slot on the left,
// who has the keyboard on the right, and a minimise and a close button.

static PluginSlot::WindowColours windowColours { juce::Colour (0xff262626), juce::Colour (0xffd0d0d0), juce::Colour (0xff767676),
                                                  juce::Colour (0xffffaf00) };

void PluginSlot::setWindowColours (const WindowColours& c) { taw::windowColours = c; }
const PluginSlot::WindowColours& PluginSlot::windowColours() { return taw::windowColours; }

static juce::Font titleFont()
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
}

struct TitleButton final : juce::Button
{
    explicit TitleButton (bool close) : Button (close ? "close" : "minimise"), isClose (close) {}

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto& c = windowColours;

        if (over || down)
            g.fillAll (isClose ? juce::Colour (0xffc42b1c).withAlpha (down ? 0.8f : 1.0f) : c.text.withAlpha (down ? 0.3f : 0.2f));

        g.setColour (over && isClose ? juce::Colours::white : over ? c.text : c.dim);
        const auto centre = getLocalBounds().toFloat().getCentre();
        const float r = 4.5f;

        if (isClose)
        {
            g.drawLine (centre.x - r, centre.y - r, centre.x + r, centre.y + r, 1.3f);
            g.drawLine (centre.x - r, centre.y + r, centre.x + r, centre.y - r, 1.3f);
        }
        else
        {
            g.drawLine (centre.x - r, centre.y, centre.x + r, centre.y, 1.3f);
        }
    }

    const bool isClose;
};

struct TitleLook final : juce::LookAndFeel_V4
{
    void drawDocumentWindowTitleBar (juce::DocumentWindow&, juce::Graphics&, int w, int h, int, int, const juce::Image*, bool) override;

    juce::Button* createDocumentWindowButton (int type) override
    {
        return type == juce::DocumentWindow::closeButton || type == juce::DocumentWindow::minimiseButton
                   ? new TitleButton (type == juce::DocumentWindow::closeButton)
                   : nullptr;
    }

    void positionDocumentWindowButtons (juce::DocumentWindow&, int x, int y, int w, int h, juce::Button* minimise, juce::Button*,
                                        juce::Button* close, bool) override
    {
        const int bw = h * 3 / 2;

        if (close != nullptr)
            close->setBounds (x + w - bw, y, bw, h);

        if (minimise != nullptr)
            minimise->setBounds (x + w - bw * 2, y, bw, h);
    }
};

static TitleLook& titleLook()
{
    static TitleLook look;
    return look;
}

juce::LookAndFeel& windowLook() { return titleLook(); }

struct PluginSlot::EditorWindow final : juce::DocumentWindow, TitledWindow
{
    EditorWindow (const juce::String& windowTitle, juce::AudioProcessorEditor* editor)
        : DocumentWindow (windowTitle, juce::Colours::black, DocumentWindow::closeButton | DocumentWindow::minimiseButton), title (windowTitle)
    {
        setLookAndFeel (&titleLook());
        setUsingNativeTitleBar (false);
        setTitleBarHeight (24);
        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        // Over the terminal, so it stays in view while you go back to typing. It never wants
        // the keyboard: that stays with taw (App hands it back if a click takes it).
        setAlwaysOnTop (true);
        setWantsKeyboardFocus (false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    ~EditorWindow() override
    {
        clearContentComponent(); // the editor goes while the look it was drawn with is still set
        setLookAndFeel (nullptr);
    }

    // Deleting a window from its own close button isn't safe; the owner closes it next frame.
    void closeButtonPressed() override
    {
        closeRequested = true;
        setVisible (false);
    }

    // The bar is ours, not Windows': JUCE would report it as a native caption and buttons, and
    // then Windows does the dragging and clicking itself, and the bar never hears a hover or a
    // double-click. As ordinary content, dragging it moves the window (ResizableWindow does
    // that) and the buttons light up. Resizing edges stay native.
    WindowControlKind findControlAtPoint (juce::Point<float> pt) const override
    {
        const auto kind = DocumentWindow::findControlAtPoint (pt);

        switch (kind)
        {
            case WindowControlKind::caption:
            case WindowControlKind::minimise:
            case WindowControlKind::maximise:
            case WindowControlKind::close:
                return WindowControlKind::client;
            default:
                return kind;
        }
    }

    // JUCE disables a window's title buttons while it isn't the active window, and these windows
    // almost never are (the keyboard goes back to taw), so keep them working regardless.
    void activeWindowStatusChanged() override
    {
        DocumentWindow::activeWindowStatusChanged();

        for (auto* b : { getMinimiseButton(), getCloseButton() })
            if (b != nullptr)
                b->setEnabled (true);
    }

    // The title bar double-clicked: the owner gives this window the keyboard to keep (or takes
    // it back). Not maximise: plugin windows are their own size.
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (getTitleBarArea().contains (e.x, e.y))
            titleDoubleClicked = true;
    }

    juce::String titleName() const override { return title; }
    juce::String titleStatus() const override { return keyboardHeld ? juce::String (L"keys here \x00b7 double-click: back to taw") : juce::String ("keys: taw"); }
    bool titleLit() const override { return keyboardHeld; }

    bool closeRequested = false, titleDoubleClicked = false;
    const juce::String title;
    bool keyboardHeld = false; // double-clicked title: it keeps the keyboard
};

void TitleLook::drawDocumentWindowTitleBar (juce::DocumentWindow& window, juce::Graphics& g, int w, int h, int, int, const juce::Image*, bool)
{
    const auto& c = windowColours;
    const auto* titled = dynamic_cast<const TitledWindow*> (&window);
    const bool held = titled != nullptr && titled->titleLit();

    g.fillAll (c.bg);

    // A line of the accent along the bottom while the window has the keyboard.
    if (held)
    {
        g.setColour (c.accent);
        g.fillRect (0, h - 2, w, 2);
    }

    const int buttons = h * 3, pad = 10;
    const auto status = titled != nullptr ? titled->titleStatus() : juce::String();
    g.setFont (titleFont());
    const int statusW = juce::GlyphArrangement::getStringWidthInt (titleFont(), status) + pad;

    g.setColour (held ? c.accent : c.dim);
    g.drawText (status, w - buttons - statusW, 0, statusW, h, juce::Justification::centredRight, true);

    auto name = titled != nullptr ? titled->titleName() : window.getName();
    name = name.upToLastOccurrenceOf (juce::String (L" \x00b7 taw"), false, false);
    g.setColour (c.text);
    g.drawText (name, pad, 0, juce::jmax (0, w - buttons - statusW - pad * 2), h, juce::Justification::centredLeft, true);
}

PluginSlot::PluginSlot (std::unique_ptr<juce::AudioPluginInstance> p, juce::PluginDescription d)
    : plugin (std::move (p)), desc (std::move (d))
{
    plugin->setPlayHead (this);
    midi.ensureSize (8192);
}

PluginSlot::~PluginSlot()
{
    window.reset(); // the editor goes before its plugin

    if (prepared)
        plugin->releaseResources();
}

void PluginSlot::prepare (double sampleRate, int maxBlock)
{
    const juce::SpinLock::ScopedLockType lock (processLock);

    if (prepared)
        plugin->releaseResources();

    plugin->prepareToPlay (sampleRate, maxBlock);
    buffer.setSize (juce::jmax (2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels()), maxBlock);
    transport.sampleRate = sampleRate;
    numSounding = 0;
    nextPos = -1;
    prepared = true;
}

void PluginSlot::setNonRealtime (bool offline)
{
    plugin->setNonRealtime (offline);
}

juce::MemoryBlock PluginSlot::getState() const
{
    juce::MemoryBlock state;
    plugin->getStateInformation (state);
    return state;
}

void PluginSlot::setState (const juce::MemoryBlock& state)
{
    if (state.getSize() > 0)
        plugin->setStateInformation (state.getData(), (int) state.getSize());
}

std::vector<PluginSlot::Parameter> PluginSlot::parameters() const
{
    std::vector<Parameter> out;
    const auto& params = plugin->getParameters();

    for (int i = 0; i < params.size(); ++i)
    {
        auto* p = params[i];

        // Hidden ones (bypass, program changes some plugins expose) aren't worth a lane.
        if (! p->isAutomatable())
            continue;

        const auto* hosted = dynamic_cast<const juce::HostedAudioProcessorParameter*> (p);
        out.push_back ({ i, hosted != nullptr ? hosted->getParameterID() : juce::String (i), p->getName (64) });
    }

    return out;
}

int PluginSlot::findParameter (const juce::String& id) const
{
    const auto& params = plugin->getParameters();

    for (int i = 0; i < params.size(); ++i)
        if (const auto* hosted = dynamic_cast<const juce::HostedAudioProcessorParameter*> (params[i]))
        {
            if (hosted->getParameterID() == id)
                return i;
        }
        else if (juce::String (i) == id)
        {
            return i;
        }

    return -1;
}

float PluginSlot::getParameter (int index) const
{
    const auto& params = plugin->getParameters();
    return juce::isPositiveAndBelow (index, params.size()) ? params[index]->getValue() : 0.0f;
}

void PluginSlot::setParameter (int index, float value)
{
    // JUCE's hosted plugins take this from any thread: the value goes with the next block.
    const auto& params = plugin->getParameters();

    if (juce::isPositiveAndBelow (index, params.size()))
        params[index]->setValue (value);
}

juce::String PluginSlot::parameterText (int index, float value) const
{
    const auto& params = plugin->getParameters();

    if (! juce::isPositiveAndBelow (index, params.size()))
        return {};

    const auto label = params[index]->getLabel();
    return params[index]->getText (value, 16).trim() + (label.isNotEmpty() ? " " + label : juce::String());
}

bool PluginSlot::hasEditor() const { return plugin->hasEditor(); }
bool PluginSlot::editorShowing() const { return window != nullptr && window->isVisible(); }
bool PluginSlot::editorWantsToClose() const { return window != nullptr && window->closeRequested; }
void* PluginSlot::editorHandle() const { return editorShowing() ? window->getWindowHandle() : nullptr; }

void PluginSlot::markKeyboard (bool held)
{
    if (window == nullptr || window->keyboardHeld == held)
        return;

    window->keyboardHeld = held;
    window->repaint (window->getTitleBarArea());
}

juce::Image PluginSlot::editorSnapshot()
{

    return window != nullptr ? window->createComponentSnapshot (window->getLocalBounds()) : juce::Image();
}

bool PluginSlot::takeTitleDoubleClick()
{
    return window != nullptr && std::exchange (window->titleDoubleClicked, false);
}

void PluginSlot::showEditor (const juce::String& title)
{
    if (window != nullptr)
    {
        window->setVisible (true);
        window->toFront (false); // in front, but the keyboard stays with taw
        return;
    }

    // Plugins without their own window get JUCE's generic one: a slider per parameter.
    auto* editor = plugin->hasEditor() ? plugin->createEditorAndMakeActive() : new juce::GenericAudioProcessorEditor (*plugin);

    if (editor != nullptr)
        window = std::make_unique<EditorWindow> (title, editor);
}

void PluginSlot::closeEditor()
{
    window.reset();
}

juce::Optional<juce::AudioPlayHead::PositionInfo> PluginSlot::getPosition() const
{
    const auto& t = transport;
    PositionInfo info;
    const auto samplesPerQuarter = t.sampleRate * 60.0 / juce::jmax (1.0, t.bpm);
    const auto ppq = (double) t.pos / samplesPerQuarter;
    const auto barQuarters = t.timeSigNum * 4.0 / juce::jmax (1, t.timeSigDen);

    info.setBpm (t.bpm);
    info.setTimeSignature (TimeSignature { t.timeSigNum, t.timeSigDen });
    info.setTimeInSamples (t.pos);
    info.setTimeInSeconds ((double) t.pos / t.sampleRate);
    info.setPpqPosition (ppq);
    info.setPpqPositionOfLastBarStart (std::floor (ppq / barQuarters) * barQuarters);
    info.setBarCount ((juce::int64) std::floor (ppq / barQuarters));
    info.setIsPlaying (t.rolling);
    info.setIsRecording (t.recording);
    return info;
}

void PluginSlot::render (const Sequence& seq, const HostTransport& t, int n, const MidiMsg* live, int numLive,
                         float* left, float* right)
{
    const juce::SpinLock::ScopedTryLockType lock (processLock);

    if (! lock.isLocked() || ! prepared || n > buffer.getNumSamples())
        return; // being prepared (a device change): a block of silence

    transport = t;
    midi.clear();

    auto add = [this] (const MidiMsg& m, juce::int64 offset)
    {
        const juce::uint8 raw[3] { m.status, m.d1, m.d2 };
        midi.addEvent (raw, m.size(), (int) offset);
    };

    const auto pos = t.pos, end = pos + n;

    // A stop or a jump (seek, loop wrap) ends every note the timeline started.
    if ((numSounding > 0 || sequenceSustain) && (! t.rolling || pos != nextPos))
    {
        for (int i = 0; i < numSounding; ++i)
            add (MidiMsg::noteOff (sounding[(size_t) i].channel, sounding[(size_t) i].pitch), 0);

        if (sequenceSustain)
            for (int ch = 0; ch < 16; ++ch)
                add (MidiMsg::controller (ch, 64, 0), 0);

        numSounding = 0;
        sequenceSustain = false;
    }

    // Chase: starting (or jumping) in the middle of things, the controllers as they were last set
    // before here (the sustain pedal, the mod wheel, a bend), then the notes already playing,
    // from their middle, as other DAWs do. Only on a start or jump, so a scan back is fine.
    if (t.rolling && pos != nextPos)
    {
        std::array<bool, 16 * 130> seen {}; // per channel: 128 controllers, pitch bend, pressure
        auto c = std::lower_bound (seq.controls.begin(), seq.controls.end(), pos,
                                   [] (const Sequence::Control& x, juce::int64 p) { return x.pos < p; });

        while (c != seq.controls.begin())
        {
            const auto& m = (--c)->msg;
            const auto type = m.type();
            const int which = type == 0xb0 ? (m.d1 & 0x7f) : type == 0xe0 ? 128 : type == 0xd0 ? 129 : -1;

            if (which < 0 || seen[(size_t) (m.channel() * 130 + which)])
                continue;

            seen[(size_t) (m.channel() * 130 + which)] = true;
            add (m, 0);

            if (type == 0xb0 && m.d1 == 64)
                sequenceSustain = sequenceSustain || m.d2 >= 64;
        }

        for (const auto& note : seq.notes)
        {
            if (note.start >= pos)
                break;

            if (note.end <= pos)
                continue;

            add (MidiMsg::noteOn (note.channel, note.pitch, note.vel), 0);

            if (note.end < end)
                add (MidiMsg::noteOff (note.channel, note.pitch), note.end - pos);
            else if (numSounding < (int) sounding.size())
                sounding[(size_t) numSounding++] = { note.end, note.channel, note.pitch };
        }
    }

    if (t.rolling)
    {
        // Notes ending in this block, then controllers, then notes starting: at any one sample a
        // note-off goes ahead of a note-on, so back-to-back repeats retrigger.
        for (int i = 0; i < numSounding;)
        {
            const auto& s = sounding[(size_t) i];

            if (s.end < end)
            {
                add (MidiMsg::noteOff (s.channel, s.pitch), juce::jmax<juce::int64> (0, s.end - pos));
                sounding[(size_t) i] = sounding[(size_t) --numSounding];
            }
            else
            {
                ++i;
            }
        }

        auto c = std::lower_bound (seq.controls.begin(), seq.controls.end(), pos,
                                   [] (const Sequence::Control& x, juce::int64 p) { return x.pos < p; });

        for (; c != seq.controls.end() && c->pos < end; ++c)
        {
            add (c->msg, c->pos - pos);

            if (c->msg.type() == 0xb0 && c->msg.d1 == 64)
                sequenceSustain = sequenceSustain || c->msg.d2 >= 64;
        }

        auto note = std::lower_bound (seq.notes.begin(), seq.notes.end(), pos,
                                      [] (const Sequence::Note& x, juce::int64 p) { return x.start < p; });

        for (; note != seq.notes.end() && note->start < end; ++note)
        {
            add (MidiMsg::noteOn (note->channel, note->pitch, note->vel), note->start - pos);

            if (note->end < end)
                add (MidiMsg::noteOff (note->channel, note->pitch), note->end - pos);
            else if (numSounding < (int) sounding.size())
                sounding[(size_t) numSounding++] = { note->end, note->channel, note->pitch };
        }

        nextPos = end;
    }
    else
    {
        nextPos = -1;
    }

    // The controller, straight through. Remember what's held so it can be let go of.
    if (live != nullptr)
    {
        for (int i = 0; i < numLive; ++i)
        {
            const auto& m = live[i];
            add (m, 0);

            if (m.isNoteOn())
                liveHeld.set ((size_t) (m.channel() * 128 + m.d1));
            else if (m.isNoteOff())
                liveHeld.reset ((size_t) (m.channel() * 128 + m.d1));
            else if (m.type() == 0xb0 && m.d1 == 64)
                liveSustain.set ((size_t) m.channel(), m.d2 >= 64);

            anyLive = true;
        }
    }
    else if (anyLive)
    {
        // Disarmed with keys or the pedal down: release them, or they'd hang.
        for (size_t k = 0; k < liveHeld.size(); ++k)
            if (liveHeld[k])
                add (MidiMsg::noteOff ((int) (k / 128), (int) (k % 128)), 0);

        for (size_t ch = 0; ch < liveSustain.size(); ++ch)
            if (liveSustain[ch])
                add (MidiMsg::controller ((int) ch, 64, 0), 0);

        liveHeld.reset();
        liveSustain.reset();
        anyLive = false;
    }

    juce::AudioBuffer<float> block (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), n);
    block.clear();
    plugin->processBlock (block, midi);

    const auto outs = plugin->getTotalNumOutputChannels();

    if (outs >= 1)
    {
        juce::FloatVectorOperations::add (left, block.getReadPointer (0), n);
        juce::FloatVectorOperations::add (right, block.getReadPointer (outs >= 2 ? 1 : 0), n);
    }
}

void PluginSlot::process (const HostTransport& t, int n, float* left, float* right)
{
    const juce::SpinLock::ScopedTryLockType lock (processLock);

    if (! lock.isLocked() || ! prepared || n > buffer.getNumSamples())
        return;

    transport = t;
    midi.clear();

    juce::AudioBuffer<float> block (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), n);
    block.clear();
    const auto ins = plugin->getTotalNumInputChannels(), outs = plugin->getTotalNumOutputChannels();

    if (ins >= 2)
    {
        block.copyFrom (0, 0, left, n);
        block.copyFrom (1, 0, right, n);
    }
    else if (ins == 1) // a mono effect gets both sides
    {
        block.copyFrom (0, 0, left, n, 0.5f);
        block.addFrom (0, 0, right, n, 0.5f);
    }

    plugin->processBlock (block, midi);

    if (outs >= 1)
    {
        juce::FloatVectorOperations::copy (left, block.getReadPointer (0), n);
        juce::FloatVectorOperations::copy (right, block.getReadPointer (outs >= 2 ? 1 : 0), n);
    }
}

//==============================================================================
struct PluginCatalog::ScanJob
{
    juce::String plugin;
    juce::File out;
    juce::ChildProcess process;
    double started = 0.0;
};

namespace
{
constexpr int parallelScans = 4;
constexpr double scanTimeoutMs = 60000.0; // a plugin that takes longer is treated as hung
} // namespace

PluginCatalog::PluginCatalog()
    : cacheFile (configFolder().getChildFile ("plugins.xml"))
{
    juce::addDefaultFormatsToManager (formats);

    if (auto xml = juce::parseXML (cacheFile))
        list.recreateFromXml (*xml);
}

PluginCatalog::~PluginCatalog()
{
    for (auto& job : jobs)
    {
        job->process.kill();
        job->out.deleteFile();
    }
}

void PluginCatalog::startScan (bool retryFailed)
{
    if (scanning)
        return;

    if (retryFailed)
        list.clearBlacklistedFiles();

    // Forget plugins that were uninstalled (files, and Audio Units, which are identifiers).
    for (const auto& d : list.getTypes())
        for (auto* format : formats.getFormats())
            if (format->getName() == d.pluginFormatName && ! format->doesPluginStillExist (d))
                list.removeType (d);

    pending.clear();
    failed.clear();
    retryAlone.clear();
    alonePass = false;

    // Only files that are new or changed since they were last looked at, and never ones that
    // failed before (unless retrying).
    for (auto* format : formats.getFormats())
        for (const auto& file : format->searchPathsForPlugins (format->getDefaultLocationsToSearch(), true, false))
            if (! list.getBlacklistedFiles().contains (file) && ! list.isListingUpToDate (file, *format))
                pending.add (file);

    total = pending.size();
    done = 0;
    scanning = true;
}

bool PluginCatalog::scanStep()
{
    if (! scanning)
        return false;

    const auto now = juce::Time::getMillisecondCounterHiRes();

    for (auto it = jobs.begin(); it != jobs.end();)
    {
        auto& job = **it;
        const bool exited = ! job.process.isRunning();

        if (exited || now - job.started > scanTimeoutMs)
        {
            if (! exited)
                job.process.kill();

            finish (job, exited);
            it = jobs.erase (it);
        }
        else
        {
            ++it;
        }
    }

    const auto self = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();

    // Some plugins fail when another is loading at the same moment (shared installs, licence
    // checks), so whatever failed gets one last try, one at a time.
    if (jobs.empty() && pending.isEmpty() && ! retryAlone.isEmpty() && ! alonePass)
    {
        pending.swapWith (retryAlone);
        alonePass = true;
    }

    while ((int) jobs.size() < (alonePass ? 1 : parallelScans) && ! pending.isEmpty())
    {
        auto job = std::make_unique<ScanJob>();
        job->plugin = pending[0];
        job->out = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("taw-scan", ".xml", false);
        job->started = now;
        pending.remove (0);

        if (job->process.start (juce::StringArray { self, "--scan-plugin", job->plugin, job->out.getFullPathName() }, 0))
            jobs.push_back (std::move (job));
        else
            finish (*job, false);
    }

    if (jobs.empty() && pending.isEmpty())
    {
        scanning = false;
        return false;
    }

    return true;
}

// Takes in what the child found. Nothing found (a crash, a hang, not really a plugin): the file
// goes on the list of ones to skip.
void PluginCatalog::finish (ScanJob& job, bool exited)
{
    bool found = false;

    if (exited)
        if (auto xml = juce::parseXML (job.out))
            for (auto* e : xml->getChildIterator())
            {
                juce::PluginDescription d;

                if (d.loadFromXml (*e))
                {
                    list.addType (d);
                    found = true;
                }
            }

    if (! found && ! alonePass)
    {
        retryAlone.add (job.plugin);
        job.out.deleteFile();
        return;
    }

    if (! found)
    {
        list.addToBlacklist (job.plugin);
        failed.add (nameOf (job.plugin));
    }

    job.out.deleteFile();
    ++done;
    save(); // after every plugin, so an interrupted scan keeps what it found
}

juce::String PluginCatalog::scanningName() const
{
    return jobs.empty() ? juce::String() : nameOf (jobs.front()->plugin);
}

juce::String PluginCatalog::nameOf (const juce::String& plugin) const
{
    if (juce::File::isAbsolutePath (plugin))
        return juce::File (plugin).getFileNameWithoutExtension();

    for (auto* format : formats.getFormats())
        if (format->fileMightContainThisPluginType (plugin))
            return format->getNameOfPluginFromIdentifier (plugin);

    return plugin;
}

// Plugins that come as both (KrumSampler, Valhalla...) are listed once, as the VST3. A session
// that chose the Audio Unit still finds it (byIdentifier).
bool PluginCatalog::shadowed (const juce::PluginDescription& d) const
{
    if (d.pluginFormatName != "AudioUnit")
        return false;

    for (const auto& other : list.getTypes())
        if (other.pluginFormatName == "VST3" && other.isInstrument == d.isInstrument && other.name.equalsIgnoreCase (d.name)
            && other.manufacturerName.equalsIgnoreCase (d.manufacturerName))
            return true;

    return false;
}

float PluginCatalog::scanProgress() const
{
    return total > 0 ? (float) done / (float) total : 1.0f;
}

int PluginCatalog::scanInThisProcess (const juce::String& pluginFile, const juce::File& out)
{
    juce::AudioPluginFormatManager fm;
    juce::addDefaultFormatsToManager (fm);
    juce::OwnedArray<juce::PluginDescription> found;

    for (auto* format : fm.getFormats())
        if (format->fileMightContainThisPluginType (pluginFile))
            format->findAllTypesForFile (found, pluginFile);

    juce::XmlElement xml ("PLUGINS");

    for (auto* d : found)
        xml.addChildElement (d->createXml().release());

    return xml.writeTo (out) && ! found.isEmpty() ? 0 : 1;
}

void PluginCatalog::save() const
{
    if (auto xml = list.createXml())
    {
        cacheFile.getParentDirectory().createDirectory();
        xml->writeTo (cacheFile);
    }
}

std::vector<juce::PluginDescription> PluginCatalog::instruments() const
{
    std::vector<juce::PluginDescription> out;

    for (const auto& d : list.getTypes())
        if (d.isInstrument && ! shadowed (d))
            out.push_back (d);

    std::sort (out.begin(), out.end(), [] (const auto& a, const auto& b) { return a.name.compareIgnoreCase (b.name) < 0; });
    return out;
}

std::vector<juce::PluginDescription> PluginCatalog::effects() const
{
    std::vector<juce::PluginDescription> out;

    for (const auto& d : list.getTypes())
        if (! d.isInstrument && ! shadowed (d))
            out.push_back (d);

    std::sort (out.begin(), out.end(), [] (const auto& a, const auto& b) { return a.name.compareIgnoreCase (b.name) < 0; });
    return out;
}

namespace
{
// "PuigTec EQP1A Mono/Stereo" -> "PuigTec EQP1A", "Mono/Stereo". No variant: the whole name, "".
std::pair<juce::String, juce::String> variantOf (const juce::String& name)
{
    for (const auto* v : { " Mono/Stereo", " Stereo", " Mono" })
        if (name.endsWithIgnoreCase (v))
            return { name.dropLastCharacters ((int) std::strlen (v)), juce::String (v).trim() };

    return { name, {} };
}
} // namespace

std::vector<juce::PluginDescription> PluginCatalog::effectsFor (bool monoTrack) const
{
    const auto all = effects();
    juce::StringArray names;

    for (const auto& d : all)
        names.add (d.name.toLowerCase());

    std::vector<juce::PluginDescription> out;

    for (const auto& d : all)
    {
        const auto [base, variant] = variantOf (d.name);
        const auto has = [&, b = base] (const char* v) { return names.contains ((b + " " + v).toLowerCase()); };

        const bool hide = monoTrack ? variant == "Stereo" && (has ("Mono") || has ("Mono/Stereo"))
                                    : (variant == "Mono" || variant == "Mono/Stereo") && has ("Stereo");

        if (! hide)
            out.push_back (d);
    }

    return out;
}

std::optional<juce::PluginDescription> PluginCatalog::find (const juce::String& queryIn, bool instrument, bool monoTrack) const
{
    const auto query = queryIn.trim().unquoted();
    std::optional<juce::PluginDescription> best;
    int bestRank = 0;

    // Exact name, then the start of a name, then anywhere in it (or the maker); shortest wins a
    // tie, except that the variant that doesn't fit the track loses ("X Mono" on a stereo track).
    auto length = [monoTrack] (const juce::String& name)
    {
        const auto variant = variantOf (name).second;
        return name.length() + ((monoTrack ? variant == "Stereo" : variant.startsWith ("Mono")) ? 100 : 0);
    };

    for (const auto& d : instrument ? instruments() : effects())
    {
        const int rank = d.name.equalsIgnoreCase (query) ? 4
                         : d.name.startsWithIgnoreCase (query) ? 3
                         : d.name.containsIgnoreCase (query) ? 2
                         : (d.manufacturerName + " " + d.name).containsIgnoreCase (query) ? 1
                                                                                            : 0;

        if (rank > bestRank || (rank == bestRank && rank > 0 && length (d.name) < length (best->name)))
        {
            best = d;
            bestRank = rank;
        }
    }

    return best;
}

std::optional<juce::PluginDescription> PluginCatalog::byIdentifier (const juce::String& id) const
{
    if (auto d = list.getTypeForIdentifierString (id))
        return *d;

    return std::nullopt;
}

std::shared_ptr<PluginSlot> PluginCatalog::create (const juce::PluginDescription& desc, double sampleRate, int maxBlock,
                                                   juce::String& error, int channels)
{
    auto instance = formats.createPluginInstance (desc, sampleRate, maxBlock, error);

    if (instance == nullptr)
    {
        if (error.isEmpty())
            error = "couldn't load " + desc.name;

        return {};
    }

    // Effects: the track's width in; out the same, or stereo from a mono track (a mono-to-stereo
    // reverb). A plugin that takes neither keeps its own layout.
    if (! desc.isInstrument)
    {
        auto tryLayout = [&] (const juce::AudioChannelSet& in, const juce::AudioChannelSet& out)
        {
            auto layout = instance->getBusesLayout();

            if (layout.inputBuses.size() > 0)
                layout.inputBuses.getReference (0) = in;

            if (layout.outputBuses.size() > 0)
                layout.outputBuses.getReference (0) = out;

            return instance->checkBusesLayoutSupported (layout) && instance->setBusesLayout (layout);
        };

        const auto mono = juce::AudioChannelSet::mono(), stereo = juce::AudioChannelSet::stereo();

        if (channels == 1)
            tryLayout (mono, mono) || tryLayout (mono, stereo) || tryLayout (stereo, stereo);
        else
            tryLayout (stereo, stereo);
    }

    auto slot = std::make_shared<PluginSlot> (std::move (instance), desc);
    slot->prepare (sampleRate, maxBlock);
    return slot;
}
} // namespace taw
