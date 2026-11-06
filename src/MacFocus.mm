#include "MacFocus.h"

#import <AppKit/AppKit.h>

#include <cstdio>
#include <cstdlib>
#include <sys/sysctl.h>
#include <unistd.h>

namespace taw::macFocus
{
namespace
{
pid_t parentOf (pid_t pid)
{
    kinfo_proc info {};
    size_t size = sizeof (info);
    int mib[] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pid };
    return sysctl (mib, 4, &info, &size, nullptr, 0) == 0 && size > 0 ? info.kp_eproc.e_ppid : 0;
}

// The nearest ancestor process that is an app with windows (taw's shell's terminal). Inside
// tmux there's none (its server isn't under the terminal): then the app the shell says it was
// started from (__CFBundleIdentifier).
NSRunningApplication* terminalApp()
{
    for (auto pid = parentOf (getpid()); pid > 1; pid = parentOf (pid))
        if (auto* app = [NSRunningApplication runningApplicationWithProcessIdentifier: pid])
            if (app.activationPolicy == NSApplicationActivationPolicyRegular)
                return app;

    if (const char* bundle = std::getenv ("__CFBundleIdentifier"))
        if (NSString* name = [NSString stringWithUTF8String: bundle])
            return [NSRunningApplication runningApplicationsWithBundleIdentifier: name].firstObject;

    return nil;
}
} // namespace

void log (const std::string& line)
{
    static const bool on = std::getenv ("TAW_FOCUS_LOG") != nullptr;
    static std::string last;

    if (! on || line == last)
        return;

    last = line;
    const char* tmp = std::getenv ("TMPDIR");

    if (FILE* f = std::fopen ((std::string (tmp != nullptr ? tmp : "/tmp/") + "/taw-focus.log").c_str(), "a"))
    {
        std::fprintf (f, "%.3f %s\n", [NSDate date].timeIntervalSince1970, line.c_str());
        std::fclose (f);
    }
}

void holdKeyboard (void* handle)
{
    @autoreleasepool
    {
        auto* view = (NSView*) handle;
        NSWindow* window = view != nil ? view.window : nil;

        if (window == nil)
            return;

        // A process started from a shell may not be allowed to come to the front at all; an
        // accessory app can (and still has no Dock icon).
        const auto policy = NSApp.activationPolicy;

        if (policy == NSApplicationActivationPolicyProhibited)
            [NSApp setActivationPolicy: NSApplicationActivationPolicyAccessory];

        // You just double-clicked taw's window, so macOS lets it take the front.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [NSApp activateIgnoringOtherApps: YES];
#pragma clang diagnostic pop
        [window makeKeyAndOrderFront: nil];

        log ("hold: policy " + std::to_string ((int) policy) + ", taw in front " + (NSApp.isActive ? "yes" : "no")
             + ", window key " + (window.isKeyWindow ? "yes" : "no"));
    }
}

Front front (const std::vector<void*>& views, void* keeper)
{
    @autoreleasepool
    {
        // NSApp.isActive can stay true after the keyboard went back to the terminal: clicks in
        // other apps then read as clicks in a plugin window, and each one handed the keyboard
        // back to the terminal. What macOS has in front settles it.
        auto* frontApp = [NSWorkspace sharedWorkspace].frontmostApplication;

        if (! NSApp.isActive || frontApp == nil || frontApp.processIdentifier != getpid())
            return Front::notTaw;

        auto* key = NSApp.keyWindow;

        if (keeper != nullptr && key != nil && key == ((NSView*) keeper).window)
            return Front::keeper;

        if (NSApp.modalWindow != nil)
            return Front::other;

        if ([NSEvent pressedMouseButtons] != 0)
            return Front::button;

        for (NSWindow* w in NSApp.windows)
            if (w.isVisible && w.level >= NSPopUpMenuWindowLevel)
                return Front::menu;

        // Nothing takes the keys: a plugin window's click, if one is open. With none open there's
        // nothing to give the keyboard back from.
        if (key == nil)
            return views.empty() ? Front::notTaw : Front::plugin;

        for (auto* v : views)
            if (v != nullptr && ((NSView*) v).window == key)
                return Front::plugin;

        return Front::other;
    }
}

bool tawInFront()
{
    @autoreleasepool
    {
        auto* app = [NSWorkspace sharedWorkspace].frontmostApplication;
        return app != nil && app.processIdentifier == getpid();
    }
}

bool terminalInFront()
{
    @autoreleasepool
    {
        auto* front = [NSWorkspace sharedWorkspace].frontmostApplication;
        auto* term = terminalApp();
        return front != nil && term != nil && front.processIdentifier == term.processIdentifier;
    }
}

void activateTerminal()
{
    @autoreleasepool
    {
        auto* app = terminalApp();

        if (app == nil)
            return;

        // Since macOS 14 an app is only activated when the one in front hands over: that's taw,
        // with an editor window in front.
        if (@available (macOS 14.0, *))
        {
            [NSApp yieldActivationToApplication: app];
            [app activateWithOptions: 0];
        }
        else
        {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [app activateWithOptions: NSApplicationActivateIgnoringOtherApps];
#pragma clang diagnostic pop
        }
    }
}

int frontmostApp()
{
    @autoreleasepool
    {
        auto* app = [NSWorkspace sharedWorkspace].frontmostApplication;
        return app != nil ? (int) app.processIdentifier : 0;
    }
}
} // namespace taw::macFocus
