#pragma once

#include "Agents.h"
#include "AudioEngine.h"
#include "Bounce.h"
#include "AudioEditor.h"
#include "BrowserWindow.h"
#include "Import.h"
#include "Library.h"
#include "LiveKeys.h"
#include "MidiEditor.h"
#include "Plugins.h"
#include "Console.h"
#include "KeyCast.h"
#include "Session.h"
#include "SystemStats.h"
#include "Timing.h"
#include "Ui.h"
#include "Video.h"

#include <functional>
#include <optional>
#include <map>
#include <set>
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
    juce::StringArray inputs; // the type's inputs, for ←→ ("" none); empty when it pairs them (ASIO)
};

juce::String formatDb (double db); // "+1.5", "-6.0", "-inf"
// Tracks as they're numbered on screen and typed: 1, 2, 3...; a kit's pads aren't numbered as
// tracks but by their kit and pad, 2.3 (App::numbering).
struct TrackNumbering
{
    std::vector<int> byNumber;              // number - 1: the track's index
    std::map<int, std::map<int, int>> pads; // kit number: pad (1-16): the track's index
    std::vector<juce::String> labels;       // index: "3", or "2.3" for pad 3 of kit 2
    juce::String label (int index) const { return index >= 0 && index < (int) labels.size() ? labels[(size_t) index] : juce::String (index + 1); }
};
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

// The same, as parts: what an agent's run_command is about to do (AgentEdits.cpp).
struct CommandShape
{
    juce::String error, verb; // verb lower case
    bool hasSelector = false, mix = false;
    std::vector<int> tracks;
    juce::StringArray args;
};
CommandShape commandShape (const juce::String& line, const TrackNumbering&);

enum class Mode
{
    Main,
    DevicePicker,
    TemplatePicker,
    Hub // the audio hub's page, with or without a session (HubPanel.cpp)
};

class App final : private juce::ValueTree::Listener
{
public:
    // openDir: an existing session to open (may be empty). Otherwise a new session is created
    // under newParent, named newName, from templateName; empty fields are asked for.
    // inBackground: a closed session an agent is editing, inside the running taw (backgroundFor):
    // no screen, no device, nothing app-wide written, its edits saved to unsaved\.
    App (juce::File openDir, juce::File newParent, juce::String newName, juce::String templateName, bool inBackground = false);
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

    // Agents in the open session (AgentEdits.cpp, docs/plan-taw-mcp.md): taw listens on a local
    // socket for taw --mcp, and runs the agents' tool calls here, on the message thread, one at a
    // time, each one undo step. Edits are refused while playing, and on what's the user's: the
    // focused track, the time selection, anything they changed in the last few seconds.
    bool startAgents();  // the socket and its listing (run() does it; tests too)
    void pollAgents();   // once a frame: the next call, or the one waiting for its import
    agents::Answer agentCall (const agents::Call&); // one call, now
    std::unique_ptr<agents::Server> agentServer;
    agents::RecentEdits recentEdits;
    bool agentsOn = true;                      // settings.json "agents": the socket at all
    Result agentsCommand (const juce::String& arg); // "agents", "agents on", "agents off"

    // What the agents are doing, for the readout on the title line, and the chip on the
    // transport while an edit is held off by playing.
    struct AgentSeen
    {
        juce::String agent, tool, text, code; // code: why it was refused ("" when it went in)
        bool edit = false;                    // a change, not a read
        double when = 0.0;                    // ms
    };
    std::optional<AgentSeen> agentLastCall, agentLastEdit;
    int agentHeld = 0;          // edits refused for playing since it started
    juce::String agentHeldBy;
    bool agentRolling = false;  // as pollAgents last saw it
    void noteAgentCall (const agents::Call&, const agents::Answer&);

    // The user's input (keys, mouse, MIDI). Their cursor and time selection are theirs while
    // they're at it; after agentIdle seconds without any, agents may edit there too (what they
    // changed in the last few seconds stays theirs either way: RecentEdits).
    double lastInputAt = 0.0; // ms
    double agentIdle = 10.0;  // seconds
    int midiSeenForIdle = 0;
    bool userIdle() const { return juce::Time::getMillisecondCounterHiRes() - lastInputAt >= agentIdle * 1000.0; }

    // What an agent just changed, lit so you see it happen: a flash, then the agents' colour
    // for a few seconds. owner: a track, or the session's tree; what: "vol", "pan", "send a",
    // "bpm", "markers", or "" (the track itself).
    struct AgentMark { juce::ValueTree owner; juce::String what; double when; };
    std::vector<AgentMark> agentMarks;
    static constexpr double agentFlashMs = 250.0, agentTintMs = 4000.0;
    void markAgentEdit (const juce::ValueTree& owner, const juce::String& what);
    double agentMarkAge (const juce::ValueTree& owner, const juce::String& what = {}) const; // ms since, or -1 (none lately; "": any)

    // An agent's new tempo on a session with audio clips waits for the user to say what
    // happens to the audio (the tempo box). MIDI and hits follow the beats either way.
    enum class TempoChoice { cancel, keepInTime, moveWithBeats };
    struct TempoAsk { juce::String agent; double from = 0.0, to = 0.0; };
    struct TempoOutcome
    {
        juce::String agent;
        double from = 0.0, to = 0.0;
        juce::String audio; // what the user chose, in words
        bool applied = false;
        double when = 0.0;  // ms
    };
    std::optional<TempoAsk> tempoAsk;         // the box is up
    std::optional<TempoOutcome> tempoOutcome; // the last answer, for the agent to read
    void answerTempoAsk (TempoChoice);
    bool handleTempoAskKey (const Key&);

    // Closed sessions (docs/plan-taw-mcp.md, "Closed sessions"): a tool given another session
    // than the open one runs on a background copy of it here, a headless App of its own (no
    // device, no screen, no user: hands-off doesn't apply), its edits saved to that session's
    // unsaved\ and never to session.json. Plugins load only for calls that need them. Opening
    // the session hands the copy's Session (tree, undo history, lock) over to this taw.
    bool background = false; // this App is such a copy
    App* liveApp = nullptr;  // ...and the taw it's in
    struct BackgroundSession
    {
        std::unique_ptr<App> app;
        double lastUsed = 0.0;     // ms
        juce::StringArray agents;  // who changed it, for the note when it's opened
    };
    std::vector<BackgroundSession> backgroundSessions;
    double backgroundIdle = 120.0; // seconds unused before a copy is let go (its edits stay in unsaved\)
    size_t maxBackground = 3;
    App* backgroundFor (const juce::File& folder, juce::String& error); // loads it the first time
    bool backgroundBusy (const juce::File& folder) const;                // a call of its still running (an import)
    void pollBackgroundSessions();                                       // imports answered, idle copies let go, a waiting open
    void noteBackgroundCall (BackgroundSession&, const agents::Call&, const agents::Answer&); // its edits to unsaved\, said
    // The session in a folder, for opening it here: the background copy's, handed over, if
    // there is one; else read from disk.
    std::unique_ptr<Session> loadSession (const juce::File& dir, juce::String& error);
    std::unique_ptr<Session> giveUpSession(); // a background copy: its edits written, its Session out
    juce::File openWhenHandedOver;            // the user opened it mid-call: opened once the call's done
    // A tempo change on a closed session with audio clips: the box comes up when it's opened.
    struct PendingTempo { juce::File folder; TempoAsk ask; };
    std::vector<PendingTempo> pendingTempo;

    // An agent's new_session / open_session: on the session screen it happens at once; over an
    // open session a box asks (save and switch, switch without saving, or stay). The agent
    // never closes or saves anything itself.
    struct SessionAsk
    {
        juce::String agent;
        bool create = false;   // new_session: made when the user says yes
        juce::String asked;    // the name or folder the agent gave (to know its retry)
        juce::File folder;     // the session to open (or to make)
        juce::String templateName;
    };
    struct SessionOutcome
    {
        juce::String agent, asked;
        bool create = false, applied = false;
        juce::File folder;
        double when = 0.0; // ms
    };
    std::optional<SessionAsk> sessionAsk;         // the box is up
    std::optional<SessionOutcome> sessionOutcome; // the last answer, for the agent's retry
    // new_session / open_session as the tools call it: "" when it's under way, else why not.
    Result switchSession (const SessionAsk&); // no session open: now; else asks
    void answerSessionAsk (juce::juce_wchar choice); // 's', 'o', 'd' (or enter: open), 0 stays
    bool handleSessionAskKey (const Key&);
    Result makeSessionFolder (const SessionAsk&); // new_session: the session on disk, from its template
    juce::File newSessionFolderFor (const juce::String& name) const; // where new_session makes one

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
    void startClosing (bool thenQuit, bool save, bool bounceAsked = true); // bounceAsked: last-saved, when it saves
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
    // Up and down move within the focused area; tab and shift+tab (and left and right, where
    // they've nothing else to do) move between areas.
    enum class PickArea { templates, sessions, bounces };
    PickArea pickArea = PickArea::sessions;
    int newSessionCursor = 0; // in newSessionName (clamped to it: other places set the name)
    int templateIndex = 0, sessionIndex = 0;
    struct RecentBounce { juce::File file; juce::String session; juce::File folder; juce::Time time; };
    std::vector<RecentBounce> recentBounces;
    int bounceIndex = 0;
    void loadRecentBounces();
    void movePickArea (PickArea to);
    void cyclePickArea (int direction); // tab: +1, shift+tab: -1
    void typeIntoName (const juce::String& text);
    std::vector<Session::Summary> recentSummaries; // beside recent: tracks, bpm, plugins, length

    // Where new sessions go (and whose sessions the screen lists alongside the recent ones):
    // kept in settings.json, so a client's drive stays the place until you change it. ctrl+o
    // picks a folder in Windows' dialog; ctrl+l types (or pastes) a path.
    juce::File sessionsFolder; // settings.json; empty: wherever taw was started
    Result setSessionsFolder (const juce::File&);
    void browseSessionsFolder();
    std::unique_ptr<juce::FileChooser> folderChooser;
    bool editingFolder = false;
    juce::String folderEdit;
    bool folderEditFresh = false; // the path shown is the old one: typing replaces it
    bool handleFolderEditKey (const Key&);
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
    // "stats": how hard the machine is working as you build the session: the audio's load,
    // the CPU and memory, and each track's share (the heavy ones first). Keys go on to the
    // session (space plays) except esc, which closes it.
    Result toggleStats();
    bool statsVisible = false;
    bool handleStatsKey (const Key&);
    SystemStats system; // sampled about once a second
    double systemSampledAt = 0.0;
    bool midiPanelVisible = false;
    int midiPanelIndex = 0;
    bool handleMidiPanelKey (const Key&);

    // The audio hub (docs/plan-audio-hub.md; HubPanel.cpp): its page shows the interface, the
    // sessions and the routes between them, from a session ("hub"), the start screen (tab) or
    // the shell ("taw hub"). "hub route in 1-2 > Song" and "hub unroute" change them, "hub
    // start" / "hub stop" run it; on the page the same without "hub". macOS only so far.
    Result hubCommand (const juce::String& rest);
    Result hubRoute (const juce::String& spec, bool add);
    void openHubPage();
    void closeHubPage();
    void handleHubPageKey (const Key&);
    bool openHubFirst = false;   // taw hub: the page once the device is open, not the start screen
    Mode hubReturn = Mode::Main; // where esc goes back to
    juce::String hubLine;        // the page's command line
    juce::var hubStatus;         // what the hub last said (void: nothing)
    juce::String hubError;       // why not
    double hubAskedAt = 0.0;     // when, to ask again now and then while the page is up
    void refreshHub (bool now);
    juce::String hubEndName (const juce::String& key, int channel, bool source) const;
    // An app device as the hub sees it: as a source "taw1in" (what apps play to taw 1 comes in),
    // as a destination "taw1out" (what goes out to apps hearing it); "Zoom" if it's named so.
    juce::String hubAppName (const juce::String& key, bool source) const;
    bool hubAppNamed (const juce::String& key) const;
    juce::String hubHardware (bool output) const; // the hub's interface that way: "MacBook Pro Speakers"

    // The hub's interface, picked over its page (i): outputs on the left, inputs on the right
    // (CoreAudio's, not the driver's own: a loop). ←→ goes between them, ↑↓ picks, enter uses both.
    struct HubInterfacePicker
    {
        bool open = false;
        juce::StringArray outputs, inputs;
        int output = 0, input = 0;
        bool onInputs = false;
    };
    HubInterfacePicker hubPicker;
    void openHubInterfacePicker();
    void handleHubInterfaceKey (const Key&);

    // Who uses each app device now ("taw 1"): the apps playing into it, and those hearing it as
    // their mic (macOS only lists an app while its audio runs). Asked with the status.
    struct HubUsers
    {
        juce::StringArray playing, listening;
    };
    std::map<juce::String, HubUsers> hubUsers;
    bool sessionOnHub() const; // the open session is one of the hub's (its device is taw hub)

    // The page's matrix: rows are where sound comes from (the interface's inputs, the app
    // devices, sessions' outputs), columns where it goes (the interface's outputs in pairs, the
    // app devices' mics, sessions' inputs). A cell stands for the routes between the two.
    struct HubEnd
    {
        juce::String key;          // "interface", "taw 1", or a session's folder
        int first = 1, width = 1;  // its channels (a session column: where it lands is per cell)
        juce::String name, sub;    // "in 1" / "Zoom" / "Song"; a column's second line: "1-2", "mic", "in"
        bool session = false, live = true;
    };

    struct HubCell
    {
        bool on = false, odd = false, self = false; // odd: routes that aren't the usual shape (typed ones)
        int land = 0, landLast = 0;                 // a session column: the inputs it lands on
        double db = 0.0;
        juce::Array<juce::var> routes;             // the routes it stands for
    };

    std::vector<HubEnd> hubRows() const;
    std::vector<HubEnd> hubCols() const;
    HubCell hubCell (const HubEnd& row, const HubEnd& col) const;
    int hubRow = 0, hubCol = 0;
    Result hubToggle();
    Result hubMoveLanding (int delta);
    Result hubLevel (double delta);
    Result hubReplace (const juce::Array<juce::var>& remove, const std::vector<std::array<int, 2>>& add,
                       const juce::String& from, const juce::String& to, double db);

    // The sample browser (ctrl+B, browse): a window of its own (BrowserWindow.h), mouse-forward
    // like a plugin's and still keyboard-led: the library's kinds as tabs, a map of the chosen
    // kind where samples that sound alike sit together, a list, and a kit's 4x4 pads. Every move
    // auditions; typing filters. Enter gives the focused sample track that sample (it keeps its
    // hits), or puts it on the chosen pad of the kit here, or makes a new sample track. index
    // <folder> adds folders to the library. See Browser.cpp.
    enum class BrowserFocus { kinds, map, list, pads }; // tab goes round them
    std::unique_ptr<BrowserWindow> browserWindow;

    // What the map's axes show (0, "similar": the sound-alike map on both). Kept in settings.json;
    // alt+arrows step them.
    static const juce::StringArray& browserAxisNames();
    int browserAxisX = 0, browserAxisY = 0;
    juce::String browserAxisRange (bool y) const; // "51 Hz → 12 kHz" over the kind shown
    std::pair<juce::String, juce::String> browserAxisEnds (bool y) const; // low, high
    void setBrowserAxis (bool y, int step);
    void setBrowserAxisTo (bool y, int axis); // the window's menus
    Result focusBrowser();          // ctrl+B: the window, in front with the keys
    Result showBrowser (bool show); // browse, and ctrl+B from inside it: open or closed
    bool browserOpen() const;
    bool handleBrowserKey (const Key&); // the window's keys (false: taw's)
    std::vector<juce::String> browserKinds() const;     // "all", then the kinds the library has
    std::vector<int> browserShown() const;              // the samples the tab and filter leave (or like this), list order
    std::pair<float, float> browserPosition (int sample) const; // on the map now showing, 0..1
    Result startIndexing (const juce::StringArray& folders);    // index <folder>...; index remove <folder>...
    Result removeFromLibrary (const juce::StringArray& folders); // index remove
    Result describeLibrary();                                    // library: its folders and how many samples each
    void pollIndexing();
    BrowserFocus browserFocus = BrowserFocus::map;
    int browserKind = 0, browserSample = -1;
    juce::String browserFilter;
    int browserLike = -1;      // showing the samples most like this one ("like this"), nearest first; -1 off
    bool browserHover = false; // the map auditions under the pointer without a click (settings.json)
    bool browserInTime = true; // while playing, an audition waits for the next grid step (settings.json)
    bool browserToPad = true;  // on a kit, what you audition goes onto the chosen pad at once
    bool browserGuide = false; // ? in the browser: its keys and mouse, over the map
    double auditionDb = -6.0; // the browser's audition voice, its own level (settings.json)
    Result setAuditionDb (double db);
    void browserPick (int sample, bool audition = true, bool toPad = true); // select (and audition, and onto the pad)
    void browserAudition (int sample);  // play it, in time with the loop when rolling
    Result browserLoad (bool newTrack); // enter: onto the pad, the focused sample track, or a new one
    Result browserLikeThis();           // the samples most like the chosen pad's sound (or the chosen one)
    int librarySampleFor (const juce::ValueTree& track) const; // the library's entry a sample track came from, or -1
    void browserPlayPad (int note);     // a pad pressed in the window
    std::unique_ptr<Library> library;
    std::atomic<int> indexDone { 0 }, indexTotal { 0 };
    bool isIndexing() const { return indexThread.joinable(); }

    // Kits (Kits.cpp): a kit is a bus (kit set) whose members are the sample tracks playing into
    // it, each on a MIDI note (pad 1 is 36, C1, what drum pads send) and maybe in a choke group.
    // Armed, the controller's notes (and keys mode's 4x4 pads) play its members; recording writes
    // the hits onto each member, as the pad key does.
    static constexpr int firstPadNote = 36;
    int kitOf (const juce::ValueTree& track) const; // the kit a sample track plays into, or -1
    int kitOf (int track) const { return session != nullptr && track >= 0 && track < session->numTracks() ? kitOf (session->track (track)) : -1; }
    int kitHere() const;                               // the focused kit, or the kit of the focused member; -1
    std::vector<int> kitMembers (int kit) const;       // its members, by note
    int kitMemberOnNote (int kit, int note) const;     // the member playing that note, or -1
    bool trackHidden (int track) const;                // a member of a folded kit
    int nextShownTrack (int from, int direction) const; // up or down past folded members
    TrackNumbering numbering() const;
    juce::String trackLabel (int index) const { return numbering().label (index); } // "3", "2.3"
    juce::String trackList (const std::vector<int>& tracks) const;                    // "track 2", "tracks 1,3", "pad 2.3"
    bool keysPlayKit() const;                          // keys mode plays the 4x4 pads (LiveKeys::setPads)
    Result addKit (const juce::String& name);          // kit / add kit [name]
    Result joinKit (const std::vector<int>& tracks, const juce::String& kitQuery); // 3 kit drums: into a kit; 3 kit off: out
    Result setPadNote (const std::vector<int>& tracks, const juce::String& arg);   // 3 note 38 / 3 note d1
    Result setChoke (const std::vector<int>& tracks, const juce::String& arg);     // 3 choke 1 / 3 choke off
    Result loadPad (int kit, int note, const juce::String& path);                   // pad 3 <file>: onto a pad (new member or a new sound)
    Result toggleFold (int kit);                                                    // shift+F on a kit
    Result describeKit (int kit) const;
    juce::String padName (int note) const;              // "pad 3 D1"
    void recordKitHit (int note, float velocity, double beat);
    // The browser's pad: where a sample loaded from the browser goes (a note on the kit here).
    int browserPadNote = firstPadNote;

    // A track whose sample the browser just swapped: its header flashes for a moment.
    // A value a shortcut changes ([ ] volume, { } the mix, alt+[ ] pan, a send's level) flashes
    // where it's shown, on every press, so your fingers get a glance's confirmation. owner: the
    // track (the session tree for the mix); what: "vol", "pan", "send a".
    void flashValue (const juce::ValueTree& owner, const juce::String& what)
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        valueFlashes.erase (std::remove_if (valueFlashes.begin(), valueFlashes.end(), [&] (const auto& f) { return f.until < now || (f.owner == owner && f.what == what); }),
                            valueFlashes.end());
        valueFlashes.push_back ({ owner, what, now + 220.0 });
    }
    bool valueFlashing (const juce::ValueTree& owner, const juce::String& what) const
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        return std::any_of (valueFlashes.begin(), valueFlashes.end(), [&] (const auto& f) { return f.owner == owner && f.what == what && now < f.until; });
    }
    struct ValueFlash { juce::ValueTree owner; juce::String what; double until; };
    std::vector<ValueFlash> valueFlashes;
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
    bool trackMenuDuplicating = false; // u: the menu asks what the copy's main playlist starts with
    std::vector<TrackMenuItem> trackMenuItems();
    Result openTrackMenu();
    bool handleTrackMenuKey (const Key&);
    Result removeTracks (std::vector<int> tracks); // "2 remove"
    // "2 dup track" (or "2 dup track empty"): a copy under each, plugins, sends and automation
    // and all; its main playlist a copy of the original's, or empty.
    Result duplicateTracks (std::vector<int> tracks, bool withClips);
    Result moveTracks (int direction);             // alt+up/down: the selected (or focused) tracks, one place

    PluginSlot* instrumentFor (int track); // the loaded plugin, or nullptr
    juce::String instrumentProblem (int track) const; // why it isn't loaded
    void openPluginPicker (int targetTrack, int effectSlot = -1); // effectSlot >= 0: effects, for that insert
    PluginCatalog plugins;
    std::unique_ptr<MidiInputs> midiInputs;

    // Keys mode (ctrl+k): the typing keyboard plays the armed instrument tracks (LiveKeys.h).
    double keysWarnedAt = -1.0e9; // a shortcut typed in keys mode (ms): the way out is shown for a moment
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
    // Separate (ctrl+e, as in Pro Tools): cut the clips at the selection's edges (what's inside
    // becomes its own clip), or with no selection at the cursor on the grid.
    Result separateClips (const std::vector<int>& tracks);
    int splitClipsAt (juce::ValueTree holder, juce::int64 at); // the clips split there

    // Fades (Fades.cpp). ctrl+f: by what the selection covers on each track, a fade in, a fade
    // out or a crossfade; again on the same fades, the next curve. "clip fade in 10ms",
    // "clip fade out 1/4", "clip fade off" on the clip under the cursor; "xfade 20ms" where two
    // clips meet nearest the cursor; "declick 2" (ms, off).
    struct FadePlan;
    Result fadeSelection (const std::vector<int>& tracks);
    Result clipCommand (const juce::StringArray& args);
    Result crossfadeCommand (const juce::StringArray& args);
    Result setDeclick (const juce::String& arg);
    Result planFades (juce::ValueTree holder, juce::Range<juce::int64> range, FadePlan& plan);
    Result applyFades (FadePlan& plan, std::optional<FadeShape> shape);
    juce::int64 lengthInSamples (const juce::String& spec) const; // "10ms", "0.5s", "1/4", "20" (ms); -1 if it isn't one
    juce::ValueTree clipUnderCursor() const;                      // the focused track's topmost audio clip there
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
    bool devicePickerFromSessions = false; // alt+d on the session screen: esc goes back there, even with no device
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
    static constexpr int helpPages = 9;
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
    mutable juce::var settingsSeen; // settings.json as this taw last read or wrote it

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
    void handleKey (const Key&); // dispatchKey, then the keycast window sees it
    void dispatchKey (const Key&);
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

    // A device that's both the input and the output and runs below 44.1 kHz: a Bluetooth
    // headset with its mic open. Instruments can go silent at that rate. Empty when it isn't.
    juce::String headsetWarning();

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
    bool editorOpen (const juce::ValueTree& node) const; // its plugin window is showing (an instrument track or an INSERT)
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
    Clipboard::Lane gatherLane (const juce::ValueTree& track, const juce::ValueTree& holder, juce::Range<juce::int64>) const;
    int clearRange (int track, juce::Range<juce::int64>); // returns how many clips/hits it touched
    int clearClips (const juce::ValueTree& track, juce::ValueTree holder, juce::Range<juce::int64>);
    void pasteLane (const juce::ValueTree& track, juce::ValueTree holder, const Clipboard::Lane&, juce::int64 at, double beats);
    int clearTracks (const std::vector<int>& tracks, juce::Range<juce::int64>); // ...and their automation there
    Result pasteAt (const Clipboard&, const std::vector<int>& destTracks, juce::int64 at);
    Result checkEditable (const std::vector<int>& tracks, bool needsRange) const;
    juce::Result parsePosition (const juce::String& spec, juce::int64& pos) const; // "17", "17.3", "1:30"

    // Puts a finished take on its track as a clip. comp: latency compensation in samples.
    juce::String placeTake (int trackIndex, const juce::File&, juce::int64 start, juce::int64 length, juce::int64 comp); // what loop recording made of it
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

    // Keycast (KeyCast.h): a window showing the keys pressed, for screen recordings. "keycast"
    // shows or hides it and sets its look; that and its place are kept in settings.json.
    KeyCast keycast;
    bool keycastShown = false;  // settings.json
    bool keycastClosed = false; // its close button hid it (until "keycast"); not saved, like the video's
    Result keycastCommand (const juce::String& rest);
    void syncKeycast();

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

    // Playlists (Playlists.cpp), as in Pro Tools: ctrl+p a new empty one, ctrl+alt+p a copy of
    // the main; either becomes the main and the old main joins the others. shift+P shows them as
    // lanes under the track (instead of its automation lanes); on one, shift+S plays it in the
    // main's place, alt+shift+up copies the selection on it to the main (no selection: makes it
    // the main), and edits (copy, cut, paste, delete) act on it. Automation stays the track's.
    Result newPlaylist (bool copy);                            // ctrl+p / ctrl+alt+p, "pl new" / "pl copy"
    Result playlistCommand (const juce::StringArray& args);    // "pl": list; "pl 2" / "pl Vox.02": the main; "pl del"
    Result togglePlaylistLanes();                              // shift+P
    Result soloPlaylistLane();                                 // shift+S on a playlist lane
    Result promoteToMain();                                    // alt+shift+up on a playlist lane
    Result cyclePlaylist (int direction);                      // ctrl+alt+up/down: the previous / next playlist is the main
    Result makeMain (int track, juce::ValueTree playlist);
    std::vector<juce::ValueTree> playlistsShown (int track) const; // the lanes under it (empty: closed)
    juce::ValueTree focusedPlaylist() const;                   // the playlist lane you're on, or invalid
    // Where a track's edits go: the playlist lane you're on (on the focused track), else the track.
    juce::ValueTree clipHolder (int track) const;
    int lanesUnder (int track) const; // automation or playlist lanes open under it (-1: the mix)
    void closePlaylistLanes (juce::ValueTree track); // and their solos: nothing hidden plays
    // Loop recording: each pass around the loop is a take. At the end the last whole pass is on
    // the main and the ones before it are new playlists (the main as it was, plus that take); a
    // pass cut short by stopping is dropped.
    struct LoopTake
    {
        juce::int64 start = 0, end = 0; // the loop, samples
    };
    std::map<juce::String, LoopTake> loopTakes; // audio takes recorded looping, by file path
    bool loopRecording = false;
    juce::int64 loopRecStart = 0, loopRecEnd = 0;
    int loopWrapBase = 0;   // the engine's wrap count when recording started
    int loopPassesDone = 0; // passes MIDI and hits have been split into so far
    struct PassTake { juce::ValueTree playlist, take; };
    std::map<int, std::vector<PassTake>> loopPassTakes; // by track: the playlists the passes made
    void startLoopRecording();  // when a recording starts with the loop on
    void pollLoopPasses();      // each frame: a pass that has finished (as heard) is split off
    void splitLoopPass();       // a pass finished: MIDI and hit tracks' takes go to new playlists
    // Where something played at the playhead pos (wraps: the loop's count then) goes on the
    // timeline, in beats: as heard, and while loop recording, folded into the loop.
    double recordedBeat (juce::int64 pos, int wraps);
    juce::String finishLoopRecording(); // stopped: the last whole pass to the main, a partial one dropped (what it did)
    juce::ValueTree takePlaylist (juce::ValueTree track, juce::ValueTree take); // the main as it is + take, as a new playlist
    juce::String placeLoopTake (int track, const juce::String& rel, juce::int64 start, juce::int64 length, const LoopTake&);
    void closeEditorsOnPlaylists();
    juce::String loopNote; // what the last loop recording did, for the stop message

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
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override
    {
        treeDirty = unsaved = true;

        // A new track, or a copy carrying its original's id: an id of its own.
        if (session != nullptr && child.hasType (ids::TRACK) && parent == session->tree() && session->trackIdTaken (child))
            session->giveTrackId (child);
    }
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
    int importKitBus = 0, importNote = -1; // a pad load (loadPad): the kit and note it goes on
    juce::ValueTree newKitMember (const juce::String& name);
    juce::String giveSample (juce::ValueTree track, const juce::String& file, const juce::File& source);
    juce::String copyOf (const juce::File& source) const;
    struct QueuedPad { int kitBus = 0, note = -1; juce::String path; } queuedPad; // a pad load waiting for the copy before it
    std::map<juce::String, juce::String> copies; // "session folder|source": its copy, from this run's imports
    juce::String pendingTemplate;
    bool treeDirty = false, quitRequested = false;

    // An agent's call running: beginEdit leaves its one undo step alone. agentWaiting: a call
    // that started an import, answered when the import is in (pollAgents finishes the import,
    // not pollImport).
    friend struct AgentTools;
    bool agentEditing = false;
    std::optional<agents::Call> agentWaiting;
    juce::String agentWaitingLabel;


    // Each sample's rank on the chosen axes, within the kind shown (rebuilt when either changes).
    struct AxisCache
    {
        const Library* library = nullptr;
        size_t count = 0;
        int kind = -1, x = -1, y = -1;
        std::vector<float> rankX, rankY; // by sample index, 0..1
        juce::String lowX, highX, lowY, highY; // the values at the ends, written out
        juce::String filter;
        int like = -1;
    };
    mutable AxisCache axisCache;
    const AxisCache& axes() const;
    void loadLibrary();
    std::thread indexThread;
    std::atomic<bool> indexFinished { false };
    juce::String indexSummary; // written by the thread before indexFinished
};
} // namespace taw
