#include "Session.h"
#include "Safety.h"

namespace taw
{
static const juce::Identifier typeKey { "type" }, childrenKey { "children" };

static void migrateTakesToClips (juce::ValueTree& session);
static void migrateNotesToMidiClips (juce::ValueTree& session);
static void wrapLooseHits (juce::ValueTree& session);

// Every open copies session.json into backups\ as it was on disk, keeping the newest few, so
// a bad save never leaves you without the version you opened.
static void backUp (const juce::File& dir)
{
    constexpr int keep = 10;
    const auto source = dir.getChildFile ("session.json");
    const auto folder = dir.getChildFile ("backups");

    if (! source.existsAsFile() || ! folder.createDirectory())
        return;

    source.copyFileTo (folder.getChildFile ("session-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".json"));

    auto old = folder.findChildFiles (juce::File::findFiles, false, "session-*.json");
    std::sort (old.begin(), old.end(), [] (const juce::File& a, const juce::File& b) { return a.getFileName() > b.getFileName(); });

    for (int i = keep; i < old.size(); ++i)
        old.getReference (i).deleteFile();
}

juce::var treeToVar (const juce::ValueTree& t)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty (typeKey, t.getType().toString());

    for (int i = 0; i < t.getNumProperties(); ++i)
    {
        const auto n = t.getPropertyName (i);

        // "type" and "children" are the format's own keys; a property with either name would
        // overwrite them and the node would be lost on load.
        jassert (n != typeKey && n != childrenKey);

        if (n != typeKey && n != childrenKey)
            obj->setProperty (n, t[n]);
    }

    if (t.getNumChildren() > 0)
    {
        juce::Array<juce::var> kids;

        for (const auto& c : t)
            kids.add (treeToVar (c));

        obj->setProperty (childrenKey, kids);
    }

    return juce::var (obj);
}

juce::ValueTree varToTree (const juce::var& v)
{
    auto* obj = v.getDynamicObject();

    if (obj == nullptr || obj->getProperty (typeKey).toString().isEmpty())
        return {};

    auto type = obj->getProperty (typeKey).toString();
    juce::String kind;

    // Builds before the fix saved tracks with their kind ("audio"/"sample") over the node type.
    if (type == "audio" || type == "sample")
    {
        kind = type;
        type = ids::TRACK.toString();
    }

    juce::ValueTree t (juce::Identifier { type });

    if (kind.isNotEmpty())
        t.setProperty (ids::kind, kind, nullptr);

    for (const auto& p : obj->getProperties())
        if (p.name != typeKey && p.name != childrenKey)
            t.setProperty (p.name, p.value, nullptr);

    if (auto* kids = obj->getProperty (childrenKey).getArray())
        for (const auto& k : *kids)
            if (auto child = varToTree (k); child.isValid())
                t.appendChild (child, nullptr);

    return t;
}

static void applyDefaults (juce::ValueTree& s)
{
    auto def = [&s] (const juce::Identifier& id, const juce::var& v)
    {
        if (! s.hasProperty (id))
            s.setProperty (id, v, nullptr);
    };

    def (ids::version, Session::currentVersion);
    def (ids::bpm, 120.0);
    def (ids::timeSigNum, 4);
    def (ids::timeSigDen, 4);
    def (ids::grid, true);
    def (ids::gridDiv, 4);
    def (ids::latencyComp, true);
    def (ids::trackHeight, 2);
    def (ids::inputQuantize, false);
    def (ids::click, false);
    def (ids::clickWhen, "rec");
    def (ids::clickDb, -6.0);
    def (ids::countIn, 4);
    def (ids::lowLatencyMax, 256);
    def (ids::countInWhen, "rec");
    def (ids::mixDb, 0.0);
    def (ids::loop, false);
    def (ids::loopStart, (juce::int64) 0);
    def (ids::loopEnd, (juce::int64) 0);
    def (ids::bounceFormat, "wav");
    def (ids::bounceFolder, "bounces");
    def (ids::bounceReveal, true);
    def (ids::videoStart, (juce::int64) 0);
    def (ids::videoOffset, 0);
    def (ids::videoShown, true);

    for (auto t : s)
    {
        if (! t.hasType (ids::TRACK))
            continue;

        auto tdef = [&t] (const juce::Identifier& id, const juce::var& v)
        {
            if (! t.hasProperty (id))
                t.setProperty (id, v, nullptr);
        };

        tdef (ids::kind, "audio");
        tdef (ids::name, "Track");
        tdef (ids::input, 0);
        tdef (ids::armed, false);
        tdef (ids::mute, false);
        tdef (ids::solo, false);
        tdef (ids::monitor, false);
        tdef (ids::gainDb, 0.0);
    }
}

//==============================================================================
// The session lock: <session>/.taw-lock names the taw that has it open (pid, computer, since
// when), so two taws never save over each other. A lock whose taw is gone (it crashed) is
// taken over. One from another computer (a shared or synced folder) can't be checked, so it
// stands until it's removed.
static juce::File lockFile (const juce::File& dir) { return dir.getChildFile (".taw-lock"); }

static bool takeLock (const juce::File& dir, juce::String& error)
{
    const auto file = lockFile (dir);
    const auto host = juce::SystemStats::getComputerName(); // for people; it can change with the network
    const auto machine = juce::SystemStats::getUniqueDeviceID();

    if (file.existsAsFile())
    {
        const auto lock = juce::JSON::parse (file);
        const auto pid = (juce::uint32) (juce::int64) lock["pid"];
        const auto since = juce::Time ((juce::int64) lock["time"]).formatted ("%d %b %H:%M");

        if (lock["machine"].toString() != machine)
        {
            error = dir.getFileName() + L" is open in taw on " + lock["host"].toString() + " (since " + since
                    + L") \x00b7 if it isn't any more, delete " + file.getFullPathName();
            return false;
        }

        if (pid != safety::thisProcess() && safety::tawRunning (pid))
        {
            error = dir.getFileName() + " is open in another taw (pid " + juce::String (pid) + ", since " + since + ")";
            return false;
        }
    }

    auto* o = new juce::DynamicObject();
    o->setProperty ("pid", (juce::int64) safety::thisProcess());
    o->setProperty ("host", host);
    o->setProperty ("machine", machine);
    o->setProperty ("time", juce::Time::currentTimeMillis());

    if (! file.replaceWithText (juce::JSON::toString (juce::var (o))))
    {
        error = "could not write " + file.getFullPathName();
        return false;
    }

    return true;
}

Session::~Session()
{
    // Only this taw's own lock: one taken over since (this one hung, say) isn't ours to remove.
    if (const auto file = lockFile (dir); (juce::uint32) (juce::int64) juce::JSON::parse (file)["pid"] == safety::thisProcess())
        file.deleteFile();
}

std::unique_ptr<Session> Session::create (const juce::File& dir, const juce::ValueTree& templateTree,
                                          double sampleRate, juce::String& error)
{
    if (Session::existsIn (dir))
    {
        error = "a session already exists in " + dir.getFullPathName();
        return {};
    }

    if (! dir.createDirectory() || ! dir.getChildFile ("audio").createDirectory())
    {
        error = "could not create " + dir.getFullPathName();
        return {};
    }

    if (! takeLock (dir, error))
        return {};

    auto tree = templateTree.isValid() ? templateTree.createCopy() : juce::ValueTree (ids::SESSION);
    tree.setProperty (ids::name, dir.getFileName(), nullptr);
    tree.setProperty (ids::sampleRate, sampleRate, nullptr);
    applyDefaults (tree);

    std::unique_ptr<Session> s (new Session (dir, tree));

    if (! s->save())
    {
        error = "could not write session.json";
        return {};
    }

    return s;
}

// Reads a session file; empty (with a reason) if it isn't a readable taw session.
static juce::ValueTree readSessionFile (const juce::File& file, juce::String& error)
{
    juce::var parsed;

    if (const auto result = juce::JSON::parse (file.loadFileAsString(), parsed); result.failed())
    {
        error = file.getFileName() + ": " + result.getErrorMessage();
        return {};
    }

    auto tree = varToTree (parsed);

    if (! tree.hasType (ids::SESSION))
    {
        error = file.getFileName() + " is not a taw session";
        return {};
    }

    return tree;
}

std::unique_ptr<Session> Session::load (const juce::File& dir, juce::String& error)
{
    // Before anything is read or repaired: another taw may be saving it.
    if (existsIn (dir) && ! takeLock (dir, error))
        return {};

    const auto file = dir.getChildFile ("session.json");
    auto tree = readSessionFile (file, error);
    juce::String note;
    bool recovered = false;

    // Unsaved changes left behind (a crash, or the window closed on them): open those.
    if (const auto work = dir.getChildFile ("unsaved"); work.isDirectory())
    {
        const auto workFile = work.getChildFile ("session.json");
        juce::String ignored;
        recovered = true;

        if (auto t = workFile.existsAsFile() ? readSessionFile (workFile, ignored) : juce::ValueTree(); t.isValid())
        {
            tree = t;
            error.clear();
        }

        note = L"opened the changes you hadn't saved (from " + (workFile.existsAsFile() ? workFile : work).getLastModificationTime().formatted ("%d %b %H:%M")
               + L") \x00b7 save keeps them, closing without saving drops them";
    }

    if (! tree.isValid())
    {
        // Damaged: open the newest backup that reads, and keep the damaged file beside it.
        auto backups = dir.getChildFile ("backups").findChildFiles (juce::File::findFiles, false, "session-*.json");
        std::sort (backups.begin(), backups.end(), [] (const juce::File& a, const juce::File& b) { return a.getFileName() > b.getFileName(); });

        for (const auto& b : backups)
        {
            juce::String ignored;

            if (tree = readSessionFile (b, ignored); tree.isValid())
            {
                const auto kept = dir.getChildFile ("session.json.damaged-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S"));
                file.moveFileTo (kept);
                b.copyFileTo (file);
                note = error + L" \x2014 opened the backup from " + b.getLastModificationTime().formatted ("%d %b %H:%M")
                       + L" (the damaged file is kept as " + kept.getFileName() + ")";
                break;
            }
        }

        if (! tree.isValid())
        {
            lockFile (dir).deleteFile(); // not opened after all
            return {};
        }
    }

    if ((int) tree.getProperty (ids::version, 1) < 2)
        migrateTakesToClips (tree);

    if ((int) tree.getProperty (ids::version, 1) < 3)
        migrateNotesToMidiClips (tree);

    wrapLooseHits (tree); // version 3 (or a hand-written file)

    applyDefaults (tree);
    dir.getChildFile ("audio").createDirectory();

    if (note.isEmpty() || recovered)
        backUp (dir);

    std::unique_ptr<Session> s (new Session (dir, tree));
    s->loadNote = note;
    s->recovered = recovered;
    return s;
}

Session::Summary Session::summarise (const juce::File& dir)
{
    Summary s;
    juce::String error;
    const auto tree = readSessionFile (dir.getChildFile ("session.json"), error);

    if (! tree.isValid())
        return s;

    s.ok = true;
    s.bpm = tree.getProperty (ids::bpm, 120.0);
    const double rate = tree.getProperty (ids::sampleRate, 44100.0);
    const double samplesPerBeat = rate * 60.0 / juce::jmax (1.0, s.bpm);
    juce::int64 end = 0;

    for (const auto& c : tree)
        s.plugins += c.hasType (ids::INSERT) ? 1 : 0; // the mix's

    for (const auto& t : tree)
    {
        if (! t.hasType (ids::TRACK))
            continue;

        ++s.tracks;
        s.plugins += t[ids::pluginName].toString().isNotEmpty() ? 1 : 0;
        const bool sampleTrack = isSampleTrack (t);

        for (const auto& c : t)
        {
            s.plugins += c.hasType (ids::INSERT) ? 1 : 0;

            if (! sampleTrack && c.hasType (ids::CLIP))
                end = juce::jmax (end, (juce::int64) c[ids::start] + (juce::int64) c[ids::length]);
            else if (isBeatClip (c))
                end = juce::jmax (end, (juce::int64) std::llround (clipEnd (c) * samplesPerBeat));
        }
    }

    s.seconds = rate > 0.0 ? (double) end / rate : 0.0;
    return s;
}

bool Session::save() const
{
    const auto json = juce::JSON::toString (treeToVar (state), juce::JSON::FormatOptions{}.withSpacing (juce::JSON::Spacing::multiLine));
    return dir.getChildFile ("session.json").replaceWithText (json + "\n");
}

bool Session::saveWork() const
{
    const auto json = juce::JSON::toString (treeToVar (state), juce::JSON::FormatOptions{}.withSpacing (juce::JSON::Spacing::multiLine));
    return workFolder().createDirectory() && workFolder().getChildFile ("session.json").replaceWithText (json + "\n");
}

int Session::numTracks() const
{
    int n = 0;

    for (const auto& c : state)
        n += c.hasType (ids::TRACK) ? 1 : 0;

    return n;
}

int Session::findBus (int busId) const
{
    int n = 0;

    for (const auto& c : state)
        if (c.hasType (ids::TRACK))
        {
            if (busId > 0 && isBusTrack (c) && (int) c[ids::busId] == busId)
                return n;

            ++n;
        }

    return -1;
}

int Session::nextBusId() const
{
    int top = 0;

    for (const auto& c : state)
        if (c.hasType (ids::TRACK))
            top = juce::jmax (top, (int) c[ids::busId]);

    return top + 1;
}

juce::ValueTree Session::send (const juce::ValueTree& track, int slot)
{
    const auto name = slotName (slot).toLowerCase();

    for (const auto& c : track)
        if (c.hasType (ids::SEND) && c[ids::slot].toString().equalsIgnoreCase (name))
            return c;

    return {};
}

juce::ValueTree Session::insert (const juce::ValueTree& track, int slot)
{
    const auto name = slotName (slot).toLowerCase();

    for (const auto& c : track)
        if (c.hasType (ids::INSERT) && c[ids::slot].toString().equalsIgnoreCase (name))
            return c;

    return {};
}

juce::ValueTree Session::automation (const juce::ValueTree& owner, const juce::String& param)
{
    for (const auto& c : owner)
        if (c.hasType (ids::AUTO) && c[ids::param].toString() == param)
            return c;

    return {};
}

std::vector<juce::ValueTree> Session::automations (const juce::ValueTree& owner)
{
    std::vector<juce::ValueTree> out;

    for (const auto& c : owner)
        if (c.hasType (ids::AUTO))
            out.push_back (c);

    return out;
}

std::vector<juce::ValueTree> Session::points (const juce::ValueTree& lane)
{
    std::vector<juce::ValueTree> out;

    for (const auto& c : lane)
        if (c.hasType (ids::POINT))
            out.push_back (c);

    std::stable_sort (out.begin(), out.end(), [] (const auto& a, const auto& b) { return (double) a[ids::beat] < (double) b[ids::beat]; });
    return out;
}

juce::ValueTree Session::makePoint (double beat, double value)
{
    return juce::ValueTree (ids::POINT, { { ids::beat, beat }, { ids::val, value } });
}

juce::ValueTree Session::track (int index) const
{
    int n = 0;

    for (const auto& c : state)
        if (c.hasType (ids::TRACK) && n++ == index)
            return c;

    return {};
}

juce::ValueTree Session::addTrack (const juce::String& trackName, int input, juce::UndoManager* um)
{
    juce::ValueTree t (ids::TRACK);
    t.setProperty (ids::kind, "audio", nullptr)
        .setProperty (ids::name, trackName, nullptr)
        .setProperty (ids::input, input, nullptr)
        .setProperty (ids::armed, false, nullptr)
        .setProperty (ids::mute, false, nullptr)
        .setProperty (ids::solo, false, nullptr)
        .setProperty (ids::monitor, false, nullptr)
        .setProperty (ids::gainDb, 0.0, nullptr);

    // Keep tracks together, ahead of markers.
    int insertAt = 0;

    for (int i = 0; i < state.getNumChildren(); ++i)
        if (state.getChild (i).hasType (ids::TRACK))
            insertAt = i + 1;

    state.addChild (t, insertAt, um);
    return t;
}

std::vector<juce::ValueTree> Session::clips (const juce::ValueTree& track)
{
    std::vector<juce::ValueTree> out;

    for (const auto& c : track)
        if (c.hasType (ids::CLIP))
            out.push_back (c);

    return out;
}

juce::ValueTree Session::lastClip (const juce::ValueTree& track)
{
    for (int i = track.getNumChildren(); --i >= 0;)
        if (track.getChild (i).hasType (ids::CLIP))
            return track.getChild (i);

    return {};
}

juce::ValueTree Session::makeClip (const juce::String& file, juce::int64 start, juce::int64 offset, juce::int64 length)
{
    juce::ValueTree c (ids::CLIP);
    c.setProperty (ids::file, file, nullptr)
        .setProperty (ids::start, start, nullptr)
        .setProperty (ids::offset, offset, nullptr)
        .setProperty (ids::length, length, nullptr);
    return c;
}

std::vector<juce::ValueTree> Session::playlists (const juce::ValueTree& track)
{
    std::vector<juce::ValueTree> out;

    for (const auto& c : track)
        if (c.hasType (ids::PLAYLIST))
            out.push_back (c);

    return out;
}

juce::String Session::mainPlaylistName (const juce::ValueTree& track)
{
    return track.hasProperty (ids::playlist) ? track[ids::playlist].toString() : track[ids::name].toString();
}

juce::String Session::nextPlaylistName (const juce::ValueTree& track)
{
    const auto base = track[ids::name].toString();
    juce::StringArray names { mainPlaylistName (track) };

    for (const auto& p : playlists (track))
        names.add (p[ids::name].toString());

    int top = 0;

    for (const auto& n : names)
        if (n.startsWith (base + ".") && n.substring (base.length() + 1).containsOnly ("0123456789") && n.length() > base.length() + 1)
            top = juce::jmax (top, n.substring (base.length() + 1).getIntValue());

    for (int k = top + 1;; ++k)
        if (const auto name = base + "." + juce::String (k).paddedLeft ('0', 2); ! names.contains (name))
            return name;
}

juce::ValueTree Session::soloedPlaylist (const juce::ValueTree& track)
{
    for (const auto& p : playlists (track))
        if ((bool) p[ids::solo])
            return p;

    return {};
}

juce::ValueTree Session::makeHit (double beat, float vel)
{
    juce::ValueTree h (ids::HIT);
    h.setProperty (ids::beat, beat, nullptr).setProperty (ids::vel, juce::jlimit (0.0f, 1.0f, vel), nullptr);
    return h;
}

std::vector<juce::ValueTree> Session::hitClips (const juce::ValueTree& track)
{
    std::vector<juce::ValueTree> out;

    for (const auto& c : track)
        if (c.hasType (ids::HITCLIP))
            out.push_back (c);

    return out;
}

juce::ValueTree Session::makeHitClip (double beat, double offset, double len)
{
    juce::ValueTree c (ids::HITCLIP);
    c.setProperty (ids::beat, beat, nullptr).setProperty (ids::offset, offset, nullptr).setProperty (ids::len, len, nullptr);
    return c;
}

std::vector<juce::ValueTree> Session::beatClips (const juce::ValueTree& track)
{
    std::vector<juce::ValueTree> out;

    for (const auto& c : track)
        if (isBeatClip (c))
            out.push_back (c);

    return out;
}

juce::ValueTree Session::makeBeatClip (const juce::ValueTree& kindOf, double beat, double offset, double len)
{
    return kindOf.hasType (ids::HITCLIP) ? makeHitClip (beat, offset, len) : makeMidiClip (beat, offset, len);
}

std::vector<juce::ValueTree> Session::midiClips (const juce::ValueTree& track)
{
    std::vector<juce::ValueTree> out;

    for (const auto& c : track)
        if (c.hasType (ids::MIDICLIP))
            out.push_back (c);

    return out;
}

juce::ValueTree Session::lastMidiClip (const juce::ValueTree& track)
{
    for (int i = track.getNumChildren(); --i >= 0;)
        if (track.getChild (i).hasType (ids::MIDICLIP))
            return track.getChild (i);

    return {};
}

juce::ValueTree Session::makeMidiClip (double beat, double offset, double len)
{
    juce::ValueTree c (ids::MIDICLIP);
    c.setProperty (ids::beat, beat, nullptr).setProperty (ids::offset, offset, nullptr).setProperty (ids::len, len, nullptr);
    return c;
}

// A hair of slack, so a note on the clip's first beat is in it whatever the rounding.
static constexpr double windowSlack = 1.0e-9;

bool Session::inWindow (const juce::ValueTree& clip, double contentBeat)
{
    const double offset = clip[ids::offset];
    return contentBeat >= offset - windowSlack && contentBeat < offset + (double) clip[ids::len] - windowSlack;
}

void Session::growMidiClip (juce::ValueTree clip, double from, double to, juce::UndoManager* um)
{
    const auto zero = contentZero (clip), end = std::max (clipEnd (clip), to);

    if (from < (double) clip[ids::beat])
        clip.setProperty (ids::beat, from, um).setProperty (ids::offset, from - zero, um);

    if (end > clipEnd (clip))
        clip.setProperty (ids::len, end - (double) clip[ids::beat], um);
}

std::vector<Session::PlacedNote> Session::placedNotes (const juce::ValueTree& track)
{
    std::vector<PlacedNote> out;

    for (const auto& clip : midiClips (track))
    {
        const auto zero = contentZero (clip), end = clipEnd (clip);

        for (const auto& n : clip)
            if (n.hasType (ids::NOTE) && inWindow (clip, n[ids::beat]))
            {
                const auto start = zero + (double) n[ids::beat];
                out.push_back ({ n, clip, start, std::min (end, start + (double) n[ids::len]) });
            }
    }

    return out;
}

std::vector<Session::PlacedHit> Session::placedHits (const juce::ValueTree& track)
{
    std::vector<PlacedHit> out;

    for (const auto& clip : hitClips (track))
        for (const auto& h : clip)
            if (h.hasType (ids::HIT) && inWindow (clip, h[ids::beat]))
                out.push_back ({ h, clip, contentZero (clip) + (double) h[ids::beat], (float) (double) h[ids::vel] });

    return out;
}

std::vector<Session::PlacedControl> Session::placedControls (const juce::ValueTree& track)
{
    std::vector<PlacedControl> out;

    for (const auto& clip : midiClips (track))
        for (const auto& c : clip)
            if (c.hasType (ids::CC) && inWindow (clip, c[ids::beat]))
                out.push_back ({ c, clip, contentZero (clip) + (double) c[ids::beat] });

    return out;
}

juce::ValueTree Session::trimmedMidiClip (const juce::ValueTree& clip, double from, double to)
{
    const auto lo = std::max (from, (double) clip[ids::beat]), hi = std::min (to, clipEnd (clip));

    if (hi <= lo)
        return {};

    const auto zero = contentZero (clip);
    auto out = makeBeatClip (clip, lo, lo - zero, hi - lo);

    for (const auto& n : clip)
        if (inWindow (out, n[ids::beat]))
            out.appendChild (n.createCopy(), nullptr);

    return out;
}

juce::ValueTree Session::makeNote (double beat, double len, int pitch, int vel, int channel)
{
    juce::ValueTree n (ids::NOTE);
    n.setProperty (ids::beat, beat, nullptr)
        .setProperty (ids::len, juce::jmax (1.0 / 256.0, len), nullptr)
        .setProperty (ids::pitch, juce::jlimit (0, 127, pitch), nullptr)
        .setProperty (ids::vel, juce::jlimit (1, 127, vel), nullptr);

    if (channel != 1)
        n.setProperty (ids::ch, juce::jlimit (1, 16, channel), nullptr);

    return n;
}

juce::ValueTree Session::makeControl (double beat, int num, int val, int channel)
{
    juce::ValueTree c (ids::CC);
    c.setProperty (ids::beat, beat, nullptr).setProperty (ids::num, num, nullptr).setProperty (ids::val, val, nullptr);

    if (channel != 1)
        c.setProperty (ids::ch, juce::jlimit (1, 16, channel), nullptr);

    return c;
}

juce::int64 Session::contentEnd() const
{
    juce::int64 end = 0;

    for (const auto& t : state)
    {
        const bool sampleTrack = isSampleTrack (t);

        for (const auto& c : t)
        {
            if (! sampleTrack && c.hasType (ids::CLIP))
                end = juce::jmax (end, (juce::int64) c[ids::start] + (juce::int64) c[ids::length]);
            else if (isBeatClip (c))
                end = juce::jmax (end, (juce::int64) std::llround (clipEnd (c) * samplesPerBeat()));
        }
    }

    return end;
}

// Version 1 tracks played only their last TAKE. That take becomes a CLIP; older takes stay in
// the file as history (they never played, and still don't).
static void migrateTakesToClips (juce::ValueTree& session)
{
    for (auto track : session)
    {
        if (! track.hasType (ids::TRACK))
            continue;

        for (int i = track.getNumChildren(); --i >= 0;)
        {
            const auto take = track.getChild (i);

            if (take.hasType (ids::TAKE))
            {
                // Latency compensation could push a take recorded from zero to a negative start.
                const auto start = (juce::int64) take[ids::start];
                const auto offset = start < 0 ? -start : (juce::int64) 0;
                track.addChild (Session::makeClip (take[ids::file], start + offset, offset, (juce::int64) take[ids::length] - offset),
                                i + 1, nullptr);
                break;
            }
        }
    }

    session.setProperty (ids::version, 2, nullptr);
}

// Version 2 kept an instrument track's notes and controllers on the track. They go into one
// MIDI clip covering them, out to whole bars; its content beats are the timeline's.
static void migrateNotesToMidiClips (juce::ValueTree& session)
{
    const auto beatsPerBar = (double) juce::jmax (1, (int) session.getProperty (ids::timeSigNum, 4));

    for (auto track : session)
    {
        if (! track.hasType (ids::TRACK))
            continue;

        std::vector<juce::ValueTree> loose;
        double first = 0.0, last = 0.0;

        for (const auto& c : track)
            if (c.hasType (ids::NOTE) || c.hasType (ids::CC))
            {
                const double beat = c[ids::beat], end = beat + (c.hasType (ids::NOTE) ? (double) c[ids::len] : 0.0);
                first = loose.empty() ? beat : std::min (first, beat);
                last = loose.empty() ? end : std::max (last, end);
                loose.push_back (c);
            }

        if (loose.empty())
            continue;

        const auto from = std::max (0.0, std::floor (first / beatsPerBar) * beatsPerBar);
        const auto to = std::max (from + beatsPerBar, std::ceil (last / beatsPerBar) * beatsPerBar);
        auto clip = Session::makeMidiClip (from, from, to - from);

        for (auto& c : loose)
        {
            track.removeChild (c, nullptr);
            clip.appendChild (c, nullptr);
        }

        track.appendChild (clip, nullptr);
    }

    session.setProperty (ids::version, 3, nullptr);
}

// Version 3 kept a sample track's hits on the track. They go into one hit clip covering them, out
// to whole bars, its content beats the timeline's, as notes did going to version 3.
static void wrapLooseHits (juce::ValueTree& session)
{
    const auto beatsPerBar = (double) juce::jmax (1, (int) session.getProperty (ids::timeSigNum, 4));

    for (auto track : session)
    {
        if (! track.hasType (ids::TRACK))
            continue;

        std::vector<juce::ValueTree> loose;
        double first = 0.0, last = 0.0;

        for (const auto& c : track)
            if (c.hasType (ids::HIT))
            {
                const double beat = c[ids::beat];
                first = loose.empty() ? beat : std::min (first, beat);
                last = loose.empty() ? beat : std::max (last, beat);
                loose.push_back (c);
            }

        if (loose.empty())
            continue;

        const auto from = std::max (0.0, std::floor (first / beatsPerBar) * beatsPerBar);
        const auto to = std::max (from + beatsPerBar, std::floor (last / beatsPerBar + 1.0) * beatsPerBar); // the bar the last hit is in
        auto clip = Session::makeHitClip (from, from, to - from);

        for (auto& c : loose)
        {
            track.removeChild (c, nullptr);
            clip.appendChild (c, nullptr);
        }

        track.appendChild (clip, nullptr);
    }

    session.setProperty (ids::version, Session::currentVersion, nullptr);
}

//==============================================================================
juce::File configFolder()
{
    // %APPDATA%\taw on Windows; ~/Library/Application Support/taw on macOS. TAW_CONFIG points
    // elsewhere (tests, a second setup).
    if (const auto custom = juce::SystemStats::getEnvironmentVariable ("TAW_CONFIG", {}); custom.isNotEmpty())
        return juce::File::getCurrentWorkingDirectory().getChildFile (custom);

    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    return base.getChildFile ("taw");
}

static juce::ValueTree makeTemplate (int numMics)
{
    juce::ValueTree s (ids::SESSION);
    s.setProperty (ids::bpm, 120.0, nullptr);

    for (int i = 1; i <= numMics; ++i)
    {
        juce::ValueTree t (ids::TRACK);
        t.setProperty (ids::name, numMics == 1 ? juce::String ("Mic") : "Mic " + juce::String (i), nullptr);
        t.setProperty (ids::input, i, nullptr);
        s.appendChild (t, nullptr);
    }

    return s;
}

static juce::File recentFile() { return configFolder().getChildFile ("recent.json"); }

std::vector<juce::File> loadRecentSessions()
{
    std::vector<juce::File> out;
    juce::var parsed;

    if (juce::JSON::parse (recentFile().loadFileAsString(), parsed).wasOk())
        if (auto* list = parsed.getArray())
            for (const auto& v : *list)
                if (const juce::File f (v.toString()); Session::existsIn (f))
                    out.push_back (f);

    return out;
}

static bool recentEnabled = false;

void enableRecentSessions() { recentEnabled = true; }

void rememberSession (const juce::File& folder)
{
    // Test and headless runs (--exec on a scratch copy, say) must never show up in the list.
    if (! recentEnabled)
        return;

    constexpr size_t maxRecent = 12;
    auto recent = loadRecentSessions();
    recent.erase (std::remove (recent.begin(), recent.end(), folder), recent.end());
    recent.insert (recent.begin(), folder);

    if (recent.size() > maxRecent)
        recent.resize (maxRecent);

    juce::Array<juce::var> list;

    for (const auto& f : recent)
        list.add (f.getFullPathName());

    configFolder().createDirectory();
    recentFile().replaceWithText (juce::JSON::toString (list));
}

static juce::File bouncesFile() { return configFolder().getChildFile ("bounces.json"); }

std::vector<BounceRecord> loadRecentBounceRecords()
{
    std::vector<BounceRecord> out;
    juce::var parsed;

    if (juce::JSON::parse (bouncesFile().loadFileAsString(), parsed).wasOk())
        if (auto* list = parsed.getArray())
            for (const auto& v : *list)
                if (const juce::File f (v["file"].toString()); f.existsAsFile())
                    out.push_back ({ f, v["session"].toString(), juce::File (v["folder"].toString()) });

    return out;
}

void rememberBounce (const juce::File& file, const juce::String& session, const juce::File& folder)
{
    if (! recentEnabled)
        return;

    constexpr size_t maxBounces = 60;
    auto list = loadRecentBounceRecords();
    list.erase (std::remove_if (list.begin(), list.end(), [&] (const auto& b) { return b.file == file; }), list.end());
    list.insert (list.begin(), { file, session, folder });

    if (list.size() > maxBounces)
        list.resize (maxBounces);

    juce::Array<juce::var> out;

    for (const auto& b : list)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("file", b.file.getFullPathName());
        o->setProperty ("session", b.session);
        o->setProperty ("folder", b.folder.getFullPathName());
        out.add (juce::var (o));
    }

    configFolder().createDirectory();
    bouncesFile().replaceWithText (juce::JSON::toString (out));
}

std::vector<Template> loadTemplates()
{
    const auto dir = configFolder().getChildFile ("templates");

    if (! dir.isDirectory())
    {
        dir.createDirectory();

        const std::pair<const char*, int> builtIns[] = { { "1mic", 1 }, { "3mic", 3 }, { "blank", 0 } };

        for (const auto& [n, mics] : builtIns)
            dir.getChildFile (juce::String (n) + ".json")
                .replaceWithText (juce::JSON::toString (treeToVar (makeTemplate (mics)),
                                                        juce::JSON::FormatOptions{}.withSpacing (juce::JSON::Spacing::multiLine)));
    }

    std::vector<Template> out;

    for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        juce::var parsed;

        if (juce::JSON::parse (f.loadFileAsString(), parsed).wasOk())
            if (auto tree = varToTree (parsed); tree.hasType (ids::SESSION))
                out.push_back ({ f.getFileNameWithoutExtension(), tree });
    }

    std::sort (out.begin(), out.end(), [] (const auto& a, const auto& b) { return a.name < b.name; });
    return out;
}
} // namespace taw
