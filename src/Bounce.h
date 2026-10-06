#pragma once

#include "AudioEngine.h"

namespace taw
{
// Offline render of the mix bus over a time range, on its own thread, faster than real time.
// Prints what the mix bus plays (track volume, mute/solo, buses and sends, mix volume); never the click or
// live inputs.
class Bounce final : private juce::Thread
{
public:
    enum class Format { wav, mp3, video };

    // Format::video: the session's video, and where in it the range starts (seconds).
    struct Picture
    {
        juce::File file;
        double from = 0.0;
    };

    Bounce (std::unique_ptr<Graph>, juce::Range<juce::int64> range, double sampleRate, Format, juce::File target, Picture = {});
    ~Bounce() override;

    void start() { startThread(); }
    void cancel() { signalThreadShouldExit(); }

    bool isDone() const { return done.load(); }
    float progress() const { return progressValue.load(); }

    // Valid once done
    bool succeeded() const { return error.isEmpty() && ! cancelled; }
    const juce::String& errorMessage() const { return error; }
    bool wasCancelled() const { return cancelled; }
    float peak() const { return peakValue; }
    const juce::File& file() const { return target; }
    double seconds() const { return (double) range.getLength() / sampleRate; }

    static juce::String extension (Format f) { return f == Format::mp3 ? ".mp3" : f == Format::video ? ".mp4" : ".wav"; }

private:
    void run() override;
    bool renderBlock (juce::int64 pos, int n, juce::AudioBuffer<float>& out);

    std::unique_ptr<Graph> graph;
    juce::Range<juce::int64> range;
    double sampleRate;
    Format format;
    juce::File target;
    Picture picture;

    juce::AudioBuffer<float> bus;
    juce::AudioBuffer<float> buses; // left and right for each bus track
    std::array<float*, Graph::maxBuses * 2> busPtrs {};
    std::atomic<float> progressValue { 0.0f };
    std::atomic<bool> done { false };
    juce::String error;
    bool cancelled = false;
    float peakValue = 0.0f;
};
} // namespace taw
