// Agents in the open session (docs/plan-taw-mcp.md): taw's socket, the listing that says where
// it is, the tool list, and the bridge (taw --mcp) that speaks MCP to an agent over stdio.

#include "Agents.h"
#include "Safety.h"
#include "Session.h"

#include <iostream>
#include <string>

#if JUCE_WINDOWS
 #include <fcntl.h>
 #include <io.h>
#endif

namespace taw::agents
{
namespace
{
constexpr juce::uint32 connectionMagic = 0x74617761; // "tawa" (the hub's control channel is "tawc")

juce::String toJson (const juce::var& v) { return juce::JSON::toString (v, true); }

void sendJson (juce::InterprocessConnection& c, const juce::var& v)
{
    const auto text = toJson (v);
    c.sendMessage (juce::MemoryBlock (text.toRawUTF8(), text.getNumBytesAsUTF8()));
}

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();

    for (const auto& [k, v] : props)
        o->setProperty (k, v);

    return o;
}
} // namespace

juce::File folder() { return configFolder().getChildFile ("agents"); }

std::vector<Listing> running()
{
    std::vector<Listing> out;

    for (const auto& f : folder().findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        const auto v = juce::JSON::parse (f);
        Listing l;
        l.pid = (juce::uint32) (juce::int64) v["pid"];
        l.port = v["port"];
        l.token = v["token"].toString();
        l.session = v["session"].toString();
        l.name = v["name"].toString();
        l.started = juce::Time::fromISO8601 (v["started"].toString());

        if (l.port <= 0 || l.token.isEmpty())
            continue;

        // A taw that died left it behind; one of ours, from a test, is real.
        if (l.pid != safety::thisProcess() && ! safety::tawRunning (l.pid))
        {
            f.deleteFile();
            continue;
        }

        out.push_back (l);
    }

    std::sort (out.begin(), out.end(), [] (const auto& a, const auto& b)
               { return a.started > b.started; });
    return out;
}

//==============================================================================
// One connection: a bridge. Its messages arrive on its own thread; it parses and queues, and
// never touches the session.
struct Server::Connection final : juce::InterprocessConnection
{
    Connection (Server& s, int i) : juce::InterprocessConnection (false, connectionMagic), server (s), id (i) {}
    ~Connection() override { disconnect(); }

    void connectionMade() override {}
    void connectionLost() override { dead = true; }
    void messageReceived (const juce::MemoryBlock& m) override { server.received (*this, juce::JSON::parse (m.toString())); }

    Server& server;
    const int id;
    juce::String agent;
    bool welcomed = false;
    std::atomic<bool> dead { false };
};

Server::Server() : secret (juce::Uuid().toString()), startedAt (juce::Time::getCurrentTime()) {}

Server::~Server()
{
    stop(); // no more connections come in

    std::vector<std::unique_ptr<Connection>> closing;
    {
        const juce::ScopedLock sl (lock);
        closing.swap (open);
    }

    closing.clear(); // each waits for its thread (outside the lock: their callbacks take it)

    if (listeningPort > 0)
        folder().getChildFile (juce::String (safety::thisProcess()) + ".json").deleteFile();
}

bool Server::start()
{
    if (! beginWaitingForSocket (0, "127.0.0.1"))
        return false;

    listeningPort = getBoundPort();
    writeListing();
    return listeningPort > 0;
}

void Server::writeListing() const
{
    if (listeningPort <= 0)
        return;

    folder().createDirectory();
    const auto v = object ({ { "pid", (juce::int64) safety::thisProcess() },
                             { "port", listeningPort },
                             { "token", secret },
                             { "session", sessionFolder == juce::File() ? juce::String() : sessionFolder.getFullPathName() },
                             { "name", sessionName },
                             { "started", startedAt.toISO8601 (true) } });
    folder().getChildFile (juce::String (safety::thisProcess()) + ".json").replaceWithText (juce::JSON::toString (v));
}

void Server::setSession (const juce::File& f, const juce::String& name)
{
    {
        const juce::ScopedLock sl (lock);

        if (f == sessionFolder && name == sessionName)
            return;

        sessionFolder = f;
        sessionName = name;
    }

    writeListing();
}

juce::InterprocessConnection* Server::createConnectionObject()
{
    const juce::ScopedLock sl (lock);
    open.push_back (std::make_unique<Connection> (*this, nextId++));
    return open.back().get();
}

void Server::received (Connection& c, const juce::var& m)
{
    const auto type = m["type"].toString();

    if (type == "hello")
    {
        const juce::ScopedLock sl (lock);

        if (m["token"].toString() != secret)
        {
            sendJson (c, object ({ { "type", "refused" }, { "reason", "wrong token (is the agents listing from another taw?)" } }));
            return;
        }

        c.agent = m["agent"].toString().trim().substring (0, 40);

        if (c.agent.isEmpty())
            c.agent = "agent";

        c.welcomed = true;
        sendJson (c, object ({ { "type", "welcome" }, { "session", sessionName }, { "folder", sessionFolder.getFullPathName() } }));
        return;
    }

    if (type == "call")
    {
        const juce::ScopedLock sl (lock);

        if (! c.welcomed)
        {
            sendJson (c, object ({ { "type", "result" }, { "id", m["id"] }, { "ok", false }, { "code", "invalid" }, { "text", "say hello first" } }));
            return;
        }

        queue.push_back ({ c.id, m["id"], c.agent, m["tool"].toString(), m["args"] });
        return;
    }

    // Anything else (not JSON, an unknown type) is ignored; the connection stays.
}

std::optional<Call> Server::next()
{
    std::vector<std::unique_ptr<Connection>> gone;
    std::optional<Call> call;

    {
        const juce::ScopedLock sl (lock);

        if (! queue.empty())
        {
            call = std::move (queue.front());
            queue.erase (queue.begin());
        }

        for (auto it = open.begin(); it != open.end();)
        {
            if ((*it)->dead)
            {
                gone.push_back (std::move (*it));
                it = open.erase (it);
            }
            else
                ++it;
        }
    }

    return call; // gone's connections are let go here, outside the lock
}

void Server::answer (const Call& call, const Answer& a)
{
    const auto reply = object ({ { "type", "result" }, { "id", call.id }, { "ok", a.ok }, { "code", a.code }, { "text", a.text }, { "data", a.data } });
    const juce::ScopedLock sl (lock);

    for (auto& c : open)
        if (c->id == call.connection && ! c->dead)
            sendJson (*c, reply);
}

int Server::connections() const
{
    const juce::ScopedLock sl (lock);
    return (int) open.size();
}

//==============================================================================
// The tools as tools/list gives them. taw (AgentEdits.cpp) runs them by these names.
juce::var toolList()
{
    static const juce::var tools = juce::JSON::parse (R"JSON([
{
  "name": "get_session",
  "description": "Read the session open in taw: tempo, time signature, where the user is (cursor, focused track, time selection), markers, a tempo change waiting on the user (tempo_request), and every track with its kind, plugin, routing (output, sends, inserts), kit membership and clips. Positions are in beats from the session start (beat 0 is bar 1). Each track has an id (\"t3\") that stays the same through moves, renames, undo and saves: pass it to the other tools as track. Its number (\"3\", or \"2.3\" for pad 3 of kit 2) is what taw's screen and run_command use, and shifts as tracks come and go. A track the user is working on has \"locked\" saying why; edits to it are refused. Works while taw plays.",
  "inputSchema": { "type": "object", "properties": {
    "include_notes": { "type": "boolean", "description": "Every note and hit in every clip (default: only how many)." } } }
},
{
  "name": "get_track",
  "description": "One track in full: as in get_session, with every MIDI clip's notes and every hit clip's hits. Note and hit beats are from their clip's start.",
  "inputSchema": { "type": "object", "properties": {
    "track": { "type": "string", "description": "Its id from get_session (\"t3\"); its number as on screen or its exact name also work." } }, "required": ["track"] }
},
{
  "name": "run_command",
  "description": "Run one line of taw's command language, as the user would type it in taw: e.g. \"2 v -6\" (volume), \"2 n Bass\" (rename), \"3 m\" (mute), \"bpm 92\", \"sig 3/4\", \"4 pat /16 x.x.x.x.\" (a step pattern over the time selection or a bar), \"2 out Verb\", \"bus Verb\", \"2 send a Verb -12\", \"3 fx b cla-2a\", \"add inst vital\". Tracks by their number as on screen (get_session's number, not id). \"bpm\" goes through set_tempo's rules. One undo step. Not allowed: save, close, quit, play, stop, record, undo/redo, bounce, moving the cursor (@), focusing, the clipboard, and anything that opens a window. A command with no track number acts on the user's focused track, which is locked: always give the track number.",
  "inputSchema": { "type": "object", "properties": {
    "command": { "type": "string" } }, "required": ["command"] }
},
{
  "name": "add_track",
  "description": "Add a track at the bottom of the session. Returns its id (and number).",
  "inputSchema": { "type": "object", "properties": {
    "kind": { "type": "string", "enum": ["audio", "instrument", "sample", "bus", "kit"] },
    "name": { "type": "string" },
    "plugin": { "type": "string", "description": "instrument: the plugin, matched by name as taw's inst command does (e.g. \"vital\"). Without one the track has no instrument yet." },
    "sample": { "type": "string", "description": "sample: the audio file it plays (a full path). Without one it has none yet." } }, "required": ["kind"] }
},
{
  "name": "remove_track",
  "description": "Remove a track (as taw's \"3 remove\": what fed a bus goes back to the mix).",
  "inputSchema": { "type": "object", "properties": {
    "track": { "type": "string" } }, "required": ["track"] }
},
{
  "name": "write_notes",
  "description": "Write MIDI notes on an instrument track: into an existing MIDI clip (clip: its index from get_track; it grows to fit) or into a new clip starting at beat. Note beats are from the clip's start. One undo step.",
  "inputSchema": { "type": "object", "properties": {
    "track": { "type": "string" },
    "clip": { "type": "integer", "description": "An existing clip's index. Leave out for a new clip." },
    "clip_beat": { "type": "number", "description": "That clip's start beat as read: refused if the clips have changed since." },
    "beat": { "type": "number", "description": "A new clip's start on the timeline, in beats (0 is bar 1)." },
    "length": { "type": "number", "description": "A new clip's length in beats (default: whole bars around the notes)." },
    "replace": { "type": "boolean", "description": "Clear the clip's notes first." },
    "notes": { "type": "array", "items": { "type": "object", "properties": {
      "beat": { "type": "number" }, "length": { "type": "number" },
      "pitch": { "type": "integer", "description": "MIDI note, 60 = C4" },
      "vel": { "type": "integer", "description": "1-127, default 100" },
      "ch": { "type": "integer", "description": "1-16, default 1" } }, "required": ["beat", "length", "pitch"] } } },
    "required": ["track", "notes"] }
},
{
  "name": "write_hits",
  "description": "Write hits on a sample track (each plays its sample once): into an existing hit clip (clip: its index; it grows to fit) or into a new clip starting at beat. Hit beats are from the clip's start. One undo step.",
  "inputSchema": { "type": "object", "properties": {
    "track": { "type": "string" },
    "clip": { "type": "integer" },
    "clip_beat": { "type": "number" },
    "beat": { "type": "number" },
    "length": { "type": "number" },
    "replace": { "type": "boolean" },
    "hits": { "type": "array", "items": { "type": "object", "properties": {
      "beat": { "type": "number" },
      "vel": { "type": "number", "description": "0-1, default 0.8" } }, "required": ["beat"] } } },
    "required": ["track", "hits"] }
},
{
  "name": "remove_clip",
  "description": "Remove one clip (audio, MIDI or hits) from a track, by its index from get_track.",
  "inputSchema": { "type": "object", "properties": {
    "track": { "type": "string" }, "clip": { "type": "integer" }, "clip_beat": { "type": "number" } }, "required": ["track", "clip"] }
},
{
  "name": "add_marker",
  "description": "Drop a named marker at a beat.",
  "inputSchema": { "type": "object", "properties": {
    "beat": { "type": "number" }, "name": { "type": "string" } }, "required": ["beat", "name"] }
},
{
  "name": "set_tempo",
  "description": "Change the session's tempo. MIDI and hits follow it (they're in beats). If the session has audio clips, nothing changes yet: taw asks the user whether the audio stays where it is in time or moves with the beats, and this answers needs-user. Call again (or read get_session's tempo_request) to see what they chose; declined means they said no.",
  "inputSchema": { "type": "object", "properties": {
    "bpm": { "type": "number", "description": "20-400" } }, "required": ["bpm"] }
},
{
  "name": "get_parameters",
  "description": "The plugin parameters write_automation can automate on a track (its instrument, then its inserts) or on the mix (its inserts): each one's param string, name, plugin, value now (0 to 1) and how the plugin shows it. Up to 200; search narrows them by name (\"cutoff\").",
  "inputSchema": { "type": "object", "properties": {
    "track": { "type": "string", "description": "A track (id, number or name), or \"mix\"." },
    "search": { "type": "string" } }, "required": ["track"] }
},
{
  "name": "write_automation",
  "description": "Write automation points on a track's (or the mix's) lane, making the lane if there isn't one, and show it under the track. param: \"vol\", \"pan\", \"mute\", \"send a\" to \"send e\", or a plugin parameter (its param from get_parameters, or its name, e.g. \"cutoff\"); the mix has vol and its inserts' parameters. Points: beat from the session start (beat 0 is bar 1, as in get_session) and value in the lane's units: dB for vol and sends (-96 to 12), -1 (left) to 1 (right) for pan, 0 or 1 for mute (held, not ramped), 0 to 1 for plugin parameters; or as text as taw takes it (\"-6\", \"l30\", \"on\", \"40%\"). Values ramp in a line from point to point and hold after the last. By default the lane's points between the first and last beat written are replaced and the rest kept; replace \"all\" replaces the whole lane (with no points: empties it). One undo step. get_track reads a track's lanes back.",
  "inputSchema": { "type": "object", "properties": {
    "track": { "type": "string", "description": "A track (id, number or name), or \"mix\"." },
    "param": { "type": "string" },
    "points": { "type": "array", "items": { "type": "object", "properties": {
      "beat": { "type": "number" }, "value": { "type": ["number", "string"] } }, "required": ["beat", "value"] } },
    "replace": { "type": "string", "enum": ["range", "all"], "description": "range (default): only between the first and last beat written; all: the whole lane." },
    "show": { "type": "boolean", "description": "Open the lane under the track (default true)." } }, "required": ["track", "param", "points"] }
},
{
  "name": "list_sessions",
  "description": "The sessions taw knows (recent ones and those in the sessions folder, newest first), the templates new_session can start from, which session is open (none: taw is on its session screen), and a session switch waiting on the user (session_request). Works with no session open.",
  "inputSchema": { "type": "object", "properties": {} }
},
{
  "name": "new_session",
  "description": "Make a new session in the sessions folder (its folder gets today's date, as taw names new sessions) from a template, and open it in taw. With no session open it happens now; answered while it opens, and edits work once it's in (busy until then). With one open, taw asks the user (save and switch, switch without saving, or stay) and this answers needs-user: call again with the same name to see what they chose; declined means they said no. Never closes or saves anything without the user.",
  "inputSchema": { "type": "object", "properties": {
    "name": { "type": "string", "description": "The session's name, e.g. \"bass idea\"." },
    "template": { "type": "string", "description": "A template from list_sessions (default: taw's own default)." } }, "required": ["name"] }
},
{
  "name": "open_session",
  "description": "Open an existing session in taw: by its folder, or its name (as list_sessions gives it). Asks the user as new_session does when another session is open.",
  "inputSchema": { "type": "object", "properties": {
    "session": { "type": "string", "description": "Its folder (a full path) or its name." } }, "required": ["session"] }
}
])JSON");
    return tools;
}

//==============================================================================
// The bridge's connection to taw: one call at a time, each answered by its id.
struct Bridge::Link final : juce::InterprocessConnection
{
    Link() : juce::InterprocessConnection (false, connectionMagic) {}
    ~Link() override { disconnect(); }

    void connectionMade() override {}

    void connectionLost() override
    {
        lost = true;
        arrived.signal();
    }

    void messageReceived (const juce::MemoryBlock& m) override
    {
        {
            const juce::ScopedLock sl (lock);
            inbox.push_back (juce::JSON::parse (m.toString()));
        }

        arrived.signal();
    }

    // The first message of this type (and id), waiting up to ms for it. Void if it didn't come.
    juce::var wait (const juce::String& type, const juce::var& id, int ms)
    {
        const auto until = juce::Time::getMillisecondCounterHiRes() + ms;

        for (;;)
        {
            {
                const juce::ScopedLock sl (lock);

                for (auto it = inbox.begin(); it != inbox.end(); ++it)
                    if ((*it)["type"].toString() == type && (id.isVoid() || (*it)["id"] == id))
                    {
                        auto v = *it;
                        inbox.erase (it);
                        return v;
                    }
            }

            const auto left = until - juce::Time::getMillisecondCounterHiRes();

            if (lost || left <= 0)
                return {};

            arrived.wait ((int) juce::jmin (left, 200.0));
        }
    }

    juce::CriticalSection lock;
    std::vector<juce::var> inbox;
    juce::WaitableEvent arrived;
    std::atomic<bool> lost { false };
};

Bridge::Bridge (juce::String sessionHint, juce::String agentName) : hint (std::move (sessionHint)), agent (std::move (agentName)) {}
Bridge::~Bridge() = default;

void Bridge::target (int port, const juce::String& token)
{
    fixedPort = port;
    fixedToken = token;
    link.reset();
}

bool Bridge::connect (juce::String& error)
{
    if (link != nullptr && link->isConnected() && ! link->lost)
        return true;

    link.reset();
    int port = fixedPort;
    auto token = fixedToken;

    if (port == 0)
    {
        // The newest taw with a session open (or the one --session names).
        const auto all = running();
        const Listing* pick = nullptr;

        for (const auto& l : all)
        {
            const bool matches = hint.isEmpty()
                                     ? l.session.isNotEmpty()
                                     : (juce::File (l.session) == juce::File::getCurrentWorkingDirectory().getChildFile (hint)
                                        || l.name.containsIgnoreCase (hint) || l.session.containsIgnoreCase (hint));

            if (matches)
            {
                pick = &l;
                break;
            }
        }

        // None has a session open: the newest taw, on its session screen (new_session and
        // open_session work there; the rest say no-session).
        if (pick == nullptr && hint.isEmpty() && ! all.empty())
            pick = &all.front();

        if (pick == nullptr)
        {
            error = all.empty()      ? juce::String ("taw isn't running (start taw and open a session)")
                    : hint.isEmpty() ? juce::String ("taw is running but has no session open")
                                     : "no running taw has a session matching " + hint.quoted();

            // Turned off (settings.json, every taw): nothing listens, and that's why.
            if (! (bool) juce::JSON::parse (configFolder().getChildFile ("settings.json")).getProperty ("agents", true))
                error = "agents are turned off in taw: the user can type agents on there (or switch it in the project panel)";

            return false;
        }

        port = pick->port;
        token = pick->token;
    }

    auto l = std::make_unique<Link>();

    if (! l->connectToSocket ("127.0.0.1", port, 2000))
    {
        error = "couldn't reach taw on port " + juce::String (port);
        return false;
    }

    sendJson (*l, object ({ { "type", "hello" }, { "token", token }, { "agent", agent.isNotEmpty() ? agent : juce::String ("agent") } }));

    if (const auto reply = l->wait ("welcome", {}, 3000); reply.isVoid())
    {
        const auto refused = l->wait ("refused", {}, 10);
        error = refused.isVoid() ? juce::String ("taw didn't answer") : "taw refused: " + refused["reason"].toString();
        return false;
    }

    link = std::move (l);
    return true;
}

Answer Bridge::call (const juce::String& tool, const juce::var& args)
{
    juce::String error;

    if (! connect (error))
        return Answer::fail (error.startsWith ("agents are turned off") ? "agents-off" : "no-session", error);

    const auto id = nextId++;
    sendJson (*link, object ({ { "type", "call" }, { "id", id }, { "tool", tool }, { "args", args } }));

    // Imports (a sample file) can take a while; anything else is a frame or two.
    const auto reply = link->wait ("result", id, 120000);

    if (reply.isVoid())
    {
        link.reset();
        return Answer::fail ("failed", "taw went away before answering (the edit may or may not have happened: read the session again)");
    }

    Answer a;
    a.ok = reply["ok"];
    a.code = reply["code"].toString();
    a.text = reply["text"].toString();
    a.data = reply["data"];
    return a;
}

juce::String Bridge::handle (const juce::String& line)
{
    if (line.trim().isEmpty())
        return {};

    const auto m = juce::JSON::parse (line);
    const auto id = m["id"];
    const auto method = m["method"].toString();
    const bool notification = ! m.hasProperty ("id");

    auto reply = [&] (const juce::var& result)
    { return toJson (object ({ { "jsonrpc", "2.0" }, { "id", id }, { "result", result } })); };
    auto error = [&] (int code, const juce::String& text)
    { return toJson (object ({ { "jsonrpc", "2.0" }, { "id", id }, { "error", object ({ { "code", code }, { "message", text } }) } })); };

    if (! m.isObject())
        return toJson (object ({ { "jsonrpc", "2.0" }, { "id", juce::var() }, { "error", object ({ { "code", -32700 }, { "message", "parse error" } }) } }));

    if (method == "initialize")
    {
        const auto& params = m["params"];

        if (agent.isEmpty())
            agent = params["clientInfo"]["name"].toString();

        // The client's version when it's one we know; else the newest we speak.
        const auto asked = params["protocolVersion"].toString();
        const juce::StringArray known { "2024-11-05", "2025-03-26", "2025-06-18" };

        return reply (object ({ { "protocolVersion", known.contains (asked) ? asked : juce::String ("2025-06-18") },
                                { "capabilities", object ({ { "tools", object ({}) } }) },
                                { "serverInfo", object ({ { "name", "taw" }, { "version", TAW_VERSION } }) },
                                { "instructions",
                                  "taw is a terminal DAW. These tools read and edit the session open in the user's running taw, live, while the user "
                                  "works in it. Read with get_session first. Edits are refused (and nothing changes) on anything the user edited "
                                  "in the last few seconds, while taw plays or records, and, while the user is at the keys, on the track their "
                                  "cursor is on and inside their time selection (once they've left the keys for a while those are open to you: "
                                  "a selected track is often the one they want worked on). Read again, pick other tracks, or try again in a "
                                  "moment. Each edit is one undo step the user can take back; you never save (the user saves). Positions are "
                                  "beats from the session start; tracks go by the ids get_session gives (\"t3\"), which never change. "
                                  "write_automation draws volume, pan, mute, sends and plugin parameters (get_parameters finds them). A tempo "
                                  "change on a session with audio, or switching sessions, waits for the user (needs-user): don't press them, "
                                  "call again later." } }));
    }

    if (notification)
        return {}; // notifications/initialized, cancelled, ...: nothing to say

    if (method == "ping")
        return reply (object ({}));

    if (method == "tools/list")
        return reply (object ({ { "tools", toolList() } }));

    if (method == "tools/call")
    {
        const auto& params = m["params"];
        const auto name = params["name"].toString();
        bool known = false;

        if (auto* tools = toolList().getArray())
            for (const auto& t : *tools)
                known = known || t["name"].toString() == name;

        if (! known)
            return error (-32602, "no tool called " + name.quoted());

        const auto a = call (name, params["arguments"].isObject() ? params["arguments"] : juce::var (object ({})));
        auto text = a.text;

        if (! a.ok)
            text = "[" + a.code + "] " + a.text;
        else if (! a.data.isVoid())
            text = (text.isNotEmpty() ? text + "\n" : juce::String()) + toJson (a.data); // one line: fewer tokens

        return reply (object ({ { "content", juce::Array<juce::var> { object ({ { "type", "text" }, { "text", text } }) } }, { "isError", ! a.ok } }));
    }

    return error (-32601, "no method " + method.quoted());
}

int runBridge (const juce::StringArray& args)
{
#if JUCE_WINDOWS
    // Lines end in \n alone: JSON-RPC over stdio, not text.
    _setmode (_fileno (stdout), _O_BINARY);
#endif

    const auto session = args.indexOf ("--session");
    const auto name = args.indexOf ("--name");
    Bridge bridge (session >= 0 ? args[session + 1] : juce::String(), name >= 0 ? args[name + 1] : juce::String());

    for (std::string line; std::getline (std::cin, line);)
    {
        const auto out = bridge.handle (juce::String::fromUTF8 (line.data(), (int) line.size()));

        if (out.isNotEmpty())
        {
            std::fwrite (out.toRawUTF8(), 1, out.getNumBytesAsUTF8(), stdout);
            std::fputc ('\n', stdout);
            std::fflush (stdout);
        }
    }

    return 0;
}
} // namespace taw::agents
