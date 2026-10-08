#pragma once

#include <juce_core/juce_core.h>

// macOS pieces of the hub that need Objective-C.
namespace taw::hub::mac
{
// Whether this process may use audio inputs: "granted", "denied", "not asked yet" or
// "restricted". macOS asks the person the first time an input is opened.
juce::String microphoneAccess();
} // namespace taw::hub::mac
