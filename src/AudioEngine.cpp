#include "AudioEngine.h"
#include "Plugins.h"

namespace taw
{
void automatePlugins (TrackPlay& tp, juce::int64 pos)
{
    for (auto& p : tp.parameterCurves)
    {
        // A mapped parameter with no points, untouched: the plugin's own setting stands.
        if (! p.curve.active())
        {
            p.last = -1.0f;
            continue;
        }

        const auto value = juce::jlimit (0.0f, 1.0f, p.curve.at (pos));

        if (value != p.last)
        {
            p.plugin->setParameter (p.index, value);
            p.last = value;
        }
    }
}

void mixInserts (Graph& g, juce::int64 pos, const HostTransport& transport, float* left, float* right, int n)
{
    // What reaches the mix at pos left the timeline the tracks' delay compensation earlier; the
    // mix's own inserts add theirs after that (both are in g.latency).
    const auto at = pos - (g.latency - g.mixBus.latency);
    applyMixGain (g, at, left, right, n);

    if (g.mixBus.inserts.empty())
        return;

    automatePlugins (g.mixBus, at);

    for (const auto& fx : g.mixBus.inserts)
        fx->process (transport, n, left, right);
}

AudioEngine::AudioEngine()
{
    formatManager.registerBasicFormats();
    diskThread.startThread (juce::Thread::Priority::high);
    mergedMidi.resize ((size_t) maxTracks * mergedMidiSize);

    // Helpers for the audio thread, one per spare core (the audio thread is the other). They
    // start again with each device, for its timing (startRunning).
    pool = std::make_unique<RenderPool>();
    poolWorkers = juce::jlimit (0, 7, juce::SystemStats::getNumPhysicalCpus() - 1);
    pool->start (poolWorkers);

    deviceManager.addAudioCallback (this);
}

AudioEngine::~AudioEngine()
{
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();

    Graph* g = nullptr;

    while (pending.pop (g))
        delete g;

    while (retired.pop (g))
        delete g;

    delete current;
    current = nullptr;

    for (PreviewSound* p = nullptr; previewPending.pop (p) || previewRetired.pop (p);)
        delete p;

    delete previewNow;

    stopping.clear();
    diskThread.stopThread (2000);
}

//==============================================================================
void AudioEngine::setGraph (std::unique_ptr<Graph> g)
{
    g->serial = ++postedSerial;

    if (! deviceRunning.load())
    {
        // Nothing is calling back, so it's safe to swap here.
        delete current;
        current = g.release();
        liveSerial.store (current->serial);
        return;
    }

    if (! pending.push (g.get()))
    {
        jassertfalse; // the audio thread has stalled
        return;
    }

    g.release();
}

void AudioEngine::triggerPad (int trackIndex, float velocity)
{
    padPresses.push ({ trackIndex, velocity }); // a full queue drops the press
}

void AudioEngine::preview (std::shared_ptr<const juce::AudioBuffer<float>> audio, float gain, int startSample)
{
    for (PreviewSound* p = nullptr; previewRetired.pop (p);)
        delete p;

    auto* p = new PreviewSound { std::move (audio), gain, juce::jmax (0, startSample) };

    if (! previewPending.push (p))
        delete p; // the audio thread is behind; this one's skipped
}

// Added to the outputs after the mix (not metered, not bounced).
void AudioEngine::renderPreview (float* const* out, int numOut, int n) noexcept
{
    for (PreviewSound* p = nullptr; previewPending.pop (p);)
    {
        if (previewNow != nullptr && ! previewRetired.push (previewNow))
            jassertfalse; // leaked rather than freed here

        previewNow = p;
        previewPos = p->start;
    }

    if (previewNow == nullptr || previewNow->audio == nullptr || previewPos >= previewNow->audio->getNumSamples())
    {
        previewAt.store (-1);
        return;
    }

    const auto& a = *previewNow->audio;
    const int count = juce::jmin (n, a.getNumSamples() - previewPos);

    for (int ch = 0; ch < juce::jmin (numOut, 2) && count > 0; ++ch)
        if (out[ch] != nullptr)
            juce::FloatVectorOperations::addWithMultiply (out[ch], a.getReadPointer (juce::jmin (ch, a.getNumChannels() - 1), previewPos), previewNow->gain, count);

    previewPos += juce::jmax (0, count);
    previewAt.store (previewPos);
}

// Live pad voices for one track, added into its bus. Like hits, they ring out over each other.
void AudioEngine::renderPads (const TrackPlay& tp, int trackIndex, float* left, float* right, int n)
{
    for (auto& v : padVoices)
    {
        if (v.track != trackIndex)
            continue;

        if (tp.sample == nullptr || v.played >= tp.sampleLength)
        {
            v.track = -1;
            continue;
        }

        const auto count = (int) juce::jmin<juce::int64> (n, tp.sampleLength - v.played);
        const auto src = (int) (tp.sampleOffset + v.played);
        const auto lastChannel = tp.sample->getNumChannels() - 1;
        juce::FloatVectorOperations::addWithMultiply (left, tp.sample->getReadPointer (0, src), v.velocity, count);
        juce::FloatVectorOperations::addWithMultiply (right, tp.sample->getReadPointer (juce::jmin (1, lastChannel), src), v.velocity, count);
        v.played += count;
    }
}

void AudioEngine::play (juce::int64 from, juce::int64 countIn)
{
    resetBlockLoad(); // counted per pass
    playPos.store (juce::jmax<juce::int64> (0, from));
    seekTo.store (-1);
    countInLeft.store (juce::jmax<juce::int64> (0, countIn));
    rolling.store (true);
}

void AudioEngine::stop()
{
    rolling.store (false);
    countInLeft.store (0);
}

void AudioEngine::seek (juce::int64 position)
{
    position = juce::jmax<juce::int64> (0, position);

    if (rolling.load())
        seekTo.store (position);
    else
        playPos.store (position);
}

std::shared_ptr<Recorder> AudioEngine::createRecorder (int trackIndex, int inputChannel, int numChannels, const juce::File& file,
                                                       double sampleRate, juce::String& error)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

    if (static_cast<juce::FileOutputStream*> (stream.get())->failedToOpen())
    {
        error = "can't write " + file.getFullPathName();
        return {};
    }

    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                   .withSampleRate (sampleRate)
                                                   .withNumChannels (numChannels)
                                                   .withBitsPerSample (32)
                                                   .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));

    if (writer == nullptr)
    {
        error = "can't create WAV writer";
        return {};
    }

    auto r = std::make_shared<Recorder>();
    r->trackIndex = trackIndex;
    r->inputChannel = inputChannel;
    r->numChannels = numChannels;
    r->file = file;
    // Four seconds of headroom between the audio thread and the disk.
    r->writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (writer.release(), diskThread,
                                                                           (int) (sampleRate * 4));
    return r;
}

void AudioEngine::finishRecorders (std::vector<std::shared_ptr<Recorder>> recs)
{
    // Call after posting the graph that no longer contains these recorders.
    for (auto& r : recs)
        stopping.push_back ({ std::move (r), postedSerial });
}

std::vector<FinishedTake> AudioEngine::poll()
{
    Graph* g = nullptr;

    if (! deviceRunning.load())
    {
        while (pending.pop (g))
        {
            delete current;
            current = g;
            liveSerial.store (g->serial);
        }
    }

    while (retired.pop (g))
        delete g;

    std::vector<FinishedTake> done;

    for (auto it = stopping.begin(); it != stopping.end();)
    {
        // The audio thread has moved past every graph that could write to it.
        if (liveSerial.load() >= it->afterSerial)
        {
            auto& r = it->recorder;
            r->writer.reset(); // flushes and closes the file
            done.push_back ({ r->trackIndex, r->file, r->startSample.load(), r->samplesWritten.load(),
                              r->droppedBlocks.load() });
            it = stopping.erase (it);
        }
        else
        {
            ++it;
        }
    }

    return done;
}

//==============================================================================
void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    deviceWorkgroup = device->getWorkgroup(); // macOS: the render pool's workers join it
    startRunning (device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples(), device->getActiveInputChannels().countNumberOfSetBits());
}

void AudioEngine::runWithoutDevice (double rate, int bufferSize, int inputs)
{
    deviceWorkgroup = {};
    startRunning (rate, bufferSize, inputs);
}

void AudioEngine::renderWithoutDevice (float* const* out, int numOut, int numSamples, const float* const* in, int numIn)
{
    audioDeviceIOCallbackWithContext (in, numIn, out, numOut, numSamples, {});
}

void AudioEngine::startRunning (double rate, int bufferSize, int inputs)
{
    // The workers again, for this device's timing (the audio callback isn't running yet). A
    // workgroup can say how many threads it takes at once, the audio thread among them.
#if JUCE_MAC
    int workers = poolWorkers;

    if (const auto most = deviceWorkgroup ? (int) deviceWorkgroup.getMaxParallelThreadCount() : 0; most > 0)
        workers = juce::jmin (workers, most - 1);

    pool->start (workers, { rate, bufferSize, deviceWorkgroup });
#endif

    const auto blockSize = juce::jmax (bufferSize * 2, 8192);
    scratch.setSize (5, blockSize); // [0] (unused), [1] silence, [2] mix L, [3] mix R, [4] (unused)
    scratch.clear();
    trackScratch.setSize (maxTracks * 2, blockSize);
    busScratch.setSize (Graph::maxBuses * 2, blockSize);

    for (int ch = 0; ch < Graph::maxBuses * 2; ++ch)
        busPtrs[(size_t) ch] = busScratch.getWritePointer (ch);

    deviceRate = rate;
    clickSamplesLeft = 0;
    numInputs.store (inputs);
    maxBlock.store (blockSize);
    liveRate.store (deviceRate);
    generation.fetch_add (1);
    deviceRunning.store (true);
}

void AudioEngine::audioDeviceStopped()
{
    deviceRunning.store (false);
    rolling.store (false);
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                                    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    juce::ScopedNoDenormals noDenormals;

    // How long this block took against how long it lasts: one that ran over is a dropout (a
    // click), counted while rolling so it can be told apart from one the driver chain made.
    struct Timer
    {
        AudioEngine& e;
        int n;
        juce::int64 start = juce::Time::getHighResolutionTicks();

        ~Timer()
        {
            if (e.deviceRate <= 0.0 || n <= 0)
                return;

            const auto load = (float) (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start) * e.deviceRate / n);

            const auto seconds = (double) n / e.deviceRate;
            e.lastBlockMs.store (seconds * 1000.0, std::memory_order_relaxed);
            smooth (e.loadAverage, &e.loadPeak, load, seconds);

            if (e.rolling.load (std::memory_order_relaxed))
            {
                if (load > 1.0f)
                    e.lateBlocks.fetch_add (1, std::memory_order_relaxed);

                if (load > e.worstLoad.load (std::memory_order_relaxed))
                    e.worstLoad.store (load, std::memory_order_relaxed);
            }
        }
    } timer { *this, numSamples };

    for (int ch = 0; ch < numOut; ++ch)
        if (out[ch] != nullptr)
            juce::FloatVectorOperations::clear (out[ch], numSamples);

    for (Graph* g = nullptr; pending.pop (g);)
    {
        if (current != nullptr && ! retired.push (current))
            jassertfalse; // leaked rather than freed on the audio thread

        current = g;
        liveSerial.store (g->serial, std::memory_order_release);
    }

    if (const auto s = seekTo.exchange (-1); s >= 0)
        playPos.store (s);

    // Split the block where it has to be: hosts may hand us more than announced (split rather
    // than allocate), and a loop wraps at an exact sample.
    for (int offset = 0; offset < numSamples;)
    {
        auto n = juce::jmin (numSamples - offset, scratch.getNumSamples());

        // A count-in ends at an exact sample: the rest of the block rolls.
        if (const auto counting = countInLeft.load(); counting > 0 && rolling.load())
            n = (int) juce::jmin<juce::int64> (n, counting);

        const auto pos = playPos.load();
        const bool looping = current != nullptr && current->loop && ! current->recording && rolling.load()
                             && current->loopEnd > current->loopStart && pos < current->loopEnd;

        if (looping && pos + n > current->loopEnd)
            n = (int) (current->loopEnd - pos);

        const float* inPtrs[maxInputs] {};
        float* outPtrs[maxInputs] {};

        for (int ch = 0; ch < juce::jmin (numIn, maxInputs); ++ch)
            inPtrs[ch] = in[ch] != nullptr ? in[ch] + offset : nullptr;

        for (int ch = 0; ch < juce::jmin (numOut, maxInputs); ++ch)
            outPtrs[ch] = out[ch] != nullptr ? out[ch] + offset : nullptr;

        process (inPtrs, juce::jmin (numIn, maxInputs), outPtrs, juce::jmin (numOut, maxInputs), n);
        offset += n;

        if (looping && playPos.load() >= current->loopEnd)
            playPos.store (current->loopStart);
    }

    renderPreview (out, numOut, numSamples);
}

// One track's own sound for this block, into its buffers: its input (monitoring) or its clips
// and hits, pads, its instrument, its inserts. Runs on any core, alongside other tracks; it
// touches only this track's things.
// Averaged over about half a second; the peak falls away over about three.
void AudioEngine::smooth (std::atomic<float>& average, std::atomic<float>* peak, float value, double blockSeconds) noexcept
{
    const auto a = (float) juce::jlimit (0.0, 1.0, blockSeconds / 0.5);
    const auto avg = average.load (std::memory_order_relaxed);
    average.store (avg + (value - avg) * a, std::memory_order_relaxed);

    if (peak != nullptr)
    {
        const auto fall = (float) std::pow (0.05, blockSeconds / 3.0); // to 5% in 3 s
        peak->store (juce::jmax (value, peak->load (std::memory_order_relaxed) * fall), std::memory_order_relaxed);
    }
}

void AudioEngine::renderTrack (int t, const Block& b) noexcept
{
    const auto from = juce::Time::getHighResolutionTicks();
    auto& tp = current->tracks[(size_t) t];
    const auto n = b.n;
    auto* trackL = trackScratch.getWritePointer (t * 2);
    auto* trackR = trackScratch.getWritePointer (t * 2 + 1);
    auto inputAt = [&] (int ch) { return ch >= 0 && ch < b.numIn ? b.in[ch] : nullptr; };
    const float* input = inputAt (tp.inputChannel);
    const float* inputR = tp.stereo ? inputAt (tp.inputChannel + 1) : input; // a stereo track's right input

    juce::FloatVectorOperations::clear (trackL, n);
    juce::FloatVectorOperations::clear (trackR, n);

    // A track either monitors its input or plays its lane, never both.
    if (tp.monitoringInput (b.isRolling))
    {
        if (input != nullptr)
            juce::FloatVectorOperations::copy (trackL, input, n);

        if (inputR != nullptr)
            juce::FloatVectorOperations::copy (trackR, inputR, n);
    }
    else if (b.isRolling)
    {
        renderClips (tp, b.pos, n, trackL, trackR);
        renderHits (tp, b.pos, n, trackL, trackR);
        toMono (tp, trackL, trackR, n);
    }

    // A bus plays what was sent to it, stopped or rolling (an input monitored into it, a reverb tail).
    if (tp.busIndex >= 0 && tp.busIndex < b.numBuses)
    {
        juce::FloatVectorOperations::copy (trackL, busPtrs[(size_t) tp.busIndex * 2], n);
        juce::FloatVectorOperations::copy (trackR, busPtrs[(size_t) tp.busIndex * 2 + 1], n);
    }

    renderPads (tp, t, trackL, trackR, n);
    automatePlugins (tp, b.pos);

    if (tp.plugin != nullptr)
    {
        const MidiMsg* liveIn = tp.midiLive ? liveMidi.data() : nullptr;
        int numLiveIn = tp.midiLive ? b.numLive : 0;

        // Auditioned notes join the controller's. While one is down the track keeps a live
        // input (even an empty one), or the plugin would let go of it at the next block.
        if (b.numAuditions > 0 || auditionsHeld[(size_t) t] > 0)
        {
            auto* merged = mergedMidi.data() + (size_t) t * mergedMidiSize;
            int m = 0;

            for (int i = 0; i < numLiveIn && m < mergedMidiSize; ++i)
                merged[m++] = liveIn[i];

            for (int i = 0; i < b.numAuditions && m < mergedMidiSize; ++i)
                if (blockAuditions[(size_t) i].track == t)
                    merged[m++] = blockAuditions[(size_t) i].msg;

            if (m > 0 || auditionsHeld[(size_t) t] > 0)
            {
                liveIn = merged;
                numLiveIn = m;
            }
        }

        tp.plugin->render (tp.sequence, b.transport, n, liveIn, numLiveIn, trackL, trackR);
    }

    // Effects run stopped too, so reverb tails ring out and monitored inputs go through them.
    for (const auto& fx : tp.inserts)
        fx->process (b.transport, n, trackL, trackR);

    const auto ms = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - from) * 1000.0;

    if (t < maxTracks)
        smooth (trackAverage[(size_t) t], &trackPeak[(size_t) t], (float) ms, (double) n / deviceRate);

    if (profiling)
        profileTrackMs[(size_t) t] = ms;
}

// The track's buffers into its bus or the mix (fader, pan, sends), its meter, and what it's
// recording. In order, one track at a time.
void AudioEngine::finishTrack (int t, const Block& b, float* mixL, float* mixR, const float* silence) noexcept
{
    auto& tp = current->tracks[(size_t) t];
    const auto n = b.n;
    auto* trackL = trackScratch.getWritePointer (t * 2);
    auto* trackR = trackScratch.getWritePointer (t * 2 + 1);
    auto inputAt = [&] (int ch) { return ch >= 0 && ch < b.numIn ? b.in[ch] : nullptr; };
    const float* input = inputAt (tp.inputChannel);
    const float* inputR = tp.stereo ? inputAt (tp.inputChannel + 1) : input;
    const bool monitoring = tp.monitoringInput (b.isRolling);

    // Live: what you're playing right now (a monitored input, an armed instrument while stopped
    // or recording) skips delay compensation.
    const bool live = monitoring || (tp.midiLive && (! b.isRolling || current->recording));
    mixTrack (tp, b.pos, trackL, trackR, n, busPtrs.data(), b.numBuses, mixL, mixR, live);

    // The meter shows what you hear from the track: the input (pre-fader, for setting
    // levels) while monitoring, otherwise its playback.
    if (monitoring && input != nullptr)
    {
        const auto range = juce::FloatVectorOperations::findMinAndMax (input, n);
        trackPeaks[(size_t) t].push (juce::jmax (range.getEnd(), -range.getStart()));
    }
    else
    {
        const auto l = juce::FloatVectorOperations::findMinAndMax (trackL, n);
        const auto r = juce::FloatVectorOperations::findMinAndMax (trackR, n);
        trackPeaks[(size_t) t].push (juce::jmax (l.getEnd(), -l.getStart(), r.getEnd(), -r.getStart()));
    }

    if (b.isRolling && tp.recorder != nullptr)
    {
        auto& rec = *tp.recorder;
        const float* srcs[2] = { input != nullptr ? input : silence, inputR != nullptr ? inputR : silence };

        if (rec.startSample.load (std::memory_order_relaxed) < 0)
            rec.startSample.store (b.pos);

        if (! rec.writer->write (srcs, n))
            rec.droppedBlocks.fetch_add (1);

        rec.samplesWritten.fetch_add (n);

        for (int i = 0; i < n; ++i)
        {
            const auto l = srcs[0][i], r = rec.numChannels > 1 ? srcs[1][i] : l;
            rec.ovPoint.lo = juce::jmin (rec.ovPoint.lo, l, r);
            rec.ovPoint.hi = juce::jmax (rec.ovPoint.hi, l, r);

            if (++rec.ovCount == Recorder::overviewBlock)
            {
                rec.overview.push (rec.ovPoint);
                rec.ovPoint = {};
                rec.ovCount = 0;
            }
        }
    }
}

void AudioEngine::process (const float* const* in, int numIn, float* const* out, int numOut, int n)
{
    for (int ch = 0; ch < numIn; ++ch)
        if (in[ch] != nullptr)
        {
            const auto range = juce::FloatVectorOperations::findMinAndMax (in[ch], n);
            inputPeaks[(size_t) ch].push (juce::jmax (range.getEnd(), -range.getStart()));
        }

    if (current == nullptr)
        return;

    // Counting in: the transport is started but holds still, so tracks behave as stopped (armed
    // ones monitor their input) and only the count-in clicks play.
    const auto counting = rolling.load() ? countInLeft.load() : (juce::int64) 0;
    const bool isRolling = rolling.load() && counting <= 0;
    const auto pos = playPos.load();

    // New pad presses take a free voice (or the oldest-started one when all 32 are ringing).
    for (PadPress press; padPresses.pop (press);)
    {
        auto* voice = &padVoices[0];

        for (auto& v : padVoices)
        {
            if (v.track < 0) { voice = &v; break; }
            if (v.played > voice->played) voice = &v;
        }

        *voice = { press.track, 0, press.velocity };
    }

    // The controller's messages for this block. They go to armed instrument tracks and, while
    // recording, back to the message thread to be written down.
    int numLive = 0;

    auto takeLive = [&] (MidiQueue& q)
    {
        for (MidiMsg m; numLive < (int) liveMidi.size() && q.pop (m);)
        {
            liveMidi[(size_t) numLive++] = m;

            if (isRolling && current->recording)
                recordedMidi.push ({ pos, m });
        }
    };

    for (auto& q : midiIn)
        takeLive (q);

    takeLive (typedMidi); // keys mode plays like a controller

    // Notes the MIDI editor auditions, for whichever track they're for.
    int numAuditions = 0;

    for (Audition a; numAuditions < (int) blockAuditions.size() && auditions.pop (a);)
        if (a.track >= 0 && a.track < maxTracks)
        {
            blockAuditions[(size_t) numAuditions++] = a;
            auto& held = auditionsHeld[(size_t) a.track];
            held = a.msg.isNoteOn() ? held + 1 : a.msg.isNoteOff() ? juce::jmax (0, held - 1) : held;
        }

    HostTransport transport;
    transport.bpm = current->bpm;
    transport.sampleRate = deviceRate;
    transport.timeSigNum = current->timeSigNum;
    transport.timeSigDen = current->timeSigDen;
    transport.pos = pos;
    transport.rolling = isRolling;
    transport.recording = isRolling && current->recording;

    auto* silence = scratch.getWritePointer (1);
    auto* mixL = scratch.getWritePointer (2);
    auto* mixR = scratch.getWritePointer (3);
    juce::FloatVectorOperations::clear (silence, n);
    juce::FloatVectorOperations::clear (mixL, n);
    juce::FloatVectorOperations::clear (mixR, n);

    const auto numBuses = juce::jmin (current->numBuses, Graph::maxBuses);

    for (int ch = 0; ch < numBuses * 2; ++ch)
        juce::FloatVectorOperations::clear (busPtrs[(size_t) ch], n);

    const auto numTracks = (int) juce::jmin (current->tracks.size(), (size_t) maxTracks);

    if (profiling)
    {
        profileTrackMs.fill (0.0);
        profileMixMs = 0.0;
    }

    block = { in, numIn, n, numLive, numAuditions, numBuses, pos, isRolling, transport };

    // First every track that isn't a bus, each on whichever core is free: they don't depend on
    // one another (only on the inputs), and this is where the time goes (instruments, inserts).
    int numParallel = 0;

    for (int t = 0; t < numTracks; ++t)
        if (current->tracks[(size_t) t].busIndex < 0)
            parallelTracks[(size_t) numParallel++] = t;

    if (parallel && pool != nullptr && pool->numWorkers() > 0 && numParallel > 1)
        pool->run (numParallel, [] (void* e, int i)
                   {
                       auto& engine = *static_cast<AudioEngine*> (e);
                       engine.renderTrack (engine.parallelTracks[(size_t) i], engine.block);
                   },
                   this);
    else
        for (int i = 0; i < numParallel; ++i)
            renderTrack (parallelTracks[(size_t) i], block);

    // Then in order: each mixed into its bus or the mix, so every bus is complete before it
    // plays (a bus renders here, once what feeds it is in). A graph without an order runs them
    // as listed.
    const auto mixFrom = profiling ? juce::Time::getHighResolutionTicks() : 0;
    double busMs = 0.0;

    for (int k = 0; k < numTracks; ++k)
    {
        const auto t = current->order.size() == current->tracks.size() ? current->order[(size_t) k] : k;

        if (t < 0 || t >= numTracks)
            continue;

        if (current->tracks[(size_t) t].busIndex >= 0)
        {
            renderTrack (t, block);
            busMs += profileTrackMs[(size_t) t];
        }

        finishTrack (t, block, mixL, mixR, silence);
    }

    const auto mixInsertsFrom = juce::Time::getHighResolutionTicks();
    mixInserts (*current, pos, transport, mixL, mixR, n);
    smooth (mixAverage, nullptr, (float) (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - mixInsertsFrom) * 1000.0),
            (double) n / deviceRate);

    if (profiling)
        profileMixMs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - mixFrom) * 1000.0 - busMs;

    const auto l = juce::FloatVectorOperations::findMinAndMax (mixL, n);
    const auto r = juce::FloatVectorOperations::findMinAndMax (mixR, n);
    masterPeaks[0].push (juce::jmax (l.getEnd(), -l.getStart()));
    masterPeaks[1].push (juce::jmax (r.getEnd(), -r.getStart()));

    // The click goes to the outputs after metering, so the mix meter shows only the music.
    // Recording counts whether it's audio (a recorder) or pad hits only (the graph's flag).
    bool anyRecording = current->recording;

    for (const auto& tp : current->tracks)
        anyRecording = anyRecording || tp.recorder != nullptr;

    const auto& click = current->click;

    // The count-in clicks on the beats before the start position (it may be negative), so they
    // lead into the music in time even from mid-beat. It clicks whether the click is on or not.
    if (counting > 0)
        renderClick (click, pos - counting, mixL, mixR, n);
    else if (isRolling && click.enabled && (anyRecording || ! click.recordOnly))
        renderClick (click, pos, mixL, mixR, n);
    else
        clickSamplesLeft = 0;

    if (numOut >= 2)
    {
        if (out[0] != nullptr) juce::FloatVectorOperations::copy (out[0], mixL, n);
        if (out[1] != nullptr) juce::FloatVectorOperations::copy (out[1], mixR, n);
    }
    else if (numOut == 1 && out[0] != nullptr)
    {
        juce::FloatVectorOperations::copy (out[0], mixL, n);
    }

    if (counting > 0)
        countInLeft.store (juce::jmax<juce::int64> (0, counting - n));
    else if (isRolling)
        playPos.store (pos + n);
}

void AudioEngine::renderClick (const ClickSettings& click, juce::int64 pos, float* left, float* right, int n)
{
    if (click.samplesPerBeat <= 0.0)
        return;

    // Next beat at or after the start of this block.
    auto beat = (juce::int64) std::ceil ((double) pos / click.samplesPerBeat - 1e-9);
    auto beatSample = (juce::int64) std::llround ((double) beat * click.samplesPerBeat);

    for (int i = 0; i < n; ++i)
    {
        if (pos + i == beatSample)
        {
            // Short decaying sine; the downbeat is higher and louder.
            const bool downbeat = click.beatsPerBar > 0 && beat % click.beatsPerBar == 0;
            clickPhase = 0.0;
            clickPhaseInc = juce::MathConstants<double>::twoPi * (downbeat ? 1600.0 : 1000.0) / deviceRate;
            clickLevel = click.gain * (downbeat ? 1.0f : 0.6f);
            clickDecay = (float) std::exp (-1.0 / (0.006 * deviceRate)); // 6 ms time constant
            clickSamplesLeft = (int) (0.04 * deviceRate);

            ++beat;
            beatSample = (juce::int64) std::llround ((double) beat * click.samplesPerBeat);
        }

        if (clickSamplesLeft > 0)
        {
            const auto s = (float) std::sin (clickPhase) * clickLevel;
            left[i] += s;
            right[i] += s;
            clickPhase += clickPhaseInc;
            clickLevel *= clickDecay;
            --clickSamplesLeft;
        }
    }
}
} // namespace taw
