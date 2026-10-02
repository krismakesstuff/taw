#pragma once

#include <juce_data_structures/juce_data_structures.h>

// Every type and property name in the session state tree. These strings are also the keys
// in session.json, so renaming one is a file-format change.
namespace taw::ids
{
#define TAW_ID(n) inline const juce::Identifier n { #n };

TAW_ID (SESSION)
TAW_ID (TRACK)
TAW_ID (TAKE) // version 1 only; loaded sessions are migrated to CLIP
TAW_ID (CLIP)
TAW_ID (HIT) // sample tracks: one trigger of the track's sample
TAW_ID (MARKER)
TAW_ID (NOTE) // instrument tracks: one MIDI note
TAW_ID (CC)   // instrument tracks: a controller, pitch bend or pressure change
TAW_ID (SEND) // a track's send to a bus: one of five slots, a to e

TAW_ID (version)
TAW_ID (name)
TAW_ID (sampleRate)
TAW_ID (bpm)
TAW_ID (timeSigNum)
TAW_ID (timeSigDen)
TAW_ID (grid)
TAW_ID (latencyComp)
TAW_ID (trackHeight) // rows per track on screen, 1-4
TAW_ID (inputQuantize) // bool: pad hits snap to the grid step as they're recorded
TAW_ID (click)     // bool
TAW_ID (clickWhen) // "always" | "rec"
TAW_ID (clickDb)
TAW_ID (countIn)     // clicks before recording starts (0 = none)
TAW_ID (countInWhen) // "rec" (starting a recording) | "always" (plain playback too)
TAW_ID (mixDb)
TAW_ID (loop) // bool
TAW_ID (loopStart)
TAW_ID (loopEnd)
TAW_ID (bounceFormat) // "wav" | "mp3"
TAW_ID (bounceFolder) // relative to the session folder, or absolute

TAW_ID (kind)         // TRACK: "audio" (default) | "sample" | "instrument" | "bus". Never "type": that key is the node type in JSON.
TAW_ID (busId)        // bus track: what outputs and sends call it (stays put when tracks move or get renamed)
TAW_ID (out)          // TRACK: the busId it plays into; absent or 0 = the mix
TAW_ID (slot)         // SEND: "a" to "e"
TAW_ID (to)           // SEND: the busId it feeds
TAW_ID (db)           // SEND: level in dB
TAW_ID (pre)          // SEND: before the track's fader (default after)
TAW_ID (plugin)       // instrument track: the plugin's identifier (PluginDescription::createIdentifierString)
TAW_ID (pluginName)   // ...and its name, to find it again if the identifier changes
TAW_ID (pluginState)  // ...and the file its state is kept in, relative to the session folder
TAW_ID (sample)       // sample track: the file it plays
TAW_ID (sampleOffset) // ...from here in the file
TAW_ID (sampleLength) // ...for this long (0 = to the end)
TAW_ID (beat)         // HIT: position in beats from the session start (follows tempo)
TAW_ID (vel)          // HIT: velocity 0..1; NOTE: velocity 1..127
TAW_ID (len)          // NOTE: length in beats
TAW_ID (pitch)        // NOTE: MIDI note number (60 = C4)
TAW_ID (ch)           // NOTE, CC: MIDI channel 1-16 (absent = 1)
TAW_ID (num)          // CC: controller 0-127, 128 = pitch bend, 129 = channel pressure
TAW_ID (val)          // CC: value (0-127; pitch bend 0-16383, 8192 = centre)
TAW_ID (gridDiv)      // SESSION: fine grid step as a note value (4 = quarter, 16 = sixteenth)
TAW_ID (input)
TAW_ID (armed)
TAW_ID (mute)
TAW_ID (solo)
TAW_ID (monitor)
TAW_ID (gainDb)

TAW_ID (file)
TAW_ID (start)  // CLIP: timeline position, samples
TAW_ID (offset) // CLIP: where in the file it starts playing, samples
TAW_ID (length)
TAW_ID (position)

#undef TAW_ID
} // namespace taw::ids
