// The phone link: a very small HTTP server (one page, one upload) for the local network.

#include "Phone.h"

#include <qrcodegen.hpp>

#include <atomic>
#include <list>
#include <thread>

namespace taw
{
namespace
{
constexpr int firstPort = 8317, portsToTry = 12;
constexpr juce::int64 maxUpload = 500LL * 1024 * 1024;

// The page. One file: no scripts or styles from anywhere else.
const char* const page = R"html(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>taw</title>
<style>
  :root { color-scheme: dark; }
  body { margin: 0; padding: 24px 16px; background: #14161a; color: #e8e6e1; font: 17px/1.4 system-ui, sans-serif; }
  h1 { font-size: 20px; margin: 0 0 4px; }
  p { margin: 0 0 20px; color: #9a9890; }
  label.button { display: block; margin: 0 0 12px; padding: 18px; border-radius: 12px; text-align: center; font-weight: 600; }
  label.record { background: #c8452f; color: #fff; }
  label.pick { background: #2a2e35; color: #e8e6e1; }
  input[type=file] { display: none; }
  ul { list-style: none; margin: 20px 0 0; padding: 0; }
  li { padding: 12px 0; border-top: 1px solid #2a2e35; overflow-wrap: anywhere; }
  li small { display: block; color: #9a9890; }
  li.ok small { color: #7fbf7f; }
  li.bad small { color: #e0705c; }
  .bar { height: 4px; margin-top: 8px; border-radius: 2px; background: #2a2e35; }
  .bar i { display: block; height: 4px; width: 0; border-radius: 2px; background: #e8e6e1; }
</style>
</head>
<body>
<h1>taw</h1>
<p>Send a recording to the session open in taw.</p>
<label class="button record">Record now<input id="record" type="file" accept="audio/*" capture></label>
<label class="button pick">Send a recording<input id="pick" type="file" accept="audio/*,.m4a,.mp3,.wav,.aac,.flac,.ogg,.aif,.aiff,.wma" multiple></label>
<ul id="list"></ul>
<script>
  const list = document.getElementById('list');
  let queue = Promise.resolve();

  function send (file) {
    const row = document.createElement('li');
    const note = document.createElement('small');
    const bar = document.createElement('div');
    const fill = document.createElement('i');
    row.textContent = file.name;
    note.textContent = 'waiting';
    bar.className = 'bar';
    bar.appendChild (fill);
    row.appendChild (note);
    row.appendChild (bar);
    list.prepend (row);

    queue = queue.then (() => new Promise (done => {
      const x = new XMLHttpRequest();
      x.open ('POST', location.pathname.replace (/\/$/, '') + '/up?name=' + encodeURIComponent (file.name));
      x.upload.onprogress = e => {
        if (e.lengthComputable) {
          fill.style.width = (100 * e.loaded / e.total) + '%';
          note.textContent = 'sending ' + Math.round (100 * e.loaded / e.total) + '%';
        }
      };
      x.onload = () => {
        const ok = x.status === 200;
        row.className = ok ? 'ok' : 'bad';
        note.textContent = ok ? 'in taw' : (x.responseText || 'taw said no');
        bar.remove();
        done();
      };
      x.onerror = () => {
        row.className = 'bad';
        note.textContent = "couldn't reach taw (is the phone link still on?)";
        bar.remove();
        done();
      };
      x.send (file);
    }));
  }

  for (const id of ['record', 'pick'])
    document.getElementById (id).addEventListener ('change', e => {
      for (const f of e.target.files) send (f);
      e.target.value = '';
    });
</script>
</body>
</html>
)html";

// 10.x, 172.16-31.x, 192.168.x, 169.254.x (link-local) and this machine itself.
bool isLocalAddress (const juce::String& address)
{
    const auto parts = juce::StringArray::fromTokens (address, ".", "");

    if (parts.size() != 4)
        return address == "::1";

    const int a = parts[0].getIntValue(), b = parts[1].getIntValue();
    return a == 10 || a == 127 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254);
}

// This machine's address on the local network: a home network's 192.168 first, then 10, then
// 172.16-31 (which virtual adapters like to take).
juce::String localAddress()
{
    juce::String best;
    int bestRank = 99;

    for (const auto& ip : juce::IPAddress::getAllAddresses (false))
    {
        const auto text = ip.toString();
        const auto parts = juce::StringArray::fromTokens (text, ".", "");

        if (parts.size() != 4 || ! isLocalAddress (text))
            continue;

        const int a = parts[0].getIntValue();
        const int rank = a == 192 ? 0 : a == 10 ? 1 : a == 172 ? 2 : 9; // 127 and 169.254: no use to a phone

        if (rank < bestRank)
        {
            best = text;
            bestRank = rank;
        }
    }

    return bestRank < 9 ? best : juce::String();
}

juce::String makeToken()
{
    // Without the look-alikes (0 o, 1 l): it may be typed.
    const char* const letters = "abcdefghijkmnpqrstuvwxyz23456789";
    juce::Random random;
    random.setSeedRandomly();
    juce::String token;

    for (int i = 0; i < 6; ++i)
        token += letters[random.nextInt (32)];

    return token;
}
} // namespace

struct PhoneLink::Server final : private juce::Thread
{
    Server() : juce::Thread ("taw phone link") {}

    ~Server() override
    {
        stopping = true;
        signalThreadShouldExit();
        listener.close();
        stopThread (3000);

        for (auto& w : workers)
            if (w.thread.joinable())
                w.thread.join();

        folder.deleteRecursively();
    }

    bool start (juce::String& error)
    {
        address = localAddress();

        if (address.isEmpty())
        {
            error = "no local network to be reached on";
            return false;
        }

        for (port = firstPort; port < firstPort + portsToTry; ++port)
            if (listener.createListener (port))
                break;

        if (port == firstPort + portsToTry)
        {
            error = "couldn't open a port (" + juce::String (firstPort) + " to " + juce::String (firstPort + portsToTry - 1) + " are taken)";
            return false;
        }

        token = makeToken();
        folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("taw-phone-" + token);
        folder.createDirectory();
        startThread();
        return true;
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            // Finished connections out of the list.
            for (auto w = workers.begin(); w != workers.end();)
            {
                if (w->done)
                {
                    w->thread.join();
                    w = workers.erase (w);
                }
                else
                    ++w;
            }

            if (listener.waitUntilReady (true, 200) != 1)
                continue;

            if (auto* connection = listener.waitForNextConnection())
            {
                auto& w = workers.emplace_back();
                w.thread = std::thread ([this, connection, &w]
                {
                    std::unique_ptr<juce::StreamingSocket> socket (connection);
                    serve (*socket);
                    w.done = true;
                });
            }
        }
    }

    static void reply (juce::StreamingSocket& socket, const char* status, const char* type, const juce::String& body)
    {
        const auto utf8 = body.toRawUTF8();
        const auto length = (int) body.getNumBytesAsUTF8();
        const auto head = juce::String ("HTTP/1.1 ") + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " + juce::String (length)
                          + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        socket.write (head.toRawUTF8(), (int) head.getNumBytesAsUTF8());
        socket.write (utf8, length);
    }

    void say (const juce::String& text)
    {
        const juce::ScopedLock sl (lock);
        statusText = text;
    }

    void serve (juce::StreamingSocket& socket)
    {
        // Nothing from outside the local network gets a word back.
        if (! isLocalAddress (socket.getHostName()))
            return;

        // The request's head: up to the blank line.
        juce::MemoryBlock data;
        int headEnd = -1;

        while (headEnd < 0 && data.getSize() < 16384 && ! stopping)
        {
            if (socket.waitUntilReady (true, 3000) != 1)
                return;

            char buffer[2048];
            const int got = socket.read (buffer, sizeof (buffer), false);

            if (got <= 0)
                return;

            data.append (buffer, (size_t) got);
            const auto* bytes = static_cast<const char*> (data.getData());

            for (int i = 0; i + 3 < (int) data.getSize(); ++i)
                if (bytes[i] == '\r' && bytes[i + 1] == '\n' && bytes[i + 2] == '\r' && bytes[i + 3] == '\n')
                {
                    headEnd = i + 4;
                    break;
                }
        }

        if (headEnd < 0)
            return;

        const auto lines = juce::StringArray::fromLines (juce::String::fromUTF8 (static_cast<const char*> (data.getData()), headEnd));
        const auto request = juce::StringArray::fromTokens (lines[0], " ", "");
        const auto method = request[0];
        const auto path = request[1].upToFirstOccurrenceOf ("?", false, false);
        const auto query = request[1].fromFirstOccurrenceOf ("?", false, false);
        juce::int64 length = -1;

        for (int i = 1; i < lines.size(); ++i)
            if (lines[i].upToFirstOccurrenceOf (":", false, false).trim().equalsIgnoreCase ("content-length"))
                length = lines[i].fromFirstOccurrenceOf (":", false, false).trim().getLargeIntValue();

        const auto home = "/" + token;

        if (method == "GET" && (path == home || path == home + "/"))
            return reply (socket, "200 OK", "text/html; charset=utf-8", page);

        // A refusal is read by the phone only if what it was sending is taken off the line first
        // (closing on unread data resets the connection, and the answer goes with it). Up to 32 MB
        // of it: more isn't worth the wait.
        auto refuse = [&] (const char* status, const char* why)
        {
            auto left = std::min<juce::int64> (length, 32LL * 1024 * 1024) - ((juce::int64) data.getSize() - headEnd);
            std::vector<char> sink (64 * 1024);

            while (method == "POST" && left > 0 && ! stopping && socket.waitUntilReady (true, 3000) == 1)
            {
                const int got = socket.read (sink.data(), (int) std::min<juce::int64> ((juce::int64) sink.size(), left), false);

                if (got <= 0)
                    break;

                left -= got;
            }

            reply (socket, status, "text/plain; charset=utf-8", why);
        };

        if (method != "POST" || path != home + "/up")
            return refuse ("404 Not Found", "not here");

        // An upload: the file's name in the query, the file itself as the body.
        juce::String name;

        for (const auto& pair : juce::StringArray::fromTokens (query, "&", ""))
            if (pair.startsWith ("name="))
                name = juce::URL::removeEscapeChars (pair.substring (5));

        name = juce::File::createLegalFileName (name.trim()).substring (0, 120);

        // What a phone's recorder makes, and what the importer can read.
        static const juce::StringArray audio { ".wav", ".aif", ".aiff", ".flac", ".ogg", ".mp3", ".m4a", ".aac", ".wma", ".mp4", ".3gp" };

        if (name.isEmpty() || ! audio.contains (juce::File::createLegalFileName (name).fromLastOccurrenceOf (".", true, false), true))
            return refuse ("415 Unsupported Media Type", "taw takes audio files only");

        if (length <= 0 || length > maxUpload)
            return refuse ("413 Payload Too Large", "too big for taw (500 MB at most)");

        const auto file = folder.getChildFile (name).getNonexistentSibling();
        const auto partial = file.getSiblingFile (file.getFileName() + ".part");
        juce::int64 written = 0;
        bool failed = false;

        {
            juce::FileOutputStream out (partial);

            if (out.failedToOpen())
                return reply (socket, "500 Internal Server Error", "text/plain; charset=utf-8", "taw couldn't keep the file");

            // What came with the head, then the rest.
            const auto already = std::min<juce::int64> (length, (juce::int64) data.getSize() - headEnd);
            failed = ! out.write (static_cast<const char*> (data.getData()) + headEnd, (size_t) already);
            written = already;
            std::vector<char> buffer (64 * 1024);

            while (written < length && ! failed && ! stopping)
            {
                if (socket.waitUntilReady (true, 20000) != 1)
                {
                    failed = true;
                    break;
                }

                const int got = socket.read (buffer.data(), (int) std::min<juce::int64> ((juce::int64) buffer.size(), length - written), false);

                if (got <= 0 || ! out.write (buffer.data(), (size_t) got))
                {
                    failed = true;
                    break;
                }

                written += got;
                say ("receiving " + name + " " + juce::String (juce::roundToInt (100.0 * (double) written / (double) length)) + "%");
            }
        }

        if (failed || stopping || written < length || ! partial.moveFileTo (file))
        {
            partial.deleteFile();
            say ("lost " + name + " part way");
            return reply (socket, "400 Bad Request", "text/plain; charset=utf-8", "it didn't all arrive");
        }

        {
            const juce::ScopedLock sl (lock);
            arrived.push_back (file);
            ++count;
            statusText = "got " + name;
        }

        reply (socket, "200 OK", "text/plain; charset=utf-8", "ok");
    }

    struct Worker
    {
        std::thread thread;
        std::atomic<bool> done { false };
    };

    juce::StreamingSocket listener;
    std::list<Worker> workers; // the accept thread's own
    std::atomic<bool> stopping { false };

    juce::String address, token;
    int port = 0;
    juce::File folder;

    juce::CriticalSection lock;
    std::vector<juce::File> arrived;
    juce::String statusText;
    int count = 0;
};

PhoneLink::PhoneLink() = default;
PhoneLink::~PhoneLink() = default;

bool PhoneLink::start (juce::String& error)
{
    if (server != nullptr)
        return true;

    auto starting = std::make_unique<Server>();

    if (! starting->start (error))
        return false;

    server = std::move (starting);
    return true;
}

void PhoneLink::stop() { server.reset(); }
bool PhoneLink::running() const { return server != nullptr; }

juce::String PhoneLink::url() const
{
    return server != nullptr ? "http://" + server->address + ":" + juce::String (server->port) + "/" + server->token : juce::String();
}

std::vector<juce::File> PhoneLink::takeReceived()
{
    if (server == nullptr)
        return {};

    const juce::ScopedLock sl (server->lock);
    return std::exchange (server->arrived, {});
}

int PhoneLink::received() const
{
    if (server == nullptr)
        return 0;

    const juce::ScopedLock sl (server->lock);
    return server->count;
}

juce::String PhoneLink::status() const
{
    if (server == nullptr)
        return {};

    const juce::ScopedLock sl (server->lock);
    return server->statusText;
}

std::vector<std::vector<bool>> PhoneLink::qr (const juce::String& text)
{
    try
    {
        const auto code = qrcodegen::QrCode::encodeText (text.toRawUTF8(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int size = code.getSize();
        std::vector<std::vector<bool>> rows ((size_t) size, std::vector<bool> ((size_t) size));

        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                rows[(size_t) y][(size_t) x] = code.getModule (x, y);

        return rows;
    }
    catch (const std::exception&)
    {
        return {};
    }
}
} // namespace taw
