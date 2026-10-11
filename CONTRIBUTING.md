# Contributing

Thanks for looking at taw. It's an early proof of concept, so things move fast and the design
([docs/design.md](docs/design.md)) is the best map of how it works and why.

## Building

See the README: Visual Studio 2022 and CMake on Windows, the Xcode command line tools and CMake
on macOS. Linux isn't supported yet (the engine, sessions and command language are
platform-neutral; the terminal and audio device code is what's missing).

## Before a pull request

- Build Release and run the self-tests that need no audio device:

  ```
  taw --keycasttest
  taw --counttest
  taw --livetest
  taw --cliptest
  taw --paralleltest
  taw --bouncetest out        (MP3 needs ffmpeg)
  taw --keytest               (macOS)
  ```

  CI runs these on Windows and macOS for every pull request.
- Match the code around you: its naming, its comment style (short, plain comments that say why),
  and its idioms. New behaviour belongs in docs/design.md too.
- Keep a pull request to one change, and say in it what you tried by hand (which platform,
  which terminal, which audio device).

## Licence

taw is AGPLv3. By contributing you agree that your contribution is licensed under the AGPLv3 as
well.
