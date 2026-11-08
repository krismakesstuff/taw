# ptformat

Reads Pro Tools sessions (`.ptx`, Pro Tools 10 and later; older `.ptf`/`.pts` too) for
`import x.ptx`. From <https://github.com/zamaudio/ptformat> (the library Ardour uses), commit
`af15ee9`. LGPL-2.1-or-later: see `COPYING.LESSER`. Copyright Damien Zammit and Robin Gareus.

Files: `ptformat.cc`, `ptformat/ptformat.h`, `ptformat/visibility.h`. taw's changes, each
marked `// taw` in the source:

- `alltracks()`: every track's name in the session, in order (the library already read them,
  to tell MIDI tracks from audio ones, but kept them to itself).
- A MIDI region's track takes the name of the MIDI playlist it sits on. Upstream picks a name
  from the track list by position, which lands on the wrong track (an audio track's name).
