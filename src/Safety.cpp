#include "Safety.h"
#include "Session.h"

#if JUCE_WINDOWS
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
 #include <dbghelp.h>
 #pragma comment(lib, "dbghelp.lib")
#else
 #include <csignal>
 #include <execinfo.h>
 #include <fcntl.h>
 #include <sys/stat.h>
 #include <termios.h>
 #include <sys/ucontext.h>
 #include <unistd.h>
 #if JUCE_MAC
  #include <libproc.h>
 #endif
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>

namespace taw::safety
{
namespace
{
std::atomic<bool> crashing { false };

void (*emergencySave) (void*) = nullptr;
void* emergencyContext = nullptr;

std::atomic<bool> closing { false };
bool runMarker = false;

#if JUCE_WINDOWS
// Everything the crash handler touches is set up in advance: after a crash the heap may be
// damaged, so the handler itself allocates nothing.
wchar_t crashDir[MAX_PATH] {};

HANDLE consoleIn = nullptr, consoleOut = nullptr;
DWORD consoleInMode = 0, consoleOutMode = 0;
bool haveConsole = false;

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

juce::uint32 currentPid() { return (juce::uint32) GetCurrentProcessId(); }

bool isTawRunning (juce::uint32 pid)
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
#else
// Everything the crash handler touches is set up in advance: after a crash the heap may be
// damaged, so the handler itself allocates nothing.
char crashDir[1024] {};

termios consoleMode {};
bool haveConsole = false;

void writeOut (const char* text)
{
    [[maybe_unused]] const auto n = ::write (STDOUT_FILENO, text, std::strlen (text));
}

const char* signalName (int sig)
{
    switch (sig)
    {
        case SIGSEGV: return "segmentation fault";
        case SIGBUS:  return "bus error";
        case SIGILL:  return "illegal instruction";
        case SIGFPE:  return "arithmetic error";
        case SIGABRT: return "abort";
        case SIGTRAP: return "trap";
        default:      return "signal";
    }
}

// The crashed thread's return addresses, walked from its registers at the moment of the crash.
// (backtrace() can't do this from the handler: it runs on its own stack, and macOS stops
// walking at the edge of the current one.)
int crashStack (void* context, void** frames, int max)
{
    int n = 0;

   #if JUCE_MAC && (defined(__aarch64__) || defined(__x86_64__))
    if (context == nullptr)
        return 0;

    const auto& state = static_cast<ucontext_t*> (context)->uc_mcontext->__ss;
    #if defined(__aarch64__)
    // System libraries sign return addresses (pointer authentication) in the top bits; user
    // addresses fit in 47 bits.
    auto strip = [] (uintptr_t a) { return a & (uintptr_t) 0x00007fffffffffff; };
    auto fp = (uintptr_t) __darwin_arm_thread_state64_get_fp (state);
    frames[n++] = (void*) strip ((uintptr_t) __darwin_arm_thread_state64_get_pc (state));
    frames[n++] = (void*) strip ((uintptr_t) __darwin_arm_thread_state64_get_lr (state)); // a leaf may not have saved it
    #else
    auto strip = [] (uintptr_t a) { return a; };
    auto fp = (uintptr_t) state.__rbp;
    frames[n++] = (void*) (uintptr_t) state.__rip;
    #endif

    // Each frame holds the caller's frame pointer, then the return address.
    while (n < max && fp != 0 && (fp & 0xf) == 0)
    {
        const auto* frame = reinterpret_cast<const uintptr_t*> (fp);
        const auto next = frame[0], ret = strip (frame[1]);

        if (ret == 0)
            break;

        if (ret != (uintptr_t) frames[n - 1])
            frames[n++] = (void*) ret;

        if (next <= fp)
            break; // stacks grow down: a caller's frame is always higher

        fp = next;
    }
   #else
    (void) context; (void) frames; (void) max;
   #endif

    return n;
}

// Writes the log (with a backtrace), restores the terminal, tells the user, then tries a last
// save. The system's own crash report (with every thread) follows when the signal is re-raised.
void handleCrash (const char* what, int sig, const void* address, void* context)
{
    if (crashing.exchange (true))
        return; // crashed again in here: give up quietly

    const auto now = std::time (nullptr);
    std::tm t {};
    localtime_r (&now, &t);
    char logPath[1200];
    std::snprintf (logPath, sizeof (logPath), "%s/crash-%04d%02d%02d-%02d%02d%02d.txt", crashDir, t.tm_year + 1900, t.tm_mon + 1,
                   t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);

    mkdir (crashDir, 0755);

    // 1. A readable line about what happened, then the crashing thread's stack.
    char line[512];
    std::snprintf (line, sizeof (line), "taw %s crashed: %s (signal %d at %p)\n", TAW_VERSION, what, sig, address);

    const auto log = ::open (logPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (log >= 0)
    {
        [[maybe_unused]] auto n = ::write (log, line, std::strlen (line));
        void* frames[64];
        auto depth = crashStack (context, frames, 64);

        if (depth == 0)
            depth = backtrace (frames, 64);

        backtrace_symbols_fd (frames, depth, log);
        ::close (log);
    }

    // 2. Leave the alternate screen and give the terminal back.
    if (haveConsole)
    {
        writeOut ("\x1b[?1006l\x1b[?1000l\x1b[<u\x1b[0m\x1b[?25h\x1b[?1049l");
        tcsetattr (STDIN_FILENO, TCSANOW, &consoleMode);
    }

    writeOut ("\ntaw crashed. Sorry!\n");
    writeOut (line);
    writeOut ("Report: ");
    writeOut (logPath);
    writeOut ("\nReopen the session to recover any take that was recording.\n");

    // 3. Last: the save. If the crash corrupted the session this may crash too, but everything
    // above is already done. (Sessions also save after every change, so this rarely matters.)
    if (emergencySave != nullptr)
    {
        emergencySave (emergencyContext);
        writeOut ("Session saved.\n");
    }
}

void crashSignal (int sig, siginfo_t* info, void* context)
{
    handleCrash (signalName (sig), sig, info != nullptr ? info->si_addr : nullptr, context);
    signal (sig, SIG_DFL);
    raise (sig); // on to the system's crash report
}

[[noreturn]] void fatal (const char* what)
{
    handleCrash (what, SIGABRT, nullptr, nullptr);
    std::abort();
}

// Closing the terminal window (SIGHUP) or being asked to stop: quit like q does.
void closeSignal (int) { closing = true; }

juce::uint32 currentPid() { return (juce::uint32) getpid(); }

bool isTawRunning (juce::uint32 pid)
{
    if (::kill ((pid_t) pid, 0) != 0 && errno != EPERM)
        return false;

    // A reused pid belonging to something else doesn't count.
    char image[4096] {};
   #if JUCE_MAC
    if (proc_pidpath ((int) pid, image, sizeof (image)) <= 0)
        return false;
   #else
    if (readlink (("/proc/" + juce::String (pid) + "/exe").toRawUTF8(), image, sizeof (image) - 1) <= 0)
        return false;
   #endif
    return juce::File (juce::String::fromUTF8 (image)).getFileNameWithoutExtension().equalsIgnoreCase ("taw");
}
#endif

juce::File runningFolder() { return configFolder().getChildFile ("running"); }

juce::File markerFile (juce::uint32 pid) { return runningFolder().getChildFile (juce::String (pid) + ".json"); }
} // namespace

juce::uint32 thisProcess() { return currentPid(); }
bool tawRunning (juce::uint32 pid) { return isTawRunning (pid); }

juce::File crashFolder() { return configFolder().getChildFile ("crashes"); }

#if JUCE_WINDOWS
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

void shutdownComplete()
{
    if (shutdownDone != nullptr)
        SetEvent (shutdownDone);
}
#else
void installCrashHandler()
{
    std::strncpy (crashDir, crashFolder().getFullPathName().toRawUTF8(), sizeof (crashDir) - 1);

    // Its own stack, so a stack overflow can still be reported.
    static char altStack[64 * 1024];
    stack_t ss {};
    ss.ss_sp = altStack;
    ss.ss_size = sizeof (altStack);
    sigaltstack (&ss, nullptr);

    struct sigaction sa {};
    sa.sa_sigaction = crashSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset (&sa.sa_mask);

    for (auto sig : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP })
        sigaction (sig, &sa, nullptr);

    std::set_terminate ([] { fatal ("uncaught C++ exception"); });

    // A pipe whose reader died (ffmpeg, pbcopy) is an error to handle, not a reason to exit.
    signal (SIGPIPE, SIG_IGN);
}

void rememberConsole (const termios& mode)
{
    consoleMode = mode;
    haveConsole = true;
}

void installCloseHandler()
{
    struct sigaction sa {};
    sa.sa_handler = closeSignal;
    sigemptyset (&sa.sa_mask);

    for (auto sig : { SIGHUP, SIGTERM, SIGINT })
        sigaction (sig, &sa, nullptr);
}

void shutdownComplete() {} // nothing waits: the signal handler returned straight away
#endif

void setEmergencySave (void (*save) (void*), void* context)
{
    emergencySave = nullptr;
    emergencyContext = context;
    emergencySave = save;
}

bool closeRequested() { return closing.load(); }

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
    o->setProperty ("pid", (int) currentPid());
    o->setProperty ("started", juce::Time::getCurrentTime().toISO8601 (true));
    o->setProperty ("session", sessionFolder == juce::File() ? juce::String() : sessionFolder.getFullPathName());
    markerFile (currentPid()).replaceWithText (juce::JSON::toString (juce::var (o)));
}

void markStopped()
{
    if (runMarker)
        markerFile (currentPid()).deleteFile();
}

UncleanExit findUncleanExit()
{
    UncleanExit result;
    juce::Time newest;

    for (const auto& f : runningFolder().findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        const auto pid = (juce::uint32) f.getFileNameWithoutExtension().getLargeIntValue();

        if (pid == currentPid() || isTawRunning (pid))
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
