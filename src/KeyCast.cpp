#include "KeyCast.h"

#if JUCE_WINDOWS
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
#endif

namespace taw
{
juce::String KeyCastLog::name (const Key& k)
{
    juce::String key;
    bool shift = k.shift;

    switch (k.code)
    {
        case Key::Char:
            if (k.ch == L' ')
                key = "Space";
            else if (juce::CharacterFunctions::isLetter ((juce::juce_wchar) k.ch))
            {
                // Uppercase is shift, wherever the terminal says so or not.
                shift = shift || juce::CharacterFunctions::isUpperCase ((juce::juce_wchar) k.ch);
                key = juce::String::charToString (juce::CharacterFunctions::toUpperCase ((juce::juce_wchar) k.ch));
            }
            else
            {
                shift = false; // shift is in the symbol already: { not shift+{
                key = juce::String::charToString ((juce::juce_wchar) k.ch);
            }
            break;

        case Key::Enter:     key = "Enter"; break;
        case Key::Backspace: key = "Backspace"; break;
        case Key::Escape:    key = "Esc"; break;
        case Key::Tab:       key = "Tab"; break;
        case Key::Left:      key = juce::String (L"\x2190"); break;
        case Key::Right:     key = juce::String (L"\x2192"); break;
        case Key::Up:        key = juce::String (L"\x2191"); break;
        case Key::Down:      key = juce::String (L"\x2193"); break;
        case Key::Home:      key = "Home"; break;
        case Key::End:       key = "End"; break;
        case Key::PageUp:    key = "PgUp"; break;
        case Key::PageDown:  key = "PgDn"; break;
        case Key::Delete:    key = "Del"; break;
        case Key::F1:        key = "F1"; break;
        case Key::None:
        case Key::Mouse:     return {};
    }

#if JUCE_MAC
    const char* alt = "Opt+"; // taw's alt is Option there
#else
    const char* alt = "Alt+";
#endif

    return juce::String (k.ctrl ? "Ctrl+" : "") + (k.alt ? alt : "") + (shift ? "Shift+" : "") + key;
}

void KeyCastLog::press (const Key& k, bool typed, double now)
{
    if (k.code == Key::Mouse || k.code == Key::None)
        return;

    // Typing on the command line: the characters make a word, until a key that isn't typing.
    if (typed && k.code == Key::Char && ! k.ctrl && ! k.alt && k.ch >= 32)
    {
        const auto c = juce::String::charToString ((juce::juce_wchar) k.ch);

        if (! entries.empty() && entries.back().word)
        {
            entries.back().text += c;
            entries.back().at = now;
            return;
        }

        entries.push_back ({ c, 1, true, now });
    }
    else
    {
        const auto text = name (k);

        if (! entries.empty() && ! entries.back().word && entries.back().text == text && now - entries.back().at < againMs)
        {
            ++entries.back().count;
            entries.back().at = now;
            return;
        }

        entries.push_back ({ text, 1, false, now });
    }

    while ((int) entries.size() > maxEntries)
        entries.pop_front();
}

bool KeyCastLog::expire (double now, double fadeMs)
{
    // Oldest first, and only the newest is ever touched again, so the front goes first.
    if (fadeMs > 0.0)
        while (! entries.empty() && now - entries.front().at > fadeMs + fadeOutMs)
            entries.pop_front();

    return ! entries.empty();
}

//==============================================================================
struct KeyCast::Impl
{
    struct View final : juce::Component
    {
        explicit View (Impl& o) : owner (o)
        {
            setOpaque (true);
            setWantsKeyboardFocus (false);
            setMouseClickGrabsKeyboardFocus (false);
        }

        void paint (juce::Graphics& g) override
        {
            const auto& look = owner.style;
            g.fillAll (look.background);

            const auto& entries = owner.log.recent();

            if (entries.empty())
                return;

            const auto now = juce::Time::getMillisecondCounterHiRes();
            const auto area = getLocalBounds().toFloat().reduced ((float) getHeight() * 0.08f, (float) getHeight() * 0.1f);

            // The text follows the window's height (times size), but never taller than a key fits.
            const auto capHeight = 1.5f; // a key's box, in text heights
            auto height = juce::jmin ((float) getHeight() * 0.45f * (float) look.size, area.getHeight() / capHeight);
            height = juce::jmax (4.0f, height);

            const auto fontName = look.font.isNotEmpty() ? look.font : juce::Font::getDefaultMonospacedFontName();
            auto font = juce::Font (juce::FontOptions (fontName, height, juce::Font::bold));

            // A long word (a path) shows its end, where the typing is.
            auto label = [] (const KeyCastLog::Entry& e)
            {
                const auto text = e.shown();
                return text.length() > 32 ? juce::String (L"\x2026") + text.getLastCharacters (31) : text;
            };

            // Newest first, as many as fit; the newest always shows, smaller if it must.
            struct Shown { juce::String text; float width; float alpha; };
            std::vector<Shown> shown;
            float total = 0.0f;

            const auto gapOf = [] (float h) { return h * 0.4f; };
            const auto padOf = [] (float h) { return h * 0.35f; };

            for (auto it = entries.rbegin(); it != entries.rend(); ++it)
            {
                const auto text = label (*it);
                const auto w = juce::GlyphArrangement::getStringWidth (font, text) + 2.0f * padOf (height);
                const auto with = total + (shown.empty() ? 0.0f : gapOf (height)) + w;

                if (! shown.empty() && with > area.getWidth())
                    break;

                // Fading: full until fade seconds untouched, then gone over fadeOutMs. Older keys dimmer.
                const auto age = now - it->at;
                const auto fadeMs = look.fade * 1000.0;
                auto alpha = fadeMs <= 0.0 || age < fadeMs ? 1.0 : juce::jmax (0.0, 1.0 - (age - fadeMs) / KeyCastLog::fadeOutMs);
                alpha *= shown.empty() ? 1.0 : 0.6;

                shown.push_back ({ text, w, (float) alpha });
                total = with;
            }

            if (total > area.getWidth()) // only the newest, and it's too wide: shrink it to fit
            {
                const auto scale = area.getWidth() / total;
                height *= scale;
                font = font.withHeight (height);
                shown.front().width *= scale;
                total = area.getWidth();
            }

            const auto gap = gapOf (height), pad = padOf (height);
            auto x = area.getCentreX() - total / 2.0f;
            const auto capH = height * capHeight;
            const auto y = area.getCentreY() - capH / 2.0f;
            g.setFont (font);

            for (auto it = shown.rbegin(); it != shown.rend(); ++it) // oldest at the left
            {
                const juce::Rectangle<float> cap (x, y, it->width, capH);
                g.setColour (look.text.withMultipliedAlpha (0.14f * it->alpha));
                g.fillRoundedRectangle (cap, height * 0.25f);
                g.setColour (look.text.withMultipliedAlpha (it->alpha));
                g.drawText (it->text, cap.reduced (pad, 0.0f), juce::Justification::centred, false);
                x += it->width + gap;
            }
        }

        Impl& owner;
    };

    // Like the video window: a normal window (move it, size it) that never takes the keyboard.
    // On top, so it stays in the recording over the terminal.
    struct Window final : juce::DocumentWindow
    {
        explicit Window (View& content)
            : DocumentWindow ("taw keys", juce::Colours::black, DocumentWindow::closeButton | DocumentWindow::minimiseButton, false)
        {
            setUsingNativeTitleBar (true);
            setResizable (true, false);
            setResizeLimits (120, 40, 16000, 9000);
            setAlwaysOnTop (true);
            setWantsKeyboardFocus (false);
            setMouseClickGrabsKeyboardFocus (false); // clicks don't take the keyboard from the terminal
            setContentNonOwned (&content, false);
            centreWithSize (960, 120);
            addToDesktop();
            noActivate();
        }

        int getDesktopWindowStyleFlags() const override
        {
            return DocumentWindow::getDesktopWindowStyleFlags() | juce::ComponentPeer::windowIgnoresKeyPresses;
        }

        void closeButtonPressed() override
        {
            closeRequested = true;
            setVisible (false);
        }

        // Windows: never activated, so clicking or dragging it leaves the keyboard in the terminal.
        void noActivate()
        {
#if JUCE_WINDOWS
            if (auto* hwnd = (HWND) getWindowHandle())
                SetWindowLongPtrW (hwnd, GWL_EXSTYLE, GetWindowLongPtrW (hwnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE | WS_EX_APPWINDOW);
#endif
        }

        bool closeRequested = false;
    };

    explicit Impl (const Style& s) : style (s) {}

    Style style;
    KeyCastLog log;
    bool dirty = false; // something new to draw
    View view { *this };
    Window window { view };
};

KeyCast::KeyCast() = default;
KeyCast::~KeyCast() = default;

void KeyCast::setVisible (bool shouldShow)
{
    if (impl == nullptr)
    {
        if (! shouldShow)
            return;

        impl = std::make_unique<Impl> (current);

        if (savedState.isNotEmpty())
            impl->window.restoreWindowStateFromString (savedState);
    }

    if (shouldShow)
        impl->window.noActivate();
    else
        impl->log.clear(); // nothing stale when it's shown again

    impl->window.setVisible (shouldShow);
}

bool KeyCast::isVisible() const { return impl != nullptr && impl->window.isVisible(); }
bool KeyCast::hasKeyboard() const { return isVisible() && impl->window.isActiveWindow(); }
bool KeyCast::takeCloseRequest() { return impl != nullptr && std::exchange (impl->window.closeRequested, false); }

void KeyCast::press (const Key& k, bool typed)
{
    if (! isVisible())
        return;

    impl->log.press (k, typed, juce::Time::getMillisecondCounterHiRes());
    impl->dirty = true;
}

void KeyCast::tick()
{
    if (! isVisible())
        return;

    // While keys are on show they may be fading: keep drawing until they've gone (and once
    // more after, to clear the last).
    const auto before = impl->log.recent().size();
    const bool left = impl->log.expire (juce::Time::getMillisecondCounterHiRes(), current.fade * 1000.0);

    if (std::exchange (impl->dirty, false) || (left && current.fade > 0.0) || impl->log.recent().size() != before)
        impl->view.repaint();
}

void KeyCast::setStyle (const Style& s)
{
    current = s;

    if (impl != nullptr)
    {
        impl->style = s;
        impl->window.setBackgroundColour (s.background);
        impl->dirty = true;
    }
}

juce::String KeyCast::windowState() const
{
    return impl != nullptr ? impl->window.getWindowStateAsString() : savedState;
}

void KeyCast::restoreWindowState (const juce::String& s)
{
    savedState = s;

    if (impl != nullptr && s.isNotEmpty())
        impl->window.restoreWindowStateFromString (s);
}

juce::Image KeyCast::snapshot()
{
    return impl != nullptr ? impl->view.createComponentSnapshot (impl->view.getLocalBounds()) : juce::Image();
}

bool KeyCast::parseColour (const juce::String& text, juce::Colour& out)
{
    auto t = text.trim().toLowerCase();

    if (t.startsWithChar ('#'))
        t = t.substring (1);

    if ((t.length() == 6 || t.length() == 8) && t.containsOnly ("0123456789abcdef"))
    {
        out = juce::Colour::fromString (t.length() == 6 ? "ff" + t : t);
        return true;
    }

    // A name: Colours::findColourForName gives back the fallback for names it doesn't know.
    const auto named = juce::Colours::findColourForName (t, juce::Colours::transparentBlack);

    if (named == juce::Colours::transparentBlack && t != "transparentblack")
        return false;

    out = named;
    return true;
}

juce::String KeyCast::colourText (juce::Colour c)
{
    const auto hex = c.toDisplayString (! c.isOpaque()).toLowerCase(); // rrggbb or aarrggbb
    return "#" + hex;
}
} // namespace taw
