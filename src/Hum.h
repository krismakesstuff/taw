#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <optional>
#include <vector>

namespace taw
{
// Hearing a part someone hummed, sang or beatboxed: the notes of a melody, and the hits of a
// beat with a guess at each one's drum. Both are worked out for any recording; pitched says
// which reading suits it. Times are seconds from the start of the audio.
struct HumNote
{
    double start = 0.0, length = 0.0;
    int pitch = 60;       // MIDI note, after the singer's tuning is taken out
    double cents = 0.0;   // how far the sung pitch sat from that note
    double levelDb = -100.0;
};

struct HumHit
{
    double time = 0.0;
    juce::String kind;    // kick, snare, hihat, open hat
    double levelDb = -100.0;

    // What the guess is made from, for a better one later (the singer's own kick, snare and hat).
    double attackHz = 0.0;   // brightness of its first 20 ms: the consonant, which says which drum
    double centroidHz = 0.0; // brightness over its first 100 ms
    double lowShare = 0.0;   // of the energy, below 300 Hz
    double highShare = 0.0;  // and above 5 kHz
    double flatness = 0.0;   // 0 tonal .. 1 noise
    double decayMs = 0.0;    // the peak until it's 20 dB down
};

struct Heard
{
    std::vector<HumNote> notes;
    std::vector<HumHit> hits;

    // The pitch as it was sung, before it was cut into notes: a MIDI note with its fraction every
    // contourHop seconds (tuning taken out), 0 where nothing is sung. For bends, and for seeing
    // what the notes were made from.
    std::vector<float> contour;
    double contourHop = 0.0;

    double seconds = 0.0;
    double tuningCents = 0.0; // the singer against A 440: every note was this far off, on average
    double pitched = 0.0;     // how much of the sounding time is held notes (0..1)

    bool sung() const { return pitched >= 0.5; } // a melody; otherwise a beat
};

Heard hear (const float* mono, int numSamples, double sampleRate);
std::optional<Heard> hear (const juce::File&, juce::AudioFormatManager&);

// A time in seconds as beats at a tempo, on a grid (in beats; 0 leaves it alone).
double humBeat (double seconds, double bpm, double grid);
} // namespace taw
