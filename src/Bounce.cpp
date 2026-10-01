#include "Bounce.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace taw
{
namespace
{
constexpr int blockSize = 8192;

template <typename T>
struct ComPtr
{
    T* p = nullptr;
    ~ComPtr() { if (p != nullptr) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

// MP3 via the Media Foundation encoder that ships with Windows: no extra installs.
class Mp3Writer
{
public:
    Mp3Writer (const juce::File& file, double sampleRate, juce::String& error)
        : rate (sampleRate)
    {
        if (FAILED (CoInitializeEx (nullptr, COINIT_MULTITHREADED)) || FAILED (MFStartup (MF_VERSION)))
        {
            error = "Media Foundation is not available";
            return;
        }

        started = true;

        if (sampleRate != 44100.0 && sampleRate != 48000.0 && sampleRate != 32000.0)
        {
            error = "MP3 needs a 32, 44.1 or 48 kHz session";
            return;
        }

        ComPtr<IMFMediaType> outType, inType;

        auto ok = SUCCEEDED (MFCreateSinkWriterFromURL (file.getFullPathName().toWideCharPointer(), nullptr, nullptr, &writer))
               && SUCCEEDED (MFCreateMediaType (&outType))
               && SUCCEEDED (outType->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio))
               && SUCCEEDED (outType->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_MP3))
               && SUCCEEDED (outType->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32) sampleRate))
               && SUCCEEDED (outType->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, 2))
               && SUCCEEDED (outType->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 320000 / 8))
               && SUCCEEDED (writer->AddStream (outType.p, &stream))
               && SUCCEEDED (MFCreateMediaType (&inType))
               && SUCCEEDED (inType->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio))
               && SUCCEEDED (inType->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_PCM))
               && SUCCEEDED (inType->SetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, 16))
               && SUCCEEDED (inType->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32) sampleRate))
               && SUCCEEDED (inType->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, 2))
               && SUCCEEDED (inType->SetUINT32 (MF_MT_AUDIO_BLOCK_ALIGNMENT, 4))
               && SUCCEEDED (inType->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32) sampleRate * 4))
               && SUCCEEDED (writer->SetInputMediaType (stream, inType.p, nullptr))
               && SUCCEEDED (writer->BeginWriting());

        if (! ok)
            error = "Windows MP3 encoder refused the format";
    }

    ~Mp3Writer()
    {
        if (writer.p != nullptr)
        {
            if (! finalised)
                writer->Finalize();

            writer.p->Release(); // before MFShutdown
            writer.p = nullptr;
        }

        if (started)
        {
            MFShutdown();
            CoUninitialize();
        }
    }

    bool write (const juce::AudioBuffer<float>& block, int n)
    {
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFSample> sample;
        const auto bytes = (DWORD) n * 4;

        if (FAILED (MFCreateMemoryBuffer (bytes, &buffer)))
            return false;

        BYTE* data = nullptr;
        buffer->Lock (&data, nullptr, nullptr);
        auto* pcm = reinterpret_cast<int16_t*> (data);

        for (int i = 0; i < n; ++i)
            for (int ch = 0; ch < 2; ++ch)
                pcm[i * 2 + ch] = (int16_t) juce::roundToInt (juce::jlimit (-1.0f, 1.0f, block.getSample (ch, i)) * 32767.0f);

        buffer->Unlock();
        buffer->SetCurrentLength (bytes);

        // Media Foundation times are in 100 ns units.
        const auto start = (LONGLONG) ((double) written * 1.0e7 / rate);
        const auto duration = (LONGLONG) ((double) (written + n) * 1.0e7 / rate) - start;
        written += n;

        return SUCCEEDED (MFCreateSample (&sample)) && SUCCEEDED (sample->AddBuffer (buffer.p))
               && SUCCEEDED (sample->SetSampleTime (start)) && SUCCEEDED (sample->SetSampleDuration (duration))
               && SUCCEEDED (writer->WriteSample (stream, sample.p));
    }

    bool finish()
    {
        finalised = true;
        return SUCCEEDED (writer->Finalize());
    }

private:
    double rate;
    bool started = false, finalised = false;
    ComPtr<IMFSinkWriter> writer;
    DWORD stream = 0;
    juce::int64 written = 0;
};
} // namespace

Bounce::Bounce (std::unique_ptr<Graph> g, juce::Range<juce::int64> r, double rate, Format f, juce::File file)
    : juce::Thread ("taw bounce"), graph (std::move (g)), range (r), sampleRate (rate), format (f), target (std::move (file))
{
    bus.setSize (2, blockSize);
}

Bounce::~Bounce()
{
    stopThread (10000);
}

bool Bounce::renderBlock (juce::int64 pos, int n, juce::AudioBuffer<float>& out)
{
    out.clear();
    auto* l = out.getWritePointer (0);
    auto* r = out.getWritePointer (1);
    auto* bl = bus.getWritePointer (0);
    auto* br = bus.getWritePointer (1);

    for (const auto& tp : graph->tracks)
    {
        if (! tp.audible)
            continue;

        juce::FloatVectorOperations::clear (bl, n);
        juce::FloatVectorOperations::clear (br, n);
        renderClips (tp, pos, n, bl, br);
        renderHits (tp, pos, n, bl, br);

        const auto g = tp.gain * (isStereo (tp) ? 1.0f : centrePan);
        juce::FloatVectorOperations::addWithMultiply (l, bl, g, n);
        juce::FloatVectorOperations::addWithMultiply (r, br, g, n);
    }

    out.applyGain (0, n, graph->mixGain);

    for (int ch = 0; ch < 2; ++ch)
        peakValue = juce::jmax (peakValue, out.getMagnitude (ch, 0, n));

    return true;
}

void Bounce::run()
{
    target.getParentDirectory().createDirectory();
    // Write under a temporary name; keep the extension, Media Foundation picks the container from it.
    const auto partial = target.getSiblingFile (target.getFileNameWithoutExtension() + ".part" + target.getFileExtension());
    juce::AudioBuffer<float> block (2, blockSize);

    std::unique_ptr<juce::AudioFormatWriter> wav;
    std::unique_ptr<Mp3Writer> mp3;

    if (format == Format::wav)
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (partial);
        wav = juce::WavAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                                  .withSampleRate (sampleRate)
                                                                  .withNumChannels (2)
                                                                  .withBitsPerSample (24));
        if (wav == nullptr)
            error = "can't write " + target.getFullPathName();
    }
    else
    {
        mp3 = std::make_unique<Mp3Writer> (partial, sampleRate, error);
    }

    for (auto pos = range.getStart(); error.isEmpty() && pos < range.getEnd(); pos += blockSize)
    {
        if (threadShouldExit())
        {
            cancelled = true;
            break;
        }

        const auto n = (int) juce::jmin<juce::int64> (blockSize, range.getEnd() - pos);
        renderBlock (pos, n, block);

        const bool ok = wav != nullptr ? wav->writeFromAudioSampleBuffer (block, 0, n) : mp3->write (block, n);

        if (! ok)
            error = "write failed: " + target.getFullPathName();

        progressValue.store ((float) (pos + n - range.getStart()) / (float) range.getLength());
    }

    if (mp3 != nullptr && error.isEmpty() && ! cancelled && ! mp3->finish())
        error = "MP3 encoder failed to finish";

    wav.reset();
    mp3.reset();

    // Only a complete file gets the real name.
    if (error.isEmpty() && ! cancelled)
    {
        if (! partial.moveFileTo (target))
            error = "couldn't rename to " + target.getFullPathName();
    }
    else
    {
        partial.deleteFile();
    }

    done.store (true);
}
} // namespace taw
