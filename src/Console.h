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
        F1,
        Mouse // a button pressed in the terminal (Windows only so far)
    };

    Code code = None;
    wchar_t ch = 0; // for Char
    bool shift = false, ctrl = false, alt = false;
    bool repeat = false; // the key is being held (auto-repeat); Windows only, terminals can't tell

    // Mouse: the cell it was pressed in, which button, and whether it was the second click of a
    // double-click (the first arrives as an ordinary press).
    int x = 0, y = 0;
    bool right = false, doubleClick = false;
};

// Raw terminal access: alternate screen, VT output, unbuffered key events.
// Two backends: the Windows console (conhost / cmd) and POSIX terminals (macOS Terminal).
class Console
{
public:
    Console() = default;
    ~Console();

    // Takes over the terminal (alternate screen, raw keys). Separate from construction so
    // headless runs (taw --exec) never touch the screen.
    bool open();
    bool isOpen() const { return opened; }
    // Whether the font has thin left-edge marks (U+258F), so grid lines and the cursor can sit
    // on a cell's leading edge like selection and clip edges do. Consolas doesn't; taw switches
    // a cmd window to Cascadia Mono when it can.
    bool hasEdgeGlyphs() const { return edgeGlyphs; }
    std::vector<Key> poll(); // non-blocking
    int width() const;

    // Plugin windows (their native handles): when one of them has the keyboard and the mouse
    // is done with it, the terminal gets the keyboard back, so keys keep reaching taw while you
    // click and drag in a plugin. Windows only; elsewhere it does nothing. Call once a frame.
    // keeper: one of taw's own windows (the MIDI editor) that keeps the keyboard while it's in
    // front; it's kept on top like the plugin windows.
    void reclaimKeyboard (const std::vector<void*>& pluginWindows, void* keeper = nullptr);
    void focusTerminal(); // the keyboard back to the terminal

    // Double-clicking a plugin window's title bar gives it the keyboard to keep (typing into the
    // plugin); this is that window until the terminal is clicked, it closes, or it's
    // double-clicked again. nullptr: taw has the keyboard.
    void* keyboardHeldBy() const;
    void toggleKeyboardHold (void* pluginWindow);

    // Puts text on the system clipboard.
    static void setClipboardText (const char* text);
    int height() const;
    void write (const std::wstring&);

private:
    bool opened = false, edgeGlyphs = false;
#ifdef _WIN32
    void* in = nullptr;
    void* out = nullptr;
    unsigned long oldInMode = 0, oldOutMode = 0;
    std::vector<unsigned short> held; // keys down now (virtual key codes), to spot auto-repeat
    unsigned long buttonsDown = 0;    // mouse buttons down at the last event, to spot presses
    void* terminal = nullptr;         // the terminal's window (conhost's, or Windows Terminal's)
    double readySince = 0.0;          // when a plugin window last had the keyboard and the mouse let go
    void* held_by = nullptr;          // a plugin window keeping the keyboard (keyboardHeldBy)
#else
    std::string pending; // bytes of a key sequence still arriving
    int pendingPolls = 0;

public:
    // Turns terminal input into keys, consuming what it used. An unfinished sequence stays in
    // bytes unless flush is set (nothing more is coming). Public for taw --keytest.
    static std::vector<Key> decode (std::string& bytes, bool flush);
#endif
};
} // namespace taw
