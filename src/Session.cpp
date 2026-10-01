#include "Session.h"

namespace taw
{
static const juce::Identifier typeKey { "type" }, childrenKey { "children" };

static void migrateTakesToClips (juce::ValueTree& session);

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
    def (ids::click, false);
    def (ids::clickWhen, "rec");
    def (ids::clickDb, -6.0);
    def (ids::mixDb, 0.0);
    def (ids::loop, false);
    def (ids::loopStart, (juce::int64) 0);
    def (ids::loopEnd, (juce::int64) 0);
    def (ids::bounceFormat, "wav");
    def (ids::bounceFolder, "bounces");

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
    const auto file = dir.getChildFile ("session.json");
    auto tree = readSessionFile (file, error);
    juce::String note;

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
            return {};
    }

    if ((int) tree.getProperty (ids::version, 1) < 2)
        migrateTakesToClips (tree);

    applyDefaults (tree);
    dir.getChildFile ("audio").createDirectory();

    if (note.isEmpty())
        backUp (dir);

    std::unique_ptr<Session> s (new Session (dir, tree));
    s->loadNote = note;
    return s;
}

bool Session::save() const
{
    const auto json = juce::JSON::toString (treeToVar (state), juce::JSON::FormatOptions{}.withSpacing (juce::JSON::Spacing::multiLine));
    return dir.getChildFile ("session.json").replaceWithText (json + "\n");
}

int Session::numTracks() const
{
    int n = 0;

    for (const auto& c : state)
        n += c.hasType (ids::TRACK) ? 1 : 0;

    return n;
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

std::vector<juce::ValueTree> Session::hits (const juce::ValueTree& track)
{
    std::vector<juce::ValueTree> out;

    for (const auto& c : track)
        if (c.hasType (ids::HIT))
            out.push_back (c);

    return out;
}

juce::ValueTree Session::makeHit (double beat, float vel)
{
    juce::ValueTree h (ids::HIT);
    h.setProperty (ids::beat, beat, nullptr).setProperty (ids::vel, juce::jlimit (0.0f, 1.0f, vel), nullptr);
    return h;
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
            else if (sampleTrack && c.hasType (ids::HIT))
                end = juce::jmax (end, (juce::int64) std::llround ((double) c[ids::beat] * samplesPerBeat()));
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

    session.setProperty (ids::version, Session::currentVersion, nullptr);
}

//==============================================================================
juce::File configFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("taw");
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
