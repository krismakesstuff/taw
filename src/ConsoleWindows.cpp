#include "Console.h"
#include "Safety.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cstring>

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
    // click can't freeze the screen.
    SetConsoleMode (in, ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS);

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

void Console::reclaimKeyboard (const std::vector<void*>& pluginWindows)
{
    const auto front = GetForegroundWindow();

    if (! opened || terminal == nullptr || front == nullptr || front == (HWND) terminal
        || std::find (pluginWindows.begin(), pluginWindows.end(), (void*) front) == pluginWindows.end())
    {
        readySince = 0.0;
        return; // not one of ours (a plugin's own dialog keeps the keyboard, so you can type a preset name)
    }

    // Not while the mouse is busy in it: a button held (dragging a knob), something captured,
    // a menu open, the window being moved.
    GUITHREADINFO gui {};
    gui.cbSize = sizeof (gui);
    const bool busy = (GetAsyncKeyState (VK_LBUTTON) & 0x8000) != 0 || (GetAsyncKeyState (VK_RBUTTON) & 0x8000) != 0
                      || (GetAsyncKeyState (VK_MBUTTON) & 0x8000) != 0
                      || (GetGUIThreadInfo (GetWindowThreadProcessId (front, nullptr), &gui)
                          && (gui.hwndCapture != nullptr || gui.hwndMenuOwner != nullptr
                              || (gui.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE | GUI_INMOVESIZE)) != 0));
    const auto now = (double) GetTickCount64();

    if (busy)
    {
        readySince = 0.0;
        return;
    }

    // A moment after the click, so a double-click or a menu that opens on release still works.
    if (readySince == 0.0)
        readySince = now;
    else if (now - readySince > 250.0)
    {
        SetForegroundWindow ((HWND) terminal);
        readySince = 0.0;
    }
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
