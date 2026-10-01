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
  SDK's GPLv3).
- **Target terminal: Windows `cmd` (conhost)** first. Rendering uses VT sequences and only
  glyphs that Consolas has (block elements, box drawing). Input uses the Win32 console API,
  which reports modifiers reliably (Shift/Ctrl+arrows).
- **Cross-platform is planned** for future updates: macOS and Linux after Windows. JUCE
  already covers audio devices (Core Audio, ALSA/JACK) and file formats, and the
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
| MIDI input | *(later)* | Timestamped events into a lock-free FIFO |

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
  TRACK { name, input, armed, mute, solo, monitor, gainDb }
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

### Navigation keys (idle and rolling)

| Key | Grid on | Grid off |
|---|---|---|
| `←` / `→` | 1 bar | 1 second |
| `Ctrl+←` / `Ctrl+→` | 4 bars | 4 seconds |
| `Alt+←` / `Alt+→` | 1 step (a beat; a 16th after `grid 16`) | 0.1 second |
| `Ctrl+Alt+←` / `Ctrl+Alt+→` | 4 steps | 0.4 second |
| `Enter` (empty command line) | back to where play started; again for the session start (rolling: keeps playing from there) | same |
| `Home` / `End` | session start / end of last take | same |
| `↑` / `↓` | focus previous / next track; below the last track is the Mix | same |
| `[` / `]` | volume −1 / +1 dB on the selection, focused track, or Mix (also while rolling) | same |
| `{` / `}` or `Ctrl+[` / `Ctrl+]` | Mix volume −1 / +1 dB, from anywhere (`{ }` because macOS Terminal sends `Ctrl+[` as Escape) | same |
| `+` / `-` (empty command line) | zoom in / out | same |
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

### Command grammar

```
[selector] verb [args]
```

Spaces are optional: `2i2` = `2 i 2`, `1-3r`, `2v-6`, `mixv-3`, `bpm92`. A verb letter always
separates the numbers, so nothing is ambiguous. Glued letters only count as an argument for
names and click modes (`2nVox`, `clickrec`), so a typo like `mutr` is rejected rather than read
as `m` + `utr`. `taw --parse "2i2"` shows how a line parses without running it.

Selectors: `1`, `1-3`, `1,4`, `*` (all), `mix`. With no selector, commands act on the
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
| `@17` / `@17.3` / `@1:30` | move cursor to bar 17 / bar 17 beat 3 / 1 min 30 s |
| `mix v -3` | set the mix volume |
| `add` / `add 3` / `add Vox` / `add 2 Gtr` | new tracks; each takes the lowest unused input |
| `add vox1 in2` / `add 2 Gtr in3` | new tracks on given inputs (Gtr 1 on 3, Gtr 2 on 4) |
| `b` / `bounce` | offline-print the mix bus over the time selection (selection required) |
| `b mp3` / `b wav D:\mixes` / `b mp3 "rough 3"` | format, folder (remembered per session), file name |
| `l` / `loop` | loop the time selection; with no selection, toggle the loop |
| `loop on` / `loop off` | |
| `bpm 92` | set tempo |
| `grid` | toggle grid |
| `click` | toggle the click (metronome) |
| `click rec` / `click always` | click only while recording / during playback too |
| `click -12` | click level in dB |
| `dev` | choose audio device |
| `panel` | open the ASIO driver control panel |
| `save`, `q` | save, quit (quitting also saves) |
| `close` | save and close the session, back to the recent sessions / new session screen |

### Rolling keys

| Key | Action |
|---|---|
| `Space` / `Esc` (empty command line) | stop (cursor returns to where play started) |
| `Enter` (empty command line) | back to where play started, still playing |
| `Ctrl+Space` | punch: toggle recording on armed tracks without stopping |
| `Ctrl+K` (or `Ctrl+M`) | drop a marker at the playhead (stopped: at the cursor); `Ctrl+K` because macOS Terminal sends `Ctrl+M` as Enter |
| `Ctrl+C` / `X` / `D`, `Delete` | edit the selection, as when stopped |
| `←` `→` (any modifier) | move the cursor / selection, not the playhead |
| `Ctrl+V` | paste at the cursor |
| `[` / `]` | track volume; `{` `}` or with `Ctrl`, the mix |
| anything else | the command line: `2m`, `3s`, `l` (loop; ignored while recording), `c` (click) |

### Planned (not in the POC)

- Ranges: `@17-25 L` (loop), `2 @17-21 r` (auto-punch with pre-roll), `1-3 @9-13 x` (clear).
- Pre-roll / count-in / metronome as session defaults.
- Takes: keep/discard, take stack, comping.
- Retroactive capture (`R`: keep what was just played while not recording).
- Queries: `1 ?`, `? inputs`.
- User keymaps and macros (config files).
- MIDI tracks, plugin hosting (CLAP/VST3, native plugin windows).
- Headless engine + socket/MCP front end for agents.

## Recording

Auto input, the pro default. `1 r` arms track 1: record-enable plus input monitoring. An armed
track monitors its input while stopped (set levels) and while recording; during plain
playback (`Space`) it plays its clips like any other track. `Ctrl+Space` records the armed
tracks from the cursor; while playing it punches in and out. `2 mon` keeps an armed track's
input in your ears during playback too, to audition a part against the mix (its clips are
silent meanwhile). A track monitors its input or plays its lane, never both. Sample tracks
can't be armed.

## Undo

Every edit is undoable: clips, hits, tracks, names, inputs, volume, mute/solo/arm, markers,
tempo, grid, click, loop. `Ctrl+Z` / `Ctrl+Y`, or `undo` / `redo`. Each command line is one
step named after itself ("undid 2v-6"); a recorded take and a whole import are one step each;
`[` `]` presses on the same target less than two seconds apart merge into one step. Not
undoable on purpose: bounce settings, cursor and
selection. Built on JUCE's `UndoManager` over the state tree; undo is refused while recording.

## Sample tracks

One sample per track; each track reads as its own step-sequencer row. Grouping tracks into a
sample group (a kit) comes later, on top of normal tracks.

```
TRACK { type: "sample", sample, sampleOffset, sampleLength, … }
  HIT { beat, vel }        # beats from the session start, so patterns follow the tempo
```

- Each hit plays the whole sample once at its velocity; overlapping hits ring out (no choke
  yet). Rendering depends only on position, like clips, so loops, seeks and bounces agree.
- `sample <file|folder>` / `add sample <file>`: new sample track(s). `3 sample <file>` gives
  track 3 a new sample and keeps its hits (swap the sound, keep the groove).
- `3 sample` converts an audio track: its latest clip (as trimmed) becomes the sample and every
  clip start becomes a hit, so it sounds the same. `3 audio` converts back. Conversion is
  lossless both ways: each type keeps the other's data.
- Dropped files under 2 s become sample tracks automatically; longer ones stay audio.
- `pat` writes steps over the time selection (repeating), or from the cursor for a bar:
  `X` accent, `x` normal, `o` ghost, `.`/`-` rest; spaces and `|` ignored. Steps are 16ths
  unless `pat /8 …` etc. A pattern replaces the hits in its range.
- `grid 16` (or 4, 8, 32) sets the fine step: `Alt+←/→` move one step (`Ctrl+Alt` four) and the view zooms
  to one column per step. `Alt+/` toggles a hit at the cursor on a focused sample track (at the
  playhead while playing).
- The lane shows each step: a hit's velocity as a shade, an empty step as a faint dot.
- Sample tracks can't be recorded onto.

**Positions between samples.** At tempos like 99 bpm a beat is 26727.27 samples, so grid lines
fall between samples. Snapping, pattern ranges and the position readout treat anything within
half a sample of a grid line as on it.

## Editing

Select a range with Shift+arrows (time) and Shift+↑↓ (tracks); with no track selection the
focused track is used. What moves depends on the track: clips on audio tracks, hits on sample
tracks.

| Key | Command | Does |
|---|---|---|
| `Ctrl+C` | `copy` | copy the range (clips trimmed to it, hits inside it) |
| `Ctrl+X` | `cut` | copy, then clear the range |
| `Ctrl+V` | `paste` | paste at the cursor (also during playback) onto the focused track, extra lanes on the tracks below |
| `Ctrl+D` | `dup` | paste right after the range, then select the copy (press again to keep repeating) |
| `Delete` | `del` | clear the range |
| | `sel 3 5` | select bars 3 to 5 (any `@` position: `sel 1.2 1:30`) |

- Pasted clips layer on top (later wins); pasted hits replace hits on the same step.
- Clips only point into files, so copies cost nothing on disk.
- A lane only pastes onto its own kind of track; mismatches are skipped and reported.
- Each edit is one undo step. Hits are positioned in beats, so they follow tempo changes.
- `Ctrl+C` with no range clears a typed command line; it never quits. Terminals turn `Ctrl+V`
  into pasted text, so `Ctrl+C` also puts `taw:paste` on the Windows clipboard; that text
  arriving is taken as the paste.

## Headless commands

`taw --exec <session> "<command>" …` runs command lines against a session without the screen,
prints each result and a summary, and saves (waiting for imports and bounces to finish). It's
the first way in for scripts and agents. Device-dependent commands (record, input checks)
report what they can't do.

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

## Bouncing

`b` renders the mix bus (track volume, mute/solo, mix volume; never the click or live inputs)
over the time selection, offline on its own thread, faster than real time. `Esc` cancels.

- WAV: 24-bit at the session rate. MP3: 320 kbps CBR through the Media Foundation encoder that
  ships with Windows, so nothing extra to install (needs a 32/44.1/48 kHz session).
- Default folder `bounces/` in the session; default name `<session>-<timestamp>`. Never
  overwrites. Files are written as `name.part.ext` and renamed only when complete.
- Reports length, path and peak; a peak over 0 dBFS is flagged as clipping.
- `taw --bouncetest <dir>` renders a synthetic mix to both formats without a device.

## Startup

With no session given, taw lists recent sessions (newest first, and any sessions in the
launch folder) above the templates. Enter on a recent session opens it; typing a name jumps to
the templates to create a new one. The recent list lives in `%APPDATA%\taw\recent.json`.

## When things go wrong

- **Saves are atomic and constant.** `session.json` is rewritten (temp file + rename) after
  every change, so a crash loses at most the last few milliseconds of edits.
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
- **Plugins (planned)** will run out of process, so a crashing plugin is bypassed, not fatal.

## Screen

```
 title:     taw ▸ session
 interface: device · driver · SR/buffer · latency · xruns · one meter per hardware input
            (meters shrink to fit, so a 16-input interface stays on one line)
 (blank)
 transport: ■ STOP · bars.beats · time · tempo/sig · click · loop   (centred)
 ruler
 tracks:    number · name · arm/mute/solo/mon · input │ timeline lane │ volume · meter · peak dB
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
