#include "App.h"
#include "AlsImport.h"
#include "Bounce.h"
#include "Hum.h"
#include "Import.h"
#include "Library.h"
#include "Plugins.h"
#include "Safety.h"
#include "Ui.h"
#include "Video.h"

#include <juce_events/juce_events.h>
#include <future>
#include <numeric>

#if TAW_HUB
 #include "Hub.h"
 #include "HubDevice.h"
 #include "HubServer.h"
#endif

#if JUCE_MAC
 #include "MacFocus.h"
 #include "TerminalSetup.h"
#endif

#if JUCE_WINDOWS
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
 #include <shellapi.h>
 #pragma comment(lib, "shell32.lib")
#endif

// taw                 open ./session.json, or start a new session here
// taw <dir>           open <dir>, or create a session called <dir>
// taw <dir> -t 3mic   create <dir> from the 3mic template without asking
#if JUCE_WINDOWS
int main()
#else
int main (int argc, char** argv)
#endif
{
    juce::ScopedJuceInitialiser_GUI juce;
    juce::StringArray args;

#if JUCE_WINDOWS
    // argv is in the ANSI code page, which mangles anything outside it (accents, Mac-mapped
    // characters); take the UTF-16 command line instead.
    int argc = 0;

    if (auto** argv = CommandLineToArgvW (GetCommandLineW(), &argc))
    {
        for (int i = 1; i < argc; ++i)
            args.add (juce::String (argv[i]));

        LocalFree (argv);
    }
#else
    for (int i = 1; i < argc; ++i)
        args.add (juce::String::fromUTF8 (argv[i]));
#endif

    // taw --version   which build this is (also in the title bar).
    if (args[0] == "--version")
    {
        std::printf ("taw %s %s built %s\n", TAW_VERSION, taw::buildCommit(), taw::buildTime());
        return 0;
    }

    // taw --scan-plugin <file> <out.xml>   (taw runs this itself) describe one plugin file in a
    // child process, so a plugin that crashes or hangs while loading can't take taw with it.
    // No crash handler here, and no Windows crash dialog: a crash is just an exit code.
    if (args[0] == "--scan-plugin")
    {
#if JUCE_WINDOWS
        SetErrorMode (SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
        return taw::PluginCatalog::scanInThisProcess (args[1], juce::File (args[2]));
    }

    // taw --mcp [--session <folder or name>] [--name <agent>]   the MCP bridge for agents
    // (docs/plan-taw-mcp.md): MCP on stdin and stdout, relayed to the session open in a running
    // taw. Like a plugin scan, it's on its own: no crash handler, no crash dialog, and if it
    // dies taw only loses a connection.
    if (args[0] == "--mcp")
    {
#if JUCE_WINDOWS
        SetErrorMode (SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
        return taw::agents::runBridge (args);
    }

    taw::safety::installCrashHandler();

#if TAW_HUB
    // taw --hub   the audio hub (docs/plan-audio-hub.md), headless; launchd runs it from
    // "taw hub.app" on macOS. taw --install-hub / --uninstall-hub: start it at login, or not.
    if (args[0] == "--hub")
        return taw::hub::run (args);

    if (args[0] == "--install-hub" || args[0] == "--uninstall-hub")
    {
        std::printf ("%s\n", (args[0] == "--install-hub" ? taw::hub::install() : taw::hub::uninstall()).toRawUTF8());
        return 0;
    }

    // taw --install-driver / --uninstall-driver   the apps' devices, taw 1 to taw 4 (admin password).
    if (args[0] == "--install-driver" || args[0] == "--uninstall-driver")
    {
        std::printf ("%s\n", (args[0] == "--install-driver" ? taw::hub::installDriver() : taw::hub::uninstallDriver()).toRawUTF8());
        return 0;
    }

    // taw --apptest   the app devices end to end, as apps would use them: clicks played into
    // taw 1, recorded from taw 2's mic, with a route taw 1 > taw 2 through the hub for the
    // while. Says whether they came through, how loud, and how late.
    if (args[0] == "--apptest")
    {
        auto ask = [] (const juce::String& type, int channel)
        {
            auto* m = new juce::DynamicObject();
            m->setProperty ("type", type);
            m->setProperty ("from", "taw 1");
            m->setProperty ("fromChannel", channel);
            m->setProperty ("to", "taw 2");
            m->setProperty ("toChannel", channel);
            juce::String error;
            taw::hub::request (m, error);
            return error;
        };

        {
            auto* m = new juce::DynamicObject();
            m->setProperty ("type", "status");
            juce::String error;
            const auto status = taw::hub::request (m, error);

            if (error.isEmpty() && (int) status["apps"] < 2)
                error = "the hub has no app devices (taw --install-driver)";

            if (error.isNotEmpty())
            {
                std::printf ("%s\n", error.toRawUTF8());
                return 1;
            }
        }

        ask ("route", 1);
        ask ("route", 2);

        // A 20 ms beep every half second out (not a single-sample click: the aggregate's drift
        // correction resamples, which smears one); where each comes back in.
        struct Clicks final : juce::AudioIODeviceCallback
        {
            void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int n,
                                                   const juce::AudioIODeviceCallbackContext&) override
            {
                for (int i = 0; i < n; ++i, ++now)
                {
                    const auto phase = now % period;
                    const float click = phase < period / 25 ? 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 500.0 * (double) phase / (2.0 * (double) period))
                                                            : 0.0f;

                    for (int c = 0; c < numOut; ++c)
                        out[c][i] = click;

                    const float heard = numIn > 0 ? std::abs (in[0][i]) : 0.0f;
                    peak = std::max (peak.load(), heard);

                    if (heard > 0.25f && now - lastHeard > period / 2)
                    {
                        lastHeard = now;
                        lateBy = (int) (now % period);
                        heardClicks++;
                    }
                }
            }

            void audioDeviceAboutToStart (juce::AudioIODevice* d) override { period = (juce::int64) d->getCurrentSampleRate() / 2; }
            void audioDeviceStopped() override {}

            juce::int64 now = 0, period = 24000, lastHeard = -1000000;
            std::atomic<float> peak { 0.0f };
            std::atomic<int> lateBy { -1 }, heardClicks { 0 };
        } clicks;

        juce::AudioDeviceManager dm;
        dm.initialise (2, 2, nullptr, false);
        dm.setCurrentAudioDeviceType ("CoreAudio", true);
        auto setup = dm.getAudioDeviceSetup();
        setup.outputDeviceName = "taw 1";
        setup.inputDeviceName = "taw 2";
        setup.useDefaultInputChannels = setup.useDefaultOutputChannels = true;
        auto error = dm.setAudioDeviceSetup (setup, true);

        if (error.isEmpty())
        {
            dm.addAudioCallback (&clicks);
            juce::Thread::sleep (3000);
            dm.removeAudioCallback (&clicks);
            const auto rate = dm.getCurrentAudioDevice()->getCurrentSampleRate();
            std::printf ("%d clicks came back from taw 2, peak %.1f dB, %d samples (%.1f ms) late\n", clicks.heardClicks.load(),
                         juce::Decibels::gainToDecibels (clicks.peak.load()), clicks.lateBy.load(), 1000.0 * clicks.lateBy.load() / rate);
        }
        else
        {
            std::printf ("couldn't open taw 1 and taw 2: %s\n", error.toRawUTF8());
        }

        dm.closeAudioDevice();
        ask ("unroute", 1);
        ask ("unroute", 2);
        const bool ok = error.isEmpty() && clicks.heardClicks >= 4 && clicks.peak > 0.4f;
        std::puts (ok ? "ok" : "FAILED");
        return ok ? 0 : 1;
    }

    // taw --hubping [seconds]   connect to the running hub as a session does, listen (silent
    // out), and say what came in, the latency it reports, and the blocks missed.
    if (args[0] == "--hubping")
    {
        struct Listen final : juce::AudioIODeviceCallback
        {
            void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int n,
                                                   const juce::AudioIODeviceCallbackContext&) override
            {
                for (int c = 0; c < numIn; ++c)
                    peak = std::max (peak.load(), juce::FloatVectorOperations::findMaximum (in[c], n));

                for (int c = 0; c < numOut; ++c)
                    juce::FloatVectorOperations::clear (out[c], n);

                blocks++;
            }

            void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
            void audioDeviceStopped() override {}
            std::atomic<float> peak { 0.0f };
            std::atomic<int> blocks { 0 };
        } listen;

        taw::hub::setClientName ("hubping", "hubping");
        auto type = taw::hub::makeDeviceType();
        std::unique_ptr<juce::AudioIODevice> d (type->createDevice ({}, {}));
        juce::BigInteger all;
        all.setRange (0, 64, true);

        if (const auto error = d->open (all, all, 0.0, 0); error.isNotEmpty())
        {
            std::printf ("%s\n", error.toRawUTF8());
            return 1;
        }

        std::printf ("connected: %.0f Hz, %d samples, %d in, %d out, latency in %d / out %d samples\n", d->getCurrentSampleRate(),
                     d->getCurrentBufferSizeSamples(), d->getActiveInputChannels().countNumberOfSetBits(),
                     d->getActiveOutputChannels().countNumberOfSetBits(), d->getInputLatencyInSamples(), d->getOutputLatencyInSamples());
        d->start (&listen);

        for (int s = 0; s < (args[1].isNotEmpty() ? args[1].getIntValue() : 3); ++s)
        {
            juce::Thread::sleep (1000);
            const auto peak = listen.peak.exchange (0.0f);
            std::printf ("%d blocks, heard %s, %d missed\n", listen.blocks.load(),
                         peak > 0.0f ? (juce::String (juce::Decibels::gainToDecibels (peak), 1) + " dB").toRawUTF8() : "silence", d->getXRunCount());
        }

        d->close();
        return 0;
    }
#endif

    if (args.contains ("-h") || args.contains ("--help"))
    {
        std::puts ("usage: taw [session-dir] [-t template]\n\n"
                   "  taw                 open the session in this folder, or start a new one\n"
                   "  taw my-song         open my-song, or create it\n"
                   "  taw my-song -t 3mic create my-song from the 3mic template\n"
                   "  taw --mcp           an MCP server for agents, editing the session open in taw\n"
                   "                      (claude mcp add taw -- <path to taw> --mcp)\n\n"
#if JUCE_WINDOWS
                   "templates and device settings live in %APPDATA%\\taw");
#else
                   "templates and device settings live in ~/Library/Application Support/taw");
#endif
        return 0;
    }

    // taw --parse "2i2" "1-3r" ...   show how command lines parse (assumes 16 tracks)
    if (args[0] == "--parse")
    {
        for (int i = 1; i < args.size(); ++i)
            std::printf ("%-16s %s\n", args[i].toRawUTF8(), taw::explainCommand (args[i], 16).toRawUTF8());

        return 0;
    }

    // taw --plugintest <file.vst3> [rate]   load one plugin and play it the way the engine does: a
    // chord from the timeline, a stop, a key from the controller, a disarm. Checks its state
    // round-trips. At 48 kHz unless a rate is given. Leaves the plugin list alone.
    if (args[0] == "--plugintest")
    {
        juce::AudioPluginFormatManager formats;
        juce::addDefaultFormatsToManager (formats);
        juce::OwnedArray<juce::PluginDescription> found;

        for (auto* format : formats.getFormats())
            format->findAllTypesForFile (found, args[1]);

        if (found.isEmpty())
        {
            std::printf ("no plugin in %s\n", args[1].toRawUTF8());
            return 1;
        }

        const auto desc = *found[0];
        const double rate = args[2].getDoubleValue() > 0.0 ? args[2].getDoubleValue() : 48000.0;
        constexpr int block = 512;
        juce::String error;
        auto instance = formats.createPluginInstance (desc, rate, block, error);

        if (instance == nullptr)
        {
            std::printf ("couldn't load %s: %s\n", desc.name.toRawUTF8(), error.toRawUTF8());
            return 1;
        }

        std::printf ("%s by %s \xc2\xb7 %s \xc2\xb7 %d in, %d out\n", desc.name.toRawUTF8(), desc.manufacturerName.toRawUTF8(),
                     desc.isInstrument ? "instrument" : "effect", instance->getTotalNumInputChannels(), instance->getTotalNumOutputChannels());

        auto slot = std::make_shared<taw::PluginSlot> (std::move (instance), desc);
        slot->prepare (rate, block);

        taw::Sequence seq;
        for (int pitch : { 60, 64, 67 })
            seq.notes.push_back ({ 0, (juce::int64) rate, 0, (juce::uint8) pitch, 100 }); // a second of C major

        taw::HostTransport t;
        t.sampleRate = rate;
        juce::AudioBuffer<float> out (2, block);

        // Renders seconds of audio from pos (rolling or not) and returns the peak.
        auto run = [&] (double seconds, bool rolling, const taw::MidiMsg* live, int numLive)
        {
            float peak = 0.0f;
            t.rolling = rolling;

            for (int done = 0; done < (int) (seconds * rate); done += block)
            {
                out.clear();
                slot->render (seq, t, block, live, numLive, out.getWritePointer (0), out.getWritePointer (1));
                live = live != nullptr ? live + numLive : nullptr, numLive = 0; // only the first block gets the keys
                peak = std::max ({ peak, out.getMagnitude (0, 0, block), out.getMagnitude (1, 0, block) });

                if (rolling)
                    t.pos += block;
            }

            return juce::Decibels::gainToDecibels (peak);
        };

        const auto chord = run (1.0, true, nullptr, 0);
        const auto tail = (run (1.5, true, nullptr, 0), run (0.5, true, nullptr, 0)); // after the release
        const taw::MidiMsg key[] { taw::MidiMsg::noteOn (0, 72, 100) };
        const auto pressed = run (0.5, false, key, 1);          // stopped, armed: a key from the controller
        const auto held = run (0.5, false, key + 1, 0);         // still armed, still held
        const auto released = (run (2.0, false, nullptr, 0), run (0.5, false, nullptr, 0)); // disarmed: let go

        const auto state = slot->getState();
        slot->setState (state);

        std::printf ("chord %.1f dB \xc2\xb7 2 s later %.1f dB \xc2\xb7 key %.1f dB \xc2\xb7 held %.1f dB \xc2\xb7 disarmed %.1f dB \xc2\xb7 state %d bytes\n",
                     chord, tail, pressed, held, released, (int) state.getSize());

        const bool ok = chord > -60.0f && tail < chord - 20.0f && pressed > -60.0f && released < pressed - 20.0f;
        std::puts (ok ? "ok" : "FAILED (expected sound for the chord and the key, and quiet after each ends)");
        return ok ? 0 : 1;
    }

    // taw --windowtest <plugin file> <out.png>   open a plugin's window as taw does and save a
    // picture of it, keyboard with taw and then held (the title bar; a plugin's own drawing may
    // not show).
    if (args[0] == "--windowtest")
    {
        juce::AudioPluginFormatManager formats;
        juce::addDefaultFormatsToManager (formats);
        juce::OwnedArray<juce::PluginDescription> found;

        for (auto* format : formats.getFormats())
            format->findAllTypesForFile (found, args[1]);

        juce::String error;
        auto instance = found.isEmpty() ? nullptr : formats.createPluginInstance (*found[0], 48000.0, 512, error);

        if (instance == nullptr)
        {
            std::printf ("couldn't load %s %s\n", args[1].toRawUTF8(), error.toRawUTF8());
            return 1;
        }

        auto slot = std::make_shared<taw::PluginSlot> (std::move (instance), *found[0]);
        slot->prepare (48000.0, 512);
        slot->showEditor ("3 kik003" + juce::String (L" \x00b7 A \x00b7 ") + found[0]->name + juce::String (L" \x00b7 taw"));
        juce::MessageManager::getInstance()->runDispatchLoopUntil (1500);

        const auto out = juce::File::getCurrentWorkingDirectory().getChildFile (args[2]);
        auto save = [&] (const juce::File& file)
        {
            const auto image = slot->editorSnapshot();
            juce::PNGImageFormat png;
            file.deleteFile();
            juce::FileOutputStream stream (file);
            return image.isValid() && stream.openedOk() && png.writeImageToStream (image, stream);
        };

        bool ok = save (out);
        slot->markKeyboard (true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
        ok = save (out.getSiblingFile (out.getFileNameWithoutExtension() + "-held.png")) && ok;
        slot->closeEditor();
        std::puts (ok ? "ok" : "FAILED");
        return ok ? 0 : 1;
    }

#if TAW_VIDEO
    // taw --videotest <video> [out.png]   the picture: frames land exactly (each one's number from
    // its timestamp, and its pixels against the ffmpeg command line's), how fast playing and
    // seeking are, and the window (follows a clock, never takes the keyboard). Saves what it
    // showed as out.png.
    if (args[0] == "--videotest" && args.size() >= 2)
    {
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (args[1].unquoted());
        taw::VideoDecoder dec;
        juce::String error;

        if (! dec.open (file, error))
        {
            std::printf ("%s\n", error.toRawUTF8());
            return 1;
        }

        const auto& info = dec.info();
        std::printf ("%s  %dx%d  %.3f fps  %.2f s\n", info.codec.toRawUTF8(), info.width, info.height, info.fps, info.duration);
        bool ok = true;
        auto now = [] { return juce::Time::getMillisecondCounterHiRes(); };

        // Playing: every frame in turn, asked for mid-frame and right at its start.
        const int playFrames = juce::jmin (300, (int) (info.duration * info.fps) - 1);
        auto t0 = now();
        juce::Image img;

        for (int i = 0; i < playFrames; ++i)
        {
            const auto got = dec.seek ((i + 0.5) / info.fps);
            const auto atStart = dec.seek (i / info.fps);

            if (got != i || atStart != i)
            {
                std::printf ("FAILED: frame %d came back as %lld (at its start: %lld)\n", i, (long long) got, (long long) atStart);
                ok = false;
                break;
            }

            dec.render (img, 1280, 720);
        }

        const auto playMs = (now() - t0) / juce::jmax (1, playFrames);
        std::printf ("playing: %.2f ms a frame (decode + scale to 1280x720), %d seeks\n", playMs, dec.seeks);

        // Jumping about: forward, back, far, near.
        juce::Random rng (7);
        double worst = 0.0, total = 0.0;
        const int jumps = 40;

        for (int j = 0; j < jumps; ++j)
        {
            const auto frame = (juce::int64) rng.nextInt ((int) (info.duration * info.fps) - 2);
            const auto start = now();
            const auto got = dec.seek (((double) frame + 0.25) / info.fps);
            const auto ms = now() - start;
            worst = juce::jmax (worst, ms);
            total += ms;

            if (got != frame)
            {
                std::printf ("FAILED: jump to frame %lld came back as %lld\n", (long long) frame, (long long) got);
                ok = false;
            }
        }

        std::printf ("jumping: %.1f ms on average, %.1f ms at worst\n", total / jumps, worst);

        if (dec.seek (-0.01) != -1 || dec.seek (info.duration + 1.0) != -1)
        {
            std::puts ("FAILED: before the start or past the end isn't black");
            ok = false;
        }

        // Pixels: each frame is nearer the ffmpeg command line's same frame than its neighbours.
        const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("ffmpeg.exe");
        const auto cli = exe.existsAsFile() ? exe.getFullPathName() : juce::String ("ffmpeg");
        const int w = 320, h = 180;
        auto reference = [&] (juce::int64 frame, juce::MemoryBlock& out)
        {
            const auto raw = juce::File::createTempFile (".raw");
            juce::ChildProcess p;
            // By number (select's n counts frames from the start): no question which frame a time means.
            juce::StringArray cmd { cli, "-v", "error", "-y", "-i", file.getFullPathName(), "-vf",
                                    "select=eq(n\\," + juce::String (frame) + "),scale=320:180:flags=bilinear",
                                    "-frames:v", "1", "-pix_fmt", "bgra", "-f", "rawvideo", raw.getFullPathName() };
            const bool ran = p.start (cmd) && p.waitForProcessToFinish (30000) && raw.loadFileAsData (out);
            raw.deleteFile();
            return ran && out.getSize() == (size_t) (w * h * 4);
        };
        auto difference = [&] (const juce::Image& a, const juce::MemoryBlock& b)
        {
            const juce::Image::BitmapData px (a, juce::Image::BitmapData::readOnly);
            const auto* ref = static_cast<const juce::uint8*> (b.getData());
            double sum = 0.0;

            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w * 4; ++x)
                    if (x % 4 != 3)
                        sum += std::abs ((int) px.getLinePointer (y)[x] - (int) ref[(y * w * 4) + x]);

            return sum / (w * h * 3);
        };

        for (const auto at : { 0.26, 0.52, 0.78 })
        {
            const auto frame = (juce::int64) (info.duration * at * info.fps);
            juce::MemoryBlock before, same, after;

            if (! reference (frame - 1, before) || ! reference (frame, same) || ! reference (frame + 1, after))
            {
                std::puts ("no ffmpeg command line to compare pixels with (skipped)");
                break;
            }

            dec.seek (((double) frame + 0.5) / info.fps);
            juce::Image mine;
            dec.render (mine, w, h);
            const auto dSame = difference (mine, same), dBefore = difference (mine, before), dAfter = difference (mine, after);
            const bool exact = dSame < dBefore && dSame < dAfter && dSame < 6.0;
            std::printf ("frame %lld: off by %.2f from ffmpeg's (its neighbours: %.2f, %.2f) %s\n", (long long) frame, dSame, dBefore, dAfter,
                         exact ? "ok" : juce::exactlyEqual (dBefore, dSame) || juce::exactlyEqual (dAfter, dSame) ? "FAILED (or the picture doesn't move there)" : "FAILED");
            ok = ok && exact;
        }

        // The window, following a clock: no keyboard taken, the right frame on it.
        std::atomic<double> clock { -1.0 };
        taw::VideoPlayer player ([&clock] { return clock.load(); });

        if (! player.open (file, error))
        {
            std::printf ("FAILED: %s\n", error.toRawUTF8());
            return 1;
        }

        // What has the keyboard: the window in front (Windows), the app in front (macOS).
#if JUCE_WINDOWS
        auto keyboardOwner = [] { return (juce::pointer_sized_int) GetForegroundWindow(); };
#else
        auto keyboardOwner = [] { return (juce::pointer_sized_int) taw::macFocus::frontmostApp(); };
#endif
        const auto front = keyboardOwner();
        player.setVisible (true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);

        if (player.shownFrame() != -1)
        {
            std::puts ("FAILED: the window isn't black before the video starts");
            ok = false;
        }

        clock = 10.5 / info.fps;
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
        const auto shown = player.shownFrame();
        std::printf ("window: frame %lld for frame 10 %s\n", (long long) shown, shown == 10 ? "ok" : "FAILED");
        ok = ok && shown == 10;

        // Playing in real time for a second: frames shown in order, none late.
        const auto playStart = now();
        int changes = 0;
        juce::int64 last = shown;

        while (now() - playStart < 1000.0)
        {
            clock = 10.5 / info.fps + (now() - playStart) / 1000.0;
            juce::MessageManager::getInstance()->runDispatchLoopUntil (5);

            if (const auto f = player.shownFrame(); f != last)
            {
                changes += f == last + 1 ? 1 : 0;
                last = f;
            }
        }

        std::printf ("played: %d of %d frames in a second, one after another\n", changes, juce::roundToInt (info.fps));

        if (keyboardOwner() != front)
        {
            std::puts ("FAILED: showing the window took the keyboard");
            ok = false;
        }

        if (args.size() >= 3)
        {
            const auto png = juce::File::getCurrentWorkingDirectory().getChildFile (args[2]);
            png.deleteFile();
            juce::FileOutputStream out (png);
            juce::PNGImageFormat format;
            const auto pic = player.snapshot();
            std::printf ("saved %s %s\n", png.getFullPathName().toRawUTF8(), out.openedOk() && pic.isValid() && format.writeImageToStream (pic, out) ? "" : "(FAILED)");
        }

        player.setVisible (false);
        std::puts (ok ? "ok" : "FAILED");
        return ok ? 0 : 1;
    }
#endif

    // taw --miditest [output]   open every MIDI input the way taw does, send a note out of a
    // loopback port (LoopBe by default), and check it comes back in. Then lists what each sent.
    if (args[0] == "--miditest")
    {
        const auto outName = args[1].isNotEmpty() ? args[1] : juce::String ("LoopBe");
        juce::StringArray others; // "--miditest LoopBe only": just the loopback input open

        if (args[2] == "only")
            for (const auto& info : juce::MidiInput::getAvailableDevices())
                if (! info.name.containsIgnoreCase (outName))
                    others.add (info.name);

        auto queuesOwned = std::make_unique<std::array<taw::MidiQueue, taw::MidiInputs::maxPorts>>(); // big: off main's stack
        auto& queues = *queuesOwned;
        taw::MidiInputs inputs (queues, others);
        std::unique_ptr<juce::MidiOutput> out;

        for (const auto& info : juce::MidiOutput::getAvailableDevices())
            if (out == nullptr && info.name.containsIgnoreCase (outName))
                out = juce::MidiOutput::openDevice (info.identifier);

        std::printf ("%d inputs open\n", inputs.names().size());

        if (out == nullptr)
        {
            std::printf ("no MIDI output matching %s to loop back through\n", outName.toRawUTF8());
            return 1;
        }

        out->sendMessageNow (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 99));
        out->sendMessageNow (juce::MidiMessage::noteOff (1, 60));
        juce::MessageManager::getInstance()->runDispatchLoopUntil (500); // some MIDI backends deliver through the message loop

        int got = 0;

        for (auto& q : queues)
            for (taw::MidiMsg m; q.pop (m);)
            {
                std::printf ("  got %s\n", taw::describe (m).toRawUTF8());
                ++got;
            }

        for (const auto& d : inputs.devices())
            if (d.events > 0 || ! d.open)
                std::printf ("  %-28s %s, %d messages\n", d.name.toRawUTF8(), d.open ? "open" : "NOT OPEN", d.events);

        std::puts (got >= 2 ? "ok" : "FAILED (sent a note, nothing came back)");
        return got >= 2 ? 0 : 1;
    }

    // taw --alsimport <set.als> <new session folder>   an Ableton Live set into a taw session (no
    // device; plugins aren't loaded). Prints the report.
    if (args[0] == "--alsimport" && args.size() >= 3)
    {
        const auto cwd = juce::File::getCurrentWorkingDirectory();
        const auto set = taw::readAbletonSet (cwd.getChildFile (args[1]), taw::newSessionRate());

        if (set.error.isNotEmpty())
        {
            std::printf ("%s\n", set.error.toRawUTF8());
            return 1;
        }

        juce::String error;

        if (! taw::writeAbletonSession (set, cwd.getChildFile (args[2]), taw::newSessionRate(), error))
        {
            std::printf ("%s\n", error.toRawUTF8());
            return 1;
        }

        for (const auto& line : set.report)
            std::printf ("%s\n", line.toRawUTF8());

        return 0;
    }

    // taw --bouncetest <folder>   render a synthetic two-track mix to WAV and MP3 (no device needed)
    if (args[0] == "--bouncetest")
    {
        constexpr double rate = 44100.0;
        auto tone = [] (double freq, float level)
        {
            auto b = std::make_shared<juce::AudioBuffer<float>> (1, (int) rate * 3);
            for (int i = 0; i < b->getNumSamples(); ++i)
                b->setSample (0, i, level * (float) std::sin (juce::MathConstants<double>::twoPi * freq * i / rate));
            return b;
        };

        const auto folder = juce::File::getCurrentWorkingDirectory().getChildFile (args[1].isNotEmpty() ? args[1] : "bouncetest");
        int failures = 0;

        for (auto format : { taw::Bounce::Format::wav, taw::Bounce::Format::mp3 })
        {
            auto g = std::make_unique<taw::Graph>();
            g->tracks.resize (2);
            const auto len = (juce::int64) (rate * 3);
            g->tracks[0].clips.push_back ({ tone (440.0, 0.5f), 0, 0, len });
            g->tracks[1].clips.push_back ({ tone (660.0, 0.5f), (juce::int64) rate, 0, len }); // enters after 1 s
            g->tracks[1].gain = 0.5f;

            const auto file = folder.getChildFile (juce::String ("test") + taw::Bounce::extension (format));
            file.deleteFile();
            taw::Bounce b (std::move (g), { 0, (juce::int64) (rate * 4) }, rate, format, file);
            b.start();

            while (! b.isDone())
                juce::Thread::sleep (5);

            std::printf ("%s  %s  peak %.2f dB  %lld bytes  %s\n", file.getFileName().toRawUTF8(), b.succeeded() ? "ok" : "FAILED",
                         juce::Decibels::gainToDecibels (b.peak()), (long long) file.getSize(), b.errorMessage().toRawUTF8());
            failures += b.succeeded() ? 0 : 1;
        }

        return failures;
    }

    // taw --paralleltest: tracks rendering on several cores sound exactly as on one. Eight
    // tones, some sent pre and post fader to a bus that's sent to a second bus, one going only
    // to a bus: rendered both ways, compared sample for sample.
    if (args[0] == "--paralleltest")
    {
        constexpr double rate = 48000.0;
        constexpr int block = 256, length = 48000;

        auto render = [&] (bool parallel)
        {
            auto engineOwned = std::make_unique<taw::AudioEngine>();
            auto& engine = *engineOwned;
            engine.runWithoutDevice (rate, block, 0);
            engine.parallel = parallel;

            auto g = std::make_unique<taw::Graph>();
            g->numBuses = 2;

            for (int t = 0; t < 8; ++t)
            {
                auto tone = std::make_shared<juce::AudioBuffer<float>> (2, length);

                for (int i = 0; i < length; ++i)
                    for (int ch = 0; ch < 2; ++ch)
                        tone->setSample (ch, i, 0.1f * (float) std::sin (juce::MathConstants<double>::twoPi * (110.0 * (t + 1) + ch * 3.0) * i / rate));

                taw::TrackPlay tp;
                tp.stereo = true;
                tp.gain = 0.5f + 0.05f * (float) t;
                tp.clips.push_back ({ tone, 0, 0, length });

                if (t % 3 == 0)
                    tp.sends.push_back ({ 0, 0.7f, t % 2 == 0, {}, {} });

                if (t == 5)
                    tp.out = 1; // only to the second bus

                g->tracks.push_back (std::move (tp));
            }

            for (int bus = 0; bus < 2; ++bus)
            {
                taw::TrackPlay tp;
                tp.stereo = true;
                tp.busIndex = bus;
                tp.out = bus == 0 ? 1 : -1; // bus 1 feeds bus 2, which goes to the mix
                g->tracks.push_back (std::move (tp));
            }

            g->order = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
            engine.setGraph (std::move (g));
            engine.play (0);

            std::vector<float> out;
            std::vector<float> l (block), r (block);
            float* outs[] = { l.data(), r.data() };

            for (int pos = 0; pos < length; pos += block)
            {
                engine.renderWithoutDevice (outs, 2, block, nullptr, 0);
                out.insert (out.end(), l.begin(), l.end());
                out.insert (out.end(), r.begin(), r.end());
            }

            return out;
        };

        const auto one = render (false), many = render (true);
        float level = 0.0f, diff = 0.0f;

        for (size_t i = 0; i < one.size(); ++i)
        {
            level = std::max (level, std::abs (one[i]));
            diff = std::max (diff, std::abs (one[i] - many[i]));
        }

        const bool ok = one.size() == many.size() && level > 0.1f && diff == 0.0f;
        std::printf ("%s  %d helper threads: peak %.3f, largest difference %g\n", ok ? "ok  " : "FAIL",
                     juce::jlimit (0, 7, juce::SystemStats::getNumPhysicalCpus() - 1), level, diff);
        return ok ? 0 : 1;
    }

    // taw --loadtest <session-dir> [buffer] [from-seconds] [seconds]: plays the session through
    // the engine as fast as it renders, without a device, timing every block against how long
    // it lasts at that buffer size (default 512), and where the time goes, track by track.
    // Then the loop wrap and a start mid-song (notes chased). Read-only: nothing is saved.
    if (args[0] == "--loadtest" && args.size() >= 2)
    {
        // On a copy, so loading can't touch the session.
        const auto source = juce::File::getCurrentWorkingDirectory().getChildFile (args[1]);
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("taw-loadtest").getChildFile (source.getFileName());
        dir.getParentDirectory().deleteRecursively();
        source.copyDirectoryTo (dir);
        dir.getChildFile (".taw-lock").deleteFile(); // the copy isn't open anywhere
        const int buffer = args.size() > 2 ? args[2].getIntValue() : 512;
        const double from = args.size() > 3 ? args[3].getDoubleValue() : 0.0;
        const double seconds = args.size() > 4 ? args[4].getDoubleValue() : 30.0;

        auto app = std::make_unique<taw::App> (dir, juce::File(), juce::String(), juce::String());

        if (app->session == nullptr)
        {
            std::printf ("couldn't open: %s\n", app->message.toRawUTF8());
            return 1;
        }

        auto& s = *app->session;
        const double rate = s.sampleRate();
        auto& engine = app->engine;
        engine.runWithoutDevice (rate, buffer, 2);

        const auto loadStart = juce::Time::getMillisecondCounterHiRes();
        app->buildPlayback();

        while (app->hasBackgroundWork())
            app->pumpBackgroundWork();

        app->buildPlayback(); // with every plugin in
        std::printf ("%s: %d tracks, plugins loaded in %.1f s, %g Hz, buffer %d (%.1f ms)\n", s.name().toRawUTF8(), s.numTracks(),
                     (juce::Time::getMillisecondCounterHiRes() - loadStart) / 1000.0, rate, buffer, buffer * 1000.0 / rate);

        auto trackName = [&] (int t)
        {
            const auto tr = s.track (t);
            const auto plugin = tr[taw::ids::pluginName].toString();
            juce::StringArray fx;

            for (int slot = 0; slot < taw::Session::numSends; ++slot)
                if (const auto in = taw::Session::insert (tr, slot); in.isValid() && ! (bool) in[taw::ids::bypass])
                    fx.add (in[taw::ids::pluginName].toString());

            return juce::String (t + 1) + " " + tr[taw::ids::name].toString() + (plugin.isNotEmpty() ? " [" + plugin + "]" : juce::String())
                   + (fx.isEmpty() ? juce::String() : " + " + fx.joinIntoString (", "));
        };

        engine.profiling = true;
        const int numTracks = s.numTracks();
        std::vector<float> l ((size_t) buffer), r ((size_t) buffer), inL ((size_t) buffer), inR ((size_t) buffer);
        float* outs[] = { l.data(), r.data() };
        const float* ins[] = { inL.data(), inR.data() };
        const double budgetMs = buffer * 1000.0 / rate;

        auto run = [&] (const char* what, juce::int64 start, double secs)
        {
            struct Block { juce::int64 pos; double ms; std::vector<double> tracks; double mix; };
            std::vector<Block> blocks;
            std::vector<double> sum ((size_t) numTracks), most ((size_t) numTracks);
            double mixSum = 0.0, mixMost = 0.0;

            engine.resetBlockLoad();
            engine.play (start);
            const auto count = (int) (secs * rate / buffer);

            for (int b = 0; b < count; ++b)
            {
                const auto pos = engine.playhead();
                const auto t0 = juce::Time::getHighResolutionTicks();
                engine.renderWithoutDevice (outs, 2, buffer, ins, 2);
                const auto ms = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1000.0;
                std::vector<double> tracks ((size_t) numTracks);

                for (int t = 0; t < numTracks; ++t)
                {
                    tracks[(size_t) t] = engine.profileTrackMs[(size_t) t];
                    sum[(size_t) t] += tracks[(size_t) t];
                    most[(size_t) t] = std::max (most[(size_t) t], tracks[(size_t) t]);
                }

                mixSum += engine.profileMixMs;
                mixMost = std::max (mixMost, engine.profileMixMs);
                blocks.push_back ({ pos, ms, std::move (tracks), engine.profileMixMs });
            }

            engine.stop();

            std::vector<double> sorted;
            for (const auto& b : blocks)
                sorted.push_back (b.ms);
            std::sort (sorted.begin(), sorted.end());
            const auto pct = [&] (double p) { return sorted[(size_t) std::min ((double) sorted.size() - 1, p * (double) sorted.size())] / budgetMs * 100.0; };
            const auto over = std::count_if (sorted.begin(), sorted.end(), [&] (double ms) { return ms > budgetMs; });
            const auto close80 = std::count_if (sorted.begin(), sorted.end(), [&] (double ms) { return ms > budgetMs * 0.8; });

            std::printf ("\n== %s: %d blocks from %s\n", what, (int) blocks.size(), app->formatBars (start).toRawUTF8());
            std::printf ("   load  median %.0f%%  p99 %.0f%%  worst %.0f%%  |  over 100%%: %d  over 80%%: %d\n", pct (0.5), pct (0.99),
                         sorted.back() / budgetMs * 100.0, (int) over, (int) close80);
            std::printf ("   per track, ms per block (budget %.2f):  mean   worst\n", budgetMs);

            for (int t = 0; t < numTracks; ++t)
                if (most[(size_t) t] > 0.05)
                    std::printf ("     %-60s %5.2f  %6.2f\n", trackName (t).substring (0, 60).toRawUTF8(), sum[(size_t) t] / (double) blocks.size(), most[(size_t) t]);

            std::printf ("     %-60s %5.2f  %6.2f\n", "mix inserts", mixSum / (double) blocks.size(), mixMost);

            // The worst blocks: when, and who took the time.
            std::vector<size_t> order (blocks.size());
            std::iota (order.begin(), order.end(), (size_t) 0);
            std::sort (order.begin(), order.end(), [&] (size_t a, size_t b) { return blocks[a].ms > blocks[b].ms; });

            for (size_t i = 0; i < std::min<size_t> (6, order.size()); ++i)
            {
                const auto& b = blocks[order[i]];
                std::vector<int> who ((size_t) numTracks);
                std::iota (who.begin(), who.end(), 0);
                std::sort (who.begin(), who.end(), [&] (int x, int y) { return b.tracks[(size_t) x] > b.tracks[(size_t) y]; });
                juce::String top;

                for (int j = 0; j < std::min (3, numTracks); ++j)
                    top << "  " << (who[(size_t) j] + 1) << ":" << juce::String (b.tracks[(size_t) who[(size_t) j]], 2);

                std::printf ("   worst #%d  block %d  at %s  %.0f%%  (%.2f ms)  tracks%s  mix:%.2f\n", (int) i + 1, (int) order[i],
                             app->formatBars (b.pos).toRawUTF8(), b.ms / budgetMs * 100.0, b.ms, top.toRawUTF8(), b.mix);
            }
        };

        run ("from the start", (juce::int64) (from * rate), seconds);

        // The stats panel as it stood at the end of that.
        app->toggleStats();
        juce::Thread::sleep (300);
        app->system.sample();
        std::printf ("\n%s", app->screenText (100, 32).toRawUTF8());
        app->toggleStats();

        if ((bool) s.tree()[taw::ids::loop])
        {
            const auto loopEnd = (juce::int64) s.tree()[taw::ids::loopEnd];
            run ("across the loop wrap", std::max<juce::int64> (0, loopEnd - (juce::int64) (4.0 * rate)), 8.0);
        }

        run ("starting mid-song (chase)", (juce::int64) (s.contentEnd() / 2), 4.0);

        engine.profiling = false;
        app->session.reset(); // read-only: nothing saved
        return 0;
    }

    // taw --savetest <session-dir> <scratch-dir>: saving is something you do. On a copy: an edit
    // goes to unsaved\ (session.json untouched), closing asks, "don't save" drops it, "save"
    // keeps it and bounces last-saved in the background with the session's own plugins, a
    // crash's changes come back, and the session screen lists and plays the bounce.
    // taw --playlisttest <session>   loop recording's passes onto playlists (an empty session,
    // made with --new; no device needed): an audio take cut per pass, pad hits split pass by pass.
    if (args[0] == "--playlisttest" && args.size() >= 2)
    {
        const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile (args[1]);
        auto appOwned = std::make_unique<taw::App> (dir, juce::File(), juce::String(), juce::String());
        auto& app = *appOwned;
        int failures = 0;
        auto check = [&] (const char* what, bool ok)
        {
            std::printf ("%s  %s\n", ok ? "ok    " : "FAILED", what);
            failures += ok ? 0 : 1;
        };

        if (app.session == nullptr)
            return 1;

        using taw::Session;
        namespace ids = taw::ids;
        auto& s = *app.session;
        app.execute ("add");
        app.execute ("add");
        auto audio = s.track (0), hits = s.track (1);
        hits.setProperty (ids::kind, "sample", nullptr);
        const auto spb = s.samplesPerBeat();
        const auto pass = (juce::int64) std::llround (4.0 * spb); // a bar's loop from the start

        // Audio: a take through two whole passes and half a third, 100 samples early (latency).
        audio.appendChild (Session::makeClip ("audio/m.wav", 0, 0, 10), nullptr);
        app.placeLoopTake (0, "audio/take.wav", -100, pass * 5 / 2 + 100, { 0, pass });
        const auto audioLists = Session::playlists (audio);
        const auto mainClips = Session::clips (audio);
        check ("two passes: one playlist, the last on the main", audioLists.size() == 1 && mainClips.size() == 2);
        check ("the playlist is the main as it was plus pass 1",
               audioLists.size() == 1 && Session::clips (audioLists[0]).size() == 2 && (juce::int64) Session::clips (audioLists[0])[1][ids::offset] == 100
                   && (juce::int64) Session::clips (audioLists[0])[1][ids::start] == 0 && (juce::int64) Session::clips (audioLists[0])[1][ids::length] == pass);
        check ("pass 2 on the main, from the loop's start", mainClips.size() == 2 && (juce::int64) mainClips[1][ids::offset] == pass + 100
                                                               && (juce::int64) mainClips[1][ids::start] == 0 && (juce::int64) mainClips[1][ids::length] == pass);
        check ("named after the track", audioLists.size() == 1 && audioLists[0][ids::name].toString() == audio[ids::name].toString() + ".01");

        // Pad hits: a hit each pass, the third pass cut short by stopping.
        app.recording = app.loopRecording = true;
        app.loopRecStart = 0;
        app.loopRecEnd = pass;
        app.loopWrapBase = app.loopPassesDone = 0;
        app.midiPassStart = 0.0;
        app.addHit (1, 1.0, 0.8f);
        app.splitLoopPass();
        app.addHit (1, 2.0, 0.8f);
        app.splitLoopPass();
        app.addHit (1, 0.5, 0.8f);
        app.finishLoopRecording();
        app.recording = app.loopRecording = false;
        const auto hitLists = Session::playlists (hits);
        const auto mainHits = Session::placedHits (hits);
        check ("hits: pass 1 on a playlist", hitLists.size() == 1 && Session::placedHits (hitLists[0]).size() == 1
                                                && std::abs (Session::placedHits (hitLists[0])[0].beat - 1.0) < 1e-9);
        check ("hits: pass 2 on the main, the short pass dropped", mainHits.size() == 1 && std::abs (mainHits[0].beat - 2.0) < 1e-9);

        // Choosing a playlist swaps it with the main, and back.
        app.focusTrack (0);
        app.execute ("pl 1");
        check ("pl 1: the playlist plays", Session::mainPlaylistName (audio) == audio[ids::name].toString() + ".01"
                                               && Session::clips (audio).size() == 2 && (juce::int64) Session::clips (audio)[1][ids::offset] == 100);
        app.execute ("pl 1");
        check ("pl 1 again: back as it was", ! audio.hasProperty (ids::playlist) && (juce::int64) Session::clips (audio)[1][ids::offset] == pass + 100);
        app.execute ("pl new");
        check ("pl new: an empty main named .02", Session::clips (audio).empty() && Session::mainPlaylistName (audio).endsWith (".02")
                                                     && Session::playlists (audio).size() == 2);
        return failures;
    }

    // taw --separatetest <session>   ctrl+e / sep on an empty session (made with --new; no device
    // needed): a selection makes one clip three, no selection cuts at the cursor on the grid.
    if (args[0] == "--separatetest" && args.size() >= 2)
    {
        const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile (args[1]);
        auto appOwned = std::make_unique<taw::App> (dir, juce::File(), juce::String(), juce::String());
        auto& app = *appOwned;
        int failures = 0;
        auto check = [&] (const char* what, bool ok)
        {
            std::printf ("%s  %s\n", ok ? "ok    " : "FAILED", what);
            failures += ok ? 0 : 1;
        };

        if (app.session == nullptr)
            return 1;

        using taw::Session;
        namespace ids = taw::ids;
        auto& s = *app.session;
        app.execute ("add");
        app.execute ("add");
        auto audio = s.track (0), keys = s.track (1);
        keys.setProperty (ids::kind, "instrument", nullptr);
        const auto spb = s.samplesPerBeat();
        const auto bar = (juce::int64) std::llround (4.0 * spb);
        auto clipAt = [] (const juce::ValueTree& c) { return std::make_tuple ((juce::int64) c[ids::start], (juce::int64) c[ids::offset], (juce::int64) c[ids::length]); };
        using Span = std::tuple<juce::int64, juce::int64, juce::int64>;

        // Audio: four bars from file offset 100, bar 2 selected.
        audio.appendChild (Session::makeClip ("audio/a.wav", 0, 100, 4 * bar), nullptr);
        app.focusTrack (0);
        app.timeAnchor = bar;
        app.timeHead = 2 * bar;
        auto r = app.execute ("sep");
        auto clips = Session::clips (audio);
        check ("a selection inside a clip makes three", r.ok && clips.size() == 3);
        check ("...head, selection, tail, each from where it was in the file",
               clips.size() == 3 && clipAt (clips[0]) == Span (0, 100, bar) && clipAt (clips[1]) == Span (bar, 100 + bar, bar)
                   && clipAt (clips[2]) == Span (2 * bar, 100 + 2 * bar, 2 * bar));
        check ("...the selection stays on the new clip", app.timeSelection() == juce::Range<juce::int64> (bar, 2 * bar));
        std::printf ("       %s\n", r.message.toRawUTF8());

        // No selection: the cursor a little past bar 4's line is cut on the line.
        app.clearTimeSelection();
        app.setCursor (3 * bar + (juce::int64) (spb * 0.05));
        r = app.execute ("sep");
        clips = Session::clips (audio);
        check ("no selection: cut at the cursor, on the grid", r.ok && clips.size() == 4 && clipAt (clips[2]) == Span (2 * bar, 100 + 2 * bar, bar)
                                                                   && clipAt (clips[3]) == Span (3 * bar, 100 + 3 * bar, bar));
        std::printf ("       %s\n", r.message.toRawUTF8());

        app.execute ("undo");
        check ("undo puts the clip back whole", Session::clips (audio).size() == 3);

        app.setCursor (6 * bar);
        check ("no clip there: says so, changes nothing", ! app.execute ("sep").ok && Session::clips (audio).size() == 3);

        // MIDI: one clip of four bars with a note in each; bar 2 selected.
        auto midi = Session::makeMidiClip (0.0, 0.0, 16.0);

        for (double b : { 1.0, 5.0, 9.0, 13.0 })
            midi.appendChild (Session::makeNote (b, 1.0, 60, 100), nullptr);

        keys.appendChild (midi, nullptr);
        app.focusTrack (1);
        app.timeAnchor = bar;
        app.timeHead = 2 * bar;
        r = app.execute ("sep");
        const auto beatClips = Session::midiClips (keys);
        const auto notes = Session::placedNotes (keys);
        check ("MIDI: a selection makes three clips", r.ok && beatClips.size() == 3);
        check ("...the middle one exactly the selection", beatClips.size() == 3 && std::abs ((double) beatClips[1][ids::beat] - 4.0) < 1e-9
                                                              && std::abs ((double) beatClips[1][ids::len] - 4.0) < 1e-9);
        check ("...every note still plays where it did", notes.size() == 4 && std::abs (notes[1].start - 5.0) < 1e-9 && std::abs (notes[3].start - 13.0) < 1e-9);
        return failures;
    }

    // taw --duptracktest <session>   "dup track" on an empty session (made with --new; no device
    // or plugins needed): the copy's place, name, mixer, sends, automation, playlists, and a
    // plugin's state in a file of its own.
    if (args[0] == "--duptracktest" && args.size() >= 2)
    {
        const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile (args[1]);
        auto appOwned = std::make_unique<taw::App> (dir, juce::File(), juce::String(), juce::String());
        auto& app = *appOwned;
        int failures = 0;
        auto check = [&] (const char* what, bool ok)
        {
            std::printf ("%s  %s\n", ok ? "ok    " : "FAILED", what);
            failures += ok ? 0 : 1;
        };

        if (app.session == nullptr)
            return 1;

        using taw::Session;
        namespace ids = taw::ids;
        auto& s = *app.session;
        app.execute ("add Vox");
        app.execute ("add Gtr");
        app.execute ("bus Verb");
        app.execute ("1 send a verb");
        app.execute ("1 v -6");
        auto vox = s.track (0);
        vox.setProperty (ids::armed, true, nullptr);

        // A stand-in effect (not installed), its settings on disk.
        juce::ValueTree fx (ids::INSERT);
        fx.setProperty (ids::slot, "b", nullptr).setProperty (ids::plugin, "none", nullptr).setProperty (ids::pluginName, "Fake Comp", nullptr)
            .setProperty (ids::pluginState, "plugins/01-Fake_Comp.state", nullptr);
        vox.appendChild (fx, nullptr);
        dir.getChildFile ("plugins").createDirectory();
        dir.getChildFile ("plugins/01-Fake_Comp.state").replaceWithText ("the knobs");

        juce::ValueTree lane (ids::AUTO);
        lane.setProperty (ids::param, "vol", nullptr);
        juce::ValueTree point (ids::POINT);
        point.setProperty (ids::beat, 4.0, nullptr).setProperty (ids::val, 0.5, nullptr);
        lane.appendChild (point, nullptr);
        vox.appendChild (lane, nullptr);

        vox.appendChild (Session::makeClip ("audio/v.wav", 0, 0, 1000), nullptr);
        juce::ValueTree alt (ids::PLAYLIST);
        alt.setProperty (ids::name, "Vox.01", nullptr);
        vox.appendChild (alt, nullptr);

        auto count = [] (const juce::ValueTree& t, const juce::Identifier& type)
        {
            int n = 0;
            for (const auto& c : t)
                n += c.hasType (type) ? 1 : 0;
            return n;
        };

        auto r = app.execute ("1 dup track");
        std::printf ("       %s\n", r.message.toRawUTF8());
        const auto copy = s.track (1);
        check ("the copy goes right under the original", r.ok && s.numTracks() == 4 && copy[ids::name].toString() == "Vox.dup1"
                                                             && s.track (2)[ids::name].toString() == "Gtr");
        check ("...with its volume, send and automation", std::abs ((double) copy[ids::gainDb] + 6.0) < 1e-9 && Session::send (copy, 0).isValid()
                                                              && count (copy, ids::AUTO) == 1 && count (copy.getChildWithName (ids::AUTO), ids::POINT) == 1);
        check ("...the main playlist's clips, not the other playlists", Session::clips (copy).size() == 1 && count (copy, ids::PLAYLIST) == 0
                                                                            && count (vox, ids::PLAYLIST) == 1);
        check ("...not armed", ! (bool) copy[ids::armed]);

        const auto copyFx = Session::insert (copy, 1);
        const auto statePath = copyFx[ids::pluginState].toString();
        check ("...its effect in the same slot, with a state file of its own",
               copyFx.isValid() && copyFx[ids::pluginName].toString() == "Fake Comp" && statePath.isNotEmpty() && statePath != fx[ids::pluginState].toString()
                   && dir.getChildFile (statePath).loadFileAsString() == "the knobs");

        r = app.execute ("1 dup track empty");
        check ("empty: a new name, no clips", r.ok && s.track (1)[ids::name].toString() == "Vox.dup2" && Session::clips (s.track (1)).empty()
                                                  && count (s.track (1), ids::AUTO) == 1);

        app.execute ("undo");
        app.execute ("undo");
        check ("undo takes the copies away", s.numTracks() == 3 && s.track (1)[ids::name].toString() == "Gtr");

        r = app.execute ("3 dup track");
        check ("a bus copy is a bus of its own", r.ok && Session::isBusTrack (s.track (3)) && (int) s.track (3)[ids::busId] != (int) s.track (2)[ids::busId]);
        return failures;
    }

    // taw --screencmdtest <session> <scratch>   the session screen's command line (":"): it opens
    // without touching the new session's name, runs the commands that need no session (theme,
    // index, library), says which ones need one, and esc or backspace closes it. Run it with
    // TAW_CONFIG pointing somewhere scratch: the theme and the library are written there.
    if (args[0] == "--screencmdtest" && args.size() >= 3)
    {
        const auto source = juce::File::getCurrentWorkingDirectory().getChildFile (args[1]);
        const auto scratch = juce::File::getCurrentWorkingDirectory().getChildFile (args[2]);
        const auto dir = scratch.getChildFile (source.getFileName());
        scratch.deleteRecursively();
        scratch.createDirectory();
        source.copyDirectoryTo (dir);
        dir.getChildFile (".taw-lock").deleteFile();
        dir.getChildFile ("unsaved").deleteRecursively();

        int failures = 0;
        auto check = [&] (bool ok, const char* what)
        {
            std::printf ("%s  %s\n", ok ? "ok    " : "FAILED", what);
            failures += ok ? 0 : 1;
        };

        auto app = std::make_unique<taw::App> (dir, juce::File(), juce::String(), juce::String());
        app->interactive = true;

        while (app->hasBackgroundWork())
            app->pumpBackgroundWork();

        app->requestClose (false);

        while (app->closing != nullptr)
            app->stepClosing();

        check (app->session == nullptr, "on the session screen");

        auto press = [&] (taw::Key::Code code, wchar_t ch = 0)
        {
            taw::Key k;
            k.code = code;
            k.ch = ch;
            app->pressKey (k);
        };
        auto type = [&] (const juce::String& s)
        {
            for (auto c : s)
                press (taw::Key::Char, (wchar_t) c);
        };
        auto run = [&] (const juce::String& line)
        {
            type (":" + line);
            press (taw::Key::Enter);
            return app->message;
        };

        // Typing still names a new session; ":" opens the line and leaves the name be.
        type ("big");
        check (app->newSessionName == "big", "typing names a new session");
        type (":");
        check (app->screenCommandOpen && app->newSessionName == "big", ": opens the command line, the name stays");
        check (app->screenText (120, 30).contains ("index <folder>"), "the line shows what it takes");
        std::printf ("%s", app->screenText (120, 30).toRawUTF8());
        type ("dar");
        press (taw::Key::Escape);
        check (! app->screenCommandOpen && app->theme != "dar" && app->newSessionName == "big", "esc closes it, nothing run");
        type (":");
        press (taw::Key::Backspace);
        check (! app->screenCommandOpen, "backspace with nothing typed closes it");
        app->newSessionName.clear();

        // A setting for every session.
        const auto themeBefore = app->theme;
        run ("light");
        check (app->theme == "light" && ! app->messageIsError && ! app->screenCommandOpen, ":light sets the theme, the line closes");
        run ("dark");
        check (app->theme == "dark", ":dark");

        // The sample library: index a folder (as typed, or dropped after "index "), then library.
        const auto samples = scratch.getChildFile ("Some Samples");
        samples.createDirectory();
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream = samples.getChildFile ("kick.wav").createOutputStream();

            if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (16)))
            {
                juce::AudioBuffer<float> b (1, 4410);
                for (int i = 0; i < b.getNumSamples(); ++i)
                    b.setSample (0, i, std::sin ((float) i * 0.1f) * (1.0f - (float) i / (float) b.getNumSamples()));
                w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
            }
        }

        std::printf ("       %s\n", run ("index " + samples.getFullPathName().replace (" ", "\\ ")).toRawUTF8());
        check (! app->messageIsError, ":index <folder> starts indexing with no session");

        for (int i = 0; i < 500 && app->isIndexing(); ++i)
        {
            app->pollIndexing();
            juce::Thread::sleep (10);
        }

        app->pollIndexing();
        std::printf ("       %s\n", app->message.toRawUTF8());
        const auto lib = run ("library");
        std::printf ("       %s\n", lib.toRawUTF8());
        check (! app->messageIsError && lib.contains ("Some Samples"), ":library lists the folder");

        // A session's command: says so, and does nothing.
        const auto said = run ("3 v -6");
        std::printf ("       %s\n", said.toRawUTF8());
        check (app->messageIsError && said.contains ("needs a session open"), "a session command says it needs a session");
        check (app->session == nullptr && app->newSessionName.isEmpty(), "still on the session screen, no name typed");

        // help: the help box over the screen; its pages turn, and esc closes it.
        run ("help");
        {
            const auto screen = app->screenText (120, 30);
            std::printf ("%s", screen.toRawUTF8());
            check (app->helpVisible && screen.contains ("RECENT SESSIONS") && screen.contains (":index f"), ":help opens the session screen's help");
            check (! screen.contains ("next grid line") && ! screen.contains ("TRACKS"), "none of a session's pages");
        }
        press (taw::Key::Right);
        check (app->helpVisible, "arrows don't close it");
        type ("x");
        check (app->newSessionName.isEmpty(), "typing doesn't reach the name behind it");
        press (taw::Key::Escape);
        check (! app->helpVisible && app->session == nullptr && ! app->quitting(), "esc closes it, taw stays");
        run ("?");
        check (app->helpVisible, ":? opens it too");
        type ("?");
        check (! app->helpVisible, "? closes it");

        // ctrl+q quits from the screen, with the command line open too.
        type (":ind");
        {
            taw::Key k;
            k.code = taw::Key::Char;
            k.ch = 'q';
            k.ctrl = true;
            app->pressKey (k);
        }
        check (app->quitting() && ! app->screenCommandOpen && app->newSessionName.isEmpty(), "ctrl+q quits, from the command line too");

        app->theme = themeBefore;
        app->saveSettings();
        app.reset();
        scratch.deleteRecursively();
        std::printf ("%s\n", failures == 0 ? "ok" : "FAILED");
        return failures == 0 ? 0 : 1;
    }

    if (args[0] == "--savetest" && args.size() >= 3)
    {
        const auto source = juce::File::getCurrentWorkingDirectory().getChildFile (args[1]);
        const auto scratch = juce::File::getCurrentWorkingDirectory().getChildFile (args[2]);
        const auto dir = scratch.getChildFile (source.getFileName());
        scratch.deleteRecursively();
        scratch.createDirectory();
        source.copyDirectoryTo (dir);
        dir.getChildFile (".taw-lock").deleteFile(); // the copy isn't open anywhere
        dir.getChildFile ("unsaved").deleteRecursively();
        dir.getChildFile ("bounces").deleteRecursively();

        int failures = 0;
        auto check = [&] (bool ok, const char* what)
        {
            std::printf ("%s  %s\n", ok ? "ok    " : "FAILED", what);
            failures += ok ? 0 : 1;
        };

        const auto sessionFile = dir.getChildFile ("session.json");
        const auto work = dir.getChildFile ("unsaved");
        auto bpmOnDisk = [&] { return (double) taw::varToTree (juce::JSON::parse (sessionFile))[taw::ids::bpm]; };
        const auto bpm0 = bpmOnDisk();

        auto open = [&]
        {
            auto app = std::make_unique<taw::App> (dir, juce::File(), juce::String(), juce::String());
            app->interactive = true;

            while (app->hasBackgroundWork())
                app->pumpBackgroundWork();

            return app;
        };

        auto close = [&] (taw::App& app, const char* answer)
        {
            app.requestClose (false);

            if (answer != nullptr)
            {
                check (app.closePrompt.has_value(), "closing with changes asks");

                if (*answer == 'd')
                    std::printf ("%s", app.screenText (100, 14).toRawUTF8());

                taw::Key k;
                k.code = taw::Key::Char;
                k.ch = (wchar_t) answer[0];
                app.pressKey (k);
            }

            while (app.closing != nullptr)
                app.stepClosing();

            check (app.session == nullptr, "closed");
        };

        // An edit, then "don't save".
        {
            auto app = open();
            check (! app->unsaved, "freshly opened: nothing unsaved");
            const auto before = sessionFile.getLastModificationTime();
            std::printf ("       %s\n", app->execute ("bpm " + juce::String (bpm0 + 7)).message.toRawUTF8());
            app->pumpBackgroundWork();
            check (app->unsaved, "an edit is unsaved");
            check (work.getChildFile ("session.json").existsAsFile(), "the edit went to unsaved\\session.json");
            check (sessionFile.getLastModificationTime() == before, "session.json untouched");
            close (*app, "d");
            check (! work.exists(), "don't save: unsaved\\ is gone");
            check (juce::exactlyEqual (bpmOnDisk(), bpm0), "don't save: session.json as it was");
            check (app->backgroundBounces.empty(), "don't save: no bounce");
        }

        // An edit, then "save": session.json, and last-saved bounced in the background.
        {
            auto app = open();
            app->execute ("bpm " + juce::String (bpm0 + 9));
            app->pumpBackgroundWork();
            close (*app, "s");
            check (! work.exists(), "save: unsaved\\ is gone");
            check (juce::exactlyEqual (bpmOnDisk(), bpm0 + 9), "save: session.json has the edit");
            check (app->backgroundBounces.size() == 1, "save: last-saved is bouncing");
            std::printf ("%s", app->screenText (100, 12).toRawUTF8());

            while (! app->backgroundBounces.empty())
            {
                app->pollBackgroundBounces();
                juce::Thread::sleep (20);
            }

            const auto mp3 = dir.getChildFile ("bounces").getChildFile (dir.getFileName() + "-last-saved.mp3");
            check (mp3.getSize() > 10000, "save: bounces\\<session>-last-saved.mp3 written");
            std::printf ("       %s  %lld bytes  %s\n", mp3.getFullPathName().toRawUTF8(), (long long) mp3.getSize(), app->message.toRawUTF8());

            // The session screen: the bounces list has it (newest, so first), and it plays.
            check (! app->recentBounces.empty() && app->recentBounces.front().file == mp3 && app->bounceIndex == 0, "the bounces list has it, selected");

            // Areas: tab and shift+tab go round them.
            using Area = taw::App::PickArea;
            auto key = [&] (taw::Key::Code code, bool ctrl, bool shift = false)
            {
                taw::Key k;
                k.code = code;
                k.ctrl = ctrl;
                k.shift = shift;
                app->pressKey (k);
                return app->pickArea;
            };
            auto type = [&] (const juce::String& s)
            {
                for (auto c : s)
                {
                    taw::Key k;
                    k.code = taw::Key::Char;
                    k.ch = (wchar_t) c;
                    app->pressKey (k);
                }
            };
            check (app->pickArea == Area::sessions, "the session screen starts in the recent sessions");
            check (key (taw::Key::Tab, false) == Area::bounces, "tab: bounces");
            check (key (taw::Key::Tab, false) == Area::templates, "tab: round to the new session");
            check (key (taw::Key::Tab, false, true) == Area::bounces, "shift+tab: back to bounces");
            check (key (taw::Key::Tab, false, true) == Area::sessions, "shift+tab: sessions");
            check (key (taw::Key::Right, false) == Area::sessions, "right stays in the area");

            // The name is a text field: typing from a list starts it, the arrows move in it.
            type ("big song");
            check (app->pickArea == Area::templates && app->newSessionName == "big song", "typing from the sessions starts a name");
            key (taw::Key::Left, true);
            type ("new ");
            check (app->newSessionName == "big new song", "ctrl+left: back a word, typing goes there");
            key (taw::Key::Home, false);
            key (taw::Key::Right, true);
            key (taw::Key::Backspace, true);
            check (app->newSessionName == "new song", "ctrl+right, ctrl+backspace: a word off");
            key (taw::Key::End, false);
            key (taw::Key::Backspace, false);
            check (app->newSessionName == "new son" && app->pickArea == Area::templates, "end, backspace");
            app->newSessionName.clear();
            key (taw::Key::Tab, false);
            const auto before = app->sessionIndex;
            key (taw::Key::Down, false);
            check (app->sessionIndex == juce::jmin (before + 1, (int) app->recent.size() - 1) && app->pickArea == Area::sessions, "down: the next session");
            // Each recent session's size at a glance.
            {
                const auto it = std::find (app->recent.begin(), app->recent.end(), dir);
                const auto i = (size_t) (it - app->recent.begin());
                check (it != app->recent.end() && i < app->recentSummaries.size() && app->recentSummaries[i].ok
                           && app->recentSummaries[i].tracks == app->recentSummaries[i].tracks && juce::exactlyEqual (app->recentSummaries[i].bpm, bpm0 + 9),
                       "recent sessions: tracks, bpm, plugins, length");
                std::printf ("%s", app->screenText (140, 20).toRawUTF8());
            }

            key (taw::Key::Tab, false);
            check (app->pickArea == Area::bounces, "tab: bounces");
            std::printf ("%s", app->screenText (100, 24).toRawUTF8());
            std::printf ("%s", app->screenText (70, 16).toRawUTF8());
            // The sessions folder: a client's drive. Nowhere to put it fails; a new folder on a
            // drive that's there is made. (The real setting goes back as it was.)
            {
                const auto old = app->sessionsFolder;
                const auto drive = scratch.getChildFile ("client drive");
                check (! app->setSessionsFolder (drive.getChildFile ("sessions")).ok, "sessions folder: somewhere that isn't there fails");
                drive.createDirectory();
                const auto r = app->setSessionsFolder (drive.getChildFile ("sessions"));
                check (r.ok && drive.getChildFile ("sessions").isDirectory() && app->newSessionParent == drive.getChildFile ("sessions"),
                       "sessions folder: made, and new sessions go there");
                std::printf ("       %s\n", r.message.toRawUTF8());
                app->sessionsFolder = old;
                app->saveSettings();
            }

            app->playBounce (mp3);

            while (! app->bouncePlayer->loaded.load())
                juce::Thread::sleep (10);

            app->pollBackgroundBounces();
            check (app->bouncePlayer != nullptr && app->bounceLengthSeconds() > 1.0, "it plays");
            std::printf ("       %.1f s long\n", app->bounceLengthSeconds());
            app->stopBounce();
        }

        // "Save only": session.json, no bounce.
        {
            const auto mp3 = dir.getChildFile ("bounces").getChildFile (dir.getFileName() + "-last-saved.mp3");
            const auto bounced = mp3.getLastModificationTime();
            auto app = open();
            app->execute ("bpm " + juce::String (bpm0 + 10));
            app->pumpBackgroundWork();
            close (*app, "o");
            check (juce::exactlyEqual (bpmOnDisk(), bpm0 + 10), "save only: session.json has the edit");
            check (app->backgroundBounces.empty() && mp3.getLastModificationTime() == bounced, "save only: no bounce");
        }

        // Nothing changed: closing doesn't ask.
        {
            auto app = open();
            close (*app, nullptr);
            check (app->backgroundBounces.empty(), "opened and closed: no bounce");
        }

        // A crash (or the window closed) with changes: they're kept, and offered next time.
        {
            auto app = open();
            app->execute ("bpm " + juce::String (bpm0 + 11));
            app->pumpBackgroundWork();
        }

        check (work.getChildFile ("session.json").existsAsFile(), "window closed on changes: kept in unsaved\\");
        check (juce::exactlyEqual (bpmOnDisk(), bpm0 + 10), "...and session.json not changed");

        {
            auto app = open();
            check (app->unsaved && app->session->recovered, "reopened: the changes are back, unsaved");
            check (juce::exactlyEqual ((double) app->session->tree()[taw::ids::bpm], bpm0 + 11), "...with the edit");
            close (*app, "d");
        }

        std::printf ("%s\n", failures == 0 ? "all ok" : "FAILURES");
        return failures;
    }

    // taw --mcptest <scratch-folder>   agents in the open session (docs/plan-taw-mcp.md), end to
    // end with no audio device: a session made there and opened as a person's taw opens it
    // (edits to unsaved\), its control socket, and a bridge on another thread talking to it
    // through real sockets, the App pumped on this thread between calls as the frame loop would.
    // Uses the config folder's agents listing (TAW_CONFIG for another).
    if (args[0] == "--mcptest" && args.size() >= 2)
    {
        const auto scratch = juce::File::getCurrentWorkingDirectory().getChildFile (args[1]);
        scratch.deleteRecursively();
        scratch.createDirectory();
        const auto dir = scratch.getChildFile ("mcp song");

        {
            juce::String error;

            if (taw::Session::create (dir, {}, 44100.0, error) == nullptr)
            {
                std::printf ("couldn't make a session: %s\n", error.toRawUTF8());
                return 1;
            }
        }

        // A tick for a sample track to load.
        const auto tick = scratch.getChildFile ("tick.wav");
        {
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (tick);
            auto writer = juce::WavAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (24));
            juce::AudioBuffer<float> b (1, 2205);

            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample (0, i, 0.5f * std::sin ((float) i * 0.3f) * std::exp (-(float) i / 300.0f));

            writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        }

        const auto sessionJson = dir.getChildFile ("session.json").loadFileAsString();
        int failures = 0;
        auto check = [&] (bool ok, const juce::String& what)
        {
            std::printf ("%s  %s\n", ok ? "ok    " : "FAILED", what.toRawUTF8());
            failures += ok ? 0 : 1;
        };

        auto app = std::make_unique<taw::App> (dir, juce::File(), juce::String(), juce::String());
        app->interactive = true; // as a person's taw: edits to unsaved\, never session.json

        if (app->session == nullptr)
        {
            std::printf ("couldn't open: %s\n", app->message.toRawUTF8());
            return 1;
        }

        auto& s = *app->session;
        auto pump = [&]
        {
            app->pollAgents();
            app->pumpBackgroundWork();
        };

        // The user's own track, with the cursor on it.
        app->execute ("add Vox");
        app->pumpBackgroundWork();
        app->recentEdits.window = 0.6; // seconds: short, so the test needn't wait long
        app->agentIdle = 1.0e7;        // the user at the keys throughout (idle is tested on its own)
        app->lastInputAt = juce::Time::getMillisecondCounterHiRes();
        auto wait = [&] (int ms)
        {
            for (const auto until = juce::Time::getMillisecondCounterHiRes() + ms; juce::Time::getMillisecondCounterHiRes() < until;)
            {
                pump();
                juce::Thread::sleep (5);
            }
        };

        check (app->startAgents(), "the control socket listens");
        pump();
        {
            bool listed = false;

            for (const auto& l : taw::agents::running())
                listed = listed || (l.pid == taw::safety::thisProcess() && l.port == app->agentServer->port() && juce::File (l.session) == dir);

            check (listed, "agents/<pid>.json says where, with the session");
        }

        // The bridge finds it by the session's folder, as --session does.
        taw::agents::Bridge bridge (dir.getFullPathName(), {});
        int rpcId = 0;
        auto rpc = [&] (taw::agents::Bridge& b, const juce::String& line)
        {
            auto f = std::async (std::launch::async, [&b, line] { return b.handle (line); });

            while (f.wait_for (std::chrono::milliseconds (1)) != std::future_status::ready)
                pump();

            pump();
            const auto out = f.get();
            return out.isEmpty() ? juce::var() : juce::JSON::parse (out);
        };
        auto method = [&] (const juce::String& name, const juce::String& params)
        { return rpc (bridge, "{\"jsonrpc\":\"2.0\",\"id\":" + juce::String (++rpcId) + ",\"method\":\"" + name + "\",\"params\":" + params + "}"); };

        struct Reply
        {
            bool error = true;
            juce::String text;
            juce::var data;
        };

        auto tool = [&] (const juce::String& name, const juce::String& arguments)
        {
            const auto r = method ("tools/call", "{\"name\":\"" + name + "\",\"arguments\":" + arguments + "}");
            Reply out;
            out.error = r["result"]["isError"];
            out.text = r["result"]["content"][0]["text"].toString();
            const auto json = out.text.startsWithChar ('{') ? out.text : out.text.fromFirstOccurrenceOf ("\n", false, false);
            out.data = json.startsWithChar ('{') ? juce::JSON::parse (json) : juce::var();
            std::printf ("       %-12s %-40s %s\n", name.toRawUTF8(), arguments.substring (0, 40).toRawUTF8(),
                         (out.error ? out.text : out.text.upToFirstOccurrenceOf ("\n", false, false)).substring (0, 110).toRawUTF8());
            return out;
        };

        // MCP itself.
        {
            const auto r = method ("initialize", R"({"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"mcptest","version":"1"}})");
            check (r["result"]["protocolVersion"].toString() == "2025-06-18" && r["result"]["serverInfo"]["name"].toString() == "taw"
                       && r["result"]["capabilities"]["tools"].isObject(),
                   "initialize: version, capabilities, serverInfo");
            check (bridge.handle (R"({"jsonrpc":"2.0","method":"notifications/initialized"})").isEmpty(), "a notification gets no answer");
            const auto list = method ("tools/list", "{}")["result"]["tools"];
            juce::StringArray names;

            for (const auto& t : *list.getArray())
                if (t["inputSchema"]["type"].toString() == "object")
                    names.add (t["name"].toString());

            check (names.size() == 15 && names.contains ("get_session") && names.contains ("run_command") && names.contains ("write_hits") && names.contains ("set_tempo")
                       && names.contains ("new_session"),
                   "tools/list: " + names.joinIntoString (", "));
            check (method ("ping", "{}")["result"].isObject(), "ping");
            check ((int) rpc (bridge, "this isn't json")["error"]["code"] == -32700, "garbage: a parse error");
            check ((int) method ("resources/list", "{}")["error"]["code"] == -32601, "an unknown method: -32601");
            check ((int) method ("tools/call", R"({"name":"make_it_good","arguments":{}})")["error"]["code"] == -32602, "an unknown tool");
        }

        // Reading.
        {
            const auto r = tool ("get_session", "{}");
            const auto tracks = r.data["tracks"];
            check (! r.error && juce::approximatelyEqual ((double) r.data["bpm"], s.bpm()) && tracks.size() == 1 && tracks[0]["name"].toString() == "Vox"
                       && tracks[0]["locked"].toString().contains ("cursor") && r.data["focused"].toString() == "t1" && tracks[0]["id"].toString() == "t1"
                       && tracks[0]["number"].toString() == "1",
                   "get_session: tempo, the track (id t1, number 1), the user's cursor on it");
        }

        // Tracks, notes, hits.
        {
            const auto r = tool ("add_track", R"({"kind":"instrument","name":"Bass"})");
            check (! r.error && r.data["track"].toString() == "t2" && r.data["number"].toString() == "2" && s.numTracks() == 2 && taw::Session::isInstrumentTrack (s.track (1))
                       && ! (bool) s.track (1)[taw::ids::armed] && app->focused == 0,
                   "add_track: an instrument track, not armed, the cursor left on Vox");
            check (app->message.startsWith ("mcptest: "), "the status line says who: " + app->message);
            check (app->um()->getUndoDescription() == "mcptest: add instrument track Bass", "one undo step, named for the agent: " + app->um()->getUndoDescription());
        }
        {
            const auto r = tool ("write_notes", R"({"track":"2","beat":0,"notes":[{"beat":0,"length":1,"pitch":48},{"beat":1,"length":1,"pitch":52},{"beat":2,"length":1,"pitch":55,"vel":90},{"beat":3,"length":1,"pitch":60}]})");
            const auto clips = taw::Session::midiClips (s.track (1));
            check (! r.error && clips.size() == 1 && juce::approximatelyEqual ((double) clips[0][taw::ids::len], 4.0) && taw::Session::placedNotes (s.track (1)).size() == 4,
                   "write_notes: a new one-bar clip with 4 notes");
            const auto more = tool ("write_notes", R"({"track":"Bass","clip":0,"notes":[{"beat":6,"length":1,"pitch":67}]})");
            const auto grown = taw::Session::midiClips (s.track (1));
            check (! more.error && grown.size() == 1 && (double) grown[0][taw::ids::len] >= 7.0 && taw::Session::placedNotes (s.track (1)).size() == 5,
                   "write_notes into clip 0 (by name): it grows to take the note");
            const auto bad = tool ("write_notes", R"({"track":"2","notes":[{"beat":0,"length":1,"pitch":200}]})");
            check (bad.error && bad.text.startsWith ("[invalid]") && taw::Session::placedNotes (s.track (1)).size() == 5, "a pitch of 200 is refused, nothing written");
        }
        {
            const auto r = tool ("add_track", R"({"kind":"sample","name":"Hat"})");
            check (! r.error && s.numTracks() == 3 && taw::Session::isSampleTrack (s.track (2)), "add_track: a sample track");
            const auto hits = tool ("write_hits", R"({"track":"3","beat":0,"hits":[{"beat":0},{"beat":0.5,"vel":0.4},{"beat":1},{"beat":1.5,"vel":0.4},{"beat":2},{"beat":2.5,"vel":0.4},{"beat":3},{"beat":3.5,"vel":0.4}]})");
            check (! hits.error && taw::Session::hitClips (s.track (2)).size() == 1 && taw::Session::placedHits (s.track (2)).size() == 8, "write_hits: eighths in a new clip");
            const auto read = tool ("get_track", R"({"track":"3"})");
            check (read.data["clips"][0]["hits"].size() == 8 && juce::approximatelyEqual ((double) read.data["clips"][0]["hits"][1]["vel"], 0.4), "get_track: the hits, with velocities");
            const auto summary = tool ("get_session", "{}");
            check ((int) summary.data["tracks"][1]["clips"][0]["note_count"] == 5, "get_session: note counts without include_notes");
        }

        // ctrl+z takes an agent's edit back; ctrl+y puts it back.
        {
            const auto undone = app->undoEdit();
            app->pumpBackgroundWork();
            check (undone.ok && taw::Session::hitClips (s.track (2)).empty(), "ctrl+z: " + undone.message);
            const auto redone = app->redoEdit();
            app->pumpBackgroundWork();
            check (redone.ok && taw::Session::placedHits (s.track (2)).size() == 8, "ctrl+y: " + redone.message);
            wait (700); // the user's undo counts as their edit: let it age
        }

        // The passthrough.
        {
            const auto cursor = app->cursor;
            const auto r = tool ("run_command", R"({"command":"2 v -6"})");
            check (! r.error && juce::approximatelyEqual ((double) s.track (1)[taw::ids::gainDb], -6.0) && app->cursor == cursor && app->focused == 0,
                   "run_command 2 v -6: done, the cursor and focus as they were");
            check (app->um()->getUndoDescription() == "mcptest: 2 v -6", "...one undo step: " + app->um()->getUndoDescription());
            const auto mine = tool ("run_command", R"({"command":"v -3"})");
            check (mine.error && mine.text.startsWith ("[hands-off]") && juce::approximatelyEqual ((double) s.track (0)[taw::ids::gainDb], 0.0),
                   "run_command v -3 (no track: the focused one) refused, Vox as it was");
            check (app->um()->getUndoDescription() == "mcptest: 2 v -6", "...and rolled back without a trace");

            for (const auto* line : { "save", "undo", "play", "@5", "2", "b" })
            {
                const auto no = tool ("run_command", juce::String (R"({"command":")") + line + "\"}");
                check (no.error && no.text.startsWith ("[invalid]"), juce::String ("run_command ") + line + " is the user's");
            }

            const auto bpm = tool ("run_command", R"({"command":"bpm 96"})");
            check (! bpm.error && juce::approximatelyEqual (s.bpm(), 96.0), "run_command bpm 96");
        }

        // The focused track.
        {
            app->focusTrack (1);
            const auto r = tool ("write_notes", R"({"track":"2","beat":8,"notes":[{"beat":0,"length":1,"pitch":50}]})");
            check (r.error && r.text.startsWith ("[hands-off]") && taw::Session::midiClips (s.track (1)).size() == 1, "write_notes on the focused track refused, nothing written");
            const auto gone = tool ("remove_track", R"({"track":"2"})");
            check (gone.error && s.numTracks() == 3, "remove_track of the focused track refused");
            app->focusTrack (0);
        }

        // The time selection: bar 1, from the ruler (every track).
        {
            app->topLane = taw::App::TopLane::ruler;
            app->timeAnchor = 0;
            app->timeHead = (juce::int64) std::llround (s.samplesPerBar());
            const auto in = tool ("write_hits", R"({"track":"3","clip":0,"hits":[{"beat":0.25}]})");
            check (in.error && in.text.contains ("selection") && taw::Session::placedHits (s.track (2)).size() == 8, "write_hits into a clip in the selection refused");
            const auto out = tool ("write_hits", R"({"track":"3","beat":8,"hits":[{"beat":0},{"beat":2}]})");
            check (! out.error && taw::Session::hitClips (s.track (2)).size() == 2, "write_hits at bar 3, outside it: fine");
            const auto lost = tool ("get_session", "{}");
            check (lost.data["selection"]["tracks"].size() == 3, "get_session: the selection and its tracks");
            const auto m1 = tool ("add_marker", R"({"beat":2,"name":"In it"})");
            const auto m2 = tool ("add_marker", R"({"beat":16,"name":"Bridge"})");
            check (m1.error && ! m2.error, "a marker inside the selection refused, one after it made");
            app->timeAnchor = app->timeHead = -1;
            app->topLane = taw::App::TopLane::none;
        }

        // What the user just changed.
        {
            app->execute ("3 m");
            app->pumpBackgroundWork();
            const auto r = tool ("write_hits", R"({"track":"3","clip":1,"hits":[{"beat":1}]})");
            check (r.error && r.text.contains ("changed"), "write_hits on a track the user just muted refused");
            wait (700);
            const auto later = tool ("write_hits", R"({"track":"3","clip":1,"hits":[{"beat":1}]})");
            check (! later.error, "...and fine a moment later");
        }

        // Playing.
        {
            app->engine.play (0);
            const auto r = tool ("add_track", R"({"kind":"audio","name":"Gtr"})");
            check (r.error && r.text.startsWith ("[playing]") && s.numTracks() == 3, "add_track while playing refused");
            const auto read = tool ("get_session", "{}");
            check (! read.error && (bool) read.data["playing"], "get_session while playing");
            const auto screen = app->screenText (120, 12);
            check (app->agentHeld == 1 && screen.contains ("mcptest waiting") && screen.contains ("held: waiting for stop"),
                   "the transport and title line show the agent held off");
            app->engine.stop();
            app->pollAgents();
            check (app->agentHeld == 0 && app->message.contains ("mcptest tried to edit once while it played"), "stopping says it can edit now: " + app->message);
        }

        // Away from the keys: the focused track is fair game (playing still isn't), and what an
        // agent changes is lit.
        {
            const auto vox = s.track (app->focused);
            app->agentIdle = 0.3;
            app->lastInputAt = juce::Time::getMillisecondCounterHiRes();
            const auto busy = tool ("run_command", R"({"command":"1 v -4"})");
            check (busy.text.startsWith ("[hands-off]") && busy.text.contains ("left the keys"), "at the keys: the focused track refused, saying when it frees");
            wait (400);
            app->engine.play (0);
            check (tool ("run_command", R"({"command":"1 v -4"})").text.startsWith ("[playing]"), "idle but playing: still refused");
            app->engine.stop();
            pump();
            app->agentMarks.clear(); // only this edit's
            const auto idle = tool ("run_command", R"({"command":"1 v -4"})");
            check (! idle.error && std::abs ((double) vox[taw::ids::gainDb] + 4.0) < 0.01, "idle: the focused track's volume changed: " + idle.text);
            check (app->agentMarkAge (vox, "vol") >= 0.0 && app->agentMarkAge (vox) >= 0.0 && app->agentMarkAge (s.track (1)) < 0.0,
                   "the track and its volume are lit, no other track");
            app->agentIdle = 1.0e7;
            app->lastInputAt = juce::Time::getMillisecondCounterHiRes();
            app->execute ("1 v 0"); // as it was, for what follows
            wait (700);
        }

        // A sample track with a sound: answered once the import is in.
        {
            const auto r = tool ("add_track", "{\"kind\":\"sample\",\"name\":\"Tick\",\"sample\":" + juce::JSON::toString (tick.getFullPathName()) + "}");
            const auto t = s.track (s.numTracks() - 1);
            check (! r.error && s.numTracks() == 4 && taw::Session::isSampleTrack (t) && t[taw::ids::name].toString() == "Tick" && t[taw::ids::sample].toString().isNotEmpty(),
                   "add_track sample with a file: imported, named");
            check (app->um()->getUndoDescription().startsWith ("mcptest: add sample track"), "...one undo step: " + app->um()->getUndoDescription());
        }

        // Clips and tracks away.
        {
            const auto r = tool ("remove_clip", R"({"track":"3","clip":1})");
            check (! r.error && taw::Session::hitClips (s.track (2)).size() == 1, "remove_clip");
            const auto gone = tool ("remove_track", R"({"track":"3"})");
            check (! gone.error && s.numTracks() == 3 && s.track (1)[taw::ids::name].toString() == "Bass" && app->focused == 0, "remove_track: Hat gone, the cursor still on Vox");
        }

        // Track ids: the same through a move, an undo and a copy; never handed out twice.
        auto idOf = [&] (const juce::String& name)
        {
            for (int t = 0; t < s.numTracks(); ++t)
                if (s.track (t)[taw::ids::name].toString() == name)
                    return s.track (t)[taw::ids::uid].toString();

            return juce::String();
        };
        {
            check (idOf ("Vox") == "t1" && idOf ("Bass") == "t2" && idOf ("Tick") == "t4", "ids: Vox t1, Bass t2, Tick t4 (Hat's t3 went with it)");
            app->focusTrack (1);
            const auto moved = app->moveTracks (-1); // alt+up: Bass above Vox
            app->pumpBackgroundWork();
            app->focusTrack (1); // the cursor back on Vox, now second
            wait (700);
            const auto r = tool ("get_track", R"({"track":"t2"})");
            check (moved.ok && r.data["name"].toString() == "Bass" && r.data["number"].toString() == "1" && idOf ("Bass") == "t2",
                   "moved to the top: Bass is number 1, still t2");
            const auto byId = tool ("write_notes", R"({"track":"t2","clip":0,"clip_beat":0,"notes":[{"beat":3.5,"length":0.5,"pitch":62}]})");
            check (! byId.error && taw::Session::placedNotes (s.track (0)).size() == 6, "write_notes by id, with clip_beat");
            const auto stale = tool ("write_notes", R"({"track":"t2","clip":0,"clip_beat":4,"notes":[{"beat":0,"length":1,"pitch":62}]})");
            check (stale.error && stale.text.contains ("changed"), "a clip_beat that doesn't match: refused");

            const auto gone = tool ("remove_track", R"({"track":"t4"})");
            const auto undone = app->undoEdit();
            app->pumpBackgroundWork();
            check (! gone.error && undone.ok && idOf ("Tick") == "t4", "removed by an agent, back with ctrl+z: still t4");
            wait (700);

            app->execute ("1 dup track"); // the user copies Bass
            app->pumpBackgroundWork();
            check (idOf ("Bass") == "t2" && idOf ("Bass.dup1") == "t5", "a copy gets an id of its own: " + idOf ("Bass.dup1"));
            app->focusTrack (s.trackById ("t1"));
            wait (700);
            const auto fresh = tool ("add_track", R"({"kind":"bus","name":"Verb"})");
            check (! fresh.error && fresh.data["track"].toString() == "t6", "a new track: t6 (t3 is never used again)");
        }

        // The setting: off, nothing listens and taw --mcp says why; on again, back.
        {
            const auto off = app->execute ("agents off");
            const auto listing = taw::agents::folder().getChildFile (juce::String (taw::safety::thisProcess()) + ".json");
            check (off.ok && app->agentServer == nullptr && ! listing.existsAsFile() && ! app->agentsOn, "agents off: no socket, no listing");
            const auto r = tool ("get_session", "{}");
            check (r.error && r.text.startsWith ("[agents-off]"), "taw --mcp: " + r.text);
            const auto on = app->execute ("agents on");
            pump();
            check (on.ok && app->agentServer != nullptr && listing.existsAsFile(), "agents on: listening again");
            check (! tool ("get_session", "{}").error, "...and the bridge finds it");
            const auto panel = app->projectSettings();
            check (std::any_of (panel.begin(), panel.end(), [] (const auto& row) { return row.name.startsWith ("Agents"); }), "the project panel has it");
        }

        // Tempo: with no audio it just changes; with audio the user says what the clips do.
        {
            const auto direct = tool ("set_tempo", R"({"bpm":100})");
            check (! direct.error && juce::approximatelyEqual (s.bpm(), 100.0), "set_tempo 100 with no audio clips: done");

            // The user records (well, imports) some audio at bar 2.
            app->cursor = (juce::int64) std::llround (s.samplesPerBar());
            app->execute ("import \"" + tick.getFullPathName() + "\"");

            while (app->hasBackgroundWork())
                pump();

            app->focusTrack (s.trackById ("t1"));
            wait (700);
            juce::ValueTree audioClip;

            for (int t = 0; t < s.numTracks(); ++t)
                if (const auto c = taw::Session::clips (s.track (t)); ! c.empty())
                    audioClip = c.front();

            const auto start = (juce::int64) audioClip[taw::ids::start];
            check (audioClip.isValid() && start > 0, "an audio clip at bar 2");

            const auto asked = tool ("set_tempo", R"({"bpm":120})");
            check (asked.error && asked.text.startsWith ("[needs-user]") && app->tempoAsk.has_value() && juce::approximatelyEqual (s.bpm(), 100.0),
                   "set_tempo 120 with audio: needs the user, nothing changed yet");
            check (app->screenText (100, 30).contains ("TEMPO"), "taw shows the tempo box");
            std::printf ("%s", app->screenText (100, 16).toRawUTF8());
            const auto waiting = tool ("get_session", "{}");
            check (waiting.data["tempo_request"]["status"].toString() == "waiting for the user", "get_session: tempo_request waiting");
            check (tool ("set_tempo", R"({"bpm":120})").text.startsWith ("[needs-user]"), "asking again: still waiting");

            auto press = [&] (wchar_t c, taw::Key::Code code = taw::Key::Char)
            {
                taw::Key k;
                k.code = code;
                k.ch = c;
                app->pressKey (k);
                app->pumpBackgroundWork();
            };

            press ('s');
            check (app->tempoAsk.has_value(), "s (stretch): not built yet, the box stays");
            press ('b');
            check (! app->tempoAsk.has_value() && juce::approximatelyEqual (s.bpm(), 120.0) && (juce::int64) audioClip[taw::ids::start] == std::llround ((double) start * 100.0 / 120.0),
                   "b: 120 bpm, the clip moved with the beats");
            check (app->um()->getUndoDescription() == "mcptest: bpm 120", "...one undo step, the agent's: " + app->um()->getUndoDescription());
            const auto after = tool ("set_tempo", R"({"bpm":120})");
            check (! after.error && after.text.contains ("moved with the beats"), "the agent asks again: " + after.text);

            const auto moved = (juce::int64) audioClip[taw::ids::start];
            tool ("set_tempo", R"({"bpm":90})");
            press (0, taw::Key::Escape);
            const auto declined = tool ("set_tempo", R"({"bpm":90})");
            check (juce::approximatelyEqual (s.bpm(), 120.0) && declined.text.startsWith ("[declined]"), "esc: the tempo stays, the agent hears no");

            tool ("set_tempo", R"({"bpm":80})");
            press ('t');
            check (juce::approximatelyEqual (s.bpm(), 80.0) && (juce::int64) audioClip[taw::ids::start] == moved, "t: 80 bpm, the clip stays where it is in time");
            const auto read = tool ("get_session", "{}");
            check (read.data["tempo_request"]["status"].toString() == "applied", "get_session: tempo_request applied");
        }

        // Automation: points on a lane (made, shown, lit), a span replaced, read back.
        {
            const auto made = tool ("add_track", R"({"kind":"audio","name":"Pad"})");
            const auto id = made.data["track"].toString();
            const auto pad = s.track (s.numTracks() - 1);
            auto pointsOf = [&] (const juce::String& param)
            {
                juce::StringArray out;

                for (const auto& p : taw::Session::points (taw::Session::automation (pad, param)))
                    out.add (juce::String ((double) p[taw::ids::beat], 2) + ":" + juce::String ((double) p[taw::ids::val], 2));

                return out.joinIntoString (" ");
            };

            app->agentMarks.clear();
            const auto vol = tool ("write_automation", "{\"track\":\"" + id + R"(","param":"volume","points":[{"beat":0,"value":-6},{"beat":4,"value":0}]})");
            check (! vol.error && pointsOf ("vol") == "0.00:-6.00 4.00:0.00", "write_automation: a vol lane with two points: " + pointsOf ("vol"));
            check ((bool) pad[taw::ids::lanes] && app->agentMarkAge (pad, "auto vol") >= 0.0, "...shown under the track, and lit");
            check (app->um()->getUndoDescription().startsWith ("mcptest: automate"), "...one undo step: " + app->um()->getUndoDescription());

            tool ("write_automation", "{\"track\":\"" + id + R"(","param":"vol","points":[{"beat":2,"value":-12}]})");
            check (pointsOf ("vol") == "0.00:-6.00 2.00:-12.00 4.00:0.00", "a point between: the rest kept: " + pointsOf ("vol"));
            tool ("write_automation", "{\"track\":\"" + id + R"(","param":"vol","points":[{"beat":1,"value":-3},{"beat":3,"value":-3}]})");
            check (pointsOf ("vol") == "0.00:-6.00 1.00:-3.00 3.00:-3.00 4.00:0.00", "a span: what was inside it replaced: " + pointsOf ("vol"));

            tool ("write_automation", "{\"track\":\"" + id + R"(","param":"pan","points":[{"beat":0,"value":"l30"},{"beat":8,"value":2}]})");
            check (pointsOf ("pan") == "0.00:-0.30 8.00:1.00", "pan from text (l30), a value past the end clamped: " + pointsOf ("pan"));

            const auto back = tool ("get_track", "{\"track\":\"" + id + "\"}");
            check (back.data["automation"].size() == 2 && back.data["automation"][0]["points"].size() == 4, "get_track reads the lanes back");

            tool ("write_automation", "{\"track\":\"" + id + R"(","param":"pan","points":[],"replace":"all"})");
            check (pointsOf ("pan").isEmpty(), "replace all with no points: the lane emptied");

            check (tool ("write_automation", R"({"track":"mix","param":"pan","points":[{"beat":0,"value":0}]})").text.startsWith ("[invalid]"), "the mix has no pan lane");
            check (tool ("write_automation", R"({"track":"mix","param":"vol","points":[{"beat":0,"value":-2}]})").error == false
                       && taw::Session::automation (s.tree(), "vol").isValid(),
                   "the mix's volume");
            check (tool ("write_automation", "{\"track\":\"" + id + R"(","param":"cutoff","points":[{"beat":0,"value":0.5}]})").text.contains ("no plugin"),
                   "a plugin parameter on a track with no plugin: says so");
            check (tool ("write_automation", "{\"track\":\"" + id + R"(","param":"vol","points":[{"beat":0,"value":"loud"}]})").text.startsWith ("[invalid]"),
                   "a value it can't read refused");
            check (tool ("write_automation", "{\"track\":\"" + s.track (app->focused)[taw::ids::uid].toString() + R"(","param":"vol","points":[{"beat":0,"value":-6}]})")
                       .text.startsWith ("[hands-off]"),
                   "the focused track while the user's at the keys: refused");
            check (! tool ("get_parameters", "{\"track\":\"" + id + "\"}").error, "get_parameters on a track with no plugins: an empty list");
        }

        // Agents never save.
        check (dir.getChildFile ("session.json").loadFileAsString() == sessionJson && dir.getChildFile ("unsaved").getChildFile ("session.json").existsAsFile(),
               "session.json untouched; the edits are in unsaved\\");
        check (app->unsaved, "the session says unsaved");

        // Strangers and rubbish.
        {
            taw::agents::Bridge stranger ({}, "stranger");
            stranger.target (app->agentServer->port(), "not the token");
            auto f = std::async (std::launch::async, [&] { return stranger.call ("get_session", juce::var (new juce::DynamicObject())); });

            while (f.wait_for (std::chrono::milliseconds (1)) != std::future_status::ready)
                pump();

            const auto a = f.get();
            check (! a.ok && a.text.contains ("token"), "the wrong token: " + a.text);
            const auto tracksNow = s.numTracks();

            juce::StreamingSocket raw;

            if (raw.connect ("127.0.0.1", app->agentServer->port(), 1000))
            {
                const char junk[] = "GET / HTTP/1.1\r\n\r\n\xff\xfe nonsense";
                raw.write (junk, (int) sizeof (junk));
            }

            wait (100);
            raw.close();

            struct Raw final : juce::InterprocessConnection
            {
                Raw() : juce::InterprocessConnection (false, 0x74617761) {}
                ~Raw() override { disconnect(); }
                void connectionMade() override {}
                void connectionLost() override {}
                void messageReceived (const juce::MemoryBlock& m) override { got = m.toString(); }
                juce::String got;
            } unwelcome;

            if (unwelcome.connectToSocket ("127.0.0.1", app->agentServer->port(), 1000))
            {
                unwelcome.sendMessage (juce::MemoryBlock ("{not json", 9));
                const juce::String call = R"({"type":"call","id":1,"tool":"remove_track","args":{"track":"1"}})";
                unwelcome.sendMessage (juce::MemoryBlock (call.toRawUTF8(), call.getNumBytesAsUTF8()));
            }

            wait (200);
            check (unwelcome.got.contains ("say hello first") && s.numTracks() == tracksNow, "a call without hello refused");
            unwelcome.disconnect();
            wait (50);
            const auto still = tool ("get_session", "{}");
            check (! still.error && still.data["tracks"].size() == tracksNow, "garbage on the socket: taw carries on");
        }

        // taw going away and coming back (a new port and token): the bridge finds it again.
        {
            app->agentServer.reset();
            const auto none = tool ("get_session", "{}");
            check (none.error && none.text.contains ("[no-session]"), "taw gone: " + none.text);
            app->startAgents();
            pump();
            const auto back = tool ("get_session", "{}");
            check (! back.error, "taw back: the bridge finds it again");
        }

        // A reload keeps every id (and the count past them).
        {
            std::map<juce::String, juce::String> before; // name: id ("Tick" and "tick" both)
            const int next = s.tree()[taw::ids::nextTrackId];

            for (int t = 0; t < s.numTracks(); ++t)
                before[s.track (t)[taw::ids::name].toString()] = s.track (t)[taw::ids::uid].toString();

            app.reset(); // as taw closing on unsaved work: it stays in unsaved\, offered next time
            app = std::make_unique<taw::App> (dir, juce::File(), juce::String(), juce::String());
            app->interactive = true;
            auto& again = *app->session;
            bool same = again.numTracks() == (int) before.size();

            for (int t = 0; t < again.numTracks(); ++t)
                same = same && before[again.track (t)[taw::ids::name].toString()] == again.track (t)[taw::ids::uid].toString();

            check (same && (int) again.tree()[taw::ids::nextTrackId] == next && next > 6,
                   "reopened: the same ids for all " + juce::String (again.numTracks()) + " tracks, the next still t" + juce::String (next));
        }

        // An old session (no ids) gets them on load.
        {
            const auto old = scratch.getChildFile ("old song");
            {
                juce::String error;
                taw::Session::create (old, {}, 44100.0, error);
            }

            auto tree = taw::varToTree (juce::JSON::parse (old.getChildFile ("session.json")));
            tree.removeProperty (taw::ids::nextTrackId, nullptr);

            for (const auto* name : { "Kick", "Snare", "Bass" })
            {
                juce::ValueTree t (taw::ids::TRACK);
                t.setProperty (taw::ids::name, name, nullptr);
                tree.appendChild (t, nullptr);
            }

            old.getChildFile ("session.json").replaceWithText (juce::JSON::toString (taw::treeToVar (tree)));
            juce::String error;
            const auto loaded = taw::Session::load (old, error);
            juce::StringArray got;

            for (int t = 0; loaded != nullptr && t < loaded->numTracks(); ++t)
                got.add (loaded->track (t)[taw::ids::uid].toString());

            check (got.joinIntoString (",") == "t1,t2,t3", "an old session's tracks get ids on load: " + got.joinIntoString (","));
        }

        // Sessions: with one open, switching waits for the user's say; on the session screen it
        // just happens. New ones go in the scratch folder, never the user's sessions folder.
        {
            app->startAgents();
            app->sessionsFolder = app->newSessionParent = scratch;
            pump();

            auto settle = [&]
            {
                for (int i = 0; i < 2000 && (app->closing != nullptr || app->opening != nullptr); ++i)
                {
                    if (app->closing != nullptr)
                        app->stepClosing();
                    else
                        app->stepOpening();

                    app->pumpBackgroundWork();
                }

                pump();
            };
            auto press = [&] (wchar_t c, taw::Key::Code code = taw::Key::Char)
            {
                taw::Key k;
                k.code = code;
                k.ch = c;
                app->pressKey (k);
                app->pumpBackgroundWork();
            };
            const auto old = scratch.getChildFile ("old song");
            const auto quoted = [] (const juce::File& f) { return juce::JSON::toString (f.getFullPathName()); };

            const auto list = tool ("list_sessions", "{}");
            check (! list.error && list.data["open"].toString() == dir.getFullPathName() && list.data["templates"].size() == (int) app->templates.size()
                       && list.data["new_sessions_go_in"].toString() == scratch.getFullPathName(),
                   "list_sessions: the open one, the templates, where new ones go");
            check (tool ("open_session", "{\"session\":" + quoted (dir) + "}").text.contains ("already open"), "open_session on the open one: already open");

            app->engine.play (0);
            check (tool ("new_session", R"({"name":"agent idea"})").text.startsWith ("[playing]") && ! app->sessionAsk.has_value(), "new_session while playing refused");
            app->engine.stop();
            pump();

            check (tool ("new_session", R"({"name":"agent idea","template":"no such"})").text.startsWith ("[invalid]"), "an unknown template refused");

            const auto asked = tool ("new_session", R"({"name":"agent idea"})");
            check (asked.error && asked.text.startsWith ("[needs-user]") && app->sessionAsk.has_value() && ! app->sessionAsk->folder.exists(),
                   "new_session over an open session: taw asks, nothing made yet");
            check (app->screenText (100, 30).contains ("NEW SESSION"), "taw shows the session box");
            std::printf ("%s", app->screenText (100, 14).toRawUTF8());
            check (tool ("new_session", R"({"name":"agent idea"})").text.startsWith ("[needs-user]"), "asking again: still waiting");
            press (0, taw::Key::Escape);
            check (! app->sessionAsk.has_value() && app->session != nullptr && app->session->folder() == dir, "esc: the session stays");
            check (tool ("new_session", R"({"name":"agent idea"})").text.startsWith ("[declined]"), "...and the agent hears no");

            tool ("new_session", R"({"name":"agent tune"})");
            const auto folder = app->sessionAsk.has_value() ? app->sessionAsk->folder : juce::File();
            press (app->hasUnsavedChanges() ? 'd' : 0, app->hasUnsavedChanges() ? taw::Key::Char : taw::Key::Enter);
            settle();
            check (app->session != nullptr && app->session->folder() == folder && folder.getFileName().startsWith ("agent tune ")
                       && folder.getParentDirectory() == scratch,
                   "yes: made (dated, in the sessions folder) and opened: " + folder.getFileName());
            const auto retry = tool ("new_session", R"({"name":"agent tune"})");
            check (! retry.error && retry.text.contains ("is open"), "the agent asks again: " + retry.text);
            check (! tool ("add_track", R"({"kind":"audio","name":"Gtr"})").error, "edits go into the new session");

            // The session screen: no box.
            app->closeSession();
            settle();
            check (app->session == nullptr, "closed: the session screen");
            check (tool ("get_session", "{}").text.startsWith ("[no-session]"), "get_session: no session");
            const auto opened = tool ("open_session", "{\"session\":" + quoted (old) + "}");
            settle();
            check (! opened.error && app->session != nullptr && app->session->folder() == old && ! app->sessionAsk.has_value(),
                   "open_session on the session screen: opened at once");

            app->closeSession();
            settle();
            const auto made = tool ("new_session", R"({"name":"screen idea"})");
            settle();
            check (! made.error && app->session != nullptr && app->session->name().startsWith ("screen idea"), "new_session on the session screen: made and opened at once");
            check (tool ("run_command", R"({"command":"import D:\\x.als"})").text.startsWith ("[invalid]"), "run_command can't import a Live set (it closes the session)");

            // Closed sessions (docs/plan-taw-mcp.md, "Closed sessions"): edited in a background
            // copy inside this taw, into their unsaved\, handed over when the user opens one.
            const auto songA = scratch.getChildFile ("song a");
            const auto songB = scratch.getChildFile ("song b");
            {
                juce::String error;
                taw::Session::create (songA, {}, 44100.0, error);
                taw::Session::create (songB, {}, 44100.0, error);
            }
            const auto bJson = songB.getChildFile ("session.json").loadFileAsString();
            const auto bUnsaved = songB.getChildFile ("unsaved").getChildFile ("session.json");
            auto bTree = [&] { return taw::varToTree (juce::JSON::parse (bUnsaved)); };
            auto trackNames = [] (const juce::ValueTree& t)
            {
                juce::StringArray names;

                for (const auto& c : t)
                    if (c.hasType (taw::ids::TRACK))
                        names.add (c[taw::ids::name].toString());

                return names.joinIntoString (",");
            };
            auto trackNamed = [] (const juce::ValueTree& t, const juce::String& name)
            {
                for (const auto& c : t)
                    if (c.hasType (taw::ids::TRACK) && c[taw::ids::name].toString() == name)
                        return c;

                return juce::ValueTree();
            };
            const juce::String B = R"("session":"song b")";

            {
                const auto toolsNow = method ("tools/list", "{}")["result"]["tools"];
                bool every = true, notListing = true;

                for (const auto& t : *toolsNow.getArray())
                {
                    const bool has = t["inputSchema"]["properties"].hasProperty ("session");
                    const auto name = t["name"].toString();

                    if (name == "list_sessions" || name == "new_session")
                        notListing = notListing && ! has;
                    else if (name != "open_session")
                        every = every && has;
                }

                check (every && notListing, "tools/list: every session tool takes session (list_sessions and new_session don't)");
            }

            // From the session screen.
            app->closeSession();
            settle();
            check (app->session == nullptr, "closed: the session screen again");
            {
                const auto read = tool ("get_session", "{" + B + "}");
                check (! read.error && read.data["name"].toString() == "song b" && ! (bool) read.data["open"] && read.data["tracks"].size() == 0 && read.data["focused"].isVoid(),
                       "get_session on a closed session: read, not open, nobody's cursor in it");
                check (app->session == nullptr && app->backgroundSessions.size() == 1, "...taw still on the session screen, one background copy");
                check (tool ("get_session", R"({"session":"no such song"})").text.startsWith ("[invalid]"), "an unknown session refused");
                const auto partial = tool ("get_session", R"({"session":"song"})");
                check (partial.text.startsWith ("[invalid]") && partial.text.contains ("song a") && partial.text.contains ("song b"),
                       "part of a name refused (it could be the wrong one), the matches offered");
            }

            const auto keys = tool ("add_track", "{" + B + R"(,"kind":"instrument","name":"Keys"})");
            const auto keysId = keys.data["track"].toString();
            check (! keys.error && keysId == "t1", "add_track on it: " + keys.text);
            check (! tool ("write_notes", "{" + B + ",\"track\":\"" + keysId + R"(","beat":0,"notes":[{"beat":0,"length":2,"pitch":60},{"beat":2,"length":2,"pitch":64}]})").error,
                   "write_notes on it");
            check (! tool ("add_marker", "{" + B + R"(,"beat":8,"name":"Verse"})").error, "add_marker on it");
            const auto vol = tool ("run_command", "{" + B + R"(,"command":"1 v -3"})");
            check (! vol.error, "run_command on it: " + vol.text);
            {
                auto* copy = app->backgroundSessions.empty() ? nullptr : app->backgroundSessions.front().app.get();
                check (copy != nullptr && copy->um()->getUndoDescription() == "mcptest: 1 v -3" && copy->um()->canUndo(),
                       "one undo step each, in the copy: " + (copy != nullptr ? copy->um()->getUndoDescription() : juce::String()));
                check (songB.getChildFile ("session.json").loadFileAsString() == bJson, "song b's session.json untouched");
                const auto t = bTree();
                const auto track = trackNamed (t, "Keys");
                check (trackNames (t) == "Keys" && taw::Session::placedNotes (track).size() == 2 && std::abs ((double) track[taw::ids::gainDb] + 3.0) < 0.01
                           && t.getChildWithName (taw::ids::MARKER).isValid(),
                       "its unsaved\\session.json has them: the track, its notes, its volume, the marker");
                check (songB.getChildFile ("unsaved").getChildFile ("agent-edits.json").loadFileAsString().contains ("mcptest"), "...and unsaved\\agent-edits.json says whose");
                const auto back = tool ("get_track", "{" + B + ",\"track\":\"" + keysId + "\"}");
                check (! back.error && back.data["clips"][0]["notes"].size() == 2 && ! back.data.hasProperty ("locked"), "get_track reads them back, nothing locked");
            }

            // Session A open, B edited.
            check (! tool ("open_session", "{\"session\":" + quoted (songA) + "}").error, "open_session song a");
            settle();
            check (app->session != nullptr && app->session->folder() == songA, "song a open");
            {
                const auto aUndo = app->um()->getUndoDescription();
                const auto hat = tool ("add_track", "{" + B + R"(,"kind":"sample","name":"Hat"})");
                const auto hatId = hat.data["track"].toString();
                const auto hits = tool ("write_hits", "{" + B + ",\"track\":\"" + hatId + R"(","beat":0,"hits":[{"beat":0},{"beat":1},{"beat":2},{"beat":3}]})");
                check (! hat.error && ! hits.error && hatId == "t2", "with song a open: a sample track and its hits on song b");
                const auto withFile = tool ("add_track", "{" + B + R"(,"kind":"sample","name":"Tick","sample":)" + quoted (tick) + "}");
                check (! withFile.error && withFile.data["track"].toString() == "t3", "a sample track with a file on song b, answered once its import is in: " + withFile.text);
                check (app->message.contains ("in song b"), "the status line says where: " + app->message);
                check (app->session->numTracks() == 0 && ! app->unsaved && app->um()->getUndoDescription() == aUndo,
                       "song a untouched: no tracks, nothing unsaved, its undo history as it was");
                const auto here = tool ("get_session", "{}");
                check (! here.error && here.data["name"].toString() == "song a" && (bool) here.data["open"], "get_session without session: song a, open");
                check (tool ("get_session", R"({"session":"song a"})").data["name"].toString() == "song a", "...and naming song a: the same");
                const auto t = bTree();
                check (trackNames (t) == "Keys,Hat,Tick" && taw::Session::placedHits (trackNamed (t, "Hat")).size() == 4 && trackNamed (t, "Tick")[taw::ids::sample].toString().isNotEmpty(),
                       "song b's unsaved\\ has Keys, Hat (4 hits) and Tick (its sample): " + trackNames (t));

                app->engine.play (0);
                check (! tool ("add_marker", "{" + B + R"(,"beat":16,"name":"Chorus"})").error, "song a playing: edits to song b still go in (nothing of it can be heard)");
                check (tool ("get_parameters", "{" + B + R"(,"track":"t1"})").text.startsWith ("[playing]"), "...but not one that would load plugins");
                app->engine.stop();
                pump();

                const auto listed = tool ("list_sessions", "{}");
                bool marked = false;

                for (const auto& entry : *listed.data["sessions"].getArray())
                    marked = marked || (entry["name"].toString() == "song b" && (bool) entry["unsaved_changes"]);

                check (marked, "list_sessions: song b has unsaved changes");
            }

            // The tempo on a closed session with audio: waits for the user to open it.
            {
                const auto imported = tool ("run_command", "{" + B + ",\"command\":" + juce::JSON::toString ("import \"" + tick.getFullPathName() + "\"") + "}");
                check (! imported.error, "run_command import on song b (an audio clip): " + imported.text);
                const auto bpmB = (double) bTree()[taw::ids::bpm];
                const auto tempo = tool ("set_tempo", "{" + B + R"(,"bpm":140})");
                check (tempo.text.startsWith ("[needs-user]") && app->pendingTempo.size() == 1 && ! app->tempoAsk.has_value() && juce::approximatelyEqual ((double) bTree()[taw::ids::bpm], bpmB),
                       "set_tempo 140 on song b with audio: waits for the user to open it, nothing changed");
                check (tool ("get_session", "{" + B + "}").data["tempo_request"]["status"].toString().contains ("open"), "get_session: its tempo_request waits for the open");
            }

            // Idle: the copy goes, the edits stay; named again, it's read from unsaved\.
            {
                app->backgroundIdle = 0.0;
                wait (30);
                check (app->backgroundSessions.empty() && ! songB.getChildFile (".taw-lock").exists() && bUnsaved.existsAsFile(),
                       "idle: the copy let go, its lock gone, its edits kept in unsaved\\");
                app->backgroundIdle = 120.0;
                const auto back = tool ("get_session", "{" + B + "}");
                check (! back.error && back.data["tracks"].size() == 4 && (bool) back.data["unsaved"] && songB.getChildFile (".taw-lock").exists(),
                       "named again: a new copy built on unsaved\\ (4 tracks, unsaved, locked again)");
            }

            // The hand-over: the user opens song b while an agent's call on it is under way.
            {
                app->closeSession();
                settle();
                const auto line = "{\"jsonrpc\":\"2.0\",\"id\":" + juce::String (++rpcId) + R"(,"method":"tools/call","params":{"name":"add_track","arguments":{)" + B
                                  + R"(,"kind":"sample","name":"Shaker","sample":)" + quoted (tick) + "}}}";
                auto f = std::async (std::launch::async, [&bridge, line] { return bridge.handle (line); });
                taw::App* copy = nullptr;

                for (int i = 0; i < 20000 && (copy == nullptr || ! app->backgroundBusy (songB)); ++i)
                {
                    app->pollAgents();
                    copy = app->backgroundSessions.empty() ? nullptr : app->backgroundSessions.front().app.get();
                    juce::Thread::sleep (1);
                }

                check (copy != nullptr && app->backgroundBusy (songB), "an agent mid-call on song b (a sample importing)");
                app->startOpening (songB); // the user, on the session screen
                check (app->session == nullptr && app->openWhenHandedOver == songB, "the user opens it: the open waits for the call");

                while (f.wait_for (std::chrono::milliseconds (1)) != std::future_status::ready)
                    pump();

                const auto reply = juce::JSON::parse (f.get());

                for (int i = 0; i < 2000 && app->session == nullptr; ++i)
                    pump();

                settle();
                check (! (bool) reply["result"]["isError"], "the call finishes: " + reply["result"]["content"][0]["text"].toString().upToFirstOccurrenceOf ("\n", false, false));
                check (app->session != nullptr && app->session->folder() == songB && app->backgroundSessions.empty(), "then song b opens: the copy handed over");
            }

            if (app->session != nullptr && app->session->folder() == songB)
            {
                auto& b = *app->session;
                const auto keysTrack = b.track (b.trackById (keysId));
                check (trackNames (b.tree()) == "Keys,Hat,Tick,tick,Shaker" && taw::Session::placedNotes (keysTrack).size() == 2,
                       "with every agent edit in it: " + trackNames (b.tree()));
                check (app->unsaved && app->hasUnsavedChanges() && b.loadNote.contains ("mcptest"), "as unsaved changes, said whose: " + b.loadNote);
                check (app->um()->getUndoDescription().startsWith ("mcptest: add sample track"), "the agent's edits are undo steps here: " + app->um()->getUndoDescription());
                check (app->tempoAsk.has_value() && juce::approximatelyEqual (app->tempoAsk->to, 140.0) && app->pendingTempo.empty(),
                       "the tempo box is up with the agent's 140");
                const auto bpmBefore = b.bpm();
                press ('t');
                check (juce::approximatelyEqual (b.bpm(), 140.0), "t: 140, the audio kept where it is in time");

                app->undoEdit(); // the tempo
                const auto undone = app->undoEdit();
                app->pumpBackgroundWork();
                check (undone.ok && b.numTracks() == 4 && juce::approximatelyEqual (b.bpm(), bpmBefore), "ctrl+z twice: the tempo, then the agent's Shaker: " + undone.message);
                app->redoEdit();
                app->redoEdit();
                app->pumpBackgroundWork();
                check (b.numTracks() == 5 && juce::approximatelyEqual (b.bpm(), 140.0), "ctrl+y twice: back");
                wait (700); // the user's undo and redo were their edits: let them age

                // The agent carries on, naming song b as before: live now, the usual rules.
                app->lastInputAt = juce::Time::getMillisecondCounterHiRes();
                app->focusTrack (b.trackById (keysId));
                const auto locked = tool ("write_notes", "{" + B + ",\"track\":\"" + keysId + R"(","clip":0,"notes":[{"beat":1,"length":1,"pitch":67}]})");
                check (locked.text.startsWith ("[hands-off]") && taw::Session::placedNotes (keysTrack).size() == 2, "the agent carries on with session song b: live, the user's focused track locked");
                app->engine.play (0);
                check (tool ("add_marker", "{" + B + R"(,"beat":24,"name":"Out"})").text.startsWith ("[playing]"), "...refused while it plays");
                app->engine.stop();
                pump();
                const auto hat = b.track (b.trackById ("t2"));
                const auto onHat = tool ("write_hits", "{" + B + R"(,"track":"t2","clip":0,"hits":[{"beat":0.5}]})");
                check (! onHat.error && taw::Session::placedHits (hat).size() == 5 && app->um()->getUndoDescription().startsWith ("mcptest: write"),
                       "...and elsewhere goes in, live, one undo step");
                check (tool ("get_session", "{" + B + "}").data["open"], "get_session naming it: open");
                check (songB.getChildFile ("session.json").loadFileAsString() == bJson, "song b's session.json still untouched");
                const auto saved = app->saveSession();
                const auto onDisk = taw::varToTree (juce::JSON::parse (songB.getChildFile ("session.json")));
                check (saved.ok && trackNames (onDisk) == "Keys,Hat,Tick,tick,Shaker" && ! songB.getChildFile ("unsaved").exists(), "ctrl+s: all of it in session.json, unsaved\\ gone");
            }
        }

        app.reset();
        std::printf ("%s\n", failures == 0 ? "all ok" : "FAILURES");
        return failures;
    }

    // taw --serve <session> [seconds]   the session open headless with its control socket, for
    // trying taw --mcp from a shell (pipe JSON-RPC into it). Edits go to unsaved\, as with a
    // person's taw; nothing is saved. 60 s unless told.
    if (args[0] == "--serve" && args.size() >= 2)
    {
        const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile (args[1]);

        if (! taw::Session::existsIn (dir))
        {
            std::printf ("no session in %s\n", dir.getFullPathName().toRawUTF8());
            return 1;
        }

        auto app = std::make_unique<taw::App> (dir, juce::File(), juce::String(), juce::String());
        app->interactive = true;

        if (app->session == nullptr || ! app->startAgents())
        {
            std::printf ("couldn't serve: %s\n", app->message.toRawUTF8());
            return 1;
        }

        const auto seconds = args.size() >= 3 ? args[2].getIntValue() : 60;
        std::printf ("serving %s on port %d for %d s\n", dir.getFullPathName().toRawUTF8(), app->agentServer->port(), seconds);
        std::fflush (stdout);
        juce::String shown;

        for (const auto until = juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0; juce::Time::getMillisecondCounterHiRes() < until;)
        {
            app->pollAgents();
            app->pumpBackgroundWork();

            if (app->message != shown)
            {
                shown = app->message;
                std::printf ("  %s\n", shown.toRawUTF8());
                std::fflush (stdout);
            }

            juce::Thread::sleep (5);
        }

        return 0;
    }

    // taw --exec <session-dir> "<command>" ...   run command lines against a session without the
    // screen, print each result, save. The first headless way in for scripts and agents.
    if (args[0] == "--exec")
    {
        const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile (args[1]);

        if (! taw::Session::existsIn (dir))
        {
            std::printf ("no session in %s\n", dir.getFullPathName().toRawUTF8());
            return 1;
        }

        auto appOwned = std::make_unique<taw::App> (dir, juce::File(), juce::String(), juce::String()); // see the note at the interactive App
        auto& app = *appOwned;

        if (app.session == nullptr)
        {
            std::printf ("couldn't open: %s\n", app.message.toRawUTF8());
            return 1;
        }

        int failures = 0;

        for (int i = 2; i < args.size(); ++i)
        {
            // ":screen" prints the screen as text (160x44; ":screen 80 24" another size); ":key o",
            // ":key right", ":key ]" press a key.
            if (args[i] == ":screen" || args[i].startsWith (":screen "))
            {
                const auto size = juce::StringArray::fromTokens (args[i].fromFirstOccurrenceOf (" ", false, false), " ", "");
                std::printf ("%s", app.screenText (size.size() == 2 ? size[0].getIntValue() : 160, size.size() == 2 ? size[1].getIntValue() : 44).toRawUTF8());
                continue;
            }

            // ":click 80 12" / ":dblclick 80 12" press the mouse in that cell of the 160x44 screen.
            if (args[i].startsWith (":click ") || args[i].startsWith (":dblclick "))
            {
                app.screenText (160, 44); // the layout the click lands on
                const auto xy = juce::StringArray::fromTokens (args[i].fromFirstOccurrenceOf (" ", false, false), " ", "");
                taw::Key k;
                k.code = taw::Key::Mouse;
                k.x = xy[0].getIntValue();
                k.y = xy[1].getIntValue();
                k.doubleClick = args[i].startsWith (":dbl");
                app.pressKey (k);
                std::printf ("%-29s cursor %s  track %d  selection %s\n", args[i].toRawUTF8(), app.formatBars (app.cursor).toRawUTF8(), app.focused + 1,
                             app.hasTimeSelection() ? (app.formatBars (app.timeSelection().getStart()) + " - " + app.formatBars (app.timeSelection().getEnd())).toRawUTF8() : "none");
                continue;
            }

            // ":sleep 500" waits (timing tests: tap tempo).
            if (args[i].startsWith (":sleep "))
            {
                juce::Thread::sleep (args[i].substring (7).getIntValue());
                continue;
            }

            // ":browser shot.png [900x580]" draws the browser's window into a picture (at its
            // opening size unless one's given).
            if (args[i].startsWith (":browser "))
            {
                auto what = args[i].substring (9).trim();
                int w = 900, h = 580;

                if (const auto size = what.fromLastOccurrenceOf (" ", false, false); size.containsChar ('x') && size.upToFirstOccurrenceOf ("x", false, false).containsOnly ("0123456789"))
                {
                    w = std::max (200, size.upToFirstOccurrenceOf ("x", false, false).getIntValue());
                    h = std::max (200, size.fromFirstOccurrenceOf ("x", false, false).getIntValue());
                    what = what.upToLastOccurrenceOf (" ", false, false).trim();
                }

                const juce::File png (what.unquoted());
                png.deleteFile();
                juce::FileOutputStream out (png);
                juce::PNGImageFormat().writeImageToStream (taw::BrowserWindow::snapshot (app, w, h), out);
                std::printf ("browser drawn to %s\n", png.getFullPathName().toRawUTF8());
                continue;
            }

            // ":bkey right" presses a key in the browser's window, as ":key" does in taw.
            if (args[i].startsWith (":key ") || args[i].startsWith (":bkey "))
            {
                const bool browserKey = args[i].startsWith (":bkey ");
                auto name = args[i].fromFirstOccurrenceOf (" ", false, false);
                taw::Key k;

                // ":key ctrl+right", ":key shift+up": modifiers first.
                for (bool more = true; more;)
                {
                    more = false;

                    if (name.startsWithIgnoreCase ("ctrl+") && name.length() > 5) { k.ctrl = more = true; name = name.substring (5); }
                    if (name.startsWithIgnoreCase ("shift+") && name.length() > 6) { k.shift = more = true; name = name.substring (6); }
                    if (name.startsWithIgnoreCase ("alt+") && name.length() > 4) { k.alt = more = true; name = name.substring (4); }
                }

                const std::pair<const char*, taw::Key::Code> named[] = { { "enter", taw::Key::Enter }, { "esc", taw::Key::Escape }, { "left", taw::Key::Left },
                                                                         { "right", taw::Key::Right }, { "up", taw::Key::Up }, { "down", taw::Key::Down },
                                                                         { "del", taw::Key::Delete }, { "tab", taw::Key::Tab }, { "backspace", taw::Key::Backspace } };
                k.code = taw::Key::Char;
                k.ch = (wchar_t) name[0];

                for (const auto& [n, code] : named)
                    if (name.equalsIgnoreCase (n))
                        k.code = code;

                if (browserKey)
                {
                    if (! app.handleBrowserKey (k))
                        app.pressKey (k);

                    while (app.hasBackgroundWork())
                    {
                        juce::Thread::sleep (10);
                        app.pumpBackgroundWork();
                    }
                }
                else
                    app.pressKey (k);

                std::printf ("%s  %-24s %s\n", browserKey ? "bkey" : "key ", name.toRawUTF8(), app.message.toRawUTF8());
                continue;
            }

            auto r = app.execute (args[i]);

            // Imports and bounces finish on their own threads; wait so results are in order.
            while (app.hasBackgroundWork())
            {
                juce::Thread::sleep (10);
                app.pumpBackgroundWork();
                r = app.messageIsError ? taw::Result::fail (app.message) : taw::Result::good (app.message);
            }

            app.pumpBackgroundWork();
            std::printf ("%s %-24s %s\n", r.ok ? "ok  " : "FAIL", args[i].toRawUTF8(), r.message.toRawUTF8());
            failures += r.ok ? 0 : 1;
        }

        if (app.session == nullptr)
        {
            std::puts ("\n(session closed)");
            return failures;
        }

        // What the session holds now.
        auto& s = *app.session;
        std::printf ("\n%s \xc2\xb7 %d tracks \xc2\xb7 mix %s dB \xc2\xb7 delay compensation %d samples\n", s.name().toRawUTF8(), s.numTracks(),
                     taw::formatDb (s.tree()[taw::ids::mixDb]).toRawUTF8(), app.mixLatency);

        for (int t = 0; t < s.numTracks(); ++t)
        {
            const auto track = s.track (t);
            juce::String what;

            // Sample tracks: their hits by beat. Audio tracks: each clip as start+length in samples.
            if (taw::Session::isInstrumentTrack (track))
            {
                // Each MIDI clip as beat+length, then the notes that play (timeline beats).
                const auto clips = taw::Session::midiClips (track);
                const auto notes = taw::Session::placedNotes (track);
                what << track[taw::ids::pluginName].toString() << ", " << (int) clips.size() << " clips:";

                for (const auto& c : clips)
                    what << " " << juce::String ((double) c[taw::ids::beat], 2) << "+" << juce::String ((double) c[taw::ids::len], 2);

                what << ", " << (int) notes.size() << " notes:";

                for (const auto& n : notes)
                    what << " " << (int) n.node[taw::ids::pitch] << "@" << juce::String (n.start, 2) << "+" << juce::String (n.end - n.start, 2);
            }
            else if (taw::Session::isSampleTrack (track))
            {
                // Each hit clip as beat+length, then the hits that play (timeline beats).
                const auto clips = taw::Session::hitClips (track);
                std::vector<double> beats;
                what << (int) clips.size() << " clips:";

                for (const auto& c : clips)
                    what << " " << juce::String ((double) c[taw::ids::beat], 2) << "+" << juce::String ((double) c[taw::ids::len], 2);

                for (const auto& h : taw::Session::placedHits (track))
                    beats.push_back (h.beat);

                std::sort (beats.begin(), beats.end());
                what << ", " << (int) beats.size() << " hits:";

                for (auto b : beats)
                    what << " " << juce::String (b, 2);
            }
            else
            {
                const auto clips = taw::Session::clips (track);
                what << (int) clips.size() << " clips:";

                for (const auto& c : clips)
                    what << " " << (juce::int64) c[taw::ids::start] << "+" << (juce::int64) c[taw::ids::length];
            }

            if (taw::Session::isBusTrack (track))
                what = "bus";

            // Routing, when it isn't just "to the mix".
            const auto routing = app.describeSends (t).fromFirstOccurrenceOf (" ", false, false).fromFirstOccurrenceOf (" ", false, false)
                                     .trimCharactersAtStart (juce::String (L"\x00b7 "));

            if (! routing.startsWith (juce::String (L"\x2192 Mix \x00b7 no sends")))
                what << " | " << routing;

            std::printf ("  %2d %-16s in%-2d %6s dB  %s\n", t + 1, track[taw::ids::name].toString().toRawUTF8(),
                         (int) track[taw::ids::input], taw::formatDb (track[taw::ids::gainDb]).toRawUTF8(), what.toRawUTF8());
        }

        return failures;
    }

#if TAW_HUB
    // taw --hubtest   the hub with no device: an engine and server in this process, a thread
    // standing in for the interface, and two sessions connected through "taw hub" devices (the
    // socket and shared memory as for real). Session A plays its inputs straight back: what the
    // interface puts out must be what came in, exactly one block later, as A's device reports.
    // Session B stalls now and then: A must never miss a block, and B's misses are counted. Then
    // the hub restarts and both sessions connect again. Uses its own settings folder.
    if (args[0] == "--hubtest")
    {
        const juce::TemporaryFile scratch;
        scratch.getFile().createDirectory();
        setenv ("TAW_CONFIG", scratch.getFile().getFullPathName().toRawUTF8(), 1);

        constexpr int block = 256, channels = 2;
        constexpr double rate = 48000.0;
        taw::hub::Engine engine;
        engine.sampleRate = rate;
        engine.blockSize = block;

        // The interface: input 1 counts samples (exact as floats), input 2 is its negative. It
        // checks each block it gets back against what it sent a block before.
        struct Interface final : juce::Thread
        {
            explicit Interface (taw::hub::Engine& e) : juce::Thread ("hubtest interface"), engine (e) {}

            void run() override
            {
                juce::AudioBuffer<float> in (channels, block), out (channels, block);
                auto next = std::chrono::steady_clock::now();
                juce::int64 sample = 0;

                while (! threadShouldExit())
                {
                    for (int i = 0; i < block; ++i)
                    {
                        in.setSample (0, i, (float) ((sample + i) % 65536));
                        in.setSample (1, i, -(float) ((sample + i) % 65536));
                    }

                    engine.process (in.getArrayOfReadPointers(), channels, out.getArrayOfWritePointers(), channels, block);

                    if (checking)
                    {
                        bool exact = true, silent = true;

                        for (int i = 0; i < block; ++i)
                        {
                            const auto want = (float) ((sample - block + i) % 65536);
                            exact = exact && juce::exactlyEqual (out.getSample (0, i), want) && juce::exactlyEqual (out.getSample (1, i), -want);
                            silent = silent && out.getSample (0, i) == 0.0f;
                        }

                        (exact ? good : silent ? missed : wrong)++;
                    }

                    // A block's time on, as an interface keeps it: woken late, it carries on from
                    // then rather than running blocks back to back to catch up.
                    sample += block;
                    const auto period = std::chrono::microseconds ((int) (1.0e6 * block / rate));
                    next = std::max (next + period, std::chrono::steady_clock::now() + period / 2);
                    std::this_thread::sleep_until (next);
                }
            }

            taw::hub::Engine& engine;
            std::atomic<bool> checking { false };
            std::atomic<int> good { 0 }, missed { 0 }, wrong { 0 };
        } interface (engine);

        struct Echo final : juce::AudioIODeviceCallback // session A: its inputs back out
        {
            void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int n,
                                                   const juce::AudioIODeviceCallbackContext&) override
            {
                for (int c = 0; c < numOut; ++c)
                    if (c < numIn)
                        juce::FloatVectorOperations::copy (out[c], in[c], n);
                    else
                        juce::FloatVectorOperations::clear (out[c], n);
            }

            void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
            void audioDeviceStopped() override {}
            void audioDeviceError (const juce::String&) override { errors++; }
            std::atomic<int> errors { 0 };
        } echo;

        struct Staller final : juce::AudioIODeviceCallback // session B: silent, and every 40th block three blocks late
        {
            void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int numOut, int n,
                                                   const juce::AudioIODeviceCallbackContext&) override
            {
                for (int c = 0; c < numOut; ++c)
                    juce::FloatVectorOperations::clear (out[c], n);

                if (++blocks % 40 == 0)
                    juce::Thread::sleep ((int) (3000.0 * block / rate));
            }

            void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
            void audioDeviceStopped() override {}
            void audioDeviceError (const juce::String&) override { errors++; }
            int blocks = 0;
            std::atomic<int> errors { 0 };
        } staller;

        bool ok = true;
        auto check = [&ok] (bool pass, const juce::String& what)
        {
            std::printf ("%-60s %s\n", what.toRawUTF8(), pass ? "ok" : "FAILED");
            ok = ok && pass;
        };

        auto startHub = [&] (std::unique_ptr<taw::hub::Server>& server)
        {
            server = std::make_unique<taw::hub::Server> (engine);
            taw::hub::HubInfo info;
            info.pid = taw::safety::thisProcess();
            info.port = server->start();
            info.inputs = taw::hub::sessionInputs (channels);
            info.outputs = channels;
            info.blockSize = block;
            info.sampleRate = rate;
            server->setHardware (channels, channels, "hubtest");
            taw::hub::infoFile().replaceWithText (juce::JSON::toString (info.toVar()));
            return info.port > 0;
        };

        auto type = taw::hub::makeDeviceType();
        std::unique_ptr<juce::AudioIODevice> a (type->createDevice ({}, {})), b (type->createDevice ({}, {}));
        auto openBoth = [&]
        {
            juce::BigInteger all;
            all.setRange (0, channels, true);
            taw::hub::setClientName ("A", "A", false); // two sessions in one process: naming B mustn't rename A
            const auto ea = a->open (all, all, rate, block);
            taw::hub::setClientName ("B", "B", false);
            const auto eb = b->open (all, all, rate, block);

            if (ea.isNotEmpty() || eb.isNotEmpty())
                std::printf ("open: %s %s\n", ea.toRawUTF8(), eb.toRawUTF8());

            a->start (&echo);
            b->start (&staller);
            return ea.isEmpty() && eb.isEmpty();
        };

        auto measure = [&] (double seconds)
        {
            juce::Thread::sleep (300); // settling: the first blocks after joining
            interface.good = interface.missed = interface.wrong = 0;
            interface.checking = true;
            juce::Thread::sleep ((int) (seconds * 1000.0));
            interface.checking = false;
        };

        std::unique_ptr<taw::hub::Server> server;
        interface.startRealtimeThread (juce::Thread::RealtimeOptions().withApproximateAudioProcessingTime (block, rate));
        check (startHub (server), "the hub listens");
        check (openBoth(), "two sessions connect");
        check (a->getOutputLatencyInSamples() == block && a->getInputLatencyInSamples() == 0,
               "A's device reports a block of output latency (" + juce::String (a->getOutputLatencyInSamples()) + ")");
        check (a->getActiveInputChannels().countNumberOfSetBits() == taw::hub::sessionInputs (channels) && juce::exactlyEqual (a->getCurrentSampleRate(), rate),
               "A runs at the hub's rate, with " + juce::String (a->getActiveInputChannels().countNumberOfSetBits()) + " inputs");

        measure (3.0);
        const auto lateA = a->getXRunCount(), lateB = b->getXRunCount();
        check (interface.wrong == 0 && interface.missed == 0 && interface.good > 400,
               "A's blocks come back exact, a block late: " + juce::String (interface.good.load()) + " good, "
                   + juce::String (interface.missed.load()) + " missed, " + juce::String (interface.wrong.load()) + " wrong");
        check (lateA == 0, "A missed no blocks (" + juce::String (lateA) + ")");
        check (lateB > 0, "B's stalls were counted (" + juce::String (lateB) + " blocks late)");

        // Routes, as taw's hub command asks for them: A off the interface, then back on.
        auto ask = [] (const juce::String& requestType, const juce::String& from, int fromChannel, const juce::String& to, int toChannel)
        {
            auto* m = new juce::DynamicObject();
            m->setProperty ("type", requestType);
            m->setProperty ("from", from);
            m->setProperty ("fromChannel", fromChannel);
            m->setProperty ("to", to);
            m->setProperty ("toChannel", toChannel);
            juce::String error;
            const auto answer = taw::hub::request (m, error);
            return error.isNotEmpty() ? -1 : answer["routes"].size();
        };

        check (ask ("unroute", "A", 0, taw::hub::interfaceKey, 0) == 6, "unroute A from the interface (6 routes left)");
        measure (0.5);
        check (interface.good == 0 && interface.wrong == 0, "the interface hears nothing of A");
        ask ("route", "A", 1, taw::hub::interfaceKey, 1);
        check (ask ("route", "A", 2, taw::hub::interfaceKey, 2) == 8, "route A to the interface again (8 routes)");
        measure (0.5);
        check (interface.wrong == 0 && interface.missed == 0 && interface.good > 50, "A is heard again, exact");

        // The hub restarts: both sessions hear it went, and connect again by themselves.
        server->stopAll();
        juce::Thread::sleep (300);
        check (echo.errors > 0 && staller.errors > 0, "both sessions heard the hub stop");
        server.reset();
        check (startHub (server), "the hub listens again");
        juce::Thread::sleep (1200); // they look every half second
        check (server->clients().size() == 2, "both sessions came back by themselves");
        measure (1.0);
        check (interface.wrong == 0 && interface.missed == 0 && interface.good > 100,
               "A is exact again: " + juce::String (interface.good.load()) + " good, " + juce::String (interface.missed.load()) + " missed");

        a->close();
        b->close();
        server.reset();
        interface.stopThread (2000);
        std::puts (ok ? "ok" : "FAILED");
        return ok ? 0 : 1;
    }
#endif

    // taw --counttest   count-in without a device: the clicks land on the beats before the start,
    // the playhead holds until they're done, and a recording starts exactly at the start.
    if (args[0] == "--counttest")
    {
        constexpr double rate = 48000.0;
        constexpr int block = 256;
        int failures = 0;

        // From a bar line, and from mid-beat: 4 clicks either way.
        for (const juce::int64 from : { (juce::int64) 96000, (juce::int64) 100000 })
        {
            auto engineOwned = std::make_unique<taw::AudioEngine>();
            auto& engine = *engineOwned;
            engine.runWithoutDevice (rate, block);

            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("taw-counttest.wav");
            juce::String err;
            auto rec = engine.createRecorder (0, 0, 1, file, rate, err);

            auto g = std::make_unique<taw::Graph>();
            g->click.samplesPerBeat = rate / 2.0; // 120 bpm
            g->click.beatsPerBar = 4;
            g->click.enabled = false; // the count-in clicks anyway
            g->recording = true;
            g->tracks.push_back ({});
            g->tracks[0].inputChannel = 0;
            g->tracks[0].recorder = rec;
            engine.setGraph (std::move (g));

            const auto spb = rate / 2.0;
            const auto firstBeat = std::ceil ((double) from / spb - 1e-9) - 4;
            const auto countIn = from - (juce::int64) std::llround (firstBeat * spb);
            engine.play (from, countIn);

            std::vector<float> l (block), r (block);
            float* outs[] = { l.data(), r.data() };
            std::vector<juce::int64> onsets;
            juce::int64 heldUntil = -1;
            juce::int64 lastSound = -100000;

            for (juce::int64 at = 0; at < countIn + (juce::int64) rate; at += block)
            {
                engine.renderWithoutDevice (outs, 2, block);

                // A click starts after silence (its sine crosses zero all the time inside).
                for (int i = 0; i < block; ++i)
                    if (std::abs (l[(size_t) i]) > 1.0e-4f)
                    {
                        if (at + i - lastSound > 1000)
                            onsets.push_back (at + i);

                        lastSound = at + i;
                    }

                if (heldUntil < 0 && engine.playhead() != from)
                    heldUntil = at;
            }

            engine.stop();
            const auto started = rec->startSample.load();

            juce::StringArray clicks;
            for (auto o : onsets)
                clicks.add (juce::String (o));

            // Clicks every beat from the start of the count-in on the grid; the playhead moves in the
            // block holding countIn; the recording starts at from.
            bool ok = onsets.size() == 4 && started == from && heldUntil >= 0 && heldUntil <= countIn && heldUntil + block > countIn;

            for (size_t k = 0; ok && k < onsets.size(); ++k)
                ok = std::abs (onsets[k] - ((juce::int64) std::llround ((firstBeat + (double) k) * spb) - (from - countIn))) <= 1; // sin (0) is silent

            std::printf ("%s from %lld: count-in %lld samples, clicks at %s, playhead moves at block %lld, take starts at %lld\n",
                         ok ? "ok  " : "FAIL", (long long) from, (long long) countIn, clicks.joinIntoString (" ").toRawUTF8(), (long long) heldUntil,
                         (long long) started);
            failures += ok ? 0 : 1;
            engine.setGraph (std::make_unique<taw::Graph>());
            engine.finishRecorders ({ rec });
            engine.poll();
            file.deleteFile();
        }

        return failures;
    }

    // taw --livetest   delay compensation and live inputs, without a device: a track with a
    // compensation delay on its output plays its clips that late, but its monitored input
    // goes straight through.
    if (args[0] == "--livetest")
    {
        constexpr double rate = 48000.0;
        constexpr int block = 64, delay = 100, at = 10;
        int failures = 0;

        auto impulse = std::make_shared<juce::AudioBuffer<float>> (1, 1000);
        impulse->clear();
        impulse->setSample (0, at, 1.0f);

        for (const bool monitoring : { true, false })
        {
            auto engineOwned = std::make_unique<taw::AudioEngine>();
            auto& engine = *engineOwned;
            engine.runWithoutDevice (rate, block, 1);

            auto g = std::make_unique<taw::Graph>();
            g->tracks.push_back ({});
            auto& tp = g->tracks[0];
            tp.inputChannel = 0;
            tp.armed = monitoring; // armed while stopped: you hear the input
            tp.clips.push_back ({ impulse, 0, 0, 1000 });
            tp.outDelay = std::make_shared<taw::RouteDelay> (delay);
            engine.setGraph (std::move (g));

            if (! monitoring)
                engine.play (0);

            std::vector<float> l (block), r (block), input (block);
            float* outs[] = { l.data(), r.data() };
            const float* ins[] = { input.data() };
            int heard = -1;

            for (int pos = 0; pos < 1000 && heard < 0; pos += block)
            {
                std::fill (input.begin(), input.end(), 0.0f);

                if (monitoring && at >= pos && at < pos + block)
                    input[(size_t) (at - pos)] = 1.0f;

                engine.renderWithoutDevice (outs, 2, block, ins, 1);

                for (int i = 0; i < block && heard < 0; ++i)
                    if (std::abs (l[(size_t) i]) > 0.1f)
                        heard = pos + i;
            }

            const auto want = monitoring ? at : at + delay;
            std::printf ("%s %s: heard at %d, want %d\n", heard == want ? "ok  " : "FAIL",
                         monitoring ? "monitored input skips the delay" : "clips wait for it", heard, want);
            failures += heard == want ? 0 : 1;
            engine.stop();
            engine.setGraph (std::make_unique<taw::Graph>());
        }

        // Kits: a controller note plays the member on it, stopped; the next pad in its choke
        // group cuts it off (a 4-sample fade), so only the new one is left.
        {
            auto engineOwned = std::make_unique<taw::AudioEngine>();
            auto& engine = *engineOwned;
            engine.runWithoutDevice (rate, block, 0);
            auto ones = std::make_shared<juce::AudioBuffer<float>> (1, 4000);
            juce::FloatVectorOperations::fill (ones->getWritePointer (0), 1.0f, 4000);

            auto g = std::make_unique<taw::Graph>();

            for (int note : { 36, 37 })
            {
                taw::TrackPlay tp;
                tp.sample = ones;
                tp.sampleLength = 4000;
                tp.padNote = note;
                tp.chokeGroup = 1;
                tp.chokeFade = 4;
                g->tracks.push_back (tp);
            }

            engine.setGraph (std::move (g));
            std::vector<float> l (block), r (block);
            float* outs[] = { l.data(), r.data() };
            auto blockPeak = [&]
            {
                engine.renderWithoutDevice (outs, 2, block);
                return l[(size_t) block - 1];
            };

            blockPeak(); // the graph goes live
            engine.playTyped (taw::MidiMsg::noteOn (0, 36, 127));
            const auto one = blockPeak();
            engine.playTyped (taw::MidiMsg::noteOn (0, 37, 127));
            const auto choked = blockPeak();
            const bool ok = std::abs (one - 1.0f) < 1e-4f && std::abs (choked - 1.0f) < 1e-4f;
            std::printf ("%s kit: note 36 plays its pad (%.3f), 37 chokes it (%.3f, want 1)\n", ok ? "ok  " : "FAIL", one, choked);
            failures += ok ? 0 : 1;
            engine.setGraph (std::make_unique<taw::Graph>());
        }

        return failures;
    }

    // taw --cliptest [session-dir]   check clip rendering rules; optionally load (and migrate) a
    //                                session and list its clips. Never saves.
    if (args[0] == "--cliptest")
    {
        auto filled = [] (int len, float v) { auto b = std::make_shared<juce::AudioBuffer<float>> (1, len); for (int i = 0; i < len; ++i) b->setSample (0, i, v + (float) i * 0.001f); return b; };

        taw::TrackPlay tp;
        tp.clips.push_back ({ filled (100, 1.0f), 0, 0, 100 });  // 1.000.. over [0, 100)
        tp.clips.push_back ({ filled (50, 2.0f), 50, 10, 30 });  // later clip: file[10..40) over [50, 80)
        tp.clips.push_back ({ filled (20, 3.0f), 150, 5, 3 });   // file[5..8) over [150, 153)

        std::vector<float> l (200, 0.0f), r (200, 0.0f);
        taw::renderClips (tp, 0, 200, l.data(), r.data());

        auto expect = [&] (int i, float v, const char* what)
        {
            const bool ok = std::abs (l[(size_t) i] - v) < 1e-5f && std::abs (r[(size_t) i] - v) < 1e-5f;
            std::printf ("%-44s sample %3d = %.3f (want %.3f) %s\n", what, i, l[(size_t) i], v, ok ? "ok" : "FAILED");
            return ok ? 0 : 1;
        };

        int failures = 0;
        failures += expect (0, 1.000f, "first clip starts");
        failures += expect (49, 1.049f, "first clip just before overlap");
        failures += expect (50, 2.010f, "later clip wins, from its offset");
        failures += expect (79, 2.039f, "later clip's last sample");
        failures += expect (80, 1.080f, "first clip uncovered again");
        failures += expect (120, 0.000f, "gap is silent");
        failures += expect (150, 3.005f, "offset clip");
        failures += expect (153, 0.000f, "clip length respected");

        // Render in odd-sized blocks: a block boundary must not change anything.
        std::vector<float> l2 (200, 0.0f), r2 (200, 0.0f);
        for (int pos = 0; pos < 200; pos += 7)
            taw::renderClips (tp, pos, std::min (7, 200 - pos), l2.data() + pos, r2.data() + pos);
        const bool same = l == l2;
        std::printf ("%-44s %s\n", "block size doesn't matter", same ? "ok" : "FAILED");
        failures += same ? 0 : 1;

        // Fades: a constant 1.0 clip, so what comes out is the curve itself.
        {
            auto ones = [] (int len) { auto b = std::make_shared<juce::AudioBuffer<float>> (1, len); for (int i = 0; i < len; ++i) b->setSample (0, i, 1.0f); return b; };
            auto twos = [] (int len) { auto b = std::make_shared<juce::AudioBuffer<float>> (1, len); for (int i = 0; i < len; ++i) b->setSample (0, i, 2.0f); return b; };
            std::vector<float> fl, fr;

            auto render = [&] (const taw::TrackPlay& t, int len, int block)
            {
                fl.assign ((size_t) len, 0.0f);
                fr.assign ((size_t) len, 0.0f);
                for (int pos = 0; pos < len; pos += block)
                    taw::renderClips (t, pos, std::min (block, len - pos), fl.data() + pos, fr.data() + pos);
            };

            auto expectF = [&] (int i, float v, const char* what)
            {
                const bool ok = std::abs (fl[(size_t) i] - v) < 1e-4f && std::abs (fr[(size_t) i] - v) < 1e-4f;
                std::printf ("%-44s sample %3d = %.4f (want %.4f) %s\n", what, i, fl[(size_t) i], v, ok ? "ok" : "FAILED");
                failures += ok ? 0 : 1;
            };

            // Fade in over the first 10 samples, out over the last 20, over silence.
            taw::TrackPlay f;
            taw::ClipPlay c { ones (100), 0, 0, 100 };
            c.fadeIn = 10;
            c.fadeOut = 20;
            c.fadeInShape = taw::FadeShape::linear;
            c.fadeOutShape = taw::FadeShape::linear;
            f.clips.push_back (c);
            render (f, 100, 100);
            expectF (0, 0.0f, "fade in starts silent");
            expectF (5, 0.5f, "linear fade in, halfway");
            expectF (10, 1.0f, "after the fade in: the clip");
            expectF (79, 1.0f, "just before the fade out");
            expectF (89, 10.0f / 20.0f, "linear fade out, halfway");
            expectF (99, 0.0f, "fade out ends silent");
            const auto whole = fl;
            render (f, 100, 7);
            std::printf ("%-44s %s\n", "fades don't care about block size", fl == whole ? "ok" : "FAILED");
            failures += fl == whole ? 0 : 1;

            // Fades longer than the clip fit inside it, the fade in first.
            f.clips[0].fadeIn = 80;
            f.clips[0].fadeOut = 80;
            render (f, 100, 100);
            expectF (40, 0.5f, "an 80-sample fade in on a 100-sample clip");
            expectF (89, 10.0f / 20.0f, "...leaves the fade out 20 samples");

            // A crossfade: a later clip of 2.0 over the 1.0 one from 40, fading in for 20.
            taw::TrackPlay x;
            x.clips.push_back ({ ones (100), 0, 0, 100 });
            taw::ClipPlay over { twos (60), 40, 0, 60 };
            over.fadeIn = 20;
            x.clips.push_back (over);
            render (x, 100, 100);
            const float mid = std::sin (juce::MathConstants<float>::halfPi * 0.5f);
            expectF (39, 1.0f, "before the crossfade: the earlier clip");
            expectF (40, 1.0f, "crossfade starts on the earlier clip");
            expectF (50, 1.0f * mid + 2.0f * mid, "equal power: both at -3 dB halfway");
            expectF (60, 2.0f, "after the crossfade: the later clip");

            // Each shape runs 0 to 1, and backwards under the other clip.
            for (int s = 0; s < 4; ++s)
            {
                const auto shape = (taw::FadeShape) s;
                const bool ok = std::abs (taw::fadeCurve (shape, 0.0f)) < 1e-6f && std::abs (taw::fadeCurve (shape, 1.0f) - 1.0f) < 1e-6f
                                && taw::fadeShapeFromName (taw::fadeShapeName (shape)) == shape;
                std::printf ("%-44s %s\n", (juce::String ("shape ") + taw::fadeShapeName (shape) + " runs 0 to 1, named").toRawUTF8(), ok ? "ok" : "FAILED");
                failures += ok ? 0 : 1;
            }
        }

        // Hits: a 10-sample sample (1.000, 1.001, ...) at 0 (vel 1) and 5 (vel 0.5) overlap and sum.
        {
            taw::TrackPlay sp;
            sp.sample = filled (10, 1.0f);
            sp.sampleLength = 10;
            sp.hits = { { 0, 1.0f }, { 5, 0.5f } };
            std::vector<float> hl (20, 0.0f), hr (20, 0.0f);
            // Render 7..20 on its own first: hit 0 must still ring into a later block.
            taw::renderHits (sp, 0, 7, hl.data(), hr.data());
            taw::renderHits (sp, 7, 13, hl.data() + 7, hr.data() + 7);

            auto expectHit = [&] (int i, float v, const char* what)
            {
                const bool ok = std::abs (hl[(size_t) i] - v) < 1e-5f;
                std::printf ("%-44s sample %3d = %.4f (want %.4f) %s\n", what, i, hl[(size_t) i], v, ok ? "ok" : "FAILED");
                return ok ? 0 : 1;
            };

            failures += expectHit (0, 1.000f, "first hit");
            failures += expectHit (4, 1.004f, "first hit alone");
            failures += expectHit (5, 1.005f + 0.5f * 1.000f, "second hit rings over the first");
            failures += expectHit (9, 1.009f + 0.5f * 1.004f, "both, across a block boundary");
            failures += expectHit (10, 0.5f * 1.005f, "first ended, second rings on");
            failures += expectHit (15, 0.0f, "both ended");
        }

        // Choke: an open hat (track 0, a 40-sample 1.0) cut by a closed hat (track 1) at 10, with
        // a 4-sample fade; a hit on its own track doesn't cut it, nor does one at the same spot.
        {
            auto ones = [] (int len) { auto b = std::make_shared<juce::AudioBuffer<float>> (1, len); for (int i = 0; i < len; ++i) b->setSample (0, i, 1.0f); return b; };
            std::vector<taw::TrackPlay> kit (2);

            for (auto& t : kit)
            {
                t.sample = ones (40);
                t.sampleLength = 40;
                t.chokeGroup = 1;
                t.chokeFade = 4;
            }

            kit[0].hits = { { 0, 1.0f }, { 5, 1.0f } };
            kit[1].hits = { { 0, 1.0f }, { 10, 1.0f } };
            taw::chokeHits (kit);
            const bool cutRight = kit[0].hits[0].cut == 10 && kit[0].hits[1].cut == 10 && kit[1].hits[0].cut == 5 && kit[1].hits[1].cut == taw::TrackPlay::never;
            std::printf ("%-44s %s\n", "choke: cut by the next hit of another track", cutRight ? "ok" : "FAILED");
            failures += cutRight ? 0 : 1;

            std::vector<float> cl (60, 0.0f), cr (60, 0.0f);

            for (int pos = 0; pos < 60; pos += 7)
                taw::renderHits (kit[0], pos, std::min (7, 60 - pos), cl.data() + pos, cr.data() + pos);

            auto expectC = [&] (int i, float v, const char* what)
            {
                const bool ok = std::abs (cl[(size_t) i] - v) < 1e-5f;
                std::printf ("%-44s sample %3d = %.4f (want %.4f) %s\n", what, i, cl[(size_t) i], v, ok ? "ok" : "FAILED");
                failures += ok ? 0 : 1;
            };

            expectC (9, 2.0f, "choke: both hits whole before the cut");
            expectC (12, 2.0f * 0.5f, "...halfway down the fade");
            expectC (14, 0.0f, "...silent after it");
        }

        // Monitoring: auto input (armed: input when stopped or recording, clips on playback);
        // "mon" keeps the input during playback; unarmed never monitors.
        {
            taw::TrackPlay m;
            auto check = [&] (const char* what, bool rolling, bool want)
            {
                const bool got = m.monitoringInput (rolling);
                std::printf ("%-44s %s %s\n", what, got ? "input" : "clips", got == want ? "ok" : "FAILED");
                failures += got == want ? 0 : 1;
            };

            check ("unarmed, stopped", false, false);
            check ("unarmed, playing", true, false);
            m.armed = true;
            check ("armed, stopped (setting levels)", false, true);
            check ("armed, playing back", true, false);
            m.recorder = std::make_shared<taw::Recorder>();
            check ("armed, recording", true, true);
            m.recorder.reset();
            m.inputOnly = true;
            check ("armed + mon, playing back (audition)", true, true);
        }

        // MIDI clips: a window onto their content, notes cut off at the clip's end.
        {
            using taw::Session;
            namespace ids = taw::ids;
            auto check = [&] (const char* what, bool ok)
            {
                std::printf ("%-44s %s\n", what, ok ? "ok" : "FAILED");
                failures += ok ? 0 : 1;
            };
            auto approx = [] (double a, double b) { return std::abs (a - b) < 1e-9; };

            juce::ValueTree track (ids::TRACK);
            track.setProperty (ids::kind, "instrument", nullptr);
            auto clip = Session::makeMidiClip (8.0, 2.0, 4.0); // timeline 8..12 shows content 2..6
            clip.appendChild (Session::makeNote (1.0, 1.0, 60, 100), nullptr); // before the window: hidden
            clip.appendChild (Session::makeNote (2.0, 1.0, 62, 100), nullptr); // on its first beat
            clip.appendChild (Session::makeNote (5.0, 3.0, 64, 100), nullptr); // runs past the end
            clip.appendChild (Session::makeNote (6.0, 1.0, 65, 100), nullptr); // at the end: hidden
            clip.appendChild (Session::makeControl (3.0, 1, 64), nullptr);
            track.appendChild (clip, nullptr);

            const auto placed = Session::placedNotes (track);
            check ("MIDI clip plays the notes in its window", placed.size() == 2);
            check ("a note lands at clip start + (beat - offset)", placed.size() == 2 && approx (placed[0].start, 8.0));
            check ("a note is cut off at the clip's end", placed.size() == 2 && approx (placed[1].start, 11.0) && approx (placed[1].end, 12.0));
            check ("controllers in the window play", Session::placedControls (track).size() == 1
                                                         && approx (Session::placedControls (track)[0].at, 9.0));

            const auto seq = taw::buildSequence (track, 100.0);
            check ("the sequence has them in samples", seq.notes.size() == 2 && seq.notes[1].start == 1100 && seq.notes[1].end == 1200);

            // Trimmed to 10..20: just the note at 11, content kept where it was.
            const auto piece = Session::trimmedMidiClip (clip, 10.0, 20.0);
            check ("a trimmed copy is cut to the range", piece.isValid() && approx (piece[ids::beat], 10.0) && approx (piece[ids::len], 2.0)
                                                             && approx (Session::contentZero (piece), Session::contentZero (clip)));
            check ("...and keeps only the notes it shows", piece.getNumChildren() == 1);
            check ("no copy when the range misses", ! Session::trimmedMidiClip (clip, 12.0, 14.0).isValid());

            // Grown back to 7: content doesn't move, so the hidden note at content 1 shows.
            auto grown = clip.createCopy();
            Session::growMidiClip (grown, 7.0, 7.0, nullptr);
            juce::ValueTree t2 (ids::TRACK);
            t2.appendChild (grown, nullptr);
            check ("growing a clip keeps its content in place", approx (grown[ids::beat], 7.0) && approx (grown[ids::len], 5.0)
                                                                    && Session::placedNotes (t2).size() == 3 && approx (Session::placedNotes (t2)[0].start, 7.0));

            // Version 2: notes on the track go into one clip, out to whole bars.
            juce::ValueTree v2 (ids::SESSION);
            v2.setProperty (ids::version, 2, nullptr).setProperty (ids::timeSigNum, 4, nullptr);
            juce::ValueTree old (ids::TRACK);
            old.setProperty (ids::kind, "instrument", nullptr);
            old.appendChild (Session::makeNote (5.5, 1.0, 60, 90), nullptr);
            old.appendChild (Session::makeNote (9.0, 3.5, 62, 90), nullptr);
            v2.appendChild (old, nullptr);
            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("taw-cliptest", "", false);
            dir.createDirectory();
            dir.getChildFile ("session.json").replaceWithText (juce::JSON::toString (taw::treeToVar (v2)));
            juce::String error;

            if (auto s = Session::load (dir, error))
            {
                const auto t = s->track (0);
                const auto clips = Session::midiClips (t);
                const auto notes = Session::placedNotes (t);
                check ("v2 notes move into one MIDI clip", clips.size() == 1 && t.getNumChildren() == 1 && notes.size() == 2);
                check ("...covering whole bars (beats 4 to 16)", clips.size() == 1 && approx (clips[0][ids::beat], 4.0) && approx (clips[0][ids::len], 12.0));
                check ("...with every note where it was", notes.size() == 2 && approx (notes[0].start, 5.5) && approx (notes[1].start, 9.0)
                                                              && approx (notes[1].end, 12.5));
                check ("...and the session is up to date", (int) s->tree()[ids::version] == Session::currentVersion);
            }
            else
            {
                check ("v2 session loads", false);
            }

            // Version 3: a sample track's hits go into one hit clip, out to the bar of the last.
            juce::ValueTree v3 (ids::SESSION);
            v3.setProperty (ids::version, 3, nullptr).setProperty (ids::timeSigNum, 4, nullptr);
            juce::ValueTree drums (ids::TRACK);
            drums.setProperty (ids::kind, "sample", nullptr);
            drums.appendChild (Session::makeHit (4.0, 1.0f), nullptr);
            drums.appendChild (Session::makeHit (10.5, 0.5f), nullptr);
            drums.appendChild (Session::makeHit (12.0, 0.8f), nullptr); // starts bar 4: the clip takes it in
            v3.appendChild (drums, nullptr);
            dir.getChildFile ("session.json").replaceWithText (juce::JSON::toString (taw::treeToVar (v3)));

            if (auto s = Session::load (dir, error))
            {
                const auto t = s->track (0);
                const auto clips = Session::hitClips (t);
                const auto hits = Session::placedHits (t);
                check ("v3 hits move into one hit clip", clips.size() == 1 && t.getNumChildren() == 1 && hits.size() == 3);
                check ("...covering whole bars (beats 4 to 16)", clips.size() == 1 && approx (clips[0][ids::beat], 4.0) && approx (clips[0][ids::len], 12.0));
                check ("...with every hit where it was", hits.size() == 3 && approx (hits[1].beat, 10.5) && std::abs (hits[1].vel - 0.5f) < 1e-6f);

                // Trimming a hit clip keeps its kind and only the hits inside.
                const auto hitPiece = Session::trimmedMidiClip (clips[0], 8.0, 12.0);
                check ("a trimmed hit clip is a hit clip with its hits", hitPiece.hasType (ids::HITCLIP) && hitPiece.getNumChildren() == 1);
            }
            else
            {
                check ("v3 session loads", false);
            }

            dir.deleteRecursively();
        }

        if (args[1].isNotEmpty())
        {
            juce::String error;

            if (auto s = taw::Session::load (juce::File::getCurrentWorkingDirectory().getChildFile (args[1]), error))
            {
                std::printf ("\n%s (version %d)\n", s->name().toRawUTF8(), (int) s->tree()[taw::ids::version]);

                for (int t = 0; t < s->numTracks(); ++t)
                    for (const auto& c : taw::Session::clips (s->track (t)))
                        std::printf ("  track %d  clip %s  start %lld  offset %lld  length %lld\n", t + 1, c[taw::ids::file].toString().toRawUTF8(),
                                     (long long) (juce::int64) c[taw::ids::start], (long long) (juce::int64) c[taw::ids::offset],
                                     (long long) (juce::int64) c[taw::ids::length]);
            }
            else
            {
                std::printf ("load failed: %s\n", error.toRawUTF8());
                ++failures;
            }
        }

        return failures;
    }

    // taw --keycasttest   check what the keycast window shows for keys: names, typing as
    // words, repeats counted, fading, and the colours its command takes.
    if (args[0] == "--keycasttest")
    {
        using taw::Key;
        auto key = [] (Key::Code code, wchar_t ch = 0, bool ctrl = false, bool alt = false, bool shift = false)
        {
            Key k;
            k.code = code;
            k.ch = ch;
            k.ctrl = ctrl;
            k.alt = alt;
            k.shift = shift;
            return k;
        };

        int failures = 0;
        auto check = [&failures] (const char* what, const juce::String& got, const juce::String& want)
        {
            const bool ok = got == want;
            std::printf ("%-28s %-28s %s\n", what, got.toRawUTF8(), ok ? "ok" : "FAILED");
            failures += ok ? 0 : 1;
        };

#if JUCE_MAC
        const juce::String alt ("Opt+");
#else
        const juce::String alt ("Alt+");
#endif
        check ("ctrl+space", taw::KeyCastLog::name (key (Key::Char, L' ', true)), "Ctrl+Space");
        check ("R (shift+r)", taw::KeyCastLog::name (key (Key::Char, L'R')), "Shift+R");
        check ("ctrl+k", taw::KeyCastLog::name (key (Key::Char, L'k', true)), "Ctrl+K");
        check ("{ with shift", taw::KeyCastLog::name (key (Key::Char, L'{', false, false, true)), "{");
        check ("alt+left", taw::KeyCastLog::name (key (Key::Left, 0, false, true)), alt + juce::String (L"\x2190"));
        check ("ctrl+shift+left", taw::KeyCastLog::name (key (Key::Left, 0, true, false, true)), juce::String (L"Ctrl+Shift+\x2190"));
        check ("enter", taw::KeyCastLog::name (key (Key::Enter)), "Enter");

        auto shown = [] (const taw::KeyCastLog& log)
        {
            juce::StringArray out;

            for (const auto& e : log.recent())
                out.add (e.shown());

            return out.joinIntoString (" | ");
        };

        // A hotkey, a typed command (its first key starts the line), enter, then a held arrow.
        {
            taw::KeyCastLog log;
            double t = 0.0;
            log.press (key (Key::Char, L'r'), false, t += 100);
            for (auto c : juce::String ("2v-6"))
                log.press (key (Key::Char, (wchar_t) c), true, t += 100);
            log.press (key (Key::Enter), false, t += 100);
            for (int i = 0; i < 6; ++i)
                log.press (key (Key::Right), false, t += 50);
            log.press (key (Key::Char, L'r'), false, t += 100);
            log.press (key (Key::Char, L'r'), false, t += 2000); // too late to count up
            log.press (key (Key::Mouse), false, t += 100);       // never shown
            check ("sequence", shown (log), juce::String (L"R | 2v-6 | Enter | \x2192 \x00d7") + "6 | R | R");

            // Each key fades from its own last press: the older ones go first.
            const auto last = t - 100.0; // the last R (the click wasn't one)
            log.expire (2300.0, 2000.0); // the first key (at 100) only 2.2 s old
            check ("none faded yet", juce::String ((int) log.recent().size()), "6");
            check ("fade 0: they stay", juce::String ((int) log.recent().size()), log.expire (last + 1.0e6, 0.0) ? "6" : "0");
            log.expire (last + 1000.0, 2000.0);
            check ("the older ones gone", shown (log), "R");
            log.expire (last + 2000.0 + taw::KeyCastLog::fadeOutMs + 1.0, 2000.0);
            check ("all gone", juce::String ((int) log.recent().size()), "0");
        }

        // Only the latest few are kept.
        {
            taw::KeyCastLog log;
            for (int i = 0; i < 20; ++i)
                log.press (key (Key::Char, (wchar_t) ('a' + i)), false, i * 100.0);
            check ("kept", juce::String ((int) log.recent().size()), juce::String (taw::KeyCastLog::maxEntries));
        }

        auto colour = [] (const char* text)
        {
            juce::Colour c;
            return taw::KeyCast::parseColour (text, c) ? taw::KeyCast::colourText (c) : juce::String ("-");
        };

        check ("#ff8800", colour ("#ff8800"), "#ff8800");
        check ("FF8800", colour ("FF8800"), "#ff8800");
        check ("#80ff8800", colour ("#80ff8800"), "#80ff8800");
        check ("yellow", colour ("yellow"), "#ffff00");
        check ("black", colour ("black"), "#000000");
        check ("nonsense", colour ("nonsense"), "-");
        return failures;
    }

#if JUCE_MAC
    // taw --terminal-profile   add the "taw" Terminal.app profile (keys taw needs, Option as Alt)
    if (args[0] == "--terminal-profile")
    {
        const auto r = taw::terminalSetup::install();
        std::puts (r.wasOk() ? "Added. Terminal opened a new window with the taw profile."
                             : r.getErrorMessage().toRawUTF8());
        return r.wasOk() ? 0 : 1;
    }
#endif

#if ! JUCE_WINDOWS
    // taw --keytest   check how terminal input decodes into keys (Terminal.app, iTerm2, kitty)
    if (args[0] == "--keytest")
    {
        auto describe = [] (const std::vector<taw::Key>& keys)
        {
            static const char* names[] = { "None", "Char", "Enter", "Backspace", "Escape", "Tab", "Left", "Right", "Up",
                                           "Down", "Home", "End", "PageUp", "PageDown", "Delete", "F1", "Mouse" };
            juce::StringArray out;

            for (const auto& k : keys)
                out.add (juce::String (k.ctrl ? "ctrl+" : "") + (k.alt ? "alt+" : "") + (k.shift ? "shift+" : "")
                         + (k.code == taw::Key::Char    ? juce::String::charToString ((juce::juce_wchar) k.ch)
                            : k.code == taw::Key::Mouse ? juce::String (k.drag ? "drag" : k.right ? "right" : "click") + (k.doubleClick ? "2" : "") + "@" + juce::String (k.x) + "," + juce::String (k.y)
                                                        : juce::String (names[k.code])));

            return out.joinIntoString (" ");
        };

        const std::pair<const char*, const char*> cases[] = {
            { "a", "a" }, { "\x7f", "Backspace" }, { "\r", "Enter" }, { "\t", "Tab" }, { "\x1b", "Escape" },
            { "\x1b[D", "Left" }, { "\x1b[1;2C", "shift+Right" }, { "\x1b[1;5D", "ctrl+Left" }, { "\x1b[1;3D", "alt+Left" },
            { "\x1b[1;7C", "ctrl+alt+Right" }, { "\x1b[1;6D", "ctrl+shift+Left" }, { "\x1b[1;2A", "shift+Up" },
            { "\x1b" "b", "alt+Left" }, { "\x1b\x10", "ctrl+alt+p" }, { "\x1b" "f", "alt+Right" }, { "\x1b\x1b[C", "alt+Right" }, { "\x1b/", "alt+/" },
            { "\x1b[H", "Home" }, { "\x1b[F", "End" }, { "\x1b[1;2H", "shift+Home" }, { "\x1b[5~", "PageUp" },
            { "\x1b[6~", "PageDown" }, { "\x1b[3~", "Delete" }, { "\x1bOP", "F1" }, { "\x1bOD", "Left" },
            { "\x1a", "ctrl+z" }, { "\x19", "ctrl+y" }, { "\x03", "ctrl+c" }, { "\x16", "ctrl+v" }, { "\x1d", "ctrl+]" },
            { "]", "]" }, { "[", "[" }, { "2m", "2 m" }, { "\xc3\xa9", "\xc3\xa9" }, { "\x1b[Z", "shift+Tab" },
            // kitty keyboard protocol (iTerm2, Ghostty, kitty, WezTerm); cmd counts as alt
            { "\x1b[27u", "Escape" }, { "\x1b[91;5u", "ctrl+[" }, { "\x1b[109;5u", "ctrl+m" }, { "\x1b[32;5u", "ctrl+ " },
            { "\x1b[47;9u", "alt+/" }, { "\x1b[122;5u", "ctrl+z" }, { "\x1b[13u", "Enter" },
            // kitty's shift+return; taw's Terminal profile: ctrl+up/down
            { "\x1b[13;2u", "shift+Enter" }, { "\x1b[1;5A", "ctrl+Up" }, { "\x1b[1;5B", "ctrl+Down" },
            // SGR mouse: presses and left drags (not releases, the wheel or the middle button), cells from 0
            { "\x1b[<0;10;5M", "click@9,4" }, { "\x1b[<0;10;5m", "" }, { "\x1b[<64;10;5M", "" }, { "\x1b[<34;10;5M", "" },
            { "\x1b[<1;10;5M", "" }, { "\x1b[<6;3;2M", "shift+right@2,1" }, { "\x1b[<16;3;2M", "ctrl+click@2,1" },
            { "\x1b[<0;20;7M\x1b[<0;20;7m\x1b[<0;20;7M\x1b[<0;20;7m\x1b[<0;20;7M", "click@19,6 click2@19,6 click@19,6" },
            { "\x1b[<0;30;7M\x1b[<0;31;7M", "click@29,6 click@30,6" },
            // a drag: once per cell it reaches, none where it was pressed; shift kept
            { "\x1b[<0;40;7M\x1b[<32;40;7M\x1b[<32;41;7M\x1b[<32;41;7M\x1b[<36;43;8M\x1b[<0;43;8m", "click@39,6 drag@40,6 shift+drag@42,7" },
        };

        // Control bytes made readable: esc[1;5D, 0x1a.
        auto label = [] (const std::string& bytes)
        {
            std::string out;

            for (auto c : bytes)
            {
                char hex[8];
                std::snprintf (hex, sizeof (hex), "0x%02x", (unsigned char) c);
                out += c == 0x1b ? "esc" : ((unsigned char) c < 0x20 || c == 0x7f) ? hex : std::string (1, c);
            }

            return out;
        };

        int failures = 0;

        for (const auto& [bytes, want] : cases)
        {
            std::string in (bytes);
            const auto got = describe (taw::Console::decode (in, true));
            const bool ok = got == juce::String::fromUTF8 (want);
            std::printf ("%-22s %-18s %s\n", label (bytes).c_str(), got.toRawUTF8(), ok ? "ok" : "FAILED");
            failures += ok ? 0 : 1;
        }

        // ctrl+space is a NUL byte.
        {
            std::string in (1, '\0');
            const auto got = describe (taw::Console::decode (in, true));
            std::printf ("%-22s %-18s %s\n", "NUL", got.toRawUTF8(), got == "ctrl+ " ? "ok" : "FAILED");
            failures += got == "ctrl+ " ? 0 : 1;
        }

        // A sequence split across reads waits for the rest instead of becoming Escape.
        {
            std::string in ("\x1b[1;");
            const auto first = taw::Console::decode (in, false);
            in += "5D";
            const auto got = describe (taw::Console::decode (in, false));
            const bool ok = first.empty() && got == "ctrl+Left";
            std::printf ("%-22s %-18s %s\n", "split esc[1; + 5D", got.toRawUTF8(), ok ? "ok" : "FAILED");
            failures += ok ? 0 : 1;
        }

        return failures;
    }
#endif

    // taw --new <dir>   an empty session there (at the new-session rate, 44.1 kHz by default), for scripts
    // and agents to build with --exec.
    if (args[0] == "--new")
    {
        juce::String error;
        const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile (args[1]);
        // At the new-session rate setting (44.1 kHz unless changed), as the menu makes them.
        auto s = args[1].isEmpty() ? nullptr : taw::Session::create (dir, {}, taw::newSessionRate(), error);
        std::printf ("%s\n", s != nullptr ? ("new session " + dir.getFullPathName()).toRawUTF8()
                                         : (args[1].isEmpty() ? juce::String ("taw --new <folder>") : error).toRawUTF8());
        return s != nullptr ? 0 : 1;
    }

    // taw --library [kinds | find <terms> | like <name> <terms>]   the sample library, for agents
    // and scripts. Terms: kind=kick, name=tape (file or folder contains), a measurement with < > or
    // = and a number (decay<200, brightness>80), sort=decay (sort=-decay: largest first),
    // limit=20, json. like <name>: the samples that sound most like the first whose file name
    // contains <name>, nearest first. Measurements: see Library::measureNames.
    if (args[0] == "--library")
    {
        taw::Library library (taw::configFolder().getChildFile ("library.json"));
        const auto& all = library.samples();
        const auto what = args[1].isEmpty() ? juce::String ("kinds") : args[1];
        auto kindOf = [] (const taw::SampleInfo& s) { return s.kind().isEmpty() ? juce::String ("other") : s.kind(); };

        if (what == "kinds")
        {
            std::map<juce::String, int> counts;

            for (const auto& s : all)
                ++counts[kindOf (s)];

            std::printf ("%d samples\n", (int) all.size());

            for (const auto& [k, n] : counts)
                std::printf ("  %-10s %d\n", k.toRawUTF8(), n);

            std::printf ("measurements: %s\n", taw::Library::measureNames().joinIntoString (" ").toRawUTF8());
            return 0;
        }

        if (what != "find" && what != "like")
        {
            std::printf ("taw --library kinds | find <terms> | like <name> <terms>\n");
            return 1;
        }

        int first = 2, limit = 20;
        std::vector<int> order;

        if (what == "like")
        {
            const auto name = args[2];
            first = 3;
            int seed = -1;

            for (int i = 0; i < (int) all.size() && seed < 0; ++i)
                if (juce::File (all[(size_t) i].path).getFileName().containsIgnoreCase (name))
                    seed = i;

            if (seed < 0)
            {
                std::printf ("no sample's name contains %s\n", name.toRawUTF8());
                return 1;
            }

            order.push_back (seed);

            for (auto j : library.nearest (seed, (int) all.size()))
                order.push_back (j);
        }
        else
        {
            for (int i = 0; i < (int) all.size(); ++i)
                order.push_back (i);
        }

        bool json = false;
        juce::String sortBy;
        bool descending = false;
        std::vector<std::function<bool (const taw::SampleInfo&)>> tests;

        for (int a = first; a < args.size(); ++a)
        {
            const auto t = args[a].trim();

            if (t == "json") { json = true; continue; }

            const int op = t.indexOfAnyOf ("<>=");

            if (op <= 0)
            {
                std::printf ("don't understand %s (kind=kick, name=tape, decay<200, sort=decay, limit=10, json)\n", t.toRawUTF8());
                return 1;
            }

            const auto key = t.substring (0, op).toLowerCase(), value = t.substring (op + 1);
            const auto c = t[op];

            if (key == "limit")       limit = value.getIntValue();
            else if (key == "sort")   { descending = value.startsWithChar ('-'); sortBy = value.trimCharactersAtStart ("-+").toLowerCase(); }
            else if (key == "kind")   tests.push_back ([=] (const taw::SampleInfo& s) { return kindOf (s).equalsIgnoreCase (value); });
            else if (key == "name")   tests.push_back ([=] (const taw::SampleInfo& s) { const juce::File f (s.path); return (f.getParentDirectory().getFileName() + "/" + f.getFileName()).containsIgnoreCase (value); });
            else if (taw::Library::measureNames().contains (key))
            {
                const auto n = value.getDoubleValue();
                tests.push_back ([=] (const taw::SampleInfo& s)
                {
                    const auto m = taw::Library::measure (s, key);
                    return c == '<' ? m < n : c == '>' ? m > n : std::abs (m - n) < 1.0e-6 * std::max (1.0, std::abs (n));
                });
            }
            else
            {
                std::printf ("no measurement called %s; there are: %s\n", key.toRawUTF8(), taw::Library::measureNames().joinIntoString (" ").toRawUTF8());
                return 1;
            }
        }

        std::vector<int> hits;

        for (auto i : order)
            if (std::all_of (tests.begin(), tests.end(), [&] (const auto& test) { return test (all[(size_t) i]); })
                || (what == "like" && i == order.front()))
                hits.push_back (i);

        if (sortBy.isNotEmpty())
            std::stable_sort (hits.begin() + (what == "like" ? 1 : 0), hits.end(), [&] (int a, int b)
            {
                const auto ma = taw::Library::measure (all[(size_t) a], sortBy), mb = taw::Library::measure (all[(size_t) b], sortBy);
                return descending ? ma > mb : ma < mb;
            });

        if ((int) hits.size() > limit + (what == "like" ? 1 : 0))
            hits.resize ((size_t) limit + (what == "like" ? 1 : 0));

        if (json)
        {
            juce::Array<juce::var> list;

            for (auto i : hits)
            {
                const auto& s = all[(size_t) i];
                auto* o = new juce::DynamicObject();
                o->setProperty ("path", s.path);
                o->setProperty ("kind", kindOf (s));
                o->setProperty ("kindBy", s.nameKind.isNotEmpty() ? "name" : "sound");

                for (const auto& m : taw::Library::measureNames())
                    o->setProperty (m, std::round (taw::Library::measure (s, m) * 100.0) / 100.0);

                list.add (o);
            }

            std::printf ("%s\n", juce::JSON::toString (list).toRawUTF8());
            return 0;
        }

        for (auto i : hits)
        {
            const auto& s = all[(size_t) i];
            std::printf ("%-9s %6.0f ms len  att %5.1f  dec %5.0f  bright %6.0f Hz  pitch %5.0f Hz  rms %5.1f  punch %4.1f  noise %.2f  low %5.1f  %s\n",
                         kindOf (s).toRawUTF8(), s.seconds * 1000.0, s.attackMs, s.decayMs, s.centroidHz, taw::Library::measure (s, "pitch"),
                         s.rmsDb, s.crestDb, s.flatness, taw::Library::measure (s, "lowend"), s.path.toRawUTF8());
        }

        std::printf ("%d shown\n", (int) hits.size());
        return 0;
    }

    // taw --index <folder> [<folder> ...]   measure the samples there into the library index (only
    // new or changed files), sort them by kind, and print what it found: how far the sound alone
    // agrees with the names, the kinds, the samples the names don't explain, and a few neighbours.
    if (args[0] == "--index")
    {
        juce::Array<juce::File> folders;

        for (int i = 1; i < args.size(); ++i)
            folders.add (juce::File::getCurrentWorkingDirectory().getChildFile (args[i]));

        taw::Library library (taw::configFolder().getChildFile ("library.json"));
        const auto started = juce::Time::getMillisecondCounterHiRes();
        library.scan (folders, [] (int done, int total) { std::printf ("\r  measuring %d / %d", done, total); std::fflush (stdout); });
        const auto accuracy = library.classify();
        library.layout();
        library.save();

        const auto& all = library.samples();
        std::printf ("\r%d samples in %.1f s \xc2\xb7 the sound alone agrees with the name for %.0f%% of named ones\n\n", (int) all.size(),
                     (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0, accuracy * 100.0);

        std::map<juce::String, std::pair<int, int>> kinds; // named, by sound only
        std::map<juce::String, int> confusions;

        for (const auto& s : all)
        {
            if (s.nameKind.isNotEmpty())
                ++kinds[s.nameKind].first;
            else
                ++kinds[s.soundKind].second;

            if (s.nameKind.isNotEmpty() && s.soundKind != s.nameKind)
                ++confusions[s.nameKind + " heard as " + s.soundKind];
        }

        for (const auto& [kind, n] : kinds)
            std::printf ("  %-10s %4d by name  %4d by sound\n", kind.toRawUTF8(), n.first, n.second);

        std::printf ("\nmismatches:");

        for (const auto& [what, n] : confusions)
            std::printf ("  %s %d", what.toRawUTF8(), n);

        auto row = [] (const taw::SampleInfo& s)
        {
            std::printf ("  %-10s %3.0f%%  pk %5.1f  rms %5.1f  att %5.1f ms  dec %6.0f ms  tail %6.0f ms  hits %d  pitch %6.1f Hz (%.2f)  bright %6.0f Hz  flat %.2f  %s\n",
                         s.kind().toRawUTF8(), s.soundKindScore * 100.0, s.peakDb, s.rmsDb, s.attackMs, s.decayMs, s.tailMs, s.hits, s.pitchHz,
                         s.pitchConfidence, s.centroidHz, s.flatness, juce::File (s.path).getFileName().toRawUTF8());
        };

        std::printf ("\n\nnot named, sorted by sound:\n");

        for (const auto& s : all)
            if (s.nameKind.isEmpty())
                row (s);

        // Neighbours of a few samples, to see "sounds alike" at work.
        std::printf ("\nneighbours:\n");
        juce::Random rng (7);

        for (int t = 0; t < 4 && ! all.empty(); ++t)
        {
            const int i = rng.nextInt ((int) all.size());
            std::printf ("%s\n", juce::File (all[(size_t) i].path).getFileName().toRawUTF8());

            for (auto j : library.nearest (i, 5))
                row (all[(size_t) j]);
        }

        return 0;
    }

    // What taw --hum and --humtest print: the notes and the hits, in beats too when there's a tempo.
    auto printHeard = [] (const taw::Heard& h, double bpm, double grid)
    {
        std::printf ("%.2f s \xc2\xb7 %s (%.0f%% held notes) \xc2\xb7 tuning %+.0f cents\n", h.seconds, h.sung() ? "sung" : "a beat",
                     h.pitched * 100.0, h.tuningCents);
        std::printf ("notes: %d\n", (int) h.notes.size());

        for (const auto& n : h.notes)
        {
            std::printf ("  %7.3f s  %5.3f s  %-4s %3d  %+4.0f c  %6.1f dB", n.start, n.length,
                         juce::MidiMessage::getMidiNoteName (n.pitch, true, true, 4).toRawUTF8(), n.pitch, n.cents, n.levelDb);

            if (bpm > 0.0)
                std::printf ("   beat %6.2f  len %5.2f", taw::humBeat (n.start, bpm, grid), std::max (grid, taw::humBeat (n.length, bpm, grid)));

            std::printf ("\n");
        }

        std::printf ("hits: %d\n", (int) h.hits.size());

        for (const auto& hit : h.hits)
        {
            std::printf ("  %7.3f s  %-8s  %6.1f dB  attack %5.0f Hz  bright %5.0f Hz  low %.2f  high %.2f  noise %.2f  decay %4.0f ms", hit.time,
                         hit.kind.toRawUTF8(), hit.levelDb, hit.attackHz, hit.centroidHz, hit.lowShare, hit.highShare, hit.flatness, hit.decayMs);

            if (bpm > 0.0)
                std::printf ("   beat %6.2f", taw::humBeat (hit.time, bpm, grid));

            std::printf ("\n");
        }
    };

    // taw --hum <file> [bpm [grid]]   hear a hummed, sung or beatboxed recording: its notes and its
    // hits. With a tempo, in beats too, on a grid (0.25 of a beat unless given; 0 for none).
    if (args[0] == "--hum" && args.size() >= 2)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        const auto started = juce::Time::getMillisecondCounterHiRes();
        const auto heard = taw::hear (juce::File::getCurrentWorkingDirectory().getChildFile (args[1]), fm);

        if (! heard)
        {
            std::printf ("can't read %s\n", args[1].toRawUTF8());
            return 1;
        }

        printHeard (*heard, args[2].getDoubleValue(), args.size() > 3 ? args[3].getDoubleValue() : 0.25);
        std::printf ("(%.0f ms)\n", juce::Time::getMillisecondCounterHiRes() - started);

        // taw --hum <file> ... contour: the sung pitch itself, a line every 20 ms where there is one.
        if (args.contains ("contour"))
            for (size_t i = 0; i < heard->contour.size(); i += 2)
                if (const auto p = heard->contour[i]; p > 0.0f)
                    std::printf ("%7.2f s  %6.2f  %*s*\n", (double) i * heard->contourHop, (double) p, juce::jlimit (0, 60, juce::roundToInt ((p - 36.0f) * 3.0f)), "");
        return 0;
    }

    // taw --humtest [folder]   a melody and a beat made by formula (a voice-like tone: weak
    // fundamental, scoops, vibrato, 25 cents flat; a kick, snare and hats), heard back and checked
    // against what was played. With a folder, the two are left there as wav files to listen to.
    if (args[0] == "--humtest")
    {
        const double rate = 44100.0, twoPi = juce::MathConstants<double>::twoPi;
        int failures = 0;

        auto check = [&] (const juce::String& what, bool ok)
        {
            std::printf ("%s  %s\n", ok ? "ok    " : "FAILED", what.toRawUTF8());
            failures += ok ? 0 : 1;
        };

        juce::Random noise (1);

        auto keep = [&] (const char* name, const std::vector<float>& x)
        {
            if (args.size() < 2)
                return;

            const auto folder = juce::File::getCurrentWorkingDirectory().getChildFile (args[1]);
            folder.createDirectory();
            const auto file = folder.getChildFile (name);
            file.deleteFile();
            std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();

            if (auto w = juce::WavAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (rate).withNumChannels (1).withBitsPerSample (24)))
            {
                const float* data = x.data();
                w->writeFromFloatArrays (&data, 1, (int) x.size());
            }
        };

        // The melody. from and to: the level a note comes from and leaves to (0 silence, 1 straight
        // from or into its neighbour, between: struck again without a breath).
        struct Sung { int pitch; double start, length, from, to; };
        const std::vector<Sung> melody {
            { 40, 0.50, 0.40, 0.0, 0.0 },                               // E2, low
            { 45, 1.00, 0.40, 0.0, 0.0 }, { 45, 1.44, 0.40, 0.0, 0.0 }, // the same note twice, 40 ms apart
            { 48, 2.00, 0.50, 0.0, 1.0 }, { 60, 2.50, 0.50, 1.0, 0.0 }, // up an octave without a gap
            { 55, 3.20, 0.40, 0.0, 0.1 }, { 55, 3.60, 0.40, 0.1, 0.0 }, // struck again on one breath
            { 72, 4.20, 0.30, 0.0, 0.0 },                               // C5, high
            { 50, 4.70, 0.12, 0.0, 0.0 } };                             // short
        {
            const double harmonics[] = { 0.2, 1.0, 0.7, 0.5, 0.3, 0.2 }, ramp = 0.03;
            std::vector<float> x ((size_t) (5.2 * rate), 0.0f);
            double phase = 0.0;

            for (const auto& s : melody)
            {
                const int at = (int) (s.start * rate), len = (int) (s.length * rate);

                for (int i = 0; i < len; ++i)
                {
                    const double t = i / rate, left = (len - i) / rate;
                    double pitch = s.pitch - 0.25;

                    if (s.from < 1.0 && t < 0.04)
                        pitch -= 1.5 * (1.0 - t / 0.04); // scooped into

                    if (t > 0.15)
                        pitch += 0.3 * std::sin (twoPi * 5.5 * (t - 0.15)); // vibrato

                    const double hz = 440.0 * std::pow (2.0, (pitch - 69.0) / 12.0);
                    phase += twoPi * hz / rate;
                    double amp = t < ramp ? s.from + (1.0 - s.from) * t / ramp : 1.0, v = 0.0;

                    if (left < ramp)
                        amp = std::min (amp, s.to + (1.0 - s.to) * left / ramp);

                    for (int h = 1; h <= 6; ++h)
                        if (hz * h < 5000.0)
                            v += harmonics[h - 1] * std::sin (phase * h);

                    x[(size_t) (at + i)] = (float) (0.25 * amp * v);
                }
            }

            for (auto& v : x)
                v += 0.001f * (noise.nextFloat() * 2.0f - 1.0f);

            keep ("hum-melody.wav", x);
            const auto heard = taw::hear (x.data(), (int) x.size(), rate);
            printHeard (heard, 0.0, 0.0);

            check ("the melody is heard as sung", heard.sung());
            check ("the singer's tuning (25 cents flat) is found", std::abs (heard.tuningCents + 25.0) < 10.0);
            check ("as many notes as were sung", heard.notes.size() == melody.size());

            for (size_t i = 0; i < std::min (heard.notes.size(), melody.size()); ++i)
            {
                const auto& n = heard.notes[i];
                const auto& s = melody[i];
                check ("note " + juce::String ((int) i + 1) + " is " + juce::MidiMessage::getMidiNoteName (s.pitch, true, true, 4)
                           + " at " + juce::String (s.start, 2) + " s for " + juce::String (s.length, 2) + " s",
                       n.pitch == s.pitch && std::abs (n.start - s.start) < 0.04 && std::abs (n.length - s.length) < 0.06);
            }
        }

        // The beat: eighths at 120 bpm. k kick, s snare, h hat, o open hat.
        {
            const juce::String pattern ("khshkkshkhshkhso");
            std::vector<float> x ((size_t) ((0.25 + 0.25 * pattern.length() + 0.5) * rate), 0.0f);

            for (int p = 0; p < pattern.length(); ++p)
            {
                const auto drum = pattern[p];
                const int at = (int) ((0.25 + 0.25 * p) * rate), len = (int) ((drum == 'o' ? 0.45 : 0.22) * rate);
                juce::IIRFilter band, high1, high2;
                band.setCoefficients (juce::IIRCoefficients::makeBandPass (rate, 2500.0, 0.8));
                high1.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, 7000.0));
                high2.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, 7000.0));
                double phase = 0.0;

                for (int i = 0; i < len; ++i)
                {
                    const double t = i / rate;
                    const float hiss = noise.nextFloat() * 2.0f - 1.0f;
                    double v = 0.0;

                    if (drum == 'k')
                    {
                        phase += twoPi * (50.0 + 90.0 * std::exp (-t / 0.03)) / rate;
                        v = 0.8 * std::sin (phase) * std::exp (-t / 0.05);
                    }
                    else if (drum == 's')
                        v = (0.6 * (double) band.processSingleSampleRaw (hiss) + 0.1 * std::sin (twoPi * 190.0 * t)) * std::exp (-t / 0.04);
                    else
                        v = 0.5 * (double) high2.processSingleSampleRaw (high1.processSingleSampleRaw (hiss)) * std::exp (-t / (drum == 'o' ? 0.09 : 0.012));

                    x[(size_t) (at + i)] += (float) v;
                }
            }

            for (auto& v : x)
                v += 0.001f * (noise.nextFloat() * 2.0f - 1.0f);

            keep ("hum-beat.wav", x);
            const auto heard = taw::hear (x.data(), (int) x.size(), rate);
            printHeard (heard, 120.0, 0.5);

            check ("the beat is heard as a beat", ! heard.sung());
            check ("as many hits as were played", (int) heard.hits.size() == pattern.length());

            for (int p = 0; p < std::min ((int) heard.hits.size(), pattern.length()); ++p)
            {
                const auto drum = pattern[p];
                const juce::String kind (drum == 'k' ? "kick" : drum == 's' ? "snare" : drum == 'o' ? "open hat" : "hihat");
                const auto& hit = heard.hits[(size_t) p];
                check ("hit " + juce::String (p + 1) + " is a " + kind + " on eighth " + juce::String (p + 1),
                       hit.kind == kind && std::abs (hit.time - (0.25 + 0.25 * p)) < 0.01);
            }
        }

        std::printf ("%d failed\n", failures);
        return failures;
    }

    // taw --phonetest   the phone link, asked as a phone would: its page, an upload, and what it
    // must refuse (a wrong address, a file that isn't audio). Prints its QR code too.
    if (args[0] == "--phonetest")
    {
        int failures = 0;

        auto check = [&] (const char* what, bool ok)
        {
            std::printf ("%s  %s\n", ok ? "ok    " : "FAILED", what);
            failures += ok ? 0 : 1;
        };

        taw::PhoneLink link;
        juce::String error;

        if (! link.start (error))
        {
            std::printf ("couldn't start: %s\n", error.toRawUTF8());
            return 1;
        }

        const auto url = link.url();
        std::printf ("%s\n", url.toRawUTF8());
        const auto host = url.fromFirstOccurrenceOf ("//", false, false).upToFirstOccurrenceOf (":", false, false);
        const int port = url.fromLastOccurrenceOf (":", false, false).upToFirstOccurrenceOf ("/", false, false).getIntValue();
        const auto home = "/" + url.fromLastOccurrenceOf ("/", false, false);

        // One request, the whole answer back.
        auto ask = [&] (const juce::String& method, const juce::String& path, const juce::MemoryBlock& body)
        {
            juce::StreamingSocket s;

            if (! s.connect (host, port, 3000))
                return juce::String ("no connection");

            const auto head = method + " " + path + " HTTP/1.1\r\nHost: taw\r\n"
                              + (method == "POST" ? "Content-Length: " + juce::String ((int) body.getSize()) + "\r\n" : juce::String()) + "\r\n";
            s.write (head.toRawUTF8(), (int) head.getNumBytesAsUTF8());

            if (body.getSize() > 0)
                s.write (body.getData(), (int) body.getSize());

            juce::MemoryBlock answer;
            char buffer[4096];

            while (s.waitUntilReady (true, 5000) == 1)
            {
                const int got = s.read (buffer, sizeof (buffer), false);

                if (got <= 0)
                    break;

                answer.append (buffer, (size_t) got);
            }

            return answer.toString();
        };

        const auto page = ask ("GET", home, {});
        check ("the page is served at the address", page.startsWith ("HTTP/1.1 200") && page.contains ("Send a recording"));
        check ("nothing at a wrong token", ask ("GET", "/nope", {}).startsWith ("HTTP/1.1 404"));
        check ("nothing at the root", ask ("GET", "/", {}).startsWith ("HTTP/1.1 404"));

        juce::MemoryBlock take (300000);
        juce::Random random (3);

        for (size_t i = 0; i < take.getSize(); ++i)
            take[i] = (char) random.nextInt (256);

        check ("an upload is taken", ask ("POST", home + "/up?name=my%20take.m4a", take).startsWith ("HTTP/1.1 200"));
        check ("a file that isn't audio is refused", ask ("POST", home + "/up?name=notes.exe", take).startsWith ("HTTP/1.1 415"));
        check ("an upload to a wrong token is refused", ask ("POST", "/nope/up?name=a.wav", take).startsWith ("HTTP/1.1 404"));
        check ("a name can't climb out of the folder", ask ("POST", home + "/up?name=..%2F..%2Fevil.wav", take).startsWith ("HTTP/1.1 200"));

        const auto got = link.takeReceived();
        check ("two files arrived", got.size() == 2 && link.received() == 2);

        if (got.size() == 2)
        {
            juce::MemoryBlock back;
            got[0].loadFileAsData (back);
            check ("the first whole, under its name", got[0].getFileName() == "my take.m4a" && back == take);
            check ("the second kept inside the link's folder", got[1].getParentDirectory() == got[0].getParentDirectory());
        }

        const auto folder = got.empty() ? juce::File() : got[0].getParentDirectory();
        const auto code = taw::PhoneLink::qr (url);
        check ("the address makes a QR code", code.size() >= 21);

        for (size_t y = 0; y < code.size(); y += 2)
        {
            for (size_t x = 0; x < code.size(); ++x)
            {
                const bool top = code[y][x], bottom = y + 1 < code.size() && code[y + 1][x];
                std::printf ("%s", top && bottom ? "\xe2\x96\x88" : top ? "\xe2\x96\x80" : bottom ? "\xe2\x96\x84" : " ");
            }

            std::printf ("\n");
        }

        link.stop();
        check ("its folder goes when it stops", folder != juce::File() && ! folder.exists());
        check ("nothing answers once it's off", ask ("GET", home, {}) == "no connection");

        // taw --phonetest <session-dir>: in taw itself too (on a scratch session: it's edited).
        // phone, a second of sine sent as a phone would, and it's a clip on a new track.
        if (args.size() >= 2)
        {
            auto app = std::make_unique<taw::App> (juce::File::getCurrentWorkingDirectory().getChildFile (args[1]), juce::File(), juce::String(), juce::String());

            if (app->session == nullptr)
            {
                std::printf ("couldn't open: %s\n", app->message.toRawUTF8());
                return failures + 1;
            }

            while (app->hasBackgroundWork())
                app->pumpBackgroundWork();

            const int tracksBefore = app->session->numTracks();
            const auto on = app->execute ("phone");
            std::printf ("       %s\n", on.message.toRawUTF8());
            check ("phone turns the link on", on.ok && app->phone.running() && app->phoneVisible);
            std::printf ("%s", app->screenText (100, 40).toRawUTF8());

            juce::MemoryBlock wav;
            {
                std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (wav, false);

                if (auto w = juce::WavAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (16)))
                {
                    juce::AudioBuffer<float> b (1, 44100);

                    for (int i = 0; i < b.getNumSamples(); ++i)
                        b.setSample (0, i, 0.5f * std::sin ((float) i * 0.05f));

                    w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
                }
            }

            const auto appUrl = app->phone.url();
            const auto appHome = "/" + appUrl.fromLastOccurrenceOf ("/", false, false);
            // (the same address and port: the first link is off)
            check ("the upload is taken", ask ("POST", appHome + "/up?name=phone%20take.wav", wav).startsWith ("HTTP/1.1 200"));

            for (const auto until = juce::Time::getMillisecondCounter() + 10000;
                 juce::Time::getMillisecondCounter() < until && (app->session->numTracks() == tracksBefore || app->hasBackgroundWork());)
            {
                app->pumpBackgroundWork();
                juce::Thread::sleep (5);
            }

            const auto track = app->session->track (app->session->numTracks() - 1);
            check ("it's a clip on a new track, named after the file",
                   app->session->numTracks() == tracksBefore + 1 && track[taw::ids::name].toString() == "phone take" && track.getNumChildren() > 0);
            std::printf ("       %s\n", app->message.toRawUTF8());
            check ("phone off ends it", app->execute ("phone off").ok && ! app->phone.running() && ! app->phoneVisible);
        }
        std::printf ("%d failed\n", failures);
        return failures;
    }

    // taw --importtest <dest> <files...>   import files at 44.1 kHz into <dest> (no device needed)
    if (args[0] == "--importtest")
    {
        // Paths go through the same quoting and splitting as a typed or dropped command line.
        juce::StringArray quoted;
        for (int i = 2; i < args.size(); ++i)
            quoted.add (args[i].quoted());

        juce::StringArray notFound;
        auto files = taw::Import::expand (taw::App::splitPaths (quoted.joinIntoString (" ")),
                                          juce::File::getCurrentWorkingDirectory(), notFound);
        taw::Import imp (std::move (files), juce::File::getCurrentWorkingDirectory().getChildFile (args[1]), 44100.0);
        imp.start();

        while (! imp.isDone())
            juce::Thread::sleep (5);

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        int failures = notFound.size();

        for (const auto& item : imp.results())
        {
            // Report where the loudest sample sits, to check resampling keeps timing.
            juce::int64 peakAt = -1;

            if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor (item.copy) })
            {
                juce::AudioBuffer<float> b ((int) r->numChannels, (int) r->lengthInSamples);
                r->read (&b, 0, b.getNumSamples(), 0, true, true);
                float best = 0;
                for (int i = 0; i < b.getNumSamples(); ++i)
                    if (std::abs (b.getSample (0, i)) > best) { best = std::abs (b.getSample (0, i)); peakAt = i; }
            }

            std::printf ("%-14s -> %-22s ch %d  len %lld  peak@%lld  %s\n", item.source.getFileName().toRawUTF8(),
                         item.copy.getFileName().toRawUTF8(), item.channels, (long long) item.length, (long long) peakAt,
                         item.error.toRawUTF8());
            failures += item.error.isEmpty() ? 0 : 1;
        }

        for (const auto& n : notFound)
            std::printf ("not found: %s\n", n.toRawUTF8());

        return failures;
    }

    if (args.contains ("--devices"))
    {
        juce::AudioDeviceManager dm;

        for (auto* type : dm.getAvailableDeviceTypes())
        {
            type->scanForDevices();
            std::printf ("%s\n", type->getTypeName().toRawUTF8());

            for (const auto& name : type->getDeviceNames (false))
                std::printf ("  out  %s\n", name.toRawUTF8());

            if (type->hasSeparateInputsAndOutputs())
                for (const auto& name : type->getDeviceNames (true))
                    std::printf ("  in   %s\n", name.toRawUTF8());
        }

        // MIDI inputs, and whether taw can open each (another app may hold it).
        std::printf ("MIDI\n");

        for (const auto& info : juce::MidiInput::getAvailableDevices())
        {
            const bool opens = juce::MidiInput::openDevice (info.identifier, nullptr) != nullptr;
            std::printf ("  in   %s  %s  (%s)\n", info.name.toRawUTF8(), opens ? "ok" : "BUSY: another app has it open",
                         info.identifier.toRawUTF8());
        }

        return 0;
    }

    // Headless check of the record path: open a device, record input 1 for two seconds.
    if (const auto st = args.indexOf ("--selftest"); st >= 0)
    {
        auto engineOwned = std::make_unique<taw::AudioEngine>();
        auto& engine = *engineOwned;
        auto& dm = engine.devices();
        dm.initialise (taw::AudioEngine::maxInputs, 2, nullptr, false);
#if JUCE_WINDOWS
        dm.setCurrentAudioDeviceType ("ASIO", true);
#else
        dm.setCurrentAudioDeviceType ("CoreAudio", true);
#endif

        if (auto* type = dm.getCurrentDeviceTypeObject())
            type->scanForDevices();
        auto setup = dm.getAudioDeviceSetup();
#if JUCE_WINDOWS
        setup.outputDeviceName = setup.inputDeviceName = args[st + 1].isNotEmpty() ? args[st + 1] : "SSL ASIO Driver 1";
#else
        if (args[st + 1].isNotEmpty()) // otherwise the system's default input and output
            setup.outputDeviceName = setup.inputDeviceName = args[st + 1];
#endif
        setup.useDefaultInputChannels = setup.useDefaultOutputChannels = true;

        if (auto err = dm.setAudioDeviceSetup (setup, true); err.isNotEmpty() || dm.getCurrentAudioDevice() == nullptr)
        {
            std::printf ("open failed: %s\n", err.toRawUTF8());
            return 1;
        }

        auto* d = dm.getCurrentAudioDevice();
        std::printf ("%s  %.0f Hz  %d samples  in %d  out %d  latency %d+%d\n", d->getName().toRawUTF8(), d->getCurrentSampleRate(),
                     d->getCurrentBufferSizeSamples(), engine.numInputChannels(), d->getActiveOutputChannels().countNumberOfSetBits(),
                     d->getInputLatencyInSamples(), d->getOutputLatencyInSamples());

        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("taw-selftest.wav");
        juce::String err;
        auto rec = engine.createRecorder (0, 0, 1, file, d->getCurrentSampleRate(), err);

        auto g = std::make_unique<taw::Graph>();
        g->tracks.push_back ({});
        g->tracks[0].inputChannel = 0;
        g->tracks[0].recorder = rec;
        engine.setGraph (std::move (g));
        engine.play (0);

        float peak = 0;
        for (int i = 0; i < 20; ++i)
        {
            juce::Thread::sleep (100);
            peak = juce::jmax (peak, engine.takeInputPeak (0));
        }

        engine.stop();
        engine.setGraph (std::make_unique<taw::Graph>());
        engine.finishRecorders ({ rec }); // rec stays referenced here, as the UI's live take does

        // Loop: a quarter-second loop should keep the playhead inside it.
        {
            const auto loopEnd = (juce::int64) (d->getCurrentSampleRate() / 4);
            auto lg = std::make_unique<taw::Graph>();
            lg->loop = true;
            lg->loopStart = 0;
            lg->loopEnd = loopEnd;
            engine.setGraph (std::move (lg));
            engine.play (0);
            juce::int64 maxSeen = 0;

            for (int i = 0; i < 50; ++i)
            {
                juce::Thread::sleep (10);
                maxSeen = juce::jmax (maxSeen, engine.playhead());
            }

            engine.stop();
            std::printf ("loop: end %lld  max playhead seen %lld  %s\n", (long long) loopEnd, (long long) maxSeen,
                         maxSeen < loopEnd ? "ok" : "FAILED");
            engine.setGraph (std::make_unique<taw::Graph>());
        }

        for (int i = 0; i < 100; ++i)
        {
            if (const auto takes = engine.poll(); ! takes.empty()) // the first take that's finished
            {
                const auto& f = takes.front();
                std::unique_ptr<juce::AudioFormatReader> r (engine.formats().createReaderFor (f.file));
                std::printf ("take: start %lld  written %lld  on disk %lld  dropped %d  input-1 peak %.1f dB\n", (long long) f.start,
                             (long long) f.length, r ? (long long) r->lengthInSamples : -1LL, f.droppedBlocks,
                             juce::Decibels::gainToDecibels (peak));
                return 0;
            }

            juce::Thread::sleep (10);
        }

        std::puts ("recorder never finished");
        return 1;
    }

    juce::String templateName;
    if (const auto t = args.indexOf ("-t"); t >= 0)
    {
        templateName = args[t + 1];
        args.removeRange (t, 2);
    }

#if TAW_HUB
    // taw hub: the hub's page, no session (a session folder called hub: taw ./hub).
    const bool hubPage = args.size() == 1 && args[0] == "hub";

    if (hubPage)
        args.clear();
#else
    const bool hubPage = false;

    // Not a session called hub by mistake (taw ./hub is one).
    if (args.size() == 1 && args[0] == "hub")
    {
        std::printf ("the audio hub runs on macOS only so far\n");
        return 1;
    }
#endif

    const auto cwd = juce::File::getCurrentWorkingDirectory();
    auto target = args.isEmpty() ? cwd : cwd.getChildFile (args[0]);

    // Plain taw from inside a session (its audio folder, say) opens that session.
    if (args.isEmpty())
        for (auto up = cwd; up != up.getParentDirectory(); up = up.getParentDirectory())
            if (taw::Session::existsIn (up))
            {
                target = up;
                break;
            }

    juce::File openDir, newParent;
    juce::String newName;

    if (taw::Session::existsIn (target) && ! hubPage)
    {
        openDir = target;
    }
    else
    {
        newParent = args.isEmpty() ? cwd : target.getParentDirectory();
        newName = args.isEmpty() ? juce::String() : target.getFileName();
    }

    // The last run died: if it had a session open and you didn't ask for anything else, go
    // straight back to it (opening it also recovers a take that was recording).
    taw::safety::installCloseHandler();
    const auto unclean = taw::safety::findUncleanExit();
    const bool reopen = unclean.found && openDir == juce::File() && args.isEmpty() && ! hubPage && taw::Session::existsIn (unclean.session);

    if (reopen)
        openDir = unclean.session;

#if JUCE_MAC
    taw::terminalSetup::offerOnce(); // before the screen is taken over
#endif

    taw::safety::startRunMarker();
    taw::enableRecentSessions();
    int status = 0;

    {
        // On the heap: App and AudioEngine are big (their queues), and main's stack frame holds
        // every test's locals at once. Plugins load on this thread and some need a lot of stack;
        // with these on it they overflowed it.
        auto appOwned = std::make_unique<taw::App> (openDir, newParent, newName, templateName);
        auto& app = *appOwned;

        if (unclean.found)
            app.reportUncleanExit (unclean.crashLog, reopen);

        app.openHubFirst = hubPage;

        status = app.run();
    }

    taw::safety::markStopped();
    taw::safety::shutdownComplete();
    return status;
}
