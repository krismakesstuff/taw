// Hearing a hummed, sung or beatboxed part. The melody: a pitch track (YIN's difference function,
// several candidates a frame, the cheapest path through them) cut into notes where the pitch
// moves, the voice stops or a note is struck again. The beat: onsets where the spectrum jumps,
// each sorted into a drum by where its energy sits.

#include "Hum.h"
#include "Import.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace taw
{
namespace
{
constexpr double hopSeconds = 0.01;                     // the pitch track's step
constexpr double lowestHz = 60.0, highestHz = 1100.0;   // a voice, with room either side
constexpr float gateDb = 40.0f;                         // this far under the loudest frame is silence
constexpr float strikeDb = 6.0f;                        // a dip this deep and back is a new note
constexpr float stepSemitones = 0.7f;                   // off the note by this much ...
constexpr int stepFrames = 5;                           // ... for this long is the next note
constexpr int shortFrames = 12;                         // shorter pieces are notes only if they hold still ...
constexpr int stableFrames = 8;                         // ... for this long ...
constexpr float stableSemitones = 0.6f;                 // ... within this; the rest join a neighbour (a scoop, a slide)
constexpr float dipSemitones = 1.0f;                    // a short piece this far off the note it joins: struck again
constexpr float sameSemitones = 0.7f;                  // pieces closer than this with nothing struck between are one note
constexpr float slideSemitones = 2.0f;                  // a piece whose pitch wanders more isn't a note
constexpr double twoSounds = 0.4;                       // hits of a family are two drums when they fit two groups this well (a silhouette, 0..1)
constexpr double twoSoundsApart = 0.18;                 // ... and the groups are this far apart (0 the same .. 1 nothing shared)
constexpr double junkDb = 14.0;                        // hits this far under the usual one are noises, not drums

const double twoPi = juce::MathConstants<double>::twoPi;

double toDb (double gain) { return 20.0 * std::log10 (std::max (gain, 1.0e-6)); }

// Every factor-th sample, after a low-pass that keeps what folds back out.
std::vector<float> decimate (const float* x, int n, int factor)
{
    if (factor <= 1)
        return { x, x + n };

    const int half = 6 * factor;
    const double cutoff = 0.4 / factor; // cycles a sample
    std::vector<double> h ((size_t) (2 * half + 1));
    double sum = 0.0;

    for (int i = -half; i <= half; ++i)
    {
        const double t = twoPi * cutoff * i;
        const double sinc = i == 0 ? 1.0 : std::sin (t) / t;
        const double hann = 0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * i / (half + 1));
        h[(size_t) (i + half)] = sinc * hann;
        sum += sinc * hann;
    }

    std::vector<float> out ((size_t) (n / factor));

    for (int m = 0; m < (int) out.size(); ++m)
    {
        const int centre = m * factor;
        double s = 0.0;

        for (int k = std::max (-half, -centre); k <= half && centre + k < n; ++k)
            s += h[(size_t) (k + half)] * (double) x[centre + k];

        out[(size_t) m] = (float) (s / sum);
    }

    return out;
}

struct PitchTrack
{
    double hop = hopSeconds;     // seconds a frame
    std::vector<float> pitch;    // MIDI note with its fraction; 0 where nothing is sung
    std::vector<float> levelDb;  // RMS around the frame
    float gate = -100.0f;        // frames under this are silence
};

struct Candidate
{
    float pitch = 0.0f; // 0: nothing sung
    float cost = 0.0f;  // how badly it fits this frame
    float total = 0.0f; // the cheapest path that ends here
    int from = 0;       // and where it came from in the frame before
};

PitchTrack trackPitch (const float* x, int n, double rate)
{
    // A voice's pitch is all under 1.1 kHz: at about 11 kHz there's a quarter of the work.
    const int factor = std::max (1, (int) (rate / 11000.0));
    auto low = decimate (x, n, factor);
    const double r = rate / factor;

    // Rumble and DC out (a 50 Hz high-pass): they tilt the difference function.
    {
        const float a = (float) (1.0 / (1.0 + twoPi * 50.0 / r));
        float last = 0.0f, y = 0.0f;

        for (auto& v : low)
        {
            y = a * (y + v - last);
            last = v;
            v = y;
        }
    }

    const int hop = std::max (1, juce::roundToInt (r * hopSeconds));
    const int tauMin = std::max (2, (int) (r / highestHz)), tauMax = (int) std::ceil (r / lowestHz);
    const int span = 2 * tauMax;
    const int frames = (int) low.size() / hop + 1;

    // Each frame sits around its time: silence before the start and after the end.
    std::vector<float> y ((size_t) (span / 2) + low.size() + (size_t) (span + tauMax + hop), 0.0f);
    std::copy (low.begin(), low.end(), y.begin() + span / 2);

    // YIN takes the first dip under a threshold. Here every threshold has a say (more for the
    // usual ones), so a frame gives each dip a likelihood rather than one answer.
    constexpr int numThresholds = 25;
    std::array<double, numThresholds> weight {};
    {
        double sum = 0.0;

        for (int k = 0; k < numThresholds; ++k)
        {
            const double s = 0.02 * (k + 1);
            sum += weight[(size_t) k] = s * std::pow (1.0 - s, 7.0);
        }

        for (auto& w : weight)
            w /= sum;
    }

    PitchTrack track;
    track.hop = hop / r;
    track.pitch.assign ((size_t) frames, 0.0f);
    track.levelDb.assign ((size_t) frames, -120.0f);

    std::vector<std::vector<Candidate>> lattice ((size_t) frames);
    std::vector<double> d ((size_t) tauMax + 2);

    for (int f = 0; f < frames; ++f)
    {
        const float* w = y.data() + (size_t) f * (size_t) hop;
        double energy = 0.0;

        for (int j = 0; j < span; ++j)
            energy += (double) w[j] * (double) w[j];

        track.levelDb[(size_t) f] = (float) toDb (std::sqrt (energy / span));

        // The difference function, cumulative mean normalised.
        d[0] = 1.0;
        double running = 0.0;

        for (int tau = 1; tau <= tauMax + 1; ++tau)
        {
            double s = 0.0;

            for (int j = 0; j < span; ++j)
            {
                const double diff = (double) w[j] - (double) w[j + tau];
                s += diff * diff;
            }

            running += s;
            d[(size_t) tau] = running > 0.0 ? s * tau / running : 1.0;
        }

        auto& cands = lattice[(size_t) f];
        cands.push_back ({});
        double voiced = 0.0, lowestSoFar = 1.0e9;

        for (int tau = tauMin; tau <= tauMax; ++tau)
        {
            const double here = d[(size_t) tau];

            if (! (here < d[(size_t) tau - 1] && here <= d[(size_t) tau + 1]) || here >= lowestSoFar)
                continue;

            // The thresholds this dip is the first to get under.
            double p = 0.0;

            for (int k = 0; k < numThresholds; ++k)
                if (const double s = 0.02 * (k + 1); s > here && s <= lowestSoFar)
                    p += weight[(size_t) k];

            lowestSoFar = here;

            if (p <= 0.0)
                continue;

            double exact = tau;
            const double before = d[(size_t) tau - 1], after = d[(size_t) tau + 1];

            if (const double den = before - 2.0 * here + after; std::abs (den) > 1.0e-12)
                exact += 0.5 * (before - after) / den;

            Candidate c;
            c.pitch = (float) (69.0 + 12.0 * std::log2 (r / exact / 440.0));
            c.cost = (float) -std::log (std::max (p, 1.0e-4));
            cands.push_back (c);
            voiced += p;
        }

        cands[0].cost = (float) -std::log (std::max (1.0 - voiced, 1.0e-4));
    }

    track.gate = std::max (-60.0f,*std::max_element (track.levelDb.begin(), track.levelDb.end()) - gateDb);

    for (int f = 0; f < frames; ++f)
        if (track.levelDb[(size_t) f] < track.gate)
        {
            lattice[(size_t) f].resize (1);
            lattice[(size_t) f][0].cost = 0.0f;
        }

    // The cheapest path: a pitch that stays put costs nothing, a leap costs up to an octave's
    // worth, and starting or stopping costs more than a frame or two of doubt.
    auto move = [] (const Candidate& a, const Candidate& b)
    {
        const bool sa = a.pitch > 0.0f, sb = b.pitch > 0.0f;

        if (sa && sb)
            return 0.35f * std::min (12.0f, std::abs (a.pitch - b.pitch));

        return sa == sb ? 0.0f : 2.5f;
    };

    for (auto& c : lattice[0])
        c.total = c.cost;

    for (int f = 1; f < frames; ++f)
        for (auto& c : lattice[(size_t) f])
        {
            const auto& prev = lattice[(size_t) f - 1];
            float best = 1.0e30f;

            for (int i = 0; i < (int) prev.size(); ++i)
                if (const float t = prev[(size_t) i].total + move (prev[(size_t) i], c); t < best)
                {
                    best = t;
                    c.from = i;
                }

            c.total = best + c.cost;
        }

    int at = 0;

    for (int i = 1; i < (int) lattice.back().size(); ++i)
        if (lattice.back()[(size_t) i].total < lattice.back()[(size_t) at].total)
            at = i;

    for (int f = frames - 1; f >= 0; --f)
    {
        track.pitch[(size_t) f] = lattice[(size_t) f][(size_t) at].pitch;
        at = lattice[(size_t) f][(size_t) at].from;
    }

    return track;
}

float median (const float* v, int n)
{
    std::vector<float> s (v, v + n);
    std::nth_element (s.begin(), s.begin() + n / 2, s.end());
    return s[(size_t) n / 2];
}

struct Segment
{
    int from = 0, to = 0; // frames
    bool struck = false;  // starts after a silence or a dip (not just a change of pitch)
    float pitch = 0.0f;
};

void findNotes (PitchTrack& track, Heard& heard)
{
    const int frames = (int) track.pitch.size();
    auto& pitch = track.pitch;
    const auto& level = track.levelDb;

    // The singer's tuning: where the sung pitches sit between the keys, on average (round the
    // circle of one semitone). Taken out, so someone 40 cents flat lands on the notes they meant.
    {
        double c = 0.0, s = 0.0;

        for (auto p : pitch)
            if (p > 0.0f)
            {
                c += std::cos (twoPi * (double) p);
                s += std::sin (twoPi * (double) p);
            }

        const double off = std::atan2 (s, c) / twoPi;
        heard.tuningCents = 100.0 * off;

        for (auto& p : pitch)
            if (p > 0.0f)
                p -= (float) off;
    }

    std::vector<Segment> segs;

    for (int a = 0; a < frames;)
    {
        if (pitch[(size_t) a] <= 0.0f)
        {
            ++a;
            continue;
        }

        int b = a;

        while (b < frames && pitch[(size_t) b] > 0.0f)
            ++b;

        // Struck again on the same breath: the level dips and comes back.
        std::vector<int> strikes { a };
        float peak = level[(size_t) a], valley = peak;
        int valleyAt = a;

        for (int i = a + 1; i < b; ++i)
        {
            const float v = level[(size_t) i];

            if (v < valley)
            {
                valley = v;
                valleyAt = i;
            }
            else if (v - valley >= strikeDb && peak - valley >= strikeDb)
            {
                strikes.push_back (valleyAt);
                peak = valley = v;
                valleyAt = i;
            }
            else if (v > peak)
            {
                peak = valley = v;
                valleyAt = i;
            }
        }

        strikes.push_back (b);

        // Within each, a new piece where the pitch leaves the note and stays away.
        for (size_t k = 0; k + 1 < strikes.size(); ++k)
        {
            const int end = strikes[k + 1];
            int start = strikes[k], away = 0;
            bool struck = true, above = false;

            for (int i = start + 1; i < end; ++i)
            {
                const int settled = i - away; // the frames before this are the note so far
                const int back = std::max (start, settled - 30);
                const float off = pitch[(size_t) i] - median (pitch.data() + back, settled - back);

                if (std::abs (off) <= stepSemitones)
                    away = 0;
                else if (away > 0 && above == (off > 0.0f))
                    ++away;
                else
                {
                    away = 1;
                    above = off > 0.0f;
                }

                if (away >= stepFrames)
                {
                    segs.push_back ({ start, settled, struck, 0.0f });
                    start = settled;
                    struck = false;
                    away = 0;
                }
            }

            segs.push_back ({ start, end, struck, 0.0f });
        }

        a = b;
    }

    // A short piece is a note of its own only if the pitch holds still in it (a grace note).
    // Otherwise it's the way into a note (a scoop, a slide between two, the dip of a consonant):
    // it joins the note after, or the one before; one on its own is a blip. A dip well under the
    // note it joins means that note was struck again ("da-da" on one pitch).
    auto middle = [&] (const Segment& s) { return median (pitch.data() + s.from, s.to - s.from); };

    for (size_t i = 0; i < segs.size();)
    {
        const auto s = segs[i];
        const int length = s.to - s.from;
        const bool joinsNext = i + 1 < segs.size() && segs[i + 1].from == s.to && ! segs[i + 1].struck;
        const bool joinsPrev = i > 0 && segs[i - 1].to == s.from && ! s.struck;
        // Still: past the way in (its first quarter) and short of the way out (its last tenth).
        const auto [lowest, highest] = std::minmax_element (pitch.begin() + s.from + length / 4, pitch.begin() + s.to - length / 10);
        const bool still = *highest - *lowest <= stableSemitones && length >= stableFrames;

        if (length >= shortFrames || still)
        {
            ++i;
            continue;
        }

        if (joinsNext)
        {
            auto& next = segs[i + 1];
            const bool dip = std::abs (middle (s) - middle (next)) > dipSemitones;
            next.from = s.from;
            next.struck = s.struck || dip;
        }
        else if (joinsPrev)
            segs[i - 1].to = s.to;

        segs.erase (segs.begin() + (std::ptrdiff_t) i);
    }

    for (auto& s : segs)
        s.pitch = middle (s);

    // Pieces at about the same pitch with nothing struck between them are one note.
    for (size_t i = 0; i + 1 < segs.size();)
    {
        auto& s = segs[i];
        const auto& next = segs[i + 1];

        if (next.from == s.to && ! next.struck && std::abs (next.pitch - s.pitch) < sameSemitones)
        {
            s.to = next.to;
            s.pitch = median (pitch.data() + s.from, s.to - s.from);
            segs.erase (segs.begin() + (std::ptrdiff_t) i + 1);
        }
        else
            ++i;
    }

    int sounding = 0, held = 0;

    for (auto l : level)
        sounding += l >= track.gate ? 1 : 0;

    for (const auto& s : segs)
    {
        // A pitch that never settles (a kick's drop, a siren) isn't a note.
        std::vector<float> sorted (pitch.begin() + s.from, pitch.begin() + s.to);
        std::sort (sorted.begin(), sorted.end());

        if (sorted[sorted.size() * 3 / 4] - sorted[sorted.size() / 4] > slideSemitones)
            continue;

        HumNote note;
        note.start = s.from * track.hop;
        note.length = (s.to - s.from) * track.hop;
        note.pitch = juce::jlimit (0, 127, juce::roundToInt (s.pitch));
        note.cents = 100.0 * ((double) s.pitch - note.pitch);
        note.levelDb = *std::max_element (level.begin() + s.from, level.begin() + s.to);
        heard.notes.push_back (note);
        held += s.to - s.from;
    }

    heard.pitched = sounding > 0 ? std::min (1.0, (double) held / sounding) : 0.0;
}

// Which drum each hit is. Its attack says which family: low (kick, toms), hats, or the middle
// (snare, percussion). Within a family nobody's mouth makes the drums the way the next one
// does, and a phone hears them its own way, so there are no numbers to hold a tom or a clap
// to: the family's hits are split in two where they fall into two sounds on their own (and
// left as one drum where they don't), and the two named against each other.
struct HitSplit
{
    std::vector<int> a, b; // b empty: one sound
};

// what: each hit as a point (the same length each); which: the hits to split.
HitSplit splitHits (const std::vector<std::vector<double>>& what, const std::vector<int>& which)
{
    const size_t n = which.size();
    HitSplit one { which, {} };

    if (n < 4)
        return one;

    std::vector<std::vector<double>> apart (n, std::vector<double> (n, 0.0));

    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j)
        {
            double sum = 0.0;

            for (size_t k = 0; k < what[(size_t) which[i]].size(); ++k)
            {
                const double d = what[(size_t) which[i]][k] - what[(size_t) which[j]][k];
                sum += d * d;
            }

            apart[i][j] = std::sqrt (sum);
        }

    // Nearest joined first (by the average between their hits), down to two.
    std::vector<std::vector<size_t>> groups;
    auto between = apart;

    for (size_t i = 0; i < n; ++i)
        groups.push_back ({ i });

    while (groups.size() > 2)
    {
        size_t a = 0, b = 1;

        for (size_t i = 0; i < groups.size(); ++i)
            for (size_t j = i + 1; j < groups.size(); ++j)
                if (between[i][j] < between[a][b])
                {
                    a = i;
                    b = j;
                }

        const double na = (double) groups[a].size(), nb = (double) groups[b].size();

        for (size_t k = 0; k < groups.size(); ++k)
            between[a][k] = between[k][a] = (na * between[a][k] + nb * between[b][k]) / (na + nb);

        groups[a].insert (groups[a].end(), groups[b].begin(), groups[b].end());
        groups.erase (groups.begin() + (std::ptrdiff_t) b);
        between.erase (between.begin() + (std::ptrdiff_t) b);

        for (auto& row : between)
            row.erase (row.begin() + (std::ptrdiff_t) b);
    }

    // Two sounds only if each has more than one hit (one odd hit is a drum hit differently)
    // and the hits sit well nearer their own than the other's.
    if (groups[0].size() < 2 || groups[1].size() < 2)
        return one;

    double fit = 0.0, gap = 0.0;

    for (size_t g = 0; g < 2; ++g)
        for (const size_t i : groups[g])
        {
            double own = 0.0, other = 0.0;

            for (const size_t j : groups[g])
                own += apart[i][j] / (double) (groups[g].size() - 1);

            for (const size_t j : groups[1 - g])
                other += apart[i][j] / (double) groups[1 - g].size();

            gap += other / (double) n;

            fit += (other - own) / std::max ({ own, other, 1.0e-9 }) / (double) n;
        }

    if (juce::SystemStats::getEnvironmentVariable ("TAW_HUM_BANDS", {}).isNotEmpty())
        std::printf ("split %d/%d  fit %.2f  gap %.2f\n", (int) groups[0].size(), (int) groups[1].size(), fit, gap);

    if (fit < twoSounds || gap < twoSoundsApart)
        return one;

    HitSplit two;

    for (const size_t i : groups[0]) two.a.push_back (which[i]);
    for (const size_t i : groups[1]) two.b.push_back (which[i]);

    return two;
}

void nameHits (std::vector<HumHit>& hits)
{
    std::vector<int> lows, mids;
    std::vector<std::vector<double>> lowPoint, midPoint;

    for (size_t h = 0; h < hits.size(); ++h)
    {
        auto& hit = hits[h];
        lowPoint.emplace_back();
        midPoint.emplace_back();

        // Low ones by their whole body. Middle ones by what's over 600 Hz and how noisy they
        // are: a snare said with some voice under it is still the snare.
        double upper = 1.0e-9;

        for (size_t b = 3; b < hit.body.size(); ++b)
            upper += hit.body[b];

        for (size_t b = 0; b < hit.body.size(); ++b)
        {
            lowPoint.back().push_back (std::sqrt ((double) hit.body[b] / 2.0));

            if (b >= 3)
                midPoint.back().push_back (std::sqrt ((double) hit.body[b] / upper / 2.0));
        }

        midPoint.back().push_back (std::sqrt (hit.flatness));

        // (1 kHz: one real take's kicks started at 170 to 700 Hz, its snares at 1.5 to 3.3 kHz.)
        if (hit.attack[0] + hit.attack[1] >= 0.4 || hit.attackHz < 1000.0)
            lows.push_back ((int) h);
        else if (hit.attack[6] + hit.attack[7] >= 0.5 || hit.attackHz > 5000.0)
            hit.kind = hit.decayMs > 120.0 ? "open hat" : "hihat"; // open if it rings
        else
            mids.push_back ((int) h);
    }

    int groupsSoFar = 0;
    const auto name = [&] (const std::vector<int>& which, const juce::String& kind)
    {
        for (const int h : which)
        {
            hits[(size_t) h].kind = kind;
            hits[(size_t) h].group = groupsSoFar;
        }

        ++groupsSoFar;
    };
    const auto mean = [&] (const std::vector<int>& which, auto&& of)
    {
        double sum = 0.0;

        for (const int h : which)
            sum += of (hits[(size_t) h]) / (double) which.size();

        return sum;
    };
    const auto deep = [] (const HumHit& hit) { return (double) hit.body[0]; };

    // Low: two sounds are a kick (the deeper) and a tom. One is a kick, unless there's not
    // much low end after its attack (a "dn", a "gu": a tom).
    if (const auto low = splitHits (lowPoint, lows); ! low.b.empty())
    {
        const bool first = mean (low.a, deep) >= mean (low.b, deep);
        name (first ? low.a : low.b, "kick");
        name (first ? low.b : low.a, "tom");
    }
    else if (! lows.empty())
    {
        const bool tom = mean (lows, [] (const HumHit& hit) { return hit.lowShare; }) < 0.5
                         || mean (lows, [] (const HumHit& hit) { return hit.centroidHz; }) > 600.0;
        name (lows, tom ? "tom" : "kick");
    }

    // Middle: two sounds are the snare (the one hit more) and percussion.
    if (const auto mid = splitHits (midPoint, mids); ! mid.b.empty())
    {
        const bool first = mid.a.size() >= mid.b.size();
        name (first ? mid.a : mid.b, "snare");
        name (first ? mid.b : mid.a, "perc");
    }
    else
    {
        name (mids, "snare");
    }
}

void findHits (const float* x, int n, double rate, float peak, Heard& heard)
{
    // 23 ms frames, an eighth apart; each sits around its time.
    const int order = juce::jlimit (8, 13, juce::roundToInt (std::log2 (rate * 0.023)));
    const int size = 1 << order, hop = size / 8, bins = size / 2 + 1;
    const int frames = n / hop + 1;
    const double binHz = rate / size;

    juce::dsp::FFT fft (order);
    std::vector<float> window ((size_t) size), frame ((size_t) size * 2), last ((size_t) bins, 0.0f);

    for (int i = 0; i < size; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (size - 1));

    auto spectrum = [&] (int start) // magnitudes of the frame from this sample, in frame[0..bins)
    {
        std::fill (frame.begin(), frame.end(), 0.0f);

        for (int i = std::max (0, -start); i < size && start + i < n; ++i)
            frame[(size_t) i] = x[start + i] * window[(size_t) i];

        fft.performFrequencyOnlyForwardTransform (frame.data(), true);
    };

    // How much louder each frame got than the one before, bin by bin, on a log scale (so a
    // quiet hit after a loud one still shows).
    const float scale = 100.0f / (peak * (float) size * 0.25f);
    std::vector<float> flux ((size_t) frames, 0.0f);

    for (int f = 0; f < frames; ++f)
    {
        spectrum (f * hop - size / 2);
        float rise = 0.0f;

        for (int k = 1; k < bins; ++k)
        {
            const float l = std::log1p (frame[(size_t) k] * scale);
            rise += std::max (0.0f, l - last[(size_t) k]);
            last[(size_t) k] = l;
        }

        flux[(size_t) f] = f > 0 ? rise : 0.0f;
    }

    const float top = *std::max_element (flux.begin(), flux.end());

    if (top <= 0.0f)
        return;

    // An onset: the biggest jump for 30 ms either side, well over what's around it.
    const double frameSeconds = hop / rate;
    const int nearby = std::max (1, juce::roundToInt (0.03 / frameSeconds)), around = juce::roundToInt (0.1 / frameSeconds);
    const int block = std::max (1, (int) (rate / 1000.0)); // 1 ms
    std::vector<int> onsets;

    for (int f = 1; f < frames; ++f)
    {
        const float v = flux[(size_t) f];

        if (v < 0.1f * top)
            continue;

        bool biggest = true;

        for (int g = std::max (0, f - nearby); g <= std::min (frames - 1, f + nearby) && biggest; ++g)
            biggest = g == f || (g < f ? flux[(size_t) g] < v : flux[(size_t) g] <= v);

        if (! biggest)
            continue;

        double mean = 0.0;
        const int lo = std::max (0, f - around), hi = std::min (frames - 1, f + around);

        for (int g = lo; g <= hi; ++g)
            mean += flux[(size_t) g];

        if (v < 1.5 * mean / (hi - lo + 1))
            continue;

        // Where in the frame it starts: the first millisecond well over what came before.
        auto energy = [&] (int from, int len)
        {
            double e = 0.0;

            for (int i = std::max (0, from); i < std::min (n, from + len); ++i)
                e += (double) x[i] * (double) x[i];

            return e / len;
        };

        const int from = f * hop - size / 2, to = std::min (n, f * hop + size / 2);
        const double before = energy (from - 5 * block, 5 * block);
        double loudest = 0.0;

        for (int i = from; i < to; i += block)
            loudest = std::max (loudest, energy (i, block));

        int onset = juce::jlimit (0, n - 1, f * hop);

        for (int i = from; i < to; i += block)
            if (const double e = energy (i, block); e > 4.0 * before && e >= 0.1 * loudest)
            {
                onset = std::max (0, i);
                break;
            }

        // Not inside a held note: a voice's vibrato moves its harmonics from bin to bin, which
        // looks like one jump after another.
        const double time = onset / rate;
        const bool held = std::any_of (heard.notes.begin(), heard.notes.end(),
                                       [time] (const HumNote& note) { return time > note.start + 0.03 && time < note.start + note.length; });

        // And not within 80 ms of the last: a mouth's drum is a consonant and then its body, one
        // hit that can jump twice.
        if (! held && (onsets.empty() || onset - onsets.back() > (int) (0.08 * rate)))
            onsets.push_back (onset);
    }

    for (size_t h = 0; h < onsets.size(); ++h)
    {
        const int onset = onsets[h];
        const int end = std::min ({ n, onset + (int) (0.2 * rate), h + 1 < onsets.size() ? onsets[h + 1] : n });

        // Its spectrum, over its first 100 ms.
        // And its attack alone (the first frame): what the mouth meant. A snare's "k" or "p" is
        // bright even when a low body follows it; a kick is low from the start.
        std::vector<double> power ((size_t) bins, 0.0);
        double attackTotal = 0.0, attackWeighted = 0.0;
        std::array<double, 8> bodyBands {}, attackBands {};
        const auto bandOf = [] (double f) { return f < 150.0 ? 0 : f < 300.0 ? 1 : f < 600.0 ? 2 : f < 1200.0 ? 3 : f < 2400.0 ? 4 : f < 5000.0 ? 5 : f < 10000.0 ? 6 : 7; };

        for (int start = onset - size / 4; start == onset - size / 4 || start < std::min (end, onset + (int) (0.1 * rate)) - size / 2; start += size / 2)
        {
            spectrum (start);

            for (int k = 0; k < bins; ++k)
                power[(size_t) k] += (double) frame[(size_t) k] * (double) frame[(size_t) k];

            if (start == onset - size / 4)
                for (int k = 1; k < bins; ++k)
                    if (const double f = k * binHz; f >= 40.0 && f <= 16000.0)
                    {
                        const double p = power[(size_t) k];
                        attackTotal += p;
                        attackWeighted += f * p;
                        attackBands[(size_t) bandOf (f)] += p;
                    }
        }

        double total = 0.0, weighted = 0.0, low = 0.0, high = 0.0, logSum = 0.0;
        int counted = 0;

        for (int k = 1; k < bins; ++k)
        {
            const double f = k * binHz, p = power[(size_t) k];

            if (f < 40.0 || f > 16000.0)
                continue;

            total += p;
            weighted += f * p;
            low += f < 300.0 ? p : 0.0;
            high += f > 5000.0 ? p : 0.0;
            bodyBands[(size_t) bandOf (f)] += p;
            logSum += std::log (p + 1.0e-12);
            ++counted;
        }

        if (total <= 0.0 || counted == 0)
            continue;

        HumHit hit;
        hit.time = onset / rate;
        hit.centroidHz = weighted / total;
        hit.lowShare = low / total;
        hit.highShare = high / total;
        hit.flatness = std::exp (logSum / counted) / (total / counted + 1.0e-12);

        for (size_t b = 0; b < 8; ++b)
        {
            hit.body[b] = (float) (bodyBands[b] / total);
            hit.attack[b] = (float) (attackTotal > 0.0 ? attackBands[b] / attackTotal : bodyBands[b] / total);
        }

        // Level, and how long until it's 20 dB down (1 ms peaks).
        float loud = 0.0f;
        int loudAt = onset;

        for (int i = onset; i < end; ++i)
            if (std::abs (x[i]) > loud)
            {
                loud = std::abs (x[i]);
                loudAt = i;
            }

        hit.levelDb = toDb (loud);
        int quiet = end;

        for (int i = loudAt; i + block <= end && quiet == end; i += block)
        {
            float most = 0.0f;

            for (int j = i; j < i + block; ++j)
                most = std::max (most, std::abs (x[j]));

            if (most < 0.1f * loud)
                quiet = i;
        }

        hit.decayMs = 1000.0 * (quiet - loudAt) / rate;

        hit.attackHz = attackTotal > 0.0 ? attackWeighted / attackTotal : hit.centroidHz;
        heard.hits.push_back (hit); // which drum: nameHits, once they're all in
    }

    // A breath, the phone being put down: far quieter than the drums around it.
    if (heard.hits.size() >= 3)
    {
        std::vector<double> levels;

        for (const auto& hit : heard.hits)
            levels.push_back (hit.levelDb);

        std::nth_element (levels.begin(), levels.begin() + (std::ptrdiff_t) levels.size() / 2, levels.end());
        const double floor = levels[levels.size() / 2] - junkDb;
        std::erase_if (heard.hits, [floor] (const HumHit& hit) { return hit.levelDb < floor; });
    }

    nameHits (heard.hits);
}
} // namespace

Heard hear (const float* x, int n, double rate)
{
    Heard heard;

    if (x == nullptr || n <= 0 || rate <= 0.0)
        return heard;

    heard.seconds = n / rate;
    float peak = 0.0f;

    for (int i = 0; i < n; ++i)
        peak = std::max (peak, std::abs (x[i]));

    if (peak < 1.0e-4f)
        return heard; // silence

    auto track = trackPitch (x, n, rate);
    findNotes (track, heard);
    findHits (x, n, rate, peak, heard);
    heard.contour = std::move (track.pitch);
    heard.contourHop = track.hop;
    return heard;
}

std::optional<Heard> hear (const juce::File& file, juce::AudioFormatManager& formats)
{
    juce::AudioBuffer<float> buffer;
    double rate = 0.0;

    if (std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor (file) };
        reader != nullptr && reader->sampleRate > 0.0 && reader->lengthInSamples > 0)
    {
        // A part, not a song: ten minutes at most.
        rate = reader->sampleRate;
        const int length = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) (600.0 * rate));
        buffer.setSize ((int) std::max (1u, std::min (2u, reader->numChannels)), length);
        reader->read (&buffer, 0, length, 0, true, buffer.getNumChannels() > 1);
    }
    else if (juce::String error; ! Import::decode (file, buffer, rate, error) || rate <= 0.0)
        return {}; // a phone's m4a goes through the system's decoder

    const int n = buffer.getNumSamples(), channels = buffer.getNumChannels();
    std::vector<float> x ((size_t) n, 0.0f);

    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < n; ++i)
            x[(size_t) i] += buffer.getSample (c, i) / (float) channels;

    return hear (x.data(), n, rate);
}

namespace
{
// The tempo the onsets were played at, from the take alone: the beat that onsets most often
// sit one or two of apart. The loud ones count for more: they're the pulse (a backbeat), and the
// quiet ones between are played loosely (a fill rushes). Between 75 and 150 bpm (one octave:
// half or double fits as well).
double tempoOf (const std::vector<double>& times, const std::vector<double>& weights)
{
    if (times.size() < 3)
        return 120.0;

    double best = 120.0, bestScore = -1.0;

    for (double bpm = 75.0; bpm < 150.0; bpm += 0.25)
    {
        const double beat = 60.0 / bpm, slack = 0.03; // a gap within about 30 ms of the beat counts
        double score = 0.0;

        for (size_t i = 0; i < times.size(); ++i)
            for (size_t j = i + 1; j < times.size() && times[j] - times[i] < 2.0 * beat + 4.0 * slack; ++j)
                for (const double lag : { beat, 2.0 * beat })
                {
                    const double off = (times[j] - times[i] - lag) / slack;
                    score += weights[i] * weights[j] * std::exp (-0.5 * off * off);
                }

        if (score > bestScore)
        {
            bestScore = score;
            best = bpm;
        }
    }

    return best;
}

// Each onset's place on the grid of sixteenths, counted from the first. The grid follows the
// player: after each onset it's pulled half way to where the onset really fell, and its step
// leans towards the gap just played.
std::vector<int> stepsOf (const std::vector<double>& times, double step)
{
    const double slowest = step * 1.1, fastest = step * 0.9; // drift, not another tempo
    std::vector<int> steps;
    double anchorTime = times.empty() ? 0.0 : times.front();
    int anchorStep = 0;

    for (size_t i = 0; i < times.size(); ++i)
    {
        if (i == 0)
        {
            steps.push_back (0);
            continue;
        }

        const double raw = anchorStep + (times[i] - anchorTime) / step;
        const int at = std::max (steps.back() + 1, juce::roundToInt (raw));
        const double late = raw - at; // in steps
        step = juce::jlimit (fastest, slowest, step * (1.0 + 0.3 * late / std::max (1, at - anchorStep)));
        anchorTime = times[i] - 0.5 * late * step;
        anchorStep = at;
        steps.push_back (at);
    }

    return steps;
}
// A beat's hits on the grid. Its loudest hits are its pulse (the backbeat, the downbeats): when
// they're a beat or more apart they go on beats first, a whole number of beats from one to the
// next, and the hits between are placed by how far along they fall. That holds where placing
// hit after hit doesn't: the quiet ones are played loosely, and their small errors add up.
// Without such a pulse (every hit as loud as the next), hit after hit it is.
std::vector<int> beatSteps (const std::vector<HumHit>& hits, double nearBpm, double& bpm, double& origin)
{
    std::vector<double> times, weights;
    double loudest = -200.0;

    for (const auto& hit : hits)
    {
        times.push_back (hit.time);
        weights.push_back (std::pow (10.0, hit.levelDb / 20.0));
        loudest = std::max (loudest, hit.levelDb);
    }

    std::vector<size_t> pulse;
    std::vector<double> pulseTimes;

    for (size_t i = 0; i < hits.size(); ++i)
        if (hits[i].levelDb >= loudest - 4.0)
        {
            pulse.push_back (i);
            pulseTimes.push_back (times[i]);
        }

    double beat = nearBpm > 0.0 ? 60.0 / nearBpm : 0.0;
    bool pulsed = pulse.size() >= 3;

    if (pulsed && beat <= 0.0)
    {
        // The beat the pulse's gaps are most nearly whole numbers of.
        double least = 1.0e9;

        for (double b = 0.4; b <= 0.8; b += 0.0025)
        {
            double off = 0.0;

            for (size_t k = 0; k + 1 < pulseTimes.size(); ++k)
            {
                const double beats = (pulseTimes[k + 1] - pulseTimes[k]) / b;
                off += std::abs (beats - std::max (1.0, std::round (beats)));
            }

            if (off < least)
            {
                least = off;
                beat = b;
            }
        }
    }

    for (size_t k = 0; pulsed && k + 1 < pulseTimes.size(); ++k)
        pulsed = pulseTimes[k + 1] - pulseTimes[k] >= 0.75 * beat;

    if (! pulsed)
    {
        bpm = nearBpm > 0.0 ? nearBpm : tempoOf (times, weights);
        origin = times.front();
        return stepsOf (times, 15.0 / bpm);
    }

    bpm = 60.0 / beat;
    const auto pulseBeats = stepsOf (pulseTimes, beat);
    std::vector<int> steps (hits.size(), 0);

    for (size_t k = 0; k < pulse.size(); ++k)
        steps[pulse[k]] = 4 * pulseBeats[k];

    for (size_t k = 0; k + 1 < pulse.size(); ++k)
        for (size_t i = pulse[k] + 1; i < pulse[k + 1]; ++i)
        {
            const double along = (times[i] - pulseTimes[k]) / (pulseTimes[k + 1] - pulseTimes[k]);
            steps[i] = steps[pulse[k]] + juce::roundToInt (along * (steps[pulse[k + 1]] - steps[pulse[k]]));
        }

    // Before the pulse starts, counted back at its first pace; after it ends, hit after hit at its last.
    const double early = (pulseTimes[1] - pulseTimes[0]) / (steps[pulse[1]] - steps[pulse[0]]);

    for (size_t i = 0; i < pulse.front(); ++i)
        steps[i] = -std::max (1, juce::roundToInt ((pulseTimes.front() - times[i]) / early));

    const size_t last = pulse.back(), before = pulse[pulse.size() - 2];
    const auto after = stepsOf ({ times.begin() + (std::ptrdiff_t) last, times.end() }, (times[last] - times[before]) / (steps[last] - steps[before]));

    for (size_t i = last; i < hits.size(); ++i)
        steps[i] = steps[last] + after[i - last];

    // From 0, with the pulse still on the beat.
    const int lead = 4 * (int) std::ceil (-*std::min_element (steps.begin(), steps.end()) / 4.0);

    for (auto& s : steps)
        s += lead;

    origin = pulseTimes.front() - steps[pulse.front()] * early;
    return steps;
}
} // namespace

HumPart arrange (const Heard& heard, bool asBeat, double nearBpm, double downbeat)
{
    HumPart part;

    if (asBeat ? heard.hits.empty() : heard.notes.empty())
        return part;

    std::vector<int> steps;

    if (downbeat >= 0.0 && nearBpm > 0.0)
    {
        // Made to a click: the tempo and where beat one fell are known, and each onset goes
        // where it falls from there (the grid still follows the player's drift).
        std::vector<double> times;

        if (asBeat)
            for (const auto& hit : heard.hits)
                times.push_back (hit.time);
        else
            for (const auto& note : heard.notes)
                times.push_back (note.start);

        part.bpm = nearBpm;
        part.firstOnset = downbeat;
        const double step = 15.0 / nearBpm;
        const bool rest = times.front() > downbeat + 0.5 * step; // it doesn't start on beat one

        if (rest)
            times.insert (times.begin(), downbeat);

        steps = stepsOf (times, step);

        if (rest)
            steps.erase (steps.begin());
    }
    else if (asBeat)
        steps = beatSteps (heard.hits, nearBpm, part.bpm, part.firstOnset);
    else
    {
        // A sung note's level says little about the pulse, and a few notes little about the
        // tempo: hummed to a session's tempo, that's the tempo (the grid follows the drift).
        std::vector<double> times;

        for (const auto& note : heard.notes)
            times.push_back (note.start);

        part.bpm = nearBpm > 0.0 ? nearBpm : tempoOf (times, std::vector<double> (times.size(), 1.0));
        part.firstOnset = times.front();
        steps = stepsOf (times, 15.0 / part.bpm);
    }

    const double step = 15.0 / part.bpm;

    if (asBeat)
    {
        double loudest = -200.0;

        for (const auto& hit : heard.hits)
            loudest = std::max (loudest, hit.levelDb);

        for (size_t i = 0; i < heard.hits.size(); ++i)
        {
            const auto& hit = heard.hits[i];
            // The loudest is a full hit; 20 dB under it, a ghost.
            part.hits.push_back ({ steps[i] * 0.25, (float) juce::jlimit (0.25, 1.0, 1.0 + (hit.levelDb - loudest) / 26.0), hit.kind });
        }
    }
    else
    {
        double loudest = -200.0;

        for (const auto& note : heard.notes)
            loudest = std::max (loudest, note.levelDb);

        for (size_t i = 0; i < heard.notes.size(); ++i)
        {
            const auto& note = heard.notes[i];
            // As long as it was held, on the grid, and out of the next note's way (one voice).
            int length = std::max (1, juce::roundToInt (note.length / step));

            if (i + 1 < steps.size())
                length = std::min (length, steps[i + 1] - steps[i]);

            part.notes.push_back ({ steps[i] * 0.25, length * 0.25, note.pitch,
                                    juce::jlimit (50, 115, juce::roundToInt (110.0 + 2.5 * (note.levelDb - loudest))) });
        }
    }

    const double end = asBeat ? part.hits.back().beat + 0.25 : part.notes.back().beat + part.notes.back().length;
    part.beats = 4.0 * std::ceil (end / 4.0);
    return part;
}

double humBeat (double seconds, double bpm, double grid)
{
    const double beat = seconds * bpm / 60.0;
    return grid > 0.0 ? std::round (beat / grid) * grid : beat;
}
} // namespace taw
