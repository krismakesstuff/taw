# Plan: the taw audio hub

Status (2026-10-09), macOS:
- **Built:** steps 0 and 1, the hub's own page (a routing matrix, an interface picker, who
  uses each app device), and step 3's virtual devices (taw.driver, "taw 1" to "taw 4").
  App → taw 1 → hub → taw 2 → app is checked with `--apptest`.
- **Next:** trying it with Zoom, then step 2 (process taps).
- **Windows:** builds without the hub (`TAW_WITH_HUB` is off there); step 0 (the session
  lock, settings merging) and `dev`'s input choice work there. See "Windows: what the hub
  needs" below.
- **Where to read:** "Step 1 as built" below, and the Hub section of `docs/design.md`.

Written so another agent can pick it up. Read `docs/design.md` first (Threads, Recording,
Routing and inserts, Delay compensation, Hub).

## Why

Today each taw opens the audio interface itself (`juce::AudioDeviceManager` in
`AudioEngine`). That has three problems:

- ASIO drivers (the user's SSL 12) usually take one program at a time, so a second taw gets
  no audio.
- There's no way to route audio between taw sessions, or between taw and other apps. The user
  runs Voicemeeter for that today.
- Nothing captures another app's audio (a Zoom call, a browser) to record ideas from.

The answer is a **hub**: one background taw process that owns the audio interface. taw
sessions connect to it as clients, and it routes between the hardware, the sessions and
(phase 2) other apps' audio.

## Scope

| Step | What | Status |
|---|---|---|
| 0 | Groundwork: session lock, shared settings files that don't clobber each other | **this plan** |
| 1 | The hub: owns the device, sessions connect as clients, a routing matrix | **this plan** |
| 2 | App capture: another program's audio (Zoom, a browser) as a hub source | **this plan** |
| 3 | A virtual audio device driver, so Zoom etc. can pick taw as a mic or speaker | **later**, not in this plan |
| 4 | Network audio (sessions on other machines, "OTA") | **later**, not in this plan |

For 3 and 4 the design must not paint us into a corner (see "Later" at the end), but
nothing is built for them yet.

## macOS first (decided with the user, 2026-10-08)

The hub is built and tried on macOS first, because there the virtual device (step 3) is a
user-space driver instead of a signed kernel driver. What the user wants to try: an app's
audio into taw, taw's mix out to an app (Zoom, a dedicated feed), and apps routed to each
other, Voicemeeter-style. Windows follows with the same hub and routing, minus the driver
(VB-Cable stands in there). Where this section and the steps below disagree, this section
wins on macOS.

**Order on macOS:** step 0, step 1, then the virtual devices (step 3, pulled forward), then
app capture with process taps (step 2). MIDI stays out for now.

**Decisions:**

- **Sessions connect over shared memory** (the "taw hub" device type in step 1), not through
  the driver. Considered and turned down: sessions using driver devices as their audio
  device, which needs no client code but adds a buffer of latency and does nothing for
  Windows.
- **The hub is always on.** A launchd user agent (`~/Library/LaunchAgents`, RunAtLoad,
  KeepAlive) starts it at login, so routes between apps work with no taw open. taw installs
  it (`taw --install-hub`, no admin), offering to the first time `hub` is used. This
  replaces "started on demand, exits 30 s after the last client" on macOS. `hub stop` and
  `hub start` unload and load the agent.
- **Only chosen apps go through the hub.** It isn't the Mac's default output; system sounds
  and other apps play to the interface as usual.
- **Virtual devices: several stereo ones**, "taw 1" to "taw 4", each a speaker (apps play
  into it) and a mic (apps record what the hub sends it). Apps like Zoom only use channels
  1-2, so each needs a device of its own.
- **App capture: devices first, process taps later.**

**The driver** (`taw.driver`, an AudioServerPlugIn built in this repo, starting from
Apple's NullAudio sample; installed to `/Library/Audio/Plug-Ins/HAL` by
`taw --install-driver`, which asks for an admin password and restarts coreaudiod; ad-hoc
signed is enough on the user's own Mac):

- coreaudiod runs plug-ins sandboxed, as its own user: it can't reach a per-user agent. So
  the driver talks to nothing. Each "taw N" has a hidden partner, "taw N (hub)"
  (`kAudioDevicePropertyIsHidden`). The partner's input is what apps play into taw N; the
  partner's output is what apps record from taw N. Plain loopback in the driver.
- The hub opens one private aggregate device: the interface (clock master) plus the hidden
  partners, with drift correction on the partners. One IO callback runs the whole matrix.
- Rates 44.1, 48, 88.2 and 96 kHz; the hub sets the partners to the interface's rate. The
  driver reports its buffering as latency.
- With the hub stopped the devices still run, silent.

**Routing nodes on macOS:** interface inputs and outputs, taw 1-4 (as speaker sources and
mic destinations), sessions' inputs and outputs, later taps. App to app is a route from
taw 1's speaker to taw 2's mic.

**Shared-memory transport on macOS** (step 1): `shm_open` for the region; the per-block
signal with `os_sync_wait_on_address` on a counter in it (macOS 14.4+, shared flag), since
macOS lacks timed POSIX semaphores. Look into the client's render thread joining the hub's
audio workgroup (`os_workgroup`, its port sent from the hub), or Apple Silicon may run it on
efficiency cores and it misses blocks.

**Check first in step 1: permissions.** A launchd agent isn't under the terminal, so macOS
asks about the microphone (the interface's inputs) for the hub itself. A bare binary may get
no prompt at all. The hub probably needs to be a minimal app bundle ("taw hub.app", same
code, `NSMicrophoneUsageDescription` and later `NSAudioCaptureUsageDescription` for taps)
that the agent runs. Prove opening the interface's inputs from the agent before building on
it.

**Step 2 on macOS:** Core Audio process taps (macOS 14.2+). A tap joins the hub's
aggregate, where CoreAudio corrects its drift, so macOS needs no `DriftResampler` (Windows
still does). Adding or removing one restarts the hub's device: a short gap.

## How things are today (what to read)

- `src/AudioEngine.h/.cpp`: `AudioEngine` is a `juce::AudioIODeviceCallback`. Its
  `audioDeviceIOCallbackWithContext` renders the whole graph (tracks, buses, mix, click) each
  block. `audioDeviceAboutToStart` calls `startRunning` with the rate, buffer size and input
  count. `runWithoutDevice` and `renderWithoutDevice` drive it with no device (tests use
  them).
- `src/App.cpp`:
  - `dm.initialise (AudioEngine::maxInputs, 2, saved, false)` (around line 400) opens the
    device from `%APPDATA%\taw\audio-device.xml`.
  - `chooseDevice` is the `dev` picker.
  - A session's sample rate switches the device's rate (around line 540; the SSL 12 takes
    about 12 s).
  - `latencyCompensation()` and `heardPlayhead()` use the device's input and output latency
    plus `mixLatency`.
- `src/Safety.cpp`: run markers in `%APPDATA%\taw\running\<pid>.json` already tolerate
  several taws running (only dead pids count as crashes).
- Shared config files in `%APPDATA%\taw\`:
  - `settings.json` (`App::saveSettings`), `library.json`, `audio-device.xml`: written whole
    from memory, so the last writer wins.
  - `recent.json` (`rememberSession`): re-read before it's written.
- Sessions: nothing stops two taws opening the same session folder. The last `session.json`
  save wins (`backups/` keeps copies made on open).
- MIDI inputs (`src/Midi.cpp`, `MidiInputs`): opened per process. WinMM ports are often
  exclusive, so a second taw may not get the controller.

## Step 0: groundwork (small, do first)

Done (2026-10-08):
- The lock lives in `Session`: `load` and `create` take it, the destructor drops it. Every
  way in is covered, including `--exec` and the tests.
- Locks name computers by hardware id (`SystemStats::getUniqueDeviceID`), because a Mac's
  hostname changes with the network.
- `safety::tawRunning` and `safety::thisProcess` are the exported checks.
- `saveSettings` merges the keys it changed.
- Every config write was already atomic: JUCE's `replaceWithText` and `XmlElement::writeTo`
  write a temporary file and rename it. The library index is written whole: two taws indexing
  at once means the last one wins, which is acceptable.

1. **Session lock.** On opening, write `<session>/.taw-lock` (pid, host, time). If it
   names a live taw (`isTawRunning` in `Safety.cpp`, private to it now: export it), refuse with "open in another taw
   (pid N)". Offer read-only later if wanted. A stale lock (dead pid) is taken over. Removed
   on close. Headless `--exec` checks it too.
2. **Shared settings.** `saveSettings` and the library's save re-read the file and merge
   only the keys this process changed, as `rememberSession` does. Or simply re-read before
   each write and write atomically (temp file, then rename).

## Step 1: the hub

### Step 1 as built (macOS, 2026-10-08)

What `docs/design.md` (Hub) describes. Where it differs from the plan below:
- **Control:** a local TCP socket (`InterprocessConnectionServer` on 127.0.0.1, its port in
  `hub-info.json`), not a named pipe. JUCE's named pipes are one connection each.
- **Sessions' inputs:** at least 8, more than the interface has when it's small.
- **Hub commands:** `hub forget <session>`, because sessions seen once stay in `hub.json`.
- **Permission (checked):** a launchd agent running "taw hub.app" gets the microphone prompt
  and keeps the answer.
- **Rebuilds:** a rebuild re-signs the bundle ad hoc, and macOS asks again. A fixed local
  signing identity would stop that (not done: it means a certificate in the login keychain;
  ask).

Not done yet in step 1:
- **requestRate:** a session at another rate plays off-speed on the hub, as it would on a
  device at the wrong rate.
- **Workgroups:** joining the hub's audio workgroup isn't done. No late blocks so far in tests
  at 256 samples.
- **The UI, checked in a pseudo-terminal:**
  - A session on "taw hub" joins, and its I/O line shows the hub.
  - The hub page works from `taw hub`, from tab on the start screen, and from `hub` in a
    session.
- **The hub's interface:** chosen with `i` on the page, a picker (`hub-device.xml`).

### Windows: what the hub needs (not started)

What's portable already: `HubServer` (routes, the plan, the engine), `HubPanel` (the page
and commands, apart from who uses an app device), `HubDevice` (the "taw hub" device type)
apart from its waiting, and the JSON control socket (`juce::InterprocessConnection`). What
isn't, file by file:
- **`HubShared.cpp`:** `shm_open` → `CreateFileMapping` / `MapViewOfFile` on a named
  mapping; `os_sync_wait_on_address` → a named auto-reset event per client each way
  (`WaitOnAddress` doesn't cross processes).
- **`Hub.cpp` (starting at login):** launchd → a per-user Run key or a Task Scheduler logon
  task; no admin either way. The "taw hub.app" bundle is macOS only (it's there for the mic
  permission); on Windows the hub is taw.exe --hub.
- **`HubDevice.cpp`:** the render thread's real-time priority → MMCSS
  (`AvSetMmThreadCharacteristics ("Pro Audio")`).
- **The interface:** ASIO, one device both ways, which is the case the hub exists for (ASIO
  takes one program at a time; the hub is that program).
- **App devices:** no driver (see "Later"). VB-Cable or Voicemeeter's devices stand in, but
  they're WASAPI devices on their own clock beside an ASIO interface, so they need the
  `DriftResampler` from step 2 before they can be hub ends.
- **Who uses an app device:** `IAudioSessionManager2` per device in place of macOS's process
  list.
- **`TAW_HUB` on Windows:** once the above is in, `TAW_WITH_HUB` turns on for WIN32 too.

Step 3 on macOS, as built:
- **Taken from the design:** what the "macOS first" section describes, with one hidden
  8-channel device in place of a hidden partner per device.
- **Rates:** 8 to 96 kHz; the low ones are for Bluetooth headsets.
- **To watch:**
  - The driver's rate is shared by every taw device, so an app picking another rate moves
    all of them. So far the aggregate's drift correction carries a hub at another rate.
  - The round trip app → hub → app was 111 ms on a 16 kHz headset at 512 samples. An
    interface at 48 kHz with smaller blocks will be much less.

### Processes

- **Same executable**, `taw --hub`: headless, no console UI. It needs the same JUCE audio
  and device code and the same config folder, and one binary keeps installs simple.
- **Started on demand.** The first taw that wants audio tries to connect. If no hub
  answers, it starts `taw --hub` detached and retries for a few seconds.
- **Lifetime.** The hub stays up while it has clients. It exits a short while (say 30 s,
  in `hub.json`) after the last client leaves, so reopening a session doesn't re-open the
  driver; the SSL 12's switch is slow. `hub stop` ends it.
- **One hub per user.** A named mutex (Windows) or a lock file (POSIX) guards against two.
- **If the hub dies,** clients see their device stop: today's "no audio device" state.
  They reconnect when it comes back, so a client must never crash on a missing hub.
- **Its own crash handling,** via `safety::installCrashHandler` and a run marker with
  `"role": "hub"`. A hub marker must not trigger the session-recovery prompt in clients.

### The client side: the hub as a JUCE device type

The least invasive way into taw is a new `juce::AudioIODeviceType` called **"taw hub"**
(`src/HubDevice.h/.cpp`), added to the engine's `AudioDeviceManager` with
`addAudioDeviceType`. Its `juce::AudioIODevice`:

- `open` connects to the hub (starting it if needed) and asks for N inputs and 2 outputs.
- It runs a high-priority thread that waits on the hub's "block ready" signal, calls the
  `AudioIODeviceCallback` (the engine) with the shared-memory buffers, then signals back.
- It reports `getCurrentSampleRate`, `getCurrentBufferSizeSamples` and, importantly,
  `getInputLatencyInSamples` / `getOutputLatencyInSamples` as the hardware's plus the hub's
  added block(s). Then `latencyCompensation()`, `heardPlayhead()`, the video clock and
  recording placement stay correct with no other change.

So `AudioEngine` doesn't change. `dev` lists "taw hub" next to ASIO, WASAPI and the rest,
and it becomes the default when a hub is configured. Choosing a real device directly still
works, the way taw runs today.

### Transport between hub and clients

- **Control:** `juce::InterprocessConnection` over a named pipe (`taw-hub-<user>`).
  Messages are small JSON:
  - `hello {pid, session, inputs, outputs, rate}`
  - `welcome {rate, block, latencyIn, latencyOut, sharedMemoryName}`
  - `routes`, `setRoute`, `stats {xruns, late}`
  - `requestRate`
  - `bye`
- **Audio:** one shared-memory region per client (Windows `CreateFileMapping` plus events;
  POSIX `shm_open` plus semaphores). JUCE has no class for this, so it's per platform,
  behind a small interface. Each region holds an input ring and an output ring of float
  blocks, plus counters.
- **Real time:**
  - The hub's device callback never waits on a client. Each block it writes every
    client's inputs, signals it, and mixes in what each client wrote **last** cycle. That
    is one block of extra output latency, reported as above.
  - A client that misses its slot plays silence for that block and counts a `late`. Other
    clients and the hardware are never held up.
  - No allocation or locks on either audio thread. Follow the engine's existing rules:
    see `SpscQueue.h`, and how `Graph`s are swapped.

### Sample rate and block size

- The hub runs the hardware at one rate and block size, and every client runs at the
  hub's.
- A session at a different rate asks the hub through `requestRate`. The hub switches only
  if that client is alone. Otherwise it refuses with a clear message ("the hub is at 48 kHz
  for 2 other sessions"), and the session plays off-speed exactly as it would today on a
  device at the wrong rate.
- Resampling clients is not in step 1.

### Routing

- **Nodes:** hardware inputs and outputs, each client's inputs and outputs, and (step 2)
  app captures.
- **Routes:** a matrix of source channel to destination channel, each with a gain.
- **Defaults, so nothing changes for one session:**
  - Every client gets every hardware input on its inputs 1..N.
  - Each client's outputs 1-2 go to hardware outputs 1-2, summed across clients.
- **Session to session:** client A's outputs into client B's inputs. That adds a block of
  delay, which the UI should say.
- **Saved** in `%APPDATA%\taw\hub.json`. Routes name clients by session folder, not pid,
  so they survive a restart.
- **In taw:**
  - A `hub` command opens a panel: the clients, the sources and where each goes. It's a
    list, not a big grid, so it fits 80x24 (a design rule: taw doesn't need a large
    screen).
  - Text forms: `hub`, `hub route <source> > <dest>`, `hub unroute ...`, `hub stop`.
  - A track input can name a hub source: `2 in zoom` in step 2.

### MIDI (decide during step 1)

MIDI inputs have the same one-program problem. Option: the hub opens the MIDI inputs too and
forwards events to clients over the shared memory, with timestamps. Clients keep their
current `MidiInputs` path when connected straight to a device. Ask the user before
building this. It may be enough that the focused session takes the controller.

### Tests

- `taw --hubtest`: starts a hub on no device (`runWithoutDevice`-style, driven by a timer
  thread) and two clients in-process or as child processes. It checks:
  - sample-exact pass-through, input to client to output;
  - reported latency equals measured latency;
  - a stalled client doesn't glitch the other;
  - clients reconnect after the hub restarts.
- The existing tests (`--counttest`, `--livetest`, `--cliptest`, `--bouncetest`) must
  still pass with a direct device.
- Close test processes with taskkill without /F (sent twice if needed). Never touch the
  user's running taw.

## Step 2: app capture

Another program's audio as a hub source, for recording ideas from a Zoom call or a reference
in the browser.

- **Windows: process loopback.** `ActivateAudioInterfaceAsync` with
  `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK` and
  `PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE` captures one process (and its
  children) without a driver. Microsoft's "ApplicationLoopback" sample shows it.
  - **First task: confirm the minimum Windows build.** The user is on Windows 10 22H2,
    build 19045. If process loopback isn't available there, fall back to plain WASAPI
    loopback on the default render device, which captures everything playing. Say so in
    the UI.
- **Finding apps:** `IAudioSessionManager2` lists processes with audio sessions.
  `hub apps` lists them by name, e.g. `hub capture zoom`.
- **Clock drift:** a capture runs on its own clock, not the interface's.
  - Each capture needs an asynchronous resampler that tracks drift. A ring buffer's fill
    level feeds a slow control loop that nudges the resampling ratio around 1.0.
  - Write this as a reusable `DriftResampler`. The "later" steps need it too.
- **Capture rate:** captures arrive at the system mixer's rate (often 48 kHz) and are
  resampled to the hub's.
- **In taw:**
  - A capture is a source in the hub's routing, routed to a client's inputs.
  - A track can record it: `2 in zoom` names the source. It records like any input, with
    latency compensation counting the capture's buffering.
- **macOS:** possibly Core Audio process taps (macOS 14.2+) or ScreenCaptureKit audio
  (macOS 13+). Not verified; look into it when the Mac build needs it.
- **Privacy:** capture only on a command from the user, show a lit "capturing zoom" in the
  transport while active, and never start one automatically.

## Later (not in this plan)

**3. Virtual audio device driver**, so Zoom and other apps can choose "taw" as their
microphone or speaker.
- **Windows:** a kernel-mode audio driver (start from Microsoft's SysVAD sample). For users
  to install it, it needs an EV code-signing certificate (a yearly cost) and Microsoft
  attestation signing per build through Partner Center. A bug can crash the machine, not
  just taw.
- **macOS:** much easier: a user-space AudioServerPlugIn (like BlackHole).
- **Until then:** route a hub output into VB-Cable (which the user has with Voicemeeter)
  and pick that in Zoom.
- **Design hook now:** a driver would just be one more hub node. Keep the hub's node and
  route model general (named endpoints, any channel count), with no special cases for
  "the hardware".

**4. Network audio ("OTA")**: clients or hubs on other machines.
- **Design hook now:** the shared-memory transport sits behind an interface, so a network
  transport can be another implementation. `DriftResampler` (step 2) handles the clock
  difference.
- It needs jitter buffers, latency reported to the session, and probably uncompressed PCM
  on a LAN first.

## Questions to ask the user before or during the work

1. Should the hub replace Voicemeeter entirely (system sounds and every app to the
   speakers)? Or only sit between taw sessions, the interface and chosen apps? This plan
   assumes the latter.
2. MIDI through the hub, or not (see above)?
3. Should the hub show anything outside taw (a tray icon)? This plan assumes not: it's
   headless and managed from taw's `hub` panel.

## House rules for whoever builds this

- Branch per step; commit there; **never push** until the user says.
- C++20, JUCE, match the surrounding style and comment density.
- Edit C++ with string escapes (`\x2192`) using the editor tools, not shell `sed`/Python
  one-liners: shell edits have corrupted backslashes before.
- Update `docs/design.md` (a "Hub" section) and the help pages (`Ui::drawHelp`, keep each
  entry within its column) as features land.
