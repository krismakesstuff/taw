#pragma once

#include <juce_core/juce_core.h>

// macOS has no MP3 encoder (or wma decoder) of its own, so taw uses an installed ffmpeg.
namespace taw::ffmpeg
{
inline const juce::String installHint = "install ffmpeg: brew install ffmpeg";

// ffmpeg on the PATH, or where Homebrew puts it. A nonexistent File if it isn't installed.
inline juce::File find()
{
    juce::StringArray dirs;
    dirs.addTokens (juce::SystemStats::getEnvironmentVariable ("PATH", {}), ":", {});
    dirs.addArray ({ "/opt/homebrew/bin", "/usr/local/bin" });

    for (const auto& d : dirs)
        if (juce::File::isAbsolutePath (d))
            if (const auto f = juce::File (d).getChildFile ("ffmpeg"); f.existsAsFile())
                return f;

    return {};
}
} // namespace taw::ffmpeg
