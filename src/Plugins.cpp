#include "Plugins.h"
#include "Session.h"

namespace taw
{
struct PluginSlot::EditorWindow final : juce::DocumentWindow
{
    EditorWindow (const juce::String& title, juce::AudioProcessorEditor* editor)
        : DocumentWindow (title, juce::Colours::black, DocumentWindow::closeButton | DocumentWindow::minimiseButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        // Over the terminal, so it stays in view while you go back to typing.
        setAlwaysOnTop (true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    // Deleting a window from its own close button isn't safe; the owner closes it next frame.
    void closeButtonPressed() override
    {
        closeRequested = true;
        setVisible (false);
    }

    bool closeRequested = false;
};

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

bool PluginSlot::hasEditor() const { return plugin->hasEditor(); }
bool PluginSlot::editorShowing() const { return window != nullptr && window->isVisible(); }
bool PluginSlot::editorWantsToClose() const { return window != nullptr && window->closeRequested; }

void PluginSlot::showEditor (const juce::String& title)
{
    if (window != nullptr)
    {
        window->setVisible (true);
        window->toFront (true);
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

    // Forget plugins that were uninstalled.
    for (const auto& d : list.getTypes())
        if (juce::File::isAbsolutePath (d.fileOrIdentifier) && ! juce::File (d.fileOrIdentifier).exists())
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
        failed.add (juce::File::isAbsolutePath (job.plugin) ? juce::File (job.plugin).getFileNameWithoutExtension() : job.plugin);
    }

    job.out.deleteFile();
    ++done;
    save(); // after every plugin, so an interrupted scan keeps what it found
}

juce::String PluginCatalog::scanningName() const
{
    return jobs.empty() ? juce::String() : juce::File (jobs.front()->plugin).getFileNameWithoutExtension();
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
        if (d.isInstrument)
            out.push_back (d);

    std::sort (out.begin(), out.end(), [] (const auto& a, const auto& b) { return a.name.compareIgnoreCase (b.name) < 0; });
    return out;
}

std::vector<juce::PluginDescription> PluginCatalog::effects() const
{
    std::vector<juce::PluginDescription> out;

    for (const auto& d : list.getTypes())
        if (! d.isInstrument)
            out.push_back (d);

    std::sort (out.begin(), out.end(), [] (const auto& a, const auto& b) { return a.name.compareIgnoreCase (b.name) < 0; });
    return out;
}

std::optional<juce::PluginDescription> PluginCatalog::find (const juce::String& queryIn, bool instrument) const
{
    const auto query = queryIn.trim().unquoted();
    std::optional<juce::PluginDescription> best;
    int bestRank = 0;

    // Exact name, then the start of a name, then anywhere in it (or the maker); shortest wins a
    // tie, except that "X Mono" loses to "X Stereo" (taw's tracks run in stereo).
    auto length = [] (const juce::String& name) { return name.length() + (name.endsWithIgnoreCase (" Mono") ? 100 : 0); };

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
                                                   juce::String& error)
{
    auto instance = formats.createPluginInstance (desc, sampleRate, maxBlock, error);

    if (instance == nullptr)
    {
        if (error.isEmpty())
            error = "couldn't load " + desc.name;

        return {};
    }

    // Effects run on a stereo track: ask for stereo in and out (a plugin that can't keep its own).
    if (! desc.isInstrument)
    {
        auto layout = instance->getBusesLayout();

        if (layout.inputBuses.size() > 0)
            layout.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();

        if (layout.outputBuses.size() > 0)
            layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();

        if (instance->checkBusesLayoutSupported (layout))
            instance->setBusesLayout (layout);
    }

    auto slot = std::make_shared<PluginSlot> (std::move (instance), desc);
    slot->prepare (sampleRate, maxBlock);
    return slot;
}
} // namespace taw
