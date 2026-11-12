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

    // https://192.168.1.20:8317/k3x9p2 (HTTPS, with a certificate of taw's own, so the page may
    // record: the phone asks once whether to go on). The same address with http works too, for
    // sending files only.
    juce::String url() const;

    // The session the recordings are going into (empty: none open), for the page to show, and
    // what its click goes by: the tempo, the beats in a bar, and whether there's nothing in the
    // session yet (the phone may then set the tempo).
    void setSession (const juce::String& name, double bpm = 0.0, int beatsPerBar = 4, bool empty = false);

    // The instruments the page offers for a sung part (the scanned plugins' names).
    void setInstruments (const juce::StringArray& names);

    // A recording that came in, and what the phone asked to be made of it: "audio" (just the
    // recording), "drums" (a beat: hits on sample tracks) or "notes" (a melody, on instrument).
    struct Take
    {
        juce::File file;
        juce::String as, instrument;
        double downbeat = -1.0; // recorded on the page to its click: where in the file beat one is (seconds) ...
        double bpm = 0.0;       // ... and the click's tempo
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
