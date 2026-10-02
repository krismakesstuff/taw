#pragma once

#include "Midi.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <bitset>
#include <optional>

namespace taw
{
// One hosted plugin and everything the audio thread needs to drive it: its MIDI buffer, the
// notes the timeline started (so a stop or jump can end them), the keys held from the
// controller, and an output buffer.
//
// This is the seam for moving plugins out of process later: taw only ever talks to a plugin
// through these calls, so a proxy that forwards them to a sandbox process can replace it.
class PluginSlot final : private juce::AudioPlayHead
{
public:
    PluginSlot (std::unique_ptr<juce::AudioPluginInstance>, juce::PluginDescription);
    ~PluginSlot() override;

    // message thread ------------------------------------------------------------
    // Safe while the audio thread is calling render: it outputs silence until this finishes.
    void prepare (double sampleRate, int maxBlock);
    void setNonRealtime (bool);

    juce::MemoryBlock getState() const;
    void setState (const juce::MemoryBlock&);

    const juce::PluginDescription& description() const { return desc; }
    int latency() const { return plugin->getLatencySamples(); } // samples it delays its output by
    int numOutputs() const { return plugin->getTotalNumOutputChannels(); }
    juce::String name() const { return desc.name; }

    bool hasEditor() const;
    bool editorShowing() const;
    void showEditor (const juce::String& title);
    void closeEditor();
    bool editorWantsToClose() const; // its close button was pressed; the owner calls closeEditor
    void* editorHandle() const;      // its native window (nullptr when closed)
    void markKeyboard (bool held);   // its title says when it's keeping the keyboard

    // audio thread (or a bounce thread) ---------------------------------------------
    // Plays [pos, pos + n) into left/right (added). Rolling: the sequence plays; any transport
    // jump ends what it started. live: what the controller sent this block (armed tracks only);
    // when a track stops listening, the keys still held are released.
    void render (const Sequence&, const HostTransport&, int n, const MidiMsg* live, int numLive, float* left, float* right);

    // An effect: left/right go through it in place. While it's being prepared they pass dry.
    void process (const HostTransport&, int n, float* left, float* right);

private:
    juce::Optional<PositionInfo> getPosition() const override;

    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::PluginDescription desc;

    juce::SpinLock processLock; // prepare holds it; render skips a block rather than wait
    bool prepared = false;
    juce::AudioBuffer<float> buffer;
    juce::MidiBuffer midi;
    HostTransport transport;

    // Notes the sequence started and when they end. Audio thread only.
    struct Sounding
    {
        juce::int64 end;
        juce::uint8 channel, pitch;
    };

    std::array<Sounding, 256> sounding {};
    int numSounding = 0;
    bool sequenceSustain = false;
    juce::int64 nextPos = -1; // where a continuous play goes next; anything else is a jump

    std::bitset<16 * 128> liveHeld; // keys down from the controller
    std::bitset<16> liveSustain;
    bool anyLive = false;

    struct EditorWindow;
    std::unique_ptr<EditorWindow> window;
};

// The plugins installed on this machine, cached in plugins.xml in the config folder. Message
// thread.
//
// Finding out what a plugin is means loading it, and some plugins crash or hang when loaded
// cold. So scanning happens in child processes (taw --scan-plugin, a few at once): a plugin
// that crashes or doesn't answer in time only takes its child down, and is skipped from then on
// ("scan retry" tries those again).
class PluginCatalog
{
public:
    PluginCatalog();
    ~PluginCatalog();

    bool hasScanned() const { return cacheFile.existsAsFile(); }

    // Non-blocking: call scanStep every frame until it returns false.
    void startScan (bool retryFailed = false);
    bool isScanning() const { return scanning; }
    bool scanStep();
    juce::String scanningName() const;
    float scanProgress() const;
    const juce::StringArray& failedLastScan() const { return failed; } // plugin names that crashed or hung

    std::vector<juce::PluginDescription> instruments() const; // by name
    std::vector<juce::PluginDescription> effects() const;     // everything else, by name
    // Effects for a mono or a stereo track: where a plugin comes as "X Mono", "X Mono/Stereo" and
    // "X Stereo", only the ones that fit (a mono track: Mono and Mono/Stereo).
    std::vector<juce::PluginDescription> effectsFor (bool monoTrack) const;
    std::optional<juce::PluginDescription> findInstrument (const juce::String& query) const { return find (query, true); }
    std::optional<juce::PluginDescription> findEffect (const juce::String& query, bool monoTrack = false) const { return find (query, false, monoTrack); }
    std::optional<juce::PluginDescription> find (const juce::String& query, bool instrument, bool monoTrack = false) const;
    std::optional<juce::PluginDescription> byIdentifier (const juce::String& id) const;

    // channels: what an effect gets fed, 1 (a mono track) or 2. A mono one is asked for mono in
    // and mono out, else mono in and stereo out; anything else runs as it comes (fed both sides).
    std::shared_ptr<PluginSlot> create (const juce::PluginDescription&, double sampleRate, int maxBlock, juce::String& error,
                                        int channels = 2);

    // The child's side of a scan: describe the plugin(s) in one file into an XML file. Returns the
    // process exit code (0 when it found something).
    static int scanInThisProcess (const juce::String& pluginFile, const juce::File& out);

private:
    struct ScanJob;

    void save() const;
    void finish (ScanJob&, bool exited);

    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList list;
    juce::File cacheFile;

    bool scanning = false;
    juce::StringArray pending, failed;
    juce::StringArray retryAlone; // failed alongside others: one more go, on its own, at the end
    bool alonePass = false;
    std::vector<std::unique_ptr<ScanJob>> jobs;
    int total = 0, done = 0;
};
} // namespace taw
