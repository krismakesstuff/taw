#pragma once

#include <string>
#include <vector>

namespace taw
{
struct Key
{
    enum Code
    {
        None,
        Char,
        Enter,
        Backspace,
        Escape,
        Tab,
        Left,
        Right,
        Up,
        Down,
        Home,
        End,
        PageUp,
        PageDown,
        Delete,
        F1
    };

    Code code = None;
    wchar_t ch = 0; // for Char
    bool shift = false, ctrl = false, alt = false;
};

// Raw terminal access: alternate screen, VT output, unbuffered key events.
// Windows console (conhost / cmd) first; the interface is small so other backends can follow.
class Console
{
public:
    Console() = default;
    ~Console();

    // Takes over the terminal (alternate screen, raw keys). Separate from construction so
    // headless runs (taw --exec) never touch the screen.
    bool open();
    bool isOpen() const { return opened; }
    std::vector<Key> poll(); // non-blocking
    int width() const;

    // Puts text on the system clipboard.
    static void setClipboardText (const char* text);
    int height() const;
    void write (const std::wstring&);

private:
    bool opened = false;
    void* in = nullptr;
    void* out = nullptr;
    unsigned long oldInMode = 0, oldOutMode = 0;
};
} // namespace taw
