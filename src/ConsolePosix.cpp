#include "Console.h"
#include "Safety.h"
#include "Session.h"

#if defined(__APPLE__)
#include "MacFocus.h"

#include <CoreGraphics/CoreGraphics.h>
#endif

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace taw
{
namespace
{
termios oldMode {};

// Keyboard enhancement (kitty protocol, "disambiguate"): terminals that support it (iTerm2,
// Ghostty, kitty, WezTerm) report ctrl+[ apart from Escape and ctrl+m apart from Enter.
// Terminal.app ignores it and keeps sending the classic sequences, which are decoded too.
// Mouse buttons are reported (1000), and moves while one is held (1002), in SGR form (1006),
// so clicks and drags come in as cells.
constexpr const char* enter = "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[>1u\x1b[?1000h\x1b[?1002h\x1b[?1006h";
constexpr const char* leave = "\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[<u\x1b[0m\x1b[?25h\x1b[?1049l";

// The terminal: stdout, until plugins' stdout goes to the log too (Console::open); then a
// copy of it only taw writes the screen to.
int screenOut = STDOUT_FILENO;

void writeAll (const char* data, size_t size)
{
    while (size > 0)
    {
        const auto n = ::write (screenOut, data, size);

        if (n <= 0)
            return; // the terminal went away (window closed)

        data += n;
        size -= (size_t) n;
    }
}

// xterm modifier parameter: 1 + shift(1) + alt(2) + ctrl(4) + super(8). Cmd (super), where a
// terminal passes it on at all, counts as alt: it sits where alt does on a Windows keyboard.
void applyModifiers (Key& k, int param)
{
    const auto m = param > 0 ? param - 1 : 0;
    k.shift = (m & 1) != 0;
    k.alt = (m & (2 | 8)) != 0;
    k.ctrl = (m & 4) != 0;
}

// Classic single-byte keys: control codes are ctrl+letter, DEL is backspace.
Key byteKey (unsigned char c)
{
    Key k;

    if (c == '\r' || c == '\n')     k.code = Key::Enter;
    else if (c == '\t')             k.code = Key::Tab;
    else if (c == 0x7f || c == 0x08) k.code = Key::Backspace;
    else if (c == 0)                { k.code = Key::Char; k.ch = L' '; k.ctrl = true; }  // ctrl+space
    else if (c <= 0x1a)             { k.code = Key::Char; k.ch = (wchar_t) ('a' + c - 1); k.ctrl = true; }
    else if (c == 0x1d)             { k.code = Key::Char; k.ch = L']'; k.ctrl = true; }
    else if (c == 0x1c)             { k.code = Key::Char; k.ch = L'\\'; k.ctrl = true; } // ctrl+backslash
    else if (c >= 0x20)             { k.code = Key::Char; k.ch = (wchar_t) c; }

    return k;
}

// A key reported by its Unicode code (kitty's CSI u, xterm's modifyOtherKeys).
Key codeKey (int code)
{
    Key k;

    switch (code)
    {
        case 27:  k.code = Key::Escape; break;
        case 13:  k.code = Key::Enter; break;
        case 9:   k.code = Key::Tab; break;
        case 127: case 8: k.code = Key::Backspace; break;
        default:
            if (code >= 32 && code < 0xe000) // the private-use range holds kitty's function keys
            {
                k.code = Key::Char;
                k.ch = (wchar_t) code;
            }
            break;
    }

    return k;
}

// CSI parameters: "1;5" -> {1, 5}. Kitty sub-parameters ("97:65") keep their first part.
std::vector<int> csiParams (const std::string& s)
{
    std::vector<int> out { 0 };
    bool sub = false;

    for (auto c : s)
    {
        if (c == ';')       { out.push_back (0); sub = false; }
        else if (c == ':')  sub = true;
        else if (c >= '0' && c <= '9' && ! sub) out.back() = out.back() * 10 + (c - '0');
    }

    return out;
}

Key csiKey (const std::string& params, char final)
{
    const auto p = csiParams (params);
    const auto mods = p.size() > 1 ? p[1] : 1;
    Key k;

    switch (final)
    {
        case 'A': k.code = Key::Up; break;
        case 'B': k.code = Key::Down; break;
        case 'C': k.code = Key::Right; break;
        case 'D': k.code = Key::Left; break;
        case 'H': k.code = Key::Home; break;
        case 'F': k.code = Key::End; break;
        case 'P': k.code = Key::F1; break;
        case 'Z': k.code = Key::Tab; k.shift = true; return k;

        case '~':
            switch (p[0])
            {
                case 1: case 7:  k.code = Key::Home; break;
                case 4: case 8:  k.code = Key::End; break;
                case 3:          k.code = Key::Delete; break;
                case 5:          k.code = Key::PageUp; break;
                case 6:          k.code = Key::PageDown; break;
                case 11:         k.code = Key::F1; break;
                case 27:         if (p.size() > 2) k = codeKey (p[2]); break; // xterm modifyOtherKeys: 27;mods;code~
                default: break;
            }
            break;

        case 'u': k = codeKey (p[0]); break; // kitty: code;mods u

        default: break;
    }

    if (k.code != Key::None)
        applyModifiers (k, mods);

    return k;
}

// The last left or right press, to spot the second click of a double-click: terminals only
// report presses, so it's the same button in the same cell soon after.
struct LastClick { double at = -1.0e9; int x = -1, y = -1; bool right = false; } lastClick;

// The cell a press or drag last reported: a drag is reported once per cell it reaches.
int dragX = -1, dragY = -1;

// An SGR mouse report: "<b;x;y" then M (pressed) or m (released). b is the button (0 left,
// 1 middle, 2 right) plus 4 shift, 8 alt, 16 ctrl, 32 moved, 64 wheel; x and y count from 1.
// Left and right presses, and moves with the left button held (drags), become keys, as the
// Windows console reports them.
Key mouseKey (const std::string& params, char final)
{
    const auto p = csiParams (params.substr (1));
    Key k;

    if (final != 'M' || p.size() < 3 || (p[0] & 64) != 0 || (p[0] & 3) == 1 || (p[0] & 3) == 3)
        return k;

    const auto x = std::max (0, p[1] - 1), y = std::max (0, p[2] - 1);

    if ((p[0] & 32) != 0) // moved
    {
        if ((p[0] & 3) != 0 || (x == dragX && y == dragY))
            return k;

        k.code = Key::Mouse;
        k.drag = true;
        k.shift = (p[0] & 4) != 0;
        k.x = dragX = x;
        k.y = dragY = y;
        return k;
    }

    k.code = Key::Mouse;
    k.right = (p[0] & 3) == 2;
    k.shift = (p[0] & 4) != 0;
    k.alt = (p[0] & 8) != 0;
    k.ctrl = (p[0] & 16) != 0;
    k.x = dragX = x;
    k.y = dragY = y;

    const auto now = juce::Time::getMillisecondCounterHiRes();
    k.doubleClick = now - lastClick.at < 500.0 && k.x == lastClick.x && k.y == lastClick.y && k.right == lastClick.right;
    lastClick = k.doubleClick ? LastClick {} : LastClick { now, k.x, k.y, k.right }; // a third click starts over
    return k;
}

// Bytes a UTF-8 sequence starting with c needs (1 for anything unexpected).
int utf8Length (unsigned char c)
{
    if (c >= 0xf0) return 4;
    if (c >= 0xe0) return 3;
    if (c >= 0xc0) return 2;
    return 1;
}

// Decodes one key at s[i]. Returns the bytes used, or 0 if the sequence hasn't fully arrived.
size_t decodeOne (const std::string& s, size_t i, Key& k)
{
    const auto c = (unsigned char) s[i];
    const auto left = s.size() - i;

    if (c != 0x1b)
    {
        if (c < 0x80)
        {
            k = byteKey (c);
            return 1;
        }

        const auto len = (size_t) utf8Length (c);

        if (left < len)
            return 0;

        const auto text = juce::String::fromUTF8 (s.data() + i, (int) len);
        k.code = Key::Char;
        k.ch = (wchar_t) text[0];
        return len;
    }

    if (left < 2)
        return 0;

    const auto next = s[i + 1];

    if (next == '[')
    {
        // CSI: parameters, intermediates, then a final byte in 0x40-0x7e.
        auto j = i + 2;

        while (j < s.size() && (unsigned char) s[j] >= 0x20 && (unsigned char) s[j] <= 0x3f)
            ++j;

        if (j >= s.size())
            return 0;

        const auto params = s.substr (i + 2, j - i - 2);
        k = params.starts_with ('<') ? mouseKey (params, s[j]) : csiKey (params, s[j]);
        return j - i + 1;
    }

    if (next == 'O')
    {
        if (left < 3)
            return 0;

        k = csiKey ({}, s[i + 2]); // SS3: F1, arrows, Home/End
        return 3;
    }

    // Terminal.app sends option+left/right as esc b / esc f (word left/right).
    if (next == 'b' || next == 'f')
    {
        k.code = next == 'b' ? Key::Left : Key::Right;
        k.alt = true;
        return 2;
    }

    // Esc first means alt (option as meta): alt plus whatever key follows.
    const auto used = decodeOne (s, i + 1, k);

    if (used == 0)
        return 0;

    k.alt = true;
    return used + 1;
}
} // namespace

std::vector<Key> Console::decode (std::string& bytes, bool flush)
{
    std::vector<Key> keys;
    size_t i = 0;

    while (i < bytes.size())
    {
        Key k;
        auto used = decodeOne (bytes, i, k);

        if (used == 0)
        {
            if (! flush)
                break; // the rest arrives on a later poll

            // Nothing more came: a lone esc is the Escape key; anything else is dropped.
            if (bytes[i] == 0x1b)
                k = Key { Key::Escape };

            used = bytes[i] == 0x1b ? 1 : bytes.size() - i;
        }

        if (k.code != Key::None)
            keys.push_back (k);

        i += used;
    }

    bytes.erase (0, i);
    return keys;
}

bool Console::open()
{
    if (opened)
        return true;

    if (! isatty (STDIN_FILENO) || ! isatty (STDOUT_FILENO) || tcgetattr (STDIN_FILENO, &oldMode) != 0)
        return false;

    safety::rememberConsole (oldMode);

    // No line buffering, no echo, and ctrl+c / ctrl+z / ctrl+y arrive as keys instead of
    // signals. Reads never wait.
    auto raw = oldMode;
    raw.c_iflag &= ~(tcflag_t) (BRKINT | ICRNL | INLCR | IGNCR | ISTRIP | IXON | IXOFF);
    raw.c_oflag &= ~(tcflag_t) OPOST;
    raw.c_lflag &= ~(tcflag_t) (ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr (STDIN_FILENO, TCSAFLUSH, &raw) != 0)
        return false;

    opened = true;
    edgeGlyphs = true; // Menlo, SF Mono and the usual terminal fonts have them

    // Whatever lands on stderr (a plugin's chatter, the Objective-C runtime's warnings about
    // classes loaded twice) would be written over the screen: it goes to taw-stderr.log in the
    // config folder until taw leaves, kept to the last run or so (over 1 MB, it starts over).
    // Some plugins print to stdout instead (Waves' WaveShell: "eInit_RM ... Compiled for 64
    // bits"): that goes there too, and taw draws on a copy of the terminal of its own.
    if (isatty (STDERR_FILENO))
    {
        const auto log = configFolder().getChildFile ("taw-stderr.log");
        log.getParentDirectory().createDirectory();
        const int flags = O_WRONLY | O_CREAT | O_APPEND | (log.getSize() > 1024 * 1024 ? O_TRUNC : 0);

        if (const int fd = ::open (log.getFullPathName().toRawUTF8(), flags, 0644); fd >= 0)
        {
            std::fflush (stderr);
            savedStderr = dup (STDERR_FILENO);

            if (savedStderr >= 0 && dup2 (fd, STDERR_FILENO) < 0)
            {
                close (savedStderr);
                savedStderr = -1;
            }

            std::fflush (stdout);

            if (const int own = dup (STDOUT_FILENO); own >= 0)
            {
                if (dup2 (fd, STDOUT_FILENO) >= 0)
                    screenOut = own;
                else
                    close (own);
            }

            close (fd);
        }
    }

    writeAll (enter, std::strlen (enter));
    return true;
}

Console::~Console()
{
    if (! opened)
        return;

    writeAll (leave, std::strlen (leave));
    tcsetattr (STDIN_FILENO, TCSAFLUSH, &oldMode);

    if (savedStderr >= 0)
    {
        std::fflush (stderr);
        dup2 (savedStderr, STDERR_FILENO);
        close (savedStderr);
    }

    if (screenOut != STDOUT_FILENO)
    {
        std::fflush (stdout);
        dup2 (screenOut, STDOUT_FILENO);
        close (screenOut);
        screenOut = STDOUT_FILENO;
    }
}

// A Mac, as on Windows: a click in a plugin window brings taw to the front, and once the mouse is
// done the keyboard goes back to the terminal, unless the window's title was double-clicked
// (then it keeps it until it closes or the terminal comes to the front).
void Console::reclaimKeyboard ([[maybe_unused]] const std::vector<void*>& pluginWindows, [[maybe_unused]] void* keeper)
{
#if defined(__APPLE__)
    if (! opened)
        return;

    if (held_by != nullptr)
    {
        readySince = 0.0;

        if (std::find (pluginWindows.begin(), pluginWindows.end(), held_by) == pluginWindows.end())
        {
            held_by = nullptr;
            macFocus::log ("keyboard back to taw (window closed)");
            macFocus::activateTerminal();
            return;
        }

        // Coming to the front takes a moment: the terminal in front only counts once taw has been.
        if (! heldReachedTaw && macFocus::tawInFront())
        {
            heldReachedTaw = true;
            macFocus::log ("taw in front: the plugin window has the keyboard");
        }

        if (heldReachedTaw ? macFocus::terminalInFront() : juce::Time::getMillisecondCounterHiRes() - heldSince > 2000.0)
        {
            macFocus::log (heldReachedTaw ? "keyboard back to taw (the terminal clicked)" : "taw never came to the front: hold dropped");
            held_by = nullptr;
        }

        return;
    }

    const auto front = macFocus::front (pluginWindows, keeper);

    if (front != macFocus::Front::plugin)
    {
        if (front != macFocus::Front::notTaw)
            macFocus::log (front == macFocus::Front::keeper ? "taw's editor window has the keyboard"
                           : front == macFocus::Front::other ? "a plugin's own window has the keyboard: left alone"
                           : front == macFocus::Front::menu  ? "a menu is open"
                                                             : "mouse button down");
        readySince = 0.0;
        return;
    }

    // A moment after the click, so a double-click or a menu that opens on release still works.
    const auto now = juce::Time::getMillisecondCounterHiRes();

    if (readySince == 0.0)
    {
        readySince = now;
        return;
    }

    if (now - readySince < 250.0)
        return;

    readySince = 0.0;
    macFocus::activateTerminal();
    macFocus::log ("plugin window clicked: keyboard back to the terminal");
#endif
}

void Console::focusTerminal()
{
    held_by = nullptr;

#if defined(__APPLE__)
    if (opened)
        macFocus::activateTerminal();
#endif
}

void* Console::keyboardHeldBy() const { return held_by; }

// A plugin window's title bar double-clicked: it keeps the keyboard (again: taw has it back).
void Console::toggleKeyboardHold ([[maybe_unused]] void* window)
{
#if defined(__APPLE__)
    if (! opened || window == nullptr)
        return;

    if (held_by == window)
    {
        macFocus::log ("title double-clicked: keyboard back to taw");
        focusTerminal();
        return;
    }

    held_by = window;
    heldReachedTaw = false;
    heldSince = juce::Time::getMillisecondCounterHiRes();
    macFocus::log ("title double-clicked: the plugin window keeps the keyboard");
    macFocus::holdKeyboard (window);
#endif
}

std::vector<Key> Console::poll()
{
    char buffer[4096];
    bool gotAny = false;

    for (;;)
    {
        const auto n = ::read (STDIN_FILENO, buffer, sizeof (buffer));

        if (n <= 0)
            break;

        pending.append (buffer, (size_t) n);
        gotAny = true;
    }

    // A key sequence split across reads waits a couple of polls (~30 ms) for the rest.
    pendingPolls = gotAny ? 0 : pendingPolls + 1;
    auto keys = decode (pending, pendingPolls >= 2);

#if defined(__APPLE__)
    // Terminal.app sends shift+return as a plain return; whether shift is down is read from the
    // keyboard instead (shift+enter: a new sample track from the browser).
    // Arrows with option are only told apart if the Terminal profile maps them (and a profile
    // made by an older taw may not): whether option is down is read from the keyboard as well.
    for (auto& k : keys)
    {
        const bool arrow = k.code == Key::Up || k.code == Key::Down || k.code == Key::Left || k.code == Key::Right;

        if (k.code == Key::Enter && ! k.shift)
            k.shift = (CGEventSourceFlagsState (kCGEventSourceStateHIDSystemState) & kCGEventFlagMaskShift) != 0;
        else if (arrow && ! k.alt)
            k.alt = (CGEventSourceFlagsState (kCGEventSourceStateHIDSystemState) & kCGEventFlagMaskAlternate) != 0;
    }
#endif

    return keys;
}

int Console::keyDown (wchar_t ch) const
{
#if defined(__APPLE__)
    // macOS key codes (kVK_ANSI_*) are places on the keyboard, named for a US layout.
    static const std::pair<wchar_t, CGKeyCode> codes[] = {
        { 'a', 0x00 }, { 's', 0x01 }, { 'd', 0x02 }, { 'f', 0x03 }, { 'h', 0x04 }, { 'g', 0x05 }, { 'z', 0x06 },
        { 'x', 0x07 }, { 'c', 0x08 }, { 'v', 0x09 }, { 'b', 0x0b }, { 'q', 0x0c }, { 'w', 0x0d }, { 'e', 0x0e },
        { 'r', 0x0f }, { 'y', 0x10 }, { 't', 0x11 }, { 'o', 0x1f }, { 'u', 0x20 }, { 'i', 0x22 }, { 'p', 0x23 },
        { 'l', 0x25 }, { 'j', 0x26 }, { '\'', 0x27 }, { 'k', 0x28 }, { ';', 0x29 }, { 'n', 0x2d }, { 'm', 0x2e },
    };

    for (const auto& [c, code] : codes)
        if (c == ch)
            return CGEventSourceKeyState (kCGEventSourceStateHIDSystemState, code) ? 1 : 0;
#else
    (void) ch;
#endif
    return -1;
}

void Console::setClipboardText (const char* text)
{
#if defined(__APPLE__)
    if (auto* pipe = popen ("pbcopy", "w"))
    {
        std::fputs (text, pipe);
        pclose (pipe);
    }
#else
    (void) text;
#endif
}

int Console::width() const
{
    winsize ws {};
    return ioctl (screenOut, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 ? ws.ws_col : 80;
}

int Console::height() const
{
    winsize ws {};
    return ioctl (screenOut, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 ? ws.ws_row : 24;
}

void Console::write (const std::wstring& s)
{
    std::string utf8;
    utf8.reserve (s.size() * 2);

    for (const auto w : s)
    {
        const auto c = (uint32_t) w;

        if (c < 0x80)
            utf8 += (char) c;
        else if (c < 0x800)
        {
            utf8 += (char) (0xc0 | (c >> 6));
            utf8 += (char) (0x80 | (c & 0x3f));
        }
        else if (c < 0x10000)
        {
            utf8 += (char) (0xe0 | (c >> 12));
            utf8 += (char) (0x80 | ((c >> 6) & 0x3f));
            utf8 += (char) (0x80 | (c & 0x3f));
        }
        else
        {
            utf8 += (char) (0xf0 | (c >> 18));
            utf8 += (char) (0x80 | ((c >> 12) & 0x3f));
            utf8 += (char) (0x80 | ((c >> 6) & 0x3f));
            utf8 += (char) (0x80 | (c & 0x3f));
        }
    }

    writeAll (utf8.data(), utf8.size());
}
} // namespace taw
