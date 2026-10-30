#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include <memory>
#include <optional>
#include <vector>

// Agents in the open session (docs/plan-taw-mcp.md). A running taw listens on a local socket;
// taw --mcp, the bridge, speaks MCP over stdio to an agent (Claude Code, any MCP client) and
// relays its tool calls there. The tools themselves run on taw's message thread
// (AgentEdits.cpp), one at a time, as a person's edits do.
namespace taw::agents
{
// <config>/agents/<pid>.json: where a running taw listens, for the bridge to find it.
juce::File folder();

struct Listing
{
    juce::uint32 pid = 0;
    int port = 0;
    juce::String token, session, name; // session: its folder ("" with none open)
    juce::Time started;
};

// The taws running now with their socket open, newest first.
std::vector<Listing> running();

// One call from an agent, as taw's message thread gets it.
struct Call
{
    int connection = 0;
    juce::var id;
    juce::String agent, tool;
    juce::var args;
};

// What the tools answer. code says why it refused: playing, hands-off, busy, no-session,
// invalid, failed.
struct Answer
{
    bool ok = true;
    juce::String code, text;
    juce::var data; // a read's JSON, or what an edit made ({ track: "4" })

    static Answer good (juce::String text, juce::var data = {}) { return { true, {}, std::move (text), std::move (data) }; }
    static Answer fail (juce::String code, juce::String text) { return { false, std::move (code), std::move (text), {} }; }
};

// taw's side: the socket. Connections run on their own threads and only parse and queue; the
// message thread takes the calls (take) and answers them (answer). Nothing a connection sends
// can reach the session any other way, so a bridge that crashes or talks rubbish only loses
// its own connection.
class Server final : private juce::InterprocessConnectionServer
{
public:
    Server();
    ~Server() override; // stops listening, lets every connection go, deletes the listing

    // Listens on a free port on 127.0.0.1 and writes the listing. False if it couldn't.
    bool start();
    int port() const { return listeningPort; }
    juce::String token() const { return secret; }

    // The session the listing names (the bridge looks for one that's open). Cheap when unchanged.
    void setSession (const juce::File& folder, const juce::String& name);

    std::optional<Call> next(); // the oldest call waiting, if any (and lets go of closed connections)
    void answer (const Call&, const Answer&);
    int connections() const;

private:
    struct Connection;
    juce::InterprocessConnection* createConnectionObject() override;
    void received (Connection&, const juce::var&);
    void writeListing() const;

    mutable juce::CriticalSection lock;
    std::vector<std::unique_ptr<Connection>> open;
    std::vector<Call> queue;
    int nextId = 1, listeningPort = 0;
    juce::String secret;
    juce::File sessionFolder;
    juce::String sessionName;
    juce::Time startedAt;
};

// The tools, for tools/list: name, description, inputSchema.
juce::var toolList();

// The bridge: one JSON-RPC message in (a line from stdin), the reply out ("" for a
// notification). Connects to taw when a tool is called, and again after taw has gone away.
class Bridge
{
public:
    // sessionHint: --session (a folder, or part of a session's name); agentName: --name.
    Bridge (juce::String sessionHint, juce::String agentName);
    ~Bridge();

    juce::String handle (const juce::String& line);

    // For tests: talk to this port and token instead of looking in the agents folder.
    void target (int port, const juce::String& token);

    // A tool call straight to taw, as tools/call makes it.
    Answer call (const juce::String& tool, const juce::var& args);

private:
    struct Link;
    bool connect (juce::String& error);

    juce::String hint, agent;
    int fixedPort = 0;
    juce::String fixedToken;
    std::unique_ptr<Link> link;
    int nextId = 1;
};

// taw --mcp [--session <folder or name>] [--name <agent>]: the bridge on stdin and stdout.
int runBridge (const juce::StringArray& args);

// When the user last changed each part of the session: per track, and the session itself (its
// settings and markers). Anything changed while agentEditing is off counts as the user's (taw's
// own bookkeeping too, which only errs on the safe side). Lives on the message thread.
class RecentEdits final : private juce::ValueTree::Listener
{
public:
    ~RecentEdits() override { watch ({}); }

    void watch (juce::ValueTree sessionTree); // again whenever the session changes
    bool watching (const juce::ValueTree& t) const { return root == t; }

    bool agentEditing = false;
    double window = 5.0; // seconds an edit of the user's keeps agents off

    // Seconds since the user last changed this track (or the session's own settings), or a
    // negative number when it's longer ago than the window.
    double recent (const juce::ValueTree& owner) const;

    // The track a node is in, or the session's tree for anything else.
    static juce::ValueTree ownerOf (const juce::ValueTree& node);

private:
    void touched (const juce::ValueTree& owner);
    void valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier&) override { touched (ownerOf (t)); }
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override { touched (child.hasType ("TRACK") ? child : ownerOf (parent)); }
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override { touched (child.hasType ("TRACK") ? child : ownerOf (parent)); }
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override { touched (ownerOf (parent)); }

    juce::ValueTree root;
    std::vector<std::pair<juce::ValueTree, double>> times; // owner, when (ms)
};
} // namespace taw::agents
