#pragma once

#include "AudioEngine.h"
#include "Bounce.h"
#include "Import.h"
#include "Plugins.h"
#include "Console.h"
#include "Session.h"
#include "Ui.h"

#include <functional>
#include <map>

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
Sequence buildSequence (const juce::ValueTree& track, double samplesPerBeat); // an instrument track's notes, in samples

// What ctrl+c holds: per track, its clips (audio) or hits (sample track), positioned relative
// to the start of the copied range.
struct Clipboard
{
    struct Lane
    {
        bool sample = false, instrument = false;
        std::vector<juce::ValueTree> items; // CLIPs (start in samples), HITs, or NOTEs and CCs (beat in beats)
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
    Result startBounce (const juce::StringArray& args);
    // New track per file at the cursor; with target >= 0, onto that track instead (a clip, or for
    // samples, the track's new sample).
    enum class ImportAs { audio, sample, autoByLength };
    Result startImport (const juce::StringArray& paths, int targetTrack = -1, ImportAs = ImportAs::audio);

    // Sample tracks
    Result convertTrack (int track, bool toSample);
    Result writePattern (const std::vector<int>& tracks, const juce::String& steps, double stepBeats);
    Result toggleHitAtCursor();

    // Instrument tracks: a VST3 instrument played from the MIDI controller while armed, and
    // the notes recorded on the track.
    // "inst <name>": a new instrument track (or, with tracks named, onto those). No name: the picker.
    Result loadInstrument (const std::vector<int>& tracks, const juce::String& query);
    Result loadInstrument (int track, const juce::PluginDescription&); // track -1: a new one
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
        return layout::laneWidth (console.width(), ioVisible, session != nullptr && session->trackHeight() > 1);
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
    void stepOpening();

    // Closing (close, or quitting with a session open), likewise.
    struct Closing
    {
        bool thenQuit = false;
        size_t done = 0, total = 0;
        juce::String current; // the plugin being saved
    };
    std::unique_ptr<Closing> closing;
    void startClosing (bool thenQuit);
    void stepClosing();
    Result finishClosing();
    Result scanPlugins (bool retryFailed); // "scan", "scan retry"
    // "midi": the MIDI inputs panel. Each input on or off (kept in settings.json), whether it
    // opened, and the last message it sent, to see a controller is getting through.
    Result toggleMidiPanel();
    bool midiPanelVisible = false;
    int midiPanelIndex = 0;
    bool handleMidiPanelKey (const Key&);
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

    PluginSlot* instrumentFor (int track); // the loaded plugin, or nullptr
    juce::String instrumentProblem (int track) const; // why it isn't loaded
    void openPluginPicker (int targetTrack, int effectSlot = -1); // effectSlot >= 0: effects, for that insert
    PluginCatalog plugins;
    std::unique_ptr<MidiInputs> midiInputs;

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
    std::map<int, juce::ValueTree> midiPassClips; // by track: the clip this pass records into (made on its first note)

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

    // Editing over the time selection (Editing.cpp). Audio tracks move clips, sample tracks hits.
    Result copySelection (const std::vector<int>& tracks);
    Result cutSelection (const std::vector<int>& tracks);
    Result deleteSelection (const std::vector<int>& tracks);
    Result duplicateSelection (const std::vector<int>& tracks); // right after itself
    Result paste (int firstTrack); // at the cursor, lanes on consecutive tracks
    Result selectRange (const juce::String& from, const juce::String& to); // "sel 3 5": bars 3 to 5
    Clipboard clipboard;

    static juce::StringArray splitPaths (const juce::String& text);
    static juce::String expandHome (const juce::String& path); // ~/x -> /Users/you/x (not on Windows)
    bool isBouncing() const { return bounce != nullptr; }
    void openDevicePicker();
    Result showControlPanel();
    // Quitting saves and closes the session first: on screen, in steps under the closing box.
    void quit()
    {
        if (session != nullptr && console.isOpen() && closing == nullptr)
            startClosing (true);
        else if (closing == nullptr)
            quitRequested = true;
    }
    Result closeSession(); // save, close, back to the recent sessions / new session screen
    void releaseSession();
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
    enum class TopLane { none, ruler, markers };
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

    juce::int64 cursor = 0, playStart = 0, viewStart = 0;
    int focused = 0, trackScroll = 0, zoomLevel = 0;
    juce::int64 timeAnchor = -1; // where a shift-selection in time started
    juce::int64 timeHead = -1;   // its moving end
    int trackAnchor = -1;        // where a shift-selection of tracks started
    bool recording = false, helpVisible = false;

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
    // The plugin being loaded right now (the screen shows it; it's blocked until it's in).
    juce::String loadingPlugin, loadingWhy;

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

private:
    void rebuildGraph();
    std::unique_ptr<Graph> buildGraph (bool offline = false); // offline: its own plugin instances, for a bounce
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
    void say (const Result&);

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { treeDirty = true; }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { treeDirty = true; }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { treeDirty = true; }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override { treeDirty = true; }

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
};
} // namespace taw
