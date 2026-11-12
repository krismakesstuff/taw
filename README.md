# taw

A terminal audio workstation for engineers and producers: keyboard-first, recording-first,
and driven by a small command language that people and agents share.

Early proof of concept. See [docs/design.md](docs/design.md) for the design and what's planned.

**Platforms:** Windows (cmd / Windows Terminal, ASIO) and macOS (Apple Silicon; Terminal,
CoreAudio, Audio Units). Linux will follow; the engine, sessions and command language are
platform-neutral.

## Download

Ready-made builds are on the [Releases](../../releases) page. They aren't code-signed yet, so the
first run takes one extra step.

**Windows (x64):** unzip `taw-<version>-windows-x64.zip` anywhere and run `taw.exe` from cmd or
Windows Terminal, in the folder where you keep your sessions. If SmartScreen says "Windows
protected your PC", choose **More info → Run anyway**. Video works out of the box (FFmpeg's DLLs
come with it).

**macOS (Apple Silicon, macOS 13 or later):** unzip `taw-<version>-macos-arm64.zip`, then clear
the download quarantine once, from Terminal, or macOS will refuse to open it:

```
xattr -dr com.apple.quarantine ~/Downloads/taw-<version>-macos-arm64
~/Downloads/taw-<version>-macos-arm64/taw
```

For MP3 bounces and video, `brew install ffmpeg`. Intel Macs: build from source (below).

Or build it yourself:

## Build (Windows)

Needs Visual Studio 2022 (C++ workload) and CMake 3.24+. JUCE, the Steinberg ASIO SDK and
FFmpeg's libraries (for video, about 100 MB) are downloaded automatically.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The binary is `build\taw_artefacts\Release\taw.exe`.

## Build (macOS)

Needs the Xcode command line tools and CMake 3.24+ (`brew install cmake`). For MP3 bounces,
video, and importing formats macOS can't read (like wma), install ffmpeg: `brew install ffmpeg`;
taw tells you if it's missing. Video is built in only if ffmpeg is there when taw is built.

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
taw                   open the session in this folder (or the one it's inside), or start a new one
taw my-song           open my-song, or create it
taw my-song -t 3mic   create my-song from the 3mic template
taw hub               the audio hub's routing page, no session (macOS)
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

Plug in a MIDI controller and type `inst` (or `inst vital`); on an instrument track the same swaps
its plugin, and `add inst vital` always makes a new track. The first time, taw scans your VST3
plugins, each in a separate process, so one that crashes or hangs is just skipped. The new instrument track is
armed, so the controller plays it straight away; `ctrl+space` records what you play as notes,
`|` (shift+`\`) opens and closes the plugin's window, even while playing, `f` fills the view with the focused track (a piano roll on
instrument tracks), and `snap` quantizes. Every MIDI input is opened and armed instrument
tracks hear all of them; `midi` shows them, switches ones you don't use off, and shows what
each is sending. Plugin settings are saved next to the session in `plugins/`.

To edit notes, double-click a MIDI clip (or `shift+E` on the track): the MIDI editor opens in a
window over the terminal, for the mouse. Drag notes to move them (alt+drag copies them, ctrl+c
and ctrl+v copy and paste at the cursor), drag an edge to change the length, double-click to draw one, right-click to delete; the lane below sets velocity or draws a
controller. `edit new` makes an empty clip to draw into. Audio clips open the same way in the
audio editor, for placing the selection and clip edges down to the sample.

Plugins run inside taw for now, so a plugin that crashes takes taw with it (the session is
saved on the way down). Running them in their own process is planned.

## Samples

taw keeps an index of your sample folders: it measures every sound once, works out what kind
of drum it is, and lays them all out on a map where sounds that are alike sit together.

**Add your folders.** `index D:\Samples\Drums` (or type `index` and drop a folder on the
terminal) measures every WAV, AIFF, FLAC, MP3 and OGG under it, on all cores, while you keep
working; the message line counts them and says when it's done. Your folders are only read:
nothing is written next to your samples, and the index lives in taw's config folder. Indexing a
folder again only measures what's new or changed. `library` lists the folders and how many
samples each holds, and `index remove <folder>` takes one out.

**What it measures.** For each sample: its length, attack, decay and tail, loudness and peak,
punch (how far the peak stands above the body), brightness, how noisy or tonal it is, how much
low end it has, its pitch when there's a clear one, and how many hits are in it (a clap has a
few). The kind (kick, snare, clap, rim, hihat, open hat, cymbal, tom, perc) comes from the file
and folder names first ("BD_808.wav", "Kicks/thump.wav"). When the name doesn't say, taw guesses
from the sound: the kind of the named samples that sound most like it, shown as
"(by its sound)". When indexing finishes it tells you how often the sound alone agreed with the
names, which is how far to trust those guesses in your library.

**Browse.** `ctrl+B` opens the browser, a window over the terminal you can work with the keys or
the mouse; `?` in it shows all its keys.

- The **tabs** across the top are the kinds, with how many of each you have.
- The **map** shows that kind's samples, one dot each. Dots close together sound alike, so
  once you find a kick that's nearly right, the ones around it are its cousins. Arrows move
  to the nearest dot that way and every move plays it; drag across the map to hear whatever is
  under the pointer. The two menus in the toolbar (or `alt+arrows`) turn the map into a chart
  of two measurements instead: brightness across and decay up, say, so short dark kicks sit
  bottom left.
- The **list** on the right has the same samples by name; the **status line** under them shows
  the chosen one's numbers.
- **Typing filters** both: words match file and folder names (`808`, `vinyl`), and measurement
  terms match the sound: `dec<200` (decay under 200 ms), `bright>1k`, `len<0.5s`, `pitch>60`,
  `loud>-20`, `punch>15`, `noise<0.3`, `low>-10`. Filter to what you want, then explore the
  smaller map. `Esc` clears the filter.
- **like this** shows the 40 samples that sound most like the chosen one, nearest first.
- **Auditions** play at the browser's own level (`shift+↑↓`, or the slider). While the song
  plays, each one waits for the next grid step, so you hear it in the groove.
- **Enter** puts the sample on the track you're on if it's a sample track (it keeps its hits,
  so with a loop playing you hear each candidate in the pattern), or makes a new sample track;
  `shift+Enter` always makes a new one. On a kit it goes on the lit pad (below).

From a shell, `taw --library find kind=kick "decay<200" sort=decay` and
`taw --library like <name>` search the same index (quote terms with `<` or `>`, or the shell
takes them as redirects), and `taw --index <folder>` builds it without opening taw.

## Kits

A sample track plays one sample; a kit gathers sample tracks onto a bus, each on a pad, so you
can play, mix and move them as one while each sound keeps its own track, hits and inserts.
`kit` makes one, `pad 3 <file>` puts a sample on pad 3, and `3 kit drums` moves track 3 into
kit Drums. Arm the kit (`shift+R`) and play it from a pad controller, or `ctrl+k` turns the
keyboard into a 4×4 pad grid laid out like an MPC's (`z x c v` up to `1 2 3 4`).
`ctrl+space` records what you play as hits. `2.3 choke 1` puts pads in a choke group, so the
closed hat cuts off the open one. In the sample browser (`ctrl+B`), what you audition goes onto
the lit pad.

## Effects, buses and sends

Every track has five insert slots for effect plugins and five sends. `bus Verb` adds a bus
track; `2 send verb` sends track 2 to it (post-fader at -6 dB; `2 send a -12 pre` sets one up),
and naming a bus that doesn't exist yet creates it, then opens the effects picker for it, so you
can search for its reverb straight away. `3 ins valhalla` puts an effect on track 3
(on a bus, that's your reverb return); `2 out drums` sends a whole track into a bus. The mix
bus has five inserts too, after its fader: `mix ins limiter`, or `\` on the Mix row. Taking one
out reads the same for an insert or a send: `3 ins b x` (or by name, `3 ins valhalla x`),
`2 send verb x`, or `del` on it in the routing column.

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

For scoring to picture: `video D:\cuts\reel1.mp4` (or type `video` and drop the file on the
terminal) opens the video in a window of its own that
follows the playhead: playing, stopped on the cursor, jumping, looping. It's a normal window
(put it on another screen, maximise it) that never takes the keyboard from taw. The video
starts at the session start; `video at 5` (or `video at 1:30`, or `video at -0:05` to skip a
slate) moves it. `video offset 2` shows the picture two frames late, for a screen that lags;
`video` shows or hides the window; `video remove` takes it out. The picture allows for what the
audio device and plugin delay compensation add, so it matches what you hear. `bv` bounces the
selection (or, with nothing selected, the whole video) as an MP4: the picture, frame for frame,
with the mix. The video gets a row under the ruler showing its length and its own timecode: up
from the first track gets you there, where the arrows step a frame (ctrl a second; hold one to
scrub) and clicking or dragging along it scrubs the picture. On macOS video needs Homebrew's
ffmpeg (`brew install ffmpeg`) when taw is built.

## Agents

taw is also an MCP server, so an AI agent (Claude Code, or any MCP client) can work in the
session you have open, live, while you watch and play. Register it once:

```
claude mcp add --scope user taw -- <path to>/taw --mcp
```

Building taw yourself on Windows? Register `build\taw_artefacts\Release\taw-mcp.exe --mcp`
instead: each build copies taw.exe to taw-mcp.exe, so agent sessions left open never hold the
taw.exe the next build has to replace.

Then ask: "add a bass track with a two-bar line under the chorus", "make a filter sweep on the
synth into bar 17", "which tracks have no output?". The agent reads the session (tracks, clips,
notes, hits, markers, automation and plugin parameters) and edits it: tracks, MIDI notes, hits,
markers, automation and any command you could type. It can list, make and open sessions too.

It works around you, not over you:

- **Each edit is one undo step**, named for the agent, so `ctrl+z` takes it back.
- **Nothing while playing.** An agent's edits wait until you stop, and the transport shows
  that one is waiting.
- **Hands off what you're working on:** the track you're on, your selection and what you just
  changed, until you've been away from the keys for 10 seconds.
- **You see it happen:** what an agent changes flashes violet, and the title line shows who's
  connected and what their last edit did. A tempo change, or switching sessions, asks you first.
- **Agents never save.** `ctrl+s` is yours.

`agents off` shuts them out (`agents on` lets them back in); the setting is kept for next time.

## Roadmap

Next up, roughly in order:

- **Plugins out of process and CLAP:** each plugin in its own process, so a crashing plugin is
  bypassed instead of taking the session down; CLAP alongside VST3.
- **Linux.**
- **Intel Macs.**

## Contributing

Issues are welcome: bugs, reports from your setup, and ideas. taw doesn't accept pull requests
yet; see [CONTRIBUTING.md](CONTRIBUTING.md). Everyone follows the
[Code of Conduct](CODE_OF_CONDUCT.md).
Security problems: [SECURITY.md](SECURITY.md). What's changed in each release is in
[CHANGELOG.md](CHANGELOG.md).

## License

AGPLv3 ([LICENSE](LICENSE)). JUCE is used under the AGPLv3; the ASIO SDK under its GPLv3 option.
FFmpeg (a GPL build, shipped as DLLs next to taw.exe with its licence, its source attached to
each release) under the GPL. Details in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

"taw" is the name of this project. The licence covers the code, not the name: a fork or a
modified build you share needs a name of its own and shouldn't suggest it's the official taw.
