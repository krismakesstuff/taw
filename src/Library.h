#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <optional>
#include <vector>

namespace taw
{
// The sample library: what each sample on your drives sounds like, measured once and kept in an
// index (in taw's config folder; nothing is written next to the samples), so samples can be
// sorted by kind and found by sound. See docs/design.md, "The sample library".
struct SampleInfo
{
    juce::String path;
    juce::int64 size = 0, modified = 0; // reanalysed when either changes

    double seconds = 0.0;   // the file's length
    double peakDb = -100.0; // sample peak
    double rmsDb = -100.0;  // over the part that sounds (to the -40 dB tail)
    double crestDb = 0.0;   // peak over RMS: high for a click, low for a dense noise
    double attackMs = 0.0;  // 10% to 90% of the peak
    double decayMs = 0.0;   // the peak until it's 20 dB down for good
    double tailMs = 0.0;    // until 40 dB down: how long it really sounds
    int hits = 1;           // separate attacks in the first second (a clap has a few)

    double pitchHz = 0.0, pitchConfidence = 0.0; // fundamental, if there's one to hear
    double centroidHz = 0.0; // brightness
    double flatness = 0.0;   // 0 tonal .. 1 noise
    double rolloffHz = 0.0;  // 85% of the energy is below this

    static constexpr int numBands = 9; // sub, low, low-mid ... air (edges in Library.cpp)
    std::array<float, numBands> bandsDb {}; // each band's share of the energy, dB
    std::array<float, 13> mfcc {};          // timbre, the usual way

    juce::String nameKind;  // kick, snare ... from the file and folder names; empty if they don't say
    juce::String soundKind; // from the sound alone: the nearest named samples
    double soundKindScore = 0.0; // how many of those neighbours agreed (0..1)

    float mapX = 0.0f, mapY = 0.0f; // on the library map (0..1): samples that sound alike are close

    juce::String kind() const { return nameKind.isNotEmpty() ? nameKind : soundKind; }
};

class Library
{
public:
    explicit Library (juce::File indexFile);

    // Measures every audio file under the folders (new or changed ones only; the rest come from
    // the index), on all cores. progress gets (done, total) now and then.
    void scan (const juce::Array<juce::File>& folders, std::function<void (int, int)> progress = {});

    // Gives samples their soundKind: the k nearest samples with a nameKind vote. Returns how
    // often that vote matches the name, for samples that have one (leave-one-out): how far the
    // sound alone can be trusted.
    double classify (int k = 5);

    // Places every sample on a 2D map (t-SNE over the same measurements classify uses), so that
    // ones that sound alike sit together. After classify.
    void layout (int iterations = 750);

    // The n samples that sound most like samples[i], nearest first.
    std::vector<int> nearest (int i, int n) const;

    bool save() const;
    const std::vector<SampleInfo>& samples() const { return items; }

    static std::optional<SampleInfo> analyse (const juce::File&, juce::AudioFormatManager&);
    static juce::String kindFromName (const juce::File&);

private:
    std::vector<float> features (const SampleInfo&) const; // scaled for distance
    void computeScaling();

    juce::File indexFile;
    std::vector<SampleInfo> items;
    std::vector<float> mean, spread;
};
} // namespace taw
