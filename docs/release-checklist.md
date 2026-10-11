# First release checklist

What's left before taw goes public and ships its first downloads. Tick items off as they're done.

## Decide

- [ ] **Version number.** A `v0.1.0` tag ("taw 0.1.0: first playable milestone", 30 Sep 2026)
      already exists, and the release workflow stops on a version that has a tag. Either bump the
      first public release to 0.2.0 (`CMakeLists.txt` and the `CHANGELOG.md` heading; the
      milestone tag stays), or delete the old tag: `git push origin :refs/tags/v0.1.0`.

## Before the repository goes public

- [ ] **Have a lawyer read [CLA.md](../CLA.md)** and the revenue wording in
      [CONTRIBUTING.md](../CONTRIBUTING.md), especially the licence to relicense (section 2) and
      the discretionary revenue sharing (section 6). Do this before the first outside pull request.
- [ ] **Add a contact email** in [CODE_OF_CONDUCT.md](../CODE_OF_CONDUCT.md), in place of
      `[CONDUCT CONTACT EMAIL]`.
- [ ] **Rewrite the history's email addresses** (about 80 commits carry the work address):

  ```
  pip install git-filter-repo
  git clone --mirror https://github.com/krismakesstuff/taw.git taw-clean
  cd taw-clean
  cat > ../mailmap <<'EOF'
  Kris Crawford <11crawfordk@gmail.com> <kris@placetostandagency.com>
  Kris Crawford <11crawfordk@gmail.com> Claude <noreply@anthropic.com>
  EOF
  git filter-repo --mailmap ../mailmap
  git log --all --format='%an <%ae> | %cn <%ce>' | sort -u    # check: no work address left
  ```

  The second mailmap line is optional: it makes the two Claude-authored commits yours (they
  already credit Claude as co-author), as the AI rule in CONTRIBUTING.md asks.

- [ ] **Push the rewritten history.** Cleanest: a new, empty public repository, since GitHub keeps
      old pull requests' commits (with the old address) in this one.

  ```
  git remote add origin https://github.com/krismakesstuff/<new-repo>.git
  git push origin --all && git push origin --tags
  ```

  Or force-push into this repository (`git push --force origin --all` and `--tags`), accepting
  that old pull requests may still show the old address.
- [ ] **Afterwards:** re-clone local copies, `git config --global user.email 11crawfordk@gmail.com`,
      and turn on GitHub's "Keep my email addresses private".

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
