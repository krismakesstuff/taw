#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
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
    juce::String kind;    // kick, snare, hihat, open hat, tom, perc (a second tom: "tom 2")
    double levelDb = -100.0;

    // What the guess is made from, for a better one later (the singer's own kick, snare and hat).
    double attackHz = 0.0;   // brightness of its first 20 ms: the consonant, which says which drum
    double centroidHz = 0.0; // brightness over its first 100 ms
    double lowShare = 0.0;   // of the energy, below 300 Hz
    double highShare = 0.0;  // and above 5 kHz
    double flatness = 0.0;   // 0 tonal .. 1 noise
    double decayMs = 0.0;    // the peak until it's 20 dB down

    // Its energy by band (below 150 Hz, to 300, 600, 1.2 k, 2.4 k, 5 k, 10 k, above), as shares:
    // of its first 100 ms and of its attack. What a take's hits are told apart by.
    std::array<float, 8> body {}, attack {};
    int group = 0; // hits that sound alike within the take share one
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

// What was heard as a part to put on the timeline: on a grid of sixteenths, in the take's own
// tempo (found from the take, and followed as it drifts: nobody hums to a click). Beat 0 is the
// first note or hit, firstOnset seconds into the recording.
struct HumPart
{
    struct Note { double beat = 0.0, length = 0.25; int pitch = 60, vel = 100; };
    struct Hit { double beat = 0.0; float vel = 0.8f; juce::String kind; };

    std::vector<Note> notes; // a melody ...
    std::vector<Hit> hits;   // ... or a beat, as asked for
    double bpm = 120.0;
    double firstOnset = 0.0;
    double beats = 0.0;      // its length, in whole bars of four
};

// nearBpm: the tempo it was hummed to (the session's), if there was one; 0 finds it from the
// take alone, which a beat is good for and a few sung notes aren't.
HumPart arrange (const Heard&, bool asBeat, double nearBpm = 0.0);

// A time in seconds as beats at a tempo, on a grid (in beats; 0 leaves it alone).
double humBeat (double seconds, double bpm, double grid);
} // namespace taw
