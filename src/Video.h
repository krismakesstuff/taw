#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

struct AVFrame;

namespace taw
{
// Picture for scoring: a video file decoded with FFmpeg, in a window of its own that follows
// the playhead. The audio is the clock; the picture only ever looks at where the playhead is.

struct VideoInfo
{
    int width = 0, height = 0;
    double fps = 0.0;      // the stream's frame rate (r_frame_rate), what "a frame" means
    int fpsNum = 25, fpsDen = 1; // ...exactly
    double duration = 0.0; // seconds
    juce::String codec;
};

// The FFmpeg DLLs are there and load (Windows loads them on first use). why: what's missing.
bool videoAvailable (juce::String& why);

// Decodes the frame showing at a time, exactly: the last frame whose timestamp is at or before
// it. Moving forward a little decodes on from where it is; anything else seeks to the keyframe
// before and decodes up to the frame. One thread at a time.
class VideoDecoder
{
public:
    VideoDecoder();
    ~VideoDecoder();

    bool open (const juce::File&, juce::String& error);
    const VideoInfo& info() const { return videoInfo; }

    // Positions on the frame showing at `seconds` (0 = the start of the first frame) and returns
    // its number (from its timestamp), or -1 before the start, past the end, or on an error.
    juce::int64 seek (double seconds);

    // That frame, scaled to w x h (an ARGB image, reused when it's already that size).
    void render (juce::Image& into, int w, int h);
    const AVFrame* frame() const; // ...as decoded (nullptr: none)

    int seeks = 0, decodedFrames = 0; // what it took, for tests

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    VideoInfo videoInfo;
};

// Bouncing with picture: an MP4 of the video, re-encoded as H.264 so every frame is exactly
// where it was (each one is the frame the window would show then; black before or after the
// video), with the mix as AAC. Fed the mix a block at a time, it encodes the frames up to there.
class VideoMuxer
{
public:
    // from: where in the video the bounce starts, seconds (negative: black until it starts).
    // samples: how long the bounce is, at sampleRate.
    VideoMuxer (const juce::File& out, const juce::File& video, double from, juce::int64 samples, double sampleRate, juce::String& error);
    ~VideoMuxer();

    bool write (const juce::AudioBuffer<float>&, int numSamples);
    bool finish(); // the rest of the picture, then the file's index

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// The video window and the thread that keeps it on the right frame. The window is only a view:
// a normal window (move it, size it, put it on another screen) that never takes the keyboard
// and takes no clicks. Its close button hides it.
class VideoPlayer
{
public:
    // clock: where the picture should be, in seconds into the video; asked on the decoder
    // thread, so it reads only what's safe there. Negative (or NaN): before the video, black.
    explicit VideoPlayer (std::function<double()> clock);
    ~VideoPlayer();

    bool open (const juce::File&, juce::String& error);
    const VideoInfo& info() const;
    juce::File file() const;

    void setVisible (bool);
    bool isVisible() const;
    bool takeCloseRequest(); // its close button was pressed (it's hidden) since last asked
    bool hasKeyboard() const; // it was activated anyway (Windows can): give the keyboard back

    // Where the window is, to put it back there next time (JUCE's window state string).
    juce::String windowState() const;
    void restoreWindowState (const juce::String&);

    // For tests: the frame on screen (-1: black) and a copy of the picture.
    juce::int64 shownFrame() const;
    juce::Image snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace taw
