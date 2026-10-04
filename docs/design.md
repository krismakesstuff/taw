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
| MIDI input | Device callbacks | One lock-free queue per port into the audio thread |

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
| `k` (empty command line) | to the markers row (ctrl+k adds a marker); `k` again returns to the track | same |
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

The keyboard does everything; the mouse is a shortcut. Windows only so far (the console reports
clicks by cell).

- Click a track's lane: that track gets the focus and the cursor goes there (as in Pro Tools).
  Shift-click stretches the time and track selections to it.
- Double-click a clip: selects it and opens it in its editor, MIDI or audio (shift+E, or
  `edit`, from the keyboard).
- `taw --exec` takes `:click X Y` and `:dblclick X Y` (cells of its 160×44 screen).

### The editors

**Shift+E** (or `edit`, `9 edit`) opens the focused track in a zoomed editor window: the MIDI
editor on an instrument track, the audio editor on an audio track, on its clip at the cursor or
else the nearest one. Shift+E again, or the close button, closes it; one is open at a time.
`edit new` makes an empty MIDI clip to draw into (over the time selection, else the bar the
cursor is in). Both sit over the terminal like the plugin windows, keep the keyboard while
they're in front (Esc hands it back) and pass on the keys they don't use. Windows only so far.

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
  changes it. Ctrl+wheel (or `+` `-`, around the cursor) zooms; the wheel scrolls.

### The MIDI editor

One MIDI clip, for fine work with the mouse.

- **Shared with taw:** it edits the session itself, one undo step per gesture, so ctrl+z (from
  either side) and the timeline follow. The cursor and playhead are taw's: clicking the ruler
  moves the cursor; the view follows the playhead while playing.
- **Notes:** drag to move (snapped to the editor's grid; alt: free), drag an edge to change the
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
  (ctrl: a bar), ctrl+A select all, ctrl+D duplicate after the selection, `q` quantize, `+` `-`
  zoom, Esc clears the selection, then hands the keys back to taw. Everything else goes to taw:
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
| `2 remove` / `2-4 remove` | delete tracks (`rm` too); a bus's feeds go back to the mix and sends to it go; ctrl+z brings it all back |
| `@17` / `@17.3` / `@1:30` | move cursor to bar 17 / bar 17 beat 3 / 1 min 30 s |
| `mix v -3` | set the mix volume |
| `add` / `add 3` / `add Vox` / `add 2 Gtr` | new tracks; each takes the lowest unused input |
| `add vox1 in2` / `add 2 Gtr in3` | new tracks on given inputs (Gtr 1 on 3, Gtr 2 on 4) |
| `b` / `bounce` | offline-print the mix bus over the time selection (selection required) |
| `b mp3` / `b wav D:\mixes` / `b mp3 "rough 3"` | format, folder (remembered per session), file name |
| `l` / `loop` | loop the time selection; with no selection, toggle the loop |
| `loop on` / `loop off` | |
| `bpm 92` | set tempo; with `bpm ` typed, `t` taps it (the average of the last 8 taps, a 2 s pause starts over) and enter sets it |
| `grid` | toggle grid |
| `click` | toggle the click (metronome) |
| `click rec` / `click always` | click only while recording / during playback too |
| `click -12` | click level in dB |
| `lowlat` / `lowlat 256` | low latency mode on/off / its limit in samples (plugins over it sit out) |
| `count 4` / `count off` | count-in: clicks before recording starts (default 4; also in `project`) |
| `count always` / `count rec` | count in on plain playback too / only when recording starts (default) |
| `height 3` | rows per track, 1-4 (default 2); saved with the session |
| `dark` / `light` / `grey` | colour theme, for every session (saved in `settings.json`) |
| `dev` | choose audio device |
| `panel` | open the ASIO driver control panel |
| `save` (or `Ctrl+S`), `close` (or `Ctrl+W`), `exit` (or `Ctrl+Q`) | save; save and go back to the sessions; save and quit (`q` alone is a view key) |
| `close` | save and close the session, back to the recent sessions / new session screen |

### Rolling keys

| Key | Action |
|---|---|
| `Space` / `Esc` (empty command line) | stop (cursor returns to where play started) |
| `Enter` (empty command line) | back to where play started, still playing |
| `Ctrl+Space` | punch: toggle recording on armed tracks without stopping (in, out, in again…) |
| `Ctrl+K` (or `Ctrl+M`) | drop a marker at the playhead (stopped: at the cursor); `Ctrl+K` because macOS Terminal sends `Ctrl+M` as Enter |
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
- Headless engine + socket/MCP front end for agents.

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
  unless `pat /8 …` etc. A pattern replaces the hits in its range. `~58` (or `swing58`)
  anywhere in it swings: every second step lands late, 50 straight to 75 (66 is a triplet feel),
  the MPC way, so `pat /16 ~58 …` swings the 16ths.
- `grid 16` (or 4, 8, 32) sets the fine step: `Alt+←/→` move one step (`Ctrl+Alt` four) and the view zooms
  to one column per step. `Alt+/` toggles a hit at the cursor on a focused sample track (at the
  playhead while playing).
- The lane shows each step: a hit's velocity as a shade, an empty step as a faint dot.
- Sample tracks can't be recorded onto.

**Positions between samples.** At tempos like 99 bpm a beat is 26727.27 samples, so grid lines
fall between samples. Snapping, pattern ranges and the position readout treat anything within
half a sample of a grid line as on it.

## Instrument tracks

A VST3 instrument per track, played from a MIDI controller and recorded as notes.

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
  name match. Either makes a new instrument track, armed, so the controller plays it at once.
  `3 inst vital` puts it on track 3 (an empty or instrument track; the notes stay).
- `scan` looks for new or changed plugins in the VST3 folders (opening the picker the first
  time starts it). Finding out what a plugin is means loading it, and some crash or hang when
  loaded cold, so each file is looked at in a child process (`taw --scan-plugin`, four at a
  time, 60 s each): one that crashes or hangs only takes its child down and is skipped from
  then on; `scan retry` tries those again. The list is cached in `plugins.xml` in the config
  folder, saved after every file.
- `|` (shift+`\`) opens the plugin under the cursor in its own window (JUCE's generic sliders
  if it has none), kept above the terminal; `|` again or its close button closes it. It works
  while playing, so enter keeps its one job (back to the start). On a track that's its
  instrument, else its first insert, else the effects picker; in the routing column, the slot
  under the cursor. `ed` is the typed version.
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
  timeline started; any jump (stop, seek, loop wrap) ends them, so nothing hangs. Notes that
  started before the play position aren't chased (yet). Plugins get the tempo, time signature
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
| `2 fx valhalla` | an effect in the first free insert (`2 fx` alone: the effects picker) |
| `2 fx b valhalla` | into insert B (replacing what's there) |
| `2 fx b` | open or close insert B's window (empty: the picker for B) |
| `2 fx b off` / `on` / `x` / `2 fx b>d` | bypass, back on, remove, swap B and D |
| `bus Verb` / `add bus Verb` | new bus track (no input, no clips, nothing to arm) |
| `2 send verb` | a send to Verb: the slot already going there, else the first free one; a name no bus has makes the bus |
| `2 send b verb` / `2 send b 6` | slot B to Verb / to bus track 6 |
| `2 send b -12` / `pre` / `post` / `off` / `on` / `x` | level (a sign or 0: `-12`, `+3`, `0`, `-inf`), before or after the fader, off, remove |
| `2 send` | list track 2's inserts, output and sends |
| `2 out drums` / `2 > drums` / `2 out mix` | where the track goes after its fader; `2 out` alone opens the picker |
| `\` | into the routing column on the focused track (see below) |
| `io` | show or hide the routing column (on by default; kept in `settings.json`) |

`fx` starts with a key that's a hotkey on an empty line (`f`), so typed it needs a track
number (`2 fx`); `ins` works on the focused track, as `inst` does.

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
  you click the terminal, close it, or double-click the title again. Windows only. Loading an effect opens its window. One window at a time: opening
  another closes the first, and while one is open it follows you as you move between inserts.
- **Signal flow.** Source (clips, hits, instrument, input) → inserts A to E → pre-fader sends →
  fader → post-fader sends → output. Bypassed inserts are skipped. Effects run while stopped
  too, so tails ring out and monitored inputs go through them. New sends are post-fader at
  0 dB. Mute silences a track's sends too. A mono track enters a bus at the pan law.
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
focused track is used. What moves depends on the track: clips on audio tracks, hits on sample
tracks, MIDI clips on instrument tracks.

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
prints each result and a summary, and saves (waiting for imports, bounces and plugin scans to
finish). `TAW_CONFIG=<folder>` uses another config folder (templates, device, plugin list). It's
the first way in for scripts and agents. Device-dependent commands (record, input checks)
report what they can't do. For testing the screen, `:key o` (or `:key right`, `:key enter`,
`:key ]`, …) presses a key and `:screen` prints the screen as plain text (160×44).

`taw --new <folder>` makes an empty session for `--exec` to build in.

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

## The sample library

For finding the right sample among thousands: taw measures each one once, sorts them by kind
and lays them out on a map where samples that sound alike sit together. `index <folder>` (on its
own thread; `taw --index <folder> ...` from a shell prints a report) adds folders.

**The browser** docks in a column right of the timeline and its meters (about two fifths of the
width, 56 to 100 columns; not on consoles under 140), from the transport row down to the input
line, which stays full width; the session stays in view while you look. In the column the map
sits on top, the list under it, the badges wrap. `ctrl+B` shows or hides it (remembered); `shift+B` takes the
keys into it and `Esc` gives them back (clearing the filter first). Inside:

- **Selectors** at the very top, `↑` from the badges, `←→` (or a click) to change:
  - **x** and **y**: what the map's axes show. `similar` (the default) is the sound-alike map
    below, both axes at once, where only nearness means anything. Otherwise two measurements:
    brightness, attack, decay, tail, length, pitch (unpitched at the bottom), loudness (RMS),
    punch (crest), noise (flatness), low end (energy under 120 Hz). Each is spread by rank within
    the kind shown, so the map fills evenly; the row shows the real range ("49 Hz → 335 Hz"), the
    rule under the map names the axes. The two are never the same; `y` is skipped on the
    sound-alike map.
  - **colours**: by kind, or by folder (each folder one of a dozen colours from its name), on
    the map and the list's dots alike, to see which pack a sound comes from.
  - All three are kept in settings.json.
- **Badges**, one per kind with its count, in the kind's colour: `←→` picks one,
  and the map and list show only that kind. `↓` goes to the map; `↑` past the top of the map or
  list comes back.
- **The map**: each kind's samples on a map of their own (so 187 kicks spread over the whole
  area, not their corner of the full one), drawn in half blocks, two near-square pixels a cell,
  each in the colour most of its samples have. The arrows go to the nearest sample within 60° of
  that direction; a click picks the nearest one. The chosen one is a ring with its name.
- **The list** (under the map), `Tab` from the map: the same samples kind by kind, then by name, each with its
  kind's dot (hollow when the sound, not the name, gave the kind) and length. `↑↓`, page keys,
  Home/End.
- **Typing** filters both by file and folder name. The last row shows the chosen sample's
  numbers: kind, length, attack, decay, pitch (when it's clear), brightness, RMS, folder.
- **Every move auditions**: the file, resampled to the device rate, straight to the outputs,
  stopped or rolling (not metered, never bounced), at the browser's own level (`shift+↑↓`, `browse
  vol -6`, saved). `Space` plays it again; `shift+Space` is the transport.
- **Enter** puts it on a track: on a focused sample track it replaces the sample and keeps the
  hits, so with the loop playing you hear each candidate in the pattern; anywhere else it makes a
  new sample track. The browser stays open for the next.

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
Opening a session loads its audio a few files per frame, with a box saying what's loading and
how far along it is; keys wait until it's open. Plugins come after, once the session is on
screen: one a frame (a slow one, like a Roland Cloud instrument, still holds its frame), most
wanted first: an armed instrument, other instruments, then inserts that are on. A bypassed
insert isn't loaded until it's switched on or its window is opened. Until its plugin is in, a
track is silent and shows dim (`loading…` under an instrument's name); the transport bar
counts what's left. Opening a plugin's window, or bounce, loads what it needs at once. Closing
(`close`, or quitting with a session open) does the same in reverse: each plugin's state is
saved and the plugin unloaded under a closing box.

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
- **Plugins** run in process for now: one that crashes takes taw down (the session is saved
  first). Scanning is already out of process: a plugin that crashes or hangs there is just
  skipped. Playing them out of process is planned.

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
