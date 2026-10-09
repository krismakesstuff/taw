// The hub from inside taw: the hub command and its panel. The hub itself is its own process
// (Hub.cpp); this asks it for its status and sends it routes.

#include "App.h"

#if TAW_HUB
 #include "Hub.h"
 #include "HubDevice.h"
 #include "HubServer.h"
 #include "HubMac.h"
#endif

namespace taw
{
#if TAW_HUB
namespace
{
constexpr const char* interfaceKey = "interface";

juce::var hubMessage (const juce::String& type)
{
    auto* m = new juce::DynamicObject();
    m->setProperty ("type", type);
    return m;
}

// One end of a route as typed: "in 3-4", "out 1", "Song 1-2", "this", "Beat". Channels count
// from 1; none given: first = last = 0.
struct End
{
    juce::String key;
    int first = 0, last = 0;
    int count() const { return first == 0 ? 0 : last - first + 1; }
};

bool isSession (const juce::String& key) { return key != interfaceKey && hub::appNumber (key) == 0; }

bool isChannels (const juce::String& t)
{
    return t.isNotEmpty() && t.containsOnly ("0123456789-") && t[0] != '-' && ! t.endsWithChar ('-');
}

bool isLevel (const juce::String& t)
{
    const auto s = t.toLowerCase().trimCharactersAtEnd ("db");
    return s.isNotEmpty() && s.containsOnly ("+-0123456789.") && (s[0] == '+' || s[0] == '-' || t.toLowerCase().endsWith ("db"));
}
} // namespace

void App::refreshHub (bool now)
{
    const auto t = juce::Time::getMillisecondCounterHiRes();

    if (! now && t - hubAskedAt < 500.0)
        return;

    hubAskedAt = t;
    hubError.clear();
    hubStatus = hub::request (hubMessage ("status"), hubError);
    hubUsers.clear();

    for (const auto& [device, users] : hub::mac::appDeviceUsers())
        hubUsers[device] = { users.playing, users.listening };
}

bool App::sessionOnHub() const
{
    if (session == nullptr)
        return false;

    if (auto* sessions = hubStatus["sessions"].getArray())
        for (const auto& s : *sessions)
            if (s["key"].toString() == session->folder().getFullPathName())
                return (bool) s["connected"];

    return false;
}

// The hub's hardware, one way: the status says "Speakers + Microphone" (two devices) or one name.
juce::String App::hubHardware (bool output) const
{
    const auto device = hubStatus["device"].toString();

    if (! device.contains (" + "))
        return device;

    return output ? device.upToFirstOccurrenceOf (" + ", false, false) : device.fromFirstOccurrenceOf (" + ", false, false);
}

juce::String App::hubEndName (const juce::String& key, int channel, bool source) const
{
    if (key == interfaceKey)
        return hubHardware (! source) + (source ? " in " : " out ") + juce::String (channel);

    if (hub::appNumber (key) > 0)
        return hubAppName (key, source) + (channel == 1 ? " L" : channel == 2 ? " R" : " " + juce::String (channel));

    if (auto* sessions = hubStatus["sessions"].getArray())
        for (const auto& s : *sessions)
            if (s["key"].toString() == key)
                return s["name"].toString() + (source ? " out " : " in ") + juce::String (channel);

    return juce::File (key).getFileName() + (source ? " out " : " in ") + juce::String (channel);
}

juce::String App::hubAppName (const juce::String& key, bool source) const
{
    const auto label = hubStatus["labels"][juce::Identifier (key)].toString();
    return label.isNotEmpty() ? label : "taw" + juce::String (hub::appNumber (key)) + (source ? "in" : "out");
}

bool App::hubAppNamed (const juce::String& key) const
{
    return hubStatus["labels"][juce::Identifier (key)].toString().isNotEmpty();
}

// "in 1-2 > Song 1-2 -6": pairs the channels (as many as both give; one to many spreads it,
// many to one sums them; none given: 1-2) and asks the hub for each pair. add false: takes
// them out (no channels: all of them).
Result App::hubRoute (const juce::String& spec, bool add)
{
    if (! spec.contains (">"))
        return Result::fail (add ? L"hub route <from> > <to> \x00b7 e.g. hub route in 1-2 > Song, hub route Song > out 1-2 -6"
                                 : L"hub unroute <from> > <to> \x00b7 e.g. hub unroute Song > out");

    refreshHub (true);

    if (hubStatus.isVoid())
        return Result::fail (hubError);

    auto words = juce::StringArray::fromTokens (spec.fromLastOccurrenceOf (">", false, false).trim(), true);
    words.removeEmptyStrings();
    double db = 0.0;

    if (add && words.size() > 1 && isLevel (words[words.size() - 1]))
    {
        db = words[words.size() - 1].toLowerCase().trimCharactersAtEnd ("db").getDoubleValue();
        words.remove (words.size() - 1);
    }

    juce::String error;
    auto parseEnd = [&] (juce::StringArray tokens, bool source) -> std::optional<End>
    {
        End e;
        tokens.removeEmptyStrings(); // "in 1 > ..." leaves one after the space

        // An app device by its name ("Zoom", or "Zoom L" with channels): as "taw 1" from here on.
        auto deviceNamed = [this] (const juce::String& name) -> juce::String
        {
            if (auto* labels = hubStatus["labels"].getDynamicObject())
                for (const auto& p : labels->getProperties())
                    if (p.value.toString().equalsIgnoreCase (name))
                        return p.name.toString();

            return {};
        };

        if (const auto whole = deviceNamed (tokens.joinIntoString (" ")); whole.isNotEmpty())
        {
            tokens = juce::StringArray::fromTokens (whole, false);
        }
        else if (tokens.size() > 1)
        {
            const auto channels = tokens[tokens.size() - 1];

            if (const auto named = deviceNamed (juce::StringArray (tokens.begin(), tokens.size() - 1).joinIntoString (" ")); named.isNotEmpty())
            {
                tokens = juce::StringArray::fromTokens (named, false);
                tokens.add (channels);
            }
        }

        // As the page names them, from the hub's side: "taw2in" (a source: what apps play to
        // taw 2 comes in), "taw2out" (a destination: out to apps hearing it), as "taw 2".
        if (const auto t = tokens[0].toLowerCase(); t.startsWith ("taw") && (t.endsWith ("out") || t.endsWith ("in")))
        {
            const bool out = t.endsWith ("out");
            const auto n = t.substring (3, t.length() - (out ? 3 : 2));

            if (n.length() == 1 && n.containsOnly ("123456789"))
            {
                if (out == source)
                {
                    error = tokens[0] + (out ? L" goes out to apps \x2014 a source is taw" + n + "in"
                                             : L" comes in from apps \x2014 a destination is taw" + n + "out");
                    return {};
                }

                tokens.set (0, "taw");
                tokens.insert (1, n);
            }
        }

        // An app device: "taw 2", then maybe its channels ("1", "1-2", "L", "R").
        if (tokens.size() >= 2 && tokens[0].equalsIgnoreCase ("taw") && tokens[1].containsOnly ("123456789") && tokens[1].length() == 1)
        {
            e.key = "taw " + tokens[1];

            if (add && (int) hubStatus["apps"] < tokens[1].getIntValue()) // taking one out always works
            {
                error = (int) hubStatus["apps"] == 0 ? L"no app devices \x2014 driver install adds taw 1 to taw 4"
                                                     : "there are " + hubStatus["apps"].toString() + " app devices";
                return {};
            }

            const auto c = tokens[2].toUpperCase();
            e.first = c == "L" ? 1 : c == "R" ? 2 : c == "L-R" || c == "LR" ? 1 : c.upToFirstOccurrenceOf ("-", false, false).getIntValue();
            e.last = c == "L" ? 1 : c == "R" || c == "L-R" || c == "LR" ? 2 : c.contains ("-") ? c.fromFirstOccurrenceOf ("-", false, false).getIntValue() : e.first;

            if (tokens.size() > 3 || e.last > 2 || (c.isNotEmpty() && e.first < 1))
            {
                error = L"an app device has 2 channels \x00b7 taw1in, taw1in L, taw1out 2";
                return {};
            }

            return e;
        }

        if (tokens.size() > 1 && isChannels (tokens[tokens.size() - 1]))
        {
            const auto c = tokens[tokens.size() - 1];
            e.first = c.upToFirstOccurrenceOf ("-", false, false).getIntValue();
            e.last = c.contains ("-") ? c.fromFirstOccurrenceOf ("-", false, false).getIntValue() : e.first;
            tokens.remove (tokens.size() - 1);

            if (e.first < 1 || e.last < e.first)
            {
                error = c + " isn't channels (1, 3-4)";
                return {};
            }
        }

        const auto name = tokens.joinIntoString (" ").trim();

        if (name.equalsIgnoreCase ("in") || name.equalsIgnoreCase ("out"))
        {
            if (name.equalsIgnoreCase ("in") != source)
            {
                error = source ? juce::String (L"the interface's outputs only take audio \x00b7 in 1, in 3-4 are its inputs")
                               : juce::String (L"the interface's inputs only give audio \x00b7 out 1-2 are its outputs");
                return {};
            }

            e.key = interfaceKey;
            return e;
        }

        if (name.equalsIgnoreCase ("this") || name.equalsIgnoreCase ("here"))
        {
            if (session == nullptr)
            {
                error = "no session open here";
                return {};
            }

            e.key = session->folder().getFullPathName();
            return e;
        }

        // A session the hub knows, by name: exactly, else the only one it starts.
        juce::StringArray keys, startKeys;

        if (auto* sessions = hubStatus["sessions"].getArray())
            for (const auto& s : *sessions)
            {
                if (s["name"].toString().equalsIgnoreCase (name))
                    keys.add (s["key"].toString());

                if (s["name"].toString().startsWithIgnoreCase (name))
                    startKeys.add (s["key"].toString());
            }

        if (keys.isEmpty())
            keys = startKeys;

        if (keys.size() != 1)
        {
            error = keys.isEmpty() ? L"the hub doesn't know " + name + L" \x00b7 a session joins it by using the taw hub device (dev)"
                                   : name + L" could be more than one session \x00b7 type more of its name";
            return {};
        }

        e.key = keys[0];
        return e;
    };

    const auto from = parseEnd (juce::StringArray::fromTokens (spec.upToFirstOccurrenceOf (">", false, false), true), true);
    const auto to = from ? parseEnd (words, false) : std::nullopt;

    if (! from || ! to)
        return Result::fail (error);

    // Which channel goes where.
    std::vector<std::pair<int, int>> pairs;

    if (! add && (from->count() == 0 || to->count() == 0))
    {
        // Taking out, a side without channels means all of them (0).
        const int fromN = std::max (1, from->count()), toN = std::max (1, to->count());

        for (int i = 0; i < std::max (fromN, toN); ++i)
            pairs.push_back ({ from->count() == 0 ? 0 : from->first + std::min (i, fromN - 1), to->count() == 0 ? 0 : to->first + std::min (i, toN - 1) });
    }
    else
    {
        const int n = std::max ({ from->count(), to->count(), from->count() == 0 && to->count() == 0 ? 2 : 0 });
        const int fromN = from->count() == 0 ? n : from->count(), toN = to->count() == 0 ? n : to->count();
        const int fromFirst = std::max (1, from->first), toFirst = std::max (1, to->first);

        if (fromN != toN && fromN != 1 && toN != 1)
            return Result::fail (juce::String (fromN) + " channels into " + juce::String (toN) + L" \x00b7 give as many on each side, or one on either");

        for (int i = 0; i < std::max (fromN, toN); ++i)
            pairs.push_back ({ fromFirst + (fromN == 1 ? 0 : i), toFirst + (toN == 1 ? 0 : i) });
    }

    int removed = 0;

    for (const auto& [f, t] : pairs)
    {
        auto m = hubMessage (add ? "route" : "unroute");
        m.getDynamicObject()->setProperty ("from", from->key);
        m.getDynamicObject()->setProperty ("fromChannel", f);
        m.getDynamicObject()->setProperty ("to", to->key);
        m.getDynamicObject()->setProperty ("toChannel", t);
        m.getDynamicObject()->setProperty ("db", db);
        juce::String failed;
        const auto answer = hub::request (m, failed);

        if (failed.isNotEmpty() || answer["error"].toString().isNotEmpty())
            return Result::fail (failed.isNotEmpty() ? failed : answer["error"].toString());

        hubStatus = answer;
        removed += (int) answer["removed"];
    }

    const auto describe = [this] (const End& e, int first, int last, bool source)
    {
        if (hub::appNumber (e.key) > 0 && first == 1 && last == 2)
            return hubAppName (e.key, source); // both sides: "taw1in", not "taw1in L-2"

        auto s = hubEndName (e.key, first, source);
        return last > first ? s + "-" + juce::String (last) : s;
    };

    const auto fromText = describe (*from, pairs.front().first, pairs.back().first, true);
    const auto toText = describe (*to, pairs.front().second, pairs.back().second, false);

    if (! add)
        return removed == 0 ? Result::fail ("no such route") : Result::good ("took out " + juce::String (removed) + (removed == 1 ? " route" : " routes"));

    return Result::good (fromText + L" \x2192 " + toText + (db != 0.0 ? "  " + juce::String (db, 1) + " dB" : juce::String())
                         + (isSession (from->key) && isSession (to->key) ? juce::String (L" \x00b7 a block later (session to session)") : juce::String()));
}

Result App::hubCommand (const juce::String& rest)
{
    const auto word = rest.upToFirstOccurrenceOf (" ", false, false).toLowerCase();
    const auto args = rest.fromFirstOccurrenceOf (" ", false, false).trim();

    if (word == "route" || word == "unroute")
        return hubRoute (args, word == "route");

    // name taw 1 Zoom: what an app device is called in taw (apps still see taw 1). name taw 1:
    // back to its own name. Its current name works too: name Zoom Call.
    if (word == "name")
    {
        refreshHub (true);

        if (hubStatus.isVoid())
            return Result::fail (hubError);

        auto words = juce::StringArray::fromTokens (args, false);
        words.removeEmptyStrings();
        juce::String key;

        if (words.size() >= 2 && words[0].equalsIgnoreCase ("taw") && hub::appNumber ("taw " + words[1]) > 0)
        {
            key = "taw " + words[1];
            words.removeRange (0, 2);
        }
        else if (auto* labels = hubStatus["labels"].getDynamicObject(); labels != nullptr && words.size() >= 1)
        {
            for (const auto& p : labels->getProperties())
                if (p.value.toString().equalsIgnoreCase (words[0]))
                    key = p.name.toString();

            if (key.isNotEmpty())
                words.remove (0);
        }

        if (key.isEmpty())
            return Result::fail (L"name taw 1 Zoom \x00b7 names an app device in taw (other apps still see taw 1)");

        auto m = hubMessage ("label");
        m.getDynamicObject()->setProperty ("key", key);
        m.getDynamicObject()->setProperty ("label", words.joinIntoString (" "));
        juce::String error;
        const auto answer = hub::request (m, error);

        if (error.isNotEmpty() || answer["error"].toString().isNotEmpty())
            return Result::fail (error.isNotEmpty() ? error : answer["error"].toString());

        hubStatus = answer;
        return Result::good (words.isEmpty() ? key + " has its own name again" : key + " is " + words.joinIntoString (" ") + " here");
    }

    if (word == "forget")
    {
        refreshHub (true);

        if (hubStatus.isVoid())
            return Result::fail (hubError);

        juce::String key;

        if (auto* sessions = hubStatus["sessions"].getArray())
            for (const auto& s : *sessions)
                if (s["name"].toString().equalsIgnoreCase (args))
                    key = s["key"].toString();

        if (key.isEmpty())
            return Result::fail ("the hub doesn't know " + (args.isEmpty() ? juce::String ("that session") : args) + L" \x00b7 hub lists the sessions");

        auto m = hubMessage ("forget");
        m.getDynamicObject()->setProperty ("key", key);
        juce::String error;
        const auto answer = hub::request (m, error);

        if (error.isNotEmpty())
            return Result::fail (error);

        hubStatus = answer;
        return Result::good ("the hub forgot " + args + " and its " + juce::String ((int) answer["removed"]) + " routes");
    }

    // interface: a picker, like dev's (from a session's command line, over the hub's page);
    // interface MacBook Pro Speakers (+ MacBook Pro Microphone): the hub's own interface, by
    // name or the start of one. The hub restarts with it.
    if (word == "interface" || word == "if")
    {
        if (args.isEmpty())
        {
            if (mode != Mode::Hub)
                openHubPage();

            openDevicePicker (true);
            return Result::good();
        }

        juce::StringArray outputs, inputs;

        for (auto* type : engine.devices().getAvailableDeviceTypes())
            if (type->getTypeName() == "CoreAudio")
            {
                type->scanForDevices();
                outputs = type->getDeviceNames (false);
                inputs = type->getDeviceNames (true);
            }

        for (auto* list : { &outputs, &inputs })
            for (int i = list->size(); --i >= 0;)
                if (list->getReference (i).startsWith ("taw ")) // the driver's own: a loop
                    list->remove (i);

        auto pick = [] (const juce::StringArray& names, const juce::String& typed) -> juce::String
        {
            juce::StringArray starting;

            for (const auto& n : names)
            {
                if (n.equalsIgnoreCase (typed))
                    return n;

                if (n.startsWithIgnoreCase (typed))
                    starting.add (n);
            }

            return starting.size() == 1 ? starting[0] : juce::String();
        };

        const auto typedOut = args.upToFirstOccurrenceOf ("+", false, false).trim();
        const auto typedIn = args.fromFirstOccurrenceOf ("+", false, false).trim();
        const auto output = pick (outputs, typedOut);
        auto input = typedIn.isNotEmpty() ? pick (inputs, typedIn) : juce::String();

        if (output.isEmpty())
            return Result::fail ("no one output called " + typedOut + L" \x00b7 i picks one");

        if (typedIn.isNotEmpty() && input.isEmpty())
            return Result::fail ("no one input called " + typedIn + L" \x00b7 i picks one");

        if (typedIn.isEmpty() && inputs.contains (output))
            input = output; // an interface with both

        return Result::good (hub::setInterface (output, input));
    }

    // driver install / uninstall: taw 1 to taw 4 for apps (an admin password; the Mac's sound
    // stops for a moment).
    if (word == "driver")
    {
        if (! args.equalsIgnoreCase ("install") && ! args.equalsIgnoreCase ("uninstall"))
            return Result::fail (L"driver install \x00b7 driver uninstall");

        const auto said = args.equalsIgnoreCase ("install") ? hub::installDriver() : hub::uninstallDriver();
        hubAskedAt = 0.0;
        return Result::good (said);
    }

    if (word == "start" || word == "stop" || word == "install" || word == "uninstall")
    {
        const auto said = word == "start" ? hub::start() : word == "stop" ? hub::stop() : word == "install" ? hub::install() : hub::uninstall();
        hubAskedAt = 0.0; // ask again
        return Result::good (said);
    }

    if (word.isNotEmpty())
        return Result::fail (L"hub \x00b7 hub route <from> > <to> \x00b7 hub unroute <from> > <to> \x00b7 hub forget <session> \x00b7 hub start \x00b7 hub stop");

    openHubPage();
    return Result::good();
}

void App::openHubPage()
{
    if (mode != Mode::Hub)
        hubReturn = mode == Mode::Main ? Mode::Main : Mode::TemplatePicker;

    mode = Mode::Hub;
    hubLine.clear();
    helpVisible = settingsVisible = pluginPickerVisible = midiPanelVisible = false;
    refreshHub (true);
    say (hubStatus.isVoid() ? Result::fail (hubError) : Result::good());
}

// Back to the session, or to the start screen (set up now if taw hub came straight here).
void App::closeHubPage()
{
    hubLine.clear();

    if (hubReturn == Mode::Main && session != nullptr)
        mode = Mode::Main;
    else
        proceedToSession();
}

//==============================================================================
// The matrix.

std::vector<App::HubEnd> App::hubRows() const
{
    std::vector<HubEnd> rows;

    for (int i = 1; i <= (int) hubStatus["inputs"]; ++i)
        rows.push_back ({ interfaceKey, i, 1, hubHardware (false).upToFirstOccurrenceOf (" ", false, false) + " in " + juce::String (i), {}, false, true });

    for (int n = 1; n <= (int) hubStatus["apps"]; ++n)
    {
        const auto key = "taw " + juce::String (n);
        const auto it = hubUsers.find (key);
        const auto playing = it != hubUsers.end() ? it->second.playing.joinIntoString (", ") : juce::String();
        const auto named = hubAppNamed (key) ? "taw" + juce::String (n) + "in " : juce::String();
        rows.push_back ({ key, 1, 2, hubAppName (key, true), named + (playing.isNotEmpty() ? L"\x25c2 " + playing : juce::String ("no app")), false, true });
    }

    if (auto* sessions = hubStatus["sessions"].getArray())
        for (const auto& s : *sessions)
            rows.push_back ({ s["key"].toString(), 1, 2, s["name"].toString(), {}, true, (bool) s["connected"] });

    return rows;
}

std::vector<App::HubEnd> App::hubCols() const
{
    std::vector<HubEnd> cols;
    const int outputs = hubStatus["outputs"];

    for (int c = 1; c <= outputs; c += 2)
        // Headed by the hardware's first word ("MacBook", "Scarlett"), so it isn't taken for
        // the Mac's own output.
        cols.push_back ({ interfaceKey, c, juce::jmin (2, outputs - c + 1), hubHardware (true).upToFirstOccurrenceOf (" ", false, false),
                          "out " + (c < outputs ? juce::String (c) + "-" + juce::String (c + 1) : juce::String (c)), false, true });

    for (int n = 1; n <= (int) hubStatus["apps"]; ++n)
    {
        const auto key = "taw " + juce::String (n);
        const auto it = hubUsers.find (key);
        const bool heard = it != hubUsers.end() && ! it->second.listening.isEmpty();
        // Named, its second line says which device it is; else who hears it, if anyone.
        cols.push_back ({ key, 1, 2, hubAppName (key, false),
                          heard ? L"\x25b8" + it->second.listening[0] : hubAppNamed (key) ? "taw" + juce::String (n) + "out" : juce::String ("as mic"),
                          false, true });
    }

    if (auto* sessions = hubStatus["sessions"].getArray())
        for (const auto& s : *sessions)
            cols.push_back ({ s["key"].toString(), 1, 0, s["name"].toString(), "inputs", true, (bool) s["connected"] });

    return cols;
}

namespace
{
// The routes a cell makes, as (from channel, to channel): stereo to stereo side by side, mono
// to stereo on both sides, stereo to mono summed; to a session from where it lands.
std::vector<std::array<int, 2>> cellPairs (const App::HubEnd& row, const App::HubEnd& col, int land)
{
    std::vector<std::array<int, 2>> pairs;

    if (col.session)
    {
        for (int i = 0; i < row.width; ++i)
            pairs.push_back ({ row.first + i, land + i });
    }
    else if (row.width == col.width)
    {
        for (int i = 0; i < row.width; ++i)
            pairs.push_back ({ row.first + i, col.first + i });
    }
    else
    {
        for (int f = 0; f < row.width; ++f)
            for (int t = 0; t < col.width; ++t)
                pairs.push_back ({ row.first + f, col.first + t });
    }

    return pairs;
}

// How many inputs a session column has: what it got if it's here, else what it would get.
int sessionInputCount (const juce::var& status, const juce::String& key)
{
    if (auto* sessions = status["sessions"].getArray())
        for (const auto& s : *sessions)
            if (s["key"].toString() == key && (bool) s["connected"])
                return s["inputs"];

    return hub::sessionInputs (status["inputs"]);
}
} // namespace

App::HubCell App::hubCell (const HubEnd& row, const HubEnd& col) const
{
    HubCell cell;
    cell.self = row.key == col.key && row.key != interfaceKey; // the interface's inputs to its outputs is monitoring

    if (auto* routes = hubStatus["routes"].getArray())
        for (const auto& r : *routes)
        {
            const int from = r["fromChannel"], to = r["toChannel"];

            if (r["from"].toString() == row.key && from >= row.first && from < row.first + row.width && r["to"].toString() == col.key
                && (col.session || (to >= col.first && to < col.first + col.width)))
                cell.routes.add (r);
        }

    cell.on = ! cell.routes.isEmpty();

    if (! cell.on)
        return cell;

    cell.db = cell.routes.getReference (0)["db"];
    cell.land = cell.landLast = (int) cell.routes.getReference (0)["toChannel"];

    for (const auto& r : cell.routes)
    {
        cell.land = juce::jmin (cell.land, (int) r["toChannel"]);
        cell.landLast = juce::jmax (cell.landLast, (int) r["toChannel"]);
    }

    // The usual shape, or something typed by hand?
    auto expected = cellPairs (row, col, cell.land);
    cell.odd = (int) expected.size() != cell.routes.size();

    for (const auto& r : cell.routes)
        cell.odd = cell.odd || std::find (expected.begin(), expected.end(), std::array<int, 2> { (int) r["fromChannel"], (int) r["toChannel"] }) == expected.end();

    return cell;
}

// Takes routes out and puts others in, between two ends, and keeps what the hub says after.
Result App::hubReplace (const juce::Array<juce::var>& remove, const std::vector<std::array<int, 2>>& add,
                        const juce::String& from, const juce::String& to, double db)
{
    auto send = [this] (const juce::var& m) -> juce::String
    {
        juce::String error;
        const auto answer = hub::request (m, error);

        if (error.isEmpty() && answer["error"].toString().isNotEmpty())
            error = answer["error"].toString();

        if (error.isEmpty())
            hubStatus = answer;

        return error;
    };

    for (const auto& r : remove)
    {
        auto m = hubMessage ("unroute");

        for (const auto* p : { "from", "fromChannel", "to", "toChannel" })
            m.getDynamicObject()->setProperty (p, r[p]);

        if (const auto error = send (m); error.isNotEmpty())
            return Result::fail (error);
    }

    for (const auto& [f, t] : add)
    {
        auto m = hubMessage ("route");
        m.getDynamicObject()->setProperty ("from", from);
        m.getDynamicObject()->setProperty ("fromChannel", f);
        m.getDynamicObject()->setProperty ("to", to);
        m.getDynamicObject()->setProperty ("toChannel", t);
        m.getDynamicObject()->setProperty ("db", db);

        if (const auto error = send (m); error.isNotEmpty())
            return Result::fail (error);
    }

    return Result::good();
}

// Space: the chosen cell on or off. Into a session it lands on the input of the same number for
// an interface input, else on the first free inputs after the interface's (stereo on a pair).
Result App::hubToggle()
{
    const auto rows = hubRows();
    const auto cols = hubCols();

    if (rows.empty() || cols.empty())
        return Result::fail ("nothing to route yet");

    const auto& row = rows[(size_t) hubRow];
    const auto& col = cols[(size_t) hubCol];
    const auto cell = hubCell (row, col);

    if (cell.self)
        return Result::fail (row.name + " into itself would feed back");

    if (cell.on)
    {
        const auto r = hubReplace (cell.routes, {}, {}, {}, 0.0);
        return r.ok ? Result::good (row.name + L" \x2192 " + col.name + " off") : r;
    }

    int land = 1;

    if (col.session)
    {
        const int count = sessionInputCount (hubStatus, col.key);
        std::vector<bool> used ((size_t) count + 2, false);

        if (auto* routes = hubStatus["routes"].getArray())
            for (const auto& r : *routes)
                if (r["to"].toString() == col.key && (int) r["toChannel"] <= count)
                    used[(size_t) (int) r["toChannel"]] = true;

        auto free = [&] (int k) { return k >= 1 && k + row.width - 1 <= count && ! used[(size_t) k] && (row.width < 2 || ! used[(size_t) k + 1]); };
        land = 0;

        if (row.key == interfaceKey && free (row.first))
            land = row.first;

        // Stereo on a pair (3-4, not 2-3).
        const int after = (int) hubStatus["inputs"] + 1;

        for (int k = row.width > 1 ? after + (after % 2 == 0 ? 1 : 0) : after; k <= count && land == 0; k += row.width)
            land = free (k) ? k : 0;

        for (int k = 1; k <= count && land == 0; ++k)
            land = free (k) ? k : 0;

        if (land == 0)
            return Result::fail (col.name + "'s inputs are all taken");
    }

    const auto r = hubReplace ({}, cellPairs (row, col, land), row.key, col.key, 0.0);

    if (! r.ok)
        return r;

    return Result::good (row.name + L" \x2192 " + col.name
                         + (col.session ? (row.width > 1 ? " inputs " : " input ") + juce::String (land) + (row.width > 1 ? "-" + juce::String (land + row.width - 1) : juce::String()) : juce::String())
                         + (! col.live ? juce::String (L" \x00b7 waits for it to open")
                            : col.session ? L" \x00b7 in the session a track takes them: i " + juce::String (land) + (row.width > 1 ? "-" + juce::String (land + row.width - 1) : juce::String())
                                                + ", then mon to hear it or a to arm it"
                                          : juce::String()));
}

// Shift+←→ on a cell into a session: the inputs it lands on, one along.
Result App::hubMoveLanding (int delta)
{
    const auto rows = hubRows();
    const auto cols = hubCols();

    if (rows.empty() || cols.empty())
        return Result::good();

    const auto& row = rows[(size_t) hubRow];
    const auto& col = cols[(size_t) hubCol];
    const auto cell = hubCell (row, col);

    if (! col.session || ! cell.on)
        return Result::fail (L"shift+\x2190\x2192 moves where a route lands in a session: on a session's column");

    const int land = juce::jlimit (1, sessionInputCount (hubStatus, col.key) - row.width + 1, cell.land + delta);

    if (land == cell.land && ! cell.odd)
        return Result::good();

    const auto r = hubReplace (cell.routes, cellPairs (row, col, land), row.key, col.key, cell.db);
    return r.ok ? Result::good (row.name + L" \x2192 " + col.name + (row.width > 1 ? " inputs " : " input ") + juce::String (land)
                                + (row.width > 1 ? "-" + juce::String (land + row.width - 1) : juce::String()))
                : r;
}

// [ ]: the chosen cell's level, a dB at a time (-60 to +12).
Result App::hubLevel (double delta)
{
    const auto rows = hubRows();
    const auto cols = hubCols();

    if (rows.empty() || cols.empty())
        return Result::good();

    const auto cell = hubCell (rows[(size_t) hubRow], cols[(size_t) hubCol]);

    if (! cell.on)
        return Result::fail ("space routes it first");

    const double db = juce::jlimit (-60.0, 12.0, std::round (cell.db) + delta);
    std::vector<std::array<int, 2>> same;

    for (const auto& r : cell.routes)
        same.push_back ({ (int) r["fromChannel"], (int) r["toChannel"] });

    const auto r = hubReplace ({}, same, rows[(size_t) hubRow].key, cols[(size_t) hubCol].key, db); // routing again sets the level
    return r.ok ? Result::good (formatDb (db) + " dB") : r;
}

// The page's keys. With its command line empty: the arrows move round the matrix, space turns
// a cell on or off, [ ] its level, shift+←→ where it lands in a session, del takes it out, n
// names an app device. Anything else typed goes to the command line ("route in 1 > Song",
// "hub" in front is fine), enter runs it.
void App::handleHubPageKey (const Key& k)
{
    const auto numRows = (int) hubRows().size(), numCols = (int) hubCols().size();
    hubRow = juce::jlimit (0, juce::jmax (0, numRows - 1), hubRow);
    hubCol = juce::jlimit (0, juce::jmax (0, numCols - 1), hubCol);
    const bool typing = hubLine.isNotEmpty();

    switch (k.code)
    {
        case Key::Up:   hubRow = juce::jmax (0, hubRow - 1); return;
        case Key::Down: hubRow = juce::jmin (numRows - 1, hubRow + 1); return;

        case Key::Left:
        case Key::Right:
            if (k.shift)
                say (hubMoveLanding (k.code == Key::Right ? 1 : -1));
            else
                hubCol = juce::jlimit (0, juce::jmax (0, numCols - 1), hubCol + (k.code == Key::Right ? 1 : -1));

            return;

        case Key::Escape:
            if (typing)
                hubLine.clear();
            else
                closeHubPage();

            return;

        case Key::Enter:
        {
            auto line = hubLine.trim();
            hubLine.clear();

            if (line.startsWithIgnoreCase ("hub ") || line.equalsIgnoreCase ("hub"))
                line = line.substring (3).trim();

            if (line.equalsIgnoreCase ("q") || line.equalsIgnoreCase ("quit"))
                quit();
            else if (line.isNotEmpty())
                say (hubCommand (line));

            refreshHub (true);
            return;
        }

        case Key::Backspace:
        case Key::Delete:
            if (typing && k.code == Key::Backspace)
            {
                hubLine = hubLine.dropLastCharacters (1);
            }
            else if (! typing && numRows > 0 && numCols > 0)
            {
                const auto rows = hubRows();
                const auto cols = hubCols();

                if (const auto cell = hubCell (rows[(size_t) hubRow], cols[(size_t) hubCol]); cell.on)
                    say (hubToggle());
            }

            return;

        case Key::Char:
            if (k.ctrl || k.alt || k.ch < 32)
                return;

            if (! typing && k.ch == ' ')
            {
                say (hubToggle());
                return;
            }

            if (! typing && (k.ch == '[' || k.ch == ']'))
            {
                say (hubLevel (k.ch == ']' ? 1.0 : -1.0));
                return;
            }

            // i: the hub's interface, in a picker.
            if (! typing && (k.ch == 'i' || k.ch == 'I'))
            {
                openDevicePicker (true);
                return;
            }

            // n: name the app device on the chosen row (or column), on the command line.
            if (! typing && (k.ch == 'n' || k.ch == 'N') && numRows > 0)
            {
                const auto rows = hubRows();
                const auto cols = hubCols();
                const auto& row = rows[(size_t) hubRow];

                if (const auto key = hub::appNumber (row.key) > 0 ? row.key
                                     : numCols > 0 && hub::appNumber (cols[(size_t) hubCol].key) > 0 ? cols[(size_t) hubCol].key
                                                                                                    : juce::String();
                    key.isNotEmpty())
                {
                    hubLine = "name " + key + " ";
                    return;
                }
            }

            hubLine += juce::String::charToString ((juce::juce_wchar) k.ch);
            return;

        default:
            return;
    }
}
#else
Result App::hubCommand (const juce::String&) { return Result::fail ("the hub runs on macOS only so far"); }
Result App::hubRoute (const juce::String&, bool) { return Result::fail ("the hub runs on macOS only so far"); }
void App::openHubPage() { say (Result::fail ("the hub runs on macOS only so far")); }
void App::closeHubPage() {}
void App::handleHubPageKey (const Key&) {}
std::vector<App::HubEnd> App::hubRows() const { return {}; }
std::vector<App::HubEnd> App::hubCols() const { return {}; }
App::HubCell App::hubCell (const HubEnd&, const HubEnd&) const { return {}; }
juce::String App::hubAppName (const juce::String& key, bool) const { return key; }
bool App::hubAppNamed (const juce::String&) const { return false; }
void App::refreshHub (bool) {}
juce::String App::hubEndName (const juce::String&, int, bool) const { return {}; }
juce::String App::hubHardware (bool) const { return {}; }
#endif
} // namespace taw
