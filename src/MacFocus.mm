#include "MacFocus.h"

#import <AppKit/AppKit.h>

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
