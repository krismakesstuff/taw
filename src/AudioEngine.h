#pragma once

#include "Automation.h"
#include "Midi.h"
#include "SpscQueue.h"
#include "RenderPool.h"

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

// One waveform overview point: the lowest and highest sample over a block, both channels
// together. Both start at zero, so a block always spans the centre line.
struct WavePoint
{
    float lo = 0.0f, hi = 0.0f;
};

// Writes one track's input to disk. Created on the message thread, fed by the audio thread.
struct Recorder
{
    static constexpr int overviewBlock = 256; // samples per waveform overview point

    int trackIndex = -1;
    int inputChannel = -1; // 0-based hardware input (a stereo track's left; its right is the next one)
    int numChannels = 1;
    juce::File file;
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;

    std::atomic<juce::int64> startSample { -1 }; // playhead when the first sample was written
    std::atomic<juce::int64> samplesWritten { 0 };
    std::atomic<int> droppedBlocks { 0 };

    SpscQueue<WavePoint, (1 << 15)> overview; // audio -> message: one point per overviewBlock samples

    // audio thread only
    WavePoint ovPoint;
    int ovCount = 0;
};

// A window into an audio file, placed on the timeline: plays audio[offset, offset + length)
// from timeline sample start. Many clips can share one file (that's how copy/paste works).
struct ClipPlay
{
    std::shared_ptr<const juce::AudioBuffer<float>> audio;
    juce::int64 start = 0, offset = 0, length = 0;
};

// A delay on one route (a track's output or a send), so every path reaches the mix, or a bus,
// together however much latency the plugins on it add. The App keeps these across graph
// rebuilds, so a fader move doesn't empty them. Audio thread only, once built.
struct RouteDelay
{
    explicit RouteDelay (int samples) : length (samples), buffer (2, samples) { buffer.clear(); }

    // Adds left/right * gain into destL/destR, length samples late. Runs every block, silent or
    // not, so nothing stale comes out later.
    void addDelayed (const float* left, const float* right, float gain, float* destL, float* destR, int n) noexcept
    {
        auto* bl = buffer.getWritePointer (0);
        auto* br = buffer.getWritePointer (1);

        for (int i = 0; i < n; ++i)
        {
            destL[i] += bl[pos];
            destR[i] += br[pos];
            bl[pos] = left[i] * gain;
            br[pos] = right[i] * gain;

            if (++pos == length)
                pos = 0;
        }
    }

    // A live input (monitored, or a played instrument) goes straight through: you're playing in
    // time with what you hear, so holding it back only makes you late. What's already in the line
    // still comes out, and silence goes in behind it.
    void addLive (const float* left, const float* right, float gain, float* destL, float* destR, int n) noexcept
    {
        auto* bl = buffer.getWritePointer (0);
        auto* br = buffer.getWritePointer (1);

        for (int i = 0; i < n; ++i)
        {
            destL[i] += bl[pos] + left[i] * gain;
            destR[i] += br[pos] + right[i] * gain;
            bl[pos] = br[pos] = 0.0f;

            if (++pos == length)
                pos = 0;
        }
    }

    const int length;
    juce::AudioBuffer<float> buffer;
    int pos = 0;
};

// Everything the audio thread needs for one track. Built from the session tree.
struct TrackPlay
{
    int inputChannel = -1; // a stereo track's left input; its right is the next one
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
    bool stereo = false; // the track's width: a mono track keeps one signal (in both sides) until it's panned
    float gain = 1.0f;

    // Automation (empty curves: not automated). Volume and sends in dB, pan -1 to 1, mute 0/1.
    // Automated values overwrite gain, pan, audible and the sends' gains as the track plays.
    Curve volumeCurve, panCurve, muteCurve;
    bool soloAudible = true; // solo lets it through: mute automation decides the rest
    struct ParameterCurve
    {
        std::shared_ptr<PluginSlot> plugin;
        int index = -1;
        Curve curve;
        float last = -1.0f; // what was last sent, so a flat stretch sends nothing
    };
    std::vector<ParameterCurve> parameterCurves; // the instrument's and the inserts' parameters

    // Pan, after the inserts: a mono signal is placed with the -3 dB pan law, a stereo one
    // balanced. monoAtPan: still mono there (no insert turned it stereo).
    bool monoAtPan = false;
    float panL = 1.0f, panR = 1.0f;

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

    // Effects, in slot order (bypassed ones left out): after the source, before sends and fader.
    std::vector<std::shared_ptr<PluginSlot>> inserts;

    std::shared_ptr<Recorder> recorder;

    // Routing. Buses are numbered in the graph (Graph::numBuses); -1 is the mix.
    struct Send
    {
        int bus = -1;
        float gain = 1.0f;
        bool pre = false; // before the fader: the track's volume doesn't change it (mute does)
        Curve curve;      // its level automation, in dB
        std::shared_ptr<RouteDelay> delay; // delay compensation, when this route needs it
    };

    static constexpr int nowhere = -2; // out: only its sends are heard (it feeds a soloed bus)

    int busIndex = -1; // a bus track: the bus it plays
    int out = -1;      // where the track goes after its fader
    std::shared_ptr<RouteDelay> outDelay;
    std::vector<Send> sends;
    int latency = 0; // samples its plugins (instrument and inserts) delay it by
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
    Curve mixCurve; // the mix volume's automation, in dB
    // The mix bus's own inserts (A-E, bypassed ones left out), their automated parameters and
    // latency (counted in latency). Only inserts, parameterCurves and latency are used.
    TrackPlay mixBus;
    double bpm = 120.0;
    int timeSigNum = 4, timeSigDen = 4;

    int latency = 0; // delay compensation: how late everything reaches the mix, in samples

    bool recording = false; // looping while recording: each pass is a take (App splits them)
    bool loop = false;
    juce::int64 loopStart = 0, loopEnd = 0;
    juce::uint64 serial = 0; // assigned by setGraph
};

constexpr float centrePan = 0.70710678f; // -3 dB pan law for a mono track in a stereo mix

// pan -1 (left) to 1 (right). Mono: the -3 dB pan law (each side 0.707 at centre, the near side
// at unity when hard over). Stereo: balance (centre leaves both sides alone).
inline void setPan (TrackPlay& tp, float pan)
{
    pan = juce::jlimit (-1.0f, 1.0f, pan);

    if (tp.monoAtPan)
    {
        const auto angle = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        tp.panL = std::cos (angle);
        tp.panR = std::sin (angle);
    }
    else
    {
        tp.panL = pan > 0.0f ? 1.0f - pan : 1.0f;
        tp.panR = pan < 0.0f ? 1.0f + pan : 1.0f;
    }
}

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

// Pan, in place: a mono track (whose signal is in both sides) is placed with the pan law; a
// stereo one is balanced. Centre: a mono track at -3 dB each side, a stereo one untouched.
inline void panTrack (const TrackPlay& tp, float* left, float* right, int n)
{
    juce::FloatVectorOperations::multiply (left, tp.panL, n);
    juce::FloatVectorOperations::multiply (right, tp.panR, n);
}

// Where a track's signal goes once it's rendered and panned: its pre-fader sends, its fader
// (applied in place, so left/right then hold what the track plays, for its meter), its
// post-fader sends, and its output, each through its delay compensation if it has one (unless
// live: a monitored input or a played instrument skips it). buses holds left and right for each
// bus (bus b at 2b and 2b + 1). Shared by live playback and bounces.
inline void routeTrack (const TrackPlay& tp, float* left, float* right, int n, float* const* buses, float* mixL, float* mixR,
                        bool live = false)
{
    auto add = [&] (int bus, float g, RouteDelay* delay)
    {
        if (bus == TrackPlay::nowhere)
            return;

        auto* dl = bus < 0 ? mixL : buses[bus * 2];
        auto* dr = bus < 0 ? mixR : buses[bus * 2 + 1];

        if (delay != nullptr && live)
            delay->addLive (left, right, g, dl, dr, n);
        else if (delay != nullptr)
            delay->addDelayed (left, right, g, dl, dr, n); // silent or not, so it stays in step
        else if (g > 0.0f)
        {
            juce::FloatVectorOperations::addWithMultiply (dl, left, g, n);
            juce::FloatVectorOperations::addWithMultiply (dr, right, g, n);
        }
    };

    for (const auto& s : tp.sends)
        if (s.pre)
            add (s.bus, tp.audible ? s.gain : 0.0f, s.delay.get());

    const auto fader = tp.audible ? tp.gain : 0.0f;
    juce::FloatVectorOperations::multiply (left, fader, n);
    juce::FloatVectorOperations::multiply (right, fader, n);

    for (const auto& s : tp.sends)
        if (! s.pre)
            add (s.bus, s.gain, s.delay.get());

    add (tp.out, 1.0f, tp.outDelay.get());
}

// A mono track plays one signal: a stereo clip on it is summed (each side at half).
inline void toMono (const TrackPlay& tp, float* left, float* right, int n)
{
    if (tp.stereo)
        return;

    juce::FloatVectorOperations::add (left, right, n);
    juce::FloatVectorOperations::multiply (left, 0.5f, n);
    juce::FloatVectorOperations::copy (right, left, n);
}

// Automation --------------------------------------------------------------------------------
inline bool mixerAutomated (const TrackPlay& tp)
{
    if (! tp.volumeCurve.empty() || ! tp.panCurve.empty() || ! tp.muteCurve.empty())
        return true;

    for (const auto& s : tp.sends)
        if (! s.curve.empty())
            return true;

    return false;
}

// The track's fader, pan, mute and sends as their automation has them at pos.
inline void automateMixer (TrackPlay& tp, juce::int64 pos)
{
    if (! tp.volumeCurve.empty())
        tp.gain = juce::Decibels::decibelsToGain (tp.volumeCurve.at (pos), -96.0f);

    if (! tp.panCurve.empty())
        setPan (tp, tp.panCurve.at (pos));

    if (! tp.muteCurve.empty())
        tp.audible = tp.soloAudible && tp.muteCurve.at (pos) < 0.5f;

    for (auto& s : tp.sends)
        if (! s.curve.empty())
            s.gain = juce::Decibels::decibelsToGain (s.curve.at (pos), -96.0f);
}

// The instrument's and effects' automated parameters, as they are at pos (sent only when they
// change). Before the plugins run for the block. In AudioEngine.cpp.
void automatePlugins (TrackPlay&, juce::int64 pos);

// Pan, sends, fader and output (panTrack then routeTrack), with the mixer's automation: an
// automated track goes in steps of automationStep samples, each at its start's values, so a
// ride is smooth however big the block. Shared by live playback and bounces.
inline void mixTrack (TrackPlay& tp, juce::int64 pos, float* left, float* right, int n, float* const* buses, int numBuses,
                      float* mixL, float* mixR, bool live = false)
{
    if (! mixerAutomated (tp))
    {
        panTrack (tp, left, right, n);
        routeTrack (tp, left, right, n, buses, mixL, mixR, live);
        return;
    }

    std::array<float*, Graph::maxBuses * 2> from {};

    for (int done = 0; done < n; done += automationStep)
    {
        const int len = juce::jmin (automationStep, n - done);
        automateMixer (tp, pos + done);

        for (int b = 0; b < juce::jmin (numBuses, Graph::maxBuses) * 2; ++b)
            from[(size_t) b] = buses[b] + done;

        panTrack (tp, left + done, right + done, len);
        routeTrack (tp, left + done, right + done, len, from.data(), mixL + done, mixR + done, live);
    }
}

// The mix volume (with its automation) on the mix bus.
inline void applyMixGain (const Graph& g, juce::int64 pos, float* left, float* right, int n)
{
    if (g.mixCurve.empty())
    {
        juce::FloatVectorOperations::multiply (left, g.mixGain, n);
        juce::FloatVectorOperations::multiply (right, g.mixGain, n);
        return;
    }

    for (int done = 0; done < n; done += automationStep)
    {
        const int len = juce::jmin (automationStep, n - done);
        const auto gain = juce::Decibels::decibelsToGain (g.mixCurve.at (pos + done), -96.0f);
        juce::FloatVectorOperations::multiply (left + done, gain, len);
        juce::FloatVectorOperations::multiply (right + done, gain, len);
    }
}

// The mix bus once every track is in it: the mix volume (and its automation), then the mix's
// inserts, after the fader as on a Pro Tools master. Shared by live playback and bounces. In
// AudioEngine.cpp.
void mixInserts (Graph&, juce::int64 pos, const HostTransport&, float* left, float* right, int n);

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

    // The sample browser's audition: plays this (already at the device's rate) straight to the
    // outputs, stopped or rolling, cutting off whatever it played before; null just stops it.
    void preview (std::shared_ptr<const juce::AudioBuffer<float>>, float gain = 0.7f, int startSample = 0);
    int previewPosition() const noexcept { return previewAt.load(); } // where it's playing; -1 when it isn't

    // numChannels 2: a stereo track, recording inputChannel and the one after it.
    std::shared_ptr<Recorder> createRecorder (int trackIndex, int inputChannel, int numChannels, const juce::File&,
                                              double sampleRate, juce::String& error);
    // Recorders stop receiving audio once a graph without them is live; they are then closed.
    void finishRecorders (std::vector<std::shared_ptr<Recorder>>);

    // Frees retired graphs and returns takes whose files are now complete.
    std::vector<FinishedTake> poll();

    // any thread ----------------------------------------------------------------
    bool isRolling() const noexcept { return rolling.load(); }
    juce::int64 countInRemaining() const noexcept { return rolling.load() ? countInLeft.load() : 0; } // samples
    juce::int64 playhead() const noexcept { return playPos.load(); }
    int loopWrapCount() const noexcept { return loopWraps.load(); } // times the loop wrapped since play started

    // Where the playhead is going: a seek not yet applied by the audio thread, else the playhead.
    // Moves start from here so quick repeated key presses add up.
    juce::int64 targetPosition() const noexcept
    {
        const auto s = seekTo.load();
        return s >= 0 ? s : playPos.load();
    }
    bool isDeviceRunning() const noexcept { return deviceRunning.load(); }

    // Blocks that took longer to render than they last, while rolling (each is a dropout on taw's
    // side), and the worst block's time as a share of its length (1 = right at the limit).
    int lateBlockCount() const noexcept { return lateBlocks.load(); }
    float worstBlockLoad() const noexcept { return worstLoad.load(); }
    void resetBlockLoad() noexcept { lateBlocks.store (0); worstLoad.store (0.0f); }

    // Where a block's time goes (taw --loadtest): each track's share of the last block, in ms,
    // and the mix's inserts. Off unless a test turns it on; read it between blocks.
    // Load as you work (the stats panel, the transport's dsp): how much of each block's time
    // the audio takes, smoothed over about half a second, and the highest in the last few
    // seconds; each track's time (ms per block) likewise. Measured stopped too: plugins run.
    float dspLoad() const noexcept { return loadAverage.load (std::memory_order_relaxed); }
    float dspPeak() const noexcept { return loadPeak.load (std::memory_order_relaxed); }
    float trackMs (int t) const noexcept { return t >= 0 && t < maxTracks ? trackAverage[(size_t) t].load (std::memory_order_relaxed) : 0.0f; }
    float trackPeakMs (int t) const noexcept { return t >= 0 && t < maxTracks ? trackPeak[(size_t) t].load (std::memory_order_relaxed) : 0.0f; }
    float mixMs() const noexcept { return mixAverage.load (std::memory_order_relaxed); }
    double blockMs() const noexcept { return lastBlockMs.load (std::memory_order_relaxed); } // how long a block lasts
    int helperThreads() const noexcept { return pool != nullptr ? pool->numWorkers() : 0; }

    bool profiling = false;
    bool parallel = true; // tracks on several cores (taw --paralleltest compares it with one)
    std::array<double, 256> profileTrackMs {};
    double profileMixMs = 0.0;

    // MIDI: each input port feeds one queue (see MidiInputs). While recording, what the audio
    // thread passed on comes back with the playhead it arrived at.
    std::array<MidiQueue, MidiInputs::maxPorts>& midiInputQueues() { return midiIn; }
    bool popRecordedMidi (TimedMidi& m) noexcept { return recordedMidi.pop (m); }
    // A message straight to a track's instrument, armed or not, never recorded: the MIDI editor
    // auditioning a note. Its note-offs are the sender's to send.
    void audition (int trackIndex, MidiMsg m) noexcept { auditions.push ({ trackIndex, m }); }
    // The typing keyboard's notes (keys mode), as if from one more controller: armed tracks
    // play them, and recording writes them down. Message thread only.
    void playTyped (MidiMsg m) noexcept { typedMidi.push (m); }
    // Bumped whenever the device (re)starts: plugins then get prepared for its rate and block.
    int deviceGeneration() const noexcept { return generation.load(); }
    double deviceSampleRate() const noexcept { return liveRate.load(); }
    int maxBlockSize() const noexcept { return maxBlock.load(); }

    // Tests: run the engine the way a device would, with no device (taw --counttest).
    void runWithoutDevice (double rate, int bufferSize, int numInputs = 0);
    void renderWithoutDevice (float* const* out, int numOut, int numSamples, const float* const* in = nullptr, int numIn = 0);

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

    struct PreviewSound { std::shared_ptr<const juce::AudioBuffer<float>> audio; float gain = 0.7f; int start = 0; };
    SpscQueue<PreviewSound*, 32> previewPending, previewRetired; // deleted on the message thread
    PreviewSound* previewNow = nullptr; // audio thread only
    int previewPos = 0;
    std::atomic<int> previewAt { -1 };
    void renderPreview (float* const* out, int numOut, int n) noexcept;

    std::array<MidiQueue, MidiInputs::maxPorts> midiIn;
    MidiQueue typedMidi; // keys mode's notes (playTyped)
    SpscQueue<TimedMidi, 4096> recordedMidi;
    std::array<MidiMsg, 512> liveMidi; // this block's controller messages, audio thread only

    struct Audition
    {
        int track;
        MidiMsg msg;
    };
    SpscQueue<Audition, 256> auditions;
    std::array<Audition, 64> blockAuditions;         // this block's, audio thread only
    static constexpr int mergedMidiSize = 512 + 64;
    std::vector<MidiMsg> mergedMidi;                   // per track (they render in parallel): its live messages plus its auditions
    std::array<int, maxTracks> auditionsHeld {};      // notes on without their off yet, per track
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
    std::atomic<int> lateBlocks { 0 };
    std::atomic<float> loadAverage { 0.0f }, loadPeak { 0.0f }, mixAverage { 0.0f };
    std::atomic<double> lastBlockMs { 0.0 };
    std::array<std::atomic<float>, maxTracks> trackAverage {}, trackPeak {};
    static void smooth (std::atomic<float>& average, std::atomic<float>* peak, float value, double blockSeconds) noexcept;
    std::atomic<float> worstLoad { 0.0f };
    std::atomic<juce::int64> playPos { 0 }, seekTo { -1 }, countInLeft { 0 };
    std::atomic<int> loopWraps { 0 };
    std::atomic<int> numInputs { 0 };

    std::array<AtomicPeak, maxInputs> inputPeaks;
    std::array<AtomicPeak, maxTracks> trackPeaks;
    std::array<AtomicPeak, 2> masterPeaks;

    juce::AudioBuffer<float> scratch; // see audioDeviceAboutToStart
    juce::AudioBuffer<float> trackScratch; // left and right for each track: they render in parallel, then mix in order

    // A block's tracks render on several cores (RenderPool): each into its own buffers, then
    // they're mixed one after another, buses after what feeds them.
    struct Block
    {
        const float* const* in;
        int numIn, n, numLive, numAuditions, numBuses;
        juce::int64 pos;
        bool isRolling;
        HostTransport transport;
    };
    Block block {};
    std::array<int, maxTracks> parallelTracks {};
    void renderTrack (int t, const Block&) noexcept; // its clips, plugin and inserts, into its buffers
    void finishTrack (int t, const Block&, float* mixL, float* mixR, const float* silence) noexcept; // mixed, metered, recorded
    std::unique_ptr<RenderPool> pool;
    int poolWorkers = 0;                  // one per spare core, at most 7
    juce::AudioWorkgroup deviceWorkgroup; // the device's, for the pool's workers (macOS)
    juce::AudioBuffer<float> busScratch; // left and right for each bus
    std::array<float*, Graph::maxBuses * 2> busPtrs {};

    // Click voice, audio thread only.
    double deviceRate = 48000.0, clickPhase = 0.0, clickPhaseInc = 0.0;
    float clickLevel = 0.0f, clickDecay = 0.0f;
    int clickSamplesLeft = 0;
};
} // namespace taw
