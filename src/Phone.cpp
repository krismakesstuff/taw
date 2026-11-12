// The phone link: a very small HTTP server (one page, one upload) for the local network.

#include "Phone.h"
#include "PhoneTls.h"

#include <qrcodegen.hpp>

#include <atomic>
#include <list>
#include <thread>

namespace taw
{
juce::File configFolder(); // Session.h

namespace
{
constexpr int firstPort = 8317, portsToTry = 12;
constexpr juce::int64 maxUpload = 500LL * 1024 * 1024;

// The page. One file: no scripts or styles from anywhere else.
const char* const page = R"html(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>taw</title>
<style>
  :root { color-scheme: dark; }
  [hidden] { display: none !important; }
  body { margin: 0; padding: 24px 16px; background: #14161a; color: #e8e6e1; font: 17px/1.4 system-ui, sans-serif; }
  h1 { font-size: 20px; margin: 0 0 4px; }
  p { margin: 0 0 20px; color: #9a9890; overflow-wrap: anywhere; }
  p b { color: #e8e6e1; }
  p.off { color: #e0705c; }
  .what { display: flex; align-items: center; gap: 6px; margin: 0 0 12px; }
  .what span { color: #9a9890; margin-right: 4px; }
  .what label { flex: 1; }
  .what input { position: absolute; opacity: 0; }
  .what b { display: block; padding: 10px 4px; border-radius: 10px; background: #2a2e35; text-align: center; font-weight: 500; font-size: 15px; }
  .what input:checked + b { background: #e8e6e1; color: #14161a; }
  select { width: 100%; margin: 0 0 12px; padding: 12px; border-radius: 10px; border: 0; background: #2a2e35; color: #e8e6e1; font: inherit; }
  #hint { font-size: 14px; margin: 0 0 16px; }
  .button { display: block; box-sizing: border-box; width: 100%; margin: 0 0 12px; padding: 18px; border: 0; border-radius: 12px; text-align: center; font: inherit; font-weight: 600; }
  .record { background: #c8452f; color: #fff; }
  .record.on { background: #e8e6e1; color: #14161a; }
  .tempo { display: flex; align-items: center; gap: 10px; margin: 0 0 12px; color: #9a9890; font-size: 15px; }
  .tempo input[type=number] { width: 76px; padding: 10px; border-radius: 10px; border: 0; background: #2a2e35; color: #e8e6e1; font: inherit; }
  .tempo input:disabled { opacity: 0.6; }
  .tempo label { flex: 1; }
  #pulse { margin: 0 0 12px; padding: 18px; border-radius: 12px; background: #2a2e35; text-align: center; }
  #pulse.beat { background: #e8e6e1; color: #14161a; }
  #pulse.beat.one { background: #c8452f; color: #fff; }
  #pulse b { display: block; font-size: 44px; line-height: 1.1; }
  #pulse small { color: inherit; opacity: 0.7; }
  a { color: #e8e6e1; }
  .pick { background: #2a2e35; color: #e8e6e1; }
  input[type=file] { display: none; }
  ul { list-style: none; margin: 20px 0 0; padding: 0; }
  li { padding: 12px 0; border-top: 1px solid #2a2e35; overflow-wrap: anywhere; }
  li small { display: block; color: #9a9890; }
  li.ok small { color: #7fbf7f; }
  li.bad small { color: #e0705c; }
  .bar { height: 4px; margin-top: 8px; border-radius: 2px; background: #2a2e35; }
  .bar i { display: block; height: 4px; width: 0; border-radius: 2px; background: #e8e6e1; }
</style>
</head>
<body>
<h1>taw</h1>
<p id="session">&nbsp;</p>
<div class="what">
  <span>Make it</span>
  <label><input type="radio" name="as" value="audio" checked><b>a recording</b></label>
  <label><input type="radio" name="as" value="drums"><b>drums</b></label>
  <label><input type="radio" name="as" value="notes"><b>notes</b></label>
</div>
<select id="inst" hidden></select>
<p id="hint"></p>
<div id="click" hidden>
  <div class="tempo">
    <span>Tempo</span><input id="bpm" type="number" min="40" max="240" step="1" inputmode="numeric">
    <label><input id="during" type="checkbox"> click while recording (headphones)</label>
  </div>
  <button id="go" class="button record">Record with a count-in</button>
  <div id="pulse" hidden><b id="count"></b><small id="counting"></small></div>
  <button id="cancel" class="button pick" hidden>Cancel (nothing is sent)</button>
</div>
<p id="plain" hidden>To record here with a count-in, open <a id="secure">the secure page</a> (the phone asks once whether to go on).</p>
<label class="button record" id="old">Record now<input id="record" type="file" accept="audio/*" capture></label>
<label class="button pick">Upload a file<input id="pick" type="file" accept="audio/*,.m4a,.mp3,.wav,.aac,.flac,.ogg,.aif,.aiff,.wma" multiple></label>
<ul id="list"></ul>
<script>
  const list = document.getElementById('list');
  const session = document.getElementById('session');
  const home = location.pathname.replace (/\/$/, '');
  let queue = Promise.resolve();

  // Which session this goes into, asked again every few seconds (it can change, or taw can go).
  function showSession (name, reached) {
    session.className = reached ? '' : 'off';
    session.textContent = '';

    if (! reached)
      session.textContent = "can't reach taw (is the phone link still on?)";
    else if (name) {
      session.append ('Recordings go into ');
      const b = document.createElement ('b');
      b.textContent = name;
      session.append (b);
    }
    else
      session.textContent = 'No session open in taw: recordings wait for one.';
  }

  function ask () {
    fetch (home + '/session', { cache: 'no-store' })
      .then (r => r.ok ? r.text() : Promise.reject())
      .then (name => showSession (name, true))
      .catch (() => showSession ('', false));
  }

  ask();
  setInterval (ask, 4000);

  // What the recording is to become: remembered on this phone.
  const inst = document.getElementById('inst');
  const hint = document.getElementById('hint');
  const kept = key => { try { return localStorage.getItem (key); } catch (e) { return null; } };
  const keep = (key, value) => { try { localStorage.setItem (key, value); } catch (e) {} };
  const chosen = () => document.querySelector ('input[name=as]:checked').value;

  function showChoice () {
    const as = chosen();
    inst.hidden = as !== 'notes';
    hint.textContent = as === 'drums' ? 'Beatbox it: kicks, snares, hats. taw puts the hits on drum tracks, with the recording beside them, muted.'
                     : as === 'notes' ? 'Hum or sing it. taw writes the notes for the instrument, with the recording beside them, muted.'
                     : 'The recording goes on a track as it is.';
  }

  for (const radio of document.querySelectorAll ('input[name=as]')) {
    radio.checked = radio.value === (kept ('taw.as') || 'audio');
    radio.addEventListener ('change', () => { keep ('taw.as', chosen()); showChoice(); });
  }

  inst.addEventListener ('change', () => keep ('taw.inst', inst.value));

  fetch (home + '/instruments', { cache: 'no-store' })
    .then (r => r.ok ? r.text() : '')
    .then (text => {
      const none = document.createElement ('option');
      none.value = '';
      none.textContent = 'no instrument yet (pick one in taw)';
      inst.append (none);

      for (const name of text.split ('\n').filter (n => n)) {
        const o = document.createElement ('option');
        o.value = o.textContent = name;
        inst.append (o);
      }

      inst.value = kept ('taw.inst') || '';

      if (inst.selectedIndex < 0)
        inst.value = '';
    })
    .catch (() => {});

  showChoice();

  function send (file, click) {
    const as = chosen(), instrument = as === 'notes' ? inst.value : '';
    const clicked = click ? '&at=' + click.at.toFixed (4) + '&bpm=' + click.bpm : '';
    const row = document.createElement('li');
    const note = document.createElement('small');
    const bar = document.createElement('div');
    const fill = document.createElement('i');
    row.textContent = file.name;
    note.textContent = 'waiting';
    bar.className = 'bar';
    bar.appendChild (fill);
    row.appendChild (note);
    row.appendChild (bar);
    list.prepend (row);

    queue = queue.then (() => new Promise (done => {
      const x = new XMLHttpRequest();
      x.open ('POST', home + '/up?name=' + encodeURIComponent (file.name) + '&as=' + as + '&inst=' + encodeURIComponent (instrument) + clicked);
      x.upload.onprogress = e => {
        if (e.lengthComputable) {
          fill.style.width = (100 * e.loaded / e.total) + '%';
          note.textContent = 'sending ' + Math.round (100 * e.loaded / e.total) + '%';
        }
      };
      x.onload = () => {
        const ok = x.status === 200;
        row.className = ok ? 'ok' : 'bad';
        note.textContent = ok ? (x.responseText ? 'in ' + x.responseText : 'in taw, waiting for a session') + (as === 'audio' ? '' : ', being made into ' + as)
                              : (x.responseText || 'taw said no');
        bar.remove();
        done();
      };
      x.onerror = () => {
        row.className = 'bad';
        note.textContent = "couldn't reach taw (is the phone link still on?)";
        bar.remove();
        done();
      };
      x.send (file);
    }));
  }

  for (const id of ['record', 'pick'])
    document.getElementById (id).addEventListener ('change', e => {
      for (const f of e.target.files) send (f);
      e.target.value = '';
    });

)html" /* (in two: a compiler's limit on one string) */ R"html(
  // Recording here, to a click: a bar counted in at the session's tempo, then the take, which
  // starts on beat one. Only a secure page may record (the https address).
  const canRecord = window.isSecureContext && navigator.mediaDevices && navigator.mediaDevices.getUserMedia
                    && (window.AudioContext || window.webkitAudioContext);
  const bpmBox = document.getElementById('bpm'), during = document.getElementById('during'), go = document.getElementById('go');
  const pulse = document.getElementById('pulse'), count = document.getElementById('count'), counting = document.getElementById('counting');
  const cancel = document.getElementById('cancel');
  let beatsPerBar = 4, bpmTouched = false, take = null;

  document.getElementById('click').hidden = ! canRecord;
  document.getElementById('old').hidden = !! canRecord;

  if (! canRecord && location.protocol === 'http:') {
    document.getElementById('plain').hidden = false;
    document.getElementById('secure').href = 'https://' + location.host + location.pathname;
  }

  during.checked = kept ('taw.during') === '1';
  during.addEventListener ('change', () => keep ('taw.during', during.checked ? '1' : '0'));
  bpmBox.addEventListener ('input', () => { bpmTouched = true; keep ('taw.bpm', bpmBox.value); });

  // The session's tempo; with nothing in the session yet, the phone's own (the take sets it).
  function askTempo () {
    fetch (home + '/tempo', { cache: 'no-store' })
      .then (r => r.ok ? r.text() : Promise.reject())
      .then (text => {
        const parts = text.split (' ');
        const bpm = Math.round (parseFloat (parts[0]) || 120), empty = parts[2] === '1';
        beatsPerBar = parseInt (parts[1]) || 4;

        if (take)
          return;

        bpmBox.disabled = ! empty;

        if (! empty)
          bpmBox.value = bpm;
        else if (! bpmTouched)
          bpmBox.value = kept ('taw.bpm') || bpm;
      })
      .catch (() => {});
  }

  if (canRecord) {
    askTempo();
    setInterval (askTempo, 4000);
  }

  // The microphone's samples, each block with the time (the audio clock's) of its first.
  const tap = 'registerProcessor ("tap", class extends AudioWorkletProcessor {'
            + '  process (inputs) {'
            + '    const x = inputs[0] && inputs[0][0];'
            + '    if (x) this.port.postMessage ({ time: currentTime, samples: x.slice (0) });'
            + '    return true;'
            + '  }'
            + '});';

  function wav (samples, rate) {
    const view = new DataView (new ArrayBuffer (44 + 2 * samples.length));
    const text = (at, s) => { for (let i = 0; i < s.length; ++i) view.setUint8 (at + i, s.charCodeAt (i)); };
    text (0, 'RIFF'); view.setUint32 (4, 36 + 2 * samples.length, true); text (8, 'WAVE'); text (12, 'fmt ');
    view.setUint32 (16, 16, true); view.setUint16 (20, 1, true); view.setUint16 (22, 1, true);
    view.setUint32 (24, rate, true); view.setUint32 (28, 2 * rate, true); view.setUint16 (32, 2, true); view.setUint16 (34, 16, true);
    text (36, 'data'); view.setUint32 (40, 2 * samples.length, true);

    for (let i = 0; i < samples.length; ++i)
      view.setInt16 (44 + 2 * i, Math.max (-32768, Math.min (32767, Math.round (samples[i] * 32767))), true);

    return view.buffer;
  }

  function tick (t, time, accent) {
    const osc = t.ctx.createOscillator(), gain = t.ctx.createGain();
    osc.frequency.value = accent ? 1760 : 1175;
    gain.gain.setValueAtTime (0.0001, time);
    gain.gain.exponentialRampToValueAtTime (0.9, time + 0.002);
    gain.gain.exponentialRampToValueAtTime (0.0001, time + 0.045);
    osc.connect (gain).connect (t.ctx.destination);
    osc.start (time);
    osc.stop (time + 0.06);
  }

  async function begin () {
    const bpm = Math.max (40, Math.min (240, Math.round (parseFloat (bpmBox.value) || 120)));
    const t = { bpm, bar: beatsPerBar, beat: 60 / bpm, blocks: [], next: 0 };
    t.stream = await navigator.mediaDevices.getUserMedia ({ audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false, channelCount: 1 } });
    t.ctx = new (window.AudioContext || window.webkitAudioContext)();
    await t.ctx.resume();
    const source = t.ctx.createMediaStreamSource (t.stream), silent = t.ctx.createGain();
    silent.gain.value = 0;

    if (t.ctx.audioWorklet) {
      const url = URL.createObjectURL (new Blob ([tap], { type: 'application/javascript' }));
      await t.ctx.audioWorklet.addModule (url);
      URL.revokeObjectURL (url);
      t.node = new AudioWorkletNode (t.ctx, 'tap', { numberOfInputs: 1, numberOfOutputs: 1, channelCount: 1 });
      t.node.port.onmessage = e => t.blocks.push (e.data);
    }
    else {
      // An older browser: bigger blocks, their time less sure.
      t.node = t.ctx.createScriptProcessor (2048, 1, 1);
      t.node.onaudioprocess = e => t.blocks.push ({ time: e.playbackTime - 2 * 2048 / t.ctx.sampleRate, samples: e.inputBuffer.getChannelData (0).slice (0) });
    }

    source.connect (t.node).connect (silent).connect (t.ctx.destination);

    // A click is heard, and a sound made to it comes back, a little late: the way out and the way in.
    const settings = t.stream.getAudioTracks()[0].getSettings ? t.stream.getAudioTracks()[0].getSettings() : {};
    const known = (t.ctx.outputLatency || 0) + (settings.latency || 0);
    t.late = known > 0 ? known + (t.ctx.baseLatency || 0) : 0.03;
    t.start = t.ctx.currentTime + 0.25;
    t.one = t.start + t.bar * t.beat; // beat one of the take

    // Clicks, set a little ahead of time: the count-in, and on through the take if asked for.
    t.timer = setInterval (() => {
      for (; t.start + t.next * t.beat < t.ctx.currentTime + 0.4; ++t.next)
        if (t.next < t.bar || during.checked)
          tick (t, t.start + t.next * t.beat, t.next % t.bar === 0);
    }, 50);

    const show = () => {
      if (take !== t)
        return;

      const beats = (t.ctx.currentTime - (t.ctx.outputLatency || 0) - t.start) / t.beat, whole = Math.floor (beats);
      const lit = beats >= 0 && beats - whole < 0.25;
      pulse.className = lit ? (((whole % t.bar) + t.bar) % t.bar === 0 ? 'beat one' : 'beat') : '';
      count.textContent = whole < 0 ? '' : whole < t.bar ? String (t.bar - whole) : String (whole % t.bar + 1);
      counting.textContent = whole < t.bar ? 'counting in' : 'recording: bar ' + Math.floor (whole / t.bar);
      requestAnimationFrame (show);
    };

    take = t;
    pulse.hidden = false;
    cancel.hidden = false;
    go.textContent = 'Stop and send';
    go.className = 'button record on';
    requestAnimationFrame (show);
  }

  function finish (t, sendIt) {
    clearInterval (t.timer);
    const until = t.ctx.currentTime;
    t.stream.getTracks().forEach (track => track.stop());
    t.node.disconnect();
    const rate = t.ctx.sampleRate;
    t.ctx.close();
    take = null;
    pulse.hidden = true;
    cancel.hidden = true;
    go.textContent = 'Record with a count-in';
    go.className = 'button record';

    // From a tenth of a second before beat one (as it came back in): a first hit a touch early is kept.
    const lead = 0.1, from = t.one + t.late - lead;

    if (! sendIt || ! t.blocks.length || until < t.one + 0.3)
      return;

    let total = 0;
    for (const b of t.blocks) total += b.samples.length;
    const all = new Float32Array (total);
    let at = 0;
    for (const b of t.blocks) { all.set (b.samples, at); at += b.samples.length; }

    const skip = Math.max (0, Math.round ((from - t.blocks[0].time) * rate));
    const stamp = new Date(), two = n => String (n).padStart (2, '0');
    const name = 'take ' + two (stamp.getHours()) + '-' + two (stamp.getMinutes()) + '-' + two (stamp.getSeconds()) + '.wav';
    const file = new File ([wav (all.subarray (Math.min (skip, total)), rate)], name, { type: 'audio/wav' });
    send (file, { at: lead - Math.max (0, (t.blocks[0].time - from)), bpm: t.bpm });
  }

  // Stops and throws the take away.
  cancel.addEventListener ('click', () => { if (take) finish (take, false); });

  go.addEventListener ('click', () => {
    if (take)
      return finish (take, true);

    go.disabled = true;
    begin().catch (e => {
      const row = document.createElement('li'), note = document.createElement('small');
      row.className = 'bad';
      row.textContent = "couldn't record";
      note.textContent = e && e.name === 'NotAllowedError' ? 'the browser was told not to let this page use the microphone' : String (e && e.message || e);
      row.appendChild (note);
      list.prepend (row);
    }).then (() => { go.disabled = false; });
  });
</script>
</body>
</html>
)html";

// 10.x, 172.16-31.x, 192.168.x, 169.254.x (link-local) and this machine itself.
bool isLocalAddress (const juce::String& address)
{
    const auto parts = juce::StringArray::fromTokens (address, ".", "");

    if (parts.size() != 4)
        return address == "::1";

    const int a = parts[0].getIntValue(), b = parts[1].getIntValue();
    return a == 10 || a == 127 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254);
}

// This machine's address on the local network: a home network's 192.168 first, then 10, then
// 172.16-31 (which virtual adapters like to take).
juce::String localAddress()
{
    juce::String best;
    int bestRank = 99;

    for (const auto& ip : juce::IPAddress::getAllAddresses (false))
    {
        const auto text = ip.toString();
        const auto parts = juce::StringArray::fromTokens (text, ".", "");

        if (parts.size() != 4 || ! isLocalAddress (text))
            continue;

        const int a = parts[0].getIntValue();
        const int rank = a == 192 ? 0 : a == 10 ? 1 : a == 172 ? 2 : 9; // 127 and 169.254: no use to a phone

        if (rank < bestRank)
        {
            best = text;
            bestRank = rank;
        }
    }

    return bestRank < 9 ? best : juce::String();
}

juce::String makeToken()
{
    // Without the look-alikes (0 o, 1 l): it may be typed.
    const char* const letters = "abcdefghijkmnpqrstuvwxyz23456789";
    juce::Random random;
    random.setSeedRandomly();
    juce::String token;

    for (int i = 0; i < 6; ++i)
        token += letters[random.nextInt (32)];

    return token;
}
} // namespace

struct PhoneLink::Server final : private juce::Thread
{
    Server() : juce::Thread ("taw phone link") {}

    ~Server() override
    {
        stopping = true;
        signalThreadShouldExit();
        listener.close();
        stopThread (3000);

        for (auto& w : workers)
            if (w.thread.joinable())
                w.thread.join();

        folder.deleteRecursively();
    }

    bool start (juce::String& error)
    {
        address = localAddress();

        if (address.isEmpty())
        {
            error = "no local network to be reached on";
            return false;
        }

        for (port = firstPort; port < firstPort + portsToTry; ++port)
            if (listener.createListener (port))
                break;

        if (port == firstPort + portsToTry)
        {
            error = "couldn't open a port (" + juce::String (firstPort) + " to " + juce::String (firstPort + portsToTry - 1) + " are taken)";
            return false;
        }

        token = makeToken();
        folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("taw-phone-" + token);
        folder.createDirectory();

        // HTTPS if it can be had (without it the page can send files, not record).
        juce::String why;
        secure = tls.prepare (configFolder().getChildFile ("phone"), address, why);
        startThread();
        return true;
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            // Finished connections out of the list.
            for (auto w = workers.begin(); w != workers.end();)
            {
                if (w->done)
                {
                    w->thread.join();
                    w = workers.erase (w);
                }
                else
                    ++w;
            }

            if (listener.waitUntilReady (true, 200) != 1)
                continue;

            if (auto* connection = listener.waitForNextConnection())
            {
                auto& w = workers.emplace_back();
                w.thread = std::thread ([this, connection, &w]
                {
                    std::unique_ptr<juce::StreamingSocket> socket (connection);
                    serve (*socket);
                    w.done = true;
                });
            }
        }
    }

    // A connection, plain or under TLS.
    struct Wire
    {
        juce::StreamingSocket& socket;
        std::unique_ptr<PhoneTls::Line> tls;

        int read (void* to, int most, int timeoutMs) // bytes; 0: none in that time; under 0: closed
        {
            if (tls != nullptr)
                return tls->read (to, most, timeoutMs);

            if (socket.waitUntilReady (true, timeoutMs) != 1)
                return 0;

            const int got = socket.read (to, most, false);
            return got > 0 ? got : -1;
        }

        void write (const void* from, int size)
        {
            if (tls != nullptr)
                tls->write (from, size);
            else
                socket.write (from, size);
        }
    };

    static void reply (Wire& socket, const char* status, const char* type, const juce::String& body)
    {
        const auto utf8 = body.toRawUTF8();
        const auto length = (int) body.getNumBytesAsUTF8();
        const auto head = juce::String ("HTTP/1.1 ") + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " + juce::String (length)
                          + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        socket.write (head.toRawUTF8(), (int) head.getNumBytesAsUTF8());
        socket.write (utf8, length);
    }

    void say (const juce::String& text)
    {
        const juce::ScopedLock sl (lock);
        statusText = text;
    }

    void serve (juce::StreamingSocket& connection)
    {
        // Nothing from outside the local network gets a word back.
        if (! isLocalAddress (connection.getHostName()))
            return;

        Wire socket { connection, nullptr };
        bool sniffed = false;

        // The request's head: up to the blank line.
        juce::MemoryBlock data;
        int headEnd = -1;

        while (headEnd < 0 && data.getSize() < 16384 && ! stopping)
        {
            char buffer[2048];
            const int got = socket.read (buffer, sizeof (buffer), 3000);

            if (got <= 0)
                return;

            // HTTPS starts with a TLS handshake (its first byte says so); plain HTTP with a letter.
            if (! std::exchange (sniffed, true) && buffer[0] == 0x16)
            {
                socket.tls = secure ? tls.accept (connection, buffer, got, stopping) : nullptr;

                if (socket.tls == nullptr)
                    return;

                continue;
            }

            data.append (buffer, (size_t) got);
            const auto* bytes = static_cast<const char*> (data.getData());

            for (int i = 0; i + 3 < (int) data.getSize(); ++i)
                if (bytes[i] == '\r' && bytes[i + 1] == '\n' && bytes[i + 2] == '\r' && bytes[i + 3] == '\n')
                {
                    headEnd = i + 4;
                    break;
                }
        }

        if (headEnd < 0)
            return;

        const auto lines = juce::StringArray::fromLines (juce::String::fromUTF8 (static_cast<const char*> (data.getData()), headEnd));
        const auto request = juce::StringArray::fromTokens (lines[0], " ", "");
        const auto method = request[0];
        const auto path = request[1].upToFirstOccurrenceOf ("?", false, false);
        const auto query = request[1].fromFirstOccurrenceOf ("?", false, false);
        juce::int64 length = -1;

        for (int i = 1; i < lines.size(); ++i)
            if (lines[i].upToFirstOccurrenceOf (":", false, false).trim().equalsIgnoreCase ("content-length"))
                length = lines[i].fromFirstOccurrenceOf (":", false, false).trim().getLargeIntValue();

        const auto home = "/" + token;

        if (method == "GET" && (path == home || path == home + "/"))
            return reply (socket, "200 OK", "text/html; charset=utf-8", page);

        if (method == "GET" && path == home + "/session")
            return reply (socket, "200 OK", "text/plain; charset=utf-8", sessionName());

        if (method == "GET" && path == home + "/tempo")
        {
            const juce::ScopedLock sl (lock);
            return reply (socket, "200 OK", "text/plain; charset=utf-8", tempo);
        }

        if (method == "GET" && path == home + "/instruments")
        {
            const juce::ScopedLock sl (lock);
            return reply (socket, "200 OK", "text/plain; charset=utf-8", instruments.joinIntoString ("\n"));
        }

        // A refusal is read by the phone only if what it was sending is taken off the line first
        // (closing on unread data resets the connection, and the answer goes with it). Up to 32 MB
        // of it: more isn't worth the wait.
        auto refuse = [&] (const char* status, const char* why)
        {
            auto left = std::min<juce::int64> (length, 32LL * 1024 * 1024) - ((juce::int64) data.getSize() - headEnd);
            std::vector<char> sink (64 * 1024);

            while (method == "POST" && left > 0 && ! stopping)
            {
                const int got = socket.read (sink.data(), (int) std::min<juce::int64> ((juce::int64) sink.size(), left), 3000);

                if (got <= 0)
                    break;

                left -= got;
            }

            reply (socket, status, "text/plain; charset=utf-8", why);
        };

        if (method != "POST" || path != home + "/up")
            return refuse ("404 Not Found", "not here");

        // An upload: the file's name and what to make of it in the query, the file itself as the body.
        juce::String name, as ("audio"), instrument;
        double downbeat = -1.0, bpm = 0.0;

        for (const auto& pair : juce::StringArray::fromTokens (query, "&", ""))
        {
            const auto value = juce::URL::removeEscapeChars (pair.fromFirstOccurrenceOf ("=", false, false));

            if (pair.startsWith ("name="))
                name = value;
            else if (pair.startsWith ("as=") && (value == "drums" || value == "notes"))
                as = value;
            else if (pair.startsWith ("inst="))
                instrument = value.trim().substring (0, 80);
            else if (pair.startsWith ("at="))
                downbeat = juce::jlimit (0.0, 60.0, value.getDoubleValue());
            else if (pair.startsWith ("bpm="))
                bpm = juce::jlimit (0.0, 400.0, value.getDoubleValue());
        }

        name = juce::File::createLegalFileName (name.trim()).substring (0, 120);

        // What a phone's recorder makes, and what the importer can read.
        static const juce::StringArray audio { ".wav", ".aif", ".aiff", ".flac", ".ogg", ".mp3", ".m4a", ".aac", ".wma", ".mp4", ".3gp" };

        if (name.isEmpty() || ! audio.contains (juce::File::createLegalFileName (name).fromLastOccurrenceOf (".", true, false), true))
            return refuse ("415 Unsupported Media Type", "taw takes audio files only");

        if (length <= 0 || length > maxUpload)
            return refuse ("413 Payload Too Large", "too big for taw (500 MB at most)");

        const auto file = folder.getChildFile (name).getNonexistentSibling();
        const auto partial = file.getSiblingFile (file.getFileName() + ".part");
        juce::int64 written = 0;
        bool failed = false;

        {
            juce::FileOutputStream out (partial);

            if (out.failedToOpen())
                return reply (socket, "500 Internal Server Error", "text/plain; charset=utf-8", "taw couldn't keep the file");

            // What came with the head, then the rest.
            const auto already = std::min<juce::int64> (length, (juce::int64) data.getSize() - headEnd);
            failed = ! out.write (static_cast<const char*> (data.getData()) + headEnd, (size_t) already);
            written = already;
            std::vector<char> buffer (64 * 1024);

            while (written < length && ! failed && ! stopping)
            {
                const int got = socket.read (buffer.data(), (int) std::min<juce::int64> ((juce::int64) buffer.size(), length - written), 20000);

                if (got <= 0 || ! out.write (buffer.data(), (size_t) got))
                {
                    failed = true;
                    break;
                }

                written += got;
                say ("receiving " + name + " " + juce::String (juce::roundToInt (100.0 * (double) written / (double) length)) + "%");
            }
        }

        if (failed || stopping || written < length || ! partial.moveFileTo (file))
        {
            partial.deleteFile();
            say ("lost " + name + " part way");
            return reply (socket, "400 Bad Request", "text/plain; charset=utf-8", "it didn't all arrive");
        }

        {
            const juce::ScopedLock sl (lock);
            arrived.push_back ({ file, as, instrument, bpm >= 20.0 ? downbeat : -1.0, bpm >= 20.0 ? bpm : 0.0 });
            ++count;
            statusText = "got " + name;
        }

        reply (socket, "200 OK", "text/plain; charset=utf-8", sessionName()); // where it went
    }

    juce::String sessionName()
    {
        const juce::ScopedLock sl (lock);
        return session;
    }

    struct Worker
    {
        std::thread thread;
        std::atomic<bool> done { false };
    };

    juce::StreamingSocket listener;
    std::list<Worker> workers; // the accept thread's own
    std::atomic<bool> stopping { false };

    juce::String address, token;
    int port = 0;
    juce::File folder;

    juce::CriticalSection lock;
    std::vector<Take> arrived;
    juce::String statusText, session, tempo;
    juce::StringArray instruments;
    PhoneTls tls;
    bool secure = false; // the key and certificate are there: HTTPS is answered
    int count = 0;
};

void PhoneLink::setInstruments (const juce::StringArray& names)
{
    if (server == nullptr)
        return;

    const juce::ScopedLock sl (server->lock);
    server->instruments = names;
}

void PhoneLink::setSession (const juce::String& name, double bpm, int beatsPerBar, bool empty)
{
    if (server == nullptr)
        return;

    const juce::ScopedLock sl (server->lock);
    server->session = name;
    server->tempo = juce::String (bpm, 2) + " " + juce::String (beatsPerBar) + " " + (empty ? "1" : "0");
}

PhoneLink::PhoneLink() = default;
PhoneLink::~PhoneLink() = default;

bool PhoneLink::start (juce::String& error)
{
    if (server != nullptr)
        return true;

    auto starting = std::make_unique<Server>();

    if (! starting->start (error))
        return false;

    server = std::move (starting);
    return true;
}

void PhoneLink::stop() { server.reset(); }
bool PhoneLink::running() const { return server != nullptr; }

juce::String PhoneLink::url() const
{
    return server != nullptr ? juce::String (server->secure ? "https://" : "http://") + server->address + ":" + juce::String (server->port) + "/" + server->token : juce::String();
}

std::vector<PhoneLink::Take> PhoneLink::takeReceived()
{
    if (server == nullptr)
        return {};

    const juce::ScopedLock sl (server->lock);
    return std::exchange (server->arrived, {});
}

int PhoneLink::received() const
{
    if (server == nullptr)
        return 0;

    const juce::ScopedLock sl (server->lock);
    return server->count;
}

juce::String PhoneLink::status() const
{
    if (server == nullptr)
        return {};

    const juce::ScopedLock sl (server->lock);
    return server->statusText;
}

std::vector<std::vector<bool>> PhoneLink::qr (const juce::String& text)
{
    try
    {
        const auto code = qrcodegen::QrCode::encodeText (text.toRawUTF8(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int size = code.getSize();
        std::vector<std::vector<bool>> rows ((size_t) size, std::vector<bool> ((size_t) size));

        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                rows[(size_t) y][(size_t) x] = code.getModule (x, y);

        return rows;
    }
    catch (const std::exception&)
    {
        return {};
    }
}
} // namespace taw
