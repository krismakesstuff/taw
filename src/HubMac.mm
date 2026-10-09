#include "HubMac.h"

#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#include <CoreAudio/CoreAudio.h>

#include <libproc.h>
#include <vector>

// Which process stands behind another (Chrome for its helpers); libsystem has it, no header.
extern "C" pid_t responsibility_get_pid_responsible_for_pid (pid_t);

namespace taw::hub::mac
{
namespace
{
constexpr const char* driverHubUID = "com.taw.driver.hub";
constexpr const char* aggregateName = "taw hub interface";
AudioObjectID aggregate = kAudioObjectUnknown;

AudioObjectPropertyAddress global (AudioObjectPropertySelector s)
{
    return { s, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
}

juce::String stringProperty (AudioObjectID object, AudioObjectPropertySelector s)
{
    CFStringRef value = nullptr;
    UInt32 size = sizeof (value);
    const auto address = global (s);

    if (AudioObjectGetPropertyData (object, &address, 0, nullptr, &size, &value) != noErr || value == nullptr)
        return {};

    const auto text = juce::String::fromCFString (value);
    CFRelease (value);
    return text;
}

bool hasStreams (AudioObjectID device, bool output)
{
    const AudioObjectPropertyAddress address { kAudioDevicePropertyStreams, output ? kAudioObjectPropertyScopeOutput : kAudioObjectPropertyScopeInput,
                                               kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    return AudioObjectGetPropertyDataSize (device, &address, 0, nullptr, &size) == noErr && size > 0;
}

// By name, with streams that way: a Bluetooth headset is two devices of the same name, one
// each way.
AudioObjectID deviceNamed (const juce::String& name, bool output)
{
    const auto address = global (kAudioHardwarePropertyDevices);
    UInt32 size = 0;

    if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr)
        return kAudioObjectUnknown;

    std::vector<AudioObjectID> ids (size / sizeof (AudioObjectID));

    if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &address, 0, nullptr, &size, ids.data()) != noErr)
        return kAudioObjectUnknown;

    for (auto id : ids)
        if (stringProperty (id, kAudioObjectPropertyName) == name && hasStreams (id, output))
            return id;

    return kAudioObjectUnknown;
}

AudioObjectID deviceWithUID (const juce::String& uid)
{
    CFStringRef cf = uid.toCFString();
    AudioObjectID id = kAudioObjectUnknown;
    UInt32 size = sizeof (id);
    const auto address = global (kAudioHardwarePropertyTranslateUIDToDevice);
    AudioObjectGetPropertyData (kAudioObjectSystemObject, &address, sizeof (cf), &cf, &size, &id);
    CFRelease (cf);
    return id;
}

double rateOf (AudioObjectID device)
{
    Float64 rate = 0.0;
    UInt32 size = sizeof (rate);
    const auto address = global (kAudioDevicePropertyNominalSampleRate);
    AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, &rate);
    return rate;
}

// Sets a device's rate and waits (up to a second) for it to take.
bool setRate (AudioObjectID device, double rate)
{
    auto there = [&] { return std::abs (rateOf (device) - rate) < 0.5; };

    if (there())
        return true;

    Float64 value = rate;
    const auto address = global (kAudioDevicePropertyNominalSampleRate);

    if (AudioObjectSetPropertyData (device, &address, 0, nullptr, sizeof (value), &value) != noErr)
        return false;

    for (int i = 0; i < 100 && ! there(); ++i)
        juce::Thread::sleep (10);

    return there();
}

NSString* ns (const juce::String& s) { return [NSString stringWithUTF8String: s.toRawUTF8()]; }

juce::String run (const juce::StringArray& command)
{
    juce::ChildProcess p;

    if (! p.start (command))
        return "couldn't run " + command[0];

    const auto out = p.readAllProcessOutput().trim();
    return p.getExitCode() == 0 ? juce::String() : (out.isNotEmpty() ? out : "failed (" + juce::String (p.getExitCode()) + ")");
}

// A shell command run as root: macOS asks for an admin password.
juce::String asAdmin (const juce::String& shell)
{
    return run ({ "/usr/bin/osascript", "-e", "do shell script \"" + shell + "\" with administrator privileges" });
}
} // namespace

juce::String microphoneAccess()
{
    switch ([AVCaptureDevice authorizationStatusForMediaType: AVMediaTypeAudio])
    {
        case AVAuthorizationStatusAuthorized:    return "granted";
        case AVAuthorizationStatusDenied:        return "denied";
        case AVAuthorizationStatusNotDetermined: return "not asked yet";
        case AVAuthorizationStatusRestricted:    return "restricted";
    }

    return "unknown";
}

bool driverInstalled() { return deviceWithUID (driverHubUID) != kAudioObjectUnknown; }

namespace
{
template <typename T>
std::vector<T> arrayProperty (AudioObjectID object, AudioObjectPropertySelector s, AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    const AudioObjectPropertyAddress address { s, scope, kAudioObjectPropertyElementMain };
    UInt32 size = 0;

    if (AudioObjectGetPropertyDataSize (object, &address, 0, nullptr, &size) != noErr || size == 0)
        return {};

    std::vector<T> values (size / sizeof (T));

    if (AudioObjectGetPropertyData (object, &address, 0, nullptr, &size, values.data()) != noErr)
        return {};

    values.resize (size / sizeof (T));
    return values;
}

template <typename T>
T scalarProperty (AudioObjectID object, AudioObjectPropertySelector s)
{
    const auto values = arrayProperty<T> (object, s);
    return values.empty() ? T {} : values.front();
}

// An app's name for a process: the app behind it (Chrome for "Google Chrome Helper"). A tool
// with no bundle (afplay) goes by its own name: the app behind it is only the terminal.
juce::String appName (pid_t pid, bool bundled)
{
    const auto owner = bundled ? responsibility_get_pid_responsible_for_pid (pid) : pid_t (-1);

    for (auto p : { owner, pid })
        if (p > 0)
            if (auto* app = [NSRunningApplication runningApplicationWithProcessIdentifier: p]; app != nil && app.localizedName != nil)
                return juce::String (app.localizedName.UTF8String);

    char name[2 * MAXCOMLEN] {};
    proc_name (pid, name, sizeof (name));
    return juce::String (name);
}
} // namespace

std::map<juce::String, DeviceUsers> appDeviceUsers()
{
    std::map<juce::String, DeviceUsers> users;
    const auto me = getpid();

    @autoreleasepool
    {
        for (auto process : arrayProperty<AudioObjectID> (kAudioObjectSystemObject, kAudioHardwarePropertyProcessObjectList))
        {
            const auto pid = scalarProperty<pid_t> (process, kAudioProcessPropertyPID);
            const auto bundle = stringProperty (process, kAudioProcessPropertyBundleID);

            if (pid == me || bundle == "com.taw.hub")
                continue;

            // A process lists its device both ways; the running flags say which way it's used.
            juce::String name;

            for (const bool output : { true, false })
            {
                if (scalarProperty<UInt32> (process, output ? kAudioProcessPropertyIsRunningOutput : kAudioProcessPropertyIsRunningInput) == 0)
                    continue;

                for (auto device : arrayProperty<AudioObjectID> (process, kAudioProcessPropertyDevices,
                                                                 output ? kAudioObjectPropertyScopeOutput : kAudioObjectPropertyScopeInput))
                {
                    const auto deviceName = stringProperty (device, kAudioObjectPropertyName);

                    if (! deviceName.startsWith ("taw ") || deviceName == "taw hub")
                        continue;

                    if (name.isEmpty())
                        name = appName (pid, bundle.isNotEmpty());

                    auto& list = output ? users[deviceName].playing : users[deviceName].listening;
                    list.addIfNotAlreadyThere (name);
                }
            }
        }
    }

    // The system's own output and input: what anything set to the Mac's sound uses.
    for (const bool output : { true, false })
    {
        const auto device = scalarProperty<AudioObjectID> (kAudioObjectSystemObject, output ? kAudioHardwarePropertyDefaultOutputDevice
                                                                                            : kAudioHardwarePropertyDefaultInputDevice);
        const auto deviceName = stringProperty (device, kAudioObjectPropertyName);

        if (deviceName.startsWith ("taw ") && deviceName != "taw hub")
            (output ? users[deviceName].playing : users[deviceName].listening).add (output ? "Mac's output" : "Mac's input");
    }

    return users;
}

juce::String makeAggregate (const juce::String& outputDevice, const juce::String& inputDevice, juce::String& error)
{
    destroyAggregate();

    const auto out = deviceNamed (outputDevice, true);
    auto in = inputDevice.isNotEmpty() ? deviceNamed (inputDevice, false) : kAudioObjectUnknown;

    if (in == out)
        in = kAudioObjectUnknown; // one device both ways
    const auto hub = deviceWithUID (driverHubUID);

    if (out == kAudioObjectUnknown || hub == kAudioObjectUnknown)
    {
        error = out == kAudioObjectUnknown ? "no device called " + outputDevice : juce::String ("taw.driver isn't installed");
        return {};
    }

    // One rate for all of them: the interface's.
    const auto rate = rateOf (out);

    if ((in != kAudioObjectUnknown && ! setRate (in, rate)) || ! setRate (hub, rate))
    {
        error = "couldn't set everything to " + juce::String (rate, 0) + " Hz";
        return {};
    }

    auto subDevice = [] (AudioObjectID id, bool driftCorrected)
    {
        return @{ @kAudioSubDeviceUIDKey: ns (stringProperty (id, kAudioDevicePropertyDeviceUID)),
                  @kAudioSubDeviceDriftCompensationKey: @(driftCorrected ? 1 : 0) };
    };

    NSMutableArray* subDevices = [NSMutableArray arrayWithObject: subDevice (out, false)];

    if (in != kAudioObjectUnknown)
        [subDevices addObject: subDevice (in, true)];

    [subDevices addObject: subDevice (hub, true)];

    NSDictionary* description = @{
        @kAudioAggregateDeviceNameKey: ns (aggregateName),
        @kAudioAggregateDeviceUIDKey: @"com.taw.hub.aggregate",
        @kAudioAggregateDeviceIsPrivateKey: @1,
        @kAudioAggregateDeviceMainSubDeviceKey: ns (stringProperty (out, kAudioDevicePropertyDeviceUID)),
        @kAudioAggregateDeviceSubDeviceListKey: subDevices
    };

    if (const auto status = AudioHardwareCreateAggregateDevice ((__bridge CFDictionaryRef) description, &aggregate); status != noErr)
    {
        aggregate = kAudioObjectUnknown;
        error = "couldn't make the aggregate device (" + juce::String ((int) status) + ")";
        return {};
    }

    // It takes a moment to be ready.
    for (int i = 0; i < 50 && deviceNamed (aggregateName, true) == kAudioObjectUnknown; ++i)
        juce::Thread::sleep (20);

    return aggregateName;
}

void destroyAggregate()
{
    if (aggregate != kAudioObjectUnknown)
        AudioHardwareDestroyAggregateDevice (aggregate);

    aggregate = kAudioObjectUnknown;
}

juce::String installDriver (const juce::File& bundle)
{
    if (! bundle.isDirectory())
        return "no driver at " + bundle.getFullPathName() + " (build taw first)";

    const auto target = juce::String ("/Library/Audio/Plug-Ins/HAL/taw.driver");

    if (const auto error = asAdmin ("rm -rf '" + target + "' && cp -R '" + bundle.getFullPathName() + "' '" + target + "' && killall coreaudiod");
        error.isNotEmpty())
        return "couldn't install the driver: " + error;

    return L"installed taw.driver: taw 1 to taw 4 are in every app's audio devices \x00b7 hub stop, then hub start, to use them";
}

juce::String uninstallDriver()
{
    if (const auto error = asAdmin ("rm -rf '/Library/Audio/Plug-Ins/HAL/taw.driver' && killall coreaudiod"); error.isNotEmpty())
        return "couldn't remove the driver: " + error;

    return "removed taw.driver";
}
} // namespace taw::hub::mac
