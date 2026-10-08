#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

// The hub as an audio device, for taw sessions: "taw hub" next to CoreAudio in the device
// picker. Opening it connects to the hub (starting it if it's installed), and the engine runs
// on its blocks as it would on the interface's. Its latency includes the hub's extra block, so
// recording, the picture and delay compensation need no change.
namespace taw::hub
{
inline constexpr const char* deviceTypeName = "taw hub";

// Which session this taw is playing: its name (shown in the hub) and folder (what the hub's
// routes call it; empty: the name). A device already connected tells the hub, unless
// tellConnected is false (tests running two sessions in one process).
void setClientName (const juce::String& name, const juce::String& key = {}, bool tellConnected = true);

std::unique_ptr<juce::AudioIODeviceType> makeDeviceType();

// One request to the running hub ("status", "route", "unroute": see Server::received) and its
// answer; void with the reason in error if there's no hub or no answer. Waits up to a second.
juce::var request (const juce::var& message, juce::String& error);
} // namespace taw::hub
