# taw

A terminal audio workstation for engineers and producers: keyboard-first, recording-first,
and driven by a small command language that people and agents share.

Early proof of concept. See [docs/design.md](docs/design.md) for the design and what's planned.

**Platforms:** Windows (cmd / Windows Terminal, ASIO) and macOS (Terminal, CoreAudio; in
progress). Linux will follow; the engine, sessions and command language are platform-neutral.

## Build (Windows)

Needs Visual Studio 2022 (C++ workload) and CMake 3.24+. JUCE, the Steinberg ASIO SDK and
FFmpeg's libraries (for video, about 100 MB) are downloaded automatically.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The binary is `build\taw_artefacts\Release\taw.exe`.

## Build (macOS)

Needs the Xcode command line tools and CMake 3.24+ (`brew install cmake`). For MP3 bounces
(and importing formats macOS can't read, like wma) install ffmpeg: `brew install ffmpeg`;
taw tells you if it's missing.

```
./taw build
```

The binary is `build/taw_artefacts/Release/taw`.

### Terminal setup (macOS)

Keys keep the same physical spots as on Windows where the terminal allows it (Option is
taw's Alt). Out of the box, Terminal.app keeps Shift+↑ ↓, Home/End (fn+← →) and
Ctrl+Option+← → for itself and types symbols with Option. The first time taw runs in
Terminal.app it offers to add a **"taw" Terminal profile** that fixes this; your own profiles
aren't changed. Said no? `taw --terminal-profile` adds it later.

Two things are up to you:

- **System Settings → Keyboard → Keyboard Shortcuts → Mission Control**: turn off
  "Move left/right a space" so Ctrl+← → reach taw.
- taw needs microphone access: macOS asks for it on behalf of your terminal app.

Terminal.app sends Ctrl+M as Enter and Ctrl+[ as Escape, so taw has twins for them that work
everywhere: **Ctrl+K** adds a marker and **{ }** (Shift+[ ]) ride the mix. iTerm2, Ghostty,
kitty and WezTerm report Ctrl+M and Ctrl+[ properly (taw asks for that) and need no profile.

## Run

From the repo root in cmd, `taw` runs the newest local build (Release or Debug) with
sessions kept in `sessions\`; `taw build` rebuilds Release. On macOS the same is `./taw` and
`./taw build`.

Or run the binary directly:

```
taw                   open the session in this folder, or start a new one
taw my-song           open my-song, or create it
taw my-song -t 3mic   create my-song from the 3mic template
taw --devices         list audio devices
```

On first run taw asks for an audio device, then a template. Device settings and templates
live in `%APPDATA%\taw` (macOS: `~/Library/Application Support/taw`). Templates are plain
JSON: copy one to make your own.

## Thirty-second tour

```
shift+R       arm track (hear input)    space       play / stop
r t           zoom out / in             tab         next clip edge (shift: select)
ctrl+space    record the armed tracks   ← →         move a bar (ctrl: 4, alt: a step)
2i3           track 2 <- input 3        shift ← →   select a range
2v-6          track 2 at -6 dB          ctrl c / v  copy / paste the range
add Vox       new track                 ?           all commands
shift+T       track menu (rename, delete...)
```

## Instruments

Plug in a MIDI controller and type `inst` (or `inst vital`). The first time, taw scans your VST3
plugins, each in a separate process, so one that crashes or hangs is just skipped. The new instrument track is
armed, so the controller plays it straight away; `ctrl+space` records what you play as notes,
`|` (shift+`\`) opens and closes the plugin's window, even while playing, `f` fills the view with the focused track (a piano roll on
instrument tracks), and `snap` quantizes. Every MIDI input is opened and armed instrument
tracks hear all of them; `midi` shows them, switches ones you don't use off, and shows what
each is sending. Plugin settings are saved next to the session in `plugins/`.

Plugins run inside taw for now, so a plugin that crashes takes taw with it (the session is
saved on the way down). Running them in their own process is planned.

## Effects, buses and sends

Every track has five insert slots for effect plugins and five sends. `bus Verb` adds a bus
track; `2 send verb` sends track 2 to it (post-fader at 0 dB; `2 send a -12 pre` sets one up),
and naming a bus that doesn't exist yet creates it. `3 fx valhalla` puts an effect on track 3
(on a bus, that's your reverb return); `2 out drums` sends a whole track into a bus.

The routing column sits left of the timeline, as in Pro Tools: inserts on a track's first row,
sends and the output on its second (the R I S M flags sit under the track's name, and the
meter is on the right). `\` steps into it: arrows pick a slot (up and down go row by row), `|` opens an
effect's window (or picks an effect or a bus for an empty slot), `M` bypasses, shift+arrows
reorder effects, `[ ]` set a send's level, `del` removes. An open plugin window follows you
from slot to slot. Soloing a track keeps its reverb; soloing a bus plays just what's on it.
`ins` (like `inst`) opens a picker of the effects that fit the focused track. Plugin latency is
compensated, so tracks stay in time through lookahead limiters and linear-phase EQs; what you
monitor skips the compensation, and `lowlat` sits out the high-latency plugins while you track.

Tracks are mono or stereo (`2 stereo`, `add Keys stereo`, `2 i 3-4` records a pair) and pan
with `2 pan l30` or alt+[ ].

## Video

For scoring to picture: `video D:\cuts\reel1.mp4` opens the video in a window of its own that
follows the playhead: playing, stopped on the cursor, jumping, looping. It's a normal window
(put it on another screen, maximise it) that never takes the keyboard from taw. The video
starts at the session start; `video at 5` (or `video at 1:30`, or `video at -0:05` to skip a
slate) moves it. `video offset 2` shows the picture two frames late, for a screen that lags;
`video` shows or hides the window; `video remove` takes it out. The picture allows for what the
audio device and plugin delay compensation add, so it matches what you hear. Windows only for
now.

## Roadmap

Next up, roughly in order:

- **Sample groups:** gather sample tracks into a kit you can program, mix and move as one,
  while each sound keeps its own track.
- **Note editing:** change single notes in the piano roll (recording comes first; this follows).
- **Plugins out of process and CLAP:** each plugin in its own process, so a crashing plugin is
  bypassed instead of taking the session down; CLAP alongside VST3.
- **Cross-platform:** macOS (in progress), then Linux.

## License

AGPLv3. JUCE is used under the AGPLv3; the ASIO SDK under its GPLv3 option. FFmpeg (a GPL
build, shipped as DLLs next to taw.exe with its licence) under the GPL.
