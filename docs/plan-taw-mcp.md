# Plan: taw-mcp, agents in the open session

Status (2026-10-29): step 1 built on branch `taw-mcp` (Windows; nothing in it is Windows-only).
See "Step 1 as built" at the end.

Written so another agent can pick it up. Read `docs/design.md` first (Threads, Undo, Headless
commands, Saving, Hub), then `src/Agents.h`.

## Why

`taw --exec` lets an agent build a session, but only one that's closed: it opens the session
itself, and `.taw-lock` keeps it out of one that's open. The user wants to record, audition and
try ideas in a running taw while agents (Claude Code, or any MCP client) add, change and take
away parts of that same session as they go: a bass line under the chords just played, a hat
pattern on the kit, a marker at the bridge. All on this machine for now.

## Scope

| Step | What | Status |
|---|---|---|
| 1 | The control socket in taw; `taw --mcp`, the stdio bridge; reads; the command passthrough; tracks, notes and hits; the hands-off and play rules; `--mcptest` | **built** |
| 2 | More structured edits: rename, volume, pan, mute, sends, outputs, inserts, kit pads, move and trim clips, CCs, automation points, tempo and time signature | next |
| 3 | Waiting instead of refusing: an edit made while playing queues and lands at the next stop (the agent's choice, `when: "stopped"`) | later |
| 4 | Asking the user: an agent proposes a change and the user takes or leaves it with a key (a pending line on screen) | later, maybe |
| 5 | Other machines (the socket off localhost, with a real login) | later, not in this plan |

## Decisions

Made by the user (fixed):

1. **Transport.** The running taw opens a local-only control socket. A separate small process,
   `taw --mcp` (a mode of taw.exe, like `--hub`, so there's nothing else to ship or keep in
   step), speaks MCP over stdio to the agent and relays to the open session. A bridge or agent
   that crashes, hangs or sends rubbish never takes taw down: it only loses its connection.
2. **Tools.** Structured reads (the session: tempo, time signature, tracks, kinds, plugins,
   clips, notes, hits, markers, sends, kit members) and structured edits (tracks, MIDI clips and
   notes, hit clips and hits, markers), **plus** a passthrough that runs a line of taw's own
   command language, exactly as `App::execute` (and `taw --exec`) does.
3. **Hands-off.** Agents may not change the track the user's cursor is on, anything inside the
   user's time selection, or anything the user edited in the last few seconds. Such an edit is
   refused with a reason. Armed tracks and open editor or plugin windows are not locked.
4. **Playing.** While taw plays or records (or counts in), edits are refused with a "playing"
   error; the agent tries again later. Reads work while playing.
5. **Click and pop free.** The engine keeps running when stopped (monitoring, auditioning,
   plugin tails), so agent edits reach it exactly as a person's do: on the message thread, into
   the state tree, then a graph rebuild handed over through the lock-free queue.

Made for the user (fixed):

- Agent edits are on taw's undo stack, one step per tool call, named after the agent
  ("claude-code: add track Bass"), so ctrl+z takes them back.
- Agents never save. Their edits go into `unsaved\` like anyone's; saving stays ctrl+s.
- Edits from several agents are done one at a time, in the order they arrive.
- An agent's change shows on taw's status line for a moment ("claude-code added track 4 Bass").

Decided here (defaults; change them if they're wrong):

- **Finding the session.** Each interactive taw writes `<config>/agents/<pid>.json` (`port`,
  `pid`, `token`, `session`, `name`, `started`) and deletes it on a clean exit. The bridge reads
  the folder, skips files whose pid isn't a running taw, and takes the newest one with a session
  open; `taw --mcp --session <folder or name>` picks one when several are running. It looks
  again on every call, so it follows taw being restarted or a session being opened later.
- **A token.** The socket listens on 127.0.0.1 only, and the first message must carry the
  token from that file (readable only by the user's account), so another local program can't
  drive taw without reading the user's files.
- **The socket is always on** in interactive taw. It costs one idle thread. (Open question:
  should there be `agents off`?)
- **Tracks are named as on screen**: `"3"`, `"2.3"` (pad 3 of kit 2), or by exact name. Track
  numbers move when tracks are added above or removed, so reads give both and an agent should
  read again after the user has been busy. (Open question: stable track ids in the file; not
  needed yet.)
- **Positions are beats** from the session start (beat 0 is bar 1), as MIDI and hit clips
  store them; audio clips are given in beats and seconds too.
- **Tempo and time-signature changes are allowed**, though they move everything in beats
  under the user's feet; refused only when the user changed a session setting in the last few
  seconds. (Open question.)
- **The passthrough can't** save, close, quit, play, stop, record, undo or redo (the user's
  history), bounce, open windows or pickers, change the device, theme or controller mapping,
  or use the clipboard (copy, cut, paste). It also can't move the user's cursor, focus,
  selection or view: those are put back after the command runs.
- **New instrument tracks aren't armed** (as they are when a person makes one), so the user's
  controller doesn't start playing the agent's part too.

## The pieces

```
Claude Code ──stdio, MCP (JSON-RPC)──▶ taw --mcp ──127.0.0.1, framed JSON──▶ taw (the open session)
                                       (the bridge)                          agents::Server: a thread
                                                                             per connection, requests
                                                                             queued; App::pollAgents
                                                                             runs them each frame on
                                                                             the message thread
```

- `src/Agents.h/.cpp`: the server (in taw), the bridge (`taw --mcp`), the tool list (shared,
  so the bridge and taw agree on names and shapes), and the discovery file.
- `src/AgentEdits.cpp`: the `App` side: the tools themselves, the guard (hands-off), and
  `pollAgents`, called once a frame from `App::run` beside `pollPlugins`.

### Taw's socket

JUCE's `InterprocessConnection` / `InterprocessConnectionServer`, as the hub uses (a 4-byte magic
and a length before each message; the magic is `"tawa"`, not the hub's `"tawc"`). Messages are
JSON objects.

```
bridge → taw   { "type": "hello", "token": "…", "agent": "claude-code" }
taw → bridge   { "type": "welcome", "session": "Song 10.29.26", "folder": "D:\\…" }
               { "type": "refused", "reason": "wrong token" }

bridge → taw   { "type": "call", "id": 7, "tool": "write_notes", "args": { … } }
taw → bridge   { "type": "result", "id": 7, "ok": true, "text": "wrote 8 notes…", "data": { … } }
               { "type": "result", "id": 7, "ok": false, "code": "playing", "text": "taw is playing…" }
```

Error codes: `playing`, `hands-off`, `busy` (an import or bounce is running, or a session is
opening or closing), `no-session`, `invalid` (bad arguments), `failed` (the command said no).

Connection threads only parse and queue; nothing they do touches the session. Replies are sent
from the message thread. A connection that goes away mid-call just has its answer dropped.

### The bridge

Newline-delimited JSON-RPC 2.0 on stdin and stdout (MCP's stdio transport), hand-rolled: it's
four methods. Nothing else is written to stdout; problems go to stderr.

| Method | Answer |
|---|---|
| `initialize` | `protocolVersion` (the client's if we know it, else `2025-06-18`), `capabilities: { tools: {} }`, `serverInfo: { name: "taw", version }`, and `instructions` (how taw's rules work) |
| `notifications/initialized`, other notifications | nothing |
| `ping` | `{}` |
| `tools/list` | the tools below |
| `tools/call` | `{ content: [{ type: "text", text }], isError }`: the data as JSON text, or the message |
| anything else with an id | error -32601 |

The agent's name for undo steps and the status line is `clientInfo.name` from `initialize`
(`--name <agent>` overrides it).

## Tools

All edits answer with what they did, as taw's own messages do, and refuse with a reason.

| Tool | Arguments | Does |
|---|---|---|
| `get_session` | `{ include_notes?: bool }` | the session: `name, bpm, time_signature [n, d], sample_rate, playing, recording, cursor_beat, focused, selection { from_beat, to_beat, tracks } \| null, loop, markers [{ name, beat }], tracks [...]`; each track as `get_track`, with clips summarised (note and hit counts) unless `include_notes` |
| `get_track` | `{ track }` | `{ id: "3", name, kind: audio\|instrument\|sample\|bus\|kit, locked?: "why", armed, mute, solo, volume_db, pan, output: "Mix"\|bus name, plugin?, sample?, kit?: "2", pad?, note?, choke?, members?: ["2.1", ...], inserts [{ slot: "A", plugin, bypass }], sends [{ slot, to, db, pre }], clips [...] }`; audio clips `{ index, beat, length_beats, seconds, length_seconds, file }`, MIDI clips `{ index, beat, length, notes [{ beat, length, pitch, vel, ch }] }`, hit clips `{ index, beat, length, hits [{ beat, vel }] }`; note and hit beats are from the clip's start |
| `run_command` | `{ command }` | one line of taw's command language (`2 v -6`, `bpm 92`, `3 pat /16 x.x.`, `add 2 Gtr`), as typed in taw, one undo step |
| `add_track` | `{ kind: audio\|instrument\|sample\|bus\|kit, name?, plugin?, sample? }` | a new track at the bottom; an instrument loads `plugin` (the best name match, as `inst`), or none; a sample track loads `sample` (a file path), or none yet |
| `remove_track` | `{ track }` | as `3 remove`: a bus's feeds go back to the mix |
| `write_notes` | `{ track, clip?, beat?, length?, notes: [{ beat, length, pitch, vel?, ch? }], replace? }` | notes into MIDI clip `clip` (an index from `get_track`; it grows to fit), or into a new clip at `beat` (`length` beats long; by default whole bars round the notes). `replace` clears the clip's notes first. `vel` 1-127 (100), `ch` 1-16 (1) |
| `write_hits` | `{ track, clip?, beat?, length?, hits: [{ beat, vel? }], replace? }` | the same for a sample track's hit clips. `vel` 0-1 (0.8) |
| `remove_clip` | `{ track, clip }` | one clip, by index |
| `add_marker` | `{ beat, name }` | a marker |

Step 2 adds `set_track` (name, volume, pan, mute, solo, output, sends), `set_session` (tempo,
time signature, loop), `move_clip`, `write_cc` and `write_automation`. Until then the passthrough
does all of those.

## Hands-off

Before an edit, taw notes what's the user's:

- **The focused track** (the one the cursor is on; nothing when it's on the Mix row).
- **The time selection**, if there is one, on the tracks it covers (the shift-selected tracks,
  else the focused one; all of them from the ruler or markers row): every clip there that
  overlaps it, and the markers in it.
- **Recent edits**: a listener on the session tree remembers, per track (and for the session's
  own settings), when the user last changed anything there. Anything changed in the last
  **5 seconds** is the user's. (Changes taw makes itself while you work count too; that only
  errs on the safe side.)

Then the edit runs inside a guard: a second listener collects every node it touches (a property
set, a child added or removed). After it, each touched node is traced up to its track (and its
clip). The edit is refused, and **rolled back** (`UndoManager::undoCurrentTransactionOnly`, which
also gives the user their redo history back), when any of these hold:

- it touched the focused track, or a recently edited one, or the session's settings after the
  user changed one;
- it touched a clip that was in the selection, or left a clip on a selected track overlapping
  it, or touched a marker in the selection, or removed a track the selection covers.

Checking afterwards instead of guessing beforehand means the passthrough gets the same rules as
the structured tools for free: `v -6` with no selector would change the focused track, so it's
refused. The rollback happens in the same message-thread call, before the graph is rebuilt, so
the engine never hears the refused edit.

`get_session` and `get_track` mark locked tracks (`locked: "your cursor is on it"`, `"you edited
it 2 s ago"`, `"your time selection is on it"`), so an agent can steer clear without trying.

## Playing

Every edit tool checks first: playing, counting in or recording → `playing`, "taw is playing ·
try again when it stops". Opening, closing or the save prompt up, or an import or bounce
running → `busy`. Reads always run.

## How edits reach the engine without a click

The same way a person's edits do; nothing new on the audio thread.

1. A connection thread parses the message and queues it (a lock shared only with other
   connection threads and `pollAgents`; the audio thread never sees it).
2. `App::pollAgents`, on the message thread once a frame, takes one call at a time.
3. The call changes the state tree with `um()`, inside one undo transaction named for the agent.
4. The App's own tree listener marks the tree dirty; the same frame's `rebuildGraph()` builds a
   new graph and hands it over through `AudioEngine`'s lock-free queue, and `persist()` writes
   `unsaved\session.json`.

Stopped, a new graph changes only what's sounding where something sounds now: an armed track's
monitoring, an instrument's held notes or tail, an audition. Agent edits only add clips, notes
and tracks or remove them; they don't touch an instrument's plugin instance unless asked to
swap it (and then exactly as `inst` does for a person). Removing a track that's ringing cuts
it, as it does for a person (open: a short fade-out on removal, for everyone).

## Tests

- `taw --mcptest <scratch-folder>`: no audio device. Makes a session there, opens it as an
  interactive taw would (edits to `unsaved\`), starts the server, and drives a bridge on another
  thread through real sockets, pumping the App on the main thread between calls, as the frame
  loop would. Checks: discovery finds this taw by its file and token; initialize, tools/list;
  get_session; add tracks (audio, instrument with no plugin, sample, bus); write notes into a
  new MIDI clip and an existing one; write hits; remove a clip; add a marker; run_command; each
  edit one undo step with the agent's name, ctrl+z taking it back; remove a track; the status
  line; refusals: playing, the focused track, a time selection, a recent edit, the passthrough's
  forbidden verbs (save, undo, play); the passthrough leaving the cursor and focus alone;
  `session.json` untouched (agents never save); a wrong token refused; a dropped connection and
  garbage on the socket leaving taw running.
- `taw --serve <session> [seconds]`: opens a session headless with the control socket and runs
  for a while, so `taw --mcp` can be checked from a shell by piping JSON-RPC into it.

## Step 1 as built

- `src/Agents.h/.cpp`: `agents::Server` (taw's socket and listing), `agents::Bridge` (one
  JSON-RPC line in, the reply out; `handle` is what the test drives), `runBridge` (`taw --mcp`,
  stdin and stdout, stdout in binary on Windows so lines end in `\n`), `toolList`, and
  `RecentEdits` (the user's last changes, per track and the session).
- `src/AgentEdits.cpp`: `struct AgentTools` (a friend of `App`) holds the tools, the guard
  (`edit`), the locks, and keeping the user's view (cursor, focus, selection, scroll, open
  panels) as it was. `App::pollAgents` runs calls for up to 20 ms a frame, one at a time.
- `App::beginEdit` does nothing while an agent's call runs (`agentEditing`), so a command that
  starts several steps is still one; `App::pollImport` leaves an agent's import to `pollAgents`,
  which puts it in inside the guard and answers then.
- In the frame loop: `startAgents()` after the device opens; `pollAgents()` before `pollImport()`.
- `Commands.cpp` gained `commandShape` (the parse, public) for the passthrough's checks.
- Rolled back on refusal: our tools all or nothing; the passthrough keeps what a command did
  when it fails on its own (as for a person), and rolls back only a hands-off breach.
- Tested: `taw --mcptest` (54 checks, all ok), clean under MSVC `/W4` and clang-cl's strict
  set; `taw --mcp` piped from a shell against `taw --serve` (every line it writes is JSON).

Not built yet, and open:

- `set_track`, `set_session`, `move_clip`, `write_cc`, automation as tools (the passthrough
  covers them meanwhile).
- **Agents off.** No setting to keep the socket shut; should there be one (`agents off` in
  `settings.json`)?
- **Stable track ids.** Numbers shift when tracks are added above or removed; agents are told to
  read again. A `uid` on each TRACK would fix it (a file change; not needed yet).
- **Tempo changes** are allowed (they move everything in beats). Should they be the user's?
- **A removed track that's ringing** is cut, as it is for a person. A short fade on removal
  would help both.
- **macOS:** nothing Windows-only, but not yet built or run there.
- **Firewall:** the socket is on 127.0.0.1 only, which Windows doesn't usually ask about; if it
  does, allow it (or deny: nothing outside the machine needs it).
