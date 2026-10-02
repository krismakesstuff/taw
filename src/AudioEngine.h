#pragma once

#include "Midi.h"
#include "SpscQueue.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

namespace taw
{
class PluginSlot;

// Peak meter shared between the audio thread (writes) and the UI (reads and resets).
struct AtomicPeak
{
    std::atomic<float> value { 0.0f };

    void push (float v) noexcept
    {
        auto cur = value.load (std::memory_order_relaxed);
        while (v > cur && ! value.compare_exchange_weak (cur, v, std::memory_order_relaxed)) {}
    }

    float take() noexcept { return value.exchange (0.0f, std::memory_order_relaxed); }
};

// Writes one track's input to disk. Created on the message thread, fed by the audio thread.
struct Recorder
{
    static constexpr int overviewBlock = 256; // samples per waveform overview point

    int trackIndex = -1;
    int inputChannel = -1; // 0-based hardware input
    juce::File file;
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;

    std::atomic<juce::int64> startSample { -1 }; // playhead when the first sample was written
    std::atomic<juce::int64> samplesWritten { 0 };
    std::atomic<int> droppedBlocks { 0 };

    SpscQueue<float, (1 << 15)> overview; // audio -> message: one peak per overviewBlock samples

    // audio thread only
    float ovPeak = 0.0f;
    int ovCount = 0;
};

// A window into an audio file, placed on the timeline: plays audio[offset, offset + length)
// from timeline sample start. Many clips can share one file (that's how copy/paste works).
struct ClipPlay
{
    std::shared_ptr<const juce::AudioBuffer<float>> audio;
    juce::int64 start = 0, offset = 0, length = 0;
};

// Everything the audio thread needs for one track. Built from the session tree.
struct TrackPlay
{
    int inputChannel = -1;
    bool audible = true;

    // Armed tracks monitor their input (auto-input, the pro default): while stopped and while
    // recording you hear the input; during playback you hear the track's clips. inputOnly keeps
    // the input in your ears during playback too, to audition a part against the mix.
    bool armed = false;
    bool inputOnly = false;

    bool monitoringInput (bool rolling) const noexcept
    {
        return armed && (inputOnly || ! rolling || recorder != nullptr);
    }
    bool stereo = false;     // any stereo clip makes the track stereo
    float gain = 1.0f;

    // Audio tracks. In session order: where clips overlap, the later one plays ("later wins").
    // Nothing is trimmed, so removing a clip uncovers whatever was underneath.
    std::vector<ClipPlay> clips;

    // Sample tracks: the sample plays once per hit, sorted by position. Hits ring out over each
    // other (no choke).
    struct Hit
    {
        juce::int64 pos;
        float vel;
    };

    std::shared_ptr<const juce::AudioBuffer<float>> sample;
    juce::int64 sampleOffset = 0, sampleLength = 0;
    std::vector<Hit> hits;

    // Instrument tracks: a plugin playing the track's notes, plus the MIDI controller while
    // the track is armed (midiLive).
    std::shared_ptr<PluginSlot> plugin;
    Sequence sequence;
    bool midiLive = false;

    std::shared_ptr<Recorder> recorder;

    // Routing. Buses are numbered in the graph (Graph::numBuses); -1 is the mix.
    struct Send
    {
        int bus = -1;
        float gain = 1.0f;
        bool pre = false; // before the fader: the track's volume doesn't change it (mute does)
    };

    static constexpr int nowhere = -2; // out: only its sends are heard (it feeds a soloed bus)

    int busIndex = -1; // a bus track: the bus it plays
    int out = -1;      // where the track goes after its fader
    std::vector<Send> sends;
};

// Immutable snapshot of what to play. The message thread builds one and hands it over; the
// audio thread swaps it in at the start of a block and hands the old one back to be freed.
struct ClickSettings
{
    bool enabled = false;
    bool recordOnly = true; // only while something is recording
    float gain = 0.5f;
    double samplesPerBeat = 24000.0;
    int beatsPerBar = 4;
};

struct Graph
{
    static constexpr int maxBuses = 32;

    std::vector<TrackPlay> tracks;
    std::vector<int> order; // tracks in the order they run: every track before the buses it feeds
    int numBuses = 0;
    ClickSettings click;
    float mixGain = 1.0f;
    double bpm = 120.0;
    int timeSigNum = 4, timeSigDen = 4;

    bool recording = false; // loops are ignored while recording (for now)
    bool loop = false;
    juce::int64 loopStart = 0, loopEnd = 0;
    juce::uint64 serial = 0; // assigned by setGraph
};

constexpr float centrePan = 0.70710678f; // -3 dB pan law for a mono track in a stereo mix

// Stereo tracks reach the mix at unity; mono tracks are panned centre with the pan law.
inline bool isStereo (const TrackPlay& tp) { return tp.stereo; }

// Writes a track's clips for [pos, pos + n) into its left/right bus at unity (mono clips go to
// both sides). Each clip overwrites what's under it, so the later clip wins where they overlap.
// Shared by live playback and bounces so the two can't drift apart.
inline void renderClips (const TrackPlay& tp, juce::int64 pos, int n, float* left, float* right)
{
    for (const auto& c : tp.clips)
    {
        const auto from = juce::jmax (pos, c.start);
        const auto to = juce::jmin (pos + n, c.start + c.length);

        if (to <= from)
            continue;

        const auto src = (int) (c.offset + (from - c.start)), count = (int) (to - from);
        const auto lastChannel = c.audio->getNumChannels() - 1;
        juce::FloatVectorOperations::copy (left + (from - pos), c.audio->getReadPointer (0, src), count);
        juce::FloatVectorOperations::copy (right + (from - pos), c.audio->getReadPointer (juce::jmin (1, lastChannel), src), count);
    }
}

// Where a track's signal goes once it's rendered into left/right at unity: its pre-fader sends,
// its fader (applied in place, so left/right then hold what the track plays, for its meter),
// its post-fader sends, and its output. buses holds left and right for each bus (bus b at 2b and
// 2b + 1). pan is the level a mono signal enters a stereo bus at. Shared by live playback and
// bounces.
inline void routeTrack (const TrackPlay& tp, float pan, float* left, float* right, int n, float* const* buses, float* mixL, float* mixR)
{
    auto add = [&] (int bus, float g)
    {
        if (g <= 0.0f || bus == TrackPlay::nowhere)
            return;

        juce::FloatVectorOperations::addWithMultiply (bus < 0 ? mixL : buses[bus * 2], left, g, n);
        juce::FloatVectorOperations::addWithMultiply (bus < 0 ? mixR : buses[bus * 2 + 1], right, g, n);
    };

    if (tp.audible)
        for (const auto& s : tp.sends)
            if (s.pre)
                add (s.bus, s.gain * pan);

    const auto fader = tp.audible ? tp.gain : 0.0f;
    juce::FloatVectorOperations::multiply (left, fader, n);
    juce::FloatVectorOperations::multiply (right, fader, n);

    if (fader > 0.0f)
    {
        for (const auto& s : tp.sends)
            if (! s.pre)
                add (s.bus, s.gain * pan);

        add (tp.out, pan);
    }
}

// Adds a sample track's hits for [pos, pos + n) into its bus. Like clips, it depends only on
// position (no voice state), so loops, seeks and offline bounces all sound the same.
inline void renderHits (const TrackPlay& tp, juce::int64 pos, int n, float* left, float* right)
{
    if (tp.sample == nullptr || tp.sampleLength <= 0 || tp.hits.empty())
        return;

    // First hit that could still be sounding at pos.
    auto it = std::upper_bound (tp.hits.begin(), tp.hits.end(), pos - tp.sampleLength,
                                [] (juce::int64 p, const TrackPlay::Hit& h) { return p < h.pos; });
    const auto lastChannel = tp.sample->getNumChannels() - 1;

    for (; it != tp.hits.end() && it->pos < pos + n; ++it)
    {
        const auto from = juce::jmax (pos, it->pos);
        const auto to = juce::jmin (pos + n, it->pos + tp.sampleLength);

        if (to <= from)
            continue;

        const auto src = (int) (tp.sampleOffset + (from - it->pos)), count = (int) (to - from);
        juce::FloatVectorOperations::addWithMultiply (left + (from - pos), tp.sample->getReadPointer (0, src), it->vel, count);
        juce::FloatVectorOperations::addWithMultiply (right + (from - pos), tp.sample->getReadPointer (juce::jmin (1, lastChannel), src), it->vel, count);
    }
}

struct FinishedTake
{
    int trackIndex;
    juce::File file;
    juce::int64 start, length;
    int droppedBlocks;
};

class AudioEngine final : private juce::AudioIODeviceCallback
{
public:
    static constexpr int maxInputs = 64;
    static constexpr int maxTracks = 128;

    AudioEngine();
    ~AudioEngine() override;

    juce::AudioDeviceManager& devices() { return deviceManager; }
    juce::AudioFormatManager& formats() { return formatManager; }

    // message thread ------------------------------------------------------------
    void setGraph (std::unique_ptr<Graph>);
    // countIn: samples of clicks before the playhead moves (the transport counts as rolling).
    void play (juce::int64 from, juce::int64 countIn = 0);
    void stop();
    void seek (juce::int64 position);

    // Plays a sample track's sample right now, stopped or rolling: a pad press (the . key).
    // Recording the hit is the caller's business; this is only what you hear.
    void triggerPad (int trackIndex, float velocity);

    std::shared_ptr<Recorder> createRecorder (int trackIndex, int inputChannel, const juce::File&,
                                              double sampleRate, juce::String& error);
    // Recorders stop receiving audio once a graph without them is live; they are then closed.
    void finishRecorders (std::vector<std::shared_ptr<Recorder>>);

    // Frees retired graphs and returns takes whose files are now complete.
    std::vector<FinishedTake> poll();

    // any thread ----------------------------------------------------------------
    bool isRolling() const noexcept { return rolling.load(); }
    juce::int64 countInRemaining() const noexcept { return rolling.load() ? countInLeft.load() : 0; } // samples
    juce::int64 playhead() const noexcept { return playPos.load(); }

    // Where the playhead is going: a seek not yet applied by the audio thread, else the playhead.
    // Moves start from here so quick repeated key presses add up.
    juce::int64 targetPosition() const noexcept
    {
        const auto s = seekTo.load();
        return s >= 0 ? s : playPos.load();
    }
    bool isDeviceRunning() const noexcept { return deviceRunning.load(); }

    // MIDI: each input port feeds one queue (see MidiInputs). While recording, what the audio
    // thread passed on comes back with the playhead it arrived at.
    std::array<MidiQueue, MidiInputs::maxPorts>& midiInputQueues() { return midiIn; }
    bool popRecordedMidi (TimedMidi& m) noexcept { return recordedMidi.pop (m); }
    // Bumped whenever the device (re)starts: plugins then get prepared for its rate and block.
    int deviceGeneration() const noexcept { return generation.load(); }
    double deviceSampleRate() const noexcept { return liveRate.load(); }
    int maxBlockSize() const noexcept { return maxBlock.load(); }

    // Tests: run the engine the way a device would, with no device (taw --counttest).
    void runWithoutDevice (double rate, int bufferSize);
    void renderWithoutDevice (float* const* out, int numOut, int numSamples);

    float takeInputPeak (int ch) noexcept { return inputPeaks[(size_t) ch].take(); }
    float takeTrackPeak (int t) noexcept { return trackPeaks[(size_t) t].take(); }
    float takeMasterPeak (int ch) noexcept { return masterPeaks[(size_t) ch].take(); }
    int numInputChannels() const noexcept { return numInputs.load(); }

private:
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

    void startRunning (double rate, int bufferSize, int numInputs);
    void process (const float* const* in, int numIn, float* const* out, int numOut, int numSamples);
    void renderClick (const ClickSettings&, juce::int64 pos, float* left, float* right, int numSamples);

    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formatManager;
    juce::TimeSliceThread diskThread { "taw disk writer" };

    SpscQueue<Graph*, 64> pending, retired;

    // Pad presses: message thread -> audio thread, played by a few voices that ring out.
    struct PadPress
    {
        int track;
        float velocity;
    };

    struct PadVoice
    {
        int track = -1; // -1: free
        juce::int64 played = 0;
        float velocity = 0.0f;
    };

    SpscQueue<PadPress, 256> padPresses;

    std::array<MidiQueue, MidiInputs::maxPorts> midiIn;
    SpscQueue<TimedMidi, 4096> recordedMidi;
    std::array<MidiMsg, 512> liveMidi; // this block's controller messages, audio thread only
    std::atomic<int> generation { 0 }, maxBlock { 8192 };
    std::atomic<double> liveRate { 0.0 };

    std::array<PadVoice, 32> padVoices; // audio thread only
    void renderPads (const TrackPlay&, int trackIndex, float* left, float* right, int n);
    Graph* current = nullptr; // audio thread (or message thread while the device is stopped)

    // A recorder is finished once the audio thread runs a graph posted after it was stopped.
    struct Stopping
    {
        std::shared_ptr<Recorder> recorder;
        juce::uint64 afterSerial;
    };

    std::vector<Stopping> stopping;
    juce::uint64 postedSerial = 0;           // message thread
    std::atomic<juce::uint64> liveSerial { 0 }; // serial of the graph the audio thread is running

    std::atomic<bool> rolling { false }, deviceRunning { false };
    std::atomic<juce::int64> playPos { 0 }, seekTo { -1 }, countInLeft { 0 };
    std::atomic<int> numInputs { 0 };

    std::array<AtomicPeak, maxInputs> inputPeaks;
    std::array<AtomicPeak, maxTracks> trackPeaks;
    std::array<AtomicPeak, 2> masterPeaks;

    juce::AudioBuffer<float> scratch; // see audioDeviceAboutToStart
    juce::AudioBuffer<float> busScratch; // left and right for each bus
    std::array<float*, Graph::maxBuses * 2> busPtrs {};

    // Click voice, audio thread only.
    double deviceRate = 48000.0, clickPhase = 0.0, clickPhaseInc = 0.0;
    float clickLevel = 0.0f, clickDecay = 0.0f;
    int clickSamplesLeft = 0;
};
} // namespace taw
