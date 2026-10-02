#include "Console.h"
#include "Safety.h"

#include <juce_core/juce_core.h>

#include <cstdio>
#include <cstring>
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
constexpr const char* enter = "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[>1u";
constexpr const char* leave = "\x1b[<u\x1b[0m\x1b[?25h\x1b[?1049l";

void writeAll (const char* data, size_t size)
{
    while (size > 0)
    {
        const auto n = ::write (STDOUT_FILENO, data, size);

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

        k = csiKey (s.substr (i + 2, j - i - 2), s[j]);
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
    writeAll (enter, std::strlen (enter));
    return true;
}

Console::~Console()
{
    if (! opened)
        return;

    writeAll (leave, std::strlen (leave));
    tcsetattr (STDIN_FILENO, TCSAFLUSH, &oldMode);
}

void Console::reclaimKeyboard (const std::vector<void*>&) {}
void* Console::keyboardHeldBy() const { return nullptr; }

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
    return decode (pending, pendingPolls >= 2);
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
    return ioctl (STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 ? ws.ws_col : 80;
}

int Console::height() const
{
    winsize ws {};
    return ioctl (STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 ? ws.ws_row : 24;
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
