#include "Video.h"

#if JUCE_WINDOWS
 #define WIN32_LEAN_AND_MEAN
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
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

        return juce::String();
    }();

    why = missing.isEmpty() ? juce::String() : "video needs FFmpeg's " + missing + " next to taw.exe";
    return missing.isEmpty();
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
    d->codec->thread_count = juce::jlimit (1, 8, juce::SystemStats::getNumCpus());
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

    videoInfo.fps = rate.num > 0 && rate.den > 0 ? av_q2d (rate) : 25.0;
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
#else
struct VideoDecoder::Impl {};
VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() = default;
bool VideoDecoder::open (const juce::File&, juce::String& error) { return videoAvailable (error); }
juce::int64 VideoDecoder::seek (double) { return -1; }
void VideoDecoder::render (juce::Image&, int, int) {}
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
        Window (View& view)
            : DocumentWindow ("taw video", juce::Colours::black, DocumentWindow::allButtons, false)
        {
            setUsingNativeTitleBar (true);
            setResizable (true, false);
            setResizeLimits (160, 90, 16000, 9000);
            setWantsKeyboardFocus (false);
            setMouseClickGrabsKeyboardFocus (false); // clicks don't take the keyboard from the terminal
            setContentNonOwned (&view, false);
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
        startThread();
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
