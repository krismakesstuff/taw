#include "Console.h"
#include "Safety.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace taw
{
namespace
{
// The cmd window's font before taw switched it, put back on exit.
CONSOLE_FONT_INFOEX oldFont {};
bool fontChanged = false;

// Windows Terminal draws with its own font (Cascadia Mono by default) and ignores console font
// calls. A classic console window (conhost) gets switched to Cascadia Mono when installed.
bool useEdgeFont (HANDLE out)
{
    if (GetEnvironmentVariableW (L"WT_SESSION", nullptr, 0) > 0)
        return true;

    oldFont.cbSize = sizeof (oldFont);

    if (! GetCurrentConsoleFontEx (out, FALSE, &oldFont))
        return false;

    if (std::wcscmp (oldFont.FaceName, L"Cascadia Mono") == 0 || std::wcscmp (oldFont.FaceName, L"Cascadia Code") == 0)
        return true;

    auto font = oldFont;
    font.dwFontSize.X = 0; // width follows the height
    font.FontFamily = FF_MODERN | TMPF_TRUETYPE | TMPF_VECTOR;
    font.FontWeight = FW_NORMAL;
    wcscpy_s (font.FaceName, L"Cascadia Mono");

    if (! SetCurrentConsoleFontEx (out, FALSE, &font))
        return false;

    fontChanged = true;
    CONSOLE_FONT_INFOEX now { sizeof (now) };
    // Windows quietly substitutes a font that isn't installed.
    return GetCurrentConsoleFontEx (out, FALSE, &now) && std::wcscmp (now.FaceName, L"Cascadia Mono") == 0;
}
} // namespace

// TAW_FOCUS_LOG=1: what's in front and why the keyboard was or wasn't taken back, to
// %TEMP%\taw-focus.log, a line each time that changes.
static void logFocus (const std::string& line)
{
    static const bool on = GetEnvironmentVariableA ("TAW_FOCUS_LOG", nullptr, 0) > 0;
    static std::string last;

    if (! on || line == last)
        return;

    last = line;
    char path[MAX_PATH] {};
    GetTempPathA (MAX_PATH, path);

    FILE* f = nullptr;

    if (fopen_s (&f, (std::string (path) + "taw-focus.log").c_str(), "a") == 0 && f != nullptr)
    {
        std::fprintf (f, "%llu %s\n", (unsigned long long) GetTickCount64(), line.c_str());
        std::fclose (f);
    }
}

static std::string describeWindow (HWND w)
{
    char cls[128] {}, title[128] {};
    GetClassNameA (w, cls, sizeof (cls));
    GetWindowTextA (w, title, sizeof (title));
    DWORD pid = 0;
    GetWindowThreadProcessId (w, &pid);
    char s[400];
    std::snprintf (s, sizeof (s), "%p [%s] \"%s\" pid %lu%s", (void*) w, cls, title, pid, pid == GetCurrentProcessId() ? " (taw)" : "");
    return s;
}

bool Console::open()
{
    if (opened)
        return true;

    in = GetStdHandle (STD_INPUT_HANDLE);
    out = GetStdHandle (STD_OUTPUT_HANDLE);

    if (! GetConsoleMode (in, &oldInMode) || ! GetConsoleMode (out, &oldOutMode))
        return false;

    safety::rememberConsole (in, oldInMode, out, oldOutMode);

    // No line buffering, no echo, Ctrl+C arrives as a key, and QuickEdit is off so a stray
    // click can't freeze the screen. Clicks arrive as events (cells), for the mouse.
    SetConsoleMode (in, ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS | ENABLE_MOUSE_INPUT);

    if (! SetConsoleMode (out, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN))
    {
        SetConsoleMode (in, oldInMode);
        return false;
    }

    opened = true;

    // The window keys arrive through: conhost's own, or (Windows Terminal, whose console
    // window is hidden) whatever's in front as taw starts, which is the terminal taw was run from.
    terminal = GetConsoleWindow();

    if (terminal == nullptr || ! IsWindowVisible ((HWND) terminal))
        terminal = GetForegroundWindow();

    logFocus ("terminal " + describeWindow ((HWND) terminal));

    edgeGlyphs = useEdgeFont (out);
    write (L"\x1b[?1049h\x1b[?25l\x1b[2J");
    return true;
}

Console::~Console()
{
    if (! opened)
        return;

    write (L"\x1b[0m\x1b[?25h\x1b[?1049l");
    SetConsoleMode (in, oldInMode);
    SetConsoleMode (out, oldOutMode);

    if (fontChanged)
        SetCurrentConsoleFontEx (out, FALSE, &oldFont);
}

void* Console::keyboardHeldBy() const { return held_by; }

// A plugin window's title bar double-clicked: it keeps the keyboard (again: taw has it back).
void Console::toggleKeyboardHold (void* window)
{
    if (! opened || window == nullptr)
        return;

    if (held_by == window)
    {
        held_by = nullptr;
        SetForegroundWindow ((HWND) terminal);
        logFocus ("title double-clicked: keyboard back to taw");
    }
    else
    {
        held_by = window;
        SetForegroundWindow ((HWND) window);
        logFocus ("title double-clicked: keyboard stays with " + describeWindow ((HWND) window));
    }
}

void Console::focusTerminal()
{
    if (! opened || terminal == nullptr)
        return;

    held_by = nullptr;

    // Windows only lets the app in front move the focus, and sometimes not even then; tapping
    // Alt (nothing reads it) lifts that lock.
    if (! SetForegroundWindow ((HWND) terminal))
    {
        INPUT alt[2] {};
        alt[0].type = alt[1].type = INPUT_KEYBOARD;
        alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
        alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput (2, alt, sizeof (INPUT));
        SetForegroundWindow ((HWND) terminal);
    }
}

void Console::reclaimKeyboard (const std::vector<void*>& pluginWindows, void* keeper)
{
    const auto front = GetForegroundWindow();

    // With the terminal in front, plugin windows stay over it. JUCE drops a window's
    // always-on-top when its app goes to the background, which is what handing the keyboard
    // back looks like; put it back, without taking the keyboard. (In another app they're let go.)
    if (front != nullptr && front == (HWND) terminal)
    {
        auto onTop = pluginWindows;

        if (keeper != nullptr)
            onTop.push_back (keeper);

        for (auto* w : onTop)
            if ((GetWindowLongPtrW ((HWND) w, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0)
            {
                SetWindowPos ((HWND) w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                logFocus ("put " + describeWindow ((HWND) w) + " back on top");
            }
    }

    // The MIDI editor keeps the keyboard while it's in front (esc in it hands it back).
    if (keeper != nullptr && front == (HWND) keeper)
    {
        readySince = 0.0;
        return;
    }

    // Held by a plugin until its window closes or the terminal is clicked.
    if (held_by != nullptr)
    {
        if (std::find (pluginWindows.begin(), pluginWindows.end(), held_by) == pluginWindows.end() || front == (HWND) terminal)
        {
            held_by = nullptr;
            logFocus ("keyboard back to taw (window closed, or the terminal clicked)");
        }
        else
        {
            readySince = 0.0;
            return;
        }
    }

    if (! opened || terminal == nullptr || front == nullptr || front == (HWND) terminal)
    {
        readySince = 0.0;
        return;
    }

    // One of taw's windows: a plugin's, or one a plugin made (Waves' shell puts the keyboard on
    // windows of its own). Not a dialog box, so a preset can still be named; and not anything
    // outside taw (you went to another app).
    DWORD pid = 0;
    GetWindowThreadProcessId (front, &pid);
    char cls[64] {};
    GetClassNameA (front, cls, sizeof (cls));
    const bool plugin = std::find (pluginWindows.begin(), pluginWindows.end(), (void*) front) != pluginWindows.end();
    const bool dialog = std::strcmp (cls, "#32770") == 0;

    if (pid != GetCurrentProcessId() || dialog || (pluginWindows.empty() && ! plugin))
    {
        logFocus ("front " + describeWindow (front) + (dialog ? ": a dialog, it keeps the keyboard" : ": not taw's, left alone"));
        readySince = 0.0;
        return;
    }

    // Not while the mouse is busy in it: a button held (dragging a knob), something captured,
    // a menu open, the window being moved.
    GUITHREADINFO gui {};
    gui.cbSize = sizeof (gui);
    const bool button = (GetAsyncKeyState (VK_LBUTTON) & 0x8000) != 0 || (GetAsyncKeyState (VK_RBUTTON) & 0x8000) != 0
                        || (GetAsyncKeyState (VK_MBUTTON) & 0x8000) != 0;
    const bool gotInfo = GetGUIThreadInfo (GetWindowThreadProcessId (front, nullptr), &gui) != 0;
    const bool menu = gotInfo && (gui.hwndMenuOwner != nullptr || (gui.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE)) != 0);
    const bool moving = gotInfo && (gui.flags & GUI_INMOVESIZE) != 0;
    const auto now = (double) GetTickCount64();

    // (Capture alone doesn't count: some plugins hold it all the time.)
    if (button || menu || moving)
    {
        logFocus ("front " + describeWindow (front) + (button ? ": mouse button down" : menu ? ": menu open" : ": being moved"));
        readySince = 0.0;
        return;
    }

    // A moment after the click, so a double-click or a menu that opens on release still works.
    if (readySince == 0.0)
    {
        readySince = now;
        return;
    }

    if (now - readySince < 250.0)
        return;

    readySince = 0.0;

    // Windows only lets the app in front move the focus, and sometimes not even then; tapping
    // Alt (nothing reads it) lifts that lock.
    if (! SetForegroundWindow ((HWND) terminal))
    {
        INPUT alt[2] {};
        alt[0].type = alt[1].type = INPUT_KEYBOARD;
        alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
        alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput (2, alt, sizeof (INPUT));
        SetForegroundWindow ((HWND) terminal);
    }

    logFocus ("front " + describeWindow (front) + " -> gave the keyboard to the terminal " + describeWindow ((HWND) terminal)
              + (GetForegroundWindow() == (HWND) terminal ? ": ok" : ": FAILED, still " + describeWindow (GetForegroundWindow())));
}

std::vector<Key> Console::poll()
{
    std::vector<Key> keys;
    DWORD available = 0;

    if (! GetNumberOfConsoleInputEvents (in, &available) || available == 0)
        return keys;

    std::vector<INPUT_RECORD> records (available);
    DWORD read = 0;

    if (! ReadConsoleInputW (in, records.data(), available, &read))
        return keys;

    for (DWORD i = 0; i < read; ++i)
    {
        // Keys released while the window was in the background never report it.
        if (records[i].EventType == FOCUS_EVENT)
            held.clear();

        // A left or right button going down (not moves, not releases, not the wheel). The second
        // click of a double-click comes as its own event, after the first one's press.
        if (records[i].EventType == MOUSE_EVENT)
        {
            const auto& m = records[i].Event.MouseEvent;
            const auto buttons = m.dwButtonState & (FROM_LEFT_1ST_BUTTON_PRESSED | RIGHTMOST_BUTTON_PRESSED);
            const auto pressed = buttons & ~buttonsDown;
            const bool twice = (m.dwEventFlags & DOUBLE_CLICK) != 0;

            if (m.dwEventFlags != MOUSE_MOVED && m.dwEventFlags != MOUSE_WHEELED && m.dwEventFlags != MOUSE_HWHEELED)
                buttonsDown = buttons;

            if ((m.dwEventFlags == 0 && pressed != 0) || (twice && buttons != 0))
            {
                Key k;
                k.code = Key::Mouse;
                k.x = m.dwMousePosition.X;
                k.y = m.dwMousePosition.Y;
                k.right = ((twice ? buttons : pressed) & RIGHTMOST_BUTTON_PRESSED) != 0;
                k.doubleClick = twice;
                k.shift = (m.dwControlKeyState & SHIFT_PRESSED) != 0;
                k.ctrl = (m.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
                k.alt = (m.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
                keys.push_back (k);
            }

            continue;
        }

        if (records[i].EventType != KEY_EVENT)
            continue;

        const auto& e = records[i].Event.KeyEvent;
        const auto wasHeld = std::find (held.begin(), held.end(), e.wVirtualKeyCode);

        if (! e.bKeyDown)
        {
            if (wasHeld != held.end())
                held.erase (wasHeld);

            continue;
        }

        // Held down, a key repeats: more downs, no up in between.
        Key k;
        k.repeat = wasHeld != held.end();

        if (! k.repeat)
            held.push_back (e.wVirtualKeyCode);

        k.shift = (e.dwControlKeyState & SHIFT_PRESSED) != 0;
        k.ctrl = (e.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        k.alt = (e.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;

        switch (e.wVirtualKeyCode)
        {
            case VK_RETURN: k.code = Key::Enter; break;
            case VK_BACK:   k.code = Key::Backspace; break;
            case VK_ESCAPE: k.code = Key::Escape; break;
            case VK_TAB:    k.code = Key::Tab; break;
            case VK_LEFT:   k.code = Key::Left; break;
            case VK_RIGHT:  k.code = Key::Right; break;
            case VK_UP:     k.code = Key::Up; break;
            case VK_DOWN:   k.code = Key::Down; break;
            case VK_HOME:   k.code = Key::Home; break;
            case VK_END:    k.code = Key::End; break;
            case VK_PRIOR:  k.code = Key::PageUp; break;
            case VK_NEXT:   k.code = Key::PageDown; break;
            case VK_DELETE: k.code = Key::Delete; break;
            case VK_F1:     k.code = Key::F1; break;
            default:
                if (e.uChar.UnicodeChar != 0)
                {
                    k.code = Key::Char;
                    k.ch = e.uChar.UnicodeChar;
                }
                break;
        }

        // Ctrl+[ and Ctrl+] arrive as control characters (0x1b is also Escape); keep them as
        // brackets with ctrl held so they can be bound like any other key.
        if (k.ctrl && ! k.shift && (e.wVirtualKeyCode == VK_OEM_4 || e.wVirtualKeyCode == VK_OEM_6))
        {
            k.code = Key::Char;
            k.ch = e.wVirtualKeyCode == VK_OEM_4 ? L'[' : L']';
        }

        // Ctrl+\ (close plugin windows) arrives as a control code.
        if (k.ctrl && ! k.alt && e.wVirtualKeyCode == VK_OEM_5)
        {
            k.code = Key::Char;
            k.ch = L'\\';
        }

        // Alt+/ (place a hit) may arrive without its character on some layouts.
        if (k.alt && ! k.ctrl && ! k.shift && e.wVirtualKeyCode == VK_OEM_2)
        {
            k.code = Key::Char;
            k.ch = L'/';
        }

        // Ctrl+Space may arrive with no character at all.
        if (k.ctrl && e.wVirtualKeyCode == VK_SPACE)
        {
            k.code = Key::Char;
            k.ch = L' ';
        }

        // Likewise Ctrl+letter arrives as a control code (Ctrl+Z = 0x1a); report the letter.
        // Ctrl+Alt is AltGr on many layouts and types real characters, so it's left alone.
        if (k.ctrl && ! k.alt && e.wVirtualKeyCode >= 'A' && e.wVirtualKeyCode <= 'Z')
        {
            k.code = Key::Char;
            k.ch = (wchar_t) (e.wVirtualKeyCode - 'A' + 'a');
        }

        if (k.code == Key::None)
            continue;

        for (WORD r = 0; r < (e.wRepeatCount > 0 ? e.wRepeatCount : 1); ++r)
        {
            keys.push_back (k);
            k.repeat = true; // a count above one is repeats too
        }
    }

    return keys;
}

int Console::keyDown (wchar_t ch) const
{
    const auto scan = VkKeyScanW (ch);

    if (scan == -1)
        return -1;

    return (GetAsyncKeyState (LOBYTE (scan)) & 0x8000) != 0 ? 1 : 0;
}

void Console::setClipboardText (const char* text)
{
    if (! OpenClipboard (nullptr))
        return;

    EmptyClipboard();
    const auto size = std::strlen (text) + 1;

    if (HGLOBAL mem = GlobalAlloc (GMEM_MOVEABLE, size))
    {
        std::memcpy (GlobalLock (mem), text, size);
        GlobalUnlock (mem);

        if (SetClipboardData (CF_TEXT, mem) == nullptr)
            GlobalFree (mem);
    }

    CloseClipboard();
}

int Console::width() const
{
    CONSOLE_SCREEN_BUFFER_INFO info {};
    GetConsoleScreenBufferInfo (out, &info);
    return info.srWindow.Right - info.srWindow.Left + 1;
}

int Console::height() const
{
    CONSOLE_SCREEN_BUFFER_INFO info {};
    GetConsoleScreenBufferInfo (out, &info);
    return info.srWindow.Bottom - info.srWindow.Top + 1;
}

void Console::write (const std::wstring& s)
{
    DWORD written = 0;
    WriteConsoleW (out, s.data(), (DWORD) s.size(), &written, nullptr);
}
} // namespace taw
