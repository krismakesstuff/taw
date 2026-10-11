# Security

## Reporting a problem

Please don't open a public issue for a security problem. Report it privately through GitHub:
the repository's **Security** tab → **Report a vulnerability**. Say what you found, how to
reproduce it, and which version (`taw --version`) and platform.

You'll get an answer as soon as the maintainer can; taw has one maintainer, so it may take a
few days. Please give a reasonable time for a fix before you publish anything.

## What's worth knowing

- **Plugins run inside taw.** A VST3 or Audio Unit plugin has the same access as taw itself.
  Only load plugins you trust; that's not something taw can protect you from (yet: running
  plugins in their own process is planned).
- **The audio hub (macOS)** installs a CoreAudio driver into `/Library/Audio/Plug-Ins/HAL`,
  with your admin password, and it runs inside macOS's audio service. Problems in the driver or
  in how the hub talks to sessions are especially worth reporting.
- **Sessions and templates** are plain JSON files; a session from someone else is data, and
  taw shouldn't do anything unexpected when opening one. If it does, that's a security problem.
- Downloads from the Releases page aren't code-signed yet. Check them against `SHA256SUMS.txt`
  on the same release.

Only the latest release gets security fixes.
