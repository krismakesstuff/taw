#include "Import.h"

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
template <typename T>
struct Com
{
    T* p = nullptr;
    ~Com() { if (p != nullptr) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

// Decodes anything Windows can (m4a/aac, mp3, wma, ...) via Media Foundation. Used when JUCE's
// own readers can't open a file.
bool readWithMediaFoundation (const juce::File& file, juce::AudioBuffer<float>& out, double& rate)
{
    if (FAILED (CoInitializeEx (nullptr, COINIT_MULTITHREADED)))
        return false;

    struct Cleanup { ~Cleanup() { CoUninitialize(); } } com;

    if (FAILED (MFStartup (MF_VERSION)))
        return false;

    struct Shutdown { ~Shutdown() { MFShutdown(); } } mf;

    constexpr auto audioStream = (DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM;
    std::vector<float> interleaved;
    UINT32 channels = 0, sampleRate = 0;

    {
        Com<IMFSourceReader> reader;
        Com<IMFMediaType> wanted, actual;

        if (FAILED (MFCreateSourceReaderFromURL (file.getFullPathName().toWideCharPointer(), nullptr, &reader))
            || FAILED (reader->SetStreamSelection ((DWORD) MF_SOURCE_READER_ALL_STREAMS, FALSE))
            || FAILED (reader->SetStreamSelection (audioStream, TRUE))
            || FAILED (MFCreateMediaType (&wanted))
            || FAILED (wanted->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio))
            || FAILED (wanted->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_Float))
            || FAILED (reader->SetCurrentMediaType (audioStream, nullptr, wanted.p))
            || FAILED (reader->GetCurrentMediaType (audioStream, &actual))
            || FAILED (actual->GetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, &channels))
            || FAILED (actual->GetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate))
            || channels == 0)
            return false;

        for (;;)
        {
            DWORD flags = 0;
            Com<IMFSample> sample;

            if (FAILED (reader->ReadSample (audioStream, 0, nullptr, &flags, nullptr, &sample)))
                return false;

            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
                break;

            if (sample.p == nullptr)
                continue;

            Com<IMFMediaBuffer> buffer;
            BYTE* data = nullptr;
            DWORD bytes = 0;

            if (FAILED (sample->ConvertToContiguousBuffer (&buffer)) || FAILED (buffer->Lock (&data, nullptr, &bytes)))
                return false;

            const auto* f = reinterpret_cast<const float*> (data);
            interleaved.insert (interleaved.end(), f, f + bytes / sizeof (float));
            buffer->Unlock();
        }
    }

    const auto frames = (int) (interleaved.size() / channels);
    const auto keep = (int) juce::jmin<UINT32> (channels, 2);
    out.setSize (keep, frames);

    for (int ch = 0; ch < keep; ++ch)
        for (int i = 0; i < frames; ++i)
            out.setSample (ch, i, interleaved[(size_t) i * channels + (size_t) ch]);

    rate = sampleRate;
    return frames > 0;
}
} // namespace

static const juce::StringArray audioExtensions { ".wav", ".aif", ".aiff", ".flac", ".ogg", ".mp3", ".m4a", ".aac", ".wma" };

Import::Import (std::vector<juce::File> sources, juce::File folder, double rate)
    : juce::Thread ("taw import"), audioFolder (std::move (folder)), sessionRate (rate)
{
    formats.registerBasicFormats();

    for (auto& s : sources)
        items.push_back ({ std::move (s), {}, 0, 0, {} });
}

Import::~Import()
{
    stopThread (10000);
}

bool Import::isAudioFile (const juce::File& f)
{
    return f.existsAsFile() && audioExtensions.contains (f.getFileExtension(), true);
}

// A name as Windows users read it: no private-use characters, no trailing spaces or dots.
static juce::String comparableName (const juce::String& name)
{
    juce::String out;

    for (auto p = name.getCharPointer(); ! p.isEmpty();)
    {
        const auto c = p.getAndAdvance();

        if (c < 0xe000 || c > 0xf8ff)
            out += juce::String::charToString (c);
    }

    while (out.endsWithChar (' ') || out.endsWithChar ('.'))
        out = out.dropLastCharacters (1);

    return out.toLowerCase();
}

juce::File Import::resolve (const juce::String& pathIn, const juce::File& relativeTo)
{
    const auto path = pathIn.unquoted().trim();
    const auto direct = relativeTo.getChildFile (path);

    if (direct.exists())
        return direct;

    // Walk down from the root, matching each missing component by its comparable name.
    const auto parts = juce::StringArray::fromTokens (direct.getFullPathName(), "\\/", {});

    if (parts.isEmpty())
        return direct;

    juce::File current (parts[0] + juce::File::getSeparatorString());

    for (int i = 1; i < parts.size(); ++i)
    {
        if (parts[i].isEmpty())
            continue;

        if (auto next = current.getChildFile (parts[i]); next.exists())
        {
            current = next;
            continue;
        }

        const auto wanted = comparableName (parts[i]);
        bool found = false;

        for (const auto& child : current.findChildFiles (juce::File::findFilesAndDirectories, false))
            if (comparableName (child.getFileName()) == wanted)
            {
                current = child;
                found = true;
                break;
            }

        if (! found)
            return direct;
    }

    return current;
}

std::vector<juce::File> Import::expand (const juce::StringArray& paths, const juce::File& relativeTo, juce::StringArray& notFound)
{
    std::vector<juce::File> out;

    for (const auto& p : paths)
    {
        const auto f = resolve (p, relativeTo);

        if (f.isDirectory())
        {
            auto inside = f.findChildFiles (juce::File::findFiles, false);
            inside.sort();

            for (const auto& c : inside)
                if (isAudioFile (c))
                    out.push_back (c);
        }
        else if (f.existsAsFile())
        {
            out.push_back (f);
        }
        else
        {
            notFound.add (p);
        }
    }

    return out;
}

void Import::run()
{
    audioFolder.createDirectory();

    for (size_t i = 0; i < items.size(); ++i)
    {
        if (threadShouldExit())
        {
            cancelled = true;
            break;
        }

        importOne (items[i]);
        progressValue.store ((float) (i + 1) / (float) items.size());
    }

    done.store (true);
}

void Import::importOne (Item& item)
{
    // Tracks are mono or stereo: anything wider keeps its first two channels.
    juce::AudioBuffer<float> audio;
    double sourceRate = 0.0;

    if (std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor (item.source) })
    {
        if (reader->lengthInSamples <= 0 || reader->lengthInSamples > std::numeric_limits<int>::max())
        {
            item.error = item.source.getFileName() + " is empty or too long";
            return;
        }

        const auto ch = juce::jlimit (1, 2, (int) reader->numChannels);
        audio.setSize (ch, (int) reader->lengthInSamples);
        reader->read (&audio, 0, audio.getNumSamples(), 0, true, ch > 1);
        sourceRate = reader->sampleRate;
    }
    else if (! readWithMediaFoundation (item.source, audio, sourceRate))
    {
        item.error = "can't read " + item.source.getFileName();
        return;
    }

    const auto channels = audio.getNumChannels();

    if (sourceRate > 0 && std::abs (sourceRate - sessionRate) > 0.5)
    {
        const auto ratio = sourceRate / sessionRate; // input samples per output sample
        const auto outLength = (int) std::ceil (audio.getNumSamples() / ratio);

        // The sinc filter delays its output; render that much extra and drop it so imports
        // stay sample-aligned.
        const auto delay = (int) std::lround (juce::WindowedSincInterpolator::getBaseLatency() / ratio);
        juce::AudioBuffer<float> resampled (channels, outLength + delay);

        for (int ch = 0; ch < channels; ++ch)
        {
            juce::WindowedSincInterpolator interpolator;
            interpolator.process (ratio, audio.getReadPointer (ch), resampled.getWritePointer (ch), outLength + delay,
                                  audio.getNumSamples(), 0);
        }

        audio.setSize (channels, outLength, false, false, false);

        for (int ch = 0; ch < channels; ++ch)
            audio.copyFrom (ch, 0, resampled, ch, delay, outLength);
    }

    const auto name = juce::File::createLegalFileName (item.source.getFileNameWithoutExtension()).replaceCharacter (' ', '_');
    const auto target = audioFolder.getNonexistentChildFile ("imp-" + name, ".wav", false);

    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (target);
    auto writer = juce::WavAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                                       .withSampleRate (sessionRate)
                                                                       .withNumChannels (channels)
                                                                       .withBitsPerSample (32)
                                                                       .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));

    if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples()))
    {
        writer.reset();
        target.deleteFile();
        item.error = "can't write a copy of " + item.source.getFileName();
        return;
    }

    item.copy = target;
    item.channels = channels;
    item.length = audio.getNumSamples();
}
} // namespace taw
