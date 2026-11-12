#pragma once

#include <juce_core/juce_core.h>

#include <memory>
#include <vector>

namespace taw
{
// The phone as a handy microphone: while it's on, taw serves one small web page on the local
// network. The phone opens it (a QR code on taw's screen carries the address), picks or makes a
// recording, and the file comes over into the session. Only while switched on, only from
// addresses on the local network, only with the token in the address, only audio.
class PhoneLink
{
public:
    PhoneLink();
    ~PhoneLink();

    // Starts listening. False (and why) if no port could be opened or there's no network.
    bool start (juce::String& error);
    void stop();
    bool running() const;

    juce::String url() const; // http://192.168.1.20:8317/k3x9p2

    // The session the recordings are going into (empty: none open), for the page to show.
    void setSession (const juce::String& name);

    // The instruments the page offers for a sung part (the scanned plugins' names).
    void setInstruments (const juce::StringArray& names);

    // A recording that came in, and what the phone asked to be made of it: "audio" (just the
    // recording), "drums" (a beat: hits on sample tracks) or "notes" (a melody, on instrument).
    struct Take
    {
        juce::File file;
        juce::String as, instrument;
    };

    // The recordings that have come in since last asked, whole, in a temporary folder that lasts
    // until stop.
    std::vector<Take> takeReceived();

    // For the panel: how many have come in, and what's happening now ("receiving take.m4a 40%").
    int received() const;
    juce::String status() const;

    // The text as a QR code: rows of modules, true where dark. Empty if it can't be made.
    static std::vector<std::vector<bool>> qr (const juce::String& text);

private:
    struct Server;
    std::unique_ptr<Server> server;
};
} // namespace taw
