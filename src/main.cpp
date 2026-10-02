#include "App.h"
#include "Bounce.h"
#include "Import.h"
#include "Plugins.h"
#include "Safety.h"

#include <juce_events/juce_events.h>

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

        std::array<taw::MidiQueue, taw::MidiInputs::maxPorts> queues;
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

        taw::App app (dir, {}, {}, {});

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

            if (args[i].startsWith (":key "))
            {
                const auto name = args[i].substring (5);
                taw::Key k;
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
        std::printf ("\n%s \xc2\xb7 %d tracks \xc2\xb7 mix %s dB\n", s.name().toRawUTF8(), s.numTracks(),
                     taw::formatDb (s.tree()[taw::ids::mixDb]).toRawUTF8());

        for (int t = 0; t < s.numTracks(); ++t)
        {
            const auto track = s.track (t);
            juce::String what;

            // Sample tracks: their hits by beat. Audio tracks: each clip as start+length in samples.
            if (taw::Session::isInstrumentTrack (track))
            {
                const auto notes = taw::Session::notes (track);
                what << track[taw::ids::pluginName].toString() << ", " << (int) notes.size() << " notes:";

                for (const auto& n : notes)
                    what << " " << (int) n[taw::ids::pitch] << "@" << juce::String ((double) n[taw::ids::beat], 2) << "+" << juce::String ((double) n[taw::ids::len], 2);
            }
            else if (taw::Session::isSampleTrack (track))
            {
                std::vector<double> beats;

                for (const auto& h : taw::Session::hits (track))
                    beats.push_back ((double) h[taw::ids::beat]);

                std::sort (beats.begin(), beats.end());
                what << (int) beats.size() << " hits:";

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
            const auto routing = app.describeSends (t).fromFirstOccurrenceOf (" ", false, false).fromFirstOccurrenceOf (" ", false, false);

            if (! routing.startsWith (juce::String (L"\x2192 Mix \x00b7 no sends")))
                what << " | " << routing;

            std::printf ("  %2d %-16s in%-2d %6s dB  %s\n", t + 1, track[taw::ids::name].toString().toRawUTF8(),
                         (int) track[taw::ids::input], taw::formatDb (track[taw::ids::gainDb]).toRawUTF8(), what.toRawUTF8());
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
                                           "Down", "Home", "End", "PageUp", "PageDown", "Delete", "F1" };
            juce::StringArray out;

            for (const auto& k : keys)
                out.add (juce::String (k.ctrl ? "ctrl+" : "") + (k.alt ? "alt+" : "") + (k.shift ? "shift+" : "")
                         + (k.code == taw::Key::Char ? juce::String::charToString ((juce::juce_wchar) k.ch) : juce::String (names[k.code])));

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
        taw::AudioEngine engine;
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
        auto rec = engine.createRecorder (0, 0, file, d->getCurrentSampleRate(), err);

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
        taw::App app (openDir, newParent, newName, templateName);

        if (unclean.found)
            app.reportUncleanExit (unclean.crashLog, reopen);

        status = app.run();
    }

    taw::safety::markStopped();
    taw::safety::shutdownComplete();
    return status;
}
