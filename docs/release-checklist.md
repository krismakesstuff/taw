# First release checklist

What's left before taw goes public and ships its first downloads. Tick items off as they're done.

## Decide

- [x] **Version number:** the first public release is **0.2.0**. The `v0.1.0` tag ("first
      playable milestone", 30 Sep 2026) stays as history.
- [x] **Commit email:** no history rewrite. Older commits keep the work address; from now on this
      repository commits as `Kris Crawford <11crawfordk@gmail.com>` (set in the repo's own git
      config, so other repositories keep their address).

- [x] **Contributions:** issues only, no outside pull requests for now, so no CLA. Releases may
      be paid downloads of the same AGPL code; no closed edition. Bring a CLA back (and have a
      lawyer read it) before taking pull requests.

## Before the repository goes public

- [ ] **Trademark:** look into registering "taw" (the README asks forks for their own name).
- [x] **Contact email** in [CODE_OF_CONDUCT.md](../CODE_OF_CONDUCT.md): 11crawfordk@gmail.com.
- [x] **Make the repository public:** Settings → General → Danger Zone → Change visibility.

## GitHub settings

- [x] **Private vulnerability reporting:** Settings → Code security → Private vulnerability
      reporting ([SECURITY.md](../SECURITY.md) points there).
- [x] **Publish branch:** once the release setup is merged, `git push origin main:publish`.
- [x] **Protect `main` and `publish`:** the ruleset "main and publish" (Settings → Rules): no
      force pushes, no deletion, for anyone. Only collaborators can push, and there are none.

## First release

See [releasing.md](releasing.md) for the steps.

- [x] Merge the release setup (CI, release workflow, contributing files) into `main`.
- [x] **Dry run:** Actions → Release → Run workflow on `main`. It's the first real test of the
      Windows and macOS builds. macOS minutes are expensive on a private repository and free on a
      public one.
- [x] Fix whatever the dry run turns up.
- [x] **Try the zips on real machines:**
  - [x] Windows: unzip, run from a terminal, SmartScreen's "Run anyway", video plays.
  - [x] macOS: the `xattr` step from the README, then taw runs.
  - [x] `taw --install-hub` and the driver install work from the unzipped folder, not the build folder.
  - [x] macOS video works with Homebrew's FFmpeg (it has to be the same major version the release
        was built against).
  - [x] Keycast: clicking or dragging its window leaves the keyboard with the terminal, and
        `taw --keycasttest` passes.
- [x] Date the version's heading in [CHANGELOG.md](../CHANGELOG.md).
- [x] Push `publish`, look over the draft release, publish it.

## Later

- [ ] **macOS signing and notarization** (Apple Developer account, $99/yr): removes the `xattr`
      step.
- [ ] **Windows signing** (Azure Trusted Signing, about $10/month): removes the SmartScreen
      warning.
- [ ] **Intel Macs:** a download (needs FFmpeg for x86_64, or a build without video).
- [ ] **Linux.**
