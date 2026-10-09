#pragma once

#include "AudioEngine.h"
#include "Bounce.h"
#include "AudioEditor.h"
#include "Import.h"
#include "Library.h"
#include "LiveKeys.h"
#include "MidiEditor.h"
#include "Plugins.h"
#include "Console.h"
#include "Session.h"
#include "Timing.h"
#include "Ui.h"
#include "Video.h"

#include <functional>
#include <optional>
#include <map>
#include <thread>

namespace taw
{
struct Result
{
    bool ok = true;
    juce::String message;

    static Result good (juce::String m = {}) { return { true, std::move (m) }; }
    static Result fail (juce::String m) { return { false, std::move (m) }; }
};

// Meter ballistics for display. Fed with raw peaks from the engine once per frame.
struct MeterState
{
    float level = 0.0f; // falls at a fixed dB/s
    float hold = 0.0f;  // peak hold
    double holdTime = 0.0;
    bool clipped = false; // latched until cleared

    void update (float peak, double now, double dt);
};

struct ClipData
{
    std::shared_ptr<const juce::AudioBuffer<float>> audio;
    std::vector<WavePoint> overview; // one per Recorder::overviewBlock samples
};

struct LiveTake
{
    std::shared_ptr<Recorder> recorder;
    std::vector<WavePoint> overview;
    juce::int64 start = -1;
    bool finishing = false;
};

struct DeviceChoice
{
    juce::String type, output, input;
};

juce::String formatDb (double db); // "+1.5", "-6.0", "-inf"
juce::String trackList (const std::vector<int>& tracks); // "track 2", "tracks 1,3"
// The rate new sessions are made at (settings.json "newRate", all sessions): 44.1 kHz unless
// changed. The device switches to a session's rate when it opens.
double newSessionRate();
Sequence buildSequence (const juce::ValueTree& track, double samplesPerBeat); // an instrument track's notes, in samples

// What ctrl+c holds: per track, its clips (audio, MIDI or hits), trimmed to the copied range
// and positioned relative to its start, and the automation under them.
struct Clipboard
{
    // A lane's curve over the range: its points, with its values at both ends.
    struct Automation
    {
        juce::String param;
        juce::String plugin; // a plugin parameter's plugin (it pastes only onto the same plugin)
        std::vector<std::pair<double, double>> points; // beats from the range start, values
    };

    struct Lane
    {
        bool sample = false, instrument = false;
        std::vector<juce::ValueTree> items; // CLIPs (start in samples), or MIDICLIPs / HITCLIPs (beat in beats)
        std::vector<Automation> automation;
    };

    juce::int64 length = 0; // samples
    double beats = 0.0;     // the same length in beats (hits follow the tempo)
    std::vector<Lane> lanes;
};

// Ctrl+C also puts this on the system clipboard: terminals turn paste keys into pasted text, and
// this text arriving is how taw knows to paste.
inline constexpr const char* pasteMarker = "taw:paste";

// How a command line parses, without running it: "tracks 2 | verb "i" | args "2"".
juce::String explainCommand (const juce::String& line, int numTracks);

enum class Mode
{
    Main,
    DevicePicker,
    TemplatePicker
};

class App final : private juce::ValueTree::Listener
{
public:
    // openDir: an existing session to open (may be empty). Otherwise a new session is created
    // under newParent, named newName, from templateName; empty fields are asked for.
    App (juce::File openDir, juce::File newParent, juce::String newName, juce::String templateName);
    ~App() override;

    int run();

    // Headless use (taw --exec): finish background work (imports, bounces, takes) the way the
    // UI loop would.
    bool hasBackgroundWork() const { return import != nullptr || bounce != nullptr || plugins.isScanning() || ! pluginQueue.empty(); }
    void pumpBackgroundWork();
    void buildPlayback() { rebuildGraph(); } // headless: the live graph now (its plugins queue to load)
    void pressKey (const Key& k) { handleKey (k); } // as if typed (taw --exec ":key o")
    juce::String screenText (int w, int h) { return ui.snapshot (*this, w, h); }

    // Commands ----------------------------------------------------------------
    // One line of the command language. Used by the command line now and by agents later.
    Result execute (const juce::String& line);

    Result record (const std::vector<int>& tracks); // empty = whatever is armed
    Result punch(); // while playing: recording on or off, playback carries on
    juce::int64 countInSamples (juce::int64 from, bool forRecording) const;
    juce::String countInStatus() const;
    Result setCountIn (const juce::String& arg); // "count 4", "count off", "count rec", "count always"
    Result play();
    Result stop();
    Result addMarker (juce::int64 position, const juce::String& name = {}); // unnamed: numbered
    Result gotoPosition (const juce::String& spec);
    Result discardTake (int track);

    // Undo -----------------------------------------------------------------------
    juce::UndoManager* um() { return session != nullptr ? session->undo() : nullptr; }
    // Starts a new undo step. Steps with the same non-empty coalesce key less than two seconds
    // apart merge, so a volume ride on [ ] undoes in one go.
    void beginEdit (const juce::String& name, const juce::String& coalesceKey = {});
    Result undoEdit();
    Result redoEdit();
    Result setClick (const juce::String& arg); // "" toggle | on | off | rec | always | <dB>
    juce::String clickStatus() const;
    Result setLoop (const juce::String& arg); // "" = loop the selection, or toggle | on | off
    Result startBounce (const juce::StringArray& args, bool withVideo = false); // withVideo: bv, an MP4 with the picture
    // New track per file at the cursor; with target >= 0, onto that track instead (a clip, or for
    // samples, the track's new sample).
    enum class ImportAs { audio, sample, autoByLength };
    Result startImport (const juce::StringArray& paths, int targetTrack = -1, ImportAs = ImportAs::audio);

    // Sample tracks
    Result convertTrack (int track, bool toSample);
    Result writePattern (const std::vector<int>& tracks, const juce::String& steps, double stepBeats, double swing = 50.0);
    Result toggleHitAtCursor();

    // Instrument tracks: a VST3 instrument played from the MIDI controller while armed, and
    // the notes recorded on the track.
    // "inst <name>": a new instrument track (or, with tracks named, onto those). No name: the picker.
    Result loadInstrument (const std::vector<int>& tracks, const juce::String& query);
    Result loadInstrument (int track, const juce::PluginDescription&); // track -1: a new one
    Result importMidiFile (const juce::File&, int targetTrack); // import song.mid: a track per part
    Result toggleEditor (int track); // "ed": the plugin's own window

    // Inserts (Inserts.cpp): five effect slots per track, A to E, after the source (and the
    // instrument), before the sends and the fader.
    Result editInserts (const std::vector<int>& tracks, const juce::StringArray& args); // "2 fx b valhalla", "2 fx b x"
    Result loadEffect (int track, int slot, const juce::PluginDescription&);
    Result removeInsert (int track, int slot);
    Result toggleInsertEditor (int track, int slot);
    Result togglePluginHere();  // |: the plugin under the cursor (instrument, insert, or a picker)
    void closePluginWindows(); // their state written first
    Result swapInserts (int track, int slot, int other);
    Result toggleBypass (int track, int slot);
    juce::String insertProblem (const juce::ValueTree& insert) const; // why it isn't loaded

    // Low latency mode ("lowlat"): inserts with more latency than the session's limit sit out, so
    // the mix isn't held back while you record. Not saved: it's for now, not the mix.
    bool lowLatency = false;
    Result setLowLatency (const juce::String& arg);
    bool insertSittingOut (const juce::ValueTree& insert) const;

    // Where the playhead is in what you hear: delay compensation makes the mix that much later
    // than what the engine is rendering. For drawing it and for markers dropped while playing.
    int laneColumns() const
    {
        const int width = console.width() - layout::browserDock (console.width(), browserVisible);
        return layout::laneWidth (width, ioVisible, session != nullptr && session->trackHeight() > 1);
    }

    juce::int64 heardPlayhead() const
    {
        const auto p = engine.playhead();
        return engine.isRolling() && mixLatency > 0 ? juce::jmax (playFrom, p - mixLatency) : p;
    }
    juce::int64 playFrom = 0; // where the engine last started playing (the loop start, with the loop on)

    // Opening a session from the menu, a few steps a frame so the screen can show progress.
    struct Opening
    {
        struct Step
        {
            juce::String kind, name; // "audio" / "plugin", and what
            std::function<void()> run;
        };

        std::vector<Step> steps;
        size_t done = 0;
    };
    std::unique_ptr<Opening> opening;
    void startOpening (const juce::File& dir);
    // import set.als: a new session from an Ableton Live set, beside this one, then opened.
    Result importAbletonSet (const juce::File& als);
    juce::File openAfterClose;      // closing, then opening this (the session an import made)
    juce::String openAfterCloseNote; // ...and what to say when it does
    void stepOpening();

    // Closing (close, or quitting with a session open), likewise.
    struct Closing
    {
        bool thenQuit = false;
        bool save = false; // the session and plugin states to disk; otherwise the changes are dropped
        size_t done = 0, total = 0;
        juce::String current; // the plugin being saved
        std::unique_ptr<Graph> bounceGraph; // last-saved.mp3: the closed session's own plugins play it
        juce::int64 bounceEnd = 0;
        juce::File bounceFile;
    };
    std::unique_ptr<Closing> closing;
    void startClosing (bool thenQuit, bool save);
    void stepClosing();
    Result finishClosing (std::optional<bool> saved = {}); // saved: a close that asked (true: it saved)

    // Saving is something you do: edits wait in the session's unsaved\ folder (Session::workFolder)
    // until save, so session.json's date says when you last worked on it, not when you opened it.
    // Closing with changes asks: save (and bounce last-saved.mp3) or drop them.
    bool unsaved = false;        // edits since the last save
    bool savedThisVisit = false; // saved since it was opened: closing bounces last-saved.mp3
    bool hasUnsavedChanges();    // edits, or a plugin tweaked in its window since the save
    Result saveSession();        // "save", ctrl+s
    struct ClosePrompt { bool thenQuit = false; };
    std::optional<ClosePrompt> closePrompt; // "save changes?" up
    void requestClose (bool thenQuit);
    bool handleClosePromptKey (const Key&);
    // Writes go to unsaved\ while a person is driving (headless runs save as they go, as before).
    juce::File pluginStateFile (const juce::String& path, bool forWriting) const;
    juce::File newPluginStateFile (const juce::String& base) const;

    // Bounces that carry on after their session closed (last-saved.mp3), so you're back at the
    // session screen at once. Quitting waits for them.
    std::vector<std::unique_ptr<Bounce>> backgroundBounces;
    std::vector<juce::String> backgroundBounceNames; // whose, for the screen
    bool quitAfterBounces = false;
    void pollBackgroundBounces();

    // The session screen's areas: templates (a new session) on top, then recent sessions and
    // the bounces taw made side by side (newest first, to audition without opening anything).
    // Arrows move within the focused area; ctrl+arrows (or tab) move between areas.
    enum class PickArea { templates, sessions, bounces };
    PickArea pickArea = PickArea::sessions;
    int templateIndex = 0, sessionIndex = 0;
    struct RecentBounce { juce::File file; juce::String session; juce::File folder; juce::Time time; };
    std::vector<RecentBounce> recentBounces;
    int bounceIndex = 0;
    void loadRecentBounces();
    void movePickArea (PickArea to);
    void handleBouncesKey (const Key&); // the bounces area's own keys (play, seek, open, reveal)
    // The bounce playing on the session screen: decoded on a thread, then played by the
    // engine's preview voice from any point.
    struct BouncePlayer
    {
        juce::File file;
        std::shared_ptr<juce::AudioBuffer<float>> audio; // at the device's rate, once loaded
        std::thread loader;
        std::atomic<bool> loaded { false };
        juce::String error;    // set before loaded
        double rate = 44100.0; // the device's, what audio is at
        double startAt = 0.0;  // seconds, once it's loaded
        bool started = false;
        ~BouncePlayer() { if (loader.joinable()) loader.join(); }
    };
    std::unique_ptr<BouncePlayer> bouncePlayer;
    void playBounce (const juce::File&);
    void stopBounce();
    void seekBounce (double seconds);
    double bouncePlaySeconds() const; // where it's playing; < 0 when it isn't
    double bounceLengthSeconds() const;
    Result scanPlugins (bool retryFailed); // "scan", "scan retry"
    // "midi": the MIDI inputs panel. Each input on or off (kept in settings.json), whether it
    // opened, and the last message it sent, to see a controller is getting through.
    Result toggleMidiPanel();
    bool midiPanelVisible = false;
    int midiPanelIndex = 0;
    bool handleMidiPanelKey (const Key&);

    // The sample browser (ctrl+B, browse): the library's kinds as badges, a map of the chosen
    // kind where samples that sound alike sit together, and a list; typing filters by name, and
    // every move auditions. Enter gives the focused sample track that sample (it keeps its hits)
    // or makes a new one. index <folder> adds folders to the library. See Browser.cpp.
    enum class BrowserFocus { map, xAxis, yAxis, kinds, list }; // top to bottom (no list now)
    // Where the Ui drew the browser, for the mouse and for moving on the map.
    struct BrowserGeometry
    {
        int left = 0, top = 0, rows = 0;               // the column
        int listX = 0, listY = 0, listW = 0, listRows = 0, listFirst = 0;
        int mapX = 0, mapY = 0, mapW = 0, mapH = 0;    // cells (two pixels each, top and bottom)
        int axisRow[2] = { -1, -1 };                   // the x and y selector rows
        struct Badge { int x, y, w; };
        std::vector<Badge> badges;                     // they wrap onto as many rows as they need
        bool valid = false;
    } browserGeometry;

    // What the map's axes show (0, "similar": the sound-alike map on both). Kept in settings.json;
    // alt+arrows step them.
    static const juce::StringArray& browserAxisNames();
    int browserAxisX = 0, browserAxisY = 0;
    juce::String browserAxisRange (bool y) const; // "51 Hz → 12 kHz" over the kind shown
    std::pair<juce::String, juce::String> browserAxisEnds (bool y) const; // low, high
    void setBrowserAxis (bool y, int step);
    bool browserActive = false; // the keys go to the browser (else it's just showing)
    Result focusBrowser();      // ctrl+B from the tracks
    Result showBrowser (bool show); // browse, and ctrl+B from inside it: docked or hidden
    bool handleBrowserKey (const Key&);
    std::vector<juce::String> browserKinds() const;     // "all", then the kinds the library has
    std::vector<int> browserShown() const;              // the samples the badge and filter leave, list order
    std::pair<float, float> browserPosition (int sample) const; // on the map now showing, 0..1
    Result startIndexing (const juce::StringArray& folders);    // index <folder>...; index remove <folder>...
    Result removeFromLibrary (const juce::StringArray& folders); // index remove
    Result describeLibrary();                                    // library: its folders and how many samples each
    void pollIndexing();
    bool browserVisible = false;
    BrowserFocus browserFocus = BrowserFocus::kinds;
    int browserKind = 0, browserSample = -1;
    juce::String browserFilter;
    double auditionDb = -6.0; // the browser's audition voice, its own level (settings.json)
    Result setAuditionDb (double db);
    std::unique_ptr<Library> library;
    std::atomic<int> indexDone { 0 }, indexTotal { 0 };
    bool isIndexing() const { return indexThread.joinable(); }

    // A track whose sample the browser just swapped: its header flashes for a moment.
    void flashTrack (const juce::ValueTree& t) { flashedTrack = t; flashUntil = juce::Time::getMillisecondCounterHiRes() + 1200.0; }
    bool trackFlashing (const juce::ValueTree& t) const { return t == flashedTrack && juce::Time::getMillisecondCounterHiRes() < flashUntil; }
    juce::ValueTree flashedTrack;
    double flashUntil = 0.0;
    // Routing (Routing.cpp): bus tracks, and each track's output and five sends (A to E).
    // Buses are named in commands by track number or name; a name nothing matches makes a new bus.
    Result addBus (const juce::String& name);                                         // "bus Verb"
    Result setOutput (const std::vector<int>& tracks, const juce::String& where);      // "2 out verb", "2 out mix"
    Result editSend (const std::vector<int>& tracks, const juce::StringArray& args);   // "2 send b verb -6 pre"
    Result setOutputBus (int track, int busId);                   // busId 0: the mix
    Result setSendBus (int track, int slot, int busId);
    Result nudgeSend (int track, int slot, double deltaDb);
    juce::String describeSends (int track) const;
    juce::String describeIoCell (int track, int cell) const;
    juce::String busName (int busId) const; // "6 Verb", or "Mix" for 0
    std::vector<int> busFeeders (int busTrack) const; // tracks whose output or a send goes to it

    // The routing column, right of the timeline ("io" shows or hides it; settings.json). o steps
    // into it on the focused track: left/right pick the output or a send, enter picks its bus.
    bool ioVisible = true;
    int ioCell = -1; // -1: not in the column; else one of these
    static constexpr int ioFirstInsert = 0, ioFirstSend = 5, ioOutput = 10;
    // Whose column you're in: the focused track, or -1 for the mix (its inserts only). Insert
    // functions take -1 for the mix too.
    int ioTrack() const { return mixFocused() ? -1 : focused; }
    juce::String insertOwnerName (int track) const; // "track 2", "the mix"

    // The bus picker: where the output or a send goes. Type to filter (a new name makes a bus).
    struct BusChoice
    {
        juce::String label;
        int busId = 0; // 0: the mix, -1: a new bus
    };
    bool busPickerVisible = false;
    juce::String busFilter;
    int busPickIndex = 0, busPickTrack = -1, busPickCell = 0;
    std::vector<BusChoice> busChoices() const;
    void openBusPicker (int track, int cell);

    // The track menu (shift+T): what can be done to the focused or selected tracks.
    struct TrackMenuItem
    {
        juce::juce_wchar key;
        juce::String label, typed; // typed: the command it stands for
        std::function<Result()> run;
        bool danger = false;
    };
    bool trackMenuVisible = false;
    int trackMenuIndex = 0;
    std::vector<int> trackMenuTracks;
    std::vector<TrackMenuItem> trackMenuItems();
    Result openTrackMenu();
    bool handleTrackMenuKey (const Key&);
    Result removeTracks (std::vector<int> tracks); // "2 remove"
    Result moveTracks (int direction);             // alt+up/down: the selected (or focused) tracks, one place

    PluginSlot* instrumentFor (int track); // the loaded plugin, or nullptr
    juce::String instrumentProblem (int track) const; // why it isn't loaded
    void openPluginPicker (int targetTrack, int effectSlot = -1); // effectSlot >= 0: effects, for that insert
    PluginCatalog plugins;
    std::unique_ptr<MidiInputs> midiInputs;

    // Keys mode (ctrl+k): the typing keyboard plays the armed instrument tracks (LiveKeys.h).
    LiveKeys liveKeys { [this] (MidiMsg m) { engine.playTyped (m); }, [this] (wchar_t ch) { return console.keyDown (ch); } };
    Result toggleLiveKeys();

    // While recording: notes whose key is still down, drawn from where they started to the
    // playhead. Written to the track as NOTEs when the key comes up.
    struct HeldNote
    {
        int channel, pitch, vel;
        double beat;   // where it goes (input quantize may have moved it)
        double played; // where it was played: its length runs from here
    };
    std::map<int, HeldNote> heldNotes; // by channel * 128 + pitch
    std::vector<int> midiRecordTracks;
    double midiPassStart = 0.0;                  // beats: where this recording pass began
    std::map<int, juce::ValueTree> midiPassClips; // by track: the clip this pass records into (made on its first note or hit)

    // "f": the focused track fills the track area (instrument tracks become a piano roll).
    bool fillView = false;

    // The instrument picker ("inst" alone): type to filter, up/down, enter loads, esc closes.
    bool pluginPickerVisible = false;
    juce::String pluginFilter;
    int pluginPickIndex = 0, pluginPickTarget = -1, pluginPickSlot = -1;
    bool pluginPickMono = false; // effects for a mono track
    std::vector<juce::PluginDescription> pickerChoices() const;
    Result returnPlayhead();
    Result setGridDivision (int noteValue);

    // Editing over the time selection (Editing.cpp). Clips move, trimmed to the selection.
    Result copySelection (const std::vector<int>& tracks);
    Result cutSelection (const std::vector<int>& tracks);
    Result deleteSelection (const std::vector<int>& tracks);
    Result duplicateSelection (const std::vector<int>& tracks); // right after itself
    Result paste (int firstTrack); // at the cursor, lanes on consecutive tracks
    Result selectRange (const juce::String& from, const juce::String& to); // "sel 3 5": bars 3 to 5
    Result selectClipAtCursor(); // "sel clip": the focused track's clip under the cursor
    // Consolidate (alt+shift+3, as in Pro Tools): what plays in the selection becomes one new
    // clip per track: MIDI and hits gathered into one clip, audio rendered to a new file.
    Result consolidate (const std::vector<int>& tracks);
    // A hit at a timeline beat on a sample track: into the clip there, this record pass's clip,
    // or a clip for its bar (growing the one just before when it reaches the bar).
    void addHit (int track, double beat, float vel);
    Clipboard clipboard;

    static juce::StringArray splitPaths (const juce::String& text);
    static juce::String expandHome (const juce::String& path); // ~/x -> /Users/you/x (not on Windows)
    bool isBouncing() const { return bounce != nullptr; }
    Result revealBounces (const juce::String& arg); // "bounces": the folder in the file explorer; "bounces on/off": after each bounce
    juce::File lastBounce;                          // this session's latest finished bounce
    bool interactive = false;                       // run() is driving (not taw --exec): may open other windows
    const Bounce* currentBounce() const { return bounce.get(); } // for the progress bar; nullptr when idle
    void openDevicePicker();
    Result showControlPanel();
    // Quitting closes the session first (asking about unsaved changes): on screen, in steps
    // under the closing box.
    void quit()
    {
        if (session != nullptr && console.isOpen() && closing == nullptr)
            requestClose (true);
        else if (closing == nullptr && ! backgroundBounces.empty() && console.isOpen())
            quitAfterBounces = true;
        else if (closing == nullptr)
            quitRequested = true;
    }
    Result closeSession(); // close (asking about unsaved changes), back to the session screen
    // decided: the close already saved or dropped the changes. Otherwise (headless, or the
    // window closed on taw) it saves: to session.json headless, to unsaved\ with a person.
    void releaseSession (bool decided = false);
    // Tells you the last run didn't end cleanly, ahead of anything opening the session said.
    void reportUncleanExit (const juce::File& crashLog, bool reopened);

    // Navigation ---------------------------------------------------------------
    enum class Step { Fine, FineLarge, Normal, Large };
    void nudge (int direction, Step, bool extendSelection = false);
    // Moves the cursor (and with extendSelection, the selection's end). Never the playhead.
    void moveTo (juce::int64, bool extendSelection = false);
    void setCursor (juce::int64);
    void focusTrack (int index, bool extendSelection = false); // index == numTracks focuses the mix
    void focusUp (bool extendSelection);   // past the first track: the bar row, then markers
    void focusDown (bool extendSelection);
    // The grid lines on screen at this zoom: bars (or groups of bars), and the finest shown
    // division under them (0 when there's none). The arrows step by the finest one.
    struct GridSpacing { double bars = 0.0, sub = 0.0; double finest() const { return sub > 0.0 ? sub : bars; } };
    GridSpacing gridSpacing() const;
    // A step for hits and alt+arrows, in beats: the grid division ("grid 16"), or the finest line
    // on screen when you've zoomed in past it, so a hit lands on the line the cursor is on.
    double hitStepBeats() const;

    // The pad key (.): plays every armed sample track now and, while recording, writes a hit on
    // each where you heard it. Returns false when no sample track is armed (the key types).
    // A held key is one hit: its repeats are swallowed (repeat: true), not played.
    bool playPads (float velocity, bool repeat = false);
    Result quantizeHits (const juce::String& arg, const std::vector<int>& tracks); // "snap [percent]" / "snap input"
    void zoom (int delta);
    void showSelectionEdge (bool end);
    Result moveToNextClipEdge (bool extendSelection);
    Result stepMarker (int direction, bool extendSelection); // markers row: arrows hop marker to marker
    void clearSelection() { clearTimeSelection(); trackAnchor = -1; }
    void clearTimeSelection() { timeAnchor = timeHead = -1; }

    // Selection: a time range and/or a range of tracks, made with shift + arrows. The time
    // range has its own two ends, independent of the cursor and the playhead.
    bool mixFocused() const;
    bool loopActive() const; // loop on with a real range: play starts at its start
    bool hasTimeSelection() const { return timeAnchor >= 0 && timeHead >= 0 && timeHead != timeAnchor; }
    juce::Range<juce::int64> timeSelection() const;
    std::vector<int> selectedTracks() const; // empty when no track selection
    std::vector<int> defaultTargets() const; // selection, else the focused track; every track from a top lane
    // Rows above the tracks you can move onto with up: there, time selections cover every track.
    enum class TopLane { none, ruler, markers, video }; // video: under the ruler, while there's a video
    TopLane topLane = TopLane::none;
    bool onTopLane() const { return topLane != TopLane::none; }

    // Volume: tracks and the mix
    Result nudgeVolume (double deltaDb); // on the selection, focused track, or mix
    Result setMixVolume (double db);

    // Formatting helpers (shared with the UI)
    juce::String formatBars (juce::int64 pos) const;
    juce::String formatTime (juce::int64 pos) const;
    double sampleRate() const;
    double samplesPerColumn() const;

    // State read by the UI --------------------------------------------------------
    Console console;
    AudioEngine engine;
    std::unique_ptr<Session> session;

    Mode mode = Mode::Main;
    std::vector<DeviceChoice> deviceChoices;
    std::vector<Template> templates;
    std::vector<juce::File> recent; // session picker: recent sessions, then templates
    int pickIndex = 0;
    juce::String newSessionName;
    juce::File newSessionParent;
    bool dateSessionNames = true; // a new session's folder is "{name} mm.dd.yy" (settings.json)
    double newRate = 44100.0;     // new sessions' sample rate (settings.json)
    static juce::String datedName (const juce::String& name); // name plus today's date, unless it ends in one

    juce::int64 cursor = 0, playStart = 0, viewStart = 0;
    int focused = 0, trackScroll = 0, zoomLevel = 0;
    juce::int64 timeAnchor = -1; // where a shift-selection in time started
    juce::int64 timeHead = -1;   // its moving end
    int trackAnchor = -1;        // where a shift-selection of tracks started
    bool recording = false, helpVisible = false;
    // Help is in pages (tabs), so it fits a small screen: left/right, tab or 1-9 turn them.
    int helpPage = 0;
    static constexpr int helpPages = 7;
    bool handleHelpKey (const Key&);

    // "project": the session's settings in a panel like help. Up/down choose, left/right change,
    // enter types the setting's command (bpm ...) or flips a switch; typed commands work too.
    struct Setting
    {
        juce::String name, value;
        juce::String command; // enter puts this on the line to type a value ("bpm "); empty: enter steps it
        bool editable = true;
    };
    bool settingsVisible = false;
    int settingsIndex = 0;
    std::vector<Setting> projectSettings() const;
    Result stepSetting (int index, int direction);
    bool handleSettingsKey (const Key&); // true when the panel used the key

    // App-wide settings, kept in settings.json next to the device settings. (Track height is
    // per session: Session::trackHeight.)
    juce::String theme = "dark"; // "dark", "light" or "grey": the commands of the same names
    juce::StringArray midiOff;   // MIDI inputs switched off, by name
    void loadSettings();
    void saveSettings() const;

    juce::String commandLine, message;

    // Tap tempo: with "bpm " typed (and no number, or a tapped one), t taps; enter sets it.
    bool tappingTempo() const;
    void tapTempo();
    std::vector<double> tapTimes; // ms, the recent taps
    juce::String tappedLine;      // what the last tap wrote
    bool messageIsError = false;
    juce::ValueTree namingMarker; // after ctrl+k the command line names this marker: enter keeps, esc leaves the number
    juce::ValueTree selectedMarker; // on the markers row, the one the arrows landed on (enter renames, del deletes)

    std::vector<LiveTake> live;
    std::map<juce::String, ClipData> clips;

    std::array<MeterState, AudioEngine::maxInputs> inputMeters;
    std::array<MeterState, AudioEngine::maxTracks> trackMeters;
    std::array<MeterState, 2> masterMeters;

private:
    void handleKey (const Key&);
    void handleMainKey (const Key&);
    void handleMouse (const Key&);
    // The MIDI or hit clip on top in [from, to) samples on a track, or invalid.
    juce::ValueTree midiClipAt (int track, double from, double to) const;
    void handleDevicePickerKey (const Key&);
    void handleTemplatePickerKey (const Key&);

    int defaultTemplateRow() const;
    void startupDevice();
    void proceedToSession();
    Result chooseDevice (const DeviceChoice&);
    Result createSession (const Template&);
    void attachSession (std::unique_ptr<Session>);
    void matchDeviceSampleRate();

    Result startRecorders (const std::vector<int>& tracks);
    juce::int64 heardPosition (juce::int64 playhead); // minus output latency, when compensating
    void drainRecordedMidi();
    void finishHeldNotes (juce::int64 at);
    void recordMidiNode (juce::ValueTree node, double from, double to); // into this pass's clip on each recording track
    bool handlePluginPickerKey (const Key&);
    bool handleBusPickerKey (const Key&);
    bool handleIoKey (const Key&);

    // Routing internals (Routing.cpp)
    void applyRouting (Graph&, bool offline); // outputs, sends, the order tracks run in, delay compensation, solo through buses
    void compensateLatency (Graph&, bool offline);
    struct KeptDelay
    {
        juce::ValueTree track;
        int edge; // -1: its output; else the n-th send
        std::shared_ptr<RouteDelay> line;
    };
    std::vector<KeptDelay> routeDelays; // the live graph's, reused while a route's delay stays the same

public:
    int mixLatency = 0; // samples the plugins make the mix late by (delay compensation)
    bool trackIsStereo (const juce::ValueTree& track); // its width: set (2 mono / 2 stereo), else from what it plays
    Result setPan (const std::vector<int>& tracks, const juce::String& value); // "2 pan -30", "2 pan l30", "2 pan c"
    Result nudgePan (const std::vector<int>& tracks, double delta);           // -1..1 units
    Result setWidth (const std::vector<int>& tracks, bool stereo);            // "2 mono", "2 stereo"
    static juce::String panText (double pan);                                 // "C", "<30", "30>"

private:
    std::vector<int> routesFrom (int track, bool withMutedSends) const; // bus tracks it feeds
    bool feeds (int from, int to) const;  // from reaches to through outputs and sends
    Result checkRoute (int track, int busId) const;
    int makeBus (const juce::String& name, juce::String& error); // a new bus track's busId, 0 on failure
    int resolveBus (const juce::String& query, juce::String& made, juce::String& error); // busId; may make one

    // Live plugin instances: instruments (the node is the track) and effects (an INSERT). Found
    // by tree identity, so they follow tracks around and come back with undo.
    struct Hosted
    {
        juce::ValueTree node;
        std::shared_ptr<PluginSlot> slot;
        juce::String problem; // why it didn't load
        juce::MemoryBlock savedState; // what's in its state file, to skip rewriting it
        bool touched = false;         // its window opened (or a controller set it): it may differ from the save
    };
    std::vector<Hosted> hosted;
    Hosted* hostedEntry (const juce::ValueTree& node); // loads it the first time
    Hosted* liveEntry (const juce::ValueTree& node);   // for the live graph: not loaded yet, it's queued (nullptr meanwhile)

public:
    std::vector<juce::ValueTree> pluginQueue; // plugins still to load after opening (one a frame)
    bool pluginPending (const juce::ValueTree& node) const;
    bool loadNextPlugin(); // the most wanted one; false when none are waiting

private:
    std::shared_ptr<PluginSlot> createPlugin (const juce::ValueTree& node, double rate, int block, juce::String& problem);

public:
    // The plugin being loaded right now (the screen shows it; it's blocked until it's in). Also
    // anything else that blocks the screen for a while (the audio device opening or changing
    // rate), titled loadingTitle when it isn't "LOADING".
    juce::String loadingPlugin, loadingWhy, loadingTitle;
    void showWaiting (const juce::String& title, const juce::String& what, const juce::String& why); // and draws it now
    void doneWaiting() { loadingPlugin.clear(); loadingWhy.clear(); loadingTitle.clear(); }
    // After the device changes rate its driver can ask to be restarted, which blocks for seconds
    // more: the box stays up until that has happened (or this time has passed).
    double deviceSettleUntil = 0.0;

private:
    void savePluginStates();
    void writePluginState (Hosted&);
    void pollPlugins(); // closed editor windows, device changes, periodic state saves
    void dropPlugin (const juce::ValueTree& node); // its state saved, its window closed
    void addInserts (TrackPlay&, const juce::ValueTree& track, bool offline);
    void followWindow (int track, int slot); // a plugin window is open: show this insert's instead
    int preparedGeneration = -1;
    int lastLatencies = 0; // pollPlugins: plugin latencies last seen (a change rebuilds the graph)
    double lastStateSave = 0.0;
    void stopRecorders();
    void collectFinishedTakes();
    void drainLiveOverviews();
    // Editing internals (Editing.cpp)
    double snapBeats (juce::int64 samples) const; // to beats, on a grid step if within half a sample
    bool hitInRange (double beat, double from, double to) const;
    Clipboard gather (const std::vector<int>& tracks, juce::Range<juce::int64>) const;
    int clearRange (int track, juce::Range<juce::int64>); // returns how many clips/hits it touched
    int clearTracks (const std::vector<int>& tracks, juce::Range<juce::int64>); // ...and their automation there
    Result pasteAt (const Clipboard&, const std::vector<int>& destTracks, juce::int64 at);
    Result checkEditable (const std::vector<int>& tracks, bool needsRange) const;
    juce::Result parsePosition (const juce::String& spec, juce::int64& pos) const; // "17", "17.3", "1:30"

    // Puts a finished take on its track as a clip. comp: latency compensation in samples.
    void placeTake (int trackIndex, const juce::File&, juce::int64 start, juce::int64 length, juce::int64 comp);
    juce::int64 latencyCompensation();

    // The take journal (recording.json in the session) lists takes still being written, so a
    // crash can't lose them: the next open repairs their files and puts them on their tracks.
    void writeTakeJournal();
    void recoverTakes();
    const ClipData* clipFor (const juce::ValueTree& clip);

public:
    const ClipData* audioFor (const juce::String& sessionRelativePath); // loads and caches
    void say (const Result&); // the message line

    // The editors (MidiEditor.h, AudioEditor.h): a window for the focused track's clips, one
    // open at a time. Keys they pass on wait here for the main loop, so a key that closes the
    // session never runs inside an editor.
    std::unique_ptr<MidiEditor> midiEditor;
    std::unique_ptr<AudioEditor> audioEditor;
    std::vector<Key> deferredKeys;
    Result openMidiEditor (const juce::ValueTree& clip);
    Result openAudioEditor (const juce::ValueTree& clip);
    Result toggleEditor();         // shift+E / "edit": the focused track's clip at the cursor, or the nearest
    Result newMidiClipForEditor(); // "edit new": an empty MIDI clip to draw into
    void* editorWindow() const;    // the open editor's window, which keeps the keyboard

    // The picture (Video.h): the session's video in a window of its own, following the playhead.
    // syncVideo keeps it in step with the session every frame (opens, swaps, closes, shows it),
    // so undo and opening sessions need nothing else.
    std::unique_ptr<VideoPlayer> video;
    Result videoCommand (const juce::StringArray& args, const juce::String& rest);
    juce::File videoFile() const; // the session's, resolved; none: no video
    juce::String videoStartText() const;
    bool videoLane() const { return video != nullptr; } // the video row shows (under the ruler)
    juce::int64 videoStartPos() const;                 // where its first frame is, samples
    juce::int64 videoLength() const;                   // how long it is, samples
    double samplesPerFrame() const;
    juce::String videoTimecode (juce::int64 pos) const; // the video's own time there: m:ss:ff ("-" before it starts)
    juce::int64 frameStart (juce::int64 pos) const;     // the first sample showing the frame at pos
    Result stepFrame (int direction, bool second, bool extendSelection); // the video row's arrows
    bool scrubbing = false; // a click on the video row: dragging moves the cursor (and the picture)

private:
    // What the decoder thread's clock reads (it can't touch the session): set each frame.
    struct VideoClock
    {
        std::atomic<juce::int64> start { 0 }, lateBy { 0 }, playFrom { 0 };
        std::atomic<double> rate { 44100.0 }, offset { 0.0 }; // offset: seconds shown late
    };
    VideoClock videoClock;
    juce::String videoFailed; // the file that wouldn't open, so it isn't tried every frame
    bool videoClosed = false; // its close button hid it (until "video", or the next open)
    void syncVideo();
    double videoSeconds(); // where the picture is (the clock, on the decoder thread)

    void rebuildGraph();
    std::unique_ptr<Graph> buildGraph (bool offline = false); // offline: its own plugin instances, for a bounce
    bool handOffPlugins = false; // an offline graph takes the live plugins themselves (closing: they're going anyway)
    void writeLane (const juce::ValueTree& owner, const juce::String& param, std::vector<std::pair<double, double>> points);
    Result automationTrack (int& track, juce::String& param); // the focused track and the lane you're on

public:
    // Automation (AutomationEdit.cpp). An owner is a track, or the session for the mix (-1).
    juce::ValueTree automationOwner (int track) const;
    // Lanes: a track's AUTO lanes show under it while it's open (ids::lanes); focusedLane is
    // the one you're on under the focused track (-1: the track's own row).
    int focusedLane = -1;
    std::vector<juce::String> lanesShown (int track) const; // params, in order (empty: closed)
    std::vector<juce::String> laneList (const juce::ValueTree& track) const; // ...open or not
    void setLaneList (juce::ValueTree track, const std::vector<juce::String>&);
    Result changeLane (int track, int index, const juce::String& param); // that lane shows param instead
    juce::String focusedLaneParam();
    Result showLane (int track, const juce::String& param);
    Result removeLane (int track, const juce::String& param);
    Result laneByName (int track, const juce::String& what); // "lane cutoff"
    Result toggleLanes();                                    // shift+A
    Result toggleLaneOff();                                  // shift+O on a lane
    static bool laneOff (const juce::ValueTree& lane) { return lane[ids::bypass]; } // its points ignored
    // load false (drawing): a plugin not loaded yet isn't loaded for this; names and values fall back.
    PluginSlot* automationPlugin (const juce::ValueTree& owner, const AutomationTarget&, bool load = true);
    double staticValue (const juce::ValueTree& owner, const juce::String& param, bool load = true); // with no automation
    double automationValue (const juce::ValueTree& owner, const juce::String& param, juce::int64 pos);
    double currentValue (const juce::ValueTree& owner, const juce::String& param); // what it is now (readouts)
    bool isAutomated (const juce::ValueTree& owner, const juce::String& param) const; // points, or a knob moving it
    juce::String automationName (const juce::ValueTree& owner, const juce::String& param, bool load = true);
    juce::String automationText (const juce::ValueTree& owner, const juce::String& param, double value, bool load = true);
    // Its lane, in samples. A lane that's off plays no points (drawing asks for them anyway).
    Curve automationCurve (const juce::ValueTree& owner, const juce::String& param, bool evenIfOff = false) const;
    bool parseAutomationValue (const juce::String& param, const juce::String& text, double& out) const;
    Result setAutomation (const juce::String& value); // pt <value>
    Result setAutomationValue (int track, const juce::String& param, double value);
    Result nudgeAutomation (int direction);          // [ ] on a track showing automation
    Result clearAutomation();                        // delete on one

    // Automation follows clip edits: copied, cut, pasted and moved with the clips over it
    // (ranges in beats). Clearing keeps the curve outside the range as it was.
    std::vector<Clipboard::Automation> gatherAutomation (const juce::ValueTree& track, double from, double to) const;
    void clearAutomationRange (const juce::ValueTree& track, double from, double to);
    void pasteAutomation (const juce::ValueTree& track, const std::vector<Clipboard::Automation>&, double at, double length);
    void moveAutomation (const juce::ValueTree& track, double from, double length, double to); // a dragged clip's
    // While writing, what a controller has written this pass on the lane at pos (false: nothing there).
    bool writtenValue (const juce::ValueTree& lane, juce::int64 pos, double& value);

    // Controllers (MIDI CC) mapped to automation lanes: a lane's num (and ch, absent: any
    // channel) say which. A mapped controller always moves its parameter (touch: while it
    // moves, then back to the curve); with write on, playing, its moves are recorded.
    Result learnControl (const juce::String& arg); // map (the next controller moved) | map 74 | map off
    Result unmapControl (const juce::String& arg); // unmap (all) | unmap 74 | unmap lane (the one you're on)
    juce::StringArray freeControl (int cc, int ch, const juce::ValueTree& keep); // off every lane but keep
    Result setWrite (const juce::String& arg);     // aw | aw on | aw off (shift+W)
    bool writeOn = false; // not saved: arming automation is for now

    // The automation list ("auto"): everything the focused track can automate, with what's
    // automated and mapped already; typing filters, enter shows it on the track.
    struct AutoRow
    {
        juce::String param, name; // a header row: name only
        bool header = false;
    };
    bool autoPanelVisible = false;
    int autoPanelTrack = -1, autoPanelIndex = 0;
    int autoPanelLane = -1; // opened from this lane: enter changes what it shows (-1: adds a lane)
    juce::String autoPanelFilter;
    std::vector<AutoRow> autoPanelRows; // as filtered
    Result openAutoPanel();
    void buildAutoPanelRows();
    bool handleAutoPanelKey (const Key&);
    bool isTouched (const juce::ValueTree& lane) const;
    juce::String controlText (const juce::ValueTree& lane) const; // "CC 74", "CC 74 ch 2"

private:
    struct Touch
    {
        juce::ValueTree lane, owner;
        std::shared_ptr<LiveValue> live;
        double lastMs = 0.0;
        std::vector<std::pair<juce::int64, float>> written; // this pass, while writing (timeline samples)
    };
    std::vector<Touch> touches;
    mutable std::vector<std::pair<juce::ValueTree, std::shared_ptr<LiveValue>>> liveValues;
    std::shared_ptr<LiveValue> liveValueFor (const juce::ValueTree& lane) const;
    struct Learning
    {
        int track = -2; // -2: not learning; -1: the mix
        juce::String param;
    } learning;
    void pollControls();
    void handleControl (const ControlMsg&);
    void releaseTouches (bool idleOnly = false); // into the lanes what was written; idleOnly: the ones let go of
    void commitTouch (Touch&);
    void keepAsSetting (const Touch&);
    void syncControlMap();
    double controlToValue (const juce::String& param, int cc) const;
    // The automated parameters of a plugin on the track: its "inst/" or "fx a/" lanes (prefix).
    void addParameterCurves (TrackPlay&, const juce::ValueTree& track, const std::shared_ptr<PluginSlot>&, const juce::String& prefix) const;
    bool cursorMovedWhileRolling = false;
    double cursorMovedAt = 0.0; // ms; the view stops following the playhead briefly after
    void pollBounce();
    void pollImport();
    void finishDrop();
    void pasteFromKey(); // ctrl+v, however the terminal delivered it
    bool dropPending = false;
    juce::String dropBuffer;
    void updateMeters (double dt);
    void keepInView (juce::int64 pos);

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { treeDirty = unsaved = true; }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { treeDirty = unsaved = true; }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { treeDirty = unsaved = true; }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override { treeDirty = unsaved = true; }
    void persist(); // after an edit: into unsaved\ (or session.json, headless)

    Ui ui;
    juce::String lastEditKey;
    double lastEditTime = 0.0;
    std::unique_ptr<Bounce> bounce;
    std::unique_ptr<Import> import;
    juce::int64 importAt = 0;
    int importTarget = -1;
    ImportAs importAs = ImportAs::audio;
    juce::String pendingTemplate;
    bool treeDirty = false, quitRequested = false;

    void browserPick (int sample); // select and audition

    // Each sample's rank on the chosen axes, within the kind shown (rebuilt when either changes).
    struct AxisCache
    {
        const Library* library = nullptr;
        size_t count = 0;
        int kind = -1, x = -1, y = -1;
        std::vector<float> rankX, rankY; // by sample index, 0..1
        juce::String lowX, highX, lowY, highY; // the values at the ends, written out
        juce::String filter;
    };
    mutable AxisCache axisCache;
    const AxisCache& axes() const;
    void loadLibrary();
    std::thread indexThread;
    std::atomic<bool> indexFinished { false };
    juce::String indexSummary; // written by the thread before indexFinished
};
} // namespace taw
