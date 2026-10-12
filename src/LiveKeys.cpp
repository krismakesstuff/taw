#include "LiveKeys.h"

namespace taw
{
namespace
{
// Semitones above A's note: the white keys on the home row, the black ones above them.
int offsetOf (wchar_t ch)
{
    switch (ch)
    {
        case 'a': return 0;   case 'w': return 1;  case 's': return 2;  case 'e': return 3;
        case 'd': return 4;   case 'f': return 5;  case 't': return 6;  case 'g': return 7;
        case 'y': return 8;   case 'h': return 9;  case 'u': return 10; case 'j': return 11;
        case 'k': return 12;  case 'o': return 13; case 'l': return 14; case 'p': return 15;
        case ';': return 16;  case '\'': return 17;
        default: return -1;
    }
}

juce::String noteName (int pitch) { return juce::MidiMessage::getMidiNoteName (pitch, true, true, 4); }

// Without polling: how long a key may go quiet before its note ends. Before the first
// auto-repeat it's the system's repeat delay; after, the gap between repeats.
constexpr double firstRepeatMs = 600.0, nextRepeatMs = 200.0;
} // namespace

void LiveKeys::setOn (bool on)
{
    if (! on)
        for (auto it = held.begin(); it != held.end();)
            noteOff ((it++)->first);

    active = on;
}

int LiveKeys::padOf (wchar_t ch)
{
    static const wchar_t rows[] = L"zxcvasdfqwer1234"; // bottom row first: pad 1 is z

    for (int i = 0; i < 16; ++i)
        if (rows[i] == ch)
            return i;

    return -1;
}

bool LiveKeys::handle (const Key& k, double nowMs)
{
    if (! active || k.code != Key::Char || k.ctrl || k.alt || k.ch == ' ')
        return false;

    const auto ch = (wchar_t) juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) k.ch);

    // Pads: shift (an upper case letter; ! @ # $ on the number row) is the accent.
    if (pads)
    {
        static const wchar_t shiftedDigits[] = L"!@#$";
        auto key = ch;
        bool accent = juce::CharacterFunctions::isUpperCase ((juce::juce_wchar) k.ch);

        for (int i = 0; i < 4; ++i)
            if (ch == shiftedDigits[i])
                key = (wchar_t) ('1' + i), accent = true;

        if (const auto pad = padOf (key); pad >= 0)
        {
            const auto it = held.find (key);

            if (it != held.end() && k.repeat)
            {
                it->second.lastSeen = nowMs; // held down: one hit, the samples are one-shots
                it->second.repeated = true;
                return true;
            }

            if (it != held.end())
                noteOff (key);

            const auto keep = vel;
            vel = accent ? 127 : keep;
            noteOn (key, padBase + pad, nowMs);
            vel = keep;
        }

        return true; // anything else typed is swallowed, as for notes
    }

    if (const auto offset = offsetOf (ch); offset >= 0)
    {
        if (const auto it = held.find (ch); it != held.end())
        {
#if JUCE_WINDOWS
            // Windows tells a new press from a held key's repeats: a new press plays again.
            if (! k.repeat)
            {
                noteOff (ch);
                noteOn (ch, base + offset, nowMs);
                return true;
            }
#endif
            it->second.lastSeen = nowMs; // held down: its auto-repeat
            it->second.repeated = true;
            return true;
        }

        noteOn (ch, base + offset, nowMs);
        return true;
    }

    // Held notes keep the pitch they started with. Velocity goes in Ableton's steps:
    // 1 20 40 60 80 100 120 127.
    if (ch == 'z') base = juce::jmax (0, base - 12);
    else if (ch == 'x') base = juce::jmin (120, base + 12);
    else if (ch == 'c') vel = vel == 127 ? 120 : vel <= 20 ? 1 : vel - 20;
    else if (ch == 'v') vel = vel == 1 ? 20 : juce::jmin (127, vel + 20);

    return true; // anything else typed is swallowed: letters are notes here, not commands
}

void LiveKeys::poll (double nowMs)
{
    for (auto it = held.begin(); it != held.end();)
    {
        const auto ch = it->first;
        const auto& h = it->second;
        ++it;

        const auto down = isDown (ch);

        if (down == 1)
        {
            polling = true;
            continue;
        }

        if (down == 0 && polling)
        {
            noteOff (ch);
            continue;
        }

        if (nowMs - h.lastSeen > (h.repeated ? nextRepeatMs : firstRepeatMs))
            noteOff (ch);
    }
}

juce::String LiveKeys::range() const
{
    return noteName (base) + "-" + noteName (juce::jmin (127, base + 17));
}

void LiveKeys::noteOn (wchar_t ch, int pitch, double nowMs)
{
    if (pitch < 0 || pitch > 127)
        return;

    send (MidiMsg::noteOn (0, pitch, vel));
    held[ch] = { pitch, nowMs, false };

    // Seen down right as it's pressed: the key state can be trusted from now on.
    if (isDown (ch) == 1)
        polling = true;
}

void LiveKeys::noteOff (wchar_t ch)
{
    if (const auto it = held.find (ch); it != held.end())
    {
        send (MidiMsg::noteOff (0, it->second.pitch));
        held.erase (it);
    }
}
} // namespace taw
