#pragma once

#include "AudioEngine.h"
#include "Bounce.h"
#include "Import.h"
#include "Console.h"
#include "Session.h"
#include "Ui.h"

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
    std::vector<float> overview; // peak per Recorder::overviewBlock samples
};

struct LiveTake
{
    std::shared_ptr<Recorder> recorder;
    std::vector<float> overview;
    juce::int64 start = -1;
    bool finishing = false;
};

struct DeviceChoice
{
    juce::String type, output, input;
};

juce::String formatDb (double db); // "+1.5", "-6.0", "-inf"
juce::String trackList (const std::vector<int>& tracks); // "track 2", "tracks 1,3"

// What ctrl+c holds: per track, its clips (audio) or hits (sample track), positioned relative
// to the start of the copied range.
struct Clipboard
{
    struct Lane
    {
        bool sample = false;
        std::vector<juce::ValueTree> items; // CLIPs (start in samples) or HITs (beat in beats)
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
    bool hasBackgroundWork() const { return import != nullptr || bounce != nullptr; }
    void pumpBackgroundWork();

    // Commands ----------------------------------------------------------------
    // One line of the command language. Used by the command line now and by agents later.
    Result execute (const juce::String& line);

    Result record (const std::vector<int>& tracks); // empty = whatever is armed
    Result punch();
    Result play();
    Result stop();
    Result addMarker (juce::int64 position);
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
    void quit() { quitRequested = true; }
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
    void zoom (int delta);
    void clearSelection() { clearTimeSelection(); trackAnchor = -1; }
    void clearTimeSelection() { timeAnchor = timeHead = -1; }

    // Selection: a time range and/or a range of tracks, made with shift + arrows. The time
    // range has its own two ends, independent of the cursor and the playhead.
    bool mixFocused() const;
    bool loopActive() const; // loop on with a real range: play starts at its start
    bool hasTimeSelection() const { return timeAnchor >= 0 && timeHead >= 0 && timeHead != timeAnchor; }
    juce::Range<juce::int64> timeSelection() const;
    std::vector<int> selectedTracks() const; // empty when no track selection
    std::vector<int> defaultTargets() const; // selection, else the focused track

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

    juce::String commandLine, message;
    bool messageIsError = false;

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
    std::unique_ptr<Graph> buildGraph();
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
