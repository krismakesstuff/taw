#include "Timing.h"
#include "Session.h"

namespace taw::timing
{
namespace
{
double launched = 0.0, lastMark = 0.0; // set by the first line
bool started = false;
std::mutex lock;

void write (double now, double took, const juce::String& what)
{
    const std::scoped_lock l (lock);
    auto file = logFile();

    if (! started)
    {
        started = true;
        launched = now - took;
        file.replaceWithText ("taw " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S") + "\n");
    }

    file.appendText (juce::String ((int) std::lround (now - launched)).paddedLeft (' ', 7) + " ms  "
                     + ("+" + juce::String ((int) std::lround (took))).paddedLeft (' ', 6) + " ms  " + what + "\n");
}
} // namespace

juce::File logFile() { return configFolder().getChildFile ("timing.log"); }

void mark (const juce::String& what)
{
    const auto now = juce::Time::getMillisecondCounterHiRes();
    write (now, started ? now - lastMark : 0.0, what);
    lastMark = now;
}

Scope::~Scope()
{
    const auto now = juce::Time::getMillisecondCounterHiRes();

    if (now - start >= minMs)
        write (now, now - start, what);

    lastMark = now;
}
} // namespace taw::timing
