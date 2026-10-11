# Plan: clip fades, the browser window, kits

Status (2026-10-11): step 1 (fades) built on branch `fades` (see "Fades" in `docs/design.md`);
the rest decided, not built. Order: fades, then the browser window, then kits (which need both).
Typed fade commands are `clip fade ...` and `xfade` (not `fade`: `f` is the fill hotkey).

Written so another agent can pick it up. Read `docs/design.md` first (Threads, Editing,
Sample tracks), and the memories on Pro Tools keys and recording-first patterns.

## 1. Clip fades and crossfades (audio clips)

### What the user does

The selection decides; one key makes it, as Pro Tools' Ctrl+F does:

| Selection | `ctrl+f` makes |
|---|---|
| From a clip's start into it | a fade in, the selection's length |
| From inside a clip to its end | a fade out |
| Across the line where two clips meet | a crossfade spanning the selection |
| Across several clips | all of those at once (Pro Tools' batch fades) |
| Just a cursor | nothing: "select a range to fade" |

- `ctrl+f` again on the same fade cycles its shape: equal power, linear, S-curve,
  exponential. The status line names it; it's heard at once.
- Typed, on the clip under the cursor: `fade in 10ms`, `fade out 1/4`, `xfade 20ms`,
  `fade off`. Lengths take ms, s, or note values (on the grid's tempo).
- **Declick on by default:** every audio clip edge without a fade gets 2 ms (`declick 2ms`,
  `declick off`; a session setting). On a character grid you can't see a click coming.
- On the timeline: ╱ ╲ at clip edges with fades, ╳ over a crossfade.
- Audio editor (step 1b): Pro Tools-style corner handles: drag a clip's top corner to set a
  fade's length; the curve is drawn over the waveform.

### Model

`CLIP` gains `fadeIn`, `fadeOut` (samples) and `fadeInShape`, `fadeOutShape` ("eqp" default,
"lin", "s", "exp"). Absent = no fade. Older taw ignores them (plays without fades), so no
session version bump.

Clips already overlap, the later one winning (`renderClips`). A fade there is a crossfade
with what's under it: in a clip's fade-in region the buffer becomes
`under * outCurve(t) + clip * inCurve(t)`, and the same mirrored in its fade-out. Over
silence `under` is 0, so it's a plain fade. So:

- **A crossfade is the later clip overlapping the earlier one, with a fade-in as long as the
  overlap.** Making one at a boundary extends the earlier clip's end and pulls the later
  clip's start back (start, offset and length together) into their handles, the audio past
  each clip's edges in its file, so they overlap over the selection. Not enough handle on a
  side: say how much there is ("12 ms of audio past the left clip's end") and make nothing.
- A clip pasted into the middle of a longer one crossfades in and out of it the same way.
- `renderClips` stays shared by playback and bounces; clips are rendered in list order so
  `under` is what earlier clips already wrote.
- Declick is applied when the graph is built (`ClipPlay` gets max(fade, declick)), so
  `renderClips` itself has no settings and `--cliptest`'s raw cases stay as they are.
- Fades are clamped so fade in + fade out fit the clip.

Edits: trimming a start keeps the fade-in's length (clamped); separate gives the left piece
the fade-in and the right the fade-out, new edges get declick only; copy/paste carries them
(they're properties of the CLIP); undo as any edit.

Clip gain (next on the editor list) lands in the same render change later.

### Checks

`--cliptest` gains: fade-in over silence, fade-out, crossfade over an earlier clip (sum of
both curves), each shape's endpoints, block-size independence, declick from a session.
A headless `--exec` run: ctrl+f's three cases and the handle refusal.

## 2. The browser as its own window

Decided: a separate window, mouse-forward like a plugin's, still fully usable from the
keyboard. It replaces the docked pane; `ctrl+b` opens and closes it.

- **Kind tabs across the top**, big enough to click. The map fills the middle; the list
  sits beside it; the audition level and filter stay.
- **Drag over the map to audition** whatever is under the pointer (XO-style); hover too,
  with a setting. Click in the list auditions.
- **Drag a sample onto a kit pad** (section 3) or press enter to load it on the focused
  track or pad.
- Keyboard: tab between tabs / map / list / pads, arrows inside, enter loads, typing filters.
  The window keeps the keyboard while in front and passes keys it doesn't use to taw, like the
  editors (`EditorWindow.h`, `keyForTaw`); esc hands back.
- **Audition in time with the loop**: while taw plays, an audition waits for the next beat
  (or grid step) so it's heard against the groove.

## 3. Kits

The user's model holds: one sample per track; a kit groups sample tracks.

### Model

A **kit is a bus** with `kit` set; its members are sample tracks playing into it, each with a
`note` (MIDI note number) and optionally a `choke` group. Pads default to note 36 (C1) and up,
what MPC, Maschine and most controllers' drum pads send. Each member keeps its own HITCLIPs,
so editing, mute, sends and inserts stay per sound. The kit row folds and unfolds its members.

- `add kit` makes the bus; loading a sample onto a pad makes the member track.
- Choke: a hit on a track cuts the ringing voices of others in its group (closed hat stops
  the open one). Engine: on a pad press, end voices of the same group with a few ms fade.

### Playing

- **Controller notes reach kits**: on the audio thread, a note-to-track lookup for armed kits
  turns note-ons into pad presses (low latency). Today controller notes only reach armed
  instrument tracks. Recording writes the hits onto each member's clip.
- **Keys mode on a kit uses a 4×4 pad grid** (MPC layout, pad 1 bottom-left):
  ```
  1 2 3 4      pads 13-16
  q w e r      pads  9-12
  a s d f      pads  5-8
  z x c v      pads  1-4
  ```
  Notes go in as a controller's would, so recording is the same. Shift is the accent (as `>`).
  Keyboard ghosting limits some 3+ key chords on typing keyboards; a pad controller is better
  for real finger drumming.
- `.` keeps playing every armed sample track at once.

### Building a kit fast (browser + kit)

- The kit's 4×4 pads show in the browser window. Pick a pad, audition sounds: what you hear
  goes onto that pad at once (Battery/XO), no separate load step.
- With the loop running you hear each candidate in the beat.
- One key: "more like this pad's sound" (the library's `like`).

## Order of work

1. Fades: model, render, declick, ctrl+f, typed commands, timeline glyphs, tests.
1b. Fades in the audio editor: curves, corner handles.
2. Browser window: tabs, map, list, audition, keyboard; retire the docked pane.
3. Kits: bus + members + notes, folding, controller routing, keys-mode pads, choke.
4. Kit building in the browser: pads, audition-replaces, like-this.
