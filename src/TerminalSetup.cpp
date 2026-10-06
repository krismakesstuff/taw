#include "TerminalSetup.h"
#include "Session.h"

#include <cstdio>
#include <unistd.h>

namespace taw::terminalSetup
{
namespace
{
const juce::File terminalDefaults ("/System/Applications/Utilities/Terminal.app/Contents/Resources/keyMappings.plist");

juce::File askedFile() { return configFolder().getChildFile ("terminal-profile.json"); }

// Keys in Terminal's format: $ shift, ^ ctrl, ~ option, then the key's code (F700 up, F701 down,
// F702 left, F703 right, F729 home, F72B end) or, for an ordinary key, its character. Values are
// what taw decodes (xterm sequences; kitty's CSI u for keys xterm has none for).
const std::pair<const char*, const char*> added[] = {
    { "$F700", "\x1b[1;2A" },  { "$F701", "\x1b[1;2B" },   // shift+up/down: select tracks
    { "~F700", "\x1b[1;3A" },  { "~F701", "\x1b[1;3B" },   // option+up/down: move tracks
    { "^F700", "\x1b[1;5A" },  { "^F701", "\x1b[1;5B" },   // ctrl+up/down: the browser's jumps
    { "$\r", "\x1b[13;2u" },                               // shift+return: apart from return
    { "^m", "\x1b[109;5u" },   { "^[", "\x1b[91;5u" },     // ctrl+m, ctrl+[: apart from return, esc
    { "F729", "\x1b[H" },      { "F72B", "\x1b[F" },       // home/end (fn+left/right)
    { "$F729", "\x1b[1;2H" },  { "$F72B", "\x1b[1;2F" },   // shift+home/end: select to there
    { "~^F702", "\x1b[1;7D" }, { "~^F703", "\x1b[1;7C" },  // ctrl+option+left/right: 4 steps
    { "$^F702", "\x1b[1;6D" }, { "$^F703", "\x1b[1;6C" },  // ...and with shift, selecting
    { "$~F702", "\x1b[1;4D" }, { "$~F703", "\x1b[1;4C" },
    { "$~^F702", "\x1b[1;8D" }, { "$~^F703", "\x1b[1;8C" },
};

juce::String run (const juce::StringArray& args, bool& ok)
{
    juce::ChildProcess p;
    ok = p.start (args);
    const auto out = ok ? p.readAllProcessOutput() : juce::String();
    ok = ok && p.waitForProcessToFinish (5000) && p.getExitCode() == 0;
    return out;
}

void remember (bool installed)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("asked", juce::Time::getCurrentTime().toISO8601 (true));
    o->setProperty ("installed", installed);
    configFolder().createDirectory();
    askedFile().replaceWithText (juce::JSON::toString (juce::var (o)));
}
} // namespace

juce::Result install()
{
    // Start from Terminal's own key list (a profile's list replaces it), then add taw's keys.
    bool ok = false;
    auto keys = juce::JSON::parse (run ({ "plutil", "-convert", "json", "-o", "-", terminalDefaults.getFullPathName() }, ok));
    auto* map = ok ? keys.getDynamicObject() : nullptr;

    if (map == nullptr)
    {
        map = new juce::DynamicObject();
        keys = juce::var (map);
    }

    for (const auto& [key, text] : added)
        map->setProperty (key, juce::String (text));

    auto* profile = new juce::DynamicObject();
    profile->setProperty ("name", "taw");
    profile->setProperty ("type", "Window Settings");
    profile->setProperty ("ProfileCurrentVersion", 2.01);
    profile->setProperty ("useOptionAsMetaKey", true);
    profile->setProperty ("keyMapBoundKeys", keys);

    // Terminal reads property lists; plutil turns the JSON into one. Binary, because XML can't
    // carry shift+return's key (a carriage return in a key name).
    configFolder().createDirectory();
    const auto json = configFolder().getChildFile ("taw.terminal.json");
    const auto target = configFolder().getChildFile ("taw.terminal");

    if (! json.replaceWithText (juce::JSON::toString (juce::var (profile))))
        return juce::Result::fail ("can't write " + json.getFullPathName());

    run ({ "plutil", "-convert", "binary1", "-o", target.getFullPathName(), json.getFullPathName() }, ok);
    json.deleteFile();

    if (! ok)
        return juce::Result::fail ("couldn't make the Terminal profile");

    run ({ "open", target.getFullPathName() }, ok);
    return ok ? juce::Result::ok() : juce::Result::fail ("couldn't open " + target.getFullPathName());
}

void offerOnce()
{
    if (juce::SystemStats::getEnvironmentVariable ("TERM_PROGRAM", {}) != "Apple_Terminal"
        || askedFile().existsAsFile() || ! isatty (STDIN_FILENO) || ! isatty (STDOUT_FILENO))
        return;

    std::puts ("taw works best in Terminal with a few keyboard settings: Option as Alt, and\n"
               "Shift+\xe2\x86\x91\xe2\x86\x93, Option+\xe2\x86\x91\xe2\x86\x93, Ctrl+\xe2\x86\x91\xe2\x86\x93, Shift+Return, Home/End (fn+\xe2\x86\x90\xe2\x86\x92) and\n"
               "Ctrl+Option+\xe2\x86\x90\xe2\x86\x92 passed on to taw.\n");
    std::fputs ("Add a \"taw\" Terminal profile with these settings? Your own profiles stay as they are. [y/n] ", stdout);
    std::fflush (stdout);

    char line[64] {};
    const bool yes = std::fgets (line, sizeof (line), stdin) != nullptr && (line[0] == 'y' || line[0] == 'Y');

    if (! yes)
    {
        remember (false);
        std::puts ("\nOK. To add it later: taw --terminal-profile\n");
    }
    else if (const auto r = install(); r.failed())
    {
        std::printf ("\n%s. To try again: taw --terminal-profile\n\n", r.getErrorMessage().toRawUTF8());
    }
    else
    {
        remember (true);
        std::puts ("\nAdded. Terminal opened a new window with the taw profile: run taw there, or make it\n"
                   "your default in Terminal > Settings > Profiles (select taw, then Default).");
    }

    std::puts ("\nOne more, done in System Settings: Keyboard > Keyboard Shortcuts > Mission Control,\n"
               "turn off \"Mission Control\", \"Application windows\", \"Move left a space\" and \"Move right a space\"\n"
               "so Ctrl+\xe2\x86\x91\xe2\x86\x93\xe2\x86\x90\xe2\x86\x92 reach taw.\n");
    std::fputs ("Press Enter to continue.", stdout);
    std::fflush (stdout);
    std::fgets (line, sizeof (line), stdin);
}
} // namespace taw::terminalSetup
