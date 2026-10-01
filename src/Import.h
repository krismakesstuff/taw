#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace taw
{
// Copies audio files into a session: decodes them (wav, aiff, flac, ogg, mp3, m4a/aac via
// Windows), resamples to the session rate, and writes 32-bit float WAVs into the session's
// audio folder so the session stays self-contained. Runs on its own thread.
class Import final : private juce::Thread
{
public:
    struct Item
    {
        juce::File source, copy; // copy is empty on failure
        int channels = 0;
        juce::int64 length = 0;
        juce::String error;
    };

    Import (std::vector<juce::File> sources, juce::File audioFolder, double sessionRate);
    ~Import() override;

    void start() { startThread(); }
    void cancel() { signalThreadShouldExit(); }

    bool isDone() const { return done.load(); }
    float progress() const { return progressValue.load(); }
    int total() const { return (int) items.size(); }

    // Valid once done
    const std::vector<Item>& results() const { return items; }
    bool wasCancelled() const { return cancelled; }

    // Expands folders to the audio files inside them (not recursive), sorted by name.
    static std::vector<juce::File> expand (const juce::StringArray& paths, const juce::File& relativeTo,
                                           juce::StringArray& notFound);
    static bool isAudioFile (const juce::File&);

    // Finds the file a path means even when its text lost invisible characters: names copied
    // from Mac or SMB shares map illegal characters (e.g. a trailing space) into the Unicode
    // private use area, and console drops can't deliver those. Returns a nonexistent File if
    // nothing matches.
    static juce::File resolve (const juce::String& path, const juce::File& relativeTo);

private:
    void run() override;
    void importOne (Item&);

    std::vector<Item> items;
    juce::File audioFolder;
    double sessionRate;
    juce::AudioFormatManager formats;

    std::atomic<float> progressValue { 0.0f };
    std::atomic<bool> done { false };
    bool cancelled = false;
};
} // namespace taw
