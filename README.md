# taw

A terminal audio workstation for engineers and producers: keyboard-first, recording-first,
and driven by a small command language that people and agents share.

Early proof of concept. See [docs/design.md](docs/design.md) for the design and what's planned.

**Platforms:** Windows for now (cmd / Windows Terminal, ASIO). taw will become
cross-platform (macOS and Linux) in future updates; the engine, sessions and command language
are already platform-neutral.

## Build (Windows)

Needs Visual Studio 2022 (C++ workload) and CMake 3.24+. JUCE and the Steinberg ASIO SDK are
downloaded automatically.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The binary is `build\taw_artefacts\Release\taw.exe`.

## Run

From the repo root in cmd, `taw` runs the newest local build (Release or Debug) with
sessions kept in `sessions\`; `taw build` rebuilds Release.

Or run the binary directly:

```
taw                   open the session in this folder, or start a new one
taw my-song           open my-song, or create it
taw my-song -t 3mic   create my-song from the 3mic template
taw --devices         list audio devices
```

On first run taw asks for an audio device, then a template. Device settings and templates
live in `%APPDATA%\taw`. Templates are plain JSON: copy one to make your own.

## Thirty-second tour

```
1 r           arm track 1 (hear input)  space       play / stop
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
- **Cross-platform:** macOS and Linux.

## License

AGPLv3. JUCE is used under the AGPLv3; the ASIO SDK under its GPLv3 option.
