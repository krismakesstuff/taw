// The audio hub process: the interface, the engine routing it, and the server sessions connect
// to. On macOS launchd runs it from "taw hub.app".

#include "Hub.h"
#include "HubServer.h"
#include "Safety.h"
#include "Session.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>

#if JUCE_MAC
 #include "HubMac.h"
 #include <unistd.h>
#endif

namespace taw::hub
{
namespace
{
constexpr const char* label = "com.taw.hub"; // the launchd agent

void log (const juce::String& line)
{
    logFile().appendText (juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S  ") + line + "\n");
}

#if JUCE_MAC
juce::File agentFile()
{
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/LaunchAgents/" + juce::String (label) + ".plist");
}

// The hub runs from "taw hub.app" beside taw (the build makes it), or this is it already.
juce::File hubExecutable()
{
    const auto self = juce::File::getSpecialLocation (juce::File::currentExecutableFile);

    if (self.getParentDirectory().getParentDirectory().getParentDirectory().getFileName() == "taw hub.app")
        return self;

    return self.getSiblingFile ("taw hub.app").getChildFile ("Contents/MacOS/taw");
}

juce::String launchctl (const juce::StringArray& args)
{
    juce::ChildProcess p;
    juce::StringArray cmd { "/bin/launchctl" };
    cmd.addArray (args);

    if (! p.start (cmd))
        return "launchctl didn't run";

    const auto out = p.readAllProcessOutput().trim();
    return p.getExitCode() == 0 ? juce::String() : (out.isNotEmpty() ? out : "launchctl failed (" + juce::String (p.getExitCode()) + ")");
}

juce::String domain() { return "gui/" + juce::String ((int) getuid()); }
#endif
} // namespace

juce::File logFile() { return configFolder().getChildFile ("hub.log"); }

int run (const juce::StringArray&)
{
    configFolder().createDirectory();

    // One hub per user: a second one leaves quietly (launchd doesn't restart a clean exit).
    juce::InterProcessLock single ("taw-hub");

    if (! single.enter (0))
    {
        log ("another hub is running: leaving");
        return 0;
    }

    if (logFile().getSize() > 1024 * 1024)
        logFile().deleteFile();

    safety::installCloseHandler();
    log ("hub starting, pid " + juce::String (safety::thisProcess()) + " (" + juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName() + ")");

#if JUCE_MAC
    log ("microphone access: " + mac::microphoneAccess());
#endif

    // The interface: the hub's own choice once it has one, else the one taw sessions use (unless
    // that's the hub itself).
    juce::AudioDeviceManager devices;
    std::unique_ptr<juce::XmlElement> saved;

    for (const auto* name : { "hub-device.xml", "audio-device.xml" })
        if (const auto f = configFolder().getChildFile (name); f.existsAsFile() && saved == nullptr)
            if (auto xml = juce::parseXML (f); xml != nullptr && xml->getStringAttribute ("deviceType") != "taw hub")
                saved = std::move (xml);

    if (const auto error = devices.initialise (64, 2, saved.get(), true); error.isNotEmpty())
        log ("device: " + error);

    // Never one of the driver's own devices (the Mac's default output might be taw 1): that
    // would be a loop.
    auto setup = devices.getAudioDeviceSetup();

    if (setup.outputDeviceName.startsWith ("taw ") || setup.inputDeviceName.startsWith ("taw "))
        if (auto* type = devices.getCurrentDeviceTypeObject())
        {
            for (const auto& name : type->getDeviceNames (false))
                if (! name.startsWith ("taw "))
                {
                    setup.outputDeviceName = name;
                    break;
                }

            for (const auto& name : type->getDeviceNames (true))
                if (! name.startsWith ("taw "))
                {
                    setup.inputDeviceName = name;
                    break;
                }

            devices.setAudioDeviceSetup (setup, true);
            setup = devices.getAudioDeviceSetup();
        }

    // From now on the hub keeps its own interface, whatever sessions choose.
    if (const auto own = configFolder().getChildFile ("hub-device.xml"); ! own.existsAsFile())
        if (auto xml = devices.createStateXml())
            xml->writeTo (own);

    const auto interfaceName = setup.outputDeviceName == setup.inputDeviceName || setup.inputDeviceName.isEmpty()
                                   ? setup.outputDeviceName
                                   : setup.outputDeviceName + " + " + setup.inputDeviceName;
    int appChannels = 0;

#if JUCE_MAC
    // With the driver installed, the interface and the driver's hidden device together, as one
    // aggregate the hub opens: taw 1 to taw 4 are its last channels.
    if (mac::driverInstalled())
    {
        juce::String error;

        if (const auto aggregate = mac::makeAggregate (setup.outputDeviceName, setup.inputDeviceName, error); aggregate.isNotEmpty())
        {
            if (auto* type = devices.getCurrentDeviceTypeObject())
                type->scanForDevices();

            auto withApps = setup;
            withApps.outputDeviceName = withApps.inputDeviceName = aggregate;
            withApps.useDefaultInputChannels = withApps.useDefaultOutputChannels = false;
            withApps.inputChannels.setRange (0, 64, true);
            withApps.outputChannels.setRange (0, 64, true);

            if (const auto failed = devices.setAudioDeviceSetup (withApps, true); failed.isEmpty())
            {
                appChannels = mac::appChannels;
            }
            else
            {
                log ("couldn't open the aggregate (" + failed + "): no apps");
                devices.setAudioDeviceSetup (setup, true);
            }
        }
        else
        {
            log ("no apps: " + error);
        }
    }
    else
    {
        log ("no apps: taw.driver isn't installed (taw --install-driver)");
    }
#endif

    Engine engine;
    Server server (engine);
    HubInfo info;
    info.pid = safety::thisProcess();
    info.port = server.start();

    if (info.port == 0)
    {
        log ("couldn't listen for sessions");
        return 1;
    }

    devices.addAudioCallback (&engine);

    if (auto* d = devices.getCurrentAudioDevice())
    {
        // The interface's own channels; the apps' come after them.
        const int inputs = d->getActiveInputChannels().countNumberOfSetBits() - appChannels;
        info.inputs = sessionInputs (inputs);
        info.outputs = d->getActiveOutputChannels().countNumberOfSetBits() - appChannels;
        info.sampleRate = d->getCurrentSampleRate();
        info.blockSize = d->getCurrentBufferSizeSamples();
        info.device = interfaceName;
        server.setHardware (inputs, info.outputs, interfaceName, appChannels / 2);
        log ("device: " + d->getName() + " (" + d->getTypeName() + "), " + juce::String (info.sampleRate, 0) + " Hz, "
             + juce::String (info.blockSize) + " samples, " + juce::String (inputs) + " inputs, " + juce::String (info.outputs) + " outputs, "
             + juce::String (appChannels / 2) + " apps");
        log ("  inputs: " + d->getInputChannelNames().joinIntoString (", ") + " (on: " + d->getActiveInputChannels().toString (2) + ")");
        log ("  outputs: " + d->getOutputChannelNames().joinIntoString (", ") + " (on: " + d->getActiveOutputChannels().toString (2) + ")");
    }
    else
    {
        log ("no audio device");
    }

#if JUCE_MAC
    log ("microphone access after opening: " + mac::microphoneAccess());
#endif

    // Where sessions find it.
    infoFile().replaceWithText (juce::JSON::toString (info.toVar()));
    log ("listening on port " + juce::String (info.port));

    // Until it's stopped (launchctl bootout sends SIGTERM). Sessions coming and going are logged,
    // with the blocks each has missed.
    juce::StringArray lastNames;

    while (! safety::closeRequested())
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
        server.tidy();
        juce::StringArray names, details;

        for (const auto& c : server.clients())
        {
            names.add (c.name);
            details.add (c.name + " (" + juce::String (c.inputs) + " in, " + juce::String (c.outputs) + " out, " + juce::String (c.late) + " late)");
        }

        if (names != lastNames)
            log (details.isEmpty() ? juce::String ("no sessions") : "sessions: " + details.joinIntoString (", "));

        lastNames = names;
    }

    log ("hub stopping");
    server.stopAll();
    devices.removeAudioCallback (&engine);
    devices.closeAudioDevice();
#if JUCE_MAC
    mac::destroyAggregate();
#endif

    if (HubInfo::fromVar (juce::JSON::parse (infoFile())).pid == info.pid)
        infoFile().deleteFile();

    return 0;
}

juce::String install()
{
#if JUCE_MAC
    const auto exe = hubExecutable();

    if (! exe.existsAsFile())
        return "no hub at " + exe.getFullPathName() + " (build taw first: it makes \"taw hub.app\" beside taw)";

    // KeepAlive only after a failure: a hub that finds another running leaves for good.
    juce::String env;

    if (const auto custom = juce::SystemStats::getEnvironmentVariable ("TAW_CONFIG", {}); custom.isNotEmpty())
        env = "  <key>EnvironmentVariables</key>\n  <dict><key>TAW_CONFIG</key><string>" + configFolder().getFullPathName() + "</string></dict>\n";

    const auto plist = juce::String ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                     "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                                     "<plist version=\"1.0\">\n<dict>\n"
                                     "  <key>Label</key><string>") + label + "</string>\n"
                       "  <key>ProgramArguments</key>\n  <array><string>" + exe.getFullPathName() + "</string><string>--hub</string></array>\n"
                       "  <key>RunAtLoad</key><true/>\n"
                       "  <key>KeepAlive</key>\n  <dict><key>SuccessfulExit</key><false/></dict>\n"
                       "  <key>ProcessType</key><string>Interactive</string>\n"
                       "  <key>StandardErrorPath</key><string>" + configFolder().getChildFile ("hub-stderr.log").getFullPathName() + "</string>\n"
                       + env + "</dict>\n</plist>\n";

    configFolder().createDirectory();
    agentFile().getParentDirectory().createDirectory();
    launchctl ({ "bootout", domain() + "/" + label }); // the old one, if any

    if (! agentFile().replaceWithText (plist))
        return "couldn't write " + agentFile().getFullPathName();

    if (const auto error = launchctl ({ "bootstrap", domain(), agentFile().getFullPathName() }); error.isNotEmpty())
        return "couldn't start the hub: " + error;

    return "the hub starts at login and is running now (" + exe.getFullPathName() + L") \x00b7 its log: " + logFile().getFullPathName();
#else
    return "the hub runs on macOS only so far";
#endif
}

juce::String installDriver()
{
#if JUCE_MAC
    const auto bundle = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("taw.driver");
    const auto said = mac::installDriver (bundle);

    // The hub picks the devices up when it starts: restart it once the restarted audio service
    // has loaded the driver.
    for (int i = 0; i < 100 && ! mac::driverInstalled(); ++i)
        juce::Thread::sleep (100);

    if (agentFile().existsAsFile())
        launchctl ({ "kickstart", "-k", domain() + "/" + label });

    return said;
#else
    return "the hub runs on macOS only so far";
#endif
}

juce::String uninstallDriver()
{
#if JUCE_MAC
    const auto said = mac::uninstallDriver();

    if (agentFile().existsAsFile())
        launchctl ({ "kickstart", "-k", domain() + "/" + label });

    return said;
#else
    return "the hub runs on macOS only so far";
#endif
}

juce::String start()
{
#if JUCE_MAC
    if (! agentFile().existsAsFile())
        return install();

    launchctl ({ "bootstrap", domain(), agentFile().getFullPathName() }); // fails harmlessly if it's loaded

    if (const auto error = launchctl ({ "kickstart", domain() + "/" + label }); error.isNotEmpty())
        return "couldn't start the hub: " + error;

    return "the hub is running";
#else
    return "the hub runs on macOS only so far";
#endif
}

juce::String restart()
{
#if JUCE_MAC
    if (! agentFile().existsAsFile())
        return "the hub isn't installed (hub start)";

    if (const auto error = launchctl ({ "kickstart", "-k", domain() + "/" + label }); error.isNotEmpty())
        return "couldn't restart the hub: " + error;

    return "the hub restarted";
#else
    return "the hub runs on macOS only so far";
#endif
}

juce::String setInterface (const juce::String& output, const juce::String& input)
{
    const auto file = configFolder().getChildFile ("hub-device.xml");
    auto xml = juce::parseXML (file);

    if (xml == nullptr)
        xml = std::make_unique<juce::XmlElement> ("DEVICESETUP");

    xml->setAttribute ("deviceType", "CoreAudio");
    xml->setAttribute ("audioOutputDeviceName", output);

    if (input.isNotEmpty())
        xml->setAttribute ("audioInputDeviceName", input);

    // Its rate, its channels: the device's own (taken when it opens).
    for (const auto* a : { "audioDeviceRate", "audioDeviceInChans", "audioDeviceOutChans" })
        xml->removeAttribute (a);

    if (! xml->writeTo (file))
        return "couldn't write " + file.getFullPathName();

    const auto chosen = output + (input.isNotEmpty() && input != output ? " + " + input : juce::String());
    const auto restarted = restart();
    return restarted == "the hub restarted" ? "the hub's interface is " + chosen : "the hub's interface will be " + chosen + L" \x00b7 " + restarted;
}

juce::String stop()
{
#if JUCE_MAC
    if (const auto error = launchctl ({ "bootout", domain() + "/" + label }); error.isNotEmpty())
        return "the hub wasn't running";

    return L"the hub is stopped \x00b7 it starts again at login, or with hub start";
#else
    return "the hub runs on macOS only so far";
#endif
}

juce::String uninstall()
{
#if JUCE_MAC
    const auto error = launchctl ({ "bootout", domain() + "/" + label });
    const bool had = agentFile().existsAsFile();
    agentFile().deleteFile();
    return had ? juce::String ("the hub is stopped and won't start at login") : "no hub was installed" + (error.isNotEmpty() ? " (" + error + ")" : juce::String());
#else
    return "the hub runs on macOS only so far";
#endif
}
} // namespace taw::hub
