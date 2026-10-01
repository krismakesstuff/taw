#include "AudioEngine.h"

namespace taw
{
AudioEngine::AudioEngine()
{
    formatManager.registerBasicFormats();
    diskThread.startThread (juce::Thread::Priority::high);
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

void AudioEngine::play (juce::int64 from)
{
    playPos.store (juce::jmax<juce::int64> (0, from));
    seekTo.store (-1);
    rolling.store (true);
}

void AudioEngine::stop()
{
    rolling.store (false);
}

void AudioEngine::seek (juce::int64 position)
{
    position = juce::jmax<juce::int64> (0, position);

    if (rolling.load())
        seekTo.store (position);
    else
        playPos.store (position);
}

std::shared_ptr<Recorder> AudioEngine::createRecorder (int trackIndex, int inputChannel, const juce::File& file,
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
                                                   .withNumChannels (1)
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
    const auto maxBlock = juce::jmax (device->getCurrentBufferSizeSamples() * 2, 8192);
    scratch.setSize (5, maxBlock); // [0] track L, [1] silence, [2] mix L, [3] mix R, [4] track R
    scratch.clear();
    deviceRate = device->getCurrentSampleRate();
    clickSamplesLeft = 0;
    numInputs.store (device->getActiveInputChannels().countNumberOfSetBits());
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

    const bool isRolling = rolling.load();
    const auto pos = playPos.load();

    auto* busL = scratch.getWritePointer (0);
    auto* busR = scratch.getWritePointer (4);
    auto* silence = scratch.getWritePointer (1);
    auto* mixL = scratch.getWritePointer (2);
    auto* mixR = scratch.getWritePointer (3);
    juce::FloatVectorOperations::clear (silence, n);
    juce::FloatVectorOperations::clear (mixL, n);
    juce::FloatVectorOperations::clear (mixR, n);

    for (size_t t = 0; t < current->tracks.size() && t < (size_t) maxTracks; ++t)
    {
        auto& tp = current->tracks[t];
        const float* input = (tp.inputChannel >= 0 && tp.inputChannel < numIn && in[tp.inputChannel] != nullptr)
                                 ? in[tp.inputChannel]
                                 : nullptr;

        juce::FloatVectorOperations::clear (busL, n);
        juce::FloatVectorOperations::clear (busR, n);

        // A track either monitors its input or plays its lane, never both.
        const bool monitoring = tp.monitoringInput (isRolling);

        if (monitoring)
        {
            if (input != nullptr)
            {
                juce::FloatVectorOperations::copy (busL, input, n);
                juce::FloatVectorOperations::copy (busR, input, n);
            }
        }
        else if (isRolling)
        {
            renderClips (tp, pos, n, busL, busR);
            renderHits (tp, pos, n, busL, busR);
        }

        const auto gain = tp.audible ? tp.gain : 0.0f;
        juce::FloatVectorOperations::multiply (busL, gain, n);
        juce::FloatVectorOperations::multiply (busR, gain, n);

        // The meter shows what you hear from the track: the input (pre-fader, for setting
        // levels) while monitoring, otherwise its playback.
        if (monitoring && input != nullptr)
        {
            const auto range = juce::FloatVectorOperations::findMinAndMax (input, n);
            trackPeaks[t].push (juce::jmax (range.getEnd(), -range.getStart()));
        }
        else
        {
            const auto l = juce::FloatVectorOperations::findMinAndMax (busL, n);
            const auto r = juce::FloatVectorOperations::findMinAndMax (busR, n);
            trackPeaks[t].push (juce::jmax (l.getEnd(), -l.getStart(), r.getEnd(), -r.getStart()));
        }

        const auto toMix = (isStereo (tp) && ! monitoring) ? 1.0f : centrePan; // a mono input pans centre
        juce::FloatVectorOperations::addWithMultiply (mixL, busL, toMix, n);
        juce::FloatVectorOperations::addWithMultiply (mixR, busR, toMix, n);

        if (isRolling && tp.recorder != nullptr)
        {
            auto& rec = *tp.recorder;
            const float* src = input != nullptr ? input : silence;

            if (rec.startSample.load (std::memory_order_relaxed) < 0)
                rec.startSample.store (pos);

            if (! rec.writer->write (&src, n))
                rec.droppedBlocks.fetch_add (1);

            rec.samplesWritten.fetch_add (n);

            for (int i = 0; i < n; ++i)
            {
                rec.ovPeak = juce::jmax (rec.ovPeak, std::abs (src[i]));

                if (++rec.ovCount == Recorder::overviewBlock)
                {
                    rec.overview.push (rec.ovPeak);
                    rec.ovPeak = 0.0f;
                    rec.ovCount = 0;
                }
            }
        }
    }

    juce::FloatVectorOperations::multiply (mixL, current->mixGain, n);
    juce::FloatVectorOperations::multiply (mixR, current->mixGain, n);

    const auto l = juce::FloatVectorOperations::findMinAndMax (mixL, n);
    const auto r = juce::FloatVectorOperations::findMinAndMax (mixR, n);
    masterPeaks[0].push (juce::jmax (l.getEnd(), -l.getStart()));
    masterPeaks[1].push (juce::jmax (r.getEnd(), -r.getStart()));

    // The click goes to the outputs after metering, so the mix meter shows only the music.
    bool anyRecording = false;

    for (const auto& tp : current->tracks)
        anyRecording = anyRecording || tp.recorder != nullptr;

    const auto& click = current->click;

    if (isRolling && click.enabled && (anyRecording || ! click.recordOnly))
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

    if (isRolling)
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
