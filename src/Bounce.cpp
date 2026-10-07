#include "Bounce.h"
#include "Plugins.h"
#include "Video.h"

#if JUCE_WINDOWS
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
#else
 #include "Ffmpeg.h"
 #include <fcntl.h>
 #include <signal.h>
 #include <spawn.h>
 #include <sys/wait.h>
 #include <unistd.h>

extern char** environ;
#endif

namespace taw
{
namespace
{
// As small as a live block: plugin automation moves once a block, so a bounce steps like playback.
constexpr int blockSize = 512;

#if JUCE_WINDOWS
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
#else
// MP3 through an installed ffmpeg (LAME inside): the mix streams into its stdin as float PCM.
class Mp3Writer
{
public:
    Mp3Writer (const juce::File& file, double sampleRate, juce::String& error)
    {
        if (sampleRate != 44100.0 && sampleRate != 48000.0 && sampleRate != 32000.0)
        {
            error = "MP3 needs a 32, 44.1 or 48 kHz session";
            return;
        }

        const auto exe = ffmpeg::find();

        if (! exe.existsAsFile())
        {
            error = juce::String (L"MP3 needs ffmpeg \x00b7 ") + ffmpeg::installHint;
            return;
        }

        const juce::StringArray args { exe.getFullPathName(), "-hide_banner", "-loglevel", "error", "-y",
                                       "-f", "f32le", "-ar", juce::String ((int) sampleRate), "-ac", "2", "-i", "pipe:0",
                                       "-c:a", "libmp3lame", "-b:a", "320k", "-f", "mp3", file.getFullPathName() };
        std::vector<char*> argv;

        for (const auto& a : args)
            argv.push_back (const_cast<char*> (a.toRawUTF8()));

        argv.push_back (nullptr);

        int fds[2];

        if (pipe (fds) != 0)
        {
            error = "can't start ffmpeg";
            return;
        }

        // ffmpeg reads the pipe; its own output goes nowhere (errors show up as its exit code).
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init (&actions);
        posix_spawn_file_actions_adddup2 (&actions, fds[0], STDIN_FILENO);
        posix_spawn_file_actions_addclose (&actions, fds[1]);
        posix_spawn_file_actions_addopen (&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen (&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

        const auto spawned = posix_spawn (&child, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy (&actions);
        ::close (fds[0]);

        if (spawned != 0)
        {
            ::close (fds[1]);
            child = -1;
            error = "can't start ffmpeg";
            return;
        }

        input = fds[1];
    }

    ~Mp3Writer()
    {
        if (input >= 0)
            ::close (input);

        if (child > 0)
        {
            if (! finalised)
                kill (child, SIGTERM);

            int status = 0;
            waitpid (child, &status, 0);
        }
    }

    bool write (const juce::AudioBuffer<float>& block, int n)
    {
        interleaved.resize ((size_t) n * 2);

        for (int i = 0; i < n; ++i)
            for (int ch = 0; ch < 2; ++ch)
                interleaved[(size_t) (i * 2 + ch)] = juce::jlimit (-1.0f, 1.0f, block.getSample (ch, i));

        auto* data = reinterpret_cast<const char*> (interleaved.data());
        auto left = interleaved.size() * sizeof (float);

        while (left > 0)
        {
            const auto w = ::write (input, data, left);

            if (w <= 0)
                return false; // ffmpeg gave up

            data += w;
            left -= (size_t) w;
        }

        return true;
    }

    bool finish()
    {
        finalised = true;
        ::close (input); // end of stream: ffmpeg writes the rest and exits
        input = -1;

        int status = 0;
        const bool ok = waitpid (child, &status, 0) == child && WIFEXITED (status) && WEXITSTATUS (status) == 0;
        child = -1;
        return ok;
    }

private:
    pid_t child = -1;
    int input = -1;
    bool finalised = false;
    std::vector<float> interleaved;
};
#endif
} // namespace

Bounce::Bounce (std::unique_ptr<Graph> g, juce::Range<juce::int64> r, double rate, Format f, juce::File file, Picture pic)
    : juce::Thread ("taw bounce"), graph (std::move (g)), range (r), sampleRate (rate), format (f), target (std::move (file)), picture (std::move (pic))
{
    bus.setSize (2, blockSize);
    buses.setSize (juce::jmax (1, juce::jmin (graph->numBuses, Graph::maxBuses) * 2), blockSize);

    for (int ch = 0; ch < buses.getNumChannels(); ++ch)
        busPtrs[(size_t) ch] = buses.getWritePointer (ch);
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
    const auto numBuses = juce::jmin (graph->numBuses, Graph::maxBuses);

    for (int ch = 0; ch < numBuses * 2; ++ch)
        juce::FloatVectorOperations::clear (busPtrs[(size_t) ch], n);

    const auto numTracks = (int) graph->tracks.size();

    for (int k = 0; k < numTracks; ++k)
    {
        const auto t = graph->order.size() == graph->tracks.size() ? graph->order[(size_t) k] : k;
        const auto& tp = graph->tracks[(size_t) t];

        // Muted for the whole bounce: skipped. With mute automation it may come back in.
        if (! tp.audible && (tp.muteCurve.empty() || ! tp.soloAudible))
            continue;

        juce::FloatVectorOperations::clear (bl, n);
        juce::FloatVectorOperations::clear (br, n);
        renderClips (tp, pos, n, bl, br);
        renderHits (tp, pos, n, bl, br);
        toMono (tp, bl, br, n);

        if (tp.busIndex >= 0 && tp.busIndex < numBuses)
        {
            juce::FloatVectorOperations::copy (bl, busPtrs[(size_t) tp.busIndex * 2], n);
            juce::FloatVectorOperations::copy (br, busPtrs[(size_t) tp.busIndex * 2 + 1], n);
        }

        // Instruments and effects: the graph has its own plugin instances (copies of the live
        // ones), so the live ones keep playing while this renders.
        HostTransport transport;
        transport.bpm = graph->bpm;
        transport.sampleRate = sampleRate;
        transport.timeSigNum = graph->timeSigNum;
        transport.timeSigDen = graph->timeSigDen;
        transport.pos = pos;
        transport.rolling = true;

        automatePlugins (graph->tracks[(size_t) t], pos);

        if (tp.plugin != nullptr)
            tp.plugin->render (tp.sequence, transport, n, nullptr, 0, bl, br);

        for (const auto& fx : tp.inserts)
            fx->process (transport, n, bl, br);

        mixTrack (graph->tracks[(size_t) t], pos, bl, br, n, busPtrs.data(), numBuses, l, r);
    }

    applyMixGain (*graph, pos - graph->latency, l, r, n); // what's heard is that late

    for (int ch = 0; ch < 2; ++ch)
        peakValue = juce::jmax (peakValue, out.getMagnitude (ch, 0, n));

    return true;
}

void Bounce::run()
{
    target.getParentDirectory().createDirectory();
    // Write under a temporary name; keep the extension, the MP3 encoder picks the container from it.
    const auto partial = target.getSiblingFile (target.getFileNameWithoutExtension() + ".part" + target.getFileExtension());
    juce::AudioBuffer<float> block (2, blockSize);

    std::unique_ptr<juce::AudioFormatWriter> wav;
    std::unique_ptr<Mp3Writer> mp3;
    std::unique_ptr<VideoMuxer> mp4;

    if (format == Format::video)
    {
        mp4 = std::make_unique<VideoMuxer> (partial, picture.file, picture.from, range.getLength(), sampleRate, error);
    }
    else if (format == Format::wav)
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

    // Delay compensation: the mix comes out graph->latency late. Run that much first (it primes
    // the plugins and delay lines) and keep what follows.
    for (auto pos = range.getStart(); pos < range.getStart() + graph->latency && ! threadShouldExit(); pos += blockSize)
        renderBlock (pos, (int) juce::jmin<juce::int64> (blockSize, range.getStart() + graph->latency - pos), block);

    for (auto pos = range.getStart() + graph->latency; error.isEmpty() && pos < range.getEnd() + graph->latency; pos += blockSize)
    {
        if (threadShouldExit())
        {
            cancelled = true;
            break;
        }

        const auto n = (int) juce::jmin<juce::int64> (blockSize, range.getEnd() + graph->latency - pos);
        renderBlock (pos, n, block);

        const bool ok = wav != nullptr ? wav->writeFromAudioSampleBuffer (block, 0, n)
                        : mp4 != nullptr ? mp4->write (block, n)
                                         : mp3->write (block, n);

        if (! ok)
            error = "write failed: " + target.getFullPathName();

        progressValue.store ((float) (pos + n - range.getStart() - graph->latency) / (float) range.getLength());
    }

    if (mp3 != nullptr && error.isEmpty() && ! cancelled && ! mp3->finish())
        error = "MP3 encoder failed to finish";

    if (mp4 != nullptr && error.isEmpty() && ! cancelled && ! mp4->finish())
        error = "the video didn't finish writing";

    wav.reset();
    mp3.reset();
    mp4.reset();

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
