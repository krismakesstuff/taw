# taw

A terminal audio workstation for engineers and producers: keyboard-first, recording-first,
and driven by a small command language that people and agents share.

Early proof of concept. See [docs/design.md](docs/design.md) for the design and what's planned.

**Platforms:** Windows (cmd / Windows Terminal, ASIO) and macOS (Terminal, CoreAudio; in
progress). Linux will follow; the engine, sessions and command language are platform-neutral.

## Build (Windows)

Needs Visual Studio 2022 (C++ workload) and CMake 3.24+. JUCE and the Steinberg ASIO SDK are
downloaded automatically.

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
```

## Roadmap

Next up, roughly in order:

- **Sample groups:** gather sample tracks into a kit you can program, mix and move as one,
  while each sound keeps its own track.
- **MIDI keyboard recording:** play parts in from a MIDI controller.
- **Plugin hosting (VST3/CLAP):** each plugin runs in its own process, so a crashing plugin is
  bypassed instead of taking the session down.
- **Cross-platform:** macOS (in progress), then Linux.

## License

AGPLv3. JUCE is used under the AGPLv3; the ASIO SDK under its GPLv3 option.
