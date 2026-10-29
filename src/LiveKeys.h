#pragma once

#include "Console.h"
#include "Midi.h"

#include <functional>
#include <map>

namespace taw
{
// Keys mode (ctrl+k): the typing keyboard is a MIDI keyboard, laid out like Ableton's. The home
// row is the white keys (A is C4), the row above the black ones; Z X shift an octave, C V the
// velocity. Its notes go in as one more controller, so armed instrument tracks play them and
// recording writes them down.
//
// Terminals report presses, not releases (Windows' console aside), so a note ends when its key
// is seen to be up (Console::keyDown, polled each frame). Where that can't be told, a note ends
// once its key stops auto-repeating.
class LiveKeys
{
public:
    using Send = std::function<void (MidiMsg)>;
    using IsDown = std::function<int (wchar_t)>; // 1 down, 0 up, -1 can't tell

    LiveKeys (Send s, IsDown d) : send (std::move (s)), isDown (std::move (d)) {}

    bool on() const { return active; }
    void setOn (bool); // off lets go of every note

    // On a kit the keys are a 4x4 pad grid, laid out like an MPC's (pad 1 bottom left):
    //   1 2 3 4   pads 13-16
    //   q w e r   pads  9-12
    //   a s d f   pads  5-8
    //   z x c v   pads  1-4
    // sending notes from firstPad up; shift is the accent (full velocity).
    void setPads (bool on, int firstPad = 36) { pads = on; padBase = firstPad; }
    bool padsOn() const { return pads; }
    static int padOf (wchar_t); // 0-15, or -1

    // A key while on: true if keys mode used it (a note, the octave or the velocity, or a
    // character it swallows so it can't type commands). Space, arrows, esc and ctrl/alt keys
    // are left to taw.
    bool handle (const Key&, double nowMs);
    bool swallowed() const { return lastSwallowed; } // the last key handled played nothing: a shortcut, kept from taw

    void poll (double nowMs); // ends the notes whose keys are up

    int velocity() const { return vel; }
    juce::String range() const; // "C4-F5": what the keys play now

private:
    struct Held
    {
        int pitch = 0;
        double lastSeen = 0.0; // the press, or its latest auto-repeat
        bool repeated = false;
    };

    void noteOn (wchar_t, int pitch, double nowMs);
    void noteOff (wchar_t);

    Send send;
    IsDown isDown;
    bool active = false;
    bool pads = false;
    int padBase = 36;
    int base = 60; // A's note
    int vel = 100;
    bool lastSwallowed = false;
    bool polling = false; // keyDown has said a key is down: it can be trusted to say it's up
    std::map<wchar_t, Held> held;
};
} // namespace taw
