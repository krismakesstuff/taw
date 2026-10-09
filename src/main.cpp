#include "App.h"
#include "AlsImport.h"
#include "Bounce.h"
#include "Import.h"
#include "Library.h"
#include "Plugins.h"
#include "Safety.h"
#include "Ui.h"
#include "Video.h"

#include <juce_events/juce_events.h>
#include <numeric>

#if JUCE_MAC
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

    taw::safety::installCrashHandler();

    if (args.contains ("-h") || args.contains ("--help"))
    {
        std::puts ("usage: taw [session-dir] [-t template]\n\n"
                   "  taw                 open the session in this folder, or start a new one\n"
                   "  taw my-song         open my-song, or create it\n"
                   "  taw my-song -t 3mic create my-song from the 3mic template\n\n"
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

    // taw --plugintest <file.vst3>   load one plugin and play it the way the engine does: a chord
    // from the timeline, a stop, a key from the controller, a disarm. Checks its state round-trips.
    // Leaves the plugin list alone.
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
        constexpr double rate = 48000.0;
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
            const auto got = dec.seek ((frame + 0.25) / info.fps);
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

            dec.seek ((frame + 0.5) / info.fps);
            juce::Image mine;
            dec.render (mine, w, h);
            const auto dSame = difference (mine, same), dBefore = difference (mine, before), dAfter = difference (mine, after);
            const bool exact = dSame < dBefore && dSame < dAfter && dSame < 6.0;
            std::printf ("frame %lld: off by %.2f from ffmpeg's (its neighbours: %.2f, %.2f) %s\n", (long long) frame, dSame, dBefore, dAfter,
                         exact ? "ok" : dBefore == dSame || dAfter == dSame ? "FAILED (or the picture doesn't move there)" : "FAILED");
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

        const auto front = GetForegroundWindow();
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

        if (GetForegroundWindow() != front)
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
                    tp.sends.push_back ({ 0, 0.7f, t % 2 == 0 });

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
            const auto pct = [&] (double p) { return sorted[(size_t) std::min ((double) sorted.size() - 1, p * sorted.size())] / budgetMs * 100.0; };
            const auto over = std::count_if (sorted.begin(), sorted.end(), [&] (double ms) { return ms > budgetMs; });
            const auto close80 = std::count_if (sorted.begin(), sorted.end(), [&] (double ms) { return ms > budgetMs * 0.8; });

            std::printf ("\n== %s: %d blocks from %s\n", what, (int) blocks.size(), app->formatBars (start).toRawUTF8());
            std::printf ("   load  median %.0f%%  p99 %.0f%%  worst %.0f%%  |  over 100%%: %d  over 80%%: %d\n", pct (0.5), pct (0.99),
                         sorted.back() / budgetMs * 100.0, (int) over, (int) close80);
            std::printf ("   per track, ms per block (budget %.2f):  mean   worst\n", budgetMs);

            for (int t = 0; t < numTracks; ++t)
                if (most[(size_t) t] > 0.05)
                    std::printf ("     %-60s %5.2f  %6.2f\n", trackName (t).substring (0, 60).toRawUTF8(), sum[(size_t) t] / blocks.size(), most[(size_t) t]);

            std::printf ("     %-60s %5.2f  %6.2f\n", "mix inserts", mixSum / blocks.size(), mixMost);

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
    if (args[0] == "--savetest" && args.size() >= 3)
    {
        const auto source = juce::File::getCurrentWorkingDirectory().getChildFile (args[1]);
        const auto scratch = juce::File::getCurrentWorkingDirectory().getChildFile (args[2]);
        const auto dir = scratch.getChildFile (source.getFileName());
        scratch.deleteRecursively();
        scratch.createDirectory();
        source.copyDirectoryTo (dir);
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
                k.ch = (juce::juce_wchar) answer[0];
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
            check (bpmOnDisk() == bpm0, "don't save: session.json as it was");
            check (app->backgroundBounces.empty(), "don't save: no bounce");
        }

        // An edit, then "save": session.json, and last-saved bounced in the background.
        {
            auto app = open();
            app->execute ("bpm " + juce::String (bpm0 + 9));
            app->pumpBackgroundWork();
            close (*app, "s");
            check (! work.exists(), "save: unsaved\\ is gone");
            check (bpmOnDisk() == bpm0 + 9, "save: session.json has the edit");
            check (app->backgroundBounces.size() == 1, "save: last-saved is bouncing");

            while (! app->backgroundBounces.empty())
            {
                app->pollBackgroundBounces();
                juce::Thread::sleep (20);
            }

            const auto mp3 = dir.getChildFile ("bounces").getChildFile ("last-saved.mp3");
            check (mp3.getSize() > 10000, "save: bounces\\last-saved.mp3 written");
            std::printf ("       %s  %lld bytes  %s\n", mp3.getFullPathName().toRawUTF8(), (long long) mp3.getSize(), app->message.toRawUTF8());

            // The session screen: the bounces list has it (newest, so first), and it plays.
            check (! app->recentBounces.empty() && app->recentBounces.front().file == mp3 && app->bounceIndex == 0, "the bounces list has it, selected");

            // Areas: ctrl+arrows move between them, the way they sit on screen.
            using Area = taw::App::PickArea;
            auto key = [&] (taw::Key::Code code, bool ctrl)
            {
                taw::Key k;
                k.code = code;
                k.ctrl = ctrl;
                app->pressKey (k);
                return app->pickArea;
            };
            check (app->pickArea == Area::sessions, "the session screen starts in the recent sessions");
            check (key (taw::Key::Right, true) == Area::bounces, "ctrl+right: bounces");
            check (key (taw::Key::Left, true) == Area::sessions, "ctrl+left: sessions");
            check (key (taw::Key::Up, true) == Area::templates, "ctrl+up: templates");
            check (key (taw::Key::Down, true) == Area::sessions, "ctrl+down: sessions");
            const auto before = app->sessionIndex;
            key (taw::Key::Down, false);
            check (app->sessionIndex == juce::jmin (before + 1, (int) app->recent.size() - 1) && app->pickArea == Area::sessions, "down: the next session");
            key (taw::Key::Right, true);
            std::printf ("%s", app->screenText (100, 24).toRawUTF8());
            std::printf ("%s", app->screenText (70, 16).toRawUTF8());
            app->playBounce (mp3);

            while (! app->bouncePlayer->loaded.load())
                juce::Thread::sleep (10);

            app->pollBackgroundBounces();
            check (app->bouncePlayer != nullptr && app->bounceLengthSeconds() > 1.0, "it plays");
            std::printf ("       %.1f s long\n", app->bounceLengthSeconds());
            app->stopBounce();
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
        check (bpmOnDisk() == bpm0 + 9, "...and session.json not changed");

        {
            auto app = open();
            check (app->unsaved && app->session->recovered, "reopened: the changes are back, unsaved");
            check ((double) app->session->tree()[taw::ids::bpm] == bpm0 + 11, "...with the edit");
            close (*app, "d");
        }

        std::printf ("%s\n", failures == 0 ? "all ok" : "FAILURES");
        return failures;
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
            // ":screen" prints the screen as text (160x44); ":key o", ":key right", ":key ]" press a key.
            if (args[i] == ":screen")
            {
                std::printf ("%s", app.screenText (160, 44).toRawUTF8());
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

            if (args[i].startsWith (":key "))
            {
                auto name = args[i].substring (5);
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
                k.ch = name[0];

                for (const auto& [n, code] : named)
                    if (name.equalsIgnoreCase (n))
                        k.code = code;

                app.pressKey (k);
                std::printf ("key  %-24s %s\n", name.toRawUTF8(), app.message.toRawUTF8());
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
                const auto piece = Session::trimmedMidiClip (clips[0], 8.0, 12.0);
                check ("a trimmed hit clip is a hit clip with its hits", piece.hasType (ids::HITCLIP) && piece.getNumChildren() == 1);
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
                            : k.code == taw::Key::Mouse ? juce::String (k.right ? "right" : "click") + (k.doubleClick ? "2" : "") + "@" + juce::String (k.x) + "," + juce::String (k.y)
                                                        : juce::String (names[k.code])));

            return out.joinIntoString (" ");
        };

        const std::pair<const char*, const char*> cases[] = {
            { "a", "a" }, { "\x7f", "Backspace" }, { "\r", "Enter" }, { "\t", "Tab" }, { "\x1b", "Escape" },
            { "\x1b[D", "Left" }, { "\x1b[1;2C", "shift+Right" }, { "\x1b[1;5D", "ctrl+Left" }, { "\x1b[1;3D", "alt+Left" },
            { "\x1b[1;7C", "ctrl+alt+Right" }, { "\x1b[1;6D", "ctrl+shift+Left" }, { "\x1b[1;2A", "shift+Up" },
            { "\x1b" "b", "alt+Left" }, { "\x1b" "f", "alt+Right" }, { "\x1b\x1b[C", "alt+Right" }, { "\x1b/", "alt+/" },
            { "\x1b[H", "Home" }, { "\x1b[F", "End" }, { "\x1b[1;2H", "shift+Home" }, { "\x1b[5~", "PageUp" },
            { "\x1b[6~", "PageDown" }, { "\x1b[3~", "Delete" }, { "\x1bOP", "F1" }, { "\x1bOD", "Left" },
            { "\x1a", "ctrl+z" }, { "\x19", "ctrl+y" }, { "\x03", "ctrl+c" }, { "\x16", "ctrl+v" }, { "\x1d", "ctrl+]" },
            { "]", "]" }, { "[", "[" }, { "2m", "2 m" }, { "\xc3\xa9", "\xc3\xa9" }, { "\x1b[Z", "shift+Tab" },
            // kitty keyboard protocol (iTerm2, Ghostty, kitty, WezTerm); cmd counts as alt
            { "\x1b[27u", "Escape" }, { "\x1b[91;5u", "ctrl+[" }, { "\x1b[109;5u", "ctrl+m" }, { "\x1b[32;5u", "ctrl+ " },
            { "\x1b[47;9u", "alt+/" }, { "\x1b[122;5u", "ctrl+z" }, { "\x1b[13u", "Enter" },
            // kitty's shift+return; taw's Terminal profile: ctrl+up/down
            { "\x1b[13;2u", "shift+Enter" }, { "\x1b[1;5A", "ctrl+Up" }, { "\x1b[1;5B", "ctrl+Down" },
            // SGR mouse: presses only (not releases, moves, the wheel or the middle button), cells from 0
            { "\x1b[<0;10;5M", "click@9,4" }, { "\x1b[<0;10;5m", "" }, { "\x1b[<64;10;5M", "" }, { "\x1b[<32;10;5M", "" },
            { "\x1b[<1;10;5M", "" }, { "\x1b[<6;3;2M", "shift+right@2,1" }, { "\x1b[<16;3;2M", "ctrl+click@2,1" },
            { "\x1b[<0;20;7M\x1b[<0;20;7m\x1b[<0;20;7M\x1b[<0;20;7m\x1b[<0;20;7M", "click@19,6 click2@19,6 click@19,6" },
            { "\x1b[<0;30;7M\x1b[<0;31;7M", "click@29,6 click@30,6" },
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
            for (const auto& f : engine.poll())
            {
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

    const auto cwd = juce::File::getCurrentWorkingDirectory();
    const auto target = args.isEmpty() ? cwd : cwd.getChildFile (args[0]);

    juce::File openDir, newParent;
    juce::String newName;

    if (taw::Session::existsIn (target))
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
    const bool reopen = unclean.found && openDir == juce::File() && args.isEmpty() && taw::Session::existsIn (unclean.session);

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

        status = app.run();
    }

    taw::safety::markStopped();
    taw::safety::shutdownComplete();
    return status;
}
