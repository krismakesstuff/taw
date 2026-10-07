#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

namespace taw
{
// A parameter's automation as the audio thread plays it: points by timeline position
// (samples), straight lines between them (held, for switches), flat before the first point and
// after the last. Values are in the parameter's own unit (Automation::Target says which).
// A controller moving a parameter right now (touch): while on, the parameter is this value,
// whatever its curve says. Written by the message thread, read by the audio thread; kept
// across graph rebuilds (the App owns it, each graph's curve points at it).
struct LiveValue
{
    std::atomic<bool> on { false };
    std::atomic<float> value { 0.0f };
};

struct Curve
{
    std::vector<juce::int64> pos;
    std::vector<float> val;
    bool steps = false;
    std::shared_ptr<LiveValue> live; // a mapped controller (null: none)
    float base = 0.0f;               // with no points: the parameter's setting

    // Nothing that could ever change the parameter: no points, no controller.
    bool empty() const noexcept { return pos.empty() && live == nullptr; }
    // Something sets the parameter right now (plugins are left alone otherwise).
    bool active() const noexcept { return ! pos.empty() || (live != nullptr && live->on.load (std::memory_order_relaxed)); }

    float at (juce::int64 p) const noexcept
    {
        if (live != nullptr && live->on.load (std::memory_order_relaxed))
            return live->value.load (std::memory_order_relaxed);

        if (pos.empty())
            return base;

        const auto it = std::upper_bound (pos.begin(), pos.end(), p);

        if (it == pos.begin())
            return val.front();

        const auto i = (size_t) (it - pos.begin()); // the first point after p

        if (i == pos.size())
            return val.back();

        const auto a = pos[i - 1], b = pos[i];

        if (steps || b == a)
            return val[i - 1];

        return val[i - 1] + (val[i] - val[i - 1]) * (float) ((double) (p - a) / (double) (b - a));
    }
};

// Automated values change in steps this long (under a millisecond), each at its start's value.
constexpr int automationStep = 32;

// What an automation lane (an AUTO node: on a TRACK, or on the SESSION for the mix) controls,
// from its param string:
//   "vol"             the fader, in dB
//   "pan"             -1 (left) to 1 (right)
//   "mute"            0 or 1 (held, not ramped)
//   "send a".."send e" a send's level, in dB
//   "inst/<id>"       the instrument plugin's parameter <id>, 0 to 1
//   "fx a/<id>"       the effect in insert slot a-e: its parameter <id>, 0 to 1
struct AutomationTarget
{
    enum class Kind { none, volume, pan, mute, send, instrument, effect };

    Kind kind = Kind::none;
    int slot = -1;         // send or insert slot, 0-4
    juce::String paramId;  // plugin parameters

    static AutomationTarget parse (const juce::String& param)
    {
        AutomationTarget t;
        const auto p = param.trim();
        auto slotOf = [] (juce::juce_wchar c) { return c >= 'a' && c <= 'e' ? (int) (c - 'a') : c >= 'A' && c <= 'E' ? (int) (c - 'A') : -1; };

        if (p == "vol")
            t.kind = Kind::volume;
        else if (p == "pan")
            t.kind = Kind::pan;
        else if (p == "mute")
            t.kind = Kind::mute;
        else if (p.startsWith ("send ") && p.length() == 6 && slotOf (p[5]) >= 0)
            t.kind = Kind::send, t.slot = slotOf (p[5]);
        else if (p.startsWith ("inst/") && p.length() > 5)
            t.kind = Kind::instrument, t.paramId = p.substring (5);
        else if (p.startsWith ("fx ") && p.length() > 5 && p[4] == '/' && slotOf (p[3]) >= 0)
            t.kind = Kind::effect, t.slot = slotOf (p[3]), t.paramId = p.substring (5);

        return t;
    }

    bool isPlugin() const { return kind == Kind::instrument || kind == Kind::effect; }
    bool isDecibels() const { return kind == Kind::volume || kind == Kind::send; }

    // The range values are kept in, and where a new lane starts when nothing says otherwise.
    float minimum() const { return isDecibels() ? -96.0f : kind == Kind::pan ? -1.0f : 0.0f; }
    float maximum() const { return isDecibels() ? 12.0f : 1.0f; }
    float clamp (float v) const { return juce::jlimit (minimum(), maximum(), v); }
};
} // namespace taw
