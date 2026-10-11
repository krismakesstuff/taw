# First release checklist

What's left before taw goes public and ships its first downloads. Tick items off as they're done.

## Decide

- [x] **Version number:** the first public release is **0.2.0**. The `v0.1.0` tag ("first
      playable milestone", 30 Sep 2026) stays as history.
- [x] **Commit email:** no history rewrite. Older commits keep the work address; from now on this
      repository commits as `Kris Crawford <11crawfordk@gmail.com>` (set in the repo's own git
      config, so other repositories keep their address).

## Before the repository goes public

- [ ] **Have a lawyer read [CLA.md](../CLA.md)** and the revenue wording in
      [CONTRIBUTING.md](../CONTRIBUTING.md), especially the licence to relicense (section 2) and
      the discretionary revenue sharing (section 6). Do this before the first outside pull request.
- [x] **Contact email** in [CODE_OF_CONDUCT.md](../CODE_OF_CONDUCT.md): 11crawfordk@gmail.com.
- [ ] **Make the repository public:** Settings → General → Danger Zone → Change visibility.

## GitHub settings

- [ ] **Private vulnerability reporting:** Settings → Code security → Private vulnerability
      reporting ([SECURITY.md](../SECURITY.md) points there).
- [ ] **CLA signatures branch:** create `cla-signatures` from `main`; the CLA bot stores
      signatures there.
- [ ] **Publish branch:** once the release setup is merged, `git push origin main:publish`.
- [ ] **Protect `publish`:** Settings → Branches (or Rules): only the maintainer pushes, no force
      pushes, no deletion.

## First release

See [releasing.md](releasing.md) for the steps.

- [ ] Merge the release setup (CI, release workflow, contributing files) into `main`.
- [ ] **Dry run:** Actions → Release → Run workflow on `main`. It's the first real test of the
      Windows and macOS builds. macOS minutes are expensive on a private repository and free on a
      public one.
- [ ] Fix whatever the dry run turns up.
- [ ] **Try the zips on real machines:**
  - [ ] Windows: unzip, run from a terminal, SmartScreen's "Run anyway", video plays.
  - [ ] macOS: the `xattr` step from the README, then taw runs.
  - [ ] `taw --install-hub` and the driver install work from the unzipped folder, not the build folder.
  - [ ] macOS video works with Homebrew's FFmpeg (it has to be the same major version the release
        was built against).
  - [ ] Keycast: clicking or dragging its window leaves the keyboard with the terminal, and
        `taw --keycasttest` passes.
- [ ] Date the version's heading in [CHANGELOG.md](../CHANGELOG.md).
- [ ] Push `publish`, look over the draft release, publish it.

## Later

- [ ] **macOS signing and notarization** (Apple Developer account, $99/yr): removes the `xattr`
      step.
- [ ] **Windows signing** (Azure Trusted Signing, about $10/month): removes the SmartScreen
      warning.
- [ ] **Intel Macs:** a download (needs FFmpeg for x86_64, or a build without video).
- [ ] **Linux.**
