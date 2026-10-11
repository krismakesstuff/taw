# Contributing

Thanks for looking at taw. It's an early proof of concept with one maintainer, Kris Crawford,
who started it and decides what's merged. The design ([docs/design.md](docs/design.md)) is the
best map of how it works and why.

Please read this whole page before you put time into a change: taw's licensing has a
commercial side, and you should know that going in.

## What's welcome

- **Bug fixes**, with the steps that show the bug.
- **Reports from your setup:** terminals, audio interfaces, plugins and operating systems taw
  hasn't been tried on.
- **Documentation** fixes and clarifications.
- **Linux groundwork:** the engine, sessions and command language are platform-neutral; the
  terminal and audio device code is what's missing.

**New features, new commands, and changes to keys: open an issue first** and wait for a yes
before you build it. taw has a strong point of view (keyboard first, a terse command language
shared by people and agents), and an issue saves you building something that won't be merged.

Response times vary. Not every issue or pull request will be answered quickly, or accepted.

## taw's commercial future

taw is open source under the AGPLv3, and the code in this repository stays available under it.
But a commercial edition of taw, or paid upgrades for some future features, may exist one day.
So that's possible with code from contributors as well as the maintainer's own, **every
contributor signs the [Contributor License Agreement](CLA.md)** before their first pull request
is merged. In short, it says:

- you keep the copyright to your contribution, and can use it however you like;
- you give the maintainer the right to use, change and distribute it under any licence,
  including commercial ones (that's what makes a paid edition possible);
- you have the right to contribute it (it's yours, or your employer agrees).

Signing takes one comment: the CLA bot asks on your first pull request and tells you what to
write. Read [CLA.md](CLA.md) itself; this summary doesn't replace it.

**Revenue:** contributing doesn't entitle you to payment or a share of any revenue. If taw ever
makes money, the maintainer may choose to share some of it with contributors, but whether to,
with whom and how much is at the maintainer's sole discretion, and nothing here is a promise that it will
happen.

## AI-assisted contributions

You may use AI tools to write code, tests or docs, on these terms:

- **A person is the author.** Every pull request is opened by, and every commit is authored by,
  a real person who has read, understood and tested the change. Credit the tool as a co-author
  if you like (a `Co-authored-by:` line), but not as the only author.
- **That person is responsible for it.** The CLA and the [Code of Conduct](CODE_OF_CONDUCT.md)
  apply to you, the person, for everything you submit, however it was written.
- **Say so.** If a change was largely generated, say which parts in the pull request.

Pull requests opened by bots or agents with no person behind them are closed.

## Building

See the [README](README.md): Visual Studio 2022 and CMake on Windows, the Xcode command line
tools and CMake on macOS.

## Before a pull request

- **Build Release and run the self-tests** that need no audio device:

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
- **Try it by hand** in taw itself, and say in the pull request what you tried it on (platform,
  terminal, audio device).
- **Match the code around you:** its naming, its spacing (`f (x)`), its comments (short and
  plain, saying why). New behaviour belongs in docs/design.md too.
- **One change per pull request**, linked to its issue.

## Reporting bugs and asking questions

Use [GitHub Issues](../../issues): the bug report form asks for what's needed (`taw --version`,
your OS, terminal and audio device, and the steps). Questions are issues too.

Security problems: don't open an issue; see [SECURITY.md](SECURITY.md).

## Releases

Releases are made by the maintainer from the `publish` branch: see
[docs/releasing.md](docs/releasing.md). Pull requests go to `main`.

## Code of conduct

Everyone taking part follows the [Code of Conduct](CODE_OF_CONDUCT.md).
