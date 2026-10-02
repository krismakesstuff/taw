#pragma once

#include "Ids.h"

namespace taw
{
// The session state tree plus the folder it lives in. Only the message thread touches this.
class Session
{
public:
    static constexpr int currentVersion = 2; // 2: tracks hold CLIPs instead of TAKEs

    // Creates <parentDir>/<name>/ with session.json and audio/, seeded from a template tree.
    static std::unique_ptr<Session> create (const juce::File& dir, const juce::ValueTree& templateTree,
                                            double sampleRate, juce::String& error);
    static std::unique_ptr<Session> load (const juce::File& dir, juce::String& error);

    static bool existsIn (const juce::File& dir) { return dir.getChildFile ("session.json").existsAsFile(); }

    bool save() const;

    // Set by load when session.json was unreadable and a backup was opened instead.
    juce::String loadNote;

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

    // Sample tracks: the track plays its sample once per HIT.
    static bool isSampleTrack (const juce::ValueTree& track) { return track[ids::kind].toString() == "sample"; }
    static std::vector<juce::ValueTree> hits (const juce::ValueTree& track);
    static juce::ValueTree makeHit (double beat, float vel);

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
} // namespace taw
