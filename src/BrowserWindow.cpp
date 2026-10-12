// The sample browser's window (see BrowserWindow.h). One component draws the kind tabs, the
// map, the list, the kit's pads and a status line, and handles the mouse; a few widgets in the
// toolbar set the axes, the audition level and the switches. The state is App's (the same the
// keys move: App::handleBrowserKey), so the window is only a view onto it.

#include "BrowserWindow.h"
#include "App.h"
#include "EditorWindow.h"

namespace taw
{
namespace
{
constexpr int tabsH = 34, barH = 34, statusH = 26, listW = 300, rowH = 22, padsHeaderH = 26, padGap = 4;
constexpr float dotR = 3.5f, pickRadius = 28.0f;

juce::String kindName (const SampleInfo& s) { return s.kind().isEmpty() ? juce::String ("other") : s.kind(); }

juce::Font uiFont (float size, bool bold = false)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), size, bold ? juce::Font::bold : juce::Font::plain));
}
} // namespace

//==============================================================================
struct BrowserWindow::Panel final : juce::Component, private juce::Timer
{
    explicit Panel (App& a) : app (a)
    {
        setWantsKeyboardFocus (true);
        const auto& c = editorColours();

        for (auto* box : { &xBox, &yBox })
        {
            const auto& names = App::browserAxisNames();

            for (int i = 0; i < names.size(); ++i)
                box->addItem (i == 0 ? juce::String ("sounds alike") : names[i], i + 1);

            box->setColour (juce::ComboBox::backgroundColourId, c.panel);
            box->setColour (juce::ComboBox::textColourId, c.text);
            box->setColour (juce::ComboBox::outlineColourId, c.faint);
            box->setColour (juce::ComboBox::arrowColourId, c.dim);
        }

        xBox.setTooltip ("across the map: sounds alike, or a measurement (alt+left/right)");
        yBox.setTooltip ("up the map (alt+up/down)");
        xBox.onChange = [this] { app.setBrowserAxisTo (false, xBox.getSelectedId() - 1); sync(); };
        yBox.onChange = [this] { app.setBrowserAxisTo (true, yBox.getSelectedId() - 1); sync(); };

        level.setRange (-60.0, 6.0, 0.5);
        level.setSliderStyle (juce::Slider::LinearHorizontal);
        level.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 20);
        level.setTextValueSuffix (" dB");
        level.setTooltip ("the audition's own level (shift+up/down)");
        level.setColour (juce::Slider::trackColourId, c.accent.withAlpha (0.6f));
        level.setColour (juce::Slider::thumbColourId, c.accent);
        level.setColour (juce::Slider::textBoxTextColourId, c.text);
        level.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        level.onValueChange = [this] { app.setAuditionDb (level.getValue()); };

        hoverButton.setTooltip ("audition whatever the pointer is over on the map, without a click");
        hoverButton.onClick = [this] { app.browserHover = hoverButton.getToggleState(); app.saveSettings(); };
        timeButton.setTooltip ("while taw plays, an audition waits for the next grid step, so you hear it in the groove");
        timeButton.onClick = [this] { app.browserInTime = timeButton.getToggleState(); app.saveSettings(); };
        padButton.setTooltip ("on a kit: every sound you audition goes onto the chosen pad at once");
        padButton.onClick = [this] { app.browserToPad = padButton.getToggleState(); };
        likeButton.setTooltip ("the samples that sound most like the chosen pad's sound (or the chosen sample); again: all of them");
        likeButton.onClick = [this] { app.say (app.browserLikeThis()); sync(); };

        for (auto* b : { &hoverButton, &timeButton, &padButton })
        {
            b->setColour (juce::ToggleButton::textColourId, c.text);
            b->setColour (juce::ToggleButton::tickColourId, c.accent);
            b->setColour (juce::ToggleButton::tickDisabledColourId, c.dim);
        }

        likeButton.setColour (juce::TextButton::buttonColourId, c.panel);
        likeButton.setColour (juce::TextButton::textColourOffId, c.text);

        for (auto* comp : std::initializer_list<juce::Component*> { &xBox, &yBox, &level, &hoverButton, &timeButton, &padButton, &likeButton })
        {
            comp->setWantsKeyboardFocus (false);
            comp->setMouseClickGrabsKeyboardFocus (false);
            addAndMakeVisible (comp);
        }

        sync();
        startTimerHz (30);
    }

    juce::String title() const
    {
        const auto* lib = app.library.get();
        return lib == nullptr ? juce::String ("samples")
                              : "samples " + juce::String ((int) shown().size()) + " of " + juce::String ((int) lib->samples().size())
                                    + (app.browserLike >= 0 ? juce::String (L" \x00b7 like this") : juce::String());
    }

private:
    App& app;
    juce::ComboBox xBox, yBox;
    juce::Slider level;
    juce::ToggleButton hoverButton { "hover plays" }, timeButton { "in time" }, padButton { L"audition \x2192 pad" };
    juce::TextButton likeButton { "like this" };

    // What's shown, kept until what decides it changes (the sort is the slow part).
    mutable std::vector<int> shownCache;
    mutable juce::String shownKey;

    int listFirst = 0, hoverSample = -1;
    juce::String lastState;

    enum class Drag { none, map, sample };
    Drag drag = Drag::none;
    int dragSample = -1, mapPicked = -1;
    juce::Point<float> downAt, dragAt;
    int dropPad = -1;
    double padLit = 0.0;
    int padLitNote = -1;

    //==============================================================================
    const std::vector<int>& shown() const
    {
        const auto* lib = app.library.get();
        const auto key = juce::String ((juce::pointer_sized_int) lib) + "|" + juce::String (lib != nullptr ? (int) lib->samples().size() : 0) + "|"
                         + juce::String (app.browserKind) + "|" + juce::String (app.browserLike) + "|" + app.browserFilter;

        if (key != shownKey)
        {
            shownKey = key;
            shownCache = app.browserShown();
        }

        return shownCache;
    }

    int kit() const { return app.kitHere(); }

    // Areas, from the top: the tabs, the toolbar, then the map with the list (and the pads) on its
    // right, and the status line at the bottom.
    juce::Rectangle<int> tabsArea() const { return { 0, 0, getWidth(), tabsH }; }
    juce::Rectangle<int> barArea() const { return { 0, tabsH, getWidth(), barH }; }
    juce::Rectangle<int> bodyArea() const { return { 0, tabsH + barH, getWidth(), getHeight() - tabsH - barH - statusH }; }
    juce::Rectangle<int> statusArea() const { return { 0, getHeight() - statusH, getWidth(), statusH }; }
    juce::Rectangle<int> sideArea() const { return bodyArea().removeFromRight (listW); }
    juce::Rectangle<int> mapArea() const { return bodyArea().withTrimmedRight (listW).reduced (8); }

    juce::Rectangle<int> padsArea() const
    {
        if (kit() < 0)
            return {};

        const auto side = sideArea();
        return side.withTop (side.getBottom() - std::min (side.getHeight() / 2 + 40, listW - 20 + padsHeaderH));
    }

    juce::Rectangle<int> listArea() const
    {
        auto side = sideArea().reduced (4, 6);

        if (const auto pads = padsArea(); ! pads.isEmpty())
            side.setBottom (pads.getY() - 4);

        return side;
    }

    int listRows() const { return std::max (1, listArea().getHeight() / rowH); }

    // Pad 1 bottom left, as on an MPC: row 0 (top) holds pads 13-16.
    juce::Rectangle<float> padRect (int index) const
    {
        const auto area = padsArea().withTrimmedTop (padsHeaderH).reduced (6).toFloat();
        const auto w = (area.getWidth() - 3 * padGap) / 4.0f, h = (area.getHeight() - 3 * padGap) / 4.0f;
        const int col = index % 4, row = 3 - index / 4;
        return { area.getX() + (float) col * (w + padGap), area.getY() + (float) row * (h + padGap), w, h };
    }

    int padAt (juce::Point<float> p) const
    {
        if (padsArea().isEmpty())
            return -1;

        for (int i = 0; i < 16; ++i)
            if (padRect (i).contains (p))
                return i;

        return -1;
    }

    juce::Point<float> dotOf (int sample) const
    {
        const auto area = mapArea().reduced (14).toFloat();
        const auto [x, y] = app.browserPosition (sample);
        return { area.getX() + x * area.getWidth(), area.getY() + y * area.getHeight() };
    }

    int sampleNear (juce::Point<float> p, float radius) const
    {
        int best = -1;
        float bestD = radius * radius;

        for (auto i : shown())
            if (const auto d = dotOf (i).getDistanceSquaredFrom (p); d < bestD)
                bestD = d, best = i;

        return best;
    }

    // Widgets follow App (the keys change the same things).
    void sync()
    {
        xBox.setSelectedId (app.browserAxisX + 1, juce::dontSendNotification);
        yBox.setSelectedId (app.browserAxisY + 1, juce::dontSendNotification);
        yBox.setEnabled (app.browserAxisX != 0);
        level.setValue (app.auditionDb, juce::dontSendNotification);
        hoverButton.setToggleState (app.browserHover, juce::dontSendNotification);
        timeButton.setToggleState (app.browserInTime, juce::dontSendNotification);
        padButton.setToggleState (app.browserToPad, juce::dontSendNotification);
        padButton.setVisible (kit() >= 0);
        likeButton.setButtonText (app.browserLike >= 0 ? "all" : "like this");
    }

    void resized() override
    {
        auto bar = barArea().reduced (8, 5);
        bar.removeFromLeft (std::min (300, bar.getWidth() / 3)); // the filter, drawn
        bar.removeFromLeft (8);
        xBox.setBounds (bar.removeFromLeft (130));
        bar.removeFromLeft (4);
        yBox.setBounds (bar.removeFromLeft (130));
        bar.removeFromLeft (10);
        likeButton.setBounds (bar.removeFromLeft (76));
        bar.removeFromLeft (10);
        level.setBounds (bar.removeFromLeft (170));
        bar.removeFromLeft (8);
        hoverButton.setBounds (bar.removeFromLeft (110));
        timeButton.setBounds (bar.removeFromLeft (90));
        padButton.setBounds (bar.removeFromLeft (140));
    }

    //==============================================================================
    void paint (juce::Graphics& g) override
    {
        const auto& c = editorColours();
        g.fillAll (c.bg);
        const auto* lib = app.library.get();

        paintTabs (g);

        // The filter, as an input line: typing anywhere goes into it.
        {
            const auto box = barArea().reduced (8, 6).withWidth (std::min (300, barArea().getWidth() / 3));
            g.setColour (c.panel);
            g.fillRoundedRectangle (box.toFloat(), 4.0f);
            g.setColour (app.browserFilter.isNotEmpty() ? c.accent : c.faint);
            g.drawRoundedRectangle (box.toFloat().reduced (0.5f), 4.0f, 1.0f);
            g.setFont (uiFont (13.0f));
            g.setColour (app.browserFilter.isEmpty() ? c.dim : c.text);
            g.drawText (app.browserFilter.isEmpty() ? juce::String (L"\x203a type: a name, or dec<200 bright>1k") : L"\x203a " + app.browserFilter + L"\x2581",
                        box.reduced (8, 0), juce::Justification::centredLeft, true);
        }

        if (lib == nullptr || lib->samples().empty())
        {
            g.setColour (c.dim);
            g.setFont (uiFont (15.0f));
#if JUCE_WINDOWS
            const juce::String how = L"no samples yet \x2014 type index D:\\Samples\\Drums in taw (it's only read)";
#else
            const juce::String how = L"no samples yet \x2014 type index ~/Samples/Drums in taw (it's only read)";
#endif
            g.drawText (app.isIndexing() ? juce::String (L"measuring samples\x2026 ") + juce::String (app.indexDone.load()) + " / " + juce::String (app.indexTotal.load()) : how,
                        bodyArea(), juce::Justification::centred, true);
            return;
        }

        paintMap (g);
        paintList (g);
        paintPads (g);
        paintStatus (g);

        // A sample being dragged to a pad: its name under the pointer.
        if (drag == Drag::sample && dragSample >= 0)
        {
            const auto name = juce::File (lib->samples()[(size_t) dragSample].path).getFileNameWithoutExtension();
            const juce::Rectangle<float> tag (dragAt.x + 12.0f, dragAt.y - 10.0f, 12.0f + (float) name.length() * 7.5f, 20.0f);
            g.setColour (c.accent.withAlpha (0.85f));
            g.fillRoundedRectangle (tag, 4.0f);
            g.setColour (c.bg);
            g.setFont (uiFont (12.0f, true));
            g.drawText (name, tag.reduced (6.0f, 0.0f), juce::Justification::centredLeft, false);
        }
    }

    std::vector<std::pair<juce::Rectangle<float>, int>> tabRects() const
    {
        std::vector<std::pair<juce::Rectangle<float>, int>> out;
        const auto kinds = app.browserKinds();
        auto x = 8.0f;

        for (int i = 0; i < (int) kinds.size(); ++i)
        {
            const auto w = 26.0f + (float) (kinds[(size_t) i].length() + 5) * 7.5f;
            out.push_back ({ { x, 6.0f, w, (float) tabsH - 8.0f }, i });
            x += w + 4.0f;
        }

        return out;
    }

    void paintTabs (juce::Graphics& g)
    {
        const auto& c = editorColours();
        g.setColour (c.panel);
        g.fillRect (tabsArea());

        const auto* lib = app.library.get();
        const auto kinds = app.browserKinds();
        std::map<juce::String, int> counts;

        if (lib != nullptr)
            for (const auto& s : lib->samples())
                ++counts[kindName (s)];

        g.setFont (uiFont (13.0f, true));

        for (const auto& [r, i] : tabRects())
        {
            const auto& kind = kinds[(size_t) i];
            const bool chosen = i == juce::jlimit (0, (int) kinds.size() - 1, app.browserKind);
            const auto colour = i == 0 ? c.text : sampleKindColour (kind);
            const int count = i == 0 ? (lib != nullptr ? (int) lib->samples().size() : 0) : counts[kind];

            g.setColour (chosen ? colour : colour.withAlpha (0.14f));
            g.fillRoundedRectangle (r, 5.0f);

            if (chosen && app.browserFocus == App::BrowserFocus::kinds)
            {
                g.setColour (c.accent);
                g.drawRoundedRectangle (r.expanded (2.0f), 6.0f, 2.0f);
            }

            g.setColour (chosen ? c.bg : colour);
            g.drawText (kind + " " + juce::String (count), r, juce::Justification::centred, false);
        }
    }

    void paintMap (juce::Graphics& g)
    {
        const auto& c = editorColours();
        const auto area = mapArea();
        const auto& all = app.library->samples();
        const bool focus = app.browserFocus == App::BrowserFocus::map;

        g.setColour (c.panel.withAlpha (0.5f));
        g.fillRoundedRectangle (area.toFloat(), 6.0f);
        g.setColour (focus ? c.accent.withAlpha (0.7f) : c.faint);
        g.drawRoundedRectangle (area.toFloat().reduced (0.5f), 6.0f, focus ? 1.5f : 1.0f);

        // The axes: the y's high end top left, low end bottom left; the x's along the bottom.
        g.setFont (uiFont (12.0f));

        if (app.browserAxisX != 0)
        {
            const auto [xLow, xHigh] = app.browserAxisEnds (false);
            const auto [yLow, yHigh] = app.browserAxisEnds (true);
            const auto& names = App::browserAxisNames();
            const auto inner = area.reduced (8, 4);
            g.setColour (c.dim);
            g.drawText (L"\x2191 " + names[app.browserAxisY] + "  " + yHigh, inner, juce::Justification::topLeft, false);
            g.drawText (yLow, inner.withTrimmedBottom (16), juce::Justification::bottomLeft, false);
            g.drawText (xLow + L"   \x2192 " + names[app.browserAxisX] + L" \x2192   " + xHigh, inner, juce::Justification::centredBottom, false);
        }
        else
        {
            g.setColour (c.faint);
            g.drawText ("close together = sounds alike", area.reduced (10, 6), juce::Justification::bottomRight, false);
        }

        const auto& list = shown();

        if (list.empty())
        {
            g.setColour (c.dim);
            g.drawText (app.browserFilter.isNotEmpty() ? L"nothing matches \x00b7 backspace" : L"none of these", area, juce::Justification::centred, false);
            return;
        }

        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (area);

        for (auto i : list)
        {
            const auto p = dotOf (i);
            g.setColour (sampleKindColour (kindName (all[(size_t) i])).withAlpha (0.85f));
            g.fillEllipse (p.x - dotR, p.y - dotR, dotR * 2.0f, dotR * 2.0f);
        }

        if (hoverSample >= 0 && hoverSample != app.browserSample && hoverSample < (int) all.size())
        {
            const auto p = dotOf (hoverSample);
            g.setColour (c.text.withAlpha (0.6f));
            g.drawEllipse (p.x - 6.0f, p.y - 6.0f, 12.0f, 12.0f, 1.0f);
        }

        if (app.browserSample >= 0 && app.browserSample < (int) all.size())
        {
            const auto p = dotOf (app.browserSample);
            const auto name = juce::File (all[(size_t) app.browserSample].path).getFileNameWithoutExtension();
            const bool playing = app.engine.previewPosition() >= 0;
            g.setColour (playing ? c.play : c.accent);
            g.drawEllipse (p.x - 8.0f, p.y - 8.0f, 16.0f, 16.0f, 2.0f);
            g.setFont (uiFont (13.0f, true));
            const auto w = (float) name.length() * 8.0f + 12.0f;
            auto label = juce::Rectangle<float> (p.x + 12.0f, p.y - 10.0f, w, 20.0f);

            if (label.getRight() > (float) area.getRight())
                label.setX (p.x - 12.0f - w);

            g.setColour (c.bg.withAlpha (0.8f));
            g.fillRoundedRectangle (label, 4.0f);
            g.setColour (c.text);
            g.drawText (name, label.reduced (6.0f, 0.0f), juce::Justification::centredLeft, false);
        }
    }

    void paintList (juce::Graphics& g)
    {
        const auto& c = editorColours();
        const auto area = listArea();
        const auto& all = app.library->samples();
        const auto& list = shown();
        const bool focus = app.browserFocus == App::BrowserFocus::list;

        g.setColour (focus ? c.accent.withAlpha (0.7f) : c.faint);
        g.drawRoundedRectangle (area.toFloat().expanded (2.0f), 5.0f, focus ? 1.5f : 1.0f);

        // Keep the chosen one in view.
        const auto at = (int) (std::find (list.begin(), list.end(), app.browserSample) - list.begin());

        if (at < (int) list.size() && (at < listFirst || at >= listFirst + listRows()))
            listFirst = std::max (0, at - listRows() / 2);

        listFirst = juce::jlimit (0, std::max (0, (int) list.size() - listRows()), listFirst);

        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (area);

        for (int r = 0; r < listRows() && listFirst + r < (int) list.size(); ++r)
        {
            const auto i = list[(size_t) (listFirst + r)];
            const auto& s = all[(size_t) i];
            const juce::Rectangle<int> row (area.getX(), area.getY() + r * rowH, area.getWidth(), rowH);
            const bool chosen = i == app.browserSample;

            if (chosen)
            {
                g.setColour (c.selection.withAlpha (0.8f));
                g.fillRect (row);
            }

            g.setColour (sampleKindColour (kindName (s)));
            g.fillRect (row.getX() + 2, row.getY() + 4, 4, rowH - 8);
            const juce::File f (s.path);
            g.setFont (uiFont (13.0f, chosen));
            g.setColour (c.text);
            g.drawText (f.getFileNameWithoutExtension(), row.withTrimmedLeft (12).withTrimmedRight (90), juce::Justification::centredLeft, true);
            g.setFont (uiFont (11.0f));
            g.setColour (c.dim);
            g.drawText (f.getParentDirectory().getFileName(), row.withTrimmedRight (4).removeFromRight (86), juce::Justification::centredRight, true);
        }
    }

    void paintPads (juce::Graphics& g)
    {
        const auto area = padsArea();

        if (area.isEmpty())
            return;

        const auto& c = editorColours();
        const auto k = kit();
        const bool focus = app.browserFocus == App::BrowserFocus::pads;

        g.setFont (uiFont (13.0f, true));
        g.setColour (focus ? c.accent : c.text);
        g.drawText (app.session->track (k)[ids::name].toString() + L" \x00b7 pads", area.withHeight (padsHeaderH).reduced (8, 0), juce::Justification::centredLeft, true);
        g.setFont (uiFont (11.0f));
        g.setColour (c.dim);
        g.drawText (app.browserToPad ? L"auditions go on the lit pad" : L"drag a sample onto a pad", area.withHeight (padsHeaderH).reduced (8, 0), juce::Justification::centredRight, true);

        const auto now = juce::Time::getMillisecondCounterHiRes();

        for (int i = 0; i < 16; ++i)
        {
            const auto r = padRect (i);
            const int note = App::firstPadNote + i;
            const auto member = app.kitMemberOnNote (k, note);
            const bool chosen = note == app.browserPadNote;
            const auto from = member >= 0 ? app.librarySampleFor (app.session->track (member)) : -1;
            const auto colour = from >= 0 ? sampleKindColour (kindName (app.library->samples()[(size_t) from])) : c.dim;

            g.setColour (member >= 0 ? colour.withAlpha (padLitNote == note && now < padLit ? 0.7f : 0.28f) : c.panel);
            g.fillRoundedRectangle (r, 5.0f);

            if (i == dropPad || chosen)
            {
                g.setColour (i == dropPad ? c.play : c.accent);
                g.drawRoundedRectangle (r.reduced (1.0f), 5.0f, chosen && focus ? 3.0f : 2.0f);
            }

            g.setFont (uiFont (11.0f, true));
            g.setColour (c.dim);
            g.drawText (juce::String (i + 1), r.reduced (5.0f, 3.0f), juce::Justification::topLeft, false);

            if (member >= 0)
            {
                g.setFont (uiFont (11.0f));
                g.setColour (c.text);
                g.drawFittedText (app.session->track (member)[ids::name].toString(), r.reduced (4.0f, 4.0f).withTrimmedTop (12.0f).toNearestInt(),
                                  juce::Justification::centred, 2, 0.8f);
            }
        }
    }

    void paintStatus (juce::Graphics& g)
    {
        const auto& c = editorColours();
        const auto area = statusArea();
        g.setColour (c.panel);
        g.fillRect (area);
        g.setFont (uiFont (12.0f));

        juce::String text;
        const auto& all = app.library->samples();

        if (app.browserSample >= 0 && app.browserSample < (int) all.size())
        {
            const auto& s = all[(size_t) app.browserSample];
            juce::StringArray parts { kindName (s) + (s.nameKind.isEmpty() ? " (by its sound)" : ""),
                                      juce::String (juce::roundToInt (s.seconds * 1000.0)) + " ms", "attack " + juce::String (s.attackMs, 1) + " ms",
                                      "decay " + juce::String (juce::roundToInt (s.decayMs)) + " ms" };

            if (s.pitchConfidence >= 0.6)
                parts.add (juce::String (juce::roundToInt (s.pitchHz)) + " Hz " + juce::MidiMessage::getMidiNoteName (juce::roundToInt (69.0 + 12.0 * std::log2 (s.pitchHz / 440.0)), true, true, 4));

            parts.add ("bright " + juce::String (juce::roundToInt (s.centroidHz)) + " Hz");
            parts.add (formatDb (s.rmsDb) + " dB rms");
            text = parts.joinIntoString (juce::String (L" \x00b7 "));
        }

        // The keys on the right, when there's room beside the numbers.
        const auto font = uiFont (12.0f);
        const juce::String keys = kit() >= 0 ? L"enter: onto the pad \x00b7 shift+enter: a new track \x00b7 tab: next area \x00b7 esc: back to taw"
                                             : L"enter: onto the track \x00b7 shift+enter: a new track \x00b7 tab: next area \x00b7 esc: back to taw";
        const auto room = (float) area.getWidth() - 20.0f - juce::GlyphArrangement::getStringWidth (font, text) - 30.0f;

        g.setColour (c.text);
        g.drawText (text, area.reduced (10, 0), juce::Justification::centredLeft, true);

        if (juce::GlyphArrangement::getStringWidth (font, keys) <= room)
        {
            g.setColour (c.dim);
            g.drawText (keys, area.reduced (10, 0), juce::Justification::centredRight, true);
        }
    }

    //==============================================================================
    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto before = hoverSample;
        hoverSample = mapArea().contains (e.getPosition()) ? sampleNear (e.position, pickRadius) : -1;

        if (app.browserHover && hoverSample >= 0 && hoverSample != app.browserSample)
            app.browserPick (hoverSample, true, false);

        if (hoverSample != before)
            repaint();
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hoverSample = -1;
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        downAt = dragAt = e.position;
        drag = Drag::none;
        dragSample = mapPicked = -1;

        for (const auto& [r, i] : tabRects())
            if (r.contains (e.position))
            {
                app.browserKind = i;
                app.browserFocus = App::BrowserFocus::kinds;
                app.browserLike = -1;
                keepSelection();
                repaint();
                return;
            }

        if (const auto pad = padAt (e.position); pad >= 0)
        {
            app.browserFocus = App::BrowserFocus::pads;
            app.browserPlayPad (App::firstPadNote + pad);
            lightPad (App::firstPadNote + pad);
            return;
        }

        if (mapArea().contains (e.getPosition()))
        {
            app.browserFocus = App::BrowserFocus::map;
            drag = Drag::map;

            if (const auto i = sampleNear (e.position, pickRadius); i >= 0)
            {
                mapPicked = i;
                dragSample = i;
                app.browserPick (i, true, false); // onto the pad when the drag ends
            }

            repaint();
            return;
        }

        if (listArea().contains (e.getPosition()))
        {
            app.browserFocus = App::BrowserFocus::list;
            const auto row = listFirst + (e.y - listArea().getY()) / rowH;

            if (row >= 0 && row < (int) shown().size())
            {
                dragSample = shown()[(size_t) row];
                app.browserPick (dragSample, true, false);
            }

            repaint();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        dragAt = e.position;

        // Over the map: audition what's under the pointer (XO-style), until it's off the map.
        if (drag == Drag::map && mapArea().contains (e.getPosition()))
        {
            if (const auto i = sampleNear (e.position, pickRadius); i >= 0 && i != app.browserSample)
            {
                mapPicked = dragSample = i;
                app.browserPick (i, true, false);
            }
        }
        else if (dragSample >= 0 && e.position.getDistanceFrom (downAt) > 6.0f)
            drag = Drag::sample; // carrying it (from the list, or off the map) to a pad

        dropPad = drag == Drag::sample || drag == Drag::map ? padAt (e.position) : -1;
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const auto pad = drag != Drag::none ? padAt (e.position) : -1;

        if (pad >= 0 && dragSample >= 0)
        {
            // Dropped on a pad: it goes on that one, which becomes the lit pad.
            app.browserPadNote = App::firstPadNote + pad;
            app.say (app.loadPad (kit(), app.browserPadNote, app.library->samples()[(size_t) dragSample].path));
            lightPad (app.browserPadNote);
        }
        else if ((drag == Drag::map && mapPicked >= 0) || (drag == Drag::none && dragSample >= 0 && listArea().contains (e.getPosition())))
        {
            // A pick: on a kit, onto the lit pad (Battery and XO's way).
            app.browserPick (app.browserSample, false, true);
        }

        drag = Drag::none;
        dragSample = mapPicked = dropPad = -1;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if ((mapArea().contains (e.getPosition()) || listArea().contains (e.getPosition())) && app.browserSample >= 0 && kit() < 0)
            app.say (app.browserLoad (false));
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (listArea().contains (e.getPosition()))
        {
            listFirst = std::max (0, listFirst - (int) std::round (w.deltaY * 12.0f));
            repaint();
        }
    }

    void keepSelection()
    {
        const auto& list = shown();

        if (std::find (list.begin(), list.end(), app.browserSample) == list.end())
            app.browserSample = list.empty() ? -1 : list.front();
    }

    void lightPad (int note)
    {
        padLitNote = note;
        padLit = juce::Time::getMillisecondCounterHiRes() + 180.0;
        repaint();
    }

    bool keyPressed (const juce::KeyPress& kp) override
    {
        const auto k = keyForTaw (kp);

        if (k.code == Key::None)
            return false;

        const auto pad = app.browserPadNote;

        if (! app.handleBrowserKey (k))
            app.deferredKeys.push_back (k); // ctrl keys, shift+space...: taw's

        if (app.browserFocus == App::BrowserFocus::pads && (k.code == Key::Enter || (k.code == Key::Char && k.ch == ' ') || pad != app.browserPadNote))
            lightPad (app.browserPadNote);

        sync();
        repaint();
        return true;
    }

    //==============================================================================
    void timerCallback() override
    {
        // Repaint when what's drawn changed (the keys, taw, a pad loading).
        const auto k = kit();
        juce::String state;
        state << app.browserSample << "|" << app.browserKind << "|" << app.browserFilter << "|" << app.browserLike << "|" << (int) app.browserFocus << "|"
              << app.browserPadNote << "|" << k << "|" << app.browserAxisX << app.browserAxisY << "|" << (app.engine.previewPosition() >= 0 ? 1 : 0)
              << "|" << (app.library != nullptr ? (int) app.library->samples().size() : -1) << "|" << app.indexDone.load() << "|" << app.auditionDb;

        if (k >= 0)
            for (auto m : app.kitMembers (k))
                state << "|" << m << app.session->track (m)[ids::name].toString() << (int) app.session->track (m)[ids::note];

        if (state != lastState || juce::Time::getMillisecondCounterHiRes() < padLit + 100.0)
        {
            lastState = state;
            sync();
            repaint();
        }
    }
};

//==============================================================================
struct BrowserWindow::Window final : EditorWindow<Panel>
{
    explicit Window (App& owner) : EditorWindow<Panel> ("samples", new Panel (owner), 1180, 720) {}
};

BrowserWindow::BrowserWindow (App& a) : app (a) {}
BrowserWindow::~BrowserWindow() { close(); }

void BrowserWindow::open (bool withKeys)
{
    if (window == nullptr)
        window = std::make_unique<Window> (app);

    if (withKeys)
        window->showWithKeys();
    else
    {
        window->closeRequested = false;
        window->setVisible (true);
    }
}

void BrowserWindow::close()
{
    if (window != nullptr && window->isActiveWindow())
        app.console.focusTerminal();

    window.reset();
}

bool BrowserWindow::isOpen() const { return window != nullptr && window->isVisible(); }
bool BrowserWindow::isActive() const { return isOpen() && window->isActiveWindow(); }
void* BrowserWindow::nativeHandle() const { return isOpen() ? window->getWindowHandle() : nullptr; }

juce::Image BrowserWindow::snapshot (App& app, int width, int height)
{
    Panel panel (app);
    panel.setSize (width, height);
    return panel.createComponentSnapshot (panel.getLocalBounds(), true, 1.0f);
}

void BrowserWindow::poll()
{
    if (window != nullptr && window->closeRequested)
    {
        app.engine.preview (nullptr);
        close();
    }
}
} // namespace taw
