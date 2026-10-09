// The session screen's bounces: last-saved.mp3 bouncing in the background after a close, and
// the session screen's bounces list, which shows what taw bounced (newest first) and plays them
// without opening anything.

#include "App.h"

namespace taw
{
void App::pollBackgroundBounces()
{
    bool finished = false;

    for (size_t i = 0; i < backgroundBounces.size();)
    {
        auto& b = backgroundBounces[i];

        if (! b->isDone())
        {
            ++i;
            continue;
        }

        const auto name = backgroundBounceNames[i];

        if (b->succeeded())
        {
            // The folder is the session's: last-saved lives in its bounce folder.
            auto folder = b->file().getParentDirectory();

            while (folder != folder.getParentDirectory() && ! Session::existsIn (folder))
                folder = folder.getParentDirectory();

            rememberBounce (b->file(), name, folder);

            if (session == nullptr)
                say (Result::good (name + L" \x00b7 " + b->file().getFileName() + L" bounced"));
        }
        else if (! b->wasCancelled())
        {
            say (Result::fail (name + L" \x00b7 the last-saved bounce failed: " + b->errorMessage()));
        }

        backgroundBounces.erase (backgroundBounces.begin() + (std::ptrdiff_t) i);
        backgroundBounceNames.erase (backgroundBounceNames.begin() + (std::ptrdiff_t) i);
        finished = true;
    }

    if (finished && session == nullptr)
    {
        loadRecentBounces();

        if (bouncePlayer == nullptr)
            bounceIndex = 0; // the new one, at the top
    }

    if (quitAfterBounces && backgroundBounces.empty())
        quitRequested = true;

    // The bounce player: decoded, so play it from where it was asked to start.
    if (bouncePlayer != nullptr && ! bouncePlayer->started && bouncePlayer->loaded.load())
    {
        bouncePlayer->started = true;

        if (bouncePlayer->error.isNotEmpty())
        {
            say (Result::fail (bouncePlayer->error));
            bouncePlayer.reset();
        }
        else
        {
            engine.preview (bouncePlayer->audio, 1.0f, (int) (bouncePlayer->startAt * bouncePlayer->rate));
        }
    }
}

// What taw bounced: the ones it remembers, and whatever sits in the recent sessions' bounce
// folders (from before it remembered, or bounced elsewhere).
void App::loadRecentBounces()
{
    const auto selected = bounceIndex < (int) recentBounces.size() ? recentBounces[(size_t) bounceIndex].file : juce::File();
    recentBounces.clear();

    auto add = [this] (const juce::File& f, const juce::String& name, const juce::File& folder)
    {
        if (f.getFileName().contains (".part."))
            return;

        for (const auto& b : recentBounces)
            if (b.file == f)
                return;

        recentBounces.push_back ({ f, name, folder, f.getLastModificationTime() });
    };

    for (const auto& r : loadRecentBounceRecords())
        add (r.file, r.session, r.folder);

    for (const auto& dir : recent)
        for (const auto& f : dir.getChildFile ("bounces").findChildFiles (juce::File::findFiles, false, "*.mp3;*.wav"))
            add (f, dir.getFileName(), dir);

    std::stable_sort (recentBounces.begin(), recentBounces.end(), [] (const auto& a, const auto& b) { return a.time > b.time; });

    if (recentBounces.size() > 40)
        recentBounces.resize (40);

    bounceIndex = 0;

    for (int i = 0; i < (int) recentBounces.size(); ++i)
        if (recentBounces[(size_t) i].file == selected)
            bounceIndex = i;
}

void App::handleBouncesKey (const Key& k)
{
    const auto n = (int) recentBounces.size();
    const auto c = k.code == Key::Char ? juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) k.ch) : 0;
    const auto* sel = bounceIndex < n ? &recentBounces[(size_t) bounceIndex] : nullptr;
    const bool playingSel = sel != nullptr && bouncePlayer != nullptr && bouncePlayer->file == sel->file && bouncePlaySeconds() >= 0.0;

    switch (k.code)
    {
        case Key::Up:
            if (bounceIndex == 0)
                movePickArea (PickArea::templates); // off the top: the area above
            else
                --bounceIndex;
            return;
        case Key::Down:  bounceIndex = juce::jmax (0, juce::jmin (n - 1, bounceIndex + 1)); return;
        case Key::Left:  seekBounce (bouncePlaySeconds() - (k.shift ? 30.0 : 5.0)); return;
        case Key::Right: seekBounce (bouncePlaySeconds() + (k.shift ? 30.0 : 5.0)); return;
        case Key::Home:  seekBounce (0.0); return;
        case Key::Enter:
            if (sel != nullptr)
                playingSel ? stopBounce() : playBounce (sel->file);
            return;
        default:
            break;
    }

    if (c == ' ' && sel != nullptr)
        playingSel ? stopBounce() : playBounce (sel->file);
    else if (c == 'o' && sel != nullptr && opening == nullptr)
    {
        if (! Session::existsIn (sel->folder))
            say (Result::fail ("can't find the session " + sel->session + " came from"));
        else
            startOpening (sel->folder);
    }
    else if (c == 'r' && sel != nullptr)
        sel->file.revealToUser();
    else if (c >= 32)
    {
        // Anything else starts a new session's name, like it does from the sessions.
        newSessionName += juce::String::charToString ((juce::juce_wchar) k.ch);
        pickArea = PickArea::templates;
    }
}

void App::playBounce (const juce::File& file)
{
    stopBounce();

    auto p = std::make_unique<BouncePlayer>();
    p->file = file;
    p->rate = engine.deviceSampleRate() > 0.0 ? engine.deviceSampleRate() : 44100.0;
    auto* raw = p.get();

    // Decoding an MP3 of a whole song takes a moment: off the message thread.
    p->loader = std::thread ([raw]
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (raw->file));

        if (reader == nullptr || reader->lengthInSamples <= 0)
        {
            raw->error = "can't read " + raw->file.getFileName();
            raw->loaded.store (true);
            return;
        }

        const double fileRate = reader->sampleRate;
        const auto n = (int) juce::jmin<juce::int64> (reader->lengthInSamples, (juce::int64) (30.0 * 60.0 * fileRate));
        juce::AudioBuffer<float> in (2, n);
        reader->read (&in, 0, n, 0, true, true);

        if (reader->numChannels == 1)
            in.copyFrom (1, 0, in, 0, 0, n);

        if (std::abs (raw->rate - fileRate) < 0.5)
        {
            raw->audio = std::make_shared<juce::AudioBuffer<float>> (std::move (in));
        }
        else
        {
            auto out = std::make_shared<juce::AudioBuffer<float>> (2, (int) std::ceil (n * raw->rate / fileRate) + 1);
            out->clear();

            for (int ch = 0; ch < 2; ++ch)
            {
                juce::LagrangeInterpolator resampler;
                resampler.process (fileRate / raw->rate, in.getReadPointer (ch), out->getWritePointer (ch), out->getNumSamples() - 1, n, 0);
            }

            raw->audio = std::move (out);
        }

        raw->loaded.store (true);
    });

    bouncePlayer = std::move (p);
}

void App::stopBounce()
{
    if (bouncePlayer == nullptr)
        return;

    engine.preview (nullptr);
    bouncePlayer.reset(); // waits for its decoding, if it's still at it
}

void App::seekBounce (double seconds)
{
    if (bouncePlayer == nullptr)
        return;

    seconds = juce::jlimit (0.0, juce::jmax (0.0, bounceLengthSeconds() - 0.5), seconds);

    if (! bouncePlayer->started)
        bouncePlayer->startAt = seconds;
    else if (bouncePlayer->audio != nullptr)
        engine.preview (bouncePlayer->audio, 1.0f, (int) (seconds * bouncePlayer->rate));
}

double App::bouncePlaySeconds() const
{
    if (bouncePlayer == nullptr)
        return -1.0;

    if (! bouncePlayer->started)
        return bouncePlayer->startAt;

    const auto at = engine.previewPosition();
    return at < 0 ? -1.0 : at / bouncePlayer->rate;
}

double App::bounceLengthSeconds() const
{
    if (bouncePlayer == nullptr || ! bouncePlayer->started || bouncePlayer->audio == nullptr)
        return 0.0;

    return bouncePlayer->audio->getNumSamples() / bouncePlayer->rate;
}
} // namespace taw
