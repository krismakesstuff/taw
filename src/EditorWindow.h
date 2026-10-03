#pragma once

#include "Console.h"
#include "Plugins.h"

namespace taw
{
// A key an editor doesn't use, as taw's console would have reported it (the editors pass those
// on to taw). Code None: nothing taw would know.
Key keyForTaw (const juce::KeyPress&);

// The window the MIDI and audio editors live in: taw's title bar (TitledWindow), over the
// terminal like the plugin windows, resizable. Content is the editor's component; it names the
// window with title(). Unlike a plugin window it keeps the keyboard while it's in front (the
// console is told: Console::reclaimKeyboard's keeper).
template <typename Content>
struct EditorWindow : juce::DocumentWindow, TitledWindow
{
    EditorWindow (const juce::String& name, Content* owned, int w, int h)
        : DocumentWindow (name, juce::Colours::black, DocumentWindow::closeButton | DocumentWindow::minimiseButton), content (owned)
    {
        setLookAndFeel (&windowLook());
        setUsingNativeTitleBar (false);
        setTitleBarHeight (24);
        setContentOwned (content, false);
        setResizable (true, false);
        setResizeLimits (520, 320, 8000, 6000);
        setAlwaysOnTop (true); // over the terminal, like the plugin windows
        centreWithSize (w, h);
    }

    ~EditorWindow() override
    {
        clearContentComponent();
        setLookAndFeel (nullptr);
    }

    void closeButtonPressed() override
    {
        closeRequested = true;
        setVisible (false);
    }

    // The title bar is ours (see the plugin windows): dragging it moves the window.
    WindowControlKind findControlAtPoint (juce::Point<float> pt) const override
    {
        const auto kind = DocumentWindow::findControlAtPoint (pt);

        switch (kind)
        {
            case WindowControlKind::caption:
            case WindowControlKind::minimise:
            case WindowControlKind::maximise:
            case WindowControlKind::close:
                return WindowControlKind::client;
            default:
                return kind;
        }
    }

    void activeWindowStatusChanged() override
    {
        DocumentWindow::activeWindowStatusChanged();

        for (auto* b : { getMinimiseButton(), getCloseButton() })
            if (b != nullptr)
                b->setEnabled (true);

        repaint (getTitleBarArea());
    }

    juce::String titleName() const override { return content->title(); }
    juce::String titleStatus() const override { return isActiveWindow() ? juce::String (L"keys here \x00b7 esc: back to taw") : juce::String ("keys: taw (click to edit)"); }
    bool titleLit() const override { return isActiveWindow(); }

    // Shows it in front, with the keyboard: you opened it to edit.
    void showWithKeys()
    {
        closeRequested = false;
        setVisible (true);
        toFront (true);
        content->grabKeyboardFocus();
    }

    Content* content = nullptr; // owned as the window's content
    bool closeRequested = false;
};
} // namespace taw
