#include "Video.h"

#if JUCE_WINDOWS
 #define WIN32_LEAN_AND_MEAN
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#elif JUCE_MAC
 #include <dlfcn.h>
#endif

#if TAW_VIDEO
JUCE_BEGIN_IGNORE_WARNINGS_MSVC (4244 4996)
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}
JUCE_END_IGNORE_WARNINGS_MSVC
#endif

#include <cmath>
#include <cstring>

namespace taw
{
bool videoAvailable (juce::String& why)
{
#if TAW_VIDEO && JUCE_WINDOWS
    // The DLLs are delay-loaded: a call into one that isn't there would crash, so look first.
    static const juce::String missing = []
    {
        for (const auto* dll : { L"avutil-61.dll", L"swresample-7.dll", L"avcodec-63.dll", L"avformat-63.dll", L"swscale-10.dll" })
            if (LoadLibraryW (dll) == nullptr)
                return juce::String (dll);

        av_log_set_level (AV_LOG_QUIET); // FFmpeg's messages would land on taw's screen
        return juce::String();
    }();

    why = missing.isEmpty() ? juce::String() : "video needs FFmpeg's " + missing + " next to taw.exe";
    return missing.isEmpty();
#elif TAW_VIDEO && JUCE_MAC
    // Weak-linked: a library that didn't load leaves its functions unbound (asked of the loader:
    // the compiler takes a function's address for granted). Homebrew's next major FFmpeg renames
    // them (libavcodec.62 to .63), which needs a rebuild.
    static const bool loaded = []
    {
        bool all = true;

        for (const auto* symbol : { "avformat_open_input", "avcodec_send_packet", "av_frame_alloc", "sws_scale" })
            all = all && dlsym (RTLD_DEFAULT, symbol) != nullptr;

        if (all)
            av_log_set_level (AV_LOG_QUIET); // FFmpeg's messages would land on taw's screen

        return all;
    }();

    // Not loaded: none installed, or Homebrew's is another major version than taw was built for
    // (its libavformat.N.dylib says which).
    static const juce::String missing = []
    {
        int found = 0;

        for (const auto* dir : { "/opt/homebrew/opt/ffmpeg/lib", "/usr/local/opt/ffmpeg/lib" })
            for (const auto& f : juce::File (dir).findChildFiles (juce::File::findFiles, false, "libavformat.*.dylib"))
                if (const auto name = f.getFileName(); name.matchesWildcard ("libavformat.*.dylib", false)
                                                       && name.fromFirstOccurrenceOf (".", false, false).upToFirstOccurrenceOf (".", false, false).containsOnly ("0123456789"))
                    found = std::max (found, name.fromFirstOccurrenceOf (".", false, false).getIntValue());

        if (found == 0)
            return juce::String ("video needs FFmpeg: brew install ffmpeg");

        if (found < LIBAVFORMAT_VERSION_MAJOR)
            return juce::String ("video needs a newer FFmpeg than the one installed: brew upgrade ffmpeg");

        if (found == LIBAVFORMAT_VERSION_MAJOR)
            return juce::String ("video couldn't load Homebrew's FFmpeg: brew reinstall ffmpeg");

        return juce::String ("video needs the FFmpeg taw was built with (libavformat ") + juce::String (LIBAVFORMAT_VERSION_MAJOR)
               + ", installed: " + juce::String (found) + "): rebuild taw";
    }();

    why = loaded ? juce::String() : missing;
    return loaded;
#else
    why = "video isn't built for this platform yet";
    return false;
#endif
}

//==============================================================================
#if TAW_VIDEO
struct VideoDecoder::Impl
{
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVStream* stream = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* current = nullptr; // the frame showing
    AVFrame* next = nullptr;    // the one after it, decoded already
    SwsContext* scaler = nullptr;
    bool haveCurrent = false, haveNext = false;
    bool inputDone = false, endOfStream = false;
    double timeBase = 0.0;  // seconds per timestamp tick
    int64_t firstPts = 0;   // the video's time 0
    int64_t frameTicks = 1; // one frame, in ticks (for the last frame's end)

    ~Impl()
    {
        sws_freeContext (scaler);
        av_frame_free (&current);
        av_frame_free (&next);
        av_packet_free (&packet);
        avcodec_free_context (&codec);
        avformat_close_input (&format);
    }

    static int64_t ptsOf (const AVFrame* f)
    {
        return f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    }

    // The next frame out of the decoder; false at the end of the stream (or on an error).
    bool decode (AVFrame* into, int& decoded)
    {
        for (;;)
        {
            const int r = avcodec_receive_frame (codec, into);

            if (r == 0)
            {
                ++decoded;
                return true;
            }

            if (r == AVERROR_EOF)
                endOfStream = true;

            if (r != AVERROR (EAGAIN) || inputDone)
                return false;

            // It wants more: the stream's next packet (a damaged one is skipped by the decoder).
            for (;;)
            {
                if (av_read_frame (format, packet) < 0)
                {
                    avcodec_send_packet (codec, nullptr); // drain what it's holding
                    inputDone = true;
                    break;
                }

                const bool ours = packet->stream_index == stream->index;

                if (ours)
                    avcodec_send_packet (codec, packet);

                av_packet_unref (packet);

                if (ours)
                    break;
            }
        }
    }

    // Decodes on until current is the last frame at or before target and next the one after.
    void advanceTo (int64_t target, int& decoded)
    {
        for (;;)
        {
            if (! haveNext)
            {
                if (endOfStream || ! decode (next, decoded))
                    return;

                haveNext = true;
            }

            if (ptsOf (next) > target)
                return;

            std::swap (current, next);
            haveCurrent = true;
            haveNext = false;
        }
    }

    void seekTo (int64_t target, int& decoded)
    {
        av_seek_frame (format, stream->index, target, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers (codec);
        haveCurrent = haveNext = inputDone = endOfStream = false;
        advanceTo (target, decoded);

        // Landed after it (a target before the first keyframe's frame): that frame it is.
        if (! haveCurrent && haveNext)
        {
            std::swap (current, next);
            haveCurrent = true;
            haveNext = false;
        }
    }

    // Is decoding on from the current frame to target better than seeking? It is when there's no
    // keyframe in between (seeking would land at or before where it is anyway), or it's close.
    bool decodeOnTo (int64_t target) const
    {
        if (target - ptsOf (current) <= frameTicks * 8)
            return true;

        const int k = av_index_search_timestamp (stream, target, AVSEEK_FLAG_BACKWARD);
        const auto* key = k >= 0 ? avformat_index_get_entry (stream, k) : nullptr;
        return key != nullptr && key->timestamp <= ptsOf (current);
    }
};

VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() = default;

bool VideoDecoder::open (const juce::File& file, juce::String& error)
{
    impl.reset();
    videoInfo = {};

    if (! videoAvailable (error))
        return false;

    if (! file.existsAsFile())
    {
        error = "no file " + file.getFullPathName();
        return false;
    }

    auto d = std::make_unique<Impl>();
    auto fail = [&] (const juce::String& why)
    {
        error = why + ": " + file.getFileName();
        return false;
    };

    if (avformat_open_input (&d->format, file.getFullPathName().toRawUTF8(), nullptr, nullptr) < 0)
        return fail ("can't open");

    if (avformat_find_stream_info (d->format, nullptr) < 0)
        return fail ("can't read");

    const AVCodec* decoder = nullptr;
    const int index = av_find_best_stream (d->format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);

    if (index < 0 || decoder == nullptr)
        return fail ("no picture in");

    d->stream = d->format->streams[index];
    d->codec = avcodec_alloc_context3 (decoder);

    if (d->codec == nullptr || avcodec_parameters_to_context (d->codec, d->stream->codecpar) < 0)
        return fail ("can't decode");

    // Frame threads: more frames a second, and a few frames' delay that only a seek notices.
    // Two at most: more crowd the cores the audio needs (a picture to score to is small work).
    d->codec->thread_count = 2;
    d->codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

    if (avcodec_open2 (d->codec, decoder, nullptr) < 0)
        return fail ("can't decode");

    d->packet = av_packet_alloc();
    d->current = av_frame_alloc();
    d->next = av_frame_alloc();
    d->timeBase = av_q2d (d->stream->time_base);
    d->firstPts = d->stream->start_time != AV_NOPTS_VALUE ? d->stream->start_time : 0;

    auto rate = d->stream->r_frame_rate;

    if (rate.num <= 0 || rate.den <= 0)
        rate = d->stream->avg_frame_rate;

    if (rate.num <= 0 || rate.den <= 0)
        rate = { 25, 1 };

    videoInfo.fps = av_q2d (rate);
    videoInfo.fpsNum = rate.num;
    videoInfo.fpsDen = rate.den;
    d->frameTicks = juce::jmax<int64_t> (1, (int64_t) std::llround (1.0 / (videoInfo.fps * d->timeBase)));
    videoInfo.width = d->codec->width;
    videoInfo.height = d->codec->height;
    videoInfo.codec = decoder->name;
    videoInfo.duration = d->stream->duration != AV_NOPTS_VALUE ? (double) d->stream->duration * d->timeBase
                         : d->format->duration != AV_NOPTS_VALUE ? (double) d->format->duration / AV_TIME_BASE
                                                                 : 0.0;
    impl = std::move (d);
    return true;
}

juce::int64 VideoDecoder::seek (double seconds)
{
    if (impl == nullptr || ! (seconds >= 0.0)) // also NaN
        return -1;

    auto& d = *impl;
    const auto target = d.firstPts + (int64_t) std::floor (seconds / d.timeBase + 1.0e-4);

    if (d.haveCurrent && target >= Impl::ptsOf (d.current))
    {
        if (d.haveNext && target < Impl::ptsOf (d.next))
            ; // still this frame
        else if (! d.haveNext && d.endOfStream)
            ; // the last frame, or past it (below)
        else if (d.decodeOnTo (target))
            d.advanceTo (target, decodedFrames);
        else
        {
            ++seeks;
            d.seekTo (target, decodedFrames);
        }
    }
    else
    {
        ++seeks;
        d.seekTo (target, decodedFrames);
    }

    if (! d.haveCurrent)
        return -1;

    const auto pts = Impl::ptsOf (d.current);

    // Past the last frame's end: black.
    if (! d.haveNext && d.endOfStream && target >= pts + (d.current->duration > 0 ? d.current->duration : d.frameTicks))
        return -1;

    return (juce::int64) std::llround ((double) (pts - d.firstPts) * d.timeBase * videoInfo.fps);
}

void VideoDecoder::render (juce::Image& into, int w, int h)
{
    if (impl == nullptr || ! impl->haveCurrent || w <= 0 || h <= 0)
        return;

    auto& d = *impl;
    const auto* f = d.current;

    // Software: it's written on the decoder thread.
    if (into.getWidth() != w || into.getHeight() != h || into.getFormat() != juce::Image::ARGB)
        into = juce::Image (juce::Image::ARGB, w, h, false, juce::SoftwareImageType());

    d.scaler = sws_getCachedContext (d.scaler, f->width, f->height, (AVPixelFormat) f->format, w, h, AV_PIX_FMT_BGRA,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (d.scaler == nullptr)
        return;

    // HD is BT.709 even when the file doesn't say (swscale would assume SD's BT.601).
    const int space = f->colorspace != AVCOL_SPC_UNSPECIFIED ? (int) f->colorspace : (f->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601);
    sws_setColorspaceDetails (d.scaler, sws_getCoefficients (space), f->color_range == AVCOL_RANGE_JPEG ? 1 : 0,
                              sws_getCoefficients (SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);

    // JUCE's ARGB is B, G, R, A in memory.
    juce::Image::BitmapData out (into, juce::Image::BitmapData::writeOnly);
    uint8_t* dst[4] = { out.data, nullptr, nullptr, nullptr };
    int stride[4] = { out.lineStride, 0, 0, 0 };
    sws_scale (d.scaler, f->data, f->linesize, 0, f->height, dst, stride);
}

const AVFrame* VideoDecoder::frame() const
{
    return impl != nullptr && impl->haveCurrent ? impl->current : nullptr;
}

//==============================================================================
struct VideoMuxer::Impl
{
    VideoDecoder decoder;
    AVFormatContext* out = nullptr;
    AVCodecContext* video = nullptr;
    AVCodecContext* audio = nullptr;
    AVStream* videoStream = nullptr;
    AVStream* audioStream = nullptr;
    AVFrame* picture = nullptr; // the encoder's frame
    AVFrame* sound = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* scaler = nullptr;
    bool headerWritten = false;

    double from = 0.0, rate = 44100.0;
    AVRational fps { 25, 1 };
    int64_t totalFrames = 0, nextFrame = 0;
    int64_t samplesIn = 0, audioPts = 0;
    std::vector<float> left, right; // mix waiting for a whole AAC frame

    bool open (const juce::File&, double from, juce::int64 samples, double sampleRate, juce::String& error);

    ~Impl()
    {
        sws_freeContext (scaler);
        av_frame_free (&picture);
        av_frame_free (&sound);
        av_packet_free (&packet);
        avcodec_free_context (&video);
        avcodec_free_context (&audio);

        if (out != nullptr)
        {
            if (out->pb != nullptr)
                avio_closep (&out->pb);

            avformat_free_context (out);
        }
    }

    // A frame (or nullptr: the end) into an encoder, and whatever comes out into the file.
    bool encode (AVCodecContext* c, AVStream* s, AVFrame* f)
    {
        if (avcodec_send_frame (c, f) < 0)
            return false;

        for (;;)
        {
            const int r = avcodec_receive_packet (c, packet);

            if (r == AVERROR (EAGAIN) || r == AVERROR_EOF)
                return true;

            if (r < 0)
                return false;

            av_packet_rescale_ts (packet, c->time_base, s->time_base);
            packet->stream_index = s->index;

            if (av_interleaved_write_frame (out, packet) < 0)
                return false;
        }
    }

    // Output frame n: the frame showing at that moment (as the window would show it), or black.
    bool encodePicture (int64_t n)
    {
        if (av_frame_make_writable (picture) < 0)
            return false;

        const auto shown = decoder.seek (from + (double) n * fps.den / fps.num);
        const auto* f = shown >= 0 ? decoder.frame() : nullptr;

        if (f != nullptr)
        {
            scaler = sws_getCachedContext (scaler, f->width, f->height, (AVPixelFormat) f->format, picture->width, picture->height,
                                           AV_PIX_FMT_YUV420P, SWS_BICUBIC, nullptr, nullptr, nullptr);

            if (scaler == nullptr)
                return false;

            // Keep its colours: the same matrix and range in as out (BT.709 for HD when unsaid).
            const int space = f->colorspace != AVCOL_SPC_UNSPECIFIED ? (int) f->colorspace : (f->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601);
            const auto* coefficients = sws_getCoefficients (space);
            sws_setColorspaceDetails (scaler, coefficients, f->color_range == AVCOL_RANGE_JPEG ? 1 : 0, coefficients, 0, 0, 1 << 16, 1 << 16);
            sws_scale (scaler, f->data, f->linesize, 0, f->height, picture->data, picture->linesize);
        }
        else
        {
            // Black, in video range.
            for (int y = 0; y < picture->height; ++y)
                std::memset (picture->data[0] + y * picture->linesize[0], 16, (size_t) picture->width);

            for (int plane = 1; plane < 3; ++plane)
                for (int y = 0; y < picture->height / 2; ++y)
                    std::memset (picture->data[plane] + y * picture->linesize[plane], 128, (size_t) picture->width / 2);
        }

        picture->pts = n;
        return encode (video, videoStream, picture);
    }

    // The waiting mix out in AAC frames; at the end (last) the short one that's left too.
    bool encodeSound (bool last)
    {
        const auto frameSize = (size_t) audio->frame_size;

        while (left.size() >= frameSize || (last && ! left.empty()))
        {
            const auto n = juce::jmin (frameSize, left.size());

            if (av_frame_make_writable (sound) < 0)
                return false;

            sound->nb_samples = (int) n;
            std::memcpy (sound->data[0], left.data(), n * sizeof (float));
            std::memcpy (sound->data[1], right.data(), n * sizeof (float));
            sound->pts = audioPts;
            audioPts += (int64_t) n;
            left.erase (left.begin(), left.begin() + (ptrdiff_t) n);
            right.erase (right.begin(), right.begin() + (ptrdiff_t) n);

            if (! encode (audio, audioStream, sound))
                return false;
        }

        return true;
    }

    // The picture up to where the mix has got (so the file interleaves as it's written).
    bool encodePicturesTo (int64_t frames)
    {
        for (frames = juce::jmin (frames, totalFrames); nextFrame < frames; ++nextFrame)
            if (! encodePicture (nextFrame))
                return false;

        return true;
    }
};

VideoMuxer::VideoMuxer (const juce::File& file, const juce::File& source, double from, juce::int64 samples, double sampleRate, juce::String& error)
{
    auto d = std::make_unique<Impl>();

    if (d->decoder.open (source, error) && d->open (file, from, samples, sampleRate, error))
        impl = std::move (d);
}

bool VideoMuxer::Impl::open (const juce::File& file, double start, juce::int64 samples, double sampleRate, juce::String& error)
{
    auto* d = this;
    const auto& info = d->decoder.info();
    d->from = start;
    d->rate = sampleRate;
    d->fps = { info.fpsNum, info.fpsDen };
    d->totalFrames = (int64_t) std::ceil ((double) samples / sampleRate * info.fps - 1.0e-6);
    const auto path = file.getFullPathName().toRawUTF8();
    auto fail = [&] (const juce::String& why)
    {
        error = why;
        return false;
    };

    if (avformat_alloc_output_context2 (&d->out, nullptr, "mp4", path) < 0 || d->out == nullptr)
        return fail ("can't make an MP4");

    const bool globalHeader = (d->out->oformat->flags & AVFMT_GLOBALHEADER) != 0;

    // Picture: H.264 (x264), near-transparent quality, the source's size (even, as 4:2:0 needs).
    const AVCodec* h264 = avcodec_find_encoder_by_name ("libx264");

    if (h264 == nullptr)
        return fail ("no H.264 encoder in this FFmpeg");

    d->video = avcodec_alloc_context3 (h264);
    d->video->width = info.width & ~1;
    d->video->height = info.height & ~1;
    d->video->pix_fmt = AV_PIX_FMT_YUV420P;
    d->video->time_base = av_inv_q (d->fps);
    d->video->framerate = d->fps;
    d->video->gop_size = juce::jmax (1, juce::roundToInt (info.fps * 2.0));
    d->video->thread_count = 0; // as many as it likes

    if (info.height >= 720)
    {
        d->video->colorspace = AVCOL_SPC_BT709;
        d->video->color_primaries = AVCOL_PRI_BT709;
        d->video->color_trc = AVCOL_TRC_BT709;
    }

    d->video->color_range = AVCOL_RANGE_MPEG;

    if (globalHeader)
        d->video->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    AVDictionary* x264 = nullptr;
    av_dict_set (&x264, "crf", "18", 0);
    av_dict_set (&x264, "preset", "faster", 0);
    const bool videoOpen = avcodec_open2 (d->video, h264, &x264) >= 0;
    av_dict_free (&x264);

    if (! videoOpen)
        return fail ("the H.264 encoder wouldn't start");

    // Sound: AAC, stereo, at the session's rate.
    const AVCodec* aac = avcodec_find_encoder (AV_CODEC_ID_AAC);

    if (aac == nullptr)
        return fail ("no AAC encoder in this FFmpeg");

    d->audio = avcodec_alloc_context3 (aac);
    d->audio->sample_fmt = AV_SAMPLE_FMT_FLTP;
    d->audio->sample_rate = (int) sampleRate;
    av_channel_layout_default (&d->audio->ch_layout, 2);
    d->audio->bit_rate = 320000;
    d->audio->time_base = { 1, (int) sampleRate };

    if (globalHeader)
        d->audio->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2 (d->audio, aac, nullptr) < 0)
        return fail ("the AAC encoder wouldn't start at " + juce::String (sampleRate / 1000.0, 1) + " kHz");

    d->videoStream = avformat_new_stream (d->out, nullptr);
    d->audioStream = avformat_new_stream (d->out, nullptr);

    if (d->videoStream == nullptr || d->audioStream == nullptr
        || avcodec_parameters_from_context (d->videoStream->codecpar, d->video) < 0
        || avcodec_parameters_from_context (d->audioStream->codecpar, d->audio) < 0)
        return fail ("can't make an MP4");

    d->videoStream->time_base = d->video->time_base;
    d->videoStream->avg_frame_rate = d->fps;
    d->audioStream->time_base = d->audio->time_base;

    if (avio_open (&d->out->pb, path, AVIO_FLAG_WRITE) < 0)
        return fail ("can't write " + file.getFullPathName());

    // Its index at the front, so it starts playing before it has all downloaded.
    AVDictionary* mp4 = nullptr;
    av_dict_set (&mp4, "movflags", "+faststart", 0);
    const bool header = avformat_write_header (d->out, &mp4) >= 0;
    av_dict_free (&mp4);

    if (! header)
        return fail ("can't write " + file.getFullPathName());

    d->headerWritten = true;
    d->packet = av_packet_alloc();
    d->picture = av_frame_alloc();
    d->sound = av_frame_alloc();
    d->picture->format = AV_PIX_FMT_YUV420P;
    d->picture->width = d->video->width;
    d->picture->height = d->video->height;
    d->sound->format = AV_SAMPLE_FMT_FLTP;
    d->sound->nb_samples = d->audio->frame_size;
    d->sound->sample_rate = d->audio->sample_rate;
    av_channel_layout_copy (&d->sound->ch_layout, &d->audio->ch_layout);

    if (av_frame_get_buffer (d->picture, 0) < 0 || av_frame_get_buffer (d->sound, 0) < 0)
        return fail ("out of memory");

    return true;
}

VideoMuxer::~VideoMuxer() = default;

bool VideoMuxer::write (const juce::AudioBuffer<float>& block, int n)
{
    if (impl == nullptr)
        return false;

    auto& d = *impl;

    for (int i = 0; i < n; ++i)
    {
        d.left.push_back (juce::jlimit (-1.0f, 1.0f, block.getSample (0, i)));
        d.right.push_back (juce::jlimit (-1.0f, 1.0f, block.getSample (1, i)));
    }

    d.samplesIn += n;
    return d.encodeSound (false) && d.encodePicturesTo ((int64_t) std::floor ((double) d.samplesIn / d.rate * av_q2d (d.fps)));
}

bool VideoMuxer::finish()
{
    if (impl == nullptr)
        return false;

    auto& d = *impl;
    return d.encodeSound (true) && d.encodePicturesTo (d.totalFrames) && d.encode (d.audio, d.audioStream, nullptr)
           && d.encode (d.video, d.videoStream, nullptr) && av_write_trailer (d.out) >= 0;
}
#else
struct VideoDecoder::Impl {};
VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() = default;
bool VideoDecoder::open (const juce::File&, juce::String& error) { return videoAvailable (error); }
juce::int64 VideoDecoder::seek (double) { return -1; }
void VideoDecoder::render (juce::Image&, int, int) {}
const AVFrame* VideoDecoder::frame() const { return nullptr; }

struct VideoMuxer::Impl {};
VideoMuxer::VideoMuxer (const juce::File&, const juce::File&, double, juce::int64, double, juce::String& error) { videoAvailable (error); }
VideoMuxer::~VideoMuxer() = default;
bool VideoMuxer::write (const juce::AudioBuffer<float>&, int) { return false; }
bool VideoMuxer::finish() { return false; }
#endif

//==============================================================================
struct VideoPlayer::Impl final : private juce::Thread
{
    // The picture, letterboxed in black. Asks the decoder thread for frames at its own size in
    // physical pixels, so drawing is a plain copy.
    struct View final : juce::Component, private juce::AsyncUpdater
    {
        explicit View (Impl& o) : owner (o)
        {
            setOpaque (true);
            setInterceptsMouseClicks (false, false);
            setWantsKeyboardFocus (false);
        }

        ~View() override { cancelPendingUpdate(); }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colours::black);
            const juce::ScopedLock sl (owner.lock);

            if (owner.shown.isValid())
            {
                g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
                g.drawImage (owner.shown, picture.toFloat(), juce::RectanglePlacement::stretchToFit);
            }
        }

        void resized() override
        {
            const auto& info = owner.decoder.info();
            const double aspect = info.width > 0 && info.height > 0 ? (double) info.width / info.height : 16.0 / 9.0;
            const auto area = getLocalBounds().toDouble();
            auto w = area.getWidth(), h = w / aspect;

            if (h > area.getHeight())
            {
                h = area.getHeight();
                w = h * aspect;
            }

            picture = juce::Rectangle<double> (w, h).withCentre (area.getCentre()).toNearestInt();
            const auto scale = (double) juce::Component::getApproximateScaleFactorForComponent (this);
            owner.wantW = juce::roundToInt (picture.getWidth() * scale);
            owner.wantH = juce::roundToInt (picture.getHeight() * scale);
        }

        void newFrame() { triggerAsyncUpdate(); }
        void handleAsyncUpdate() override { repaint(); }

        Impl& owner;
        juce::Rectangle<int> picture;
    };

    struct Window final : juce::DocumentWindow
    {
        // Set up before it's on the desktop: JUCE brings a window to the front, keyboard and all,
        // when changing these remakes the native window.
        Window (View& content)
            : DocumentWindow ("taw video", juce::Colours::black, DocumentWindow::allButtons, false)
        {
            setUsingNativeTitleBar (true);
            setResizable (true, false);
            setResizeLimits (160, 90, 16000, 9000);
            setWantsKeyboardFocus (false);
            setMouseClickGrabsKeyboardFocus (false); // clicks don't take the keyboard from the terminal
            setContentNonOwned (&content, false);
            centreWithSize (960, 540);
            addToDesktop();
            noActivate();
        }

        // Ignores keys: it never takes the keyboard, not even when it's shown.
        int getDesktopWindowStyleFlags() const override
        {
            return DocumentWindow::getDesktopWindowStyleFlags() | juce::ComponentPeer::windowIgnoresKeyPresses;
        }

        void closeButtonPressed() override
        {
            closeRequested = true;
            setVisible (false);
        }

        std::function<void()> onHide;
        void visibilityChanged() override
        {
            DocumentWindow::visibilityChanged();

            if (! isVisible() && onHide != nullptr)
                onHide();
        }

        // Windows: never activated, so clicking or dragging it leaves the keyboard in the terminal.
        void noActivate()
        {
#if JUCE_WINDOWS
            if (auto* hwnd = (HWND) getWindowHandle())
                SetWindowLongPtrW (hwnd, GWL_EXSTYLE, GetWindowLongPtrW (hwnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE | WS_EX_APPWINDOW);
#endif
        }

        std::atomic<bool> closeRequested { false };
    };

    explicit Impl (std::function<double()> c)
        : Thread ("taw video"), clock (std::move (c))
    {
        window.onHide = [this] { showing = false; };
    }

    ~Impl() override { stopThread (2000); }

    void run() override
    {
        juce::Image back;
        juce::int64 lastFrame = -2;
        int lastW = 0, lastH = 0;

        while (! threadShouldExit())
        {
            if (! showing)
            {
                wait (20); // nothing to keep up to date
                continue;
            }

            const auto frame = decoder.seek (clock());
            const int w = wantW, h = wantH;

            if (frame != lastFrame || w != lastW || h != lastH)
            {
                if (frame >= 0)
                    decoder.render (back, w, h);

                {
                    const juce::ScopedLock sl (lock);
                    std::swap (shown, back);

                    if (frame < 0)
                        shown = {};

                    shownFrame = frame;
                }

                view.newFrame();
                lastFrame = frame;
                lastW = w;
                lastH = h;
            }

            wait (3);
        }
    }

    bool open (const juce::File& f, juce::String& error)
    {
        stopThread (2000);

        if (! decoder.open (f, error))
            return false;

        file = f;
        window.setName (f.getFileName() + juce::String (L" \x00b7 taw"));
        view.resized(); // the new picture's shape
        startThread (juce::Thread::Priority::low); // the audio comes first
        return true;
    }

    std::function<double()> clock;
    VideoDecoder decoder;
    juce::File file;
    juce::CriticalSection lock;
    juce::Image shown;          // under lock
    juce::int64 shownFrame = -1; // under lock
    std::atomic<int> wantW { 0 }, wantH { 0 };
    std::atomic<bool> showing { false };
    View view { *this };
    Window window { view };
};

VideoPlayer::VideoPlayer (std::function<double()> clock) : impl (std::make_unique<Impl> (std::move (clock))) {}
VideoPlayer::~VideoPlayer() = default;

bool VideoPlayer::open (const juce::File& f, juce::String& error) { return impl->open (f, error); }
const VideoInfo& VideoPlayer::info() const { return impl->decoder.info(); }
juce::File VideoPlayer::file() const { return impl->file; }

void VideoPlayer::setVisible (bool shouldShow)
{
    if (shouldShow)
        impl->window.noActivate();

    impl->window.setVisible (shouldShow);
    impl->showing = shouldShow;
}

bool VideoPlayer::isVisible() const { return impl->window.isVisible(); }
bool VideoPlayer::hasKeyboard() const { return impl->window.isVisible() && impl->window.isActiveWindow(); }
bool VideoPlayer::takeCloseRequest() { return impl->window.closeRequested.exchange (false); }

juce::String VideoPlayer::windowState() const { return impl->window.getWindowStateAsString(); }

void VideoPlayer::restoreWindowState (const juce::String& s)
{
    if (s.isNotEmpty())
        impl->window.restoreWindowStateFromString (s);
}

juce::int64 VideoPlayer::shownFrame() const
{
    const juce::ScopedLock sl (impl->lock);
    return impl->shownFrame;
}

juce::Image VideoPlayer::snapshot() const
{
    const juce::ScopedLock sl (impl->lock);
    return impl->shown.isValid() ? impl->shown.createCopy() : juce::Image();
}
} // namespace taw
