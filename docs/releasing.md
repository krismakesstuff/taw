# Releasing

Releases come from the `publish` branch and nowhere else. Pushes to `main` only build and test
(CI); tags don't trigger anything.

1. On `main`: set the version in `CMakeLists.txt` (`project(taw VERSION x.y.z ...)`) and give
   `CHANGELOG.md` a `## [x.y.z] - yyyy-mm-dd` section with the notes. Merge as usual.
2. Optional dry run: Actions → Release → Run workflow, on `main`. It builds and packages both
   platforms and keeps the zips as the run's artifacts, without making a release.
3. Publish: bring `publish` up to `main` and push it.

   ```
   git checkout publish
   git merge --ff-only main
   git push origin publish
   ```

   The Release workflow stops at once if that version already has a tag or a release (bump the
   version), or if CHANGELOG.md has no section for it. Otherwise it builds Windows and macOS,
   runs the self-tests, and makes a **draft** release `vx.y.z` with the zips, FFmpeg's source,
   `SHA256SUMS.txt` and the CHANGELOG notes.
4. Look the draft over on the Releases page (download and try the zips), then publish it.
   Publishing creates the `vx.y.z` tag on the commit it was built from.

To rebuild a draft (a fix before publishing): delete the draft, push the fix to `publish`.

## Keeping publish safe

In the repository's Settings → Branches (or Rules), protect `publish`: only the maintainer may
push, no force pushes, no deletion. Then a release can only start from a deliberate push.
