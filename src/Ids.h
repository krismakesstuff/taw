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
TAW_ID (MIDICLIP) // instrument tracks: a window onto the NOTEs and CCs inside it
TAW_ID (HITCLIP)  // sample tracks: the same, onto the HITs inside it (version 3 kept hits on the track)
TAW_ID (NOTE) // in a MIDICLIP: one MIDI note (version 2 kept them on the track)
TAW_ID (CC)   // in a MIDICLIP: a controller, pitch bend or pressure change
TAW_ID (SEND) // a track's send to a bus: one of five slots, a to e
TAW_ID (INSERT) // an effect plugin in one of a track's five insert slots, a to e
TAW_ID (AUTO)   // an automation lane, on a TRACK (or on the SESSION: the mix's volume); its POINTs
TAW_ID (POINT)  // in an AUTO: a value (val) at a beat on the timeline
TAW_ID (PLAYLIST) // on a TRACK: another version of its clips (name, solo); the track's own clips are the main playlist

TAW_ID (version)
TAW_ID (name)
TAW_ID (sampleRate)
TAW_ID (bpm)
TAW_ID (timeSigNum)
TAW_ID (timeSigDen)
TAW_ID (grid)
TAW_ID (latencyComp)
TAW_ID (lowLatencyMax) // low latency mode: plugins with more latency than this (samples) sit out
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
TAW_ID (bounceReveal) // open the file explorer on a finished bounce
TAW_ID (videoFile)    // the picture: relative to the session folder, or absolute; absent = no video
TAW_ID (videoStart)   // ...where its first frame is on the timeline, samples (negative: it starts before the session)
TAW_ID (videoOffset)  // ...frames it's shown late (negative: early), for a screen that lags
TAW_ID (videoShown)   // ...its window is open
TAW_ID (videoWindow)  // ...where the window was (JUCE's window state string)

TAW_ID (kind)         // TRACK: "audio" (default) | "sample" | "instrument" | "bus". Never "type": that key is the node type in JSON.
TAW_ID (busId)        // bus track: what outputs and sends call it (stays put when tracks move or get renamed)
TAW_ID (out)          // TRACK: the busId it plays into; absent or 0 = the mix
TAW_ID (slot)         // SEND: "a" to "e"
TAW_ID (to)           // SEND: the busId it feeds
TAW_ID (db)           // SEND: level in dB
TAW_ID (pre)          // SEND: before the track's fader (default after)
TAW_ID (bypass)       // INSERT: skipped (the plugin stays loaded)
TAW_ID (plugin)       // instrument track or INSERT: the plugin's identifier (PluginDescription::createIdentifierString)
TAW_ID (pluginName)   // ...and its name, to find it again if the identifier changes
TAW_ID (pluginState)  // ...and the file its state is kept in, relative to the session folder
TAW_ID (sample)       // sample track: the file it plays
TAW_ID (sampleOffset) // ...from here in the file
TAW_ID (sampleLength) // ...for this long (0 = to the end)
TAW_ID (beat)         // MIDICLIP, HITCLIP: position in beats from the session start (follows tempo); NOTE, CC, HIT: in their clip's content
TAW_ID (vel)          // HIT: velocity 0..1; NOTE: velocity 1..127
TAW_ID (len)          // NOTE, MIDICLIP, HITCLIP: length in beats
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
TAW_ID (pan)      // TRACK: -1 (left) to 1 (right); absent = centre
TAW_ID (channels) // TRACK: 1 (mono) or 2 (stereo); absent = from what it plays (a stereo clip or sample: stereo)

TAW_ID (param) // AUTO: what it controls (AutomationTarget: "vol", "pan", "mute", "send a", "inst/<id>", "fx a/<id>")
TAW_ID (lanes)      // TRACK: its automation lanes are open under it
TAW_ID (laneParams) // TRACK: what its lanes show, in order (params, one per line); absent = its AUTO lanes
TAW_ID (playlist)   // TRACK: the main playlist's name; absent = the track's name
TAW_ID (playlists)  // TRACK: its playlists are open under it as lanes (never with its automation lanes)

TAW_ID (file)
TAW_ID (start)  // CLIP: timeline position, samples
TAW_ID (offset) // CLIP: where in the file it starts playing, samples; MIDICLIP, HITCLIP: where in its content, beats
TAW_ID (length)
TAW_ID (position)
TAW_ID (fadeIn)       // CLIP: fade in from its start, samples; over another clip it's a crossfade (absent = none)
TAW_ID (fadeOut)      // CLIP: fade out to its end, the same
TAW_ID (fadeInShape)  // ...its curve: "eqp" (equal power, default) | "lin" | "s" | "exp"
TAW_ID (fadeOutShape)
TAW_ID (declickMs)    // SESSION: the fade every audio clip edge gets when it has none (absent = 2, 0 = off)

#undef TAW_ID
} // namespace taw::ids
