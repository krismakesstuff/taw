# Changelog

## Unreleased

- The sample browser has a guide to its keys and mouse: `?` in it, or the toolbar's `?` button.
- The README explains the sample library: indexing folders, what's measured, and browsing.
- The session screen has a command line: `:` opens it, for what needs no session (`index`,
  `library`, `scan`, `agents`, `hub`, `keycast`, the theme, `dev`, `q`); `:help` shows the
  screen's own keys and commands. `ctrl+q` quits there, as `esc` does.
- The help shows the keys in yellow and what they do in plain text.
- macOS: once taw handed the keyboard back to the terminal, clicks in other apps could read as
  clicks in a plugin window, so every app you clicked lost the keyboard to the terminal until
  the session closed.

## [0.2.0] - 2026-11-01

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
  hits, markers, automation (volume, pan, mute, sends, plugin parameters) and any command line,
  each one undo step. Never while playing or on what you just changed, and not on the track
  you're on or inside your selection until you've been away from the keys for 10 s. What an agent
  changes flashes violet; the title line shows who's connected and what their last edit did.
  Agents can list, make and open sessions (over an open one, taw asks you first). Agents never
  save.
