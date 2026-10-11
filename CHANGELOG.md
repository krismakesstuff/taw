# Changelog

## [0.2.0] - unreleased

The first public release: an early proof of concept, for Windows and macOS (Apple Silicon).
(0.1.0 was the first playable milestone, never released.)

- Recording-first timeline driven by the keyboard and a small command language (`2v-6`,
  `add Vox in2`, `1-3m`); the same commands run headless with `taw --exec` for scripts and agents.
- Audio, MIDI and sample tracks; VST3 instruments and effects (Audio Units too on macOS), five
  inserts and five sends per track, buses, latency compensation and a low-latency mode.
- Editing: copy, cut, paste, duplicate, consolidate, undo; markers; loop recording onto playlists.
- Automation lanes, written by hand or recorded from MIDI controllers.
- A sample library and browser, with a sound-alike map.
- Video for scoring to picture, and bouncing with picture (`bv`).
- Bouncing to WAV and MP3.
- Importing Ableton Live sets.
- The audio hub (macOS): routing between apps and sessions through taw's own audio devices.
- Keycast: a window showing the keys you press, for screen recordings.
