// The audio editor window (see AudioEditor.h). One component draws the ruler and the track's
// clips (a name bar to drag them by, the waveform under it, per channel) and handles the mouse.
// Positions are timeline samples; the view is a start and a number of samples per pixel.

#include "AudioEditor.h"
#include "App.h"
#include "EditorWindow.h"

namespace taw
{
namespace
{
constexpr int toolbarH = 32, rulerH = 30, nameH = 18, edgeGrab = 5;

struct SnapChoice
{
    const char* name;
    double beats; // 0: off (to the sample); -1: a bar
};

const SnapChoice snapChoices[] = { { "snap: off (to the sample)", 0.0 }, { "snap: bar", -1.0 }, { "snap: beat", 1.0 },
                                   { "snap: 1/8", 0.5 },              { "snap: 1/16", 0.25 }, { "snap: 1/32", 0.125 } };
} // namespace

//==============================================================================
struct AudioEditor::Wave final : juce::Component, private juce::Timer, private juce::ValueTree::Listener
{
    explicit Wave (App& a) : app (a)
    {
        setWantsKeyboardFocus (true);

        for (int i = 0; i < (int) std::size (snapChoices); ++i)
            snapBox.addItem (snapChoices[i].name, i + 1);

        snapBox.setSelectedId (app.session != nullptr && app.session->gridOn() ? 5 : 1, juce::dontSendNotification);
        zeroButton.setTooltip ("selection edges and trims land on the nearest zero crossing");

        for (auto* c : { (juce::Component*) &snapBox, (juce::Component*) &zeroButton })
        {
            c->setWantsKeyboardFocus (false);
            addAndMakeVisible (c);
        }

        const auto& c = editorColours();
        snapBox.setColour (juce::ComboBox::backgroundColourId, c.panel);
        snapBox.setColour (juce::ComboBox::textColourId, c.text);
        snapBox.setColour (juce::ComboBox::outlineColourId, c.faint);
        snapBox.setColour (juce::ComboBox::arrowColourId, c.dim);
        zeroButton.setColour (juce::ToggleButton::textColourId, c.text);
        zeroButton.setColour (juce::ToggleButton::tickColourId, c.accent);
        zeroButton.setColour (juce::ToggleButton::tickDisabledColourId, c.dim);
        startTimerHz (30);
    }

    ~Wave() override { track.removeListener (this); }

    void show (const juce::ValueTree& clip)
    {
        track.removeListener (this);
        track = clip.getParent();
        track.addListener (this);
        focusClip = clip;

        if (const auto t = trackIndex(); t >= 0 && track.hasType (ids::TRACK))
            app.focusTrack (t); // taw's selection and keys act on this track (on a playlist lane, already on it)

        // Height to the track's loudest peak, so a quiet take isn't a flat line (alt+wheel changes it).
        float peak = 0.0f;

        for (const auto& c : clips())
            if (c.data != nullptr)
                for (const auto& p : c.data->overview)
                    peak = std::max ({ peak, -p.lo, p.hi });

        ampZoom = peak > 0.0f ? juce::jlimit (1.0, 64.0, 0.9 / peak) : 1.0;
        needsFrame = true;
        frame();
        repaint();
    }

    juce::String title() const
    {
        if (! track.isValid())
            return "audio editor";

        auto name = juce::String (trackIndex() + 1) + " " + track[ids::name].toString() + L" \x00b7 audio";

        if (focusClip.getParent() == track)
            name << L" \x00b7 " << focusClip[ids::file].toString().replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false);

        return name;
    }

    juce::ValueTree track;

private:
    App& app;
    juce::ComboBox snapBox;
    juce::ToggleButton zeroButton { "zero crossings" };
    juce::ValueTree focusClip;

    double viewStart = 0.0, spp = 256.0; // samples per pixel
    double ampZoom = 1.0;
    bool dirty = true, needsFrame = true;
    juce::int64 lastCursor = -1, lastAnchor = -2, lastHead = -2;
    juce::String hover;

    enum class Drag { none, ruler, select, move, trimStart, trimEnd };
    Drag drag = Drag::none;
    juce::ValueTree dragged;
    juce::int64 origStart = 0, origOffset = 0, origLength = 0;
    double downPos = 0.0;

    //==============================================================================
    int laneTop() const { return toolbarH + rulerH; }
    juce::Rectangle<int> laneArea() const { return { 0, laneTop(), getWidth(), getHeight() - laneTop() }; }
    float xOf (double pos) const { return (float) ((pos - viewStart) / spp); }
    double posAt (float x) const { return viewStart + x * spp; }
    double spb() const { return app.session->samplesPerBeat(); }

    int trackIndex() const
    {
        for (int i = 0; app.session != nullptr && i < app.session->numTracks(); ++i)
            if (app.session->track (i) == Session::trackOf (track))
                return i;

        return -1;
    }

    struct Shown
    {
        juce::ValueTree node;
        juce::int64 start, length, offset;
        const ClipData* data;
        juce::int64 end() const { return start + length; }
    };

    std::vector<Shown> clips() const
    {
        std::vector<Shown> out;

        for (const auto& c : Session::clips (track))
        {
            const auto it = app.clips.find (c[ids::file].toString());
            out.push_back ({ c, (juce::int64) c[ids::start], (juce::int64) c[ids::length], (juce::int64) c[ids::offset],
                             it != app.clips.end() ? &it->second : nullptr });
        }

        return out;
    }

    // The clip on top at a position (later clips play over earlier ones).
    std::optional<Shown> clipAt (double pos) const
    {
        const auto all = clips();

        for (auto it = all.rbegin(); it != all.rend(); ++it)
            if (pos >= (double) it->start && pos < (double) it->end())
                return *it;

        return std::nullopt;
    }

    void frame()
    {
        if (getWidth() <= 0 || ! focusClip.isValid())
            return;

        needsFrame = false;
        const auto len = (double) juce::jmax<juce::int64> (1, (juce::int64) focusClip[ids::length]);
        spp = juce::jlimit (0.02, 1.0e6, len * 1.1 / getWidth());
        viewStart = (double) (juce::int64) focusClip[ids::start] - len * 0.05;
    }

    //==============================================================================
    // Snapping: to the grid, or (zero crossings on) the nearest zero crossing.

    double gridSamples() const
    {
        const auto beats = snapChoices[juce::jlimit (0, (int) std::size (snapChoices) - 1, snapBox.getSelectedId() - 1)].beats;
        return beats < 0.0 ? spb() * juce::jmax (1, app.session->beatsPerBar()) : beats * spb();
    }

    juce::int64 snap (double pos, bool free, bool toZero) const
    {
        const auto g = gridSamples();
        auto p = (juce::int64) std::llround (g > 0.0 && ! free ? std::round (pos / g) * g : pos);

        if (toZero && zeroButton.getToggleState())
            p = zeroCrossingNear (p);

        return std::max<juce::int64> (0, p);
    }

    // The nearest sample where the clip there crosses zero (channels summed), within a few pixels.
    juce::int64 zeroCrossingNear (juce::int64 pos) const
    {
        const auto c = clipAt ((double) pos);

        if (! c || c->data == nullptr)
            return pos;

        const auto& audio = *c->data->audio;
        auto value = [&] (juce::int64 i)
        {
            float v = 0.0f;

            for (int ch = 0; ch < audio.getNumChannels(); ++ch)
                v += audio.getSample (ch, (int) i);

            return v;
        };

        const auto range = (juce::int64) juce::jlimit (8.0, 4096.0, spp * 8.0);
        const auto at = c->offset + (pos - c->start);
        const auto lo = std::max<juce::int64> (c->offset, at - range), hi = std::min<juce::int64> (c->offset + c->length - 1, at + range);

        for (juce::int64 d = 0; d <= range; ++d)
            for (const auto i : { at - d, at + d })
                if (i > lo && i <= hi && ((value (i - 1) <= 0.0f) != (value (i) <= 0.0f)))
                    return c->start + (i - c->offset);

        return pos;
    }

    //==============================================================================
    void resized() override
    {
        auto bar = getLocalBounds().removeFromTop (toolbarH).reduced (6, 4);
        snapBox.setBounds (bar.removeFromLeft (220));
        bar.removeFromLeft (10);
        zeroButton.setBounds (bar.removeFromLeft (140));

        if (needsFrame)
            frame();
    }

    void paint (juce::Graphics& g) override
    {
        if (app.session == nullptr || ! track.isValid())
            return;

        const auto& c = editorColours();
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain)));
        g.fillAll (c.bg);

        // Toolbar: what's under the mouse, then what the mouse does.
        g.setColour (c.panel);
        g.fillRect (0, 0, getWidth(), toolbarH);
        g.setColour (c.dim);
        const auto height = ampZoom > 1.01 ? L"height \x00d7" + juce::String (ampZoom, ampZoom < 10.0 ? 1 : 0) + L" \x00b7 " : juce::String();
        g.drawText (height + (hover.isNotEmpty() ? hover
                                                 : juce::String (L"drag: select (taw's keys act on it) \x00b7 edges: trim \x00b7 name bar: move (alt: off the grid) \x00b7 double-click: the clip \x00b7 ctrl+wheel: zoom \x00b7 alt+wheel: height")),
                    zeroButton.getRight() + 12, 0, getWidth() - zeroButton.getRight() - 18, toolbarH, juce::Justification::centredLeft, true);

        const auto lane = laneArea();
        const auto bpb = (double) juce::jmax (1, app.session->beatsPerBar());
        const auto first = posAt (0.0f), last = posAt ((float) getWidth());

        // Ruler: bar numbers, and beat ticks when there's room.
        g.setColour (c.panel);
        g.fillRect (0, toolbarH, getWidth(), rulerH);
        const auto pxPerBeat = spb() / spp;
        const auto everyBars = pxPerBeat * bpb < 50.0 ? std::ceil (50.0 / (pxPerBeat * bpb)) : 1.0;

        for (auto b = std::floor (first / spb()); b * spb() <= last; b += 1.0)
        {
            const auto x = xOf (b * spb());
            const bool barLine = std::fmod (b, bpb) == 0.0;
            const auto bar = std::floor (b / bpb);

            if (barLine && std::fmod (bar, everyBars) == 0.0)
            {
                g.setColour (c.text);
                g.fillRect (x, (float) toolbarH, 1.0f, 12.0f);
                g.drawText (juce::String ((int) bar + 1), (int) x + 3, toolbarH, 50, 14, juce::Justification::centredLeft, false);
            }
            else if (! barLine && pxPerBeat >= 10.0)
            {
                g.setColour (c.dim);
                g.fillRect (x, (float) toolbarH + 8.0f, 1.0f, 5.0f);
            }
        }

        // The grid, down a strip of the lane: the snap grid faintly (when its lines are far enough
        // apart), beats, then bars. Drawn in the gaps and again over each clip's background, under
        // its waveform, so it shows through the audio.
        const auto snapStep = gridSamples();

        auto drawGrid = [&] (juce::Rectangle<float> strip)
        {
            if (snapStep > 0.0 && snapStep / spp >= 8.0 && snapStep < spb())
                for (auto s = std::ceil (posAt (strip.getX()) / snapStep) * snapStep; s <= posAt (strip.getRight()); s += snapStep)
                {
                    g.setColour (c.faint.withAlpha (0.5f));
                    g.fillRect (xOf (s), strip.getY(), 1.0f, strip.getHeight());
                }

            for (auto b = std::floor (posAt (strip.getX()) / spb()); b * spb() <= posAt (strip.getRight()); b += 1.0)
            {
                const bool barLine = std::fmod (b, bpb) == 0.0;

                if (barLine && std::fmod (std::floor (b / bpb), everyBars) == 0.0)
                    g.setColour (c.bar);
                else if (! barLine && pxPerBeat >= 10.0)
                    g.setColour (c.faint);
                else
                    continue;

                g.fillRect (xOf (b * spb()), strip.getY(), 1.0f, strip.getHeight());
            }
        };

        drawGrid (lane.toFloat());

        // Zoomed right in: every sample gets a tick on the ruler.
        if (spp < 0.25)
        {
            g.setColour (c.faint);

            for (auto s = std::floor (first); s <= last; s += 1.0)
                g.fillRect (xOf (s), (float) toolbarH + rulerH - 5.0f, 1.0f, 4.0f);
        }

        // The clips, in order (later ones on top), each a name bar over its waveform.
        juce::Graphics::ScopedSaveState clipLane (g);
        g.reduceClipRegion (lane);

        for (const auto& clip : clips())
        {
            const auto x0 = std::max (-1.0f, xOf ((double) clip.start)), x1 = std::min ((float) getWidth() + 1.0f, xOf ((double) clip.end()));

            if (x1 <= 0.0f || x0 >= (float) getWidth())
                continue;

            const bool focus = clip.node == focusClip;
            const juce::Rectangle<float> body (x0, (float) lane.getY(), x1 - x0, (float) lane.getHeight());
            g.setColour (c.clip);
            g.fillRect (body);
            drawGrid (body.withTrimmedTop ((float) nameH));
            g.setColour (focus ? c.accent.withAlpha (0.55f) : c.faint);
            g.fillRect (body.withHeight ((float) nameH));
            g.setColour (focus ? c.bg : c.text);
            g.drawText (clip.node[ids::file].toString().replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false),
                        body.withHeight ((float) nameH).reduced (5.0f, 0.0f), juce::Justification::centredLeft, true);
            g.setColour (focus ? c.accent : c.dim);
            g.drawRect (body, 1.0f);

            if (clip.data != nullptr)
                drawWave (g, clip, body.withTrimmedTop ((float) nameH + 2.0f).withTrimmedBottom (4.0f));
        }

        // taw's time selection, over the clips.
        if (app.hasTimeSelection())
        {
            const auto sel = app.timeSelection();
            const auto x0 = xOf ((double) sel.getStart()), x1 = xOf ((double) sel.getEnd());
            g.setColour (c.selection.withAlpha (0.45f));
            g.fillRect (juce::Rectangle<float> (x0, (float) lane.getY(), std::max (1.0f, x1 - x0), (float) lane.getHeight()));
        }

        auto line = [&] (juce::int64 pos, juce::Colour colour, float w)
        {
            g.setColour (colour);
            g.fillRect (xOf ((double) pos), (float) toolbarH, w, (float) (getHeight() - toolbarH));
        };

        if (! app.engine.isRolling() || app.cursor != app.heardPlayhead())
            line (app.cursor, c.accent, 1.0f);

        if (app.engine.isRolling())
            line (app.heardPlayhead(), app.recording ? c.rec : c.play, 2.0f);
    }

    // One clip's waveform, per channel: lines through the samples when they're far enough apart
    // to see, else a column per pixel from the lowest sample to the highest (the overview when a
    // pixel covers more than an overview block).
    void drawWave (juce::Graphics& g, const Shown& clip, juce::Rectangle<float> r) const
    {
        const auto& c = editorColours();
        const auto& audio = *clip.data->audio;
        const int channels = audio.getNumChannels();
        const auto bandH = r.getHeight() / (float) channels;
        const auto fileLen = (juce::int64) audio.getNumSamples();

        for (int ch = 0; ch < channels; ++ch)
        {
            const auto band = r.withY (r.getY() + ch * bandH).withHeight (bandH);
            const auto mid = band.getCentreY(), half = band.getHeight() * 0.48f * (float) ampZoom;
            auto yOf = [&] (float v) { return juce::jlimit (band.getY(), band.getBottom(), mid - v * half); };

            g.setColour (c.faint);
            g.fillRect (band.getX(), mid, band.getWidth(), 1.0f); // the zero line

            const auto xa = std::max (0.0f, band.getX()), xb = std::min ((float) getWidth(), band.getRight());
            g.setColour (c.wave);

            if (spp <= 1.0)
            {
                // Each sample: a line through them, dots once they're well apart.
                const auto from = std::max (clip.offset, clip.offset + (juce::int64) std::floor (posAt (xa) - (double) clip.start) - 1);
                const auto to = std::min ({ fileLen - 1, clip.offset + clip.length - 1, clip.offset + (juce::int64) std::ceil (posAt (xb) - (double) clip.start) + 1 });
                juce::Path path;

                for (auto i = from; i <= to; ++i)
                {
                    const auto x = xOf ((double) (clip.start + (i - clip.offset)) + 0.5);
                    const auto y = yOf (audio.getSample (ch, (int) i));

                    if (i == from)
                        path.startNewSubPath (x, y);
                    else
                        path.lineTo (x, y);

                    if (spp < 0.12)
                        g.fillEllipse (x - 2.5f, y - 2.5f, 5.0f, 5.0f);
                }

                g.strokePath (path, juce::PathStrokeType (1.2f));
                continue;
            }

            const bool useOverview = spp >= (double) Recorder::overviewBlock && ! clip.data->overview.empty();

            for (auto x = std::floor (xa); x < xb; x += 1.0f)
            {
                const auto t0 = posAt (x) - (double) clip.start, t1 = t0 + spp;
                const auto a = std::max<juce::int64> (0, (juce::int64) std::floor (t0)) + clip.offset;
                const auto b = std::min<juce::int64> (clip.length, (juce::int64) std::ceil (t1)) + clip.offset;

                if (b <= a || a >= fileLen)
                    continue;

                float lo = 0.0f, hi = 0.0f;

                if (useOverview)
                {
                    const auto& ov = clip.data->overview;
                    const auto ia = (size_t) (a / Recorder::overviewBlock), ib = std::min (ov.size(), (size_t) ((b + Recorder::overviewBlock - 1) / Recorder::overviewBlock));

                    for (auto i = ia; i < ib; ++i)
                        lo = std::min (lo, ov[i].lo), hi = std::max (hi, ov[i].hi);
                }
                else
                {
                    const auto range = audio.findMinMax (ch, (int) a, (int) (std::min (b, fileLen) - a));
                    lo = range.getStart(), hi = range.getEnd();
                }

                const auto y0 = yOf (hi), y1 = yOf (lo);
                g.fillRect (x, y0, 1.0f, std::max (1.0f, y1 - y0));
            }
        }
    }

    //==============================================================================
    void mouseMove (const juce::MouseEvent& e) override
    {
        if (app.session == nullptr)
            return;

        const auto pos = std::max (0.0, posAt (e.position.x));
        hover = app.formatBars ((juce::int64) pos) + L" \x00b7 " + app.formatTime ((juce::int64) pos) + L" \x00b7 sample " + juce::String ((juce::int64) pos);
        auto shape = juce::MouseCursor::NormalCursor;

        if (e.y >= laneTop())
            if (const auto c = clipAt (pos))
            {
                if (std::abs (e.position.x - xOf ((double) c->start)) <= edgeGrab || std::abs (e.position.x - xOf ((double) c->end())) <= edgeGrab)
                    shape = juce::MouseCursor::LeftRightResizeCursor;
                else if (e.y < laneTop() + nameH)
                    shape = juce::MouseCursor::DraggingHandCursor;
                else
                    shape = juce::MouseCursor::IBeamCursor;
            }

        setMouseCursor (shape);
        repaint (0, 0, getWidth(), toolbarH);
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hover.clear();
        repaint (0, 0, getWidth(), toolbarH);
    }

    // The clip whose edge is under x (the nearer edge of the nearest one), and which edge.
    std::optional<std::pair<Shown, bool>> edgeAt (float x) const
    {
        std::optional<std::pair<Shown, bool>> best;
        float bestD = (float) edgeGrab + 0.5f;

        for (const auto& c : clips())
            for (const bool end : { false, true })
                if (const auto d = std::abs (x - xOf ((double) (end ? c.end() : c.start))); d < bestD)
                {
                    bestD = d;
                    best = std::make_pair (c, end);
                }

        return best;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();

        if (app.session == nullptr || ! track.isValid())
            return;

        if (const auto t = trackIndex(); t >= 0 && app.focused != t)
            app.focusTrack (t);

        const bool free = e.mods.isAltDown();
        downPos = posAt (e.position.x);
        drag = Drag::none;

        if (e.y >= toolbarH && e.y < laneTop())
        {
            drag = Drag::ruler;
            app.moveTo (snap (downPos, free, false));
            repaint();
            return;
        }

        if (e.y < laneTop())
            return;

        // An edge: trim. The name bar: move. Anywhere else: select, from here.
        if (const auto edge = edgeAt (e.position.x))
        {
            dragged = edge->first.node;
            drag = edge->second ? Drag::trimEnd : Drag::trimStart;
        }
        else if (const auto c = clipAt (downPos); c && e.y < laneTop() + nameH)
        {
            dragged = c->node;
            drag = Drag::move;
        }
        else
        {
            drag = Drag::select;
            app.moveTo (snap (downPos, free, true));
        }

        if (dragged.isValid())
        {
            origStart = dragged[ids::start];
            origOffset = dragged[ids::offset];
            origLength = dragged[ids::length];
            focusClip = dragged;
            app.beginEdit (drag == Drag::move ? "move clip" : "trim clip");
        }

        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (app.session == nullptr || ! track.isValid())
            return;

        const bool free = e.mods.isAltDown();
        const auto pos = posAt (e.position.x);
        const auto fileLen = [&]
        {
            const auto it = app.clips.find (dragged[ids::file].toString());
            return it != app.clips.end() ? (juce::int64) it->second.audio->getNumSamples() : origOffset + origLength;
        };

        switch (drag)
        {
            case Drag::ruler:
                app.moveTo (snap (pos, free, false));
                break;

            case Drag::select:
                // taw's own selection, so its keys act on exactly this.
                app.moveTo (snap (pos, free, true), true);
                break;

            case Drag::move:
            {
                const auto start = std::max<juce::int64> (0, snap ((double) origStart + (pos - downPos), free, false));
                dragged.setProperty (ids::start, start, app.um());
                break;
            }

            case Drag::trimStart:
            {
                // Not before the file's start (or zero), not past the clip's end.
                const auto end = origStart + origLength;
                const auto lowest = std::max<juce::int64> (0, origStart - origOffset);
                const auto start = juce::jlimit (lowest, end - 1, snap (pos, free, true));
                dragged.setProperty (ids::start, start, app.um())
                    .setProperty (ids::offset, origOffset + (start - origStart), app.um())
                    .setProperty (ids::length, end - start, app.um());
                break;
            }

            case Drag::trimEnd:
            {
                const auto end = juce::jlimit (origStart + 1, origStart + (fileLen() - origOffset), snap (pos, free, true));
                dragged.setProperty (ids::length, end - origStart, app.um());
                break;
            }

            case Drag::none:
                break;
        }

        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        // A moved clip takes its automation with it (in the same undo step).
        if (drag == Drag::move && dragged.isValid() && (juce::int64) dragged[ids::start] != origStart && app.session != nullptr)
        {
            const auto spb = app.session->samplesPerBeat();
            if (track.hasType (ids::TRACK)) // a playlist's clip: the automation is the track's, and stays
                app.moveAutomation (track, (double) origStart / spb, (double) origLength / spb, (double) (juce::int64) dragged[ids::start] / spb);
        }

        drag = Drag::none;
        dragged = {};
        repaint();
    }

    // Double-click a clip: select all of it.
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (app.session == nullptr || e.y < laneTop())
            return;

        if (const auto c = clipAt (posAt (e.position.x)))
        {
            app.timeAnchor = c->start;
            app.timeHead = c->end();
            focusClip = c->node;

            if (! app.engine.isRolling())
                app.setCursor (c->start);

            repaint();
        }
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const auto dy = w.deltaY != 0.0f ? w.deltaY : w.deltaX;

        if (ctrlOrCmd (e.mods))
            zoomAround (e.position.x, std::pow (1.25, -dy * 4.0));
        else if (e.mods.isAltDown())
            ampZoom = juce::jlimit (0.25, 64.0, ampZoom * std::pow (1.25, dy * 4.0));
        else
            viewStart -= dy * getWidth() * spp * 0.25;

        viewStart = std::max (-getWidth() * spp * 0.5, viewStart);
        repaint();
    }

    void zoomAround (float x, double factor)
    {
        const auto at = posAt (x);
        spp = juce::jlimit (0.02, 1.0e6, spp * factor);
        viewStart = at - x * spp;
    }

    bool keyPressed (const juce::KeyPress& kp) override
    {
        if (app.session == nullptr)
            return false;

        const auto ch = kp.getTextCharacter();
        const auto mods = kp.getModifiers();

        // + and - zoom around taw's cursor when it's in view, else the middle.
        const auto cursorX = xOf ((double) app.cursor);
        const auto around = cursorX >= 0.0f && cursorX <= (float) getWidth() ? cursorX : (float) getWidth() * 0.5f;

        if ((ch == '+' || ch == '=') && ! ctrlOrCmd (mods))
            zoomAround (around, 1.0 / 1.5);
        else if (ch == '-' && ! ctrlOrCmd (mods))
            zoomAround (around, 1.5);
        else if (kp.getKeyCode() == juce::KeyPress::escapeKey)
            app.console.focusTerminal(); // the keyboard back to taw
        else
        {
            // Everything else is taw's: delete, ctrl+x / c / v / d, space, l, ctrl+z, shift+E.
            const auto k = keyForTaw (kp);

            if (k.code == Key::None)
                return false;

            app.deferredKeys.push_back (k);
        }

        repaint();
        return true;
    }

    //==============================================================================
    void timerCallback() override
    {
        if (app.session == nullptr || ! track.isValid())
            return;

        if (app.engine.isRolling() && drag == Drag::none)
        {
            const auto at = (double) app.heardPlayhead();

            if (at < viewStart || at > viewStart + getWidth() * spp)
                viewStart = at - getWidth() * spp * 0.1;
        }

        if (dirty || app.engine.isRolling() || app.cursor != lastCursor || app.timeAnchor != lastAnchor || app.timeHead != lastHead)
        {
            dirty = false;
            lastCursor = app.cursor;
            lastAnchor = app.timeAnchor;
            lastHead = app.timeHead;
            repaint();
        }
    }

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { dirty = true; }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { dirty = true; }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { dirty = true; }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override { dirty = true; }
};

//==============================================================================
struct AudioEditor::Window final : EditorWindow<Wave>
{
    explicit Window (App& app) : EditorWindow<Wave> ("audio editor", new Wave (app), 1100, 520) {}
};

AudioEditor::AudioEditor (App& a) : app (a) {}
AudioEditor::~AudioEditor() { close(); }

void AudioEditor::open (const juce::ValueTree& clip)
{
    if (window == nullptr)
        window = std::make_unique<Window> (app);

    window->content->show (clip);
    window->showWithKeys();
}

void AudioEditor::close()
{
    if (window != nullptr && window->isActiveWindow())
        app.console.focusTerminal();

    window.reset();
}

bool AudioEditor::isOpen() const { return window != nullptr && window->isVisible(); }
juce::ValueTree AudioEditor::track() const { return window != nullptr ? window->content->track : juce::ValueTree(); }
void* AudioEditor::nativeHandle() const { return isOpen() ? window->getWindowHandle() : nullptr; }

void AudioEditor::poll()
{
    if (window == nullptr)
        return;

    // Closed with its button, or its track gone (undone, removed) or no longer audio.
    const auto t = window->content->track;

    if (window->closeRequested || ! t.getParent().isValid() || ! Session::trackOf (t).getParent().isValid() || ! Session::isAudioTrack (Session::trackOf (t)))
        close();
}
} // namespace taw
