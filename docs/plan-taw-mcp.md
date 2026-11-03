# Plan: taw-mcp, agents in the open session

Status (2026-10-29): step 1 built on branch `taw-mcp` (Windows; nothing in it is Windows-only),
with the user's answers to its four questions: an `agents` setting, stable track ids, tempo
changes that ask the user about audio, `--mcptest` in CI. See "Step 1 as built" at the end.

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
- **The socket has a setting, on by default** (decided with the user, 2026-10-29):
  `"agents"` in `settings.json` (every session, like the theme), `agents` / `agents on` /
  `agents off` in taw, and an "Agents (all sessions)" row in the project panel. Off: no
  socket, no `agents\<pid>.json`, and `taw --mcp` answers `agents-off`: "agents are turned off
  in taw: the user can type agents on there". Agents can't switch it (the passthrough refuses
  `agents`).
- **Stable track ids** (decided with the user): every TRACK has a `uid` ("t7") for good.
  It survives moves, renames, undo/redo (the node keeps it) and save/load; a copy (`dup
  track`) gets a new one; ids are never handed out twice (`nextTrackId` on the SESSION counts
  past every id given, so an undone track's id can't come back on another). `App`'s tree
  listener gives one to any TRACK that arrives without one, or with another's, so every way of
  making tracks is covered without touching them; `Session::load` and `create` give ids to
  tracks that have none (old sessions, templates). Neither property is undoable, and neither
  counts as anyone's edit for the hands-off rules. Reads give `id` and `number` (`"3"`, as on
  screen and in `run_command`); every tool's `track` takes the id, and falls back to the number
  or the exact name (kept: a person pasting "3" into a chat still works, and the id is tried
  first so it can't be mistaken).
- **Clips stay by index**, with a check: `clip_beat` (the clip's start, as read) makes a
  write or a removal refuse ("the clips have changed, read the track again") if clip `n` isn't
  the clip the agent read. No clip ids: clips are made in many places (recording, splitting,
  pasting, loop passes), and a start beat on one track is as good as an id for "the clip I
  read". Markers need none: agents only add them.
- **Positions are beats** from the session start (beat 0 is bar 1), as MIDI and hit clips
  store them; audio clips are given in beats and seconds too.
- **Tempo** (decided with the user): agents may change it; see "Tempo and audio" below. The
  time signature (`sig 3/4`) changes no positions in time and goes straight through.
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
opening or closing), `no-session`, `agents-off` (the bridge: nothing listens because the
setting is off), `needs-user` (a tempo change waiting on the user), `declined` (the user said
no to it), `invalid` (bad arguments), `failed` (the command said no).

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
| `get_session` | `{ include_notes?: bool }` | the session: `name, bpm, time_signature [n, d], sample_rate, playing, recording, cursor_beat, focused (an id), selection { from_beat, to_beat, tracks (ids) } \| null, loop, tempo_request, markers [{ name, beat }], tracks [...]`; each track as `get_track`, with clips summarised (note and hit counts) unless `include_notes` |
| `get_track` | `{ track }` | `{ id: "t3", number: "3", name, kind: audio\|instrument\|sample\|bus\|kit, locked?: "why", armed, mute, solo, volume_db, pan, output: "Mix"\|bus name, plugin?, sample?, kit?: (its id), note?, choke?, members?: [ids], inserts [{ slot: "A", plugin, bypass }], sends [{ slot, to, db, pre }], clips [...] }`; audio clips `{ index, beat, length_beats, seconds, length_seconds, file }`, MIDI clips `{ index, beat, length, notes [{ beat, length, pitch, vel, ch }] }`, hit clips `{ index, beat, length, hits [{ beat, vel }] }`; note and hit beats are from the clip's start |
| `run_command` | `{ command }` | one line of taw's command language (`2 v -6`, `3 pat /16 x.x.`, `add 2 Gtr`), as typed in taw (tracks by `number`), one undo step; `bpm` goes through `set_tempo` |
| `add_track` | `{ kind: audio\|instrument\|sample\|bus\|kit, name?, plugin?, sample? }` | a new track at the bottom, answering `{ track: id, number }`; an instrument loads `plugin` (the best name match, as `inst`), or none; a sample track loads `sample` (a file path), or none yet |
| `remove_track` | `{ track }` | as `3 remove`: a bus's feeds go back to the mix |
| `write_notes` | `{ track, clip?, clip_beat?, beat?, length?, notes: [{ beat, length, pitch, vel?, ch? }], replace? }` | notes into MIDI clip `clip` (an index from `get_track`; it grows to fit; `clip_beat` checks it's the one read), or into a new clip at `beat` (`length` beats long; by default whole bars round the notes). `replace` clears the clip's notes first. `vel` 1-127 (100), `ch` 1-16 (1) |
| `write_hits` | `{ track, clip?, clip_beat?, beat?, length?, hits: [{ beat, vel? }], replace? }` | the same for a sample track's hit clips. `vel` 0-1 (0.8) |
| `remove_clip` | `{ track, clip, clip_beat? }` | one clip, by index |
| `add_marker` | `{ beat, name }` | a marker |
| `set_tempo` | `{ bpm }` | the tempo; with audio clips in the session, the user decides first (below) |

`track` is the id from a read ("t3"); the number as on screen ("3", "2.3") or the exact name
work too.

Step 2 adds `set_track` (name, volume, pan, mute, solo, output, sends), `set_session` (time
signature, loop), `move_clip`, `write_cc` and `write_automation`. Until then the passthrough
does all of those.

## Tempo and audio

Decided with the user (2026-10-29): agents may change the tempo. MIDI and hit clips are in
beats, so they always follow it. Audio clips are in samples (`CLIP.start`), so with audio in
the session the user decides what happens to it, every time:

1. The agent's `set_tempo` (or `run_command bpm …`) changes nothing and answers `needs-user`.
2. taw shows the tempo box (it takes the keys until it's answered, as the save prompt does):
   "TEMPO claude-code wants 100 → 120 bpm · MIDI and hits follow the beats. Your audio clips:"
   - `t` keep them where they are in time (they stay at their seconds): only the tempo changes.
   - `b` move them with the beats: each clip's start goes to the same beat at the new tempo
     (`start × old / new`), in the main playlist and the others. The audio isn't stretched,
     so a clip runs as long in seconds as it did and lines up with the grid only at its start.
   - `s` stretch them to the new tempo: shown faint, "coming, not built yet". Pressing it says
     so and the box stays.
   - `esc` cancel: the tempo stays.
3. The answer is applied at once, as one undo step named for the agent ("claude-code: bpm 120").
   Markers and the loop stay in time either way, as they do when a person changes the tempo.
4. The agent finds out by calling `set_tempo` again: "the tempo is 120 (the user chose: audio
   clips moved with the beats)", or `declined` if they cancelled (for two minutes, so it doesn't
   keep asking), or `needs-user` while the box is still up. `get_session`'s `tempo_request` says
   the same (`waiting for the user`, `applied`, `cancelled`, with what was chosen).

Both real options are possible with clips as they're stored today; only stretching isn't.
**Which the user picks tells us whether time-stretching is wanted:** if they keep reaching for
`b` and then fixing the audio, or press `s`, stretching is next.

With no audio clips the change goes straight through (the hands-off rule for the session's
settings still holds). The time signature changes nothing in time, so `sig` needs no box.

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
  garbage on the socket leaving taw running. Then: track ids through a move (alt+up), an agent's
  removal undone by the user, a user's `dup track` (a new id) and a new track (never a used id),
  writing by id and a stale `clip_beat` refused; `agents off` (no socket, no listing, the
  bridge says `agents-off`) and `agents on`; the tempo: straight through with no audio, and with
  an audio clip `needs-user`, the box on screen, `s` doing nothing yet, `b` moving the clip, the
  retry hearing the choice, `esc` and `declined`, `t` leaving the clip; a reload keeping every
  id; an old session without ids getting t1, t2, t3 on load.
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
- Then, with the user's answers: the `agents` setting (`App::agentsCommand`, settings.json,
  the project panel), track ids (`Session::giveTrackIds` / `giveTrackId`, `App`'s
  `valueTreeChildAdded`), `clip_beat`, `set_tempo` and the tempo box (`App::tempoAsk`,
  `answerTempoAsk`, `Ui::drawTempoPrompt`).
- Tested: `taw --mcptest` (81 checks, all ok; in CI on Windows and macOS), the other
  device-free self-tests and `--duptracktest` unchanged, clean under MSVC `/W4` and clang-cl's
  strict set; `taw --mcp` piped from a shell against `taw --serve` (every line it writes is JSON).

Not built yet, and open:

- `set_track`, `set_session`, `move_clip`, `write_cc`, automation as tools (the passthrough
  covers them meanwhile).
- Stretching audio to a new tempo (the box shows it as coming).
- The tempo box takes the keys until it's answered, space included. If that's too much while
  the user is mid-idea, it could sit on the status line instead and wait.
- **A removed track that's ringing** is cut, as it is for a person. A short fade on removal
  would help both.
- **macOS:** nothing Windows-only, but not yet built or run there.
- **Firewall:** the socket is on 127.0.0.1 only, which Windows doesn't usually ask about; if it
  does, allow it (or deny: nothing outside the machine needs it).

## Closed sessions (branch `mcp-closed-sessions`)

Status (2026-11-02): built on the branch (see "As built" at the end of this section).

Agents can read and edit a session that isn't the one open in taw: while the user is on the
session screen auditioning bounces, or working in another session.

### Decided by the user

1. **Edits go into the session's `unsaved\` work folder**, the saving model's own: nothing is
   written to `session.json`. When the user opens the session they see the agent's edits as
   unsaved changes; ctrl+s keeps them, closing without saving drops them. An `unsaved\` folder
   that's already there (the user's own changes, left by closing the window or a crash) is built
   on: the session is read from it, as an open would.
2. **Opening it hands over live.** If the user opens a session while an agent is editing it in
   the background, the background edits finish, taw opens the session with them in it, and the
   agent carries on against the live session under the usual rules (the focused track and the
   time selection locked, refused while playing, and so on).

### The argument

Every session-scoped tool (`get_session`, `get_track`, `add_track`, `remove_track`,
`write_notes`, `write_hits`, `remove_clip`, `add_marker`, `set_tempo`, `run_command`,
`get_parameters`, `write_automation`) takes an optional `session`: a folder (full path) or a
name as `list_sessions` gives it, found the way `open_session` finds one.

- Left out: the open session, as before (`no-session` with none open).
- The open session (by name or folder): exactly as before.
- Another session: a **background copy** of it in the running taw (below). Not found: `invalid`.
- The open session while it's opening or closing: `busy`, as before.

### The background copy

A second, headless `App` in the same process, made for the session the first time an agent
names it (`App::backgroundFor`), and kept in the running taw's `backgroundSessions`. Chosen
over a separate process because it runs exactly the code the open session runs (commands,
kits, routing, automation, plugins), and handing it over is moving one object.

- **No audio, no screen, no user.** It never opens an audio device (its engine has none: the
  device is only opened by `run()`), never builds a playback graph, opens no windows, MIDI
  inputs or socket, and writes nothing app-wide: not `settings.json`, the recent sessions, the
  run marker, the hub's client name, nor the crash-time emergency save (a background `App` is
  made with `background = true`, which skips them).
- **Its saves go to `unsaved\`** (`interactive = true` from the start, so even its first
  write, on load, goes there), after every call that changed something; with that,
  `unsaved\agent-edits.json` names the agents, so an open later says whose changes they are.
- **It holds the session's lock** (`.taw-lock`, this taw's pid) while it's loaded, as an open
  session does, so `taw --exec` or another taw can't write over it meanwhile.
- **Runs on the message thread, one call at a time**, from `pollAgents`, like every call.
  Nothing it does touches the open session, its engine or its undo history.
- **Plugins load only when a call needs one:** `get_parameters`, `write_automation` on a
  plugin parameter, `add_track` with a plugin, and `run_command` verbs that load one (`inst`,
  `fx`, `ins`). Each loads in the background copy's own host (at the session's rate), never in
  the live graph; reads like `get_session` load none. Loading a plugin blocks the message
  thread for as long as the plugin takes (as it does when a person loads one), so while the
  open session plays or records those calls answer `playing` instead (the rest still work).
- **Unloaded after 2 minutes idle** (`backgroundIdle`), and at most 3 at a time (the one
  used least recently goes first): its plugin states written to `unsaved\plugins\` if it
  changed anything, its lock let go. An agent that comes back gets a fresh copy, read from
  `unsaved\`. Quitting taw unloads them all the same way.

### The rules in the background

- **Hands-off doesn't apply:** there's no cursor, focus or time selection of the user's in a
  closed session, and no recent edits of theirs. Reads give no `locked`, and `cursor_beat`,
  `focused` and `selection` are null. `get_session` says `"open": false`.
- **Playing:** the open session playing doesn't hold up background edits (they can't be heard),
  except those that load a plugin (above).
- **Busy:** an import (a sample file, `import` in `run_command`) runs on its own in the
  background copy and is answered when it's in, as for the open session; meanwhile other calls
  to that session say `busy`.
- **The status line** says what an agent did elsewhere ("claude-code, in Song B: wrote 4 notes
  …"), so the user knows.

### Undo

The background copy keeps its own undo history, one step per call named for the agent, as
today. **Handing over moves the whole `Session`** (tree and `UndoManager`) into the open taw, so
once the user opens it every agent edit made since the copy was loaded is a ctrl+z step. If the
copy was unloaded (idle) before the user opened it, the edits are still there as unsaved
changes, but not as undo steps: the history went with the copy. (Keeping histories after
unloading would mean keeping the trees in memory with the locks held; two minutes covers the
case that matters, the user opening it while the agent works.)

### Opening it: the hand-over

Every way the user opens a session (`startOpening`: the session screen, `open_session`, a
switch the box allowed; and a typed name on the template picker) goes through
`App::loadSession`, which takes the background copy's `Session` if there is one, instead of
reading the folder again:

1. If the copy is mid-call (an import running), the open waits for it: `openWhenHandedOver`
   holds the folder, the status line says "finishing claude-code's edit to Song B", and new
   calls to that session say `busy` until it's open. `pollAgents` answers the import, then
   opens.
2. The copy writes its plugin states to `unsaved\plugins\` and its tree to `unsaved\`, lets go
   of its listener and plugins, and gives up the `Session` (and its lock, which is this taw's
   anyway). The copy is then deleted.
3. The open taw attaches it as if it had read it from `unsaved\`: unsaved changes, with a note
   naming the agents ("opened with claude-code's changes from while it was closed, not saved
   yet · ctrl+s keeps them, ctrl+z takes them back"). Audio and plugins load as on any open.
4. The agent's next call naming that session finds it open and gets the live rules.

### Tempo on a closed session

With no audio clips, `set_tempo` goes straight through (MIDI and hits follow the beats). With
audio clips the user decides, as before, but they can't be asked about a session that isn't
open, so:

- The call changes nothing and answers `needs-user`: "Song B has audio clips: taw asks the user
  what to do with them when they open it". The request is kept in the running taw
  (`pendingTempo`, one per session, the latest wins; not on disk, so it survives the copy
  being unloaded but not taw quitting).
- When the user opens that session, the tempo box comes up at once with the agent's request.
  From then on it's the open session's flow: calling `set_tempo` again hears the choice.
- `get_session` on the closed session shows it as `tempo_request: { status: "waiting for the
  user to open the session" }`.

The alternative, applying it with the audio kept in time (the safe choice), was turned down: it
decides for the user, which is what the box is there to avoid.

### Tests (`--mcptest`, added)

From the session screen: read and edit a closed session (tracks, notes, a marker, the
passthrough), its `session.json` untouched and `unsaved\` holding the edits, one undo step each
in the copy, no `locked`; with session A open, edit session B (sample track, hits, an import
answered when it's in), A untouched (its tracks, unsaved flag and undo history), reads without
`session` still A's; the tempo on B with audio waiting for the user; unloading after idle (the
lock gone, `unsaved\` kept) and loading again from `unsaved\`; the hand-over while an import is
under way (the open waits, then opens B with every edit, unsaved, the agent's steps on ctrl+z,
the tempo box up); the agent carrying on against the live B (the focused track locked, playing
refused); a save writing it all to `session.json`.

### As built

- `App.h/.cpp`: `App (..., bool inBackground)` makes the copy (`background`: no emergency save,
  no settings written, not remembered as recent, no run marker or hub name; `interactive` from
  the start). `attachSession` raises a `pendingTempo` request as the tempo box (unless the
  tempo is there already). `startOpening` waits for a busy copy (`openWhenHandedOver`) and
  reads through `loadSession`; `createSession` (a typed name that's an existing session) does
  too, refusing while a copy is mid-call.
- `AgentEdits.cpp`: `App::agentCall` routes by `session` (exactly a folder or a name: a part of
  a name is refused with the matches, since it could pick the wrong session to change);
  `backgroundFor`, `backgroundBusy`, `pollBackgroundSessions` (from `pollAgents`: imports
  answered, a waiting open, idle copies let go), `noteBackgroundCall` (the copy's tree and
  plugin states to `unsaved\` after each call that changed it, `agent-edits.json`, the status
  line), `giveUpSession` and `loadSession` (the hand-over). `AgentTools` has `live` (the taw a
  copy is in): no locks, `get_session`'s `open`/`note`, the pending tempo, `pluginsWait`.
  `list_sessions` gives each session's `unsaved_changes`.
- `Agents.cpp`: `toolList` adds `session` to every session tool's schema in one place; the
  bridge's instructions say how to use it. `Session::load` names the agents from
  `agent-edits.json` in its note.
- Tested: `taw --mcptest` 167 checks (49 new), all ok; `--keycasttest`, `--counttest`,
  `--livetest`, `--cliptest`, `--paralleltest`, `--bouncetest`, `--duptracktest` unchanged.

Open:

- **Undo after an idle unload** is gone (the edits stay, as unsaved changes). If agents work in
  long gaps, `backgroundIdle` could grow, at the cost of holding the lock longer.
- **A copy is a whole `App`**: its engine starts its disk thread and render helpers though
  nothing plays. Cheap enough for three; an engine that starts them only with a device would
  make it nothing.
- **Plugin loads still block the screen** for as long as the plugin takes (refused while
  playing). Loading them on another thread is a bigger change, for everyone's plugins.
- The tempo request isn't on disk: quitting taw before opening the session forgets it (the
  agent's next `set_tempo` asks again).
- `--savetest` crashes after "right stays in the area" on this machine, with and without these
  changes (it isn't in CI): worth a look on its own.
