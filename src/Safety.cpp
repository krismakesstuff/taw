#include "Safety.h"
#include "Session.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdlib>
#include <exception>
#pragma comment(lib, "dbghelp.lib")

namespace taw::safety
{
namespace
{
// Everything the crash handler touches is set up in advance: after a crash the heap may be
// damaged, so the handler itself allocates nothing.
wchar_t crashDir[MAX_PATH] {};
std::atomic<bool> crashing { false };

void (*emergencySave) (void*) = nullptr;
void* emergencyContext = nullptr;

HANDLE consoleIn = nullptr, consoleOut = nullptr;
DWORD consoleInMode = 0, consoleOutMode = 0;
bool haveConsole = false;

std::atomic<bool> closing { false };
bool runMarker = false;
HANDLE shutdownDone = nullptr;

void writeOut (const char* text)
{
    DWORD written = 0;
    WriteFile (GetStdHandle (STD_OUTPUT_HANDLE), text, (DWORD) std::strlen (text), &written, nullptr);
}

// Writes the dump and log, restores the console, tells the user, then tries a last save.
void handleCrash (EXCEPTION_POINTERS* info, const char* what)
{
    if (crashing.exchange (true))
        return; // crashed again in here: give up quietly

    SYSTEMTIME t;
    GetLocalTime (&t);
    wchar_t stem[MAX_PATH], dumpPath[MAX_PATH], logPath[MAX_PATH];
    swprintf_s (stem, L"%s\\crash-%04d%02d%02d-%02d%02d%02d", crashDir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    swprintf_s (dumpPath, L"%s.dmp", stem);
    swprintf_s (logPath, L"%s.txt", stem);

    CreateDirectoryW (crashDir, nullptr);

    // 1. The minidump: every thread's stack, enough to find the bug.
    const HANDLE dump = CreateFileW (dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (dump != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei { GetCurrentThreadId(), info, FALSE };
        MiniDumpWriteDump (GetCurrentProcess(), GetCurrentProcessId(), dump,
                           (MINIDUMP_TYPE) (MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                           info != nullptr ? &mei : nullptr, nullptr, nullptr);
        CloseHandle (dump);
    }

    // 2. A readable line about what happened.
    char line[512];
    const auto code = info != nullptr ? info->ExceptionRecord->ExceptionCode : 0;
    const auto* address = info != nullptr ? info->ExceptionRecord->ExceptionAddress : nullptr;
    const int n = sprintf_s (line, "taw %s crashed: %s (code 0x%08lX at %p, thread %lu)\r\n", TAW_VERSION, what,
                             (unsigned long) code, address, (unsigned long) GetCurrentThreadId());

    const HANDLE log = CreateFileW (logPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (log != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile (log, line, (DWORD) n, &written, nullptr);
        CloseHandle (log);
    }

    // 3. Leave the alternate screen and give the console back.
    if (haveConsole)
    {
        writeOut ("\x1b[0m\x1b[?25h\x1b[?1049l");
        SetConsoleMode (consoleIn, consoleInMode);
        SetConsoleMode (consoleOut, consoleOutMode);
    }

    char path[MAX_PATH * 3] {};
    WideCharToMultiByte (CP_UTF8, 0, logPath, -1, path, (int) sizeof (path), nullptr, nullptr);
    writeOut ("\r\ntaw crashed. Sorry!\r\n");
    writeOut (line);
    writeOut ("Report: ");
    writeOut (path);
    writeOut ("\r\nReopen the session to recover any take that was recording.\r\n");

    // 4. Last: the save. If the crash corrupted the session this may crash too, but everything
    // above is already done. (Sessions also save after every change, so this rarely matters.)
    if (emergencySave != nullptr)
    {
        emergencySave (emergencyContext);
        writeOut ("Session saved.\r\n");
    }
}

LONG WINAPI unhandledException (EXCEPTION_POINTERS* info)
{
    handleCrash (info, "unhandled exception");
    return EXCEPTION_EXECUTE_HANDLER; // end the process without Windows' own dialog
}

[[noreturn]] void fatal (const char* what)
{
    handleCrash (nullptr, what);
    TerminateProcess (GetCurrentProcess(), 3);
    for (;;) {}
}

BOOL WINAPI consoleControl (DWORD event)
{
    switch (event)
    {
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
        case CTRL_BREAK_EVENT:
            closing = true;
            // Hold the process open while the app stops recording and saves.
            WaitForSingleObject (shutdownDone, 4500);
            return TRUE;

        default:
            return FALSE;
    }
}

juce::File runningFolder() { return configFolder().getChildFile ("running"); }

juce::File markerFile (DWORD pid) { return runningFolder().getChildFile (juce::String ((juce::uint32) pid) + ".json"); }

bool isTawRunning (DWORD pid)
{
    const HANDLE p = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);

    if (p == nullptr)
        return false;

    DWORD exitCode = 0;
    wchar_t image[MAX_PATH] {};
    DWORD size = MAX_PATH;
    const bool alive = GetExitCodeProcess (p, &exitCode) && exitCode == STILL_ACTIVE
                       && QueryFullProcessImageNameW (p, 0, image, &size)
                       && juce::File (juce::String (image)).getFileNameWithoutExtension().equalsIgnoreCase ("taw");
    CloseHandle (p);
    return alive; // a reused pid belonging to something else doesn't count
}
} // namespace

juce::File crashFolder() { return configFolder().getChildFile ("crashes"); }

void installCrashHandler()
{
    const auto dir = crashFolder().getFullPathName();
    wcsncpy_s (crashDir, dir.toWideCharPointer(), _TRUNCATE);

    SetUnhandledExceptionFilter (unhandledException);
    std::set_terminate ([] { fatal ("uncaught C++ exception"); });
    _set_purecall_handler ([] { fatal ("pure virtual call"); });
    _set_invalid_parameter_handler ([] (const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) { fatal ("invalid parameter"); });
    _set_abort_behavior (0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}

void setEmergencySave (void (*save) (void*), void* context)
{
    emergencySave = nullptr;
    emergencyContext = context;
    emergencySave = save;
}

void rememberConsole (void* in, unsigned long inMode, void* out, unsigned long outMode)
{
    consoleIn = in;
    consoleOut = out;
    consoleInMode = inMode;
    consoleOutMode = outMode;
    haveConsole = true;
}

void installCloseHandler()
{
    shutdownDone = CreateEventW (nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler (consoleControl, TRUE);
}

bool closeRequested() { return closing.load(); }

void shutdownComplete()
{
    if (shutdownDone != nullptr)
        SetEvent (shutdownDone);
}

void startRunMarker()
{
    runMarker = true;
    markRunning ({});
}

void markRunning (const juce::File& sessionFolder)
{
    if (! runMarker)
        return;

    runningFolder().createDirectory();
    auto* o = new juce::DynamicObject();
    o->setProperty ("pid", (int) GetCurrentProcessId());
    o->setProperty ("started", juce::Time::getCurrentTime().toISO8601 (true));
    o->setProperty ("session", sessionFolder == juce::File() ? juce::String() : sessionFolder.getFullPathName());
    markerFile (GetCurrentProcessId()).replaceWithText (juce::JSON::toString (juce::var (o)));
}

void markStopped()
{
    if (runMarker)
        markerFile (GetCurrentProcessId()).deleteFile();
}

UncleanExit findUncleanExit()
{
    UncleanExit result;
    juce::Time newest;

    for (const auto& f : runningFolder().findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        const auto pid = (DWORD) f.getFileNameWithoutExtension().getLargeIntValue();

        if (pid == GetCurrentProcessId() || isTawRunning (pid))
            continue;

        const auto marker = juce::JSON::parse (f);
        const auto started = f.getCreationTime();

        if (! result.found || started > newest)
        {
            result.found = true;
            newest = started;
            const auto session = marker["session"].toString();
            result.session = session.isEmpty() ? juce::File() : juce::File (session);
        }

        f.deleteFile();
    }

    if (result.found)
        for (const auto& log : crashFolder().findChildFiles (juce::File::findFiles, false, "crash-*.txt"))
            if (log.getLastModificationTime() >= newest
                && (result.crashLog == juce::File() || log.getLastModificationTime() > result.crashLog.getLastModificationTime()))
                result.crashLog = log;

    return result;
}

bool repairWavHeader (const juce::File& file)
{
    juce::int64 dataSizeAt = 0, dataBytes = 0, riffSize = 0;

    {
        juce::FileInputStream in (file);

        if (in.failedToOpen())
            return false;

        const auto fileSize = in.getTotalLength();
        char riff[4], wave[4];

        if (in.read (riff, 4) != 4 || std::memcmp (riff, "RIFF", 4) != 0)
            return false;

        const auto oldRiffSize = (juce::int64) (juce::uint32) in.readInt();

        if (in.read (wave, 4) != 4 || std::memcmp (wave, "WAVE", 4) != 0)
            return false;

        int blockAlign = 0;

        // Walk the chunks to "data"; everything before it was written whole when recording began.
        for (;;)
        {
            if (in.getPosition() + 8 > fileSize)
                return false;

            char id[4];
            in.read (id, 4);
            const auto size = (juce::int64) (juce::uint32) in.readInt();
            const auto bodyAt = in.getPosition();

            if (std::memcmp (id, "fmt ", 4) == 0)
            {
                in.setPosition (bodyAt + 12);
                blockAlign = (juce::uint16) in.readShort();
            }

            if (std::memcmp (id, "data", 4) == 0)
            {
                if (blockAlign <= 0)
                    return false;

                dataSizeAt = bodyAt - 4;
                dataBytes = ((fileSize - bodyAt) / blockAlign) * blockAlign;
                riffSize = bodyAt + dataBytes - 8;

                if (size == dataBytes && oldRiffSize == riffSize)
                    return true; // already right
                break;
            }

            in.setPosition (bodyAt + size + (size & 1));
        }
    }

    juce::FileOutputStream out (file); // opens at the end without truncating

    if (out.failedToOpen())
        return false;

    out.setPosition (4);
    out.writeInt ((int) (juce::uint32) riffSize);
    out.setPosition (dataSizeAt);
    out.writeInt ((int) (juce::uint32) dataBytes);
    out.flush();
    return true;
}
} // namespace taw::safety
