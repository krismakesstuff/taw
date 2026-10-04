// The sample library: measuring samples (loudness, envelope, pitch, spectrum), sorting them by
// kind, and finding the ones that sound alike.

#include "Library.h"

#include <juce_dsp/juce_dsp.h>

#include <atomic>
#include <map>
#include <numeric>
#include <thread>

namespace taw
{
namespace
{
constexpr double maxSeconds = 10.0; // one-shots; a long file is measured on its start
constexpr double bandEdges[SampleInfo::numBands + 1] = { 0, 60, 120, 250, 500, 1000, 2000, 4000, 8000, 1.0e9 };

double toDb (double gain) { return 20.0 * std::log10 (std::max (gain, 1.0e-5)); }

// Spectrum, brightness, flatness, bands and MFCCs from the energy-weighted average of 2048-point
// frames over the part that sounds.
void measureSpectrum (const float* x, int n, double rate, SampleInfo& info)
{
    constexpr int order = 11, size = 1 << order, hop = size / 4, maxFrames = 200;
    juce::dsp::FFT fft (order);
    std::vector<float> window ((size_t) size), frame ((size_t) size * 2);
    std::vector<double> power ((size_t) size / 2 + 1, 0.0);

    for (int i = 0; i < size; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (size - 1));

    for (int start = 0, frames = 0; frames == 0 || (start < n && frames < maxFrames); start += hop, ++frames)
    {
        std::fill (frame.begin(), frame.end(), 0.0f);

        for (int i = 0; i < size && start + i < n; ++i)
            frame[(size_t) i] = x[start + i] * window[(size_t) i];

        fft.performFrequencyOnlyForwardTransform (frame.data(), true);

        for (size_t k = 0; k < power.size(); ++k)
            power[k] += (double) frame[k] * (double) frame[k];
    }

    const double binHz = rate / size;
    double total = 0.0, weighted = 0.0, logSum = 0.0;
    int flatBins = 0;
    std::array<double, SampleInfo::numBands> bands {};

    for (size_t k = 1; k < power.size(); ++k)
    {
        const double f = (double) k * binHz, p = power[k];
        total += p;
        weighted += f * p;

        for (int b = 0; b < SampleInfo::numBands; ++b)
            if (f >= bandEdges[b] && f < bandEdges[b + 1])
                bands[(size_t) b] += p;

        if (f >= 20.0 && f <= 16000.0)
        {
            logSum += std::log (p + 1.0e-12);
            ++flatBins;
        }
    }

    if (total <= 0.0)
        return;

    info.centroidHz = weighted / total;

    double flatMean = 0.0;

    for (size_t k = 1; k < power.size(); ++k)
        if (const double f = (double) k * binHz; f >= 20.0 && f <= 16000.0)
            flatMean += power[k];

    flatMean /= std::max (1, flatBins);
    info.flatness = flatBins > 0 ? std::exp (logSum / flatBins) / (flatMean + 1.0e-12) : 0.0;

    for (double acc = 0.0, k = 1; k < (double) power.size(); ++k)
        if ((acc += power[(size_t) k]) >= 0.85 * total)
        {
            info.rolloffHz = k * binHz;
            break;
        }

    for (int b = 0; b < SampleInfo::numBands; ++b)
        info.bandsDb[(size_t) b] = (float) (10.0 * std::log10 (bands[(size_t) b] / total + 1.0e-9));

    // MFCCs: 26 mel bands from 20 Hz to 16 kHz (or Nyquist), log, DCT.
    constexpr int melBands = 26;
    auto mel = [] (double f) { return 2595.0 * std::log10 (1.0 + f / 700.0); };
    auto hz = [] (double m) { return 700.0 * (std::pow (10.0, m / 2595.0) - 1.0); };
    const double lo = mel (20.0), hi = mel (std::min (16000.0, rate / 2.0));
    std::array<double, melBands> logMel {};

    for (int m = 0; m < melBands; ++m)
    {
        const double f0 = hz (lo + (hi - lo) * m / (melBands + 1)), f1 = hz (lo + (hi - lo) * (m + 1) / (melBands + 1)),
                     f2 = hz (lo + (hi - lo) * (m + 2) / (melBands + 1));
        double e = 0.0;

        for (size_t k = 1; k < power.size(); ++k)
        {
            const double f = (double) k * binHz;

            if (f > f0 && f < f2)
                e += power[k] * (f < f1 ? (f - f0) / (f1 - f0) : (f2 - f) / (f2 - f1));
        }

        logMel[(size_t) m] = std::log (e / total + 1.0e-10);
    }

    for (int c = 0; c < (int) info.mfcc.size(); ++c)
    {
        double s = 0.0;

        for (int m = 0; m < melBands; ++m)
            s += logMel[(size_t) m] * std::cos (juce::MathConstants<double>::pi * c * (m + 0.5) / melBands);

        info.mfcc[(size_t) c] = (float) s;
    }
}

// YIN on a few windows after the attack: the fundamental and how clearly it's there.
void measurePitch (const float* x, int n, int from, int to, double rate, SampleInfo& info)
{
    const int tauMin = std::max (2, (int) (rate / 2000.0)), tauMax = (int) (rate / 30.0);
    const int width = (int) juce::nextPowerOfTwo (2 * tauMax), span = width - tauMax;
    std::vector<double> d ((size_t) tauMax + 1);

    for (int start = from, tries = 0; tries < 3 && start + width <= std::min (n, std::max (to, from + width)); start += width / 2, ++tries)
    {
        d[0] = 1.0;
        double running = 0.0;

        for (int tau = 1; tau <= tauMax; ++tau)
        {
            double s = 0.0;

            for (int j = 0; j < span; ++j)
            {
                const double diff = (double) x[start + j] - (double) x[start + j + tau];
                s += diff * diff;
            }

            running += s;
            d[(size_t) tau] = running > 0.0 ? s * tau / running : 1.0; // cumulative mean normalised
        }

        int best = -1;

        for (int tau = tauMin; tau < tauMax; ++tau)
            if (d[(size_t) tau] < 0.2 && d[(size_t) tau] <= d[(size_t) tau + 1])
            {
                best = tau;
                break;
            }

        if (best < 0)
            best = (int) (std::min_element (d.begin() + tauMin, d.end()) - d.begin());

        const double confidence = juce::jlimit (0.0, 1.0, 1.0 - d[(size_t) best]);

        if (confidence > info.pitchConfidence)
        {
            // Parabolic interpolation around the dip.
            double tau = best;

            if (best > tauMin && best < tauMax)
            {
                const double a = d[(size_t) best - 1], b = d[(size_t) best], c = d[(size_t) best + 1];

                if (const double den = a - 2.0 * b + c; std::abs (den) > 1.0e-12)
                    tau += 0.5 * (a - c) / den;
            }

            info.pitchHz = rate / tau;
            info.pitchConfidence = confidence;
        }
    }
}

// Names of kinds, from the shorthand sample packs use. Exact tokens are short codes (bd, sd);
// words are looked for anywhere in a token (snare in snaredrum). Earlier entries win.
struct KindWords { const char* kind; std::vector<const char*> exact, words; };

const std::vector<KindWords>& kindWords()
{
    static const std::vector<KindWords> table {
        { "open hat", { "oh", "ohh", "ophat", "ohat", "opn" }, { "openhat", "openhh" } },
        { "kick",     { "bd", "kd", "kck", "bassd", "bdrum", "kk" }, { "kick", "kik", "bassdrum" } },
        { "snare",    { "sd", "sn", "snar", "sna", "snr" }, { "snare" } },
        { "clap",     { "cp", "clp", "hc" }, { "clap" } },
        { "rim",      { "rs", "rm" }, { "rim", "stick" } },
        { "hihat",    { "hh", "ch", "chh", "hht", "cht" }, { "hihat", "hat" } },
        { "cymbal",   { "cy", "cym", "cr", "rd" }, { "cymbal", "crash", "ride", "splash", "china" } },
        { "tom",      { "lt", "mt", "ht", "ft", "tm" }, { "tom" } },
        { "perc",     { "cb", "cl", "ma", "tri", "sh", "ag", "wb", "lc", "mc", "hicon", "locon", "mucon", "con" },
                      { "perc", "conga", "bongo", "tamb", "cowbell", "shaker", "clave", "triangle", "block", "agogo",
                        "cabasa", "guiro", "mara", "timba", "bell", "click", "cow", "snap" } },
    };
    return table;
}

juce::String kindOfText (const juce::String& text)
{
    juce::StringArray tokens;
    juce::String t;

    for (auto c : text.toLowerCase() + " ")
    {
        if (juce::CharacterFunctions::isLetter (c))
            t += c;
        else if (t.isNotEmpty())
        {
            tokens.add (t);
            t.clear();
        }
    }

    // "open hat" in two tokens.
    if (tokens.contains ("open") && (tokens.contains ("hat") || tokens.contains ("hh") || tokens.contains ("hihat")))
        return "open hat";

    for (const auto& k : kindWords())
        for (const auto& token : tokens)
        {
            for (auto* e : k.exact)
                if (token == e)
                    return k.kind;

            for (auto* w : k.words)
                if (token.contains (w))
                    return k.kind;
        }

    return {};
}
} // namespace

juce::String Library::kindFromName (const juce::File& file)
{
    // The file's own name first; then the two folders above it ("BassDrum/808_T5.wav").
    if (auto k = kindOfText (file.getFileNameWithoutExtension()); k.isNotEmpty())
        return k;

    auto dir = file.getParentDirectory();

    for (int up = 0; up < 2 && dir.getParentDirectory() != dir; ++up, dir = dir.getParentDirectory())
        if (auto k = kindOfText (dir.getFileName()); k.isNotEmpty())
            return k;

    return {};
}

std::optional<SampleInfo> Library::analyse (const juce::File& file, juce::AudioFormatManager& formats)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return {};

    SampleInfo info;
    info.path = file.getFullPathName();
    info.size = file.getSize();
    info.modified = file.getLastModificationTime().toMilliseconds();
    info.nameKind = kindFromName (file);

    const double rate = reader->sampleRate;
    info.seconds = (double) reader->lengthInSamples / rate;

    const int n = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) (maxSeconds * rate));
    const int channels = (int) std::max (1u, std::min (2u, reader->numChannels));
    juce::AudioBuffer<float> buffer (channels, n);
    reader->read (&buffer, 0, n, 0, true, channels > 1);

    // Mono: the sum of the channels, halved.
    std::vector<float> x ((size_t) n);

    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < n; ++i)
            x[(size_t) i] += buffer.getSample (c, i) / (float) channels;

    float peak = 0.0f;
    int peakAt = 0;

    for (int i = 0; i < n; ++i)
        if (std::abs (x[(size_t) i]) > peak)
        {
            peak = std::abs (x[(size_t) i]);
            peakAt = i;
        }

    info.peakDb = toDb (peak);

    if (peak < 1.0e-5f)
        return info; // silence

    // Attack: from 10% to 90% of the peak.
    int at10 = 0, at90 = peakAt;

    while (at10 < n && std::abs (x[(size_t) at10]) < 0.1f * peak)
        ++at10;

    at90 = at10;

    while (at90 < n && std::abs (x[(size_t) at90]) < 0.9f * peak)
        ++at90;

    info.attackMs = 1000.0 * (at90 - at10) / rate;

    // A 1 ms peak envelope for the decay, the tail and the hits.
    const int block = std::max (1, (int) (rate / 1000.0));
    std::vector<float> env ((size_t) (n + block - 1) / (size_t) block, 0.0f);

    for (int i = 0; i < n; ++i)
        env[(size_t) (i / block)] = std::max (env[(size_t) (i / block)], std::abs (x[(size_t) i]));

    int last20 = peakAt / block, last40 = peakAt / block;

    for (int b = 0; b < (int) env.size(); ++b)
    {
        if (env[(size_t) b] >= 0.1f * peak)
            last20 = b;

        if (env[(size_t) b] >= 0.01f * peak)
            last40 = b;
    }

    info.decayMs = std::max (0.0, (double) (last20 - peakAt / block));
    info.tailMs = (double) last40 + 1.0;

    // Hits: attacks in the first second, counted above 500 Hz (a kick's slow wave would read as
    // a burst every cycle): the 2 ms RMS jumps 8 dB over its last 16 ms, within 20 dB of the
    // loudest, at least 8 ms after the previous one. A clap has a few.
    {
        juce::IIRFilter highPass;
        highPass.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, 500.0));
        const int len = std::min (n, (int) rate), hb = std::max (1, (int) (rate / 500.0));
        std::vector<float> hp (x.begin(), x.begin() + len), hpEnv ((size_t) (len / hb) + 1, 0.0f);
        highPass.processSamples (hp.data(), len);

        for (int i = 0; i < len; ++i)
            hpEnv[(size_t) (i / hb)] += hp[(size_t) i] * hp[(size_t) i] / (float) hb;

        float loudest = 0.0f;

        for (auto& e : hpEnv)
            loudest = std::max (loudest, e = std::sqrt (e));

        info.hits = 0;

        for (int b = 0, lastHit = -1000; b < (int) hpEnv.size(); ++b)
        {
            float floor = b > 0 ? hpEnv[(size_t) b - 1] : 0.0f;

            for (int k = std::max (0, b - 8); k < b; ++k)
                floor = std::min (floor, hpEnv[(size_t) k]);

            if (hpEnv[(size_t) b] >= 0.1f * loudest && hpEnv[(size_t) b] > 2.5f * floor + 1.0e-7f && b - lastHit >= 4)
            {
                ++info.hits;
                lastHit = b;
            }
        }
    }

    info.hits = std::max (1, info.hits);

    const int soundEnd = std::min (n, (last40 + 1) * block);
    double sum = 0.0;

    for (int i = 0; i < soundEnd; ++i)
        sum += (double) x[(size_t) i] * x[(size_t) i];

    info.rmsDb = toDb (std::sqrt (sum / std::max (1, soundEnd)));
    info.crestDb = info.peakDb - info.rmsDb;

    measureSpectrum (x.data(), soundEnd, rate, info);
    measurePitch (x.data(), n, std::min (n, peakAt + (int) (0.005 * rate)), soundEnd, rate, info);
    return info;
}

//==============================================================================
namespace
{
juce::var toVar (const SampleInfo& s)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("path", s.path);
    o->setProperty ("size", s.size);
    o->setProperty ("modified", s.modified);
    o->setProperty ("seconds", s.seconds);
    o->setProperty ("peakDb", s.peakDb);
    o->setProperty ("rmsDb", s.rmsDb);
    o->setProperty ("crestDb", s.crestDb);
    o->setProperty ("attackMs", s.attackMs);
    o->setProperty ("decayMs", s.decayMs);
    o->setProperty ("tailMs", s.tailMs);
    o->setProperty ("hits", s.hits);
    o->setProperty ("pitchHz", s.pitchHz);
    o->setProperty ("pitchConfidence", s.pitchConfidence);
    o->setProperty ("centroidHz", s.centroidHz);
    o->setProperty ("flatness", s.flatness);
    o->setProperty ("rolloffHz", s.rolloffHz);

    juce::Array<juce::var> bands, mfcc;

    for (auto b : s.bandsDb)
        bands.add (b);

    for (auto m : s.mfcc)
        mfcc.add (m);

    o->setProperty ("bandsDb", bands);
    o->setProperty ("mfcc", mfcc);
    o->setProperty ("mapX", s.mapX);
    o->setProperty ("mapY", s.mapY);
    o->setProperty ("kindX", s.kindX);
    o->setProperty ("kindY", s.kindY);
    o->setProperty ("kind", s.kind()); // for anything reading the index; taw works it out again
    o->setProperty ("byName", s.nameKind.isNotEmpty());
    o->setProperty ("soundKind", s.soundKind);
    o->setProperty ("soundKindScore", s.soundKindScore);
    return o;
}

SampleInfo fromVar (const juce::var& v)
{
    SampleInfo s;
    s.path = v["path"];
    s.size = (juce::int64) v["size"];
    s.modified = (juce::int64) v["modified"];
    s.seconds = v["seconds"];
    s.peakDb = v["peakDb"];
    s.rmsDb = v["rmsDb"];
    s.crestDb = v["crestDb"];
    s.attackMs = v["attackMs"];
    s.decayMs = v["decayMs"];
    s.tailMs = v["tailMs"];
    s.hits = v["hits"];
    s.pitchHz = v["pitchHz"];
    s.pitchConfidence = v["pitchConfidence"];
    s.centroidHz = v["centroidHz"];
    s.flatness = v["flatness"];
    s.rolloffHz = v["rolloffHz"];
    s.mapX = v["mapX"];
    s.mapY = v["mapY"];
    s.kindX = v["kindX"];
    s.kindY = v["kindY"];

    for (int i = 0; i < SampleInfo::numBands && i < v["bandsDb"].size(); ++i)
        s.bandsDb[(size_t) i] = v["bandsDb"][i];

    for (int i = 0; i < (int) s.mfcc.size() && i < v["mfcc"].size(); ++i)
        s.mfcc[(size_t) i] = v["mfcc"][i];

    s.nameKind = Library::kindFromName (juce::File (s.path)); // names are cheap; rules may improve
    s.soundKind = v["soundKind"].toString();
    s.soundKindScore = v["soundKindScore"];
    return s;
}
} // namespace

Library::Library (juce::File file) : indexFile (std::move (file))
{
    if (const auto v = juce::JSON::parse (indexFile); v["samples"].isArray())
        for (const auto& s : *v["samples"].getArray())
            items.push_back (fromVar (s));
}

bool Library::save() const
{
    juce::Array<juce::var> list;

    for (const auto& s : items)
        list.add (toVar (s));

    auto* root = new juce::DynamicObject();
    root->setProperty ("version", 1);
    root->setProperty ("samples", list);
    indexFile.getParentDirectory().createDirectory();
    return indexFile.replaceWithText (juce::JSON::toString (juce::var (root), true));
}

void Library::scan (const juce::Array<juce::File>& folders, std::function<void (int, int)> progress)
{
    std::map<juce::String, SampleInfo> known;

    for (auto& s : items)
        known[s.path] = s;

    // Every audio file under the folders, but not the ._ files a Mac leaves on shared drives.
    juce::Array<juce::File> files;

    for (const auto& folder : folders)
        for (const auto& f : folder.findChildFiles (juce::File::findFiles, true, "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg"))
            if (! f.getFileName().startsWith ("._"))
                files.add (f);

    std::vector<SampleInfo> results ((size_t) files.size());
    std::vector<char> ok ((size_t) files.size(), 0);
    std::vector<int> todo;

    for (int i = 0; i < files.size(); ++i)
    {
        const auto& f = files.getReference (i);

        if (auto it = known.find (f.getFullPathName());
            it != known.end() && it->second.size == f.getSize() && it->second.modified == f.getLastModificationTime().toMilliseconds())
        {
            results[(size_t) i] = it->second;
            ok[(size_t) i] = 1;
        }
        else
        {
            todo.push_back (i);
        }
    }

    std::atomic<int> next { 0 }, done { 0 };
    std::vector<std::thread> workers;

    for (unsigned w = 0; w < std::max (1u, std::thread::hardware_concurrency()); ++w)
        workers.emplace_back ([&]
        {
            juce::AudioFormatManager formats; // one each: readers aren't shared across threads
            formats.registerBasicFormats();

            for (int t; (t = next++) < (int) todo.size();)
            {
                const int i = todo[(size_t) t];

                if (auto info = analyse (files.getReference (i), formats))
                {
                    results[(size_t) i] = std::move (*info);
                    ok[(size_t) i] = 1;
                }

                ++done;
            }
        });

    while (done < (int) todo.size())
    {
        if (progress)
            progress (done, (int) todo.size());

        std::this_thread::sleep_for (std::chrono::milliseconds (200));
    }

    for (auto& w : workers)
        w.join();

    // What was indexed outside these folders stays; what's inside them is now what's there.
    std::vector<SampleInfo> kept;

    for (auto& s : items)
    {
        const juce::File f (s.path);
        bool inside = false;

        for (const auto& folder : folders)
            inside = inside || f.isAChildOf (folder);

        if (! inside)
            kept.push_back (std::move (s));
    }

    for (size_t i = 0; i < results.size(); ++i)
        if (ok[i])
            kept.push_back (std::move (results[i]));

    items = std::move (kept);
}

//==============================================================================
// The measurements that tell drums apart, each scaled to the spread across the library, then
// weighted: the envelope and brightness say the most, the bands and MFCCs fill in timbre.
std::vector<float> Library::features (const SampleInfo& s) const
{
    std::vector<float> f;
    auto add = [&f] (double v) { f.push_back ((float) v); };

    add (std::log (s.attackMs + 0.5));
    add (std::log (s.decayMs + 1.0));
    add (std::log (s.tailMs + 1.0));
    add (s.crestDb);
    add (std::log (s.centroidHz + 20.0));
    add (std::log (s.rolloffHz + 20.0));
    add (s.flatness);
    add (std::log ((double) s.hits));

    for (auto b : s.bandsDb)
        add (b);

    for (size_t i = 1; i < s.mfcc.size(); ++i)
        add (s.mfcc[i]);

    return f;
}

void Library::computeScaling()
{
    mean.clear();
    spread.clear();

    if (items.empty())
        return;

    const auto dims = features (items.front()).size();
    mean.assign (dims, 0.0f);
    spread.assign (dims, 0.0f);

    for (const auto& s : items)
    {
        const auto f = features (s);

        for (size_t d = 0; d < dims; ++d)
            mean[d] += f[d] / (float) items.size();
    }

    for (const auto& s : items)
    {
        const auto f = features (s);

        for (size_t d = 0; d < dims; ++d)
            spread[d] += (f[d] - mean[d]) * (f[d] - mean[d]) / (float) items.size();
    }

    // Weights: envelope 1.5, the spectral summary 1.2, bands 0.6, MFCCs 0.5.
    for (size_t d = 0; d < dims; ++d)
    {
        const float weight = d < 4 ? 1.5f : d < 8 ? 1.2f : d < 8 + SampleInfo::numBands ? 0.6f : 0.5f;
        spread[d] = std::sqrt (std::max (spread[d], 1.0e-9f)) / weight;
    }
}

namespace
{
float distance (const std::vector<float>& a, const std::vector<float>& b)
{
    float s = 0.0f;

    for (size_t d = 0; d < a.size(); ++d)
        s += (a[d] - b[d]) * (a[d] - b[d]);

    return s;
}
} // namespace

double Library::classify (int k)
{
    computeScaling();
    std::vector<std::vector<float>> scaled;

    for (const auto& s : items)
    {
        auto f = features (s);

        for (size_t d = 0; d < f.size(); ++d)
            f[d] = (f[d] - mean[d]) / spread[d];

        scaled.push_back (std::move (f));
    }

    int named = 0, agreed = 0;

    for (size_t i = 0; i < items.size(); ++i)
    {
        // The k nearest samples whose names say what they are (never itself).
        std::vector<std::pair<float, size_t>> near;

        for (size_t j = 0; j < items.size(); ++j)
            if (j != i && items[j].nameKind.isNotEmpty())
                near.push_back ({ distance (scaled[i], scaled[j]), j });

        const auto take = std::min ((size_t) k, near.size());
        std::partial_sort (near.begin(), near.begin() + (long) take, near.end());

        std::map<juce::String, double> votes;
        double total = 0.0;

        for (size_t v = 0; v < take; ++v)
        {
            const double w = 1.0 / (std::sqrt (near[v].first) + 0.1);
            votes[items[near[v].second].nameKind] += w;
            total += w;
        }

        auto& s = items[i];
        s.soundKind.clear();
        s.soundKindScore = 0.0;

        for (const auto& [kind, w] : votes)
            if (w > s.soundKindScore * total)
            {
                s.soundKind = kind;
                s.soundKindScore = w / total;
            }

        if (s.nameKind.isNotEmpty())
        {
            ++named;
            agreed += s.soundKind == s.nameKind ? 1 : 0;
        }
    }

    return named > 0 ? (double) agreed / named : 0.0;
}

std::vector<int> Library::nearest (int i, int n) const
{
    std::vector<std::pair<float, int>> all;
    auto scale = [this] (const SampleInfo& s)
    {
        auto f = features (s);

        for (size_t d = 0; d < f.size() && d < mean.size(); ++d)
            f[d] = (f[d] - mean[d]) / spread[d];

        return f;
    };

    const auto me = scale (items[(size_t) i]);

    for (int j = 0; j < (int) items.size(); ++j)
        if (j != i)
            all.push_back ({ distance (me, scale (items[(size_t) j])), j });

    const auto take = std::min ((size_t) n, all.size());
    std::partial_sort (all.begin(), all.begin() + (long) take, all.end());

    std::vector<int> out;

    for (size_t t = 0; t < take; ++t)
        out.push_back (all[t].second);

    return out;
}
// t-SNE, exact (fine to a few thousand samples; a bigger library wants Barnes-Hut): neighbours in
// the measurements stay neighbours on the map. Perplexity 30, early exaggeration for the first
// 250 steps, then the map is scaled to 0..1.
namespace
{
// Positions (0..1) for the scaled measurements, one pair per row.
std::vector<std::pair<float, float>> tsne (const std::vector<std::vector<float>>& scaled, int iterations)
{
    const int n = (int) scaled.size();

    if (n < 3)
        return std::vector<std::pair<float, float>> ((size_t) n, { 0.5f, 0.5f });

    // Too many for the exact form (n^2 memory): until Barnes-Hut, the first two measurements'
    // ranks (the envelope) spread them out instead.
    if (n > 3000)
    {
        std::vector<int> byX ((size_t) n), byY ((size_t) n);
        std::iota (byX.begin(), byX.end(), 0);
        byY = byX;
        std::sort (byX.begin(), byX.end(), [&] (int a, int b) { return scaled[(size_t) a][4] < scaled[(size_t) b][4]; }); // brightness
        std::sort (byY.begin(), byY.end(), [&] (int a, int b) { return scaled[(size_t) a][2] > scaled[(size_t) b][2]; }); // tail, longest at the top
        std::vector<std::pair<float, float>> out ((size_t) n);

        for (int r = 0; r < n; ++r)
        {
            out[(size_t) byX[(size_t) r]].first = (float) r / (float) (n - 1);
            out[(size_t) byY[(size_t) r]].second = (float) r / (float) (n - 1);
        }

        return out;
    }

    std::vector<double> dist ((size_t) n * (size_t) n), p ((size_t) n * (size_t) n, 0.0);

    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            dist[(size_t) (i * n + j)] = distance (scaled[(size_t) i], scaled[(size_t) j]);

    // Each sample's neighbourhood: a Gaussian whose width gives the wanted perplexity.
    const double perplexity = std::min (30.0, (n - 1) / 3.0), target = std::log (perplexity);

    for (int i = 0; i < n; ++i)
    {
        double lo = 0.0, hi = 1.0e12, beta = 1.0;

        for (int step = 0; step < 60; ++step)
        {
            double sum = 0.0, entropy = 0.0;

            for (int j = 0; j < n; ++j)
                if (j != i)
                    sum += p[(size_t) (i * n + j)] = std::exp (-beta * dist[(size_t) (i * n + j)]);

            sum = std::max (sum, 1.0e-300);

            for (int j = 0; j < n; ++j)
                if (j != i)
                {
                    auto& v = p[(size_t) (i * n + j)];
                    v /= sum;
                    entropy -= v > 1.0e-300 ? v * std::log (v) : 0.0;
                }

            if (std::abs (entropy - target) < 1.0e-4)
                break;

            if (entropy > target) { lo = beta; beta = hi >= 1.0e12 ? beta * 2.0 : (beta + hi) / 2.0; }
            else                  { hi = beta; beta = (beta + lo) / 2.0; }
        }
    }

    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
        {
            const double v = std::max ((p[(size_t) (i * n + j)] + p[(size_t) (j * n + i)]) / (2.0 * n), 1.0e-12);
            p[(size_t) (i * n + j)] = p[(size_t) (j * n + i)] = v;
        }

    juce::Random rng (1);
    std::vector<double> y ((size_t) n * 2), gain ((size_t) n * 2, 1.0), velocity ((size_t) n * 2, 0.0), grad ((size_t) n * 2), q ((size_t) n * (size_t) n);

    for (auto& v : y)
        v = (rng.nextDouble() - 0.5) * 1.0e-2;

    for (int it = 0; it < iterations; ++it)
    {
        const double exaggeration = it < 250 ? 12.0 : 1.0, momentum = it < 250 ? 0.5 : 0.8;
        double qSum = 0.0;

        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j)
            {
                const double dx = y[(size_t) i * 2] - y[(size_t) j * 2], dy = y[(size_t) i * 2 + 1] - y[(size_t) j * 2 + 1];
                const double v = 1.0 / (1.0 + dx * dx + dy * dy);
                q[(size_t) (i * n + j)] = q[(size_t) (j * n + i)] = v;
                qSum += 2.0 * v;
            }

        std::fill (grad.begin(), grad.end(), 0.0);

        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                if (j != i)
                {
                    const double v = q[(size_t) (i * n + j)];
                    const double m = 4.0 * (exaggeration * p[(size_t) (i * n + j)] - v / qSum) * v;
                    grad[(size_t) i * 2] += m * (y[(size_t) i * 2] - y[(size_t) j * 2]);
                    grad[(size_t) i * 2 + 1] += m * (y[(size_t) i * 2 + 1] - y[(size_t) j * 2 + 1]);
                }

        for (size_t k = 0; k < y.size(); ++k)
        {
            gain[k] = (grad[k] > 0.0) != (velocity[k] > 0.0) ? gain[k] + 0.2 : std::max (0.01, gain[k] * 0.8);
            velocity[k] = momentum * velocity[k] - 200.0 * gain[k] * grad[k];
            y[k] += velocity[k];
        }
    }

    double x0 = 1.0e300, x1 = -1.0e300, y0 = 1.0e300, y1 = -1.0e300;

    for (int i = 0; i < n; ++i)
    {
        x0 = std::min (x0, y[(size_t) i * 2]); x1 = std::max (x1, y[(size_t) i * 2]);
        y0 = std::min (y0, y[(size_t) i * 2 + 1]); y1 = std::max (y1, y[(size_t) i * 2 + 1]);
    }

    std::vector<std::pair<float, float>> out;

    for (int i = 0; i < n; ++i)
        out.push_back ({ (float) ((y[(size_t) i * 2] - x0) / std::max (1.0e-9, x1 - x0)),
                         (float) ((y[(size_t) i * 2 + 1] - y0) / std::max (1.0e-9, y1 - y0)) });

    return out;
}
} // namespace

void Library::layout (int iterations)
{
    if (items.empty())
        return;

    if (mean.empty())
        computeScaling();

    std::vector<std::vector<float>> scaled;

    for (const auto& s : items)
    {
        auto f = features (s);

        for (size_t d = 0; d < f.size(); ++d)
            f[d] = (f[d] - mean[d]) / spread[d];

        scaled.push_back (std::move (f));
    }

    // The whole library, then each kind on its own, so a kind's map spreads its samples out.
    const auto all = tsne (scaled, iterations);

    for (size_t i = 0; i < items.size(); ++i)
        std::tie (items[i].mapX, items[i].mapY) = all[i];

    std::map<juce::String, std::vector<size_t>> byKind;

    for (size_t i = 0; i < items.size(); ++i)
        byKind[items[i].kind()].push_back (i);

    for (const auto& [kind, members] : byKind)
    {
        std::vector<std::vector<float>> subset;

        for (auto i : members)
            subset.push_back (scaled[i]);

        const auto placed = tsne (subset, iterations);

        for (size_t m = 0; m < members.size(); ++m)
            std::tie (items[members[m]].kindX, items[members[m]].kindY) = placed[m];
    }
}

} // namespace taw
