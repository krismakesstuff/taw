// The MIDI editor window (see MidiEditor.h). One component draws and handles everything below
// the toolbar: the ruler, the keyboard down the left, the notes, and the velocity or controller
// lane at the bottom. Positions are timeline beats on screen; a clip's NOTEs and CCs keep
// content beats (Session::contentZero converts).

#include "MidiEditor.h"
#include "App.h"
#include "EditorWindow.h"

namespace taw
{
namespace
{
EditorColours colours { juce::Colour (0xff1c1c1c), juce::Colour (0xff262626), juce::Colour (0xffd0d0d0), juce::Colour (0xff767676),
                        juce::Colour (0xff444444), juce::Colour (0xff626262), juce::Colour (0xffd0d0d0), juce::Colour (0xff303030),
                        juce::Colour (0xff181818), juce::Colour (0xff5fd7af), juce::Colour (0xffffd75f), juce::Colour (0xffffaf00),
                        juce::Colour (0xff00af5f), juce::Colour (0xffff0000), juce::Colour (0xff121212),
                        juce::Colour (0xff87afd7), juce::Colour (0xff303030), juce::Colour (0xff005f87) };

constexpr int toolbarH = 32, rulerH = 34, keysW = 58, laneH = 110, gap = 4;
constexpr double minLength = 1.0 / 64.0; // beats: the shortest a note gets by dragging

struct GridChoice
{
    const char* name;
    double beats; // 0: off; -1: a bar
};

const GridChoice gridChoices[] = { { "grid: bar", -1.0 },     { "grid: 1/2", 2.0 },    { "grid: 1/4", 1.0 },
                                   { "grid: 1/8", 0.5 },      { "grid: 1/16", 0.25 },  { "grid: 1/32", 0.125 },
                                   { "grid: 1/8 triplet", 1.0 / 3.0 }, { "grid: 1/16 triplet", 1.0 / 6.0 }, { "grid: off", 0.0 } };

struct LaneChoice
{
    const char* name;
    int num; // -1: velocity; 0-127 a controller, 128 pitch bend, 129 channel pressure
};

const LaneChoice laneChoices[] = { { "lane: velocity", -1 },        { "lane: mod wheel (cc 1)", 1 }, { "lane: breath (cc 2)", 2 },
                                   { "lane: volume (cc 7)", 7 },    { "lane: pan (cc 10)", 10 },     { "lane: expression (cc 11)", 11 },
                                   { "lane: sustain (cc 64)", 64 }, { "lane: pitch bend", 128 },     { "lane: pressure", 129 } };

int maxValue (int num) { return num == 128 ? 16383 : 127; }
bool isBlack (int pitch) { const int k = pitch % 12; return k == 1 || k == 3 || k == 6 || k == 8 || k == 10; }
juce::String noteName (int pitch) { return juce::MidiMessage::getMidiNoteName (pitch, true, true, 4); }
int channelOf (const juce::ValueTree& node) { return juce::jlimit (1, 16, (int) node.getProperty (ids::ch, 1)) - 1; }

// The letter a key press is ('A'-'Z'), or 0. JUCE gives letters in either case, and ctrl+letter
// as its control code (ctrl+Z: 0x1a), sometimes without saying ctrl is down.
bool isControlCode (int code) { return code >= 1 && code <= 26 && code != 8 && code != 9 && code != 13; } // not backspace, tab, return

int letterOf (const juce::KeyPress& kp)
{
    const int code = kp.getKeyCode();

    if (code >= 'A' && code <= 'Z') return code;
    if (code >= 'a' && code <= 'z') return code - 'a' + 'A';
    if (isControlCode (code)) return code - 1 + 'A';
    return 0;
}

bool ctrlHeld (const juce::KeyPress& kp) { return ctrlOrCmd (kp.getModifiers()) || isControlCode (kp.getKeyCode()); }
} // namespace

Key keyForTaw (const juce::KeyPress& kp)
{
    Key k;
    const auto mods = kp.getModifiers();
    k.shift = mods.isShiftDown();
    k.ctrl = ctrlHeld (kp);
    k.alt = mods.isAltDown();
    const int code = kp.getKeyCode();

    if (code == juce::KeyPress::returnKey)         k.code = Key::Enter;
    else if (code == juce::KeyPress::escapeKey)    k.code = Key::Escape;
    else if (code == juce::KeyPress::backspaceKey) k.code = Key::Backspace;
    else if (code == juce::KeyPress::tabKey)       k.code = Key::Tab;
    else if (code == juce::KeyPress::leftKey)      k.code = Key::Left;
    else if (code == juce::KeyPress::rightKey)     k.code = Key::Right;
    else if (code == juce::KeyPress::upKey)        k.code = Key::Up;
    else if (code == juce::KeyPress::downKey)      k.code = Key::Down;
    else if (code == juce::KeyPress::homeKey)      k.code = Key::Home;
    else if (code == juce::KeyPress::endKey)       k.code = Key::End;
    else if (code == juce::KeyPress::pageUpKey)    k.code = Key::PageUp;
    else if (code == juce::KeyPress::pageDownKey)  k.code = Key::PageDown;
    else if (code == juce::KeyPress::deleteKey)    k.code = Key::Delete;
    else if (code == juce::KeyPress::F1Key)        k.code = Key::F1;
    else
    {
        k.code = Key::Char;
        auto ch = kp.getTextCharacter();

        // Ctrl+letter: the letter, as the console reports it.
        if (k.ctrl && letterOf (kp) != 0)
            ch = (juce::juce_wchar) (letterOf (kp) - 'A' + 'a');
        else if (ch == 0 && code == juce::KeyPress::spaceKey)
            ch = ' ';

        k.ch = (wchar_t) ch;

        if (k.ch == 0)
            k.code = Key::None;
    }

    return k;
}

void setEditorColours (const EditorColours& c) { colours = c; }
const EditorColours& editorColours() { return colours; }

//==============================================================================
struct MidiEditor::Roll final : juce::Component, private juce::Timer, private juce::ValueTree::Listener
{
    explicit Roll (App& a) : app (a)
    {
        setWantsKeyboardFocus (true);

        for (int i = 0; i < (int) std::size (gridChoices); ++i)
            gridBox.addItem (gridChoices[i].name, i + 1);

        for (int i = 0; i < (int) std::size (laneChoices); ++i)
            laneBox.addItem (laneChoices[i].name, i + 1);

        for (auto* c : { (juce::Component*) &gridBox, (juce::Component*) &laneBox, (juce::Component*) &quantizeButton })
        {
            c->setWantsKeyboardFocus (false); // the keys stay with the notes
            addAndMakeVisible (c);
        }

        gridBox.onChange = [this] { repaint(); };
        laneBox.onChange = [this] { laneNum = laneChoices[juce::jlimit (0, (int) std::size (laneChoices) - 1, laneBox.getSelectedId() - 1)].num; repaint(); };
        laneBox.setSelectedId (1, juce::dontSendNotification);
        quantizeButton.onClick = [this] { quantize(); };
        quantizeButton.setTooltip ("snap note starts to the grid (the selection, or every note)");
        styleControls();
        startTimerHz (30);
    }

    ~Roll() override
    {
        stopSounding();
        track.removeListener (this);
    }

    // The clip shown. Opening another one keeps the zoom; the view goes to it.
    void show (const juce::ValueTree& c)
    {
        stopSounding();
        track.removeListener (this);
        clip = c;
        track = c.getParent();
        track.addListener (this);
        selected.clear();

        // The session's fine step to start with (grid 16: sixteenths).
        if (gridBox.getSelectedId() == 0)
        {
            const auto step = app.session->fineStepBeats();
            int pick = 3;

            for (int i = 0; i < (int) std::size (gridChoices); ++i)
                if (std::abs (gridChoices[i].beats - step) < 1e-9)
                    pick = i + 1;

            gridBox.setSelectedId (pick, juce::dontSendNotification);
        }

        needsFrame = true;
        frameClip(); // now if it has a size, else once it gets one
        repaint();
    }

    juce::String title() const
    {
        if (! clip.isValid() || app.session == nullptr)
            return "MIDI editor";

        const auto spb = app.session->samplesPerBeat();
        const auto from = std::llround ((double) clip[ids::beat] * spb), to = std::llround (Session::clipEnd (clip) * spb);
        return juce::String (trackIndex() + 1) + " " + track[ids::name].toString() + L" \x00b7 clip " + app.formatBars (from) + L" \x2192 "
               + app.formatBars (to);
    }

    juce::ValueTree clip, track;

private:
    App& app;
    juce::ComboBox gridBox, laneBox;
    juce::TextButton quantizeButton { "quantize" };

    double viewBeat = 0.0, pxPerBeat = 64.0, scrollY = 0.0;
    int rowH = 12, laneNum = -1;
    std::vector<juce::ValueTree> selected; // NOTEs in the clip
    bool dirty = true, needsFrame = true;
    juce::int64 lastCursor = -1; // taw's cursor when last drawn

    // Last note drawn: the next one is like it.
    double lastLength = 1.0;
    int lastVelocity = 100;

    // The note being auditioned (-1: none).
    int soundingPitch = -1, soundingChannel = 0;

    enum class Drag { none, move, resizeEnd, resizeStart, marquee, velocity, velocityAll, control, ruler, clipStart, clipEnd, keys };
    Drag drag = Drag::none;

    struct Original
    {
        juce::ValueTree node;
        double start, len;
        int pitch, vel;
    };
    std::vector<Original> originals;
    Original anchor {};
    juce::Point<float> downAt;
    double downBeat = 0.0;
    int downPitch = 60;
    juce::Rectangle<float> marquee;
    std::vector<juce::ValueTree> marqueeBase;
    juce::ValueTree draggedControl;
    juce::String hover; // what's under the mouse, for the ruler's corner

    //==============================================================================
    // Geometry

    int rollTop() const { return toolbarH + rulerH; }
    int laneTop() const { return getHeight() - laneH; }
    int rollBottom() const { return laneTop() - gap; }
    juce::Rectangle<int> rollArea() const { return { keysW, rollTop(), getWidth() - keysW, rollBottom() - rollTop() }; }
    juce::Rectangle<int> laneArea() const { return { keysW, laneTop(), getWidth() - keysW, laneH }; }

    float xOf (double beat) const { return (float) (keysW + (beat - viewBeat) * pxPerBeat); }
    double beatAt (float x) const { return viewBeat + (x - (float) keysW) / pxPerBeat; }
    float yOf (int pitch) const { return (float) (rollTop() + (127 - pitch) * rowH - scrollY); }
    int pitchAt (float y) const { return juce::jlimit (0, 127, 127 - (int) std::floor ((y - (float) rollTop() + (float) scrollY) / (float) rowH)); }

    double visibleBeats() const { return (getWidth() - keysW) / pxPerBeat; }

    // The lane: a value (0..max) at a height, and back.
    float laneY (double value, int max) const
    {
        const auto r = laneArea().reduced (0, 6).toFloat();
        return r.getBottom() - (float) (value / max) * r.getHeight();
    }
    double laneValue (float y, int max) const
    {
        const auto r = laneArea().reduced (0, 6).toFloat();
        return juce::jlimit (0.0, (double) max, (double) ((r.getBottom() - y) / r.getHeight()) * max);
    }

    double spb() const { return app.session->samplesPerBeat(); }
    double zero() const { return Session::contentZero (clip); }
    double clipStart() const { return clip[ids::beat]; }
    double clipEnd() const { return Session::clipEnd (clip); }
    double beatsPerBar() const { return (double) juce::jmax (1, app.session->beatsPerBar()); }

    double gridBeats() const
    {
        const auto g = gridChoices[juce::jlimit (0, (int) std::size (gridChoices) - 1, gridBox.getSelectedId() - 1)].beats;
        return g < 0.0 ? beatsPerBar() : g;
    }

    // To the nearest grid line (alt held: free).
    double snap (double beat, bool free) const
    {
        const auto g = gridBeats();
        return g <= 0.0 || free ? beat : std::round (beat / g) * g;
    }
    // To the grid line at or before.
    double snapDown (double beat, bool free) const
    {
        const auto g = gridBeats();
        return g <= 0.0 || free ? beat : std::floor (beat / g + 1e-9) * g;
    }

    int trackIndex() const
    {
        for (int i = 0; app.session != nullptr && i < app.session->numTracks(); ++i)
            if (app.session->track (i) == track)
                return i;

        return -1;
    }

    // Frames the clip: a little room either side, its notes in the middle vertically.
    void frameClip()
    {
        if (getWidth() <= keysW || ! clip.isValid())
            return;

        needsFrame = false;

        const auto len = std::max (1.0, clipEnd() - clipStart());
        pxPerBeat = juce::jlimit (4.0, 400.0, (getWidth() - keysW) / (len * 1.1));
        viewBeat = clipStart() - len * 0.05;

        int lo = 127, hi = 0;

        for (const auto& n : clip)
            if (n.hasType (ids::NOTE))
                lo = std::min (lo, (int) n[ids::pitch]), hi = std::max (hi, (int) n[ids::pitch]);

        if (lo > hi)
            lo = hi = 60;

        const auto rows = std::max (1, rollArea().getHeight() / rowH);
        const auto middle = (lo + hi) / 2;
        scrollY = juce::jlimit (0.0, std::max (0.0, 128.0 * rowH - rollArea().getHeight()), (127 - middle - rows / 2) * (double) rowH);
    }

    //==============================================================================
    // The clip's notes, on the timeline

    struct ShownNote
    {
        juce::ValueTree node;
        double start, end; // end cut off at the clip's end
        double fullEnd;    // as long as it is
        int pitch, vel;
        bool inWindow;
    };

    std::vector<ShownNote> notesOf (const juce::ValueTree& c) const
    {
        std::vector<ShownNote> out;
        const auto z = Session::contentZero (c), end = Session::clipEnd (c);

        for (const auto& n : c)
            if (n.hasType (ids::NOTE))
            {
                const auto s = z + (double) n[ids::beat], full = s + (double) n[ids::len];
                out.push_back ({ n, s, std::min (end, full), full, (int) n[ids::pitch], (int) n[ids::vel], Session::inWindow (c, n[ids::beat]) });
            }

        return out;
    }

    juce::Rectangle<float> rectOf (const ShownNote& n, bool full = false) const
    {
        const auto x0 = xOf (n.start), x1 = xOf (full ? n.fullEnd : std::max (n.start, n.end));
        return { x0, yOf (n.pitch), std::max (3.0f, x1 - x0), (float) rowH };
    }

    bool isSelected (const juce::ValueTree& n) const { return std::find (selected.begin(), selected.end(), n) != selected.end(); }

    // The note under a point (the topmost, one that plays), and which part of it.
    std::optional<ShownNote> noteAt (juce::Point<float> p) const
    {
        const auto notes = notesOf (clip);

        for (auto it = notes.rbegin(); it != notes.rend(); ++it)
            if (it->inWindow && rectOf (*it).expanded (1.0f, 0.0f).contains (p))
                return *it;

        return std::nullopt;
    }

    // A note on another clip on the track under a point (double-click opens that clip).
    juce::ValueTree otherClipAt (juce::Point<float> p) const
    {
        for (const auto& c : Session::midiClips (track))
            if (c != clip)
                for (const auto& n : notesOf (c))
                    if (n.inWindow && rectOf (n).contains (p))
                        return c;

        return {};
    }

    void pruneSelection()
    {
        selected.erase (std::remove_if (selected.begin(), selected.end(), [this] (const juce::ValueTree& n) { return n.getParent() != clip; }),
                        selected.end());
    }

    //==============================================================================
    // Edits: each gesture is one undo step

    void begin (const juce::String& what) { app.beginEdit (what); }
    juce::UndoManager* um() { return app.um(); }

    void setStart (juce::ValueTree n, double timelineBeat) { n.setProperty (ids::beat, timelineBeat - zero(), um()); }

    // A note moved or lengthened out of the clip takes the clip with it, so it still plays.
    void growToFit (const std::vector<juce::ValueTree>& nodes)
    {
        if (nodes.empty())
            return;

        double lo = clipStart(), hi = clipEnd();

        for (const auto& n : nodes)
        {
            const auto s = zero() + (double) n[ids::beat];
            lo = std::min (lo, s);
            hi = std::max (hi, s + (double) n[ids::len]);
        }

        Session::growMidiClip (clip, std::max (0.0, lo), hi, um());
    }

    juce::ValueTree addNote (double start, int pitch)
    {
        begin ("draw note");
        auto n = Session::makeNote (start - zero(), lastLength, pitch, lastVelocity);
        clip.appendChild (n, um());
        growToFit ({ n });
        return n;
    }

    void deleteSelected()
    {
        pruneSelection();

        if (selected.empty())
            return;

        begin (selected.size() == 1 ? "delete note" : "delete notes");

        for (auto& n : selected)
            clip.removeChild (n, um());

        selected.clear();
    }

    void transpose (int semitones)
    {
        pruneSelection();

        if (selected.empty())
            return;

        for (const auto& n : selected)
            if ((int) n[ids::pitch] + semitones < 0 || (int) n[ids::pitch] + semitones > 127)
                return;

        begin ("transpose notes");

        for (auto& n : selected)
            n.setProperty (ids::pitch, (int) n[ids::pitch] + semitones, um());

        if (selected.size() == 1)
            audition ((int) selected.front()[ids::pitch], channelOf (selected.front()), (int) selected.front()[ids::vel], true);
    }

    void shiftSelected (double beats)
    {
        pruneSelection();

        if (selected.empty())
            return;

        for (const auto& n : selected)
            if (zero() + (double) n[ids::beat] + beats < 0.0)
                return;

        begin ("move notes");

        for (auto& n : selected)
            n.setProperty (ids::beat, (double) n[ids::beat] + beats, um());

        growToFit (selected);
    }

    void duplicateSelected()
    {
        pruneSelection();

        if (selected.empty())
            return;

        double lo = 1e9, hi = -1e9;

        for (const auto& n : selected)
            lo = std::min (lo, (double) n[ids::beat]), hi = std::max (hi, (double) n[ids::beat] + (double) n[ids::len]);

        // Right after the selection, rounded up to the grid so a pattern repeats in time.
        const auto g = gridBeats();
        auto span = hi - lo;
        span = g > 0.0 ? std::max (g, std::ceil (span / g - 1e-9) * g) : span;

        begin ("duplicate notes");
        std::vector<juce::ValueTree> copies;

        for (const auto& n : selected)
        {
            auto c = n.createCopy();
            c.setProperty (ids::beat, (double) n[ids::beat] + span, nullptr);
            clip.appendChild (c, um());
            copies.push_back (c);
        }

        growToFit (copies);
        selected = copies;
    }

    void quantize()
    {
        const auto g = gridBeats();

        if (g <= 0.0)
        {
            app.say (Result::fail ("pick a grid to quantize to"));
            return;
        }

        pruneSelection();
        std::vector<juce::ValueTree> targets = selected;

        if (targets.empty())
            for (const auto& n : notesOf (clip))
                if (n.inWindow)
                    targets.push_back (n.node);

        begin ("quantize notes");
        int moved = 0;

        for (auto& n : targets)
        {
            const auto s = zero() + (double) n[ids::beat], to = std::round (s / g) * g;

            if (std::abs (to - s) > 1e-9)
            {
                setStart (n, to);
                ++moved;
            }
        }

        growToFit (targets);
        app.say (Result::good ("quantized " + juce::String (moved) + (moved == 1 ? " note" : " notes") + " to the "
                               + juce::String (gridChoices[gridBox.getSelectedId() - 1].name).fromFirstOccurrenceOf (": ", false, false) + " grid"
                               + (selected.empty() ? " (every note in the clip)" : "")));
        repaint();
    }

    //==============================================================================
    // Auditioning: through the track's instrument, armed or not

    void audition (int pitch, int channel, int vel, bool brief = false)
    {
        if (pitch == soundingPitch && channel == soundingChannel)
            return;

        stopSounding();
        const auto t = trackIndex();

        if (t < 0)
            return;

        app.engine.audition (t, MidiMsg::noteOn (channel, pitch, juce::jlimit (1, 127, vel)));
        soundingPitch = pitch;
        soundingChannel = channel;

        // From the keyboard there's no mouse-up to end it.
        if (brief)
            juce::Timer::callAfterDelay (250, [safe = juce::Component::SafePointer<Roll> (this), pitch]
                                         {
                                             if (safe != nullptr && safe->soundingPitch == pitch)
                                                 safe->stopSounding();
                                         });
    }

    void stopSounding()
    {
        if (soundingPitch >= 0)
            if (const auto t = trackIndex(); t >= 0)
                app.engine.audition (t, MidiMsg::noteOff (soundingChannel, soundingPitch));

        soundingPitch = -1;
    }

    //==============================================================================
    // The lane: velocity stalks, or a controller's points

    std::vector<juce::ValueTree> controlsShown() const
    {
        std::vector<juce::ValueTree> out;

        for (const auto& c : clip)
            if (c.hasType (ids::CC) && (int) c[ids::num] == laneNum)
                out.push_back (c);

        std::sort (out.begin(), out.end(), [] (const auto& a, const auto& b) { return (double) a[ids::beat] < (double) b[ids::beat]; });
        return out;
    }

    juce::ValueTree controlAt (juce::Point<float> p) const
    {
        for (const auto& c : controlsShown())
            if (juce::Point<float> (xOf (zero() + (double) c[ids::beat]), laneY ((int) c[ids::val], maxValue (laneNum))).getDistanceFrom (p) < 6.0f)
                return c;

        return {};
    }

    // Notes whose velocity stalk is at x (the selected ones first).
    std::vector<juce::ValueTree> stalksAt (float x, float within) const
    {
        std::vector<juce::ValueTree> out;

        for (const auto& n : notesOf (clip))
            if (n.inWindow && std::abs (xOf (n.start) - x) <= within)
                out.push_back (n.node);

        std::stable_partition (out.begin(), out.end(), [this] (const auto& n) { return isSelected (n); });
        return out;
    }

    //==============================================================================
    void resized() override
    {
        auto bar = getLocalBounds().removeFromTop (toolbarH).reduced (6, 4);
        gridBox.setBounds (bar.removeFromLeft (170));
        bar.removeFromLeft (6);
        quantizeButton.setBounds (bar.removeFromLeft (84));
        bar.removeFromLeft (6);
        laneBox.setBounds (bar.removeFromLeft (200));

        if (needsFrame)
            frameClip();
    }

    void styleControls()
    {
        for (auto* box : { &gridBox, &laneBox })
        {
            box->setColour (juce::ComboBox::backgroundColourId, colours.panel);
            box->setColour (juce::ComboBox::textColourId, colours.text);
            box->setColour (juce::ComboBox::outlineColourId, colours.faint);
            box->setColour (juce::ComboBox::arrowColourId, colours.dim);
        }

        quantizeButton.setColour (juce::TextButton::buttonColourId, colours.panel);
        quantizeButton.setColour (juce::TextButton::textColourOffId, colours.text);
        quantizeButton.setColour (juce::ComboBox::outlineColourId, colours.faint);
    }

    void paint (juce::Graphics& g) override
    {
        if (! clip.isValid() || app.session == nullptr)
            return;

        pruneSelection();
        const auto font = juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
        g.setFont (font);
        g.fillAll (colours.bg);

        // Toolbar: what the mouse does, after the controls.
        g.setColour (colours.panel);
        g.fillRect (0, 0, getWidth(), toolbarH);
        g.setColour (colours.dim);
        g.drawText (L"double-click: new note \x00b7 drag: move (alt: off the grid) \x00b7 edge: length \x00b7 right-click: delete \x00b7 ctrl+wheel: zoom",
                    laneBox.getRight() + 12, 0, getWidth() - laneBox.getRight() - 18, toolbarH, juce::Justification::centredLeft, true);

        const auto roll = rollArea();
        const auto bpb = beatsPerBar();
        const auto firstBeat = std::floor (beatAt ((float) keysW)), lastBeat = std::ceil (beatAt ((float) getWidth()));
        const auto g0 = gridBeats();

        // Rows: black keys darker, a line under each C.
        {
            juce::Graphics::ScopedSaveState clipRoll (g);
            g.reduceClipRegion (roll);

            for (int p = pitchAt ((float) roll.getBottom()); p <= pitchAt ((float) roll.getY()); ++p)
            {
                const auto y = yOf (p);

                if (isBlack (p))
                {
                    g.setColour (colours.blackRow);
                    g.fillRect ((float) roll.getX(), y, (float) roll.getWidth(), (float) rowH);
                }

                if (p % 12 == 0)
                {
                    g.setColour (colours.faint);
                    g.fillRect ((float) roll.getX(), y + rowH - 1.0f, (float) roll.getWidth(), 1.0f);
                }
            }

            // Outside the clip: shaded.
            g.setColour (colours.outside.withAlpha (0.6f));
            g.fillRect (juce::Rectangle<float> ((float) roll.getX(), (float) roll.getY(), std::max (0.0f, xOf (clipStart()) - roll.getX()), (float) roll.getHeight()));
            g.fillRect (juce::Rectangle<float> (xOf (clipEnd()), (float) roll.getY(), std::max (0.0f, roll.getRight() - xOf (clipEnd())), (float) roll.getHeight()));

            // Grid: the chosen grid faintly (when there's room), beats, bars.
            if (g0 > 0.0 && g0 * pxPerBeat >= 6.0)
                for (auto b = std::ceil (beatAt ((float) keysW) / g0) * g0; b < lastBeat; b += g0)
                {
                    g.setColour (colours.faint.withAlpha (0.45f));
                    g.fillRect (xOf (b), (float) roll.getY(), 1.0f, (float) roll.getHeight());
                }

            for (auto b = firstBeat; b <= lastBeat; b += 1.0)
            {
                const bool barLine = std::fmod (b, bpb) == 0.0;

                if (! barLine && pxPerBeat < 8.0)
                    continue;

                g.setColour (barLine ? colours.bar : colours.faint);
                g.fillRect (xOf (b), (float) roll.getY(), 1.0f, (float) roll.getHeight());
            }

            // Other clips' notes on the track: outlines, to play against (double-click opens one).
            for (const auto& c : Session::midiClips (track))
                if (c != clip)
                    for (const auto& n : notesOf (c))
                        if (n.inWindow)
                        {
                            g.setColour (colours.dim.withAlpha (0.7f));
                            g.drawRect (rectOf (n).reduced (0.0f, 1.0f), 1.0f);
                        }

            // This clip's notes: brighter for louder; hidden ones (outside its window) faint;
            // the part past the clip's end, which doesn't play, faint too.
            for (const auto& n : notesOf (clip))
            {
                const auto r = rectOf (n).reduced (0.0f, 1.0f);
                const bool sel = isSelected (n.node);

                if (! n.inWindow)
                {
                    g.setColour (colours.note.withAlpha (0.18f));
                    g.fillRect (rectOf (n, true).reduced (0.0f, 1.0f));
                    continue;
                }

                if (n.fullEnd > n.end)
                {
                    g.setColour (colours.note.withAlpha (0.18f));
                    g.fillRect (juce::Rectangle<float> (r.getRight(), r.getY(), xOf (n.fullEnd) - r.getRight(), r.getHeight()));
                }

                const auto base = sel ? colours.noteSelected : colours.note;
                g.setColour (base.withAlpha (0.45f + 0.55f * (float) n.vel / 127.0f));
                g.fillRoundedRectangle (r, 2.0f);
                g.setColour (sel ? colours.text : base.darker (0.6f));
                g.drawRoundedRectangle (r, 2.0f, 1.0f);

                if (r.getWidth() > 34.0f && rowH >= 11)
                {
                    g.setColour (colours.bg);
                    g.drawText (noteName (n.pitch), r.reduced (3.0f, 0.0f), juce::Justification::centredLeft, false);
                }
            }

            if (! marquee.isEmpty())
            {
                g.setColour (colours.accent.withAlpha (0.15f));
                g.fillRect (marquee);
                g.setColour (colours.accent);
                g.drawRect (marquee, 1.0f);
            }
        }

        // The keyboard down the left.
        {
            juce::Graphics::ScopedSaveState clipKeys (g);
            g.reduceClipRegion (0, roll.getY(), keysW, roll.getHeight());
            g.setColour (colours.panel);
            g.fillRect (0, roll.getY(), keysW, roll.getHeight());

            // White keys first, as one strip with a line where two white keys meet (under each C
            // and F); then the black keys over it, shorter, as on a piano.
            g.setColour (colours.whiteKey);
            g.fillRect (0, roll.getY(), keysW - 1, roll.getHeight());

            for (int p = pitchAt ((float) roll.getBottom()); p <= pitchAt ((float) roll.getY()); ++p)
            {
                const auto y = yOf (p);

                if (p == soundingPitch && ! isBlack (p))
                {
                    g.setColour (colours.accent);
                    g.fillRect (0.0f, y, (float) (keysW - 1), (float) rowH);
                }

                if (p % 12 == 0 || p % 12 == 5)
                {
                    g.setColour (colours.dim);
                    g.fillRect (0.0f, y + rowH - 1.0f, (float) (keysW - 1), 1.0f);
                }
            }

            for (int p = pitchAt ((float) roll.getBottom()); p <= pitchAt ((float) roll.getY()); ++p)
            {
                const auto y = yOf (p);

                if (isBlack (p))
                {
                    g.setColour (p == soundingPitch ? colours.accent : colours.blackKey);
                    g.fillRoundedRectangle (0.0f, y + 1.0f, keysW * 0.6f, (float) rowH - 2.0f, 1.5f);
                }
                else if (p % 12 == 0 || rowH >= 14)
                {
                    g.setColour (colours.bg);
                    g.drawText (noteName (p), 0, (int) y, keysW - 4, rowH, juce::Justification::centredRight, false);
                }
            }
        }

        // Ruler: bars, beats, the clip (its edges drag), and where taw's cursor is.
        {
            const juce::Rectangle<int> ruler (keysW, toolbarH, getWidth() - keysW, rulerH);
            g.setColour (colours.panel);
            g.fillRect (0, toolbarH, getWidth(), rulerH);
            juce::Graphics::ScopedSaveState clipRuler (g);
            g.reduceClipRegion (ruler);

            const auto every = pxPerBeat * bpb < 40.0 ? std::ceil (40.0 / (pxPerBeat * bpb)) : 1.0;

            for (auto b = std::floor (firstBeat / bpb) * bpb; b <= lastBeat; b += 1.0)
            {
                const bool barLine = std::fmod (b, bpb) == 0.0;
                const auto bar = (int) std::floor (b / bpb);

                if (barLine && std::fmod ((double) bar, every) == 0.0)
                {
                    g.setColour (colours.text);
                    g.fillRect (xOf (b), (float) toolbarH, 1.0f, 12.0f);
                    g.drawText (juce::String (bar + 1), (int) xOf (b) + 3, toolbarH, 40, 14, juce::Justification::centredLeft, false);
                }
                else if (! barLine && pxPerBeat >= 12.0)
                {
                    g.setColour (colours.dim);
                    g.fillRect (xOf (b), (float) toolbarH + 7.0f, 1.0f, 5.0f);
                }
            }

            // Every clip on the track as a strip; this one bright, with handles on its edges.
            const auto stripY = (float) toolbarH + 18.0f, stripH = 12.0f;

            for (const auto& c : Session::midiClips (track))
            {
                const auto r = juce::Rectangle<float> (xOf (c[ids::beat]), stripY, xOf (Session::clipEnd (c)) - xOf (c[ids::beat]), stripH);
                g.setColour (c == clip ? colours.note : colours.faint);
                g.fillRect (r);

                if (c == clip)
                {
                    g.setColour (colours.text);
                    g.fillRect (r.getX(), stripY - 2.0f, 3.0f, stripH + 4.0f);
                    g.fillRect (r.getRight() - 3.0f, stripY - 2.0f, 3.0f, stripH + 4.0f);
                }
            }
        }

        // The lane.
        {
            const auto lane = laneArea();
            g.setColour (colours.panel);
            g.fillRect (0, lane.getY(), getWidth(), lane.getHeight());
            g.setColour (colours.dim);
            g.drawText (laneNum < 0 ? "vel" : laneNum == 128 ? "bend" : laneNum == 129 ? "press" : "cc " + juce::String (laneNum),
                        4, lane.getY() + 4, keysW - 8, 14, juce::Justification::centredLeft, false);

            juce::Graphics::ScopedSaveState clipLane (g);
            g.reduceClipRegion (lane);
            g.setColour (colours.outside.withAlpha (0.6f));
            g.fillRect (juce::Rectangle<float> ((float) lane.getX(), (float) lane.getY(), std::max (0.0f, xOf (clipStart()) - lane.getX()), (float) lane.getHeight()));
            g.fillRect (juce::Rectangle<float> (xOf (clipEnd()), (float) lane.getY(), std::max (0.0f, lane.getRight() - xOf (clipEnd())), (float) lane.getHeight()));

            if (laneNum < 0)
            {
                for (const auto& n : notesOf (clip))
                    if (n.inWindow)
                    {
                        const auto x = xOf (n.start), y = laneY (n.vel, 127);
                        g.setColour (isSelected (n.node) ? colours.noteSelected : colours.note);
                        g.fillRect (x, y, 2.0f, laneY (0, 127) - y);
                        g.fillEllipse (x - 2.5f, y - 2.5f, 7.0f, 7.0f);
                    }
            }
            else
            {
                // Steps: a controller holds its value until the next one.
                const auto points = controlsShown();
                const auto max = maxValue (laneNum);
                g.setColour (colours.note);

                for (size_t i = 0; i < points.size(); ++i)
                {
                    const auto x = xOf (zero() + (double) points[i][ids::beat]), y = laneY ((int) points[i][ids::val], max);
                    const auto next = i + 1 < points.size() ? xOf (zero() + (double) points[i + 1][ids::beat]) : xOf (clipEnd());
                    g.fillRect (x, y, std::max (1.0f, next - x), 2.0f);
                    g.fillEllipse (x - 3.0f, y - 3.0f, 7.0f, 7.0f);
                }

                if (laneNum == 128)
                {
                    g.setColour (colours.faint);
                    g.fillRect ((float) lane.getX(), laneY (8192, max), (float) lane.getWidth(), 1.0f); // centre
                }
            }
        }

        // taw's cursor and playhead, through everything.
        auto line = [&] (juce::int64 pos, juce::Colour c, float w)
        {
            const auto x = xOf ((double) pos / spb());

            if (x >= keysW && x <= getWidth())
            {
                g.setColour (c);
                g.fillRect (x, (float) toolbarH, w, (float) (getHeight() - toolbarH));
            }
        };

        if (! app.engine.isRolling() || app.cursor != app.heardPlayhead())
            line (app.cursor, colours.accent, 1.0f);

        if (app.engine.isRolling())
            line (app.heardPlayhead(), app.recording ? colours.rec : colours.play, 2.0f);

        // What's under the mouse, in the corner.
        g.setColour (colours.dim);
        g.drawText (hover, 4, toolbarH + 2, keysW - 6, rulerH - 4, juce::Justification::topLeft, true);
    }

    //==============================================================================
    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto p = e.position;
        auto shape = juce::MouseCursor::NormalCursor;
        juce::String now;

        if (rollArea().contains (e.getPosition()))
        {
            now = noteName (pitchAt (p.y));

            if (const auto n = noteAt (p))
            {
                const auto r = rectOf (*n);
                shape = p.x > r.getRight() - 6.0f && r.getWidth() > 12.0f   ? juce::MouseCursor::RightEdgeResizeCursor
                        : p.x < r.getX() + 4.0f && r.getWidth() > 12.0f      ? juce::MouseCursor::LeftEdgeResizeCursor
                                                                              : juce::MouseCursor::DraggingHandCursor;
                now = noteName (n->pitch) + "\nvel " + juce::String (n->vel);
            }
        }
        else if (e.y >= toolbarH && e.y < toolbarH + rulerH && e.x >= keysW)
        {
            if (std::abs (p.x - xOf (clipStart())) < 6.0f || std::abs (p.x - xOf (clipEnd())) < 6.0f)
                shape = juce::MouseCursor::LeftRightResizeCursor;
        }

        setMouseCursor (shape);

        if (now != hover)
        {
            hover = now;
            repaint (0, toolbarH, keysW, rulerH);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();

        if (app.session == nullptr || ! clip.isValid())
            return;

        const auto p = e.position;
        const bool free = e.mods.isAltDown();
        downAt = p;
        downBeat = beatAt (p.x);
        downPitch = pitchAt (p.y);
        drag = Drag::none;
        pruneSelection();

        // Ruler: a clip edge drags; anywhere else puts taw's cursor there (and drags it).
        if (e.y >= toolbarH && e.y < toolbarH + rulerH)
        {
            if (e.x < keysW)
                return;

            if (std::abs (p.x - xOf (clipEnd())) < 6.0f)
                drag = Drag::clipEnd;
            else if (std::abs (p.x - xOf (clipStart())) < 6.0f)
                drag = Drag::clipStart;
            else
            {
                drag = Drag::ruler;
                app.moveTo (std::llround (std::max (0.0, snap (downBeat, free)) * spb()));
            }

            if (drag == Drag::clipEnd || drag == Drag::clipStart)
                begin ("resize clip");

            repaint();
            return;
        }

        // The keyboard: plays the key (drag along it for more).
        if (e.x < keysW && e.y >= rollTop() && e.y < rollBottom())
        {
            drag = Drag::keys;
            audition (downPitch, 0, 100);
            repaint();
            return;
        }

        if (rollArea().contains (e.getPosition()))
        {
            const auto hit = noteAt (p);

            if (e.mods.isPopupMenu())
            {
                if (hit)
                {
                    begin ("delete note");
                    clip.removeChild (hit->node, um());
                    selected.erase (std::remove (selected.begin(), selected.end(), hit->node), selected.end());
                }

                return;
            }

            if (! hit)
            {
                drag = Drag::marquee;
                marqueeBase = e.mods.isShiftDown() ? selected : std::vector<juce::ValueTree>();
                selected = marqueeBase;
                marquee = {};
                repaint();
                return;
            }

            if (e.mods.isShiftDown() && isSelected (hit->node))
            {
                selected.erase (std::remove (selected.begin(), selected.end(), hit->node), selected.end());
                repaint();
                return;
            }

            if (! isSelected (hit->node))
            {
                if (! e.mods.isShiftDown())
                    selected.clear();

                selected.push_back (hit->node);
            }

            const auto r = rectOf (*hit);
            drag = p.x > r.getRight() - 6.0f && r.getWidth() > 12.0f ? Drag::resizeEnd
                   : p.x < r.getX() + 4.0f && r.getWidth() > 12.0f   ? Drag::resizeStart
                                                                     : Drag::move;
            begin (drag == Drag::move ? "move notes" : "change note lengths");
            originals.clear();

            for (const auto& n : selected)
            {
                const Original o { n, zero() + (double) n[ids::beat], (double) n[ids::len], (int) n[ids::pitch], (int) n[ids::vel] };
                originals.push_back (o);

                if (n == hit->node)
                    anchor = o;
            }

            lastLength = anchor.len;
            lastVelocity = anchor.vel;
            audition (anchor.pitch, channelOf (anchor.node), anchor.vel);
            repaint();
            return;
        }

        if (laneArea().contains (e.getPosition()))
        {
            if (laneNum < 0)
            {
                // A selected note's stalk moves every selected note's velocity together; any
                // other stalk is set where you drag, and so is each one you drag across.
                const auto stalks = stalksAt (p.x, 4.0f);

                if (! stalks.empty() && isSelected (stalks.front()) && selected.size() > 1)
                {
                    drag = Drag::velocityAll;
                    originals.clear();

                    for (const auto& n : selected)
                        originals.push_back ({ n, 0.0, 0.0, (int) n[ids::pitch], (int) n[ids::vel] });
                }
                else
                {
                    drag = Drag::velocity;
                }

                begin ("set velocity");
                mouseDrag (e);
                return;
            }

            // A controller: right-click deletes a point, a point drags, elsewhere adds one.
            auto point = controlAt (p);

            if (e.mods.isPopupMenu())
            {
                if (point.isValid())
                {
                    begin ("delete controller point");
                    clip.removeChild (point, um());
                }

                return;
            }

            begin (point.isValid() ? "move controller point" : "add controller point");

            if (! point.isValid())
            {
                const auto at = juce::jlimit (clipStart(), clipEnd() - minLength, snapDown (downBeat, free));
                point = Session::makeControl (at - zero(), laneNum, (int) std::round (laneValue (p.y, maxValue (laneNum))));
                clip.appendChild (point, um());
            }

            draggedControl = point;
            drag = Drag::control;
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (app.session == nullptr || ! clip.isValid())
            return;

        const auto p = e.position;
        const bool free = e.mods.isAltDown();
        const auto dx = beatAt (p.x) - downBeat;

        switch (drag)
        {
            case Drag::move:
            {
                auto start = snap (anchor.start + dx, free);
                auto delta = start - anchor.start;
                auto dp = pitchAt (p.y) - downPitch;

                for (const auto& o : originals)
                {
                    delta = std::max (delta, -o.start); // nothing before the session's start
                    dp = juce::jlimit (-o.pitch, 127 - o.pitch, dp);
                }

                for (auto& o : originals)
                {
                    o.node.setProperty (ids::beat, o.start + delta - zero(), um());
                    o.node.setProperty (ids::pitch, o.pitch + dp, um());
                }

                audition (anchor.pitch + dp, channelOf (anchor.node), anchor.vel);
                break;
            }

            case Drag::resizeEnd:
            {
                const auto end = snap (anchor.start + anchor.len + dx, free);
                const auto delta = end - (anchor.start + anchor.len);

                for (auto& o : originals)
                    o.node.setProperty (ids::len, std::max (minLength, o.len + delta), um());

                lastLength = std::max (minLength, anchor.len + delta);
                break;
            }

            case Drag::resizeStart:
            {
                auto delta = snap (anchor.start + dx, free) - anchor.start;

                for (const auto& o : originals)
                    delta = juce::jlimit (-o.start, o.len - minLength, delta);

                for (auto& o : originals)
                    o.node.setProperty (ids::beat, o.start + delta - zero(), um()).setProperty (ids::len, o.len - delta, um());

                break;
            }

            case Drag::marquee:
            {
                marquee = juce::Rectangle<float> (downAt, p);
                selected = marqueeBase;

                for (const auto& n : notesOf (clip))
                    if (n.inWindow && rectOf (n).intersects (marquee) && ! isSelected (n.node))
                        selected.push_back (n.node);

                break;
            }

            case Drag::velocity:
            {
                const auto vel = juce::jlimit (1, 127, (int) std::round (laneValue (p.y, 127)));
                const auto lo = std::min (p.x, (float) e.getMouseDownX()), hi = std::max (p.x, (float) e.getMouseDownX());
                const auto under = stalksAt (p.x, 4.0f);

                // The stalk under the mouse, and while dragging across, every stalk it passed.
                for (const auto& n : notesOf (clip))
                    if (n.inWindow && ((xOf (n.start) >= lo && xOf (n.start) <= hi && std::abs (e.getDistanceFromDragStartX()) > 3)
                                       || (! under.empty() && n.node == under.front())))
                    {
                        auto node = n.node;
                        node.setProperty (ids::vel, vel, um());
                    }

                lastVelocity = vel;
                break;
            }

            case Drag::velocityAll:
            {
                const auto delta = (int) std::round ((downAt.y - p.y) / (laneArea().getHeight() - 12.0f) * 127.0f);

                for (auto& o : originals)
                    o.node.setProperty (ids::vel, juce::jlimit (1, 127, o.vel + delta), um());

                break;
            }

            case Drag::control:
            {
                const auto at = juce::jlimit (clipStart(), clipEnd() - minLength, snap (beatAt (p.x), free));
                draggedControl.setProperty (ids::beat, at - zero(), um())
                    .setProperty (ids::val, (int) std::round (laneValue (p.y, maxValue (laneNum))), um());
                break;
            }

            case Drag::ruler:
                app.moveTo (std::llround (std::max (0.0, snap (beatAt (p.x), free)) * spb()));
                break;

            case Drag::clipStart:
            {
                const auto z = zero(), end = clipEnd();
                const auto at = juce::jlimit (0.0, end - minLength, snap (beatAt (p.x), free));
                clip.setProperty (ids::beat, at, um()).setProperty (ids::offset, at - z, um()).setProperty (ids::len, end - at, um());
                break;
            }

            case Drag::clipEnd:
            {
                const auto at = std::max (clipStart() + minLength, snap (beatAt (p.x), free));
                clip.setProperty (ids::len, at - clipStart(), um());
                break;
            }

            case Drag::keys:
                audition (pitchAt (p.y), 0, 100);
                break;

            case Drag::none:
                break;
        }

        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (drag == Drag::move || drag == Drag::resizeEnd || drag == Drag::resizeStart)
            growToFit (selected);

        if (drag != Drag::velocity && drag != Drag::velocityAll)
            stopSounding();

        drag = Drag::none;
        marquee = {};
        draggedControl = {};
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (app.session == nullptr || ! clip.isValid() || ! rollArea().contains (e.getPosition()) || e.mods.isPopupMenu())
            return;

        if (noteAt (e.position))
            return;

        // Another clip's note: open that clip.
        if (const auto other = otherClipAt (e.position); other.isValid())
        {
            show (other);
            return;
        }

        // Empty: a new note at the grid line before the click, as long as the last one.
        const auto start = std::max (0.0, snapDown (beatAt (e.position.x), e.mods.isAltDown()));
        const auto n = addNote (start, pitchAt (e.position.y));
        selected = { n };
        audition ((int) n[ids::pitch], 0, lastVelocity, true);
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const auto dy = w.deltaY != 0.0f ? w.deltaY : w.deltaX;

        if (ctrlOrCmd (e.mods))
        {
            // Zoom time around the mouse.
            const auto at = beatAt (e.position.x);
            pxPerBeat = juce::jlimit (2.0, 800.0, pxPerBeat * std::pow (1.25, dy * 4.0));
            viewBeat = at - (e.position.x - keysW) / pxPerBeat;
        }
        else if (e.mods.isAltDown())
        {
            const auto pitch = pitchAt (e.position.y);
            rowH = juce::jlimit (6, 32, rowH + (dy > 0 ? 2 : -2));
            scrollY = (127 - pitch) * (double) rowH - (e.position.y - rollTop());
        }
        else if (e.mods.isShiftDown() || w.deltaX != 0.0f)
        {
            viewBeat -= dy * visibleBeats() * 0.25;
        }
        else
        {
            scrollY -= dy * rowH * 12.0;
        }

        viewBeat = std::max (-visibleBeats() * 0.5, viewBeat);
        scrollY = juce::jlimit (0.0, std::max (0.0, 128.0 * rowH - rollArea().getHeight()), scrollY);
        repaint();
    }

    //==============================================================================
    bool keyPressed (const juce::KeyPress& kp) override
    {
        if (app.session == nullptr || ! clip.isValid())
            return false;

        const auto code = kp.getKeyCode();
        const auto mods = kp.getModifiers();
        pruneSelection();
        const bool any = ! selected.empty();

        if ((code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) && any)
            deleteSelected();
        else if (code == juce::KeyPress::upKey && any)
            transpose (mods.isShiftDown() ? 12 : 1);
        else if (code == juce::KeyPress::downKey && any)
            transpose (mods.isShiftDown() ? -12 : -1);
        else if ((code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) && any)
        {
            const auto g = gridBeats() > 0.0 ? gridBeats() : 1.0;
            shiftSelected ((code == juce::KeyPress::leftKey ? -1.0 : 1.0) * (ctrlOrCmd (mods) ? beatsPerBar() : g));
        }
        else if (letterOf (kp) == 'A' && ctrlHeld (kp) && ! mods.isAltDown())
        {
            selected.clear();

            for (const auto& n : notesOf (clip))
                if (n.inWindow)
                    selected.push_back (n.node);
        }
        else if (letterOf (kp) == 'D' && ctrlHeld (kp) && ! mods.isAltDown() && any)
            duplicateSelected();
        else if (letterOf (kp) == 'Q' && ! mods.isAnyModifierKeyDown() && ! ctrlHeld (kp))
            quantize();
        else if ((kp.getTextCharacter() == '+' || kp.getTextCharacter() == '=') && ! ctrlOrCmd (mods))
            zoomBy (1.5);
        else if (kp.getTextCharacter() == '-' && ! ctrlOrCmd (mods))
            zoomBy (1.0 / 1.5);
        else if (code == juce::KeyPress::escapeKey)
        {
            if (any)
                selected.clear();
            else
                app.console.focusTerminal(); // the keyboard back to taw
        }
        else
        {
            // Everything else is taw's: space plays, ctrl+z undoes, shift+E closes this.
            const auto k = keyForTaw (kp);

            if (k.code == Key::None)
                return false;

            app.deferredKeys.push_back (k);
        }

        repaint();
        return true;
    }

    void zoomBy (double factor)
    {
        const auto middle = beatAt ((float) (keysW + getWidth()) * 0.5f);
        pxPerBeat = juce::jlimit (2.0, 800.0, pxPerBeat * factor);
        viewBeat = middle - visibleBeats() * 0.5;
    }

    //==============================================================================
    void timerCallback() override
    {
        if (app.session == nullptr || ! clip.isValid())
            return;

        // The view follows the playhead (as taw's does), unless you're dragging something.
        if (app.engine.isRolling() && drag == Drag::none)
        {
            const auto at = (double) app.heardPlayhead() / spb();

            if (at < viewBeat || at > viewBeat + visibleBeats())
                viewBeat = at - visibleBeats() * 0.1;
        }

        if (dirty || app.engine.isRolling() || app.cursor != lastCursor)
        {
            dirty = false;
            lastCursor = app.cursor;
            repaint();

            if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
                w->repaint (w->getTitleBarArea()); // its title names the clip, which may have moved
        }
    }

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { dirty = true; }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { dirty = true; }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { dirty = true; }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override { dirty = true; }
};

//==============================================================================
struct MidiEditor::Window final : EditorWindow<Roll>
{
    explicit Window (App& app) : EditorWindow<Roll> ("MIDI editor", new Roll (app), 1100, 640) {}
};

MidiEditor::MidiEditor (App& a) : app (a) {}
MidiEditor::~MidiEditor() { close(); }

void MidiEditor::open (const juce::ValueTree& clip)
{
    if (window == nullptr)
        window = std::make_unique<Window> (app);

    window->content->show (clip);
    window->showWithKeys();
}

void MidiEditor::close()
{
    if (window != nullptr && window->isActiveWindow())
        app.console.focusTerminal();

    window.reset();
}

bool MidiEditor::isOpen() const { return window != nullptr && window->isVisible(); }
juce::ValueTree MidiEditor::clip() const { return window != nullptr ? window->content->clip : juce::ValueTree(); }
void* MidiEditor::nativeHandle() const { return isOpen() ? window->getWindowHandle() : nullptr; }

void MidiEditor::poll()
{
    if (window == nullptr)
        return;

    // Closed with its button, or its clip gone (undone, deleted, the track removed).
    if (window->closeRequested || ! window->content->clip.getParent().isValid() || ! window->content->clip.getParent().getParent().isValid())
        close();
}
} // namespace taw
