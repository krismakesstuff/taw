#pragma once

#include <juce_core/juce_core.h>

// macOS pieces of the hub that need Objective-C or CoreAudio itself.
namespace taw::hub::mac
{
// Whether this process may use audio inputs: "granted", "denied", "not asked yet" or
// "restricted". macOS asks the person the first time an input is opened.
juce::String microphoneAccess();

// The driver's devices (taw.driver: "taw 1" to "taw 4", and the hidden "taw hub" the hub uses).
inline constexpr int appDevices = 4;   // taw 1 .. taw 4
inline constexpr int appChannels = 2 * appDevices; // the hidden device's channels each way
bool driverInstalled();

// A private aggregate device (only this process sees it): the interface (its output device,
// and its input device when that's another one), clocked by it, and the driver's hidden device
// after it, drift-corrected, at the interface's rate. Returns its name, for JUCE to open, or
// empty with the reason in error. Its last appChannels inputs and outputs are the apps'.
juce::String makeAggregate (const juce::String& outputDevice, const juce::String& inputDevice, juce::String& error);
void destroyAggregate();

// Installs or removes taw.driver in /Library/Audio/Plug-Ins/HAL (asks for an admin password)
// and restarts coreaudiod. Returns what happened.
juce::String installDriver (const juce::File& bundle);
juce::String uninstallDriver();
} // namespace taw::hub::mac
