// The hub from inside taw: the hub command and its panel. The hub itself is its own process
// (Hub.cpp); this asks it for its status and sends it routes.

#include "App.h"

#if TAW_HUB
 #include "Hub.h"
 #include "HubDevice.h"
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
}

juce::String App::hubEndName (const juce::String& key, int channel, bool source) const
{
    if (key == interfaceKey)
        return (source ? "in " : "out ") + juce::String (channel);

    if (auto* sessions = hubStatus["sessions"].getArray())
        for (const auto& s : *sessions)
            if (s["key"].toString() == key)
                return s["name"].toString() + " " + juce::String (channel);

    return juce::File (key).getFileName() + " " + juce::String (channel);
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

    if (! add && from->count() == 0 && to->count() == 0)
    {
        pairs.push_back ({ 0, 0 }); // all of them
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
        auto s = hubEndName (e.key, first, source);
        return last > first ? s + "-" + juce::String (last) : s;
    };

    const auto fromText = describe (*from, pairs.front().first, pairs.back().first, true);
    const auto toText = describe (*to, pairs.front().second, pairs.back().second, false);

    if (! add)
        return removed == 0 ? Result::fail ("no such route") : Result::good ("took out " + juce::String (removed) + (removed == 1 ? " route" : " routes"));

    return Result::good (fromText + L" \x2192 " + toText + (db != 0.0 ? "  " + juce::String (db, 1) + " dB" : juce::String())
                         + (from->key != interfaceKey && to->key != interfaceKey ? juce::String (L" \x00b7 a block later (session to session)") : juce::String()));
}

Result App::hubCommand (const juce::String& rest)
{
    const auto word = rest.upToFirstOccurrenceOf (" ", false, false).toLowerCase();
    const auto args = rest.fromFirstOccurrenceOf (" ", false, false).trim();

    if (word == "route" || word == "unroute")
        return hubRoute (args, word == "route");

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

    if (word == "start" || word == "stop" || word == "install" || word == "uninstall")
    {
        const auto said = word == "start" ? hub::start() : word == "stop" ? hub::stop() : word == "install" ? hub::install() : hub::uninstall();
        hubAskedAt = 0.0; // ask again
        return Result::good (said);
    }

    if (word.isNotEmpty())
        return Result::fail (L"hub \x00b7 hub route <from> > <to> \x00b7 hub unroute <from> > <to> \x00b7 hub forget <session> \x00b7 hub start \x00b7 hub stop");

    hubPanelVisible = ! hubPanelVisible;

    if (! hubPanelVisible)
        return Result::good();

    helpVisible = settingsVisible = pluginPickerVisible = midiPanelVisible = false;
    hubPanelIndex = 0;
    refreshHub (true);
    return hubStatus.isVoid() ? Result::fail (hubError) : Result::good();
}

bool App::handleHubPanelKey (const Key& k)
{
    if (! hubPanelVisible || ! commandLine.isEmpty())
        return false;

    const auto* routes = hubStatus["routes"].getArray();
    const int n = routes != nullptr ? routes->size() : 0;
    hubPanelIndex = juce::jlimit (0, juce::jmax (0, n - 1), hubPanelIndex);

    auto send = [this, routes] (const juce::String& type, double db)
    {
        const auto& r = routes->getReference (hubPanelIndex);
        auto m = hubMessage (type);

        for (const auto* p : { "from", "fromChannel", "to", "toChannel" })
            m.getDynamicObject()->setProperty (p, r[p]);

        m.getDynamicObject()->setProperty ("db", db);
        juce::String error;

        if (const auto answer = hub::request (m, error); error.isEmpty())
            hubStatus = answer;
        else
            say (Result::fail (error));
    };

    switch (k.code)
    {
        case Key::Up:     hubPanelIndex = juce::jmax (0, hubPanelIndex - 1); return true;
        case Key::Down:   hubPanelIndex = juce::jmin (n - 1, hubPanelIndex + 1); return true;
        case Key::Escape: hubPanelVisible = false; return true;
        case Key::Delete:
        case Key::Backspace:
            if (n > 0)
                send ("unroute", 0.0);

            return true;
        case Key::Char:
            // [ ] the route's level, a dB at a time (from -60 to +12).
            if ((k.ch == '[' || k.ch == ']') && ! k.ctrl && ! k.alt && n > 0)
            {
                const double db = juce::jlimit (-60.0, 12.0, std::round ((double) routes->getReference (hubPanelIndex)["db"]) + (k.ch == ']' ? 1.0 : -1.0));
                send ("route", db);
                return true;
            }

            return false; // typing starts a command as usual
        default:
            return false;
    }
}
#else
Result App::hubCommand (const juce::String&) { return Result::fail ("the hub runs on macOS only so far"); }
Result App::hubRoute (const juce::String&, bool) { return Result::fail ("the hub runs on macOS only so far"); }
void App::refreshHub (bool) {}
bool App::handleHubPanelKey (const Key&) { return false; }
juce::String App::hubEndName (const juce::String&, int, bool) const { return {}; }
#endif
} // namespace taw
