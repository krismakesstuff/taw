// Reading an Ableton Live set. A .als is gzipped XML: Ableton > LiveSet > Tracks (MidiTrack,
// AudioTrack, GroupTrack, ReturnTrack), MasterTrack, Locators, Transport. Values sit in Value
// attributes ("<Manual Value="0.5" />"); automation envelopes point at an AutomationTarget Id
// somewhere in their track. Positions are in beats, like taw's notes and automation.

#include "AlsImport.h"

#include "Automation.h"
#include "Ids.h"
#include "Session.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <map>

namespace taw
{
namespace
{
using Xml = juce::XmlElement;

// "<Name Value="x" />" at a path of child tags, e.g. value (t, "Name/EffectiveName").
const Xml* at (const Xml* node, const juce::String& path)
{
    for (const auto& tag : juce::StringArray::fromTokens (path, "/", {}))
    {
        if (node == nullptr)
            return nullptr;

        node = node->getChildByName (tag);
    }

    return node;
}

juce::String value (const Xml* node, const juce::String& path, const juce::String& otherwise = {})
{
    const auto* n = at (node, path);
    return n != nullptr && n->hasAttribute ("Value") ? n->getStringAttribute ("Value") : otherwise;
}

double number (const Xml* node, const juce::String& path, double otherwise = 0.0)
{
    const auto v = value (node, path);
    return v.isEmpty() ? otherwise : v.getDoubleValue();
}

bool flag (const Xml* node, const juce::String& path, bool otherwise = false)
{
    const auto v = value (node, path);
    return v.isEmpty() ? otherwise : v == "true";
}

// Live's faders and sends are gain (1 = 0 dB); its sends bottom out at -70 dB, which is off.
double toDb (double gain, double floorDb = -96.0)
{
    if (gain <= 0.0)
        return -96.0;

    const auto db = 20.0 * std::log10 (gain);
    return db <= floorDb ? -96.0 : juce::jlimit (-96.0, 12.0, db);
}

// Live's automation starts with an event far before the song (its value up to the first real
// one); that's beat 0 here.
constexpr double beforeTheSong = -60000.0;

struct Plugin
{
    juce::String name;
    juce::MemoryBlock state; // JUCE's VST3 state (VST3PluginState with the component's), empty if none
    bool on = true, vst3 = true;
};

// A Live device: a VST3 (or VST2) plugin with its saved state, or one of Live's own (name only).
struct Device
{
    const Xml* node = nullptr;
    bool isPlugin = false;
    Plugin plugin;
    juce::String liveName; // Live's own device: "EQ Three", "Compressor"
};

juce::String liveDeviceName (const juce::String& tag)
{
    static const std::map<juce::String, juce::String> names {
        { "FilterEQ3", "EQ Three" }, { "Eq8", "EQ Eight" }, { "Compressor2", "Compressor" }, { "GlueCompressor", "Glue Compressor" },
        { "PhaserNew", "Phaser" }, { "Reverb", "Reverb" }, { "Delay", "Delay" }, { "Saturator", "Saturator" },
        { "AutoFilter", "Auto Filter" }, { "Limiter", "Limiter" }, { "Utility", "Utility" }, { "StereoGain", "Utility" },
        { "Chorus2", "Chorus-Ensemble" }, { "OriginalSimpler", "Simpler" }, { "MultiSampler", "Sampler" },
        { "InstrumentGroupDevice", "Instrument Rack" }, { "AudioEffectGroupDevice", "Audio Effect Rack" },
        { "DrumGroupDevice", "Drum Rack" }, { "MidiEffectGroupDevice", "MIDI Effect Rack" },
    };

    const auto it = names.find (tag);
    return it != names.end() ? it->second : tag;
}

Device readDevice (const Xml& d)
{
    Device out;
    out.node = &d;

    if (d.getTagName() != "PluginDevice")
    {
        out.liveName = liveDeviceName (d.getTagName());
        return out;
    }

    out.isPlugin = true;
    out.plugin.on = flag (&d, "On/Manual", true);

    if (const auto* info = at (&d, "PluginDesc/Vst3PluginInfo"))
    {
        out.plugin.name = value (info, "Name");

        // JUCE keeps a VST3's state as the plugin's own component state (and its controller's),
        // base64 in a little XML; Live keeps the same bytes as hex. Re-wrapped, the patch comes
        // across as it was.
        const auto hex = [&] (const juce::String& tag)
        {
            const auto* n = at (info, "Preset/Vst3Preset/" + tag);
            return n != nullptr ? n->getAllSubText().removeCharacters (" \t\r\n") : juce::String();
        };

        juce::MemoryBlock component, controller;

        if (const auto h = hex ("ProcessorState"); h.isNotEmpty())
            component.loadFromHexString (h);

        if (const auto h = hex ("ControllerState"); h.isNotEmpty())
            controller.loadFromHexString (h);

        if (component.getSize() > 0)
        {
            Xml state ("VST3PluginState");
            state.createNewChildElement ("IComponent")->addTextElement (component.toBase64Encoding());

            if (controller.getSize() > 0)
                state.createNewChildElement ("IEditController")->addTextElement (controller.toBase64Encoding());

            juce::AudioProcessor::copyXmlToBinary (state, out.plugin.state);
        }
    }
    else if (const auto* vst2 = at (&d, "PluginDesc/VstPluginInfo"))
    {
        out.plugin.name = value (vst2, "PlugName");
        out.plugin.vst3 = false; // its settings are in a format JUCE doesn't take back
    }
    else
    {
        out.plugin.name = value (&d, "PluginDesc/AuPluginInfo/Name");
        out.plugin.vst3 = false;
    }

    return out;
}

std::vector<Device> devicesOf (const Xml& track)
{
    std::vector<Device> out;

    if (const auto* chain = at (&track, "DeviceChain/DeviceChain/Devices"))
        for (const auto* d : chain->getChildIterator())
            out.push_back (readDevice (*d));

    return out;
}

// What an automation envelope moves, in taw's terms: a lane param and how to turn Live's value
// into the lane's.
struct Target
{
    enum class Convert { gainToDb, sendToDb, same, bool01Inverted, normalise };

    Target (juce::String p = {}, Convert c = Convert::same, double lo = 0.0, double hi = 1.0, juce::String why = {})
        : param (std::move (p)), convert (c), min (lo), max (hi), skipped (std::move (why)) {}

    juce::String param;
    Convert convert;
    double min, max;      // normalise: the parameter's range in Live
    juce::String skipped; // why it can't come across (empty: it can)
};

double convertValue (const Target& t, double v)
{
    switch (t.convert)
    {
        case Target::Convert::gainToDb:       return toDb (v);
        case Target::Convert::sendToDb:       return toDb (v, -69.95);
        case Target::Convert::bool01Inverted: return v >= 0.5 ? 0.0 : 1.0; // Live's "Speaker" on = not muted
        case Target::Convert::normalise:      return t.max > t.min ? juce::jlimit (0.0, 1.0, (v - t.min) / (t.max - t.min)) : 0.0;
        case Target::Convert::same:
        default:                              return v;
    }
}

struct TrackReader
{
    double sampleRate = 44100.0, bpm = 120.0;
    juce::File setFolder;
    AbletonSet& out;
    std::map<juce::String, int> busOfGroup; // Live group track Id -> taw busId
    std::vector<int> busOfReturn;           // send i -> taw busId
    std::vector<bool> sendPre;              // send i is pre-fader
    int trackNumber = 0;                    // for plugin state file names

    double samplesPerBeat() const { return sampleRate * 60.0 / bpm; }

    juce::String stateFileFor (const juce::String& who, const juce::String& plugin)
    {
        auto base = who + "-" + juce::File::createLegalFileName (plugin).replaceCharacter (' ', '_');
        auto path = "plugins/" + base + ".state";

        for (int n = 2; std::any_of (out.files.begin(), out.files.end(), [&] (const auto& f) { return f.path == path; }); ++n)
            path = "plugins/" + base + "-" + juce::String (n) + ".state";

        return path;
    }

    // The plugin's node properties (plugin name, saved state) on a track or an INSERT.
    void setPlugin (juce::ValueTree node, const Plugin& p, const juce::String& who, const juce::String& where)
    {
        node.setProperty (ids::pluginName, p.name, nullptr);

        if (p.state.getSize() > 0)
        {
            const auto path = stateFileFor (who, p.name);
            out.files.push_back ({ path, p.state });
            node.setProperty (ids::pluginState, path, nullptr);
        }
        else
        {
            out.report.add (where + ": " + p.name + (p.vst3 ? " has no saved settings in the set" : " isn't VST3, so it starts from its default settings"));
        }
    }

    // Plugins after the instrument go in the inserts, A to E. Live's own devices are named in the
    // report; there's nothing in taw that plays them.
    std::map<int, juce::String> addInserts (juce::ValueTree owner, const std::vector<Device>& devices, size_t from, const juce::String& who,
                                           const juce::String& where)
    {
        std::map<int, juce::String> prefixOf; // device index -> its lanes' prefix ("fx a/")
        int slot = 0;
        juce::StringArray skipped;

        for (size_t i = from; i < devices.size(); ++i)
        {
            const auto& d = devices[i];

            if (! d.isPlugin)
            {
                if (flag (d.node, "On/Manual", true)) // one switched off in Live made no sound anyway
                    skipped.add (d.liveName);

                continue;
            }

            if (slot >= Session::numSends)
            {
                skipped.add (d.plugin.name + " (no free insert)");
                continue;
            }

            juce::ValueTree insert (ids::INSERT);
            insert.setProperty (ids::slot, Session::slotName (slot).toLowerCase(), nullptr);

            if (! d.plugin.on)
                insert.setProperty (ids::bypass, true, nullptr);

            setPlugin (insert, d.plugin, who, where);
            owner.appendChild (insert, nullptr);
            prefixOf[(int) i] = "fx " + Session::slotName (slot).toLowerCase() + "/";
            ++slot;
        }

        if (! skipped.isEmpty())
            out.report.add (where + ": Live's own " + skipped.joinIntoString (", ") + L" didn't come across \x2014 put a plugin in its place");

        return prefixOf;
    }

    // Automation: each envelope onto a lane, its events as points (straight lines between them).
    void addAutomation (juce::ValueTree owner, const Xml& track, const std::map<juce::String, Target>& targets, const juce::String& where,
                        const juce::StringArray& lanesOff = {})
    {
        const auto* envelopes = at (&track, "AutomationEnvelopes/Envelopes");

        if (envelopes == nullptr)
            return;

        for (const auto* env : envelopes->getChildIterator())
        {
            const auto id = value (env, "EnvelopeTarget/PointeeId");
            const auto it = targets.find (id);

            if (it == targets.end())
            {
                out.report.add (where + ": automation of something taw doesn't have (a Live device's control) was left out");
                continue;
            }

            const auto& t = it->second;

            if (t.skipped.isNotEmpty())
            {
                // One value all along (Live keeps a single event for a control never moved) isn't
                // a change worth reporting.
                juce::StringArray values;

                if (const auto* events = at (env, "Automation/Events"))
                    for (const auto* e : events->getChildIterator())
                        values.addIfNotAlreadyThere (e->getStringAttribute ("Value"));

                if (values.size() > 1)
                    out.report.add (where + ": " + t.skipped);

                continue;
            }

            juce::ValueTree lane (ids::AUTO);
            lane.setProperty (ids::param, t.param, nullptr);
            const auto range = AutomationTarget::parse (t.param);

            if (const auto* events = at (env, "Automation/Events"))
                for (const auto* e : events->getChildIterator())
                {
                    const auto time = e->getDoubleAttribute ("Time");
                    const auto raw = e->getStringAttribute ("Value");
                    const auto v = raw == "true" ? 1.0 : raw == "false" ? 0.0 : raw.getDoubleValue();
                    lane.appendChild (Session::makePoint (time < beforeTheSong ? 0.0 : time, range.clamp ((float) convertValue (t, v))), nullptr);
                }

            if (lane.getNumChildren() == 0)
                continue;

            if (lanesOff.contains (t.param))
                lane.setProperty (ids::bypass, true, nullptr);

            owner.appendChild (lane, nullptr);
        }
    }

    // The mixer's automatable controls, by their AutomationTarget Id.
    void mixerTargets (const Xml& track, std::map<juce::String, Target>& targets, bool isMaster)
    {
        const auto* mixer = at (&track, "DeviceChain/Mixer");

        auto add = [&] (const juce::String& path, Target t)
        {
            if (const auto* a = at (mixer, path + "/AutomationTarget"))
                targets[a->getStringAttribute ("Id")] = t;
        };

        add ("Volume", { "vol", Target::Convert::gainToDb });

        if (isMaster)
        {
            add ("Pan", { {}, Target::Convert::same, 0, 1, "the master's pan automation was left out (the mix has no pan)" });
            add ("Tempo", { {}, Target::Convert::same, 0, 1, "tempo changes were left out (taw has one tempo)" });
            add ("TimeSignature", { {}, Target::Convert::same, 0, 1, "time signature changes were left out" });
            return;
        }

        add ("Pan", { "pan", Target::Convert::same });
        add ("Speaker", { "mute", Target::Convert::bool01Inverted });

        if (const auto* sends = at (mixer, "Sends"))
        {
            int i = 0;

            for (const auto* holder : sends->getChildIterator())
            {
                if (const auto* a = at (holder, "Send/AutomationTarget"))
                    targets[a->getStringAttribute ("Id")] = i < Session::numSends
                                                                ? Target { "send " + Session::slotName (i).toLowerCase(), Target::Convert::sendToDb }
                                                                : Target { {}, Target::Convert::same, 0, 1, "a send past the fifth was left out" };
                ++i;
            }
        }
    }

    // A device's plugin parameters as lane targets (prefix: "inst/" or "fx a/"; empty: the device
    // didn't come across).
    void pluginTargets (const Device& d, const juce::String& prefix, std::map<juce::String, Target>& targets)
    {
        // PluginFloatParameter sits under ParameterList, or deeper in older sets: search it all.
        std::function<void (const Xml&)> walk = [&] (const Xml& n)
        {
            for (const auto* c : n.getChildIterator())
            {
                if (c->getTagName() == "PluginFloatParameter")
                {
                    const auto* a = at (c, "ParameterValue/AutomationTarget");

                    if (a == nullptr)
                        continue;

                    const auto id = value (c, "ParameterId");
                    const auto name = value (c, "ParameterName");
                    Target t;

                    if (prefix.isEmpty())
                        t.skipped = "automation of " + name + " on " + d.plugin.name + " was left out (the plugin didn't come across)";
                    else if (id.isEmpty() || id == "-1")
                        t.skipped = "automation of " + name + " on " + d.plugin.name + " was left out (Live didn't save which parameter it is)";
                    else
                        t = { prefix + id, Target::Convert::normalise, number (c, "ParameterValue/MidiControllerRange/Min", 0.0),
                              number (c, "ParameterValue/MidiControllerRange/Max", 1.0) };

                    targets[a->getStringAttribute ("Id")] = t;
                    continue;
                }

                walk (*c);
            }
        };

        walk (*d.node);
    }

    void liveDeviceTargets (const Device& d, std::map<juce::String, Target>& targets, const juce::String& where)
    {
        // Every automatable control of a Live device: left out, by name.
        std::function<void (const Xml&, const juce::String&)> walk = [&] (const Xml& n, const juce::String& control)
        {
            for (const auto* c : n.getChildIterator())
            {
                if (c->getTagName() == "AutomationTarget")
                    targets[c->getStringAttribute ("Id")] = { {}, Target::Convert::same, 0, 1,
                                                              "automation of " + d.liveName + " (" + control + ") was left out" };
                else
                    walk (*c, c->getTagName());
            }
        };

        walk (*d.node, {});
        juce::ignoreUnused (where);
    }

    // Live's mixer settings onto a taw track (or, isMaster, the session's mix).
    void readMixer (juce::ValueTree owner, const Xml& track, bool isMaster, bool isReturn)
    {
        const auto* mixer = at (&track, "DeviceChain/Mixer");
        juce::ignoreUnused (isReturn);

        if (isMaster)
        {
            owner.setProperty (ids::mixDb, toDb (number (mixer, "Volume/Manual", 1.0)), nullptr);
            return;
        }

        owner.setProperty (ids::gainDb, toDb (number (mixer, "Volume/Manual", 1.0)), nullptr);

        if (const auto pan = number (mixer, "Pan/Manual"); std::abs (pan) > 0.001)
            owner.setProperty (ids::pan, juce::jlimit (-1.0, 1.0, pan), nullptr);

        if (! flag (mixer, "Speaker/Manual", true))
            owner.setProperty (ids::mute, true, nullptr);

        if (flag (mixer, "SoloSink"))
            owner.setProperty (ids::solo, true, nullptr);

        if (const auto* sends = at (mixer, "Sends"))
        {
            int i = 0;

            for (const auto* holder : sends->getChildIterator())
            {
                const auto db = toDb (number (holder, "Send/Manual"), -69.95);

                // (A return's sends to the other returns come across too; Live doesn't let one feed itself.)
                if (i < (int) busOfReturn.size() && i < Session::numSends && db > -95.0 && flag (holder, "Active", true))
                {
                    juce::ValueTree send (ids::SEND);
                    send.setProperty (ids::slot, Session::slotName (i).toLowerCase(), nullptr)
                        .setProperty (ids::to, busOfReturn[(size_t) i], nullptr)
                        .setProperty (ids::db, db, nullptr);

                    if (i < (int) sendPre.size() && sendPre[(size_t) i])
                        send.setProperty (ids::pre, true, nullptr);

                    owner.appendChild (send, nullptr);
                }

                ++i;
            }
        }
    }

    juce::ValueTree midiClipPiece (const Xml& clip, double at, double offset, double len)
    {
        auto piece = Session::makeMidiClip (at, offset, len);

        if (const auto* keyTracks = ::taw::at (&clip, "Notes/KeyTracks"))
            for (const auto* kt : keyTracks->getChildIterator())
            {
                const auto pitch = (int) number (kt, "MidiKey", 60);

                if (const auto* notes = ::taw::at (kt, "Notes"))
                    for (const auto* n : notes->getChildIterator())
                    {
                        const auto beat = n->getDoubleAttribute ("Time");

                        if (! n->getBoolAttribute ("IsEnabled", true) || beat < offset - 1.0e-9 || beat >= offset + len - 1.0e-9)
                            continue;

                        piece.appendChild (Session::makeNote (beat, n->getDoubleAttribute ("Duration"), pitch,
                                                              juce::jlimit (1, 127, juce::roundToInt (n->getDoubleAttribute ("Velocity", 100.0)))),
                                           nullptr);
                    }
            }

        return piece;
    }

    // A Live MIDI clip as taw MIDI clips: one window onto its notes, or, looped, a window per
    // pass of the loop (taw's clips don't loop).
    void readMidiClips (juce::ValueTree track, const Xml& t, const juce::String& where)
    {
        const auto* events = at (&t, "DeviceChain/MainSequencer/ClipTimeable/ArrangerAutomation/Events");
        int envelopes = 0, muted = 0;

        if (events != nullptr)
            for (const auto* clip : events->getChildIterator())
            {
                if (clip->getTagName() != "MidiClip")
                    continue;

                if (flag (clip, "Disabled"))
                {
                    ++muted;
                    continue;
                }

                const auto start = number (clip, "CurrentStart"), end = number (clip, "CurrentEnd");
                const auto loopStart = number (clip, "Loop/LoopStart"), loopEnd = number (clip, "Loop/LoopEnd");
                const auto contentStart = loopStart + number (clip, "Loop/StartRelative");

                if (! flag (clip, "Loop/LoopOn"))
                {
                    track.appendChild (midiClipPiece (*clip, start, contentStart, end - start), nullptr);
                }
                else if (loopEnd > loopStart + 1.0e-6)
                {
                    for (double pos = start, from = contentStart; pos < end - 1.0e-6;)
                    {
                        const auto len = std::min (loopEnd - from, end - pos);

                        if (len <= 1.0e-6)
                        {
                            from = loopStart;
                            continue;
                        }

                        track.appendChild (midiClipPiece (*clip, pos, from, len), nullptr);
                        pos += len;
                        from = loopStart;
                    }
                }

                if (const auto* env = at (clip, "Envelopes/Envelopes"))
                    envelopes += env->getNumChildElements();
            }

        if (muted > 0)
            out.report.add (where + ": " + juce::String (muted) + " muted clip" + (muted == 1 ? " was" : "s were") + " left out");

        if (envelopes > 0)
            out.report.add (where + ": " + juce::String (envelopes) + " clip envelope" + (envelopes == 1 ? " was" : "s were") + " left out");

        if (const auto* slots = at (&t, "DeviceChain/MainSequencer/ClipSlotList"))
        {
            int sessionClips = 0;

            for (const auto* s : slots->getChildIterator())
                if (const auto* v = at (s, "ClipSlot/Value"); v != nullptr && v->getNumChildElements() > 0)
                    ++sessionClips;

            if (sessionClips > 0)
                out.report.add (where + ": " + juce::String (sessionClips) + " Session View clip" + (sessionClips == 1 ? " was" : "s were")
                                + " left out (the arrangement is what came across)");
        }
    }

    juce::File sampleFile (const Xml& clip) const
    {
        const auto* ref = at (&clip, "SampleRef/FileRef");
        const auto path = value (ref, "Path");

        if (path.isNotEmpty() && juce::File::isAbsolutePath (path) && juce::File (path).existsAsFile())
            return juce::File (path);

        // Older sets: the path relative to the project.
        if (const auto rel = value (ref, "RelativePath"); rel.isNotEmpty())
            if (const auto f = setFolder.getChildFile (rel); f.existsAsFile())
                return f;

        return juce::File (path);
    }

    // Audio clips: copied into the session and placed by time. A clip Live time-stretches (warped
    // to a tempo other than the set's) can't play right here, so it's left for an export.
    // Returns true if any was left out that way.
    bool readAudioClips (juce::ValueTree track, const Xml& t, const juce::String& where)
    {
        const auto* events = at (&t, "DeviceChain/MainSequencer/Sample/ArrangerAutomation/Events");
        bool stretched = false;
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::map<juce::String, juce::String> copied; // source file -> its path in the session

        if (events == nullptr)
            return false;

        for (const auto* clip : events->getChildIterator())
        {
            if (clip->getTagName() != "AudioClip")
                continue;

            const auto name = value (clip, "Name");

            if (flag (clip, "Disabled"))
            {
                out.report.add (where + ": muted clip " + name.quoted() + " was left out");
                continue;
            }

            const auto file = sampleFile (*clip);
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

            if (reader == nullptr)
            {
                out.report.add (where + ": couldn't read " + file.getFullPathName() + " (clip " + name.quoted() + ")");
                continue;
            }

            const auto fileRate = reader->sampleRate;
            const auto start = number (clip, "CurrentStart"), end = number (clip, "CurrentEnd");
            const auto contentStart = number (clip, "Loop/LoopStart") + number (clip, "Loop/StartRelative");
            double offsetSeconds = contentStart; // unwarped: Live's clip times are seconds into the file

            if (flag (clip, "IsWarped"))
            {
                // Warp markers pair seconds in the file with beats in the clip. Between the first
                // and last, that's a tempo: the set's, and the audio plays as recorded.
                std::vector<std::pair<double, double>> markers; // (seconds, beats)

                if (const auto* wm = at (clip, "WarpMarkers"))
                    for (const auto* m : wm->getChildIterator())
                        markers.push_back ({ m->getDoubleAttribute ("SecTime"), m->getDoubleAttribute ("BeatTime") });

                if (markers.size() < 2 || markers.back().second <= markers.front().second)
                {
                    out.report.add (where + ": clip " + name.quoted() + " is warped in a way taw can't follow; it was left out");
                    continue;
                }

                const auto clipBpm = 60.0 * (markers.back().second - markers.front().second) / (markers.back().first - markers.front().first);

                if (std::abs (clipBpm / bpm - 1.0) > 0.001 || markers.size() > 2)
                {
                    stretched = true;
                    continue;
                }

                offsetSeconds = markers.front().first + (contentStart - markers.front().second) * 60.0 / bpm;
            }

            if (flag (clip, "Loop/LoopOn"))
                out.report.add (where + ": clip " + name.quoted() + " loops in Live; it plays once here");

            if (std::abs (fileRate - sampleRate) > 0.5)
                out.report.add (where + ": " + file.getFileName() + " is " + juce::String (fileRate / 1000.0, 1) + " kHz and the session "
                                + juce::String (sampleRate / 1000.0, 1) + L" kHz \x2014 it will play off-speed");

            if (std::abs (number (clip, "SampleVolume", 1.0) - 1.0) > 0.001 || number (clip, "Fades/FadeInLength") > 0.01
                || number (clip, "Fades/FadeOutLength") > 0.01)
                out.report.add (where + ": clip " + name.quoted() + "'s gain and fades in Live didn't come across");

            auto& to = copied[file.getFullPathName()];

            if (to.isEmpty())
            {
                to = "audio/" + juce::File::createLegalFileName (file.getFileName());

                for (int n = 2; std::any_of (out.copies.begin(), out.copies.end(), [&] (const auto& c) { return c.to == to; }); ++n)
                    to = "audio/" + file.getFileNameWithoutExtension() + "-" + juce::String (n) + file.getFileExtension();

                out.copies.push_back ({ file, to });
            }

            const auto spb = samplesPerBeat();
            track.appendChild (Session::makeClip (to, std::llround (start * spb), std::llround (offsetSeconds * fileRate),
                                                  std::llround ((end - start) * spb)),
                               nullptr);
        }

        return stretched;
    }

    void readTrack (juce::ValueTree session, const Xml& t)
    {
        const auto tag = t.getTagName();
        const auto name = value (&t, "Name/EffectiveName", "Track");
        const auto where = name;
        const bool isReturn = tag == "ReturnTrack", isGroup = tag == "GroupTrack", isMidi = tag == "MidiTrack";
        ++trackNumber;
        const auto who = juce::String (trackNumber).paddedLeft ('0', 2);

        juce::ValueTree track (ids::TRACK);
        track.setProperty (ids::name, name, nullptr);
        track.setProperty (ids::kind, isReturn || isGroup ? "bus" : isMidi ? "instrument" : "audio", nullptr);

        // (A return's busId is set by the caller, which knows which return it is.)
        if (isGroup)
            track.setProperty (ids::busId, busOfGroup[t.getStringAttribute ("Id")], nullptr);

        // In a group: into its bus.
        if (const auto group = value (&t, "TrackGroupId", "-1"); group != "-1" && busOfGroup.count (group) > 0)
            track.setProperty (ids::out, busOfGroup[group], nullptr);

        readMixer (track, t, false, isReturn);

        // Devices: on a MIDI track the first is its instrument; the plugins after it are inserts.
        std::map<juce::String, Target> targets;
        mixerTargets (t, targets, false);
        const auto devices = devicesOf (t);
        size_t firstInsert = 0;

        if (isMidi)
        {
            if (! devices.empty() && devices.front().isPlugin)
            {
                setPlugin (track, devices.front().plugin, who, where);
                pluginTargets (devices.front(), "inst/", targets);
                firstInsert = 1;
            }
            else
            {
                out.report.add (where + ": " + (devices.empty() ? juce::String ("no instrument")
                                                                 : "Live's " + devices.front().liveName + L" (no instrument plugin)")
                                + L" \x2014 inst <name> picks one");

                if (! devices.empty())
                    liveDeviceTargets (devices.front(), targets, where);

                firstInsert = devices.empty() ? 0 : 1;
            }
        }

        const auto prefixes = addInserts (track, devices, firstInsert, who, where);

        for (size_t i = firstInsert; i < devices.size(); ++i)
        {
            if (devices[i].isPlugin)
                pluginTargets (devices[i], prefixes.count ((int) i) > 0 ? prefixes.at ((int) i) : juce::String(), targets);
            else
                liveDeviceTargets (devices[i], targets, where);
        }

        juce::StringArray lanesOff;

        if (isMidi)
        {
            readMidiClips (track, t, where);
        }
        else if (tag == "AudioTrack" && readAudioClips (track, t, where))
        {
            // Time-stretched in Live: export it from Live (its fader, devices and automation are
            // in the export) and drop it here at bar 1. Its volume automation is kept but off,
            // and its fader at 0 dB, so it isn't applied twice.
            out.report.add (where + L": its audio is time-stretched in Live, which taw can't do yet \x2014 export the track from Live "
                                    L"from bar 1 and import it onto this track at bar 1. Its volume automation is here but off (shift+O), "
                                    L"its fader at 0 dB, since the export has them");
            track.setProperty (ids::gainDb, 0.0, nullptr);
            lanesOff.add ("vol");
        }

        if (isGroup)
            out.report.add (where + ": a group in Live, a bus here (its tracks play into it)");

        addAutomation (track, t, targets, where, lanesOff);

        if (flag (&t, "Freeze"))
            out.report.add (where + ": frozen in Live; it came across unfrozen");

        session.appendChild (track, nullptr);
    }
};
} // namespace

AbletonSet readAbletonSet (const juce::File& als, double sampleRate)
{
    AbletonSet out;
    juce::FileInputStream raw (als);

    if (raw.failedToOpen())
    {
        out.error = "couldn't open " + als.getFullPathName();
        return out;
    }

    juce::GZIPDecompressorInputStream unzipped (&raw, false, juce::GZIPDecompressorInputStream::gzipFormat);
    const auto text = unzipped.readEntireStreamAsString();
    const auto root = juce::XmlDocument::parse (text);
    const auto* set = root != nullptr ? root->getChildByName ("LiveSet") : nullptr;

    if (set == nullptr)
    {
        out.error = als.getFileName() + " isn't a Live set taw can read";
        return out;
    }

    const auto* master = set->getChildByName ("MasterTrack");
    const auto* tracks = set->getChildByName ("Tracks");

    if (master == nullptr || tracks == nullptr)
    {
        out.error = als.getFileName() + " has no tracks";
        return out;
    }

    TrackReader reader { sampleRate, 120.0, als.getParentDirectory(), out, {}, {}, {}, 0 };
    reader.bpm = juce::jlimit (20.0, 999.0, number (master, "DeviceChain/Mixer/Tempo/Manual", 120.0));

    // Live's time signature is one number: numerator - 1 + 99 x log2 (denominator). 201 is 4/4.
    const auto sig = (int) number (master, "DeviceChain/Mixer/TimeSignature/Manual", 201);
    const auto beatsPerBar = sig % 99 + 1, denominator = 1 << juce::jlimit (0, 5, sig / 99);

    out.session = juce::ValueTree (ids::SESSION);
    auto& s = out.session;
    s.setProperty (ids::bpm, reader.bpm, nullptr)
        .setProperty (ids::timeSigNum, beatsPerBar, nullptr)
        .setProperty (ids::timeSigDen, denominator, nullptr);

    // Buses first: the returns (sends A, B... feed them in order) and the groups.
    int busId = 0;

    for (const auto* t : tracks->getChildIterator())
    {
        if (t->getTagName() == "ReturnTrack")
            reader.busOfReturn.push_back (++busId);
        else if (t->getTagName() == "GroupTrack")
            reader.busOfGroup[t->getStringAttribute ("Id")] = ++busId;
    }

    if (const auto* pre = set->getChildByName ("SendsPre"))
        for (const auto* p : pre->getChildIterator())
            reader.sendPre.push_back (p->getStringAttribute ("Value") == "true");

    if (reader.busOfReturn.size() > (size_t) Session::numSends)
        out.report.add ("Live has " + juce::String ((int) reader.busOfReturn.size()) + " returns; taw's tracks have five sends, so sends past E were left out");

    int returnsSeen = 0;

    for (const auto* t : tracks->getChildIterator())
    {
        const auto tag = t->getTagName();

        if (tag != "MidiTrack" && tag != "AudioTrack" && tag != "ReturnTrack" && tag != "GroupTrack")
            continue;

        if (tag == "ReturnTrack")
        {
            // readTrack numbers returns by how many it has made; mark the next one.
            reader.readTrack (s, *t);
            s.getChild (s.getNumChildren() - 1).setProperty (ids::busId, reader.busOfReturn[(size_t) returnsSeen++], nullptr);
            continue;
        }

        reader.readTrack (s, *t);
    }

    // The master: its volume and plugins on the mix, its volume automation on the mix's lane.
    reader.readMixer (s, *master, true, false);
    {
        std::map<juce::String, Target> targets;
        reader.mixerTargets (*master, targets, true);
        const auto devices = devicesOf (*master);
        const auto prefixes = reader.addInserts (s, devices, 0, "mix", "Master");

        for (size_t i = 0; i < devices.size(); ++i)
        {
            if (devices[i].isPlugin)
                reader.pluginTargets (devices[i], prefixes.count ((int) i) > 0 ? prefixes.at ((int) i) : juce::String(), targets);
            else
                reader.liveDeviceTargets (devices[i], targets, "Master");
        }

        reader.addAutomation (s, *master, targets, "Master");
    }

    // Locators as markers, the loop as the loop.
    const auto spb = reader.samplesPerBeat();

    if (const auto* locators = at (set, "Locators/Locators"))
        for (const auto* l : locators->getChildIterator())
        {
            juce::ValueTree m (ids::MARKER);
            m.setProperty (ids::position, (juce::int64) std::llround (number (l, "Time") * spb), nullptr)
                .setProperty (ids::name, value (l, "Name"), nullptr);
            s.appendChild (m, nullptr);
        }

    if (const auto* transport = set->getChildByName ("Transport"))
    {
        const auto loopStart = number (transport, "LoopStart"), loopLength = number (transport, "LoopLength");

        if (loopLength > 0.0)
            s.setProperty (ids::loop, flag (transport, "LoopOn"), nullptr)
                .setProperty (ids::loopStart, (juce::int64) std::llround (loopStart * spb), nullptr)
                .setProperty (ids::loopEnd, (juce::int64) std::llround ((loopStart + loopLength) * spb), nullptr);
    }

    // Duplicate report lines (the same Live device on several tracks says so once each).
    juce::StringArray unique;

    for (const auto& line : out.report)
        unique.addIfNotAlreadyThere (line);

    out.report = unique;
    return out;
}

bool writeAbletonSession (const AbletonSet& set, const juce::File& dir, double sampleRate, juce::String& error)
{
    auto session = Session::create (dir, set.session, sampleRate, error);

    if (session == nullptr)
        return false;

    for (const auto& f : set.files)
    {
        const auto file = dir.getChildFile (f.path);
        file.getParentDirectory().createDirectory();

        if (! file.replaceWithData (f.data.getData(), f.data.getSize()))
        {
            error = "couldn't write " + file.getFullPathName();
            return false;
        }
    }

    for (const auto& c : set.copies)
    {
        const auto file = dir.getChildFile (c.to);
        file.getParentDirectory().createDirectory();

        if (! c.from.copyFileTo (file))
        {
            error = "couldn't copy " + c.from.getFullPathName();
            return false;
        }
    }

    juce::StringArray lines;
    lines.add ("Imported from Ableton Live: what didn't come across");
    lines.add ({});
    lines.addArray (set.report.isEmpty() ? juce::StringArray ("everything came across") : set.report);
    dir.getChildFile ("import-report.txt").replaceWithText (lines.joinIntoString ("\n") + "\n");
    return true;
}
} // namespace taw
