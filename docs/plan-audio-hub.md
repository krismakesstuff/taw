# Plan: the taw audio hub

Status: planned, not started (2026-10-07). Written so another agent can pick it up. Read
`docs/design.md` first (Threads, Recording, Routing and inserts, Delay compensation).

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

1. **Session lock.** On opening, write `<session>/.taw-lock` (pid, host, time). If it
   names a live taw (`isTawRunning` in `Safety.cpp`, private to it now: export it), refuse with "open in another taw
   (pid N)". Offer read-only later if wanted. A stale lock (dead pid) is taken over. Removed
   on close. Headless `--exec` checks it too.
2. **Shared settings.** `saveSettings` and the library's save re-read the file and merge
   only the keys this process changed, as `rememberSession` does. Or simply re-read before
   each write and write atomically (temp file, then rename).

## Step 1: the hub

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
