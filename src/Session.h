#pragma once

#include "Ids.h"

namespace taw
{
// The session state tree plus the folder it lives in. Only the message thread touches this.
class Session
{
public:
    static constexpr int currentVersion = 4; // 2: tracks hold CLIPs instead of TAKEs; 3: notes live in MIDICLIPs; 4: hits in HITCLIPs

    // Creates <parentDir>/<name>/ with session.json and audio/, seeded from a template tree.
    static std::unique_ptr<Session> create (const juce::File& dir, const juce::ValueTree& templateTree,
                                            double sampleRate, juce::String& error);
    static std::unique_ptr<Session> load (const juce::File& dir, juce::String& error);

    static bool existsIn (const juce::File& dir) { return dir.getChildFile ("session.json").existsAsFile(); }

    // A saved session at a glance, without opening it (the session screen): how big it is.
    struct Summary
    {
        bool ok = false;
        int tracks = 0, plugins = 0; // plugins: instruments and inserts, the mix's too
        double bpm = 0.0, seconds = 0.0; // seconds: to the end of the last clip
    };
    static Summary summarise (const juce::File& dir);

    bool save() const;

    // Edits wait in unsaved\ (the session as it is, and plugin states) until a save, so
    // session.json and its date only change when you save. Left there by a crash (or the window
    // closed with changes), the next open picks them up.
    juce::File workFolder() const { return dir.getChildFile ("unsaved"); }
    bool saveWork() const;
    void discardWork() const { workFolder().deleteRecursively(); }

    // Set by load when session.json was unreadable and a backup was opened instead.
    juce::String loadNote;
    bool recovered = false; // load found unsaved changes left in workFolder() and opened them

    // Pass undo() to tree changes made by editing; pass nullptr for bookkeeping that shouldn't be
    // undone (the automatic arm/restore around a record pass, bounce settings).
    juce::UndoManager* undo() { return &undoManager; }

    juce::ValueTree& tree() { return state; }
    const juce::ValueTree& tree() const { return state; }
    const juce::File& folder() const { return dir; }
    juce::File audioFolder() const { return dir.getChildFile ("audio"); }

    int numTracks() const;
    juce::ValueTree track (int index) const; // 0-based
    juce::ValueTree addTrack (const juce::String& name, int input, juce::UndoManager* = nullptr);

    juce::String name() const { return state[ids::name]; }
    double sampleRate() const { return state[ids::sampleRate]; }
    double bpm() const { return state[ids::bpm]; }
    int beatsPerBar() const { return state[ids::timeSigNum]; }
    bool gridOn() const { return state[ids::grid]; }
    int trackHeight() const { return juce::jlimit (1, 4, (int) state[ids::trackHeight]); } // rows per track

    double samplesPerBeat() const { return sampleRate() * 60.0 / bpm(); }
    double samplesPerBar() const { return samplesPerBeat() * beatsPerBar(); }

    // A track's clips in session order (later ones play over earlier ones where they overlap).
    static std::vector<juce::ValueTree> clips (const juce::ValueTree& track);
    static juce::ValueTree lastClip (const juce::ValueTree& track);
    static juce::ValueTree makeClip (const juce::String& file, juce::int64 start, juce::int64 offset, juce::int64 length);

    // Sample tracks: the track plays its sample once per HIT. Hits live in HITCLIPs, which work
    // like MIDI clips (below): a window onto the hits inside, so a loop moves, copies and trims
    // as one thing. A hit's beat is in its clip's content.
    static bool isSampleTrack (const juce::ValueTree& track) { return track[ids::kind].toString() == "sample"; }
    static juce::ValueTree makeHit (double beat, float vel);
    static std::vector<juce::ValueTree> hitClips (const juce::ValueTree& track);
    static juce::ValueTree makeHitClip (double beat, double offset, double len);
    struct PlacedHit { juce::ValueTree node, clip; double beat; float vel; };
    static std::vector<PlacedHit> placedHits (const juce::ValueTree& track); // the hits that play, on the timeline (beats)

    // The clips measured in beats, either kind: MIDI clips and hit clips.
    static bool isBeatClip (const juce::ValueTree& c) { return c.hasType (ids::MIDICLIP) || c.hasType (ids::HITCLIP); }
    static std::vector<juce::ValueTree> beatClips (const juce::ValueTree& track);
    // A new clip of the same kind as this one.
    static juce::ValueTree makeBeatClip (const juce::ValueTree& kindOf, double beat, double offset, double len);

    // Instrument tracks: a plugin plays the track's NOTEs and CCs (positions in beats).
    static bool isInstrumentTrack (const juce::ValueTree& track) { return track[ids::kind].toString() == "instrument"; }
    static bool isAudioTrack (const juce::ValueTree& track) { return ! isSampleTrack (track) && ! isInstrumentTrack (track) && ! isBusTrack (track); }

    // Bus tracks play what other tracks send them (their output or a send), through their own
    // fader. They have no clips and no input.
    static bool isBusTrack (const juce::ValueTree& track) { return track[ids::kind].toString() == "bus"; }
    int findBus (int busId) const; // the bus track's index, or -1
    int nextBusId() const;
    static constexpr int numSends = 5; // slots a to e
    static juce::ValueTree send (const juce::ValueTree& track, int slot); // invalid when the slot is empty
    static juce::ValueTree insert (const juce::ValueTree& track, int slot); // the effect in slot a-e, or invalid
    static juce::String slotName (int slot) { return juce::String::charToString ((juce::juce_wchar) ('A' + slot)); }

    // Automation: AUTO lanes on a track (or the session, for the mix), each a param and its
    // POINTs (beat, val), in any order on disk; points() sorts them by beat.
    static juce::ValueTree automation (const juce::ValueTree& owner, const juce::String& param); // invalid when none
    static std::vector<juce::ValueTree> automations (const juce::ValueTree& owner);
    static std::vector<juce::ValueTree> points (const juce::ValueTree& lane);
    static juce::ValueTree makePoint (double beat, double value);
    static juce::ValueTree makeNote (double beat, double len, int pitch, int vel, int channel = 1);
    static juce::ValueTree makeControl (double beat, int num, int val, int channel = 1);

    // MIDI clips (and hit clips): a window [offset, offset + len) onto the clip's content (its NOTEs and CCs,
    // positioned in content beats), placed at beat on the timeline. Like an audio clip's window
    // into its file, trimming hides notes without losing them. A note belongs to the clip it
    // starts in, and is cut off at the clip's end. Overlapping MIDI clips both play.
    static std::vector<juce::ValueTree> midiClips (const juce::ValueTree& track);
    static juce::ValueTree makeMidiClip (double beat, double offset, double len);
    static juce::ValueTree lastMidiClip (const juce::ValueTree& track);
    // Where the clip's content beat 0 falls on the timeline.
    static double contentZero (const juce::ValueTree& clip) { return (double) clip[ids::beat] - (double) clip[ids::offset]; }
    static double clipEnd (const juce::ValueTree& clip) { return (double) clip[ids::beat] + (double) clip[ids::len]; }
    static bool inWindow (const juce::ValueTree& clip, double contentBeat);
    // Grows the clip (never shrinks it) to take in [from, to) on the timeline; its content stays put.
    static void growMidiClip (juce::ValueTree clip, double from, double to, juce::UndoManager*);

    // The notes and controllers that play, placed on the timeline (in beats), each with its node
    // and the clip it's in. Notes end at their clip's end.
    struct PlacedNote { juce::ValueTree node, clip; double start, end; };
    struct PlacedControl { juce::ValueTree node, clip; double at; };
    static std::vector<PlacedNote> placedNotes (const juce::ValueTree& track);
    static std::vector<PlacedControl> placedControls (const juce::ValueTree& track);

    // A copy of a clip (MIDI or hits) cut down to [from, to) on the timeline, keeping only what's
    // inside that (the rest would be hidden for good). Invalid if they don't meet.
    static juce::ValueTree trimmedMidiClip (const juce::ValueTree& clip, double from, double to);

    // The fine grid step (alt + arrows, step editing), in beats: 1 for quarters, 0.25 for 16ths.
    double fineStepBeats() const { return 4.0 / juce::jmax (1, (int) state[ids::gridDiv]); }

    // End of the last clip or hit on any track.
    juce::int64 contentEnd() const;

private:
    Session (juce::File folder, juce::ValueTree tree) : dir (std::move (folder)), state (std::move (tree)) {}

    juce::File dir;
    juce::ValueTree state;
    juce::UndoManager undoManager;
};

// Generic ValueTree <-> JSON: { "type": "...", <properties>, "children": [ ... ] }.
juce::var treeToVar (const juce::ValueTree&);
juce::ValueTree varToTree (const juce::var&);

// Templates are session-shaped JSON files in the user's config folder. Built-ins are written
// there on first run so users can see and edit them.
struct Template
{
    juce::String name;
    juce::ValueTree tree;
};

juce::File configFolder();
std::vector<Template> loadTemplates();

// Recently opened session folders, newest first, only those that still exist.
std::vector<juce::File> loadRecentSessions();
void rememberSession (const juce::File& folder);
void enableRecentSessions(); // interactive runs only; until called, rememberSession does nothing

// Bounces taw made, newest first, only those still there (for the session screen).
struct BounceRecord
{
    juce::File file;
    juce::String session;
    juce::File folder; // the session's
};
std::vector<BounceRecord> loadRecentBounceRecords();
void rememberBounce (const juce::File& file, const juce::String& session, const juce::File& folder); // likewise only once enabled
} // namespace taw
