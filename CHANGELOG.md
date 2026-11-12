# Changelog

## Unreleased

- Clips drag with the mouse on the timeline: along their track by the grid's step, selected as
  they go, their automation with them. In the MIDI editor, the clip's strip in the ruler drags
  it too.
- The MIDI editor works on every clip of the track: a press on another clip's note or strip
  makes that the clip being edited, and carries on as the edit. A drag from empty lane selects time.
- Double-click opens plugins and sends: an insert in the routing column (an empty slot picks an
  effect), a send or the output (picks its bus), an instrument track's name (its instrument).
  One click steps onto the slot.
- The MIDI editor copies notes: alt+drag takes copies of the selected notes, and ctrl+c / ctrl+x
  / ctrl+v copy, cut and paste them at the cursor (in another clip too). Off the grid is now
  ctrl+drag for notes (alt copies).
- The phone link: `phone` shows a QR code; a phone on the same network opens its page and sends
  recordings, which land at the cursor on a new track. `phone off` ends it.
- A recording sent from the phone as drums or as notes is made into the part: a beatboxed take
  becomes hits on a sample track for each drum (samples picked from the library), a hummed or
  sung one becomes notes on the instrument chosen on the phone. The recording sits beside the
  part, muted, lined up with it.
- A beatboxed take's drums are told apart by how its own hits group: low hits that fall into two
  sounds are a kick and a tom, middle ones a snare and percussion.
- Parts made from the phone's takes all start on the same bar line (the first one's);
  `phone here` moves that to the cursor's bar.
- The phone's page records on its own, to a click: a bar counted in at the session's tempo (the
  phone's, in a session with nothing in it yet), then the take, which lands with its beat one on
  the bar line, so a part that starts off the beat stays there. The click can carry on through
  the take, for headphones. For this the link is HTTPS with a certificate of taw's own: the phone
  warns once. The `http` address still sends files.
- Sung parts from the phone are kept to a key, so one agrees with the next: found from the first
  one (or chosen on the phone, or `phone key a minor`), and the notes outside it go to the nearest
  one inside. `phone key auto` finds it again; "no key" on the phone leaves the notes as sung.
- "Snap to the grid" on the phone (on unless turned off): off, hits and notes land where they
  were played.
- The phone's page says when the session closed in taw, or when taw can't be reached any more,
  with a button to close the tab; it comes back by itself if taw does.
- `taw --phone` starts a new session with the phone link on (`taw my-song --phone`: in my-song);
  `:phone` on the session screen does the same.

- The sample browser has a guide to its keys and mouse: `?` in it, or the toolbar's `?` button.
- The README explains the sample library: indexing folders, what's measured, and browsing.
- The session screen has a command line: `:` opens it, for what needs no session (`index`,
  `library`, `scan`, `agents`, `hub`, `keycast`, the theme, `dev`, `q`); `:help` shows the
  screen's own keys and commands. `ctrl+q` quits there, as `esc` does.
- The help shows the keys in yellow and what they do in plain text.
- `add` makes several tracks of a kind: `add 3 audio`, `add 3 inst` (empty, ready for
  `5-7 inst <name>`), `add 3 inst juno`, `add 4 sample` (empty, ready for a sample).
- Inserts are `ins` everywhere (help, messages, docs; `fx` still works). Removing an insert or a
  send reads the same: by slot, by name (`2 ins l3 x`, `2 send verb x`) or the only one
  (`2 ins x`); `del` and `remove` work as `x`. `2 ins` alone lists what's in the slots under
  the picker, and the messages name the command that removes or bypasses.
- The track list scrolls by rows: a track with many lanes, or an open kit, shows as far as it
  fits instead of leaving the view empty. The scroll bar is always there: grey with everything
  in view, else an accent thumb as long as the share in view; the Mix row says which tracks and
  rows are in view. The focused track's frame says what's open under it and the key that folds it.
- Plugins that print to stdout (Waves) no longer write over the screen: it goes to
  `taw-stderr.log` with their stderr.
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
