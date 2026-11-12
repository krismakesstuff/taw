# taw — design

taw is a terminal audio workstation for engineers and producers. It is keyboard-first,
recording-first, and built so an agent can drive it through the same command language a
human types.

This document records the decisions made so far. Sections marked **(open)** are still
being worked out.

## Principles

1. **Fewest keystrokes for complex actions.** A GUI lets you do complex things by pointing,
   but you have to know where things live. taw replaces spatial knowledge with a small,
   composable grammar.
2. **Every character on screen is intentional.** Low resolution is fine; noise is not.
3. **One command language.** Every keystroke maps to a named command. The TUI, scripts and
   agents are all clients of that language. Nothing is TUI-only.
4. **Hackable by default.** Keymaps, templates and sessions are plain files. Pros have
   opinions about workflow; taw lets them encode them.
5. **Non-destructive recording.** Every pass is a take. Nothing is overwritten.

## Stack

- **C++20 + JUCE 9** used as a library (devices, audio formats, ValueTree, later plugin
  hosting). The engine itself is written from scratch; Tracktion Engine is a reference,
  not a dependency.
- **ASIO** via the Steinberg ASIO SDK 2.3.4, used under its GPLv3 option. WASAPI and
  DirectSound are also available.
- **License: AGPLv3** (required by JUCE's open-source licence; compatible with the ASIO
  SDK's GPLv3). Releases may be sold as paid downloads of the same AGPL code; there's no closed
  edition. Outside pull requests aren't taken yet (issues only), so there's no CLA.
- **Target terminal: Windows `cmd` (conhost)** first. Rendering uses VT sequences and only
  glyphs that Consolas has (block elements, box drawing). Input uses the Win32 console API,
  which reports modifiers reliably (Shift/Ctrl+arrows).
- **Cross-platform:** Windows first, macOS in progress (Apple Silicon builds and ships), Linux
  next. JUCE already covers audio devices (Core Audio, ALSA/JACK) and file formats, and the
  Windows-only parts are kept behind small seams so each can get a POSIX counterpart:
  - `Console`: Win32 console input → a termios backend that parses VT key sequences (with an
    extended keyboard protocol where the terminal has one, so modifiers stay reliable).
  - `Safety`: minidumps, console control handler, process checks → signal handlers, core
    dumps / crash logs, `kill(pid, 0)`.
  - Windows Media Foundation (MP3 encoding, m4a/aac/wma import) → a portable encoder/decoder.
  - Clipboard, the UTF-16 command line, and Mac/SMB private-use path names.
  The command language, sessions and engine are platform-neutral and stay that way.

## Threads

Modelled on Tracktion Engine.

| Thread | Owns | Rules |
|---|---|---|
| Message | Session state tree, command execution, undo, graph building | Only thread that mutates the tree |
| UI | Rendering | Reads the tree and engine atomics; never blocks audio. *In the POC this runs on the message thread; it moves out once rendering gets heavy.* |
| Audio (device callback) | The compiled playback graph | No allocation, no locks, no file I/O |
| Disk writer | Take files | Fed by lock-free FIFOs (`AudioFormatWriter::ThreadedWriter`) |
| MIDI input | Device callbacks | One lock-free queue per port into the audio thread |
| Video | Decoding and scaling the picture | Reads the engine's playhead atomics and a few the message thread sets each frame; hands frames to the window under a lock |

The message thread compiles the tree into an immutable **playback graph** and hands it to
the audio thread through a lock-free queue. Retired graphs come back the same way and are
freed on the message thread. The audio thread never reads the tree.

Meters are atomics written by the audio thread and read by the UI.

## Session on disk

```
my-song/
  session.json     the state tree, human- and agent-readable
  audio/           take files (WAV, 32-bit float)
```

`session.json` is a direct serialisation of the state tree:

```
SESSION { name, sampleRate, bpm, timeSigNum, timeSigDen, grid, latencyComp, click, clickWhen, clickDb, mixDb, loop, loopStart, loopEnd }
  TRACK { uid, name, input, armed, mute, solo, monitor, gainDb }   # uid: "t7", the track's for good
    CLIP { file, start, offset, length }  # plays file[offset, offset+length) from timeline start
  MARKER { position, name }
```

All positions are stored in samples at the session sample rate.

**Clips.** A track holds any number of clips, each a window into an audio file. Many clips
can point at one file, which is what makes copy/paste free. Where clips overlap, the later one
(in session order) plays: "later wins" at playback time, so nothing is trimmed and removing a
clip (`x`) uncovers what was beneath it. A track is stereo if any of its clips is; mono clips
on it play to both sides. Recording and import each add a clip; a take recorded from zero
that latency compensation would push before zero starts at zero and skips into its file.
Version 1 sessions (one active `TAKE` per track) are migrated on load: the active take becomes
a clip, older takes stay in the file as history.

`taw --cliptest [session]` checks the clip rules and shows how a session loads.

## Timeline

- Time is always shown (mm:ss.ms). Bars/beats are shown when the grid is on.
- The grid is **on by default** and can be toggled off for free-time recording. With the
  grid off, navigation moves in seconds instead of bars.

## Input model

One mode, stopped or rolling: keystrokes build a command line and `Enter` runs it, so `2m`,
`3s`, `l`, `c` work during playback as they do stopped (as in any DAW). Navigation, volume,
Space and Ctrl keys act immediately and never touch the command line. While rolling, a
PLAYING / RECORDING label sits in front of the prompt. Changing the audio device needs a stop.

**Device rates.** The engine and its plugins run at the device's rate. A Bluetooth headset
whose own mic is open drops to 16 kHz mono, and some instruments are silent there (Roland's
EARTH is; `taw --plugintest <file> 16000` shows it). In `dev`, ←→ gives an output another
input (the Mac's mic, or none), which keeps the headset in stereo at 44.1/48 kHz. A device
that's its own input and runs below 44.1 kHz gets a warning saying so. Planned: running the
engine at the session's rate and converting to the device's, so any device works.

### Navigation keys (idle and rolling)

Where the keys go is framed, as on the session screen, so it reads at a glance with your
hands and mind on the music: the focused track gets a heavy frame (a row above and below it,
its automation and playlist lanes inside) with its number and name in the top edge, the Mix
the same, and the markers, bar or video row heavy sides. The frame turns record-red while
the focused track is armed, and says "armed" or "● recording" after its name. Clicking a
track's frame is clicking the track.

| Key | Grid on | Grid off |
|---|---|---|
| `←` / `→` | to the next grid line shown, so it follows the zoom (a bar, a beat, an 8th...) | 1 second |
| `Ctrl+←` / `Ctrl+→` | 4 of those | 4 seconds |
| `Alt+←` / `Alt+→` | 1 step (a beat; a 16th after `grid 16`) | 0.1 second |
| `Ctrl+Alt+←` / `Ctrl+Alt+→` | 4 steps | 0.4 second |
| `Enter` (empty command line) | back to where play started; again for the session start (rolling: keeps playing from there) | same |
| `Home` / `End` | session start / end of last take | same |
| `↑` / `↓` | focus previous / next track; below the last track is the Mix, above the first the bar row, then the markers row (selections there cover every track). On the markers row `←`/`→` (and Tab) hop marker to marker, picking it and moving the cursor; Enter renames it, Delete/Backspace removes it, Shift stretches the selection marker to marker; `↓` goes back to normal arrows | same |
| `[` / `]` | volume −1 / +1 dB on the selection, focused track, or Mix (also while rolling) | same |
| `{` / `}` or `Ctrl+[` / `Ctrl+]` | Mix volume −1 / +1 dB, from anywhere (`{ }` because macOS Terminal sends `Ctrl+[` as Escape) | same |
| `r` / `t` (empty command line; also `-` / `+`) | zoom out / in (Pro Tools keys) | same |
| `Ctrl+↑` / `Ctrl+↓` | first / last track; with Shift, selects every track from where you were to there | same |
| `` ` `` (empty command line) | to the markers row (`~` adds a marker); `` ` `` again returns to the track | same |
| `f` (empty command line) | fill: the focused track takes every track row (a piano roll on instrument tracks); `f` again shows all tracks | same |
| `q` / `w` (empty command line) | cursor to the selection's start / end, centred in the view; the selection stays (select, `snap`, `q`, play) | same |
| `Tab` / `Shift+Tab` | cursor to the next clip start or end, or the next hit on sample tracks, on the selected or focused tracks / extend the selection to it | same |
| `Shift+R` `S` `M` `I` (empty command line) | arm / solo / mute / input-monitor the selected or focused tracks | same |
| `Shift+T` (empty command line) | the track menu for the selected or focused tracks: rename, input, output, add an effect, plugin window, mono/stereo, convert, delete. Each item has a letter and shows the command it stands for | same |
| `Alt+C` | clear the latched clip lights (inputs, tracks, the mix); Esc doesn't, it's busy with the line, selections and stopping | same |
| `.` / `>` (empty command line, a sample track armed) | the pad key: plays every armed sample track now (`>` = accent); while recording, writes a hit on each where you heard it (output latency taken off). Held down it's one hit, not a roll: the samples are one-shots, so the key's auto-repeat is ignored (Windows; terminals elsewhere don't report releases). Recording-first: patterns are played in; `snap` / `snap 50` quantize afterwards, `snap input` snaps while recording (off by default) | same |
| `Space` (empty command line) | play / stop | same |

**During playback the arrows never move the playhead.** They move the cursor (drawn apart from
the playhead) and, with Shift, the selection, so you can select, copy and paste while the music
keeps going. Pastes land at the cursor. Stopping leaves the cursor where you put it (or returns
to where play started if you didn't move it). To move the playhead itself: `@17`, or Enter to
go back to where play started. The view follows the playhead, pausing for 3 s after a cursor
move so you can see where you are.

**Shift is reserved for selection.** It never changes how far a key moves. Shift plus any
horizontal move extends a time selection; Shift+`↑`/`↓` extends a track selection (never the
Mix). The time selection has its own two ends, separate from the cursor and playhead, so it
can be made while playing (e.g. mark the next loop and press `l` without stopping). A plain
move clears the selection; `Esc` clears it too. A time selection with no track
selection spans all tracks. Selections are the basis for editing later (cut, loop, punch
ranges).

### Mouse

The keyboard does everything; the mouse is a shortcut. Clicks arrive by cell: the Windows
console's mouse events, and on macOS the terminal's SGR mouse reports (a double-click is the same
cell clicked again within half a second). To select text in the terminal while taw runs, turn
off View > Allow Mouse Reporting (Terminal.app).

- Click a track's lane: that track gets the focus and the cursor goes there (as in Pro Tools).
  Shift-click stretches the time and track selections to it.
- Double-click a clip: selects it and opens it in its editor, MIDI or audio (shift+E, or
  `edit`, from the keyboard).
- Drag a clip (audio, MIDI or hits; the one on top): it moves along its track, onto the grid
  lines on screen when the grid is on, as one undo step. Its automation goes with it when the
  button is let go. Not while recording.
- Drag from empty lane: selects time.
- `taw --exec` takes `:click X Y`, `:dblclick X Y` and `:drag X Y X2` (cells of its 160×44
  screen).

### The editors

**Shift+E** (or `edit`, `9 edit`) opens the focused track in a zoomed editor window: the MIDI
editor on an instrument track, the audio editor on an audio track, on its clip at the cursor or
else the nearest one. Shift+E again, or the close button, closes it; one is open at a time.
`edit new` makes an empty MIDI clip to draw into (over the time selection, else the bar the
cursor is in). Both sit over the terminal like the plugin windows, keep the keyboard while
they're in front (Esc hands it back) and pass on the keys they don't use.

### The audio editor

The focused audio track's clips, zoomable from the whole take down to single samples (a line
through them, then dots). The time selection, cursor and playhead are taw's, so the editor is
for placing them precisely and taw's keys do the editing.

- **Drag across the waveform:** taw's time selection on this track, snapped to the editor's
  grid (bar to 1/32, or off: to the sample; alt: free) and, with "zero crossings" on, to the
  nearest zero crossing. Then Delete, ctrl+x / c / v, ctrl+d, `l`, `b` act on exactly that.
  A click puts the cursor there; the ruler too. Double-click a clip: select all of it.
- **A clip's edges:** drag to trim (it's a window into its file: never past the file, nothing
  lost). **Its name bar:** drag to move it.
- **Height:** starts scaled to the track's loudest peak (a quiet take isn't a flat line); alt+wheel
  changes it. Ctrl+wheel (or `r` `t` / `+` `-`, around the cursor) zooms; the wheel scrolls.

### The MIDI editor

One MIDI clip, for fine work with the mouse.

- **Shared with taw:** it edits the session itself, one undo step per gesture, so ctrl+z (from
  either side) and the timeline follow. The cursor and playhead are taw's: clicking the ruler
  moves the cursor; the view follows the playhead while playing.
- **Notes:** drag to move (snapped to the editor's grid; ctrl: free), alt+drag to copy what's
  selected (the copies move, the notes stay), drag an edge to change the
  length, double-click empty space to draw one (as long and loud as the last one touched),
  right-click or Delete to remove. Drag on empty space selects (shift adds). A note moved or
  stretched past the clip takes the clip with it. Other clips' notes on the track show as
  outlines; double-click one to open its clip. Hidden notes (trimmed off) show faint.
- **Ruler:** bars and beats; the track's clips as strips, this one with handles: drag its edges
  to trim or extend it (notes are only hidden).
- **Keyboard down the side:** click (and drag) to play notes. Clicking or dragging a note
  plays it too, through the track's instrument, armed or not (the engine's audition queue).
- **Lane:** velocity (drag a stalk; across several to set each; a selected one moves every
  selected velocity together), or a controller: mod wheel, breath, volume, pan, expression,
  sustain, pitch bend, pressure. Click to add a point, drag to move, right-click to remove; a
  controller holds its value until the next point.
- **Toolbar:** the grid (bar to 1/32, triplets, off; it starts at the session's `grid` step) and
  quantize (note starts in the selection, or the whole clip, to the grid; also `q`).
- **Keys** while it's in front: Delete, ↑↓ transpose (shift: an octave), ←→ move by the grid
  (ctrl: a bar), ctrl+A select all, ctrl+D duplicate after the selection, ctrl+C / ctrl+X copy / cut the
  selected notes and ctrl+V pastes them at taw's cursor (into this clip, or another one opened
  since; the cursor goes to their end, so pastes follow on; with no notes selected ctrl+C is
  taw's, as before), `q` quantize, `r` `t`
  (or `+` `-`) zoom the window around taw's cursor (the session's zoom stays), Esc clears the selection, then hands the keys back to taw. Everything else goes to taw:
  space plays, ctrl+z undoes, ctrl+s saves, shift+E closes it.
- **Mouse wheel:** scrolls pitch; shift: time; ctrl: zoom time around the mouse; alt: row height.

### Command grammar

```
[selector] verb [args]
```

Spaces are optional: `2i2` = `2 i 2`, `1-3r`, `2v-6`, `mixv-3`, `bpm92`. A verb letter always
separates the numbers, so nothing is ambiguous. Glued letters only count as an argument for
names and click modes (`2nVox`, `clickrec`), so a typo like `mutr` is rejected rather than read
as `m` + `utr`. `taw --parse "2i2"` shows how a line parses without running it.

Selectors: `1`, `1-3`, `1,4`, `*` (all), `mix`, and a kit's pads `2.3`, `2.1-4` (see Kits). With no selector, commands act on the
shift-selected tracks, else the focused track (or the Mix, for `v`). `r` with no selector
records the selected tracks, else whatever is armed.

| Command | Meaning |
|---|---|
| `1 r` / `1-3 r` | arm (record-enable + input monitoring); a group toggles together |
| `Ctrl+Space` / `rec` | record the armed tracks from the cursor (while playing: punch in/out) |
| `1 rec` | arm exactly track 1 and record (one command, for scripts and agents) |
| `2 a` | same as `2 r` |
| `2 m` / `2 s` | toggle mute / solo |
| `2 mon` | armed track keeps its input during playback too (audition against the mix) |
| `3 i 4` | track 3 takes hardware input 4 |
| `2 v -6` | set volume to −6 dB |
| `2 n Vox` | rename |
| `Alt+↑` / `Alt+↓` | move the selected tracks (or the focused one) up / down a place, swapping with the neighbour; ctrl+z puts it back |
| `2 remove` / `2-4 remove` | delete tracks (`rm` too); a bus's feeds go back to the mix and sends to it go; ctrl+z brings it all back |
| `2 dup track` / `2 dup track empty` | duplicate tracks (`u` in the track menu, which asks which): each copy goes right under its original as `Vox.dup1`, with its plugins as they're set now (in state files of its own), inserts, sends, output, volume, pan, mute, solo and automation. Its main playlist is a copy of the original's clips, or empty; the other playlists stay with the original. The copy isn't armed. A copy of a bus is a new bus (what feeds the original stays there). One undo step |
| `@17` / `@17.3` / `@1:30` | move cursor to bar 17 / bar 17 beat 3 / 1 min 30 s |
| `mix v -3` | set the mix volume |
| `add` / `add 3` / `add Vox` / `add 2 Gtr` | new tracks; each takes the lowest unused input |
| `add vox1 in2` / `add 2 Gtr in3` | new tracks on given inputs (Gtr 1 on 3, Gtr 2 on 4) |
| `add 3 audio` / `add 3 inst` / `add 4 sample` | several tracks of a kind at once: audio (as `add 3`), instrument tracks with no plugin yet ("no instrument yet"; `5-7 inst juno` loads one on all three), empty sample tracks (`7 sample <file>` gives one its sample) |
| `add 3 inst juno` | three tracks playing Juno, named Juno 1-3; only the first is armed, so the controller plays one |
| `b` / `bounce` | offline-print the mix bus over the time selection (selection required) |
| `b mp3` / `b wav D:\mixes` / `b mp3 "rough 3"` | format, folder (remembered per session), file name |
| `l` / `loop` | loop the time selection; with no selection, toggle the loop |
| `loop on` / `loop off` | |
| `bpm 92` | set tempo; with `bpm ` typed, `t` taps it (the average of the last 8 taps, a 2 s pause starts over) and enter sets it |
| `grid` | toggle grid |
| `click` | toggle the click (metronome) |
| `click rec` / `click always` (`click a`) | click only while recording / during playback too |
| `click -12` | click level in dB |
| `lowlat` / `lowlat 256` | low latency mode on/off / its limit in samples (plugins over it sit out) |
| `count 4` / `count off` | count-in: clicks before recording starts (default 4; also in `project`) |
| `count always` (`count a`) / `count rec` | count in on plain playback too / only when recording starts (default) |
| `height 3` | rows per track, 1-4 (default 2); saved with the session |
| `dark` / `light` / `grey` | colour theme, for every session (saved in `settings.json`) |
| `dev` | choose audio device; ←→ on a row picks its input (CoreAudio: another device, or none) |
| `panel` | open the ASIO driver control panel |
| `save` (or `Ctrl+S`), `close` (or `Ctrl+W`), `exit` (or `Ctrl+Q`) | save; go back to the sessions; quit (`q` alone is a view key). With unsaved changes, closing and quitting ask: `s` save (and bounce last-saved), `o` save only, `d` don't save, `esc` stay |

### Rolling keys

| Key | Action |
|---|---|
| `Space` / `Esc` (empty command line) | stop (cursor returns to where play started) |
| `Enter` (empty command line) | back to where play started, still playing |
| `Ctrl+Space` | punch: toggle recording on armed tracks without stopping (in, out, in again…) |
| `~` (shift+`` ` ``, empty command line) | drop a marker at the playhead (stopped: at the cursor) |
| `Ctrl+C` / `X` / `D`, `Delete` | edit the selection, as when stopped |
| `←` `→` (any modifier) | move the cursor / selection, not the playhead |
| `Ctrl+V` | paste at the cursor |
| `[` / `]` | track volume; `{` `}` or with `Ctrl`, the mix |
| anything else | the command line: `2m`, `3s`, `l` (loop; ignored while recording), `c` (click) |

### Planned (not in the POC)

- Ranges: `@17-25 L` (loop), `2 @17-21 r` (auto-punch with pre-roll), `1-3 @9-13 x` (clear).
- Pre-roll (count-in is done: the transport holds while the click counts, then rolls).
- Takes: keep/discard, take stack, comping.
- Retroactive capture (`R`: keep what was just played while not recording).
- Queries: `1 ?`, `? inputs`.
- User keymaps and macros (config files).
- Plugins out of process; CLAP. Fades and clip gain in the audio editor.
- A headless engine for agents (the MCP front end to a running taw is done: "Agents in the open
  session").

## Recording

Auto input, the pro default. `1 r` arms track 1: record-enable plus input monitoring. An armed
track monitors its input while stopped (set levels) and while recording; during plain
playback (`Space`) it plays its clips like any other track. `Ctrl+Space` records the armed
tracks from the cursor; while playing it punches in and out. `2 mon` keeps an armed track's
input in your ears during playback too, to audition a part against the mix (its clips are
silent meanwhile). A track monitors its input or plays its lane, never both. Sample tracks
can't be armed.

**Count-in.** Recording from a stop counts in first: 4 clicks by default (`count`, or the
project panel). The transport holds at the cursor while the click counts, then rolls and
records from exactly there. Armed tracks monitor their input meanwhile, and the transport bar
shows `COUNT 3`. The clicks fall on the grid beats just before the cursor, so from mid-beat
they still lead in on time. The count-in clicks even with the click off, at the click level.
Punching in while playing never counts in. `count always` counts in on plain playback too.

**Punching.** While playing, `Ctrl+Space` punches in. Pressed again, it punches out:
recording stops and the take is saved, but playback carries on and the armed tracks go back
to playing their clips. Press it again to punch in again; each pass is its own take.

## Undo

Every edit is undoable: clips, hits, tracks, names, inputs, volume, mute/solo/arm, markers,
tempo, grid, click, loop. `Ctrl+Z` / `Ctrl+Y`, or `undo` / `redo`. Each command line is one
step named after itself ("undid 2v-6"); a recorded take and a whole import are one step each;
`[` `]` presses on the same target less than two seconds apart merge into one step. Not
undoable on purpose: bounce settings, cursor and
selection. Built on JUCE's `UndoManager` over the state tree; undo is refused while recording.

## Sample tracks

One sample per track; each track reads as its own step-sequencer row. A kit groups them (below).

```
TRACK { type: "sample", sample, sampleOffset, sampleLength, … }
  HITCLIP { beat, offset, len }  # like a MIDI clip: a window onto its content, in beats
    HIT { beat, vel }            # beats in the clip's content, so patterns follow the tempo
```

- Hits live in clips, so a loop copies, moves, repeats and trims as one block. A hit clip works
  like a MIDI clip: trimming hides hits without losing them, and overlapping hit clips both play.
- Hits placed with `Alt+/` go into the clip under the cursor; outside one, the clip ending
  earlier in that bar grows to the bar's end, else a new one-bar clip is made, so entering steps
  bar after bar builds one clip. Hits recorded with the pad key go into one clip per record pass,
  as notes do. `pat` writes one clip over its range.
- Version 3 sessions kept hits on the track; on load they go into one clip from the bar of the
  first hit to the end of the bar of the last.

- Each hit plays the whole sample once at its velocity; overlapping hits ring out, unless a
  kit's choke group cuts them (below). Rendering depends only on position, like clips, so loops,
  seeks and bounces agree.
- `sample <file|folder>` / `add sample <file>`: new sample track(s). `3 sample <file>` gives
  track 3 a new sample and keeps its hits (swap the sound, keep the groove).
- `3 sample` converts an audio track: its latest clip (as trimmed) becomes the sample and every
  clip start becomes a hit, so it sounds the same. `3 audio` converts back. Conversion is
  lossless both ways: each type keeps the other's data.
- Dropped files under 2 s become sample tracks automatically; longer ones stay audio.
- `pat` writes steps over the time selection (repeating), or from the cursor for a bar:
  `X` accent, `x` normal, `o` ghost, `.`/`-` rest; spaces and `|` ignored. Steps are 16ths
  unless `pat /8 …` etc. A pattern is a new clip over its range, replacing what was there. `~58` (or `swing58`)
  anywhere in it swings: every second step lands late, 50 straight to 75 (66 is a triplet feel),
  the MPC way, so `pat /16 ~58 …` swings the 16ths.
- `grid 16` (or 4, 8, 32) sets the fine step: `Alt+←/→` move one step (`Ctrl+Alt` four) and the view zooms
  to one column per step. `Alt+/` toggles a hit at the cursor on a focused sample track (at the
  playhead while playing).
- The lane shows each step: a hit's velocity as a shade, an empty step as a faint dot. A clip's
  steps are shaded (neighbouring clips alternate) and outlined top and bottom.
- Sample tracks can't be recorded onto.

### Kits

A kit groups sample tracks for finger drumming and quick kit building (`Kits.cpp`). Still one
sample per track: each member keeps its own hits, mute, sends and inserts.

```
TRACK { kind: "bus", kit: true, folded?, … }      # the kit: a bus its members play into
TRACK { kind: "sample", out: <kit's busId>, note, choke?, sampleFrom, … }   # a member, a pad
```

| Command | Does |
|---|---|
| `kit` / `add kit [name]` | a new kit ("Kit 1"), focused; `kit` on a kit (or a member) lists its pads |
| `pad 3 <file>` | a sample onto pad 3 of the kit here (1-16; pad 1 is note 36, what drum pads send): the member on it gets the new sound and keeps its hits, or a new member is made under the kit. `pad 3` alone lights pad 3 for the browser |
| `3 kit drums` / `2.3 kit off` | sample track 3 into kit Drums (on the next free pad unless it has a note) / pad 2.3 out of it, a track again |
| `2.3 note 38` / `2.3 note d2` | which note plays pad 3 of kit 2 (names with middle C as C4); the pad already there swaps |
| `2.3 choke 1` / `2.1-4 choke off` | its choke group, 1-16 |
| `shift+F` / `kit fold` | fold the kit's members away under it (or open it) |

- **Playing:** a kit armed (`shift+R` on it: it shows `R` like a track) plays from the MIDI
  controller: each note-on plays the members on that note at its velocity, on the audio thread
  (`TrackPlay::padNote`, set only while the kit is armed), so a pad controller is as quick as the
  pad key. Several kits armed layer, as instruments do; an armed instrument hears the same notes.
- **Keys mode on a kit** (`ctrl+k` while on a kit or a member, which arms the kit; or with a kit
  armed and no instrument): the keyboard is a 4×4 pad grid laid out like an MPC's, pad 1 bottom
  left: `z x c v` pads 1-4, `a s d f` 5-8, `q w e r` 9-12, `1 2 3 4` 13-16. Shift is the accent
  (full velocity). Held, a key is one hit. Its notes go in as a controller's would
  (`LiveKeys::setPads`).
- **Recording:** `ctrl+space` with a kit armed records what you play: each note-on writes a hit
  onto the member it played, where you heard it (`recordKitHit`, from the recorded MIDI, input
  quantize as for the pad key), one clip per member per pass, so loop recording splits passes as
  for pad hits. `.` still plays every armed sample track.
- **Choke:** a hit on a member cuts off the ringing hits of other members in its group (the
  closed hat stops the open one) with a 5 ms fade. On the timeline it's decided when the graph is
  built (`chokeHits`: each hit's `cut` is the next hit on another track of its group; one at the
  same moment doesn't cut), so it depends only on positions and bounces agree; live, a pad press
  fades out the group's other voices. Groups are numbered per kit, so two kits never choke each
  other. Not yet: a live pad choking the timeline's hits, or the other way round.
- **Pads are lanes, not tracks.** Members aren't numbered as tracks: the tracks after a kit
  carry on from its number (Mic 1, Kit 2, Gtr 3), and a pad is its kit and pad, `2.3`, on screen
  (its frame says "2.3 808SD (pad 3 of Kit 1)"), in messages and as a selector: `2.3 m`,
  `2.3 v -6`, `2.1-4 choke 1` (a range takes the pads there are). `*` still means every track,
  pads too. `App::numbering` maps both ways (`TrackNumbering`); inside, a pad is still a track
  index. With no selector, commands act on the pad lane you're on, as on any track.
- **Screen:** the kit row has `kit` as its label and `▾`/`▸` before its name, and always shows
  every pad's hits together like a drum part in a MIDI clip: the pads in use one per half row
  (higher notes higher), each hit a mark in its column, over one clip shade per stretch the
  pads' clips cover. Pad lanes show `│` instead of a number and their pad (`pad1`, or `n60`
  past 16) as their label. Folded, the pad lanes take no rows; ↑↓ pass them, and anything that
  focuses one (a pad load) opens the kit.
- `taw --livetest` checks a note playing its pad and choking another; `--cliptest` the timeline's
  choke.

**Positions between samples.** At tempos like 99 bpm a beat is 26727.27 samples, so grid lines
fall between samples. Snapping, pattern ranges and the position readout treat anything within
half a sample of a grid line as on it.

## Instrument tracks

A VST3 instrument per track (or an Audio Unit on macOS), played from a MIDI controller and
recorded as notes. A plugin installed as both is listed once, as the VST3.

```
TRACK { kind: "instrument", plugin, pluginName, pluginState, … }
  MIDICLIP { beat, len, offset }          # on the timeline at beat, for len beats, showing its content from offset
    NOTE { beat, len, pitch, vel, ch? }   # content beats, so parts follow the tempo; vel 1-127; ch 1-16 (absent = 1)
    CC   { beat, num, val, ch? }          # num 0-127 controllers, 128 pitch bend (0-16383), 129 pressure
```

- **MIDI clips.** Notes live in clips, as audio lives in clips. A clip is a window onto its
  content: `[offset, offset + len)`, placed at `beat`, so trimming hides notes without losing
  them. A note belongs to the clip it starts in and is cut off at the clip's end. Overlapping
  MIDI clips both play (recording over a part adds to it). Each recording pass makes a new clip,
  from where it began to where it stopped (a pass with nothing played makes none); its content
  beats are the timeline's. Version 2 sessions (notes on the track) get one clip per track around
  their notes, out to whole bars.

- `inst` opens the instrument picker (type to filter, enter loads); `inst vital` loads the best
  name match. On an instrument track (or selected ones) either swaps its plugin, keeping its
  notes, inserts, sends and mixer; anywhere else it makes a new instrument track, armed, so the
  controller plays it at once. `add inst vital` always makes a new track; `3 inst vital` puts
  it on track 3 (an empty or instrument track; the notes stay).
- `scan` looks for new or changed plugins in the VST3 folders, and on macOS among the Audio
  Units (opening the picker the first time starts it). Finding out what a plugin is means loading it, and some crash or hang when
  loaded cold, so each file is looked at in a child process (`taw --scan-plugin`, four at a
  time, 60 s each): one that crashes or hangs only takes its child down and is skipped from
  then on; `scan retry` tries those again. The list is cached in `plugins.xml` in the config
  folder, saved after every file.
- `|` (shift+`\`) opens the plugin under the cursor in its own window (JUCE's generic sliders
  if it has none), kept above the terminal; `|` again or its close button closes it. It works
  while playing, so enter keeps its one job (back to the start). On a track that's its
  instrument, else its first insert, else the effects picker; in the routing column, the slot
  under the cursor. `ed` is the typed version.
- **Keys mode** (`ctrl+k`, again or esc leaves): the typing keyboard is a MIDI keyboard, laid out
  like Ableton's. `a s d f g h j k l ; '` are the white keys from C4, `w e t y u o p` the black
  ones; `z`/`x` shift an octave, `c`/`v` step the velocity (1 20 40 ... 127). Its notes go in as
  one more controller (`AudioEngine::playTyped`), so armed instrument tracks play them and
  recording writes them down. Other characters are swallowed (letters are notes, not
  commands); space, arrows and ctrl keys work as usual. Terminals report presses but not
  releases, so a note ends when its key is seen to be up, read from the keyboard each frame
  (`Console::keyDown`: `GetAsyncKeyState` on Windows, `CGEventSourceKeyState` on macOS, by
  the key's place on a US keyboard). Where that can't be read, a note ends once its key stops
  auto-repeating.
- **MIDI in.** Every input is opened at startup and as devices come and go (up to 32), unless
  switched off. `midi` opens the MIDI panel, like the audio device picker: each input on or off
  (kept by name in `settings.json`, for every session), whether it opened (Windows doesn't
  share MIDI inputs, so another app can hold one), and the last message it sent, lit while it
  sends. The title row has an activity light. Each port has a lock-free queue to the audio
  thread. `taw --devices` lists the inputs and whether they open; `taw --miditest` loops a note
  through a loopback port (LoopBe). Like the pad key, **armed** instrument tracks hear the controller; several
  armed tracks layer. A track that stops listening releases the keys and pedal still held.
- **Recording.** While recording, what the audio thread passed on goes back to the message
  thread with the playhead it arrived at; output latency is taken off (latency compensation
  on), as for pad hits. A note is written when its key comes up (keys still down at stop end
  there); controllers, pitch bend and pressure are written as CCs. Input quantize moves note
  starts and keeps lengths. Recording adds to what's on the track; each pass undoes as one step
  (notes within two seconds of each other coalesce, like pad hits).
- **Playback.** The graph carries the notes in samples. The plugin slot keeps the notes the
  timeline started; any jump (stop, seek, loop wrap) ends them, so nothing hangs. Starting or
  jumping into the middle of things chases them: each controller's last value before the
  position (sustain, mod wheel, pitch bend, pressure, per channel) goes out first, then the
  notes already under the playhead start from there (at their velocity), live and in bounces.
  Plugins get the tempo, time signature
  and position through a play head, for synced arps and LFOs.
- **State.** Plugin state is opaque and can be megabytes, so it lives in its own file
  (`plugins/NN-Name.state`), written when it changed: on closing the window, every 15 s, on
  close and quit. Undoing a track away keeps its file, so redo brings the sound back.
- **Bounces** render instrument tracks through their own plugin instances (copies of the live
  ones' state), offline, so the live ones keep playing.
- **Fill view (`f`).** The focused track takes all the track rows; ↑↓ still move between
  tracks. On an instrument track that's a piano roll: one pitch per row, named on the left,
  black keys shaded. In the normal view a lane spans the pitches in use, one per half row.
  Audio tracks get the same zoom later.
- `snap` quantizes note starts on instrument tracks as it does hits (a note snapped back past its
  clip's start takes the clip with it); Tab stops at clip edges and note starts and ends; copy,
  cut, paste, duplicate and delete work on MIDI clips as on audio clips: trimmed to the range,
  split, removed. A copy keeps only the notes that start in what was copied. `x` drops the
  latest clip.
- **In process, for now.** `PluginSlot` is the only way taw talks to a plugin (prepare,
  render, state, editor), so an out-of-process proxy can replace it. Until then a crashing
  plugin takes taw down; the crash handler saves the session first.
- `taw --plugintest <file.vst3>` loads one plugin and plays it the way the engine does (a chord
  from the timeline, a stop, a controller key, a disarm), and checks its state round-trips.

## Routing and inserts

For every track: five insert slots (A to E) for effect plugins, five sends (A to E) and an
output, the way a console has them. Bus tracks take sends and outputs, so a reverb or delay
lives once on a bus and tracks send to it. Buses also submix a group.

```
TRACK { kind: "bus", busId, … }      # busId: what outputs and sends call it (stable across moves and renames)
TRACK { …, out? }                    # the busId it plays into; absent = the mix
  INSERT { slot: "a"-"e", plugin, pluginName, pluginState, bypass? }   # as an instrument track keeps its plugin
  SEND { slot: "a"-"e", to, db, pre?, mute? }
```

| Command | Meaning |
|---|---|
| `ins` / `ins cla-2a` | the effects picker for the focused track's first free insert / the best match by name (like `inst`) |
| `2 ins valhalla` | an effect in the first free insert (`2 ins` alone: the effects picker, and the message line lists what's in the slots) |
| `2 ins b valhalla` | into insert B (replacing what's there) |
| `2 ins b` | open or close insert B's window (empty: the picker for B) |
| `2 ins b x` / `2 ins l3 x` / `2 ins x` | remove insert B / the one whose name matches / the only one (`del` and `remove` work as `x` does); with several and none named, it says which are there |
| `2 ins b off` / `on` (by name or the only one too) / `2 ins b>d` | bypass, back on, swap B and D |
| `bus Verb` / `add bus Verb` | new bus track (no input, no clips, nothing to arm) |
| `2 send verb` | a send to Verb: the slot already going there, else the first free one; a name no bus has makes the bus |
| `2 send b verb` / `2 send b 6` | slot B to Verb / to bus track 6 |
| `2 send b -12` / `pre` / `post` / `off` / `on` / `x` | level (a sign or 0: `-12`, `+3`, `0`, `-inf`), before or after the fader, off, remove |
| `2 send verb x` / `2 send x` | remove the send going to Verb / the only send (`off` and `on` the same ways); a bus it doesn't send to says so rather than making one |
| `2 send` | list track 2's inserts, output and sends |
| `2 out drums` / `2 > drums` / `2 out mix` | where the track goes after its fader; `2 out` alone opens the picker |
| `\` | into the routing column on the focused track (see below) |
| `io` | show or hide the routing column (on by default; kept in `settings.json`) |

Inserts are `ins` and sends `send`, named for where they sit in the routing (an effect can be
either). `fx` is the old name for `ins` and still works. Removing reads the same for both: the
slot, the name (an insert's plugin, a send's bus) or nothing when there's only one, then `x`.
On the routing column (`\`) it's `del` on the cell, and `M` bypasses an insert or mutes a send.

The effects picker lists what fits the track: where a plugin comes as "X Mono", "X Mono/Stereo"
and "X Stereo" (Waves), a mono track sees the Mono and Mono/Stereo ones, a stereo track the
Stereo one. Typed names pick the same way.

- **The routing column** sits left of the timeline, after the track header, as in Pro Tools
  (the meter is right of the timeline). A track's first row shows inserts A to E, its second
  sends A to E and then the output, and a third row the send levels (pre-fader in the accent
  colour). A track one row tall shows its inserts and output. `\` steps into the column: ←→ walk the inserts, the
  sends, then the output; ↑↓ go row by row (a track's inserts, its sends, the next track's
  inserts; on the output, track to track). On an insert, `|` opens its window (an empty one:
  the effects picker, type to filter), `M` bypasses it, shift+←→ moves it along the chain, del
  removes it. On a send, `|` picks its bus (a name nothing matches makes that bus), `[ ]`
  ride its level, `M` turns it off, `P` flips pre/post, del removes it. On the output, `|`
  picks a bus and del puts it back on the mix. Esc or `\` leaves. `ed` there opens the insert.
- **Plugin windows.** They have taw's own title bar (the theme's colours; the track, slot and
  plugin on the left; who has the keyboard on the right; minimise and close). They sit over the terminal but never keep the keyboard: click and drag
  in one, and a moment after the mouse lets go the terminal has the keyboard again, so keys
  keep reaching taw (not while a menu is open or a button is held; a plugin's own dialogs, like
  naming a preset, keep it). `Ctrl+\` (or `ed close`) closes every plugin window from anywhere. Double-click a plugin window's title bar to let it keep the
  keyboard (typing into the plugin): its title and the transport bar say `keys → plugin` until
  you click the terminal, close it, or double-click the title again. Windows only. Loading an effect opens its window, and so does picking an instrument (`inst`), to choose its sound. While a plugin's window is open, the track shows which plugin it is in the plugin colour (as `keys → plugin` is): the `inst` label and the plugin's name on an instrument track, the insert's slot in the routing column, so with several of the same plugin it's plain which one you're changing. One window at a time: opening
  another closes the first, and while one is open it follows you as you move between inserts.
- **Signal flow.** Source (clips, hits, instrument, input) → inserts A to E → pre-fader sends →
  fader → post-fader sends → output. Bypassed inserts are skipped. Effects run while stopped
  too, so tails ring out and monitored inputs go through them. New sends are post-fader at
  −6 dB. A bus a send makes (`2 send verb` with no Verb yet, or a new name picked with `|` on
  a send) opens the effects picker for its insert A, so its reverb (or whatever it's for) is
  the next thing chosen; esc leaves it empty. Mute silences a track's sends too. A mono track enters a bus at the pan law.
- **Effects follow the track's width.** A mono track's effects are asked for mono in and out,
  else mono in and stereo out (a mono-to-stereo reverb: the rest of the track is stereo from
  there); a stereo track's for stereo. A plugin that takes neither runs as it is, fed both
  sides (a mono-only one gets them summed) with its output to both.
- **State** is kept like an instrument's (`plugins/NN-Name.state`), so undoing a removed
  insert brings it back as it was. Bounces run their own copies of every insert.
- **Delay compensation.** Each track's plugins (instrument and inserts) make it late by their
  latency; a bus starts as late as the latest thing feeding it. Every route (an output or a
  send) is delayed so everything meets in time at each bus and at the mix, so a dry track and
  its send to a reverb bus with a linear-phase EQ on it still line up. The delay lines are
  kept across graph rebuilds (a fader move doesn't empty them) and follow a plugin whose
  latency changes. The whole mix comes out as late as its latest path: recordings, pad hits
  and MIDI are placed that much earlier, and bounces drop it from the start; the playhead is
  drawn where you hear it. `taw --exec` prints it.
- **Live inputs skip it.** A monitored input, and an armed instrument while stopped or
  recording, go straight through their routes' compensation delays: you're playing in time with
  what you hear, so holding them back would only make you late. (What was already in a delay
  still comes out.) Their own plugins' latency, and a latent plugin on a bus they send to, still
  count.
- **The mix bus has inserts too** (A to E, `INSERT`s on the `SESSION`): `mix ins limiter`,
  `mix ins b`, `mix ins b off`, `mix ins l3 x` and the rest work as `2 ins` does, and on the Mix row `\` steps
  into its slots (one row, inserts only) and `|` opens one. They run after the mix volume, as
  on a Pro Tools master fader, so a fade-out goes into the limiter rather than coming after
  it (`mixInserts`, shared by playback and bounces). Their latency adds to the mix's after the
  tracks' delay compensation; the mix volume's automation is read where the audio entering
  the mix is, and the inserts' parameter automation (`fx a/<id>` lanes on the session) the
  same. Always stereo. In code, track index -1 means the mix (`ioTrack`, as `automationOwner`).
  The mix has automation lanes too, under the Mix row (shift+A there, `mix lane`, `mix auto`):
  its volume and its inserts' parameters. The Mix row moves up to make room for them.
- **Low latency mode** (`lowlat`, off by default, not saved): inserts with more latency than the
  session's limit (`lowlat 256`, in samples; also in `project`) sit out, so the mix stops
  running late while you record. They're drawn yellow and the transport shows `LOW LAT`.
  `lowlat` again brings them back. Bounces always use every plugin.
- **No loops.** A route that would feed back (a bus feeding itself, or a chain that comes
  round) is refused, and the picker only lists buses that can take it. The graph runs every
  track before the buses it feeds. A loop in a hand-edited session is broken: the routes that
  close it are dropped.
- **Solo follows the signal.** Buses a soloed track feeds stay audible (soloing a vocal keeps
  its reverb). Tracks feeding a soloed bus keep feeding it but stay out of the mix (soloing the
  reverb plays only the reverb; soloing a drum bus plays the drums).
- A track whose output bus is gone (undo) plays to the mix; a send to it is shown as `?` and is silent.
- Bounces route the same way: live playback and bounces share `routeTrack`.
- Up to 32 buses. A bus plays even while stopped, so monitored inputs and tails reach it.

## Mono, stereo and pan

A track is mono or stereo. Audio tracks are mono unless they play a stereo clip, or are set:
`2 stereo` / `2 mono`, `add Keys stereo`, or giving a pair of inputs (`2 i 3-4`). A stereo
track records two inputs (its input and the next one) into one stereo file; its input shows as
`i3-4`. Instrument and bus tracks are always stereo. A mono track plays one signal (a stereo
clip on it is summed) until it's panned or an effect makes it stereo.

`2 pan l30`, `2 pan 45r`, `2 pan c` (or -100 to 100); alt+[ and alt+] nudge the selected or
focused tracks 5 at a time. Pan sits after the inserts and before the sends and fader, so sends
follow it. A mono signal pans with the -3 dB law (0.707 each side at centre, unity hard over);
a stereo one is balanced (centre leaves it alone). The meter panel shows pan under the volume,
and `st` on stereo tracks.

```
TRACK { …, pan?, channels? }   # pan -1..1 (absent = centre); channels 1 or 2 (absent = from what it plays)
```

## Editing

Select a range with Shift+arrows (time) and Shift+↑↓ (tracks); with no track selection the
focused track is used. What moves is clips, trimmed to the range: audio clips, hit clips on
sample tracks, MIDI clips on instrument tracks.

| Key | Command | Does |
|---|---|---|
| `Ctrl+C` | `copy` | copy the range (clips trimmed to it) |
| `Ctrl+X` | `cut` | copy, then clear the range |
| `Ctrl+V` | `paste` | paste at the cursor (also during playback) onto the focused track, extra lanes on the tracks below |
| `Ctrl+D` | `dup` | paste right after the range, then select the copy (press again to keep repeating) |
| `Delete` | `del` | clear the range |
| | `sel 3 5` | select bars 3 to 5 (any `@` position: `sel 1.2 1:30`) |
| double-click | `sel clip` | select the clip under the cursor on the focused track (a MIDI or audio clip also opens its editor on double-click) |
| `Alt+Shift+3` | `cons` | consolidate: what plays in the range becomes one clip per track (below) |
| `Ctrl+E` | `sep` | separate (Pro Tools' separate clip): with a range, the clips are cut at both its edges, so the range becomes a clip of its own (one clip makes three); with none, cut at the cursor (the playhead while playing) on the nearest grid line. The halves of an audio clip point into the same file; a MIDI or hit clip's tail keeps the notes and hits that start in it |

- Pasted audio clips layer on top (later wins); pasted hit clips replace what was under them
  (two loops at once would double every hit); pasted MIDI clips add to what's there.
- **Consolidate** (`cons`, as in Pro Tools): the range on each track becomes one clip exactly
  its length. MIDI and hit clips: the notes and hits that play there are gathered into one new
  clip (a note keeps the length it played for). Audio: what plays there is rendered into a new
  file in `audio/` (`03-Name-cons.wav`, 32-bit float), silence where nothing plays, later clips
  over earlier ones; the old files stay. Gain, inserts and pan aren't printed. Undo takes the
  new clip away and brings the old ones back.
- Clips only point into files, so copies cost nothing on disk.
- A lane only pastes onto its own kind of track; mismatches are skipped and reported.
- Each edit is one undo step. Hits are positioned in beats, so they follow tempo changes.
- `Ctrl+C` with no range clears a typed command line; it never quits. Terminals turn `Ctrl+V`
  into pasted text, so `Ctrl+C` also puts `taw:paste` on the Windows clipboard; that text
  arriving is taken as the paste.

### Fades

Audio clips only (`Fades.cpp`). `Ctrl+F` (Pro Tools' fades) works from the selection, on each
selected audio track:

| Selection | Makes |
|---|---|
| from a clip's start (or before it) into it | a fade in, to the selection's end |
| from inside a clip to its end (or past it) | a fade out, from the selection's start |
| across where two clips meet (touch or overlap) | a crossfade spanning the selection |

Several at once are all made; a whole clip inside the selection gets nothing. `Ctrl+F` again
on the same fades moves each to its next curve: equal power (`eqp`, default), linear (`lin`),
S-curve (`s`), exponential (`exp`). Typed (not `fade`: `f` is a hotkey): `clip fade` (as
`Ctrl+F`), `clip fade in 10ms [shape]`, `clip fade out 1/4`, `clip fade off` on the clip under
the cursor; `xfade 20ms [shape]` where two clips meet nearest the cursor (left selected, so
`Ctrl+F` changes its curve); `declick 2` / `declick off`. Lengths: `ms`, `s`, note values
(`1/4` a beat), a bare number in ms.

- **Model:** `fadeIn`, `fadeOut` (samples) and `fadeInShape`, `fadeOutShape` on the CLIP.
  `renderClips` crossfades a clip's fades with what's under it (what earlier clips wrote, or
  silence): `under * curve(1 - t) + clip * curve(t)`. So a crossfade is the later clip
  overlapping the earlier one with a fade as long as the overlap; making one stretches both
  into their handles (the file past their edges), and refuses, saying how much there is, when
  a handle is too short. Only the top clip of a crossfade keeps a fade on that side.
- **Declick:** every audio clip edge without a fade gets one of `declickMs` (session, default
  2, 0 off) when the graph is built, except where another clip butts against it exactly (a cut
  in one take carries straight on).
- **Edits:** edges a cut, delete, separate or copy makes are new, so they lose their fades;
  edges that stay keep them.
- **Screen:** on the clip's top row, `╱` over a fade in, `╲` over a fade out, `╳` over a
  crossfade. Declick isn't drawn.
- **Next:** curves and corner handles in the audio editor; clip gain in the same render.

### Playlists

As in Pro Tools: other versions of a track's clips, so you re-record the takes you don't like
and keep the ones you might. A track's own clips are its **main** playlist, the one that plays,
records and bounces; the others are `PLAYLIST` children of the track (a name, the clips),
which read like a track wherever clips are read. Automation belongs to the track and is shared
by all of them. `Playlists.cpp`.

| Key | Command | Does |
|---|---|---|
| `Ctrl+P` | `pl new` | a new empty playlist becomes the main (`Vox.01`, `Vox.02`...); the old main joins the others |
| `Ctrl+Alt+P` | `pl copy` | likewise, starting as a copy of the main |
| `Shift+P` | | the focused track's playlists as lanes under it, each as tall as the track (closes its automation lanes: a track shows one or the other) |
| `Shift+S` on a lane | | it plays in the main's place, on that track only (one at a time; again: the main). Closing the lanes ends it |
| `Ctrl+Alt+↑/↓` | | the previous / next playlist (by name, round again) becomes the main, playlist lanes open or closed |
| `Alt+Shift+↑` on a lane | | its selection replaces that range on the main (Copy Selection to Main Playlist); no selection: the whole playlist becomes the main |
| | `pl`, `pl 2`, `pl Vox.02` | list them; make one the main (numbered as the lanes are), the main taking its place |
| | `pl del [n]` | delete one (its audio stays in `audio\`) |

- On a playlist lane, copy, cut, paste, delete, duplicate, consolidate, Tab and shift+E act on
  its clips, and none of it touches the automation. The track row shows the main's name.
- **Loop recording:** with the loop on, recording starts at the loop and goes round it, each
  pass a take. When it stops, the last whole pass is on the main and each one before it is a new
  playlist (the main as it was, plus that take); a pass cut short by stopping is dropped, as in
  Pro Tools (its audio stays in the file). Audio is one file through every pass, cut into a clip
  per pass when it's placed (`loopTakes`); MIDI and pad hits are folded into the loop as they
  come (the engine counts loop wraps, `TimedMidi::wraps`) and split off at each pass's end, as
  heard. Recorded within one pass it's an ordinary take. `taw --playlisttest <session>` checks
  the splitting.

## Headless commands

`taw --exec <session> "<command>" …` runs command lines against a session without the screen,
prints each result and a summary, and saves (waiting for imports, bounces and plugin scans to
finish). `TAW_CONFIG=<folder>` uses another config folder (templates, device, plugin list). It's
the first way in for scripts and agents. Device-dependent commands (record, input checks)
report what they can't do. For testing the screen, `:key o` (or `:key right`, `:key enter`,
`:key ]`, …) presses a key and `:screen` prints the screen as plain text (160×44).

`taw --new <folder>` makes an empty session for `--exec` to build in.

### Agents in the open session

`--exec` only reaches a session that's closed. `taw --mcp` lets an agent (Claude Code, any MCP
client) work in the one open in a running taw while you record and audition in it
(`docs/plan-taw-mcp.md`; `src/Agents.*`, `src/AgentEdits.cpp`).

- **How:** each interactive taw listens on a local socket (127.0.0.1, a free port) and says so in
  `agents/<pid>.json` in the config folder, with a token the bridge must send. `taw --mcp` speaks
  MCP over stdio and relays tool calls there; it picks the newest taw with a session open
  (`--session <folder or name>` picks another). A bridge that crashes or talks rubbish only loses
  its connection. Claude Code: `claude mcp add taw -- <path>\taw.exe --mcp`.
- **Tools:** `get_session`, `get_track` (reads, also while playing); `add_track`, `remove_track`,
  `write_notes`, `write_hits`, `remove_clip`, `add_marker`; and `run_command`, a line of the
  command language as typed (not save, close, quit, the transport, undo/redo, bounce, the
  cursor, focus or clipboard, or anything that opens a window).
- **Your parts stay yours:** while you're at it (a key, a click or MIDI in the last 10 s), an
  edit touching the track you're on or anything inside your time selection is refused with the
  reason, and rolled back before the engine hears it (the edit runs in a guard that notes
  everything it touched). Leave the keys and those are open to agents, so you can sit back with
  the track you want worked on still selected. Anything you changed in the last 5 s is refused
  either way. While playing, recording or counting in, every edit is refused ("playing");
  agents try again.
- **You see it happen:** what an agent changes flashes in the agents' colour (violet) for a
  moment, then stays that colour for 4 s: the track's name (a new track too), its volume, pan
  or send, the mix volume, the tempo.
- **Like a person's edits:** on the message thread (`App::pollAgents`, once a frame, one call at a
  time), into the tree, then the usual graph rebuild, so nothing new runs on the audio thread.
  Each call is one undo step named for the agent ("claude-code: add track Bass"), and its line
  shows on the status line. Agents never save; their edits wait in `unsaved\` like yours. Your
  cursor, focus, selection and view stay where they were.
- **Readout:** the title line, left of the MIDI, says `agents on` (`agents off`), then the
  agent's name once it calls, with a light that blinks on each call, and for 10 s what its last
  edit did: ✓ and its line, `held: waiting for stop`, `hands off: ...`, `working...` (an import),
  `asks you` (the tempo box), or ✗ and why. An edit refused while playing also puts a yellow
  `● claude-code waiting ×3` chip on the transport; on stop the status line says it tried and can
  edit now.
- **Automation:** `write_automation` (a track or `mix`; `vol`, `pan`, `mute`, `send a`-`e`, or a
  plugin parameter by name or by `get_parameters`' param; points as beat and value in the lane's
  units, or text as `pt` takes it). By default it replaces only the span it writes; `replace:
  all` the whole lane. The lane is made if needed and opened under its track (not over open
  playlists), and its name lights up like other agent edits. `get_track` and `get_session`
  (`mix_automation`) read lanes back.
- **Sessions:** `list_sessions` (recent and in the sessions folder, the templates, which is
  open), `new_session` (a name, dated as yours are, in the sessions folder, from a template) and
  `open_session` (a folder or name). On the session screen they happen at once (`taw --mcp` with
  no `--session` picks a taw with nothing open when none has a session). Over an open session a
  box asks: with unsaved changes `s` save (and bounce last-saved) / `o` save only / `d` don't
  save, then switch; with none, `enter` switch; `esc` stays. The agent gets `needs-user`, and
  calling again with the same name says what you chose (`declined` for 2 minutes). Nothing is
  made until you say yes. `run_command` can't `import` a Live set (that closes the session).
- **On or off:** `agents` / `agents on` / `agents off` (and a row in `project`), kept in
  `settings.json` for every session, on by default. Off, nothing listens, and `taw --mcp` says
  agents are turned off.
- **Track ids:** every TRACK has a `uid` ("t7", with `nextTrackId` on the SESSION) that stays
  through moves, renames, undo and saves; a copy gets a new one, and none is handed out twice.
  Sessions from before get them on load. Agents name tracks by it.
- **Tempo:** an agent's tempo change goes straight through unless there are audio clips (MIDI
  and hits are in beats and follow it). Then a box asks you: `t` keep the audio where it is in
  time, `b` move each clip's start with the beats (not stretched), `esc` cancel; stretching is
  shown as coming. The agent hears what you chose when it asks again.
- `taw --mcptest <folder>` checks it all with no device; `taw --serve <session> [seconds]` holds a
  session open headless so `taw --mcp` can be tried from a shell.

`taw --library` lets an agent go through the sample library the way the browser lets a person:
`kinds` (counts, and the measurement names), `find <terms>`, and `like <name> <terms>` (the
samples that sound most like the first whose file name contains `<name>`, nearest first).
Terms: `kind=kick`, `name=tape` (file or folder name contains), a measurement compared with a
number (`decay<200`, `brightness>80`; brightness and pitch in Hz, attack, decay, tail and
length in ms, loudness, peak, punch and lowend in dB, noise 0..1, hits), `sort=decay` (or
`sort=-decay`, largest first), `limit=20`, `json` (paths, kinds and every measurement). The
paths go straight into `sample "<path>"`. Asked for "a boom bap beat at 80", an agent can pick a
knocking kick (`find kind=kick brightness>100 decay<150`), a crack snare, dusty hats, then
`--new`, `--exec` with `bpm 80`, `sample`, `pat /16 ~58 …`, levels, and a bounce to check.

## Importing

`import <path> [<path> ...]` makes one new track per audio file, placed at the cursor. A folder
imports every audio file in it (not recursive), in name order. `2 import <path>` puts the
file(s) on track 2 as new takes instead. Quote paths with spaces, or give a single unquoted
path.

- Files are decoded (wav, aiff, flac, ogg, mp3 via JUCE; m4a/aac/wma and anything else Windows
  can decode via Media Foundation), resampled to the session rate (windowed sinc, latency
  compensated so timing stays sample-exact), and copied into `audio/` as 32-bit float WAV, so
  the session never depends on the original files.
- Mono stays mono; stereo stays stereo (wider files keep their first two channels). A stereo
  track reaches the mix at unity; mono tracks use the -3 dB centre pan law.
- Runs on its own thread; `Esc` cancels.
- Known gap: MP3 encoder padding isn't trimmed, so MP3 imports start ~25-40 ms late.

**MIDI files** (`.mid`, `.midi`) import at once, not on the thread. Each of the file's tracks (each
channel, in a one-track file) becomes an instrument track named after it, holding one MIDI clip at
the cursor, whole bars long, with its notes and controllers; `inst <name>` on each track then picks
what plays it (until then the track says so). `2 import song.mid` puts the first part on track 2.
Into an empty session the file's first tempo comes too; a tempo map doesn't (taw has one tempo).

**Drag and drop imports immediately.** A console window turns dropped files into typed text.
taw spots that as a burst of characters faster than anyone types, holds it back from the key
handlers (a drop during playback must never fire hotkeys), and once the burst has finished
arriving checks it: if it's nothing but existing absolute paths, they're imported at once (a
new track each); anything else was an ordinary paste and lands on the command line.

**Paths from Mac drives and SMB shares.** Those names map characters Windows forbids (a
trailing space, a trailing period, `:` …) into the Unicode private use area, and a console drop
loses them. When a path doesn't exist as written, taw walks it component by component and
matches names ignoring private-use characters and trailing spaces/dots. Command-line arguments
are read as UTF-16 for the same reason.

`taw --importtest <dest> <files...>` imports without a device, for testing.

### Ableton Live sets

`import song.als` (or dropping one) makes a **new session** from the set, beside the open one
and named after it, then closes this one and opens that (`AlsImport.cpp`). The set and its
files are only read. What didn't come across is listed in the new session's
`import-report.txt` (the message line says how many notes it has).

- **Comes across:**
  - tempo and time signature, locators as markers, the loop
  - MIDI tracks as instrument tracks: the first device is the instrument, later plugins are
    inserts
  - audio tracks: files copied into `audio/`
  - returns as buses, with every track's sends (Live's pre/post per return), and groups as
    buses their tracks play into
  - the master's volume, and its plugins on the mix
  - volume, pan, mute and sends
  - automation as lanes: mixer controls, plugin parameters (Live keeps them in the plugin's
    own units: normalised by the range Live saved), the master's volume. Live's opening
    event becomes the point at beat 0.
- **MIDI clips:** a clip is a window onto its content, as in taw: `CurrentStart` and `End`
  on the timeline, from `LoopStart + StartRelative` in its content. Content can start
  before 0. A looped clip becomes a clip per pass. Disabled notes are left out.
- **Plugins:** found by name. A VST3's saved state is Live's `ProcessorState` and
  `ControllerState`. Those are the plugin's own bytes as hex, re-wrapped as JUCE's
  `VST3PluginState` and written to `plugins/`, so patches come across exactly. Checked: a
  ZENOLOGY saves back the same bytes it was given. A VST2 or AU starts from its default
  settings.
- **Left out, and said so:**
  - Live's own devices (EQ Three, Compressor, racks…); ones switched off aren't mentioned
  - their automation
  - clip envelopes, muted clips, Session View clips
  - tempo and time signature changes (a single value isn't a change)
  - clip gain and fades
- **Time-stretched audio:** warped to a tempo other than the set's, it can't play right
  without time-stretching. Its clips are left out and the report says to export the track
  from Live and import it at bar 1. That track's fader is set to 0 dB and its volume
  automation is imported but off (shift+O), because the export has them.
- `taw --alsimport <set.als> <new session folder>` does it without a device, for testing.

## The sample library

For finding the right sample among thousands: taw measures each one once, sorts them by kind
and lays them out on a map where samples that sound alike sit together. `index <folder>` (on its
own thread; `taw --index <folder> ...` from a shell prints a report) adds folders; typing
`index` and dropping a folder on the input line does the same, as a drop alone imports.
`index remove <folder>` (or `index remove` and a drop) takes a folder's samples out again, and
`library` lists the indexed folders with how many samples each holds. The folders are kept in
the index; one inside an indexed folder isn't listed twice.

**The browser** is a window of its own (`BrowserWindow.cpp`), over the terminal like the editors:
mouse-forward like a plugin's, and as usable from the keyboard. `ctrl+B` opens it in front with
the keys (on the map; typing filters from anywhere), `Esc` gives them back to taw (clearing the
filter, or "like this", first) and leaves it open, and `ctrl+B` while it has the keys closes it
(`browse` opens or closes it; `browse hide`). Keys it doesn't use (ctrl ones, `shift+Space`) go to
taw. It needs the screen: headless, `taw --library` is the way in. The state and keys are App's
(`Browser.cpp`); the window only draws it and has the mouse. `taw --exec` can draw it into a
picture (`:browser shot.png`, or `:browser shot.png 600x500` at another size) and press its keys (`:bkey right`). The docked column it replaced is
gone. In the window:

- **The guide**: `?` (or the toolbar's `?`) lays the keys, the mouse and what the colours and
  filter terms mean over the map and list; `?`, `Esc` or a click puts it away, and any other key
  puts it away and does what it does. The status line ends with "? keys" when there's room.
- **Kind tabs** across the top, each with its count, in the kind's colour; a click (or `←→` with
  the tabs focused) shows only that kind. `Tab` goes round the tabs, the map, the list and (on a
  kit) the pads; `↑` from the top of the map reaches the tabs.
- **The map** fills the middle, the **list** sits on its right (the same samples one by one, the
  kind as a strip, the folder dim), and the **status line** under them shows the chosen sample's
  numbers. **Drag over the map** to audition whatever is under the pointer (XO-style); with
  **hover plays** on (settings.json) moving over it is enough. A click in the list auditions; a
  double-click (off a kit) loads it.
- **Audition in time** (on by default, settings.json): while taw plays, an audition waits for the
  next grid step of what you're hearing (the playhead less delay compensation, wrapping at the
  loop's end), so each candidate is heard in the groove. The engine holds it in its preview voice
  until the playhead gets there (`PreviewSound::at`), giving up after 4 s.
- **Like this:** the toolbar's button shows the 40 samples that sound most like the chosen pad's
  sound (or the chosen sample), nearest first; again (or esc, or a tab) shows everything.
- **Selectors** in the toolbar show what the map's axes are; `alt+←→` steps x and `alt+↑↓`
  steps y from anywhere in the browser (or the menus):
  - `similar` (the default) is the sound-alike map below, both axes at once, where only nearness
    means anything. Otherwise two measurements: brightness, attack, decay, tail, length, pitch
    (unpitched at the bottom), loudness (RMS), punch (crest), noise (flatness), low end (energy
    under 120 Hz). Each is spread by rank over what's shown (the kind and the filter), so the map
    fills evenly and filtering down spreads the few left over the whole map; the row shows the
    real range ("49 Hz → 335 Hz"). On the map the y axis is named with its high end in the
    top-left corner and its low end in the bottom-left (on a plate over the dots), the x axis on
    the rule under it: low end, name, high end. The two are never the same; `y` is skipped on
    the sound-alike map. Kept in settings.json.
  - Colours are always by kind, like the badges.
- **The map**: each kind's samples on a map of their own (so 187 kicks spread over the whole
  area, not their corner of the full one), a dot each in its kind's colour. The arrows go to the
  nearest sample within 60° of that direction; `ctrl+arrow` jumps a fifth of the map that way (to
  the sample nearest that spot), to cross it quickly and then home in with the plain arrows. A
  click picks the nearest one. The chosen one is a ring with its name.
- **Typing** filters, in the toolbar's input box: words match file and folder names, and measurement terms the sounds,
  `decay<200 bright>1k pitch>60 len<0.5s` (the `--library` names or short ones: bright, att, dec,
  len, loud, low; numbers take k and s). `Space` between terms (with the filter empty it plays
  the sample again). The idea: filter to the qualities you want, then move on a smaller map.
  Page keys and Home/End step through what's shown in order. The status line shows the chosen
  sample's numbers: kind, length, attack, decay, pitch (when it's clear), brightness, RMS.
- **Every move auditions**: the file, resampled to the device rate, straight to the outputs,
  stopped or rolling (not metered, never bounced), at the browser's own level (`shift+↑↓`, `browse
  vol -6`, saved). `Space` plays it again (while the filter is empty); `shift+Space` is the transport.
- **Enter** puts it on a track: on a kit (or one of its members), onto the lit pad (below); on a
  focused sample track it replaces the sample and keeps the hits, so with the loop playing you hear
  each candidate in the pattern; anywhere else it makes a new sample track. **shift+Enter** always
  makes a new sample track. A replacement shows: the track's header flashes, the message says
  "track 5 Kick now plays bd6 (was …) · ctrl+z undoes", and a track still named after its old
  sample (`sampleFrom`, the file it was copied from) takes the new one's name (a name you gave
  it stays). The browser stays open for the next.
- **The pads** (when you're on a kit): its 4×4 under the list, pad 1 bottom left, each with its
  member's name in its sound's kind colour. A click (or `Tab` to them and the arrows) lights a
  pad and plays it; `Space` plays it again. With **audition → pad** on (the default), every
  sample you pick goes onto the lit pad at once, as in Battery or XO: the member keeps its hits,
  so with the loop running you hear each candidate in the beat, and a run of picks is one undo
  step (`ctrl+z` puts the old sound back). Hovering only auditions; a drag over the map loads
  where it's let go. **Drag a sample** from the list or the map onto any pad to put it there.
  An empty pad gets a new member, under the kit. A sample the session already has a copy of
  (this run's imports, or a track's `sampleFrom`) goes on from that copy, so going back and
  forth doesn't fill `audio/`; each new one is copied in as any import is.

- **The index** is `library.json` in taw's config folder. Nothing is written beside the samples.
  A file is measured again only when its size or date changes; `._` files (Mac resource forks
  on shared drives) are skipped. All cores; about 500 one-shots a second.
- **Measured** (from the first 10 s, in mono): peak; RMS over the part that sounds; crest
  factor; attack (10% to 90% of the peak); decay (to 20 dB down for good) and tail (40 dB down);
  hits, attacks above 500 Hz in the first second (a clap has several; above 500 Hz so a kick's
  slow wave isn't a hit per cycle); pitch and its confidence (YIN after the attack); brightness
  (spectral centroid), rolloff, flatness (tonal versus noise); energy in nine bands from sub to
  air; 13 MFCCs.
- **Kinds** (kick, snare, clap, rim, hihat, open hat, cymbal, tom, perc) come from the file's
  name, then its two folders, in the shorthand packs use (`808BD`, `snar5`, `HiHat/`). The sound
  sorts the rest: the five nearest named samples vote, nearer ones counting more. Measured on
  samples whose names say what they are, the sound alone agrees 92% of the time (808 kit plus
  an HR16); the misses are between neighbouring kinds (a short open hat and a closed one).
- **Sounds alike** means close in the measurements, each scaled to its spread across the
  library and weighted: the envelope most, then the brightness and noisiness, then the bands
  and MFCCs.
- **The map** is a t-SNE of the same distances, scaled to 0..1 and kept in the index. It's the
  exact O(n²) form, fine to a few thousand samples; a whole drive will want Barnes-Hut.

## Automation

Volume, pan, mute, the sends, the mix volume and any plugin parameter (instrument or insert)
can change over time.

- **Model:** an `AUTO` lane per parameter on its track (on the `SESSION` for the mix), with
  `POINT`s at beats (so they follow the tempo, like MIDI). `param` says what it controls
  (`AutomationTarget`): `vol` and `send a`-`e` in dB, `pan` -1..1, `mute` 0/1 (held, not
  ramped), `inst/<id>` and `fx a/<id>` a plugin parameter 0..1 by its VST3 ID. Straight lines
  between points, flat before the first and after the last; a new lane starts at the session
  start with the parameter's setting, so its first point ramps from where it was.
- **Playback:** `buildGraph` turns lanes into `Curve`s (samples). The mixer's are applied by
  `mixTrack` in steps of 32 samples, each at its start's value, so a ride is smooth whatever the
  block size; plugin parameters are set before each block (`automatePlugins`, only when they
  change; JUCE's hosted parameters take values from the audio thread). The mix volume is read
  at the playhead less delay compensation, so it moves with what you hear. Bounces use the same
  functions and 512-sample blocks, so they step like playback. Mute automation decides
  muting where there is some (solo still applies).
- **Lanes:** shift+A opens a track's automation lanes under its clip row (`lanes` on the
  track; a track with none opens with a volume lane), two rows each on tall tracks, one on
  short ones: the curve as a filled graph, points in the accent colour, and in the header the
  parameter, its value at the cursor (the playhead when rolling) and its controller. Up and
  down move through them (`focusedLane`); a click lands on one. Tracks are as tall as their
  lanes make them, so the layout keeps a row map (`LaneMap::rows`) for the mouse. A lane stays
  when its points are cleared, until it's removed (`auto`, delete).
- **Lanes are views:** what a track's lanes show is its own list (`laneParams`, kept once a
  lane is added or changed; before that, its AUTO lanes), apart from the automation itself.
  On a lane, `|` (or `auto`) opens the list and enter makes that lane show another parameter
  (`changeLane`; a lane already showing it swaps places); the old parameter's automation keeps
  playing and stays in the list. `lane cutoff` does the same by name; from the track's row
  both add a lane instead.
- **Editing:** on a lane, `pt <value>` puts a point at the cursor (with a selection, the
  selection becomes that level and the curve outside it stays), alt+up/down moves the selection's
  points (or the point at the cursor; makes one if needed), and delete removes the
  selection's points. Each is one undo step. `[ ]` still ride the track's volume. `lane cutoff` adds a lane by name.
- **The list (`auto`):** everything the focused track can automate: its lanes first, the
  mixer, then each plugin's automatable parameters (instrument, then inserts), with each
  one's value, points and controller. Typing filters; enter adds (and goes to) a lane;
  delete removes one.
- **Controllers:** `map` maps the next CC moved to the lane you're on (`map 74` names
  it, and moves it off any other lane), saved on the lane (`num`, `ch`). `unmap` clears every
  mapping, `unmap 74` one controller, `unmap lane` the lane you're on. The MIDI inputs send mapped CCs (all CCs
  while learning) to the message thread instead of the instruments (`ControlRouting`). A
  moving controller sets a `LiveValue` the lane's curve reads, so you hear it at once (touch);
  on a lane with no points a knob is a fader (where it's left becomes the setting). With write
  on (shift+W, `aw`) and playing, its moves are recorded; 0.6 s after it stops moving, the
  touch releases: its points replace that stretch (one undo step) and the curve glides back
  over 0.25 s. A stop releases everything. shift+O on a lane turns its automation off (`bypass`
  on the AUTO): its points stay, drawn faint with OFF, but playback, bounces and readouts use
  the setting, a mapped knob moves the setting, and writing passes it by; shift+O again turns
  it back on. While it writes, the lane draws what's been
  written so far, from the touch's first point to the playhead, in the record colour
  (`writtenValue`).
- **Follows the clips:** automation goes where the clips over it go. Copy (and so cut,
  duplicate) takes each automated lane's curve over the range with the clips: its value at
  each end and the points between (`gatherAutomation`). Paste puts it down at the cursor,
  replacing what the lane had there and keeping the curve either side (`pasteAutomation`;
  a plugin parameter only onto the same plugin). Cut and delete clear it from the range,
  joining the curve across the gap (`clearAutomationRange`). Dragging a clip in the audio
  editor moves the automation under it when it's let go (`moveAutomation`, the same undo
  step). Consolidate leaves automation alone.

## Bouncing

`b` renders the mix bus (track volume, mute/solo, mix volume; never the click or live inputs)
over the time selection, offline on its own thread, faster than real time. `Esc` cancels.

- WAV: 24-bit at the session rate. MP3: 320 kbps CBR through the Media Foundation encoder that
  ships with Windows, so nothing extra to install (needs a 32/44.1/48 kHz session).
- Default folder `bounces/` in the session; default name `<session>-<timestamp>`. Never
  overwrites. Files are written as `name.part.ext` and renamed only when complete.
- Reports length, path and peak; a peak over 0 dBFS is flagged as clipping. A progress bar sits
  in front of the command line while it runs.
- A finished bounce opens in the file explorer with the file picked out (a session setting, on
  by default; `bounces off` / `bounces on`). `bounces` opens the folder any time. Headless runs
  (`--exec`) never open it.
- `taw --bouncetest <dir>` renders a synthetic mix to both formats without a device.

## Video

Picture for scoring (`Video.h`). The audio is the clock: the picture never runs a timer of its
own, it shows the frame for where the playhead is, so it can't drift.

- **Decoding:** FFmpeg's libraries (libavformat, libavcodec, libswscale), linked from Gyan's
  pinned 9.0.2 shared build, which CMake downloads. The DLLs are delay-loaded, so taw starts
  without loading them, and still runs without video if they're missing. The build's own
  `.lib` files are MinGW import libraries, which MSVC links but can't delay-load (taw.exe then
  wouldn't start without the DLLs), so CMake makes MSVC import libraries from its `.def` files
  at configure time (`lib /def`). FFmpeg rather than
  Media Foundation because editors deliver ProRes and DNx, which Windows can't decode. On
  macOS, Homebrew's FFmpeg (`brew install ffmpeg`, 8.x works) is found at build time and
  weak-linked: without it taw still starts, and `videoAvailable` asks the loader whether the
  libraries are there. Homebrew's next major version renames them, which needs a rebuild.
- **Exact frames:** the frame for a time is the last one whose timestamp is at or before it.
  Moving forward decodes on from the current frame (it's always one frame ahead, so playback
  only has to scale); a jump past a keyframe seeks to the keyframe before the target and decodes
  up to it. `--videotest` checks frames against the ffmpeg command line's, pixel for pixel.
- **Clock:** rolling, the playhead less the mix's delay compensation and the device's output
  latency (what you hear); stopped, the cursor. Less the video's start (`videoStart`, samples,
  negative to skip a slate), less `videoOffset` frames (for a screen that lags).
- **Window:** a normal JUCE window with the native title bar, a view only: it ignores keys,
  never activates (`WS_EX_NOACTIVATE` on Windows, set up before it goes on the desktop, because
  JUCE activates a window when it remakes it; on macOS it can't become the key window, and if
  it's activated anyway `syncVideo` hands the keyboard back to the terminal), and the decoder thread scales frames to its size in
  physical pixels so painting is a copy. Its close button hides it until `video` or the next
  open (not saved: Windows sends every window the same close on shutdown). Its place is saved
  in the session (`videoWindow`).
- **Video row:** a top lane under the ruler (`TopLane::video`) while a player is open: the
  video's extent as a bar with its name, labels of the video's own time, frame ticks when a
  frame is two columns or more, and the timecode at the cursor (non-drop m:ss:ff, negative
  before the video) in the header. Up from the first track reaches it. Arrows step frames
  (ctrl: seconds; alt keeps the beat), landing on each frame's first sample (`frameStart`:
  the first sample the decoder shows that frame at); tab hops to its start and end; a click
  puts the cursor on a frame and a drag scrubs (moves with the left button held come in as
  `Key::drag`, once per cell: from the Windows console, and on macOS from the terminal's
  button-event mouse mode, 1002). Selections made there cover every track,
  like the other top lanes.
- **Bounce with picture (`bv`):** `Bounce` with `Format::video` feeds the mix to `VideoMuxer`,
  which re-encodes the picture (x264, CRF 18, the source's size and frame rate) rather than
  copying it, so a bounce can start on any frame. Output frame n is the frame the window would
  show at range start + n frames (black outside the video); the mix is AAC 320k. Frames are
  encoded as the mix reaches them, so the file interleaves and the progress bar covers both.
  The range is the selection, else the whole video (clipped to the session start). The video
  offset isn't applied: it's for the screen. FFmpeg's log is silenced (it would draw over the
  screen).
- **Session:** `videoFile` (relative to the session if inside it), `videoStart`,
  `videoOffset`, `videoShown`. `syncVideo` reconciles the player with the tree every frame, so
  undo and opening sessions need nothing else.

## Keycast

A window showing the keys pressed, for screen recordings of taw (`KeyCast.h`). `keycast` shows
or hides it; `keycast font <name>`, `color <colour>`, `bg <colour>` (#rrggbb or a name),
`size 1.5` (the text's scale) and `fade 2` (seconds; 0 keeps them) set its look, `reset` puts
the look back. All of it, and the window's place, is kept in settings.json (`keycast`), for
every session.

- **What it sees:** taw's own keys, as `App::handleKey` gets them (after it has handled one),
  so it needs no keyboard hook or permission and nothing typed in other apps shows. Keys typed
  into a plugin window or the MIDI editor that taw doesn't get don't show; mouse clicks don't.
- **What it shows:** the last few presses, oldest dimmer, each fading on its own after `fade`
  seconds untouched. The same key again within a second counts up (`→ ×6`). A key that leaves
  text on the command line is typing: typed characters join into one word (`2v-6`), until a key
  that isn't typing (Enter, Esc, an arrow). Names: `Ctrl+Space`, `Shift+R` (any uppercase
  letter), `Alt+←` (`Opt+` on macOS), letters as caps.
- **Window:** like the video window, a view only (ignores keys, never activates, hands the
  keyboard back if it's activated anyway), and always on top. The text's height follows the
  window's (times `size`); keys that don't fit are left out, and one alone that doesn't fit
  shrinks. Its close button hides it until `keycast` (not saved, like the video's).
  `--keycasttest` checks names, grouping, fading and colours.

## Hub

The audio hub (`docs/plan-audio-hub.md`; macOS so far): one background taw that owns the
interface and routes between it and taw sessions, Voicemeeter-style. Apps (Zoom, a browser)
join it later through virtual devices.

- **Process:** `taw --hub`, headless, run by a launchd agent (`com.taw.hub`, RunAtLoad,
  KeepAlive after a failure) from "taw hub.app", a bundle the build makes beside taw (the same
  binary, ad-hoc signed). The bundle is what gets the microphone permission, since launchd
  starts it, not a terminal. `taw --install-hub` / `--uninstall-hub`, or `hub start` /
  `hub stop` inside taw. One per user (an `InterProcessLock`). Its log is `hub.log` in the
  config folder; `hub-info.json` there says where it listens (a local TCP port), its pid, rate,
  block size, and how many inputs a session gets.
- **Its interface:** `hub-device.xml` once it has one (written the first time it starts),
  else the device taw sessions saved (unless that's the hub itself), else the default; never
  one of the driver's own devices (that would be a loop). `i` on the hub page (or `interface`)
  opens a picker, outputs and inputs side by side (←→ between them, ↑↓ choose, "● current"
  marks what the hub has, enter uses both); `interface MacBook Pro Speakers + MacBook Pro
  Microphone` sets it typed (the start of a name will do). Either restarts the hub on it.
- **Apps: taw.driver** (`driver/TawDriver.c`), a CoreAudio AudioServerPlugIn built beside
  taw. Any app can pick "taw 1" to "taw 4" (stereo both ways) as its speaker or mic. The hub
  reaches them through a hidden 8-channel device, "taw hub": its inputs 2N-1, 2N are what apps
  play into taw N, its outputs go to taw N's mic. Inside it's loopback through ring buffers
  indexed by sample time; every device runs on one clock (one anchor and period, one clock
  domain), so sample time means the same instant on each, and reading clears what was read so
  an app that stops leaves silence. coreaudiod runs it sandboxed: it talks to nothing.
  `taw --install-driver` (or `driver install` on the hub page) copies it to
  `/Library/Audio/Plug-Ins/HAL` with an admin password, restarts coreaudiod (every app's sound
  stops for a moment) and then the hub; `--uninstall-driver` takes it out. Rates 8 to 96 kHz
  (the low ones for Bluetooth headsets with their mic open). A Bluetooth headset is two
  CoreAudio devices with one name, one each way: devices are found by name and direction.
- **The hub's device with apps:** a private aggregate (`HubMac.mm`, only the hub sees it) of
  the interface's output device, its input device when that's another one, and the hidden
  device, clocked by the interface with drift correction on the others. Its last 8 channels
  each way are the apps'. Apps may set the driver to another rate than the hub's; the drift
  correction carries it (seen working at 44.1 kHz against a 16 kHz headset).
- **Sessions join it as a device:** "taw hub" in `dev`, a `juce::AudioIODeviceType`
  (`HubDevice.cpp`), so `AudioEngine` doesn't change. Opening it starts the hub if it's
  installed, says hello over the socket (JSON messages), and maps the shared memory the hub
  made for it (`HubShared.h`). A session gets the interface's inputs, at least 8 (room for apps
  and other sessions on a MacBook's one mic), and 2 outputs.
- **Real time:** each block the hub writes every session's inputs for cycle n into slot n % 2,
  publishes n and wakes it (`os_sync_wake_by_address_any`); the session's device thread
  (real-time priority) renders and marks n done. At n + 1 the hub plays what was done, or
  silence and a late block. The hub never waits on a session, so one that stalls doesn't touch
  the others. A session's outputs are a block later than straight from the interface, which
  its device reports as output latency, so recording, the picture and delay compensation are
  right as they are.
- **Routes:** source channel to destination channel with a level, kept in `hub.json`. Ends are
  `interface`, an app device (`taw 1`: from it, what apps play into it; to it, its mic), or a
  session by its folder, so routes outlive restarts and wait for a session that's closed. A session the hub sees for the first time gets the defaults (every interface
  input to its inputs in order, its outputs 1-2 to the interface's 1-2); after that its routes
  are whatever they've been made. Session to session adds a block. The plan the audio thread
  runs is rebuilt on any change and handed over through a queue like the engine's graphs.
- **In taw:** the hub has a page of its own (`Mode::Hub`), with or without a session: `taw hub`
  from the shell opens taw straight on it (a session folder called hub: `taw ./hub`), alt+h on
  the start screen, `hub` in a session (which plays on meanwhile; one that isn't on the hub is
  told to pick taw hub in `dev`). It shows the interface and a matrix named from the hub's
  side: rows come in, columns go out. Rows are the interface's inputs one by one ("MacBook in
  1": the hardware by its first word, so it isn't taken for the Mac's own output), the app
  devices as `taw1in` (what apps play to taw 1) and sessions' outputs as stereo; columns are
  the interface's outputs in pairs ("MacBook / out 1-2"), the app devices as `taw1out` (what
  apps hear with taw 1 as their mic) and sessions' inputs. Only open sessions are shown: a
  closed one's routes wait in the hub and come back with it (`hub forget` drops them). Beside an app device: who uses it
  now (macOS's process list, a helper as its app, "system output" when the Mac's sound is set
  to it; an app shows only while its audio runs). The chosen row and column are in the accent.
  Arrows move, space turns a cell on or off (stereo to stereo side by side, mono to both sides;
  into a session on the input of the same number for an interface input, else the first free
  pair after the interface's), `[ ]` its level, shift+←→ which of a session's inputs it lands
  on (a track there takes them with `i`), del takes it out, `n` names the app device (`name
  taw 1 Zoom`: a label in taw, kept in hub.json; route commands take it too), `i` the
  interface. A cell is ● on, · off, the inputs it lands on in a session column, ~ when typed
  routes don't fit the shape, ─ an app device or session into itself. The line under it says
  what the cell does. It scrolls both ways. Esc goes back. Its command line takes `route in
  1-2 > Song`, `route taw1in > Song`, `route Song > out 1-2 -6`, `unroute Song > out`, `forget
  Song`, `start`, `stop` (`hub` in front is fine, and in a session it's needed). `this` is the
  session here. Channels pair up (one to many spreads, many to one sums, none given: 1-2). A
  taw with no session open joins with no name and gets no routes until one opens.
- **Without the hub** (Windows so far, or `-DTAW_WITH_HUB=OFF`): the hub command and page say
  it's macOS only, `taw hub` too (rather than making a session called hub).
- **The hub going away:** sessions hear it, wait for it, and carry on when it's back (same rate
  and block), without choosing the device again.
- **Tests:** `taw --hubtest` (an engine and server in process, a thread for the interface, two
  sessions through real "taw hub" devices: sample-exact a block late, the reported latency, a
  stalling session not touching the other, routes, the hub restarting). `taw --hubping` joins
  the real hub as a session for a few seconds and says what it heard. `taw --apptest` plays
  beeps into taw 1 and records taw 2's mic through a route between them (beeps, not clicks:
  drift correction resamples, which smears a one-sample click).

## Startup

With no session given, taw lists recent sessions (newest first, and any sessions in the
launch folder) above the templates. Enter on a recent session opens it; typing a name jumps to
the templates to create a new one. The recent list lives in `%APPDATA%\taw\recent.json`.
Opening a session loads its audio a few files per frame, with a box saying what's loading and
how far along it is; keys wait until it's open. Plugins come after, once the session is on
screen: one a frame (a slow one, like a Roland Cloud instrument, still holds its frame), most
wanted first: an armed instrument, other instruments, then inserts that are on. A bypassed
insert isn't loaded until it's switched on or its window is opened. Until its plugin is in, a
track is silent and shows dim (`loading…` under an instrument's name); the transport bar
counts what's left. Opening a plugin's window, or bounce, loads what it needs at once. Closing
(`close`, or quitting with a session open) does the same in reverse: each plugin unloaded (its
state saved, if you chose to save) under a closing box.

### Saving, last-saved, and the session screen
Saving is something you do (`ctrl+s`), not something taw does behind you. Edits go to the
session's `unsaved\` folder as you make them (the session as it is now, and plugin states
written when a window closes or a plugin is removed); `session.json` and `plugins\` change only
when you save. So the sessions list, sorted by when `session.json` last changed, says what you
last worked on, not what you last opened. The title bar says `unsaved` while there are edits.

Closing or quitting with changes (edits, or a plugin window opened and its settings changed
since the save) asks: `s` saves and bounces, `o` saves without bouncing, `d` drops the changes (`unsaved\` is deleted),
`esc` stays. Saving as you close bounces `bounces\<session>-last-saved.mp3` (WAV at rates MP3 can't do),
bar 1 to the end of the last clip plus two seconds, over the last one. It bounces in the
background with the session's own plugins (they were about to be unloaded anyway, so nothing
reloads), and the session screen comes up at once, with a lit BOUNCING label and a progress
bar near the bottom. A close
without changes bounces too if you saved during the visit. Quitting waits for the bounce on the
session screen (any key stays, esc quits now and drops it).

If taw crashes or the window is closed on unsaved changes, they stay in `unsaved\`; the next
open picks them up, says so, and they're still unsaved until you save. Headless runs (`--exec`)
save to `session.json` as they go, as before.

The session screen: a new session at the top (type its name; the templates under it), then two
columns, recent sessions on the left and bounces on the right, each in a frame; the one with
the keys is drawn heavy and lit. `up` and `down` move within that area (off the end of a list,
into the area beyond); `tab` and `shift+tab` go round the areas, and so do `right` and `left`, so
you never scroll through one list to reach another. The name is a text field: `left` and `right`
move its cursor (past either end, to the next area), `ctrl` with them (and with `backspace` and
`delete`) a word at a time, `home` and `end` to either end. Typing a
name from a list takes you to the templates. The bounces list shows what taw bounced, newest
first: every `b` bounce and last-saved (remembered in `bounces.json` in the config folder),
plus whatever is in the recent sessions' `bounces` folders. With the bounces focused (or one
playing), a player sits at the foot of their frame: the one playing and how far through, else
the one picked, ready. `enter` (or `space`) plays it through the output, decoded on a thread,
without opening anything; while it plays `left` and `right` seek 5 s (alt: 30) instead of moving
between areas, `home` goes back to the start,
`esc` stops it, `o` opens the session it came from, `r` shows the file in Explorer. The header names the audio device they play through (red "no audio device" when a saved
one has gone); `alt+d` opens the device picker, and `esc` or `enter` there comes back.

Each recent session shows its size at a glance, in grey between its name and when it was last
saved: tracks, tempo, plugins (instruments and inserts, the mix's too) and length (to the end
of the last clip), as many as the column fits, read from its `session.json` without opening it.

**Where sessions go.** New sessions are made in the folder on the `in` line, and the screen lists
the sessions in it alongside the recent ones. `ctrl+o` picks another in Windows' folder dialog;
`ctrl+l` types (or pastes) a path over it, `enter` sets it, `esc` leaves it. A folder that isn't
there yet is made, if the drive or folder it would go in is. It's kept in `settings.json`
(`sessionsFolder`), so a client's drive stays the place until you change it; plain `taw` starts
there, and says so if the drive has been unplugged (then it's wherever taw was started).

**Commands with no session.** `:` on the session screen opens a command line of its own (a
session's name can't have a `:`, so typing still names a new session). It runs the commands that
need no session: `help` (or `?`), `index <folder>` (and `index remove`; a folder can be dropped
after `index `), `library`, `scan` / `scan retry`, `agents on|off`, `hub`, `keycast`,
`dark` / `light` / `grey`, `dev` (as `alt+d`) and `q`. Anything else says it needs a session
open and does nothing. `enter` runs it; `esc`, or `backspace` with nothing typed, closes it.
The plugin scan steps here too, so `scan` finishes without opening a session.
`:help` shows a page of the screen's own keys and commands, not a session's; `?` or `esc`
closes it. `esc` or `ctrl+q` quits from anywhere on the screen.

**Slow drivers.** Opening the audio device can take seconds, and changing its sample rate
(opening a 48 kHz session with the device at 44.1) takes an SSL 12 about six, then as long
again when the driver asks to be restarted; its MIDI ports wait the whole time. So: a box says
`OPENING <device>` or `SWITCHING <device> to 48 kHz` while it happens and stays up until the
driver has settled; MIDI inputs open on a thread of their own (the MIDI panel says so while
they're coming in); the device's state is saved at the new rate, so the next start opens the
hardware as it was left instead of switching it back. New sessions (the menu and `taw --new`)
are made at 44.1 kHz, or the rate set in `settings` ("New session rate": 44.1, 48, 88.2 or
96 kHz, for all sessions; `newRate` in settings.json); the device follows each session's rate. Plugins wait for the first frame.

`timing.log` in the config folder lists each step of the last start and open with how long it
took (and anything later that held a frame for over 150 ms), to find what made one slow.

### Many cores, and late blocks
The audio thread doesn't render the tracks alone: every track that isn't a bus renders on
whichever core is free (one helper thread per spare physical core, at the audio thread's
priority, MMCSS "Pro Audio" on Windows), each into its own buffers. Then, on the audio thread,
in order, each is mixed into its bus or the mix, and the buses render once what feeds them is
in. So a block takes about as long as its slowest track instead of all of them added up.
`taw --paralleltest` checks it sounds exactly as on one core. Bounces render on their own
thread, one track after another, as before.

Right of the transport bar, in grey, `dsp NN%` is always shown: how much of each block's time the audio takes (averaged
over half a second; plugins run stopped too, so it's live while you build). Playing, `peak NN%`
joins it: the slowest block this pass. A block over 100% is a dropout (a click), counted in red;
stopping says how many. `stats` opens a panel with the rest: dsp now, its high point in the last
3 s and this pass's worst; the block length and how many cores the tracks share; the machine's
CPU (and taw's share), taw's memory and what's free; and every track's time per block, heaviest
first, as a bar of one block with its recent peak marked, plus the mix's inserts. Keys still
reach the session while it's up (space plays); esc or `stats` closes it.
`taw --loadtest <session> [buffer]` plays a copy of a session without the device, as fast as it
renders, and prints the load (median, p99, worst), each track's share, and the worst blocks:
from the start, across the loop wrap, and starting mid-song. The video decoder keeps to two
threads at low priority so it doesn't crowd the audio.

## When things go wrong

- **Edits are kept as you go.** `unsaved\session.json` is rewritten after every change (until you
  save, which writes `session.json`), so a crash loses at most the last few milliseconds of
  edits; the next open picks them up.
- **One taw per session.** Opening a session writes `.taw-lock` in it (pid, computer, when);
  another taw (or `--exec`) opening it is refused while that taw runs. A lock left by a taw
  that's gone is taken over. One from another computer (a synced folder) can't be checked, so
  it stands until deleted; the message says where it is. Computers are told apart by their
  hardware id, since a Mac's hostname changes with the network.
- **Shared settings.** `settings.json` is shared by every taw running: each writes only the
  settings it changed since it last read or wrote the file, over the file as it is then.
- **Backups.** Each open copies `session.json` to `backups\session-<time>.json` (newest 10
  kept). If `session.json` won't load, taw opens the newest backup that does and keeps the
  damaged file as `session.json.damaged-<time>`.
- **Takes in progress.** While recording, `recording.json` in the session lists each take's
  file, track, start and latency compensation. On the next open, taw rewrites each file's WAV
  header from its size (a writer that never finished leaves stale sizes) and places the clip.
  Only the last moments still in memory are lost.
- **Crashes.** Any thread: a minidump and a one-line log go to `%APPDATA%\taw\crashes`, the
  console is restored, and taw tries one last save. `crashtest` (not in help) crashes on purpose.
- **Unclean exits.** Each interactive run keeps `%APPDATA%\taw\running\<pid>.json` naming its
  session, deleted on a clean exit. A marker whose process is gone means that run died: plain
  `taw` reopens its session directly (recovering takes) and says what happened.
- **Closing the window** (or Ctrl+Break, logoff) quits like `q`: recording stops, takes are kept.
- **Plugins** run in process for now: one that crashes takes taw down (the session is saved
  first). Scanning is already out of process: a plugin that crashes or hangs there is just
  skipped. Playing them out of process is planned.

## Building, checking and releasing

- **Build:** CMake, with JUCE, the ASIO SDK and (on Windows) FFmpeg downloaded by FetchContent.
  Visual Studio 2022 on Windows, the Xcode command line tools on macOS (`./taw build`).
- **Warnings:** taw's own files (not JUCE's, the SDKs' or FFmpeg's, whose headers are system
  includes) build warning-free with clang's strict set wherever clang compiles them, and with
  MSVC's `/W4`. The clang set is JUCE's recommended list, spelled out in CMake because JUCE
  gives clang-cl only `/W4`, less `-Wswitch-enum`: key handlers switch on the key with a
  `default` for the many they ignore (`-Wswitch` still catches a switch without one that
  misses a case). Floats compared exactly on purpose say so with `juce::exactlyEqual`.
  `TAW_WARNINGS_AS_ERRORS` makes any warning an error, on taw's files only; CI turns it on,
  local builds leave it off so a stray warning doesn't stop work in progress.
- **Tests:** the self-tests that need no audio device (`--keycasttest`, `--counttest`,
  `--livetest`, `--cliptest`, `--paralleltest`, `--bouncetest`, `--mcptest`, and `--keytest` on macOS),
  each exiting non-zero on a failure. The others need a device, plugins or files.
- **CI** (`.github/workflows/ci.yml`, every pull request and push to `main`): Windows with
  MSVC (build and self-tests), Windows with clang-cl (taw's files only, so the Windows-only
  code meets clang's rules too), macOS on Apple Silicon (build and self-tests), all with
  warnings as errors.
- **Releases** come only from pushing the `publish` branch (`.github/workflows/release.yml`,
  steps in `docs/releasing.md`): it builds and tests both platforms, then drafts a GitHub
  release for the version in `CMakeLists.txt` with that version's `CHANGELOG.md` notes, the
  zips (Windows x64 with FFmpeg's DLLs; macOS arm64, 13 and later, with the hub app and driver),
  FFmpeg's source and `SHA256SUMS.txt`. Publishing the draft tags the commit. A version that
  already has a tag stops the run before anything builds. Downloads aren't signed yet: the
  README covers SmartScreen and macOS's quarantine. `v0.1.0` tags the first playable
  milestone; 0.2.0 is the first public release.

## Screen

```
 title:     taw ▸ session
 interface: device · driver · SR/buffer · latency · xruns · one meter per hardware input
            (meters shrink to fit, so a 16-input interface stays on one line)
 (blank)
 transport: ■ STOP · bars.beats · time · tempo/sig · click · loop   (centred)
 ruler
 tracks:    number · name · input │ inserts A-E / sends A-E · out │ timeline lane │ volume · meter · peak
            second row: R I S M flags (an instrument's plugin), pan under the volume
            one row tall: the flags go after the name, in a wider header
            the type in colour: a strip down the left edge and a filled label (audio blue: its
            input; smp warm; inst teal; bus dark green), so a track reads at a glance
 mix
 message:   feedback from the last command
 command:   : 1-3 r
 hints:     keys available in the current mode
```

## POC scope

1. Create a session from a template (`blank`, `1mic`, `3mic`, or a user template file).
2. Pick an audio device on first boot from a list; remember it.
3. Live meters for hardware inputs, tracks and mix.
4. Record one or more tracks to WAV; draw the waveform while recording.
5. Play takes back.
6. The navigation keys and the command subset above.
