# Contributing

Thanks for looking at taw. It's an early proof of concept with one maintainer, Kris Crawford,
who started it and decides where it goes. The design ([docs/design.md](docs/design.md)) is the
best map of how it works and why.

## Issues, not pull requests (for now)

taw doesn't accept pull requests yet; they're closed without review. What helps most:

- **Bug reports**, with the steps that show the bug.
- **Reports from your setup:** terminals, audio interfaces, plugins and operating systems taw
  hasn't been tried on.
- **Ideas** for features, commands and keys: say what you're trying to do, not just the
  solution.
- **Documentation** that's wrong or unclear.

Use [GitHub Issues](../../issues): the bug report form asks for what's needed (`taw --version`,
your OS, terminal and audio device, and the steps). Questions are issues too. Response times
vary, and not every idea will be taken up.

Security problems: don't open an issue; see [SECURITY.md](SECURITY.md).

## Building it yourself

taw is AGPLv3: you're free to build it, change it and share your changes under the same licence
([LICENSE](LICENSE)). Forks need a name of their own (see the README's License section).

See the [README](README.md) for the build: Visual Studio 2022 and CMake on Windows, the Xcode
command line tools and CMake on macOS. The self-tests that need no audio device:

```
taw --keycasttest
taw --counttest
taw --livetest
taw --cliptest
taw --paralleltest
taw --bouncetest out        (MP3 needs ffmpeg)
taw --keytest               (macOS)
```

## Releases

Releases are made by the maintainer from the `publish` branch: see
[docs/releasing.md](docs/releasing.md).

## Code of conduct

Everyone taking part follows the [Code of Conduct](CODE_OF_CONDUCT.md).
