#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>

namespace taw
{
// The phone link over HTTPS (Mbed TLS). A phone's browser lets a page record only over HTTPS,
// so the link has a key and a certificate of its own: signed by nobody, which the phone says
// once ("not private") and then remembers. They're made the first time and kept, so it stays
// once.
class PhoneTls
{
public:
    PhoneTls();
    ~PhoneTls();

    // The key and certificate for this machine's address, from the folder or made into it.
    bool prepare (const juce::File& folder, const juce::String& address, juce::String& error);

    // One connection.
    class Line
    {
    public:
        ~Line();

        int read (void* to, int most, int timeoutMs); // bytes; 0: none in that time; under 0: closed
        bool write (const void* from, int size);

    private:
        friend class PhoneTls;
        struct State;
        std::unique_ptr<State> state;
    };

    // The handshake, on a connection whose first bytes were read already. Null if it fails
    // (a phone that hasn't said yes to the certificate yet hangs up here).
    std::unique_ptr<Line> accept (juce::StreamingSocket& socket, const void* first, int firstSize, const std::atomic<bool>& stopping);

private:
    struct State;
    std::unique_ptr<State> state;
};
} // namespace taw
