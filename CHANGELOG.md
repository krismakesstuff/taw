# Changelog

## [0.2.0] - unreleased

The first public release: an early proof of concept, for Windows and macOS (Apple Silicon).
(0.1.0 was the first playable milestone, never released.)

- Recording-first timeline driven by the keyboard and a small command language (`2v-6`,
  `add Vox in2`, `1-3m`); the same commands run headless with `taw --exec` for scripts and agents.
- Audio, MIDI and sample tracks; VST3 instruments and effects (Audio Units too on macOS), five
  inserts and five sends per track, buses, latency compensation and a low-latency mode.
- Editing: copy, cut, paste, duplicate, consolidate, undo; markers; loop recording onto playlists;
  `Ctrl+Alt+↑/↓` makes the previous / next playlist the main.
- Clip fades and crossfades (`Ctrl+F` from the selection, `xfade`, `clip fade`), with a short
  declick on every clip edge.
- Automation lanes, written by hand or recorded from MIDI controllers.
- A sample library and browser, with a sound-alike map, in a window of its own: kind tabs, drag
  over the map to audition, auditions in time with the loop, "like this".
- Kits: sample tracks grouped on a bus, each on a pad; played from a pad controller or the
  keyboard's 4x4 (MPC layout), recorded as hits, with choke groups; built in the browser, where
  what you audition goes onto the lit pad.
- Video for scoring to picture, and bouncing with picture (`bv`).
- Bouncing to WAV and MP3.
- Importing Ableton Live sets.
- The audio hub (macOS): routing between apps and sessions through taw's own audio devices; the hub
  panel shows open sessions only (a closed one's routes come back with it).
- `click a` / `count a` for `always`; a blinking beat in the transport, the downbeat lit.
- Keys mode: a shortcut typed there (shift+R and the like) says, in red, how to leave it.
- The session screen: ←→ move between areas; the header names the audio device bounces play
  through, `alt+d` changes it. The hub comes first among devices, and taw's own app devices
  aren't offered to sessions.
- Keycast: a window showing the keys you press, for screen recordings.
- Agents in the open session: `taw --mcp` is an MCP server (Claude Code: `claude mcp add taw --
  taw --mcp`) that reads and edits the session open in a running taw, live: tracks, MIDI notes,
  hits, markers and any command line, each one undo step, never on the track you're on, inside
  your selection, what you just changed, or while playing. Agents never save.
