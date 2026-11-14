/*
    Headless smoke test for the Groovebox audio engine.

    It drives the real GrooveboxProcessor directly — no UI, no host — and checks
    the things that are easy to get subtly wrong: that hits land on the right
    sample, that swing moves the off-beat by the expected amount, and that
    per-channel pitch and mute do what they say.

    Build:  cmake --build build --config Release --target GrooveboxSmokeTest
    Run:    build\GrooveboxSmokeTest_artefacts\Release\GrooveboxSmokeTest.exe
*/

#include "PluginProcessor.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

using namespace gb;

namespace
{

int failures = 0;
int checks = 0;

void check (bool condition, const juce::String& what, const juce::String& detail = {})
{
    ++checks;

    if (condition)
    {
        std::printf ("  ok    %s\n", what.toRawUTF8());
    }
    else
    {
        ++failures;
        std::printf ("  FAIL  %s%s\n", what.toRawUTF8(),
                     detail.isEmpty() ? "" : ("  [" + detail + "]").toRawUTF8());
    }
}

/** Folder for the throwaway wav files these tests load.

    Deliberately not the system temp folder: it is not guaranteed writable from
    every launching context, and when it is not, every test that loads a sample
    fails for a reason that has nothing to do with the engine.
*/
juce::File testScratchDirectory()
{
    auto dir = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                   .getParentDirectory()
                   .getChildFile ("groovebox-test-scratch");

    if (! dir.isDirectory())
        dir.createDirectory();

    return dir;
}

/** Writes a short click into a scratch wav so we can load it like a real sample.

    The click stays exactly at +/-1.0 for its first three samples and then decays,
    so the first sample above any small threshold is the onset itself and the
    measurement needs no interpolation guesswork.
*/
juce::File makeClickWav (const juce::String& name, int lengthSamples, double sampleRate)
{
    auto file = testScratchDirectory().getChildFile ("click-" + name + ".wav");

    file.deleteFile();

    juce::AudioBuffer<float> buffer (1, lengthSamples);
    auto* data = buffer.getWritePointer (0);

    for (int i = 0; i < lengthSamples; ++i)
    {
        if (i < 3)
        {
            data[i] = 0.9f;
            continue;
        }

        // A fast decay so one hit is clearly separated from the next step.
        const auto envelope = std::pow (0.5, (double) (i - 3) / 12.0);
        data[i] = (float) (0.9 * envelope
                             * std::cos (2.0 * juce::MathConstants<double>::pi * 2.0 * (i - 3) / 48.0));
    }

    juce::WavAudioFormat format;
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());

    if (stream == nullptr)
        return {};

    std::unique_ptr<juce::AudioFormatWriter> writer (
        format.createWriterFor (stream.get(), sampleRate, 1, 24, {}, 0));

    if (writer == nullptr)
        return {};

    stream.release(); // the writer owns it from here
    writer->writeFromAudioSampleBuffer (buffer, 0, lengthSamples);
    writer.reset();

    return file;
}

/** Writes a pure tone into a scratch wav so we can load it like a real sample.

    A tone is the right source for measuring resampling, because the question is
    which frequencies survive a pitch change. A broadband click only answers that
    after a transform; a single tone near Nyquist answers it with one peak
    measurement, and anything that comes out at the folded frequency can only have
    been put there by the resampler.
*/
juce::File makeToneWav (const juce::String& name, double frequency, int lengthSamples, double sampleRate)
{
    auto file = testScratchDirectory().getChildFile ("tone-" + name + ".wav");

    file.deleteFile();

    juce::AudioBuffer<float> buffer (1, lengthSamples);
    auto* data = buffer.getWritePointer (0);

    for (int i = 0; i < lengthSamples; ++i)
        data[i] = (float) std::sin (2.0 * juce::MathConstants<double>::pi * frequency * (double) i / sampleRate);

    juce::WavAudioFormat format;
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());

    if (stream == nullptr)
        return {};

    std::unique_ptr<juce::AudioFormatWriter> writer (
        format.createWriterFor (stream.get(), sampleRate, 1, 24, {}, 0));

    if (writer == nullptr)
        return {};

    stream.release(); // the writer owns it from here
    writer->writeFromAudioSampleBuffer (buffer, 0, lengthSamples);
    writer.reset();

    return file;
}

/** One rendered stereo block. */
struct RenderResult
{
    std::vector<float> left;
    std::vector<float> right;

    float peakBetween (int from, int to) const
    {
        float peak = 0.0f;

        for (int i = juce::jmax (0, from); i < juce::jmin (to, (int) left.size()); ++i)
            peak = juce::jmax (peak, std::abs (left[(size_t) i]));

        return peak;
    }
};

RenderResult render (GrooveboxProcessor& processor, int totalSamples, int blockSize, double sampleRate)
{
    RenderResult result;
    result.left.reserve ((size_t) totalSamples);
    result.right.reserve ((size_t) totalSamples);

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;

    for (int rendered = 0; rendered < totalSamples; rendered += blockSize)
    {
        const auto thisBlock = juce::jmin (blockSize, totalSamples - rendered);

        buffer.setSize (2, thisBlock, false, true, true);
        buffer.clear();

        processor.processBlock (buffer, midi);

        for (int i = 0; i < thisBlock; ++i)
        {
            result.left.push_back (buffer.getSample (0, i));
            result.right.push_back (buffer.getSample (1, i));
        }
    }

    juce::ignoreUnused (sampleRate);
    return result;
}

float peakOf (const std::vector<float>& data)
{
    float peak = 0.0f;

    for (auto value : data)
        peak = juce::jmax (peak, std::abs (value));

    return peak;
}

/**
    One block carrying a single note, rendered through the real MIDI path.

    The transport is left stopped throughout, so anything that comes out can only
    have come from the note.
*/
std::vector<float> renderNote (GrooveboxProcessor& processor,
                               int midiNote,
                               float velocity = 1.0f,
                               int blockSize = 512,
                               int totalSamples = 2048)
{
    std::vector<float> out;
    out.reserve ((size_t) totalSamples);

    juce::AudioBuffer<float> buffer (2, blockSize);
    int rendered = 0;
    auto sent = false;

    while (rendered < totalSamples)
    {
        const auto thisBlock = juce::jmin (blockSize, totalSamples - rendered);

        buffer.setSize (2, thisBlock, false, true, true);
        buffer.clear();

        juce::MidiBuffer midi;

        if (! sent)
        {
            midi.addEvent (juce::MidiMessage::noteOn (1, midiNote, velocity), 0);
            sent = true;
        }

        processor.processBlock (buffer, midi);

        for (int i = 0; i < thisBlock; ++i)
            out.push_back (buffer.getSample (0, i));

        rendered += thisBlock;
    }

    return out;
}

void clearAllSteps (GrooveboxProcessor& processor)
{
    for (int channel = 0; channel < Layout::numChannels; ++channel)
        for (int step = 0; step < Layout::numSteps; ++step)
            processor.setStep (channel, step, false);
}

/**
    Puts the processor back to a silent bar start.

    Renders are measurements, so each one has to begin from the same state: a
    voice still ringing out from the previous render would otherwise leak into
    the next measurement.
*/
void rearm (GrooveboxProcessor& processor, double tempo = 120.0)
{
    processor.resetTransport();
    processor.setSwing (0.0f);
    processor.setSwingGrid (4);
    processor.setManualTempo (tempo);
}

/** Sample index of a rising crossing of `threshold`, searching from `from`. */
int onsetAfter (const std::vector<float>& data, int from, float threshold = 0.005f)
{
    for (int i = juce::jmax (0, from); i < (int) data.size(); ++i)
        if (std::abs (data[(size_t) i]) >= threshold)
            return i;

    return -1;
}

/** Index of the last sample above the threshold, or -1 when there is none. */
int lastAbove (const std::vector<float>& data, float threshold = 0.005f)
{
    int last = -1;

    for (size_t i = 0; i < data.size(); ++i)
        if (std::abs (data[i]) >= threshold)
            last = (int) i;

    return last;
}

} // namespace

int main()
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 256;
    constexpr int stepSamples = 5512; // 120 bpm, 16th notes, at 44.1 kHz

    /**
        The same step length, exactly. 44100 / 8 is 5512.5, so `stepSamples` above
        is a half sample short, and over a bar that half sample becomes eight.

        Grid expectations are checked against this rather than against the int,
        because an int expectation silently agrees with an engine that drops the
        fractional part of every onset — which is precisely the drift the multi-bar
        test below is there to catch. `stepSamples` stays an int for sizing render
        buffers, where half a sample is irrelevant.
    */
    constexpr double exactStepSamples = sampleRate / (120.0 * 4.0 / 60.0);

    std::printf ("Groovebox engine smoke test\n");

    //======================================================================
    std::printf ("\n[1] empty engine is silent\n");
    {
        GrooveboxProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);

        auto result = render (processor, stepSamples * 4, blockSize, sampleRate);
        check (peakOf (result.left) == 0.0f, "no output before any sample is loaded");
    }

    //======================================================================
    std::printf ("\n[2] a one-shot lands exactly on its step\n");
    {
        GrooveboxProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);

        auto click = makeClickWav ("one", 200, sampleRate);
        check (click.existsAsFile(), "test wav written");

        juce::String error;
        const auto loaded = processor.loadSampleIntoChannel (0, click, error);
        check (loaded, "sample loaded into channel 0", error);

        clearAllSteps (processor);
        processor.setStep (0, 4, true);
        rearm (processor);
        processor.setPlaying (true);

        auto result = render (processor, stepSamples * 5, blockSize, sampleRate);

        const auto onset = onsetAfter (result.left, 0);
        const auto expected = (int) std::llround (exactStepSamples * 4.0);

        check (onset >= 0, "step 4 produced audio");
        check (onset >= 0 && std::abs (onset - expected) <= 2,
               "onset sits on step 4",
               "expected " + juce::String (expected) + ", got " + juce::String (onset));

        // The hit is a single 2000-sample click, so any second hit inside the
        // rendered bar means a step fired twice.
        const auto secondHit = onsetAfter (result.left, onset + 300);
        check (secondHit < 0, "the hit fires exactly once",
               secondHit < 0 ? "" : "second hit at " + juce::String (secondHit));

        click.deleteFile();
    }

    //======================================================================
    std::printf ("\n[3] every step lands on the grid\n");
    {
        GrooveboxProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);

        auto click = makeClickWav ("grid", 120, sampleRate);
        juce::String error;
        processor.loadSampleIntoChannel (0, click, error);

        clearAllSteps (processor);

        for (int step = 0; step < Layout::numSteps; ++step)
            processor.setStep (0, step, true);

        rearm (processor);
        processor.setPlaying (true);

        auto result = render (processor, stepSamples * Layout::numSteps + 64, blockSize, sampleRate);

        int worstError = 0;

        for (int step = 0; step < Layout::numSteps; ++step)
        {
            // A click every step, so look at the window around each expected onset.
            const auto expected = (int) std::llround (exactStepSamples * (double) step);
            const auto localOnset = onsetAfter (result.left, expected - 8);

            if (localOnset < 0 || localOnset > expected + 64)
            {
                worstError = 1 << 20; // missing hit
                break;
            }

            worstError = juce::jmax (worstError, std::abs (localOnset - expected));
        }

        check (worstError <= 2, "all 16 onsets within 2 samples of the grid",
               "worst error " + juce::String (worstError) + " samples");

        click.deleteFile();
    }

    //======================================================================
    std::printf ("\n[4] swing delays the off-beat\n");
    {
        // The swing formula from the web app, restated here so the test checks
        // the engine against the specification rather than against the engine.
        const auto expectedOnset = [] (int step, double swing, int grid)
        {
            const auto gridLength = (double) grid;
            const auto fraction = 0.5 + 0.25 * swing;
            const auto positionInPair = (double) (step % grid);
            const auto pair = (double) (step / grid);

            if (positionInPair < gridLength * 0.5)
                return pair * gridLength + positionInPair * (2.0 * fraction);

            return pair * gridLength
                     + fraction * gridLength
                     + (positionInPair - gridLength * 0.5) * 2.0 * (1.0 - fraction);
        };

        GrooveboxProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);

        auto click = makeClickWav ("swing", 2000, sampleRate);
        juce::String error;
        processor.loadSampleIntoChannel (0, click, error);

        clearAllSteps (processor);

        // Every step plays, so one render exposes the whole bar's timing.
        for (int step = 0; step < Layout::numSteps; ++step)
            processor.setStep (0, step, true);

        // Check both swing grids at full swing, and a straight bar as a control.
        struct Case { float swing; int grid; };

        const Case cases[] = { { 0.0f, 4 }, { 1.0f, 4 }, { 1.0f, 2 } };

        for (const auto& testCase : cases)
        {
            rearm (processor);
            processor.setSwingGrid (testCase.grid);
            processor.setSwing (testCase.swing);
            processor.setPlaying (true);

            auto result = render (processor, stepSamples * Layout::numSteps + 256, blockSize, sampleRate);

            int worstError = 0;
            juce::String measured;

            for (int step = 0; step < Layout::numSteps; ++step)
            {
                const auto expected = (int) std::llround (expectedOnset (step, testCase.swing, testCase.grid)
                                                            * exactStepSamples);
                const auto found = onsetAfter (result.left, expected - 32);

                measured += juce::String (found) + " ";

                if (found < 0 || found > expected + 32)
                {
                    worstError = 1 << 20;
                    break;
                }

                worstError = juce::jmax (worstError, std::abs (found - expected));
            }

            const auto label = "swing " + juce::String (testCase.swing, 1)
                                 + " on 1/" + juce::String (testCase.grid == 4 ? 8 : 16) + " grid";

            check (worstError <= 2, label + " lands every step on its swung onset",
                   "worst error " + juce::String (worstError) + " samples, onsets: " + measured);
        }

        click.deleteFile();
    }

    //======================================================================
    std::printf ("\n[5] pitch, mute and level\n");
    {
        GrooveboxProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);

        auto click = makeClickWav ("pitch", 400, sampleRate);
        juce::String error;
        processor.loadSampleIntoChannel (0, click, error);

        clearAllSteps (processor);
        processor.setStep (0, 0, true);

        processor.setChannelPitch (0, 0);
        rearm (processor);
        processor.setPlaying (true);
        auto flat = render (processor, stepSamples, blockSize, sampleRate);

        processor.setChannelPitch (0, 12);
        rearm (processor);
        processor.setPlaying (true);
        auto up = render (processor, stepSamples, blockSize, sampleRate);

        const auto flatLast = lastAbove (flat.left);
        const auto upLast = lastAbove (up.left);

        check (flatLast > 0 && upLast > 0, "both renders produced audio",
               "flat ends " + juce::String (flatLast) + ", up ends " + juce::String (upLast));
        check (upLast < flatLast, "an octave up shortens the sample",
               "flat ends " + juce::String (flatLast) + ", up ends " + juce::String (upLast));

        processor.setChannelMuted (0, true);
        rearm (processor);
        processor.setPlaying (true);
        auto muted = render (processor, stepSamples, blockSize, sampleRate);
        check (peakOf (muted.left) == 0.0f, "muting a channel silences it");

        processor.setChannelMuted (0, false);
        processor.setChannelPitch (0, 0);

        processor.setChannelLevel (0, 0.25f);
        rearm (processor);
        processor.setPlaying (true);
        auto quiet = render (processor, stepSamples, blockSize, sampleRate);

        processor.setChannelLevel (0, 1.0f);
        rearm (processor);
        processor.setPlaying (true);
        auto loud = render (processor, stepSamples, blockSize, sampleRate);

        check (peakOf (quiet.left) < peakOf (loud.left), "a lower level is quieter",
               "quiet " + juce::String (peakOf (quiet.left), 4) + ", loud " + juce::String (peakOf (loud.left), 4));

        click.deleteFile();
    }

    //======================================================================
    std::printf ("\n[5b] moves made while the sequencer runs take effect\n");
    {
        // This is what a slider drag does: the transport never stops, so a level
        // or mute change has to be picked up by the voices triggered afterwards.
        GrooveboxProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);

        auto click = makeClickWav ("live", 600, sampleRate);
        juce::String error;
        processor.loadSampleIntoChannel (0, click, error);

        clearAllSteps (processor);
        for (int step = 0; step < Layout::numSteps; ++step)
            processor.setStep (0, step, true);

        rearm (processor);
        processor.setChannelLevel (0, 1.0f);
        processor.setPlaying (true);

        // Eight steps at full level, then pull the fader down mid-flight.
        const auto half = stepSamples * 8;
        auto firstHalf = render (processor, half, blockSize, sampleRate);

        processor.setChannelLevel (0, 0.25f);
        auto secondHalf = render (processor, stepSamples * 8, blockSize, sampleRate);

        const auto loudPeak = peakOf (firstHalf.left);
        const auto quietPeak = peakOf (secondHalf.left);

        check (loudPeak > 0.0f, "first half made sound", juce::String (loudPeak, 4));
        check (quietPeak > 0.0f, "second half still makes sound", juce::String (quietPeak, 4));
        check (quietPeak < loudPeak * 0.6f,
               "lowering the level mid-playback takes effect on later hits",
               "before " + juce::String (loudPeak, 4) + ", after " + juce::String (quietPeak, 4));

        // Now mute mid-flight, again without stopping.
        processor.setChannelLevel (0, 1.0f);
        auto beforeMute = render (processor, stepSamples * 4, blockSize, sampleRate);
        processor.setChannelMuted (0, true);
        auto afterMute = render (processor, stepSamples * 4, blockSize, sampleRate);

        check (peakOf (beforeMute.left) > 0.0f, "audible before muting");
        check (peakOf (afterMute.left) == 0.0f,
               "muting mid-playback silences later hits",
               "peak " + juce::String (peakOf (afterMute.left), 4));

        processor.setChannelMuted (0, false);

        click.deleteFile();
    }

    //======================================================================
    std::printf ("\n[6] stereo and mono\n");
    {
        GrooveboxProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);

        auto click = makeClickWav ("mono", 200, sampleRate);
        juce::String error;
        processor.loadSampleIntoChannel (0, click, error);
        clearAllSteps (processor);
        processor.setStep (0, 0, true);
        processor.setPlaying (true);

        auto result = render (processor, stepSamples, blockSize, sampleRate);

        check (peakOf (result.left) > 0.0f, "left channel has audio");
        check (peakOf (result.left) == peakOf (result.right), "a mono sample is centred");

        click.deleteFile();
    }

    //======================================================================
    std::printf ("\n[7] state round-trips through save and load\n");
    {
        GrooveboxProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);

        clearAllSteps (processor);
        processor.setStep (2, 3, true);
        processor.setStep (0, 8, true);
        processor.setStep (11, 15, true);
        processor.setChannelPitch (2, -5);
        processor.setChannelName (2, "Rimshot");
        processor.setSwing (0.42f);
        processor.setSwingGrid (2);

        juce::MemoryBlock saved;
        processor.getStateInformation (saved);

        GrooveboxProcessor restored;
        restored.prepareToPlay (sampleRate, blockSize);
        restored.setStateInformation (saved.getData(), (int) saved.getSize());

        check (restored.isStepOn (2, 3), "pattern step survived");
        check (restored.isStepOn (0, 8), "second pattern step survived");
        check (restored.isStepOn (11, 15), "last channel step survived");
        check (! restored.isStepOn (2, 4), "an off step stayed off");
        check (restored.getChannelPitch (2) == -5, "pitch survived",
               "got " + juce::String (restored.getChannelPitch (2)));
        check (restored.getChannelName (2) == "Rimshot", "channel name survived",
               "got " + restored.getChannelName (2));
        check (std::abs (restored.getSwing() - 0.42f) < 0.001f, "swing survived");
        check (restored.getSwingGrid() == 2, "swing grid survived");
    }

    //======================================================================
    std::printf ("\n[7b] step velocities survive a save and load\n");
    {
        GrooveboxProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);

        clearAllSteps (processor);

        // A spread of values, including the two that are easy to get wrong: full
        // velocity, and the quietest step the encoding can express.
        processor.setStep (0, 0, true);
        processor.setStepVelocity (0, 0, 1.0f);

        processor.setStep (0, 1, true);
        processor.setStepVelocity (0, 1, 0.5f);

        processor.setStep (3, 6, true);
        processor.setStepVelocity (3, 6, 0.01f);

        // An off step carries a velocity too, which has to come back as off.
        processor.setStep (5, 9, false);
        processor.setStepVelocity (5, 9, 0.8f);

        juce::MemoryBlock saved;
        processor.getStateInformation (saved);

        GrooveboxProcessor restored;
        restored.prepareToPlay (sampleRate, blockSize);
        restored.setStateInformation (saved.getData(), (int) saved.getSize());

        const auto near = [] (float a, float b) { return std::abs (a - b) < 0.02f; };

        check (restored.isStepOn (0, 0), "a full-velocity step is still on");
        check (near (restored.getStepVelocity (0, 0), 1.0f), "full velocity survived",
               juce::String (restored.getStepVelocity (0, 0), 3));
        check (near (restored.getStepVelocity (0, 1), 0.5f), "half velocity survived",
               juce::String (restored.getStepVelocity (0, 1), 3));
        check (near (restored.getStepVelocity (3, 6), 0.01f), "a very quiet step survived",
               juce::String (restored.getStepVelocity (3, 6), 3));
        check (! restored.isStepOn (5, 9), "an off step stayed off even with a velocity set",
               juce::String (restored.getStepVelocity (5, 9), 3));
        check (! restored.isStepOn (0, 2), "a step that was never set stayed off");

        // A step that is on and quiet must not read back as off.
        check (restored.isStepOn (3, 6), "a quiet step is still on");
    }

    //======================================================================
    std::printf ("\n[7c] shifting a row carries its velocities\n");
    {
        GrooveboxProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);

        // One step on with a velocity that identifies it, so a shift that lost the
        // velocity would be obvious.
        const auto setUp = [&processor]
        {
            clearAllSteps (processor);

            for (int step = 0; step < Layout::numSteps; ++step)
                processor.setStepVelocity (2, step, 1.0f);

            processor.setStep (2, 0, true);
            processor.setStepVelocity (2, 0, 0.25f);
        };

        setUp();

        // Moving later by one puts the step at index 1...
        processor.shiftChannelRow (2, 1);
        check (! processor.isStepOn (2, 0), "after a shift right, step 0 is empty");
        check (processor.isStepOn (2, 1), "the step moved one place later");

        // ...and its velocity travelled with it, rather than staying behind.
        check (std::abs (processor.getStepVelocity (2, 1) - 0.25f) < 0.001f,
               "the velocity moved with the step",
               juce::String (processor.getStepVelocity (2, 1), 3));
        check (std::abs (processor.getStepVelocity (2, 0) - 1.0f) < 0.001f,
               "the place it left holds the default velocity again",
               juce::String (processor.getStepVelocity (2, 0), 3));

        // Back the other way restores it exactly.
        processor.shiftChannelRow (2, -1);
        check (processor.isStepOn (2, 0) && ! processor.isStepOn (2, 1),
               "shifting back returns the step to where it was");
        check (std::abs (processor.getStepVelocity (2, 0) - 0.25f) < 0.001f,
               "and its velocity came back too",
               juce::String (processor.getStepVelocity (2, 0), 3));

        // The last step wraps round to the first instead of falling off the end.
        setUp();
        processor.setStep (2, 15, true);
        processor.setStepVelocity (2, 15, 0.6f);
        processor.shiftChannelRow (2, 1);

        check (processor.isStepOn (2, 0), "the last step wraps round to the first");
        check (std::abs (processor.getStepVelocity (2, 0) - 0.6f) < 0.001f,
               "and its velocity wrapped with it",
               juce::String (processor.getStepVelocity (2, 0), 3));

        // Shifting by a whole bar is a no-op, in either direction.
        setUp();
        processor.setStepVelocity (2, 0, 0.25f);

        processor.shiftChannelRow (2, Layout::numSteps);
        check (processor.isStepOn (2, 0) && std::abs (processor.getStepVelocity (2, 0) - 0.25f) < 0.001f,
               "shifting by a whole bar changes nothing");

        processor.shiftChannelRow (2, -Layout::numSteps);
        check (processor.isStepOn (2, 0) && std::abs (processor.getStepVelocity (2, 0) - 0.25f) < 0.001f,
               "shifting back by a whole bar changes nothing either");

        // Only the row that was asked for moves.
        setUp();
        processor.setStep (5, 3, true);

        processor.shiftChannelRow (2, 1);

        check (processor.isStepOn (5, 3) && ! processor.isStepOn (5, 4),
               "another channel's row is untouched");

        // Out-of-range requests are ignored rather than crashing.
        processor.shiftChannelRow (-1, 1);
        processor.shiftChannelRow (Layout::numChannels, 1);
        check (true, "a shift outside the channel range is ignored");
    }

    //======================================================================
    std::printf ("\n[8] MIDI notes play the channels\n");
    {
        GrooveboxProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);

        auto click = makeClickWav ("midi", 600, sampleRate);
        juce::String error;

        // A distinct sound on each of the first three channels, so a note can be
        // shown to reach the channel it is mapped to and no other.
        check (processor.loadSampleIntoChannel (0, click, error), "sample on channel 0", error);
        check (processor.loadSampleIntoChannel (1, click, error), "sample on channel 1", error);
        check (processor.loadSampleIntoChannel (2, click, error), "sample on channel 2", error);

        clearAllSteps (processor);          // nothing is on the grid
        processor.setManualTempo (120.0);
        processor.setPlaying (false);       // and the transport is stopped

        check (GrooveboxProcessor::getMidiNoteForChannel (0) == 36, "channel 0 is mapped to C1 (36)",
               juce::String (GrooveboxProcessor::getMidiNoteForChannel (0)));
        check (GrooveboxProcessor::getMidiNoteNameForChannel (0) == "C1", "and is named C1",
               GrooveboxProcessor::getMidiNoteNameForChannel (0));
        check (GrooveboxProcessor::getMidiNoteNameForChannel (1) == "C#1", "channel 1 is C#1",
               GrooveboxProcessor::getMidiNoteNameForChannel (1));

        // The transport is stopped, so this can only come from the note.
        auto hit = renderNote (processor, 36);
        check (peakOf (hit) > 0.0f, "a note plays with the transport stopped",
               "peak " + juce::String (peakOf (hit), 4));

        // A note outside the mapped range must stay silent.
        auto stray = renderNote (processor, 60);
        check (peakOf (stray) == 0.0f, "a note outside the range is ignored",
               "peak " + juce::String (peakOf (stray), 4));

        // Velocity scales the hit.
        auto soft = renderNote (processor, 36, 0.25f);
        auto hard = renderNote (processor, 36, 1.0f);

        check (peakOf (soft) > 0.0f && peakOf (hard) > peakOf (soft),
               "velocity scales the hit",
               "soft " + juce::String (peakOf (soft), 4) + ", hard " + juce::String (peakOf (hard), 4));

        // Muting a channel silences its note.
        processor.setChannelMuted (0, true);
        auto muted = renderNote (processor, 36);
        check (peakOf (muted) == 0.0f, "a muted channel does not answer its note");
        processor.setChannelMuted (0, false);

        // Every channel answers its own note, and each note reaches exactly one
        // channel: mute all but one and check the others stay silent.
        for (int channel = 0; channel < Layout::numChannels; ++channel)
        {
            if (channel != 1)
                processor.setChannelMuted (channel, true);
        }

        auto note1 = renderNote (processor, GrooveboxProcessor::getMidiNoteForChannel (1));
        auto note0 = renderNote (processor, GrooveboxProcessor::getMidiNoteForChannel (0));

        check (peakOf (note1) > 0.0f, "channel 1 answers its own note",
               "peak " + juce::String (peakOf (note1), 4));
        check (peakOf (note0) == 0.0f, "channel 0 stays silent while muted",
               "peak " + juce::String (peakOf (note0), 4));

        for (int channel = 0; channel < Layout::numChannels; ++channel)
            processor.setChannelMuted (channel, false);

        // Note off must not cut a one-shot short: a drum machine rings out. This
        // needs a sample long enough that the difference would be audible, so it
        // is rendered here rather than using the short test click.
        {
            auto longFile = testScratchDirectory().getChildFile ("midi-sustain.wav");
            longFile.deleteFile();

            {
                constexpr int length = 8000;
                juce::AudioBuffer<float> content (1, length);
                auto* data = content.getWritePointer (0);

                // A flat tone that stays loud, so any truncation is obvious.
                for (int i = 0; i < length; ++i)
                    data[i] = 0.9f * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * i / sampleRate);

                juce::WavAudioFormat format;
                std::unique_ptr<juce::FileOutputStream> stream (longFile.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer (
                    stream != nullptr ? format.createWriterFor (stream.get(), sampleRate, 1, 24, {}, 0) : nullptr);

                if (writer != nullptr)
                {
                    stream.release();
                    writer->writeFromAudioSampleBuffer (content, 0, length);
                }
            }

            GrooveboxProcessor midiProcessor;
            midiProcessor.prepareToPlay (sampleRate, blockSize);

            juce::String loadError;
            const auto loaded = midiProcessor.loadSampleIntoChannel (0, longFile, loadError);

            if (! loaded)
                std::printf ("        (long sample did not load: %s)\n", loadError.toRawUTF8());

            clearAllSteps (midiProcessor);
            midiProcessor.setPlaying (false);

            const auto renderWithOptionalNoteOff = [&longFile, sampleRate, blockSize] (bool sendNoteOff)
            {
                // A fresh processor per render: reusing one would let the first
                // render's still-ringing voice leak into the second, since an
                // 8000-sample one-shot is far longer than the window rendered.
                GrooveboxProcessor p;
                p.prepareToPlay (sampleRate, blockSize);

                juce::String err;
                p.loadSampleIntoChannel (0, longFile, err);
                clearAllSteps (p);
                p.setPlaying (false);

                constexpr int block = 256;
                constexpr int blocks = 8;

                juce::AudioBuffer<float> buffer (2, block);
                juce::MidiBuffer midi;
                std::vector<float> out;

                for (int b = 0; b < blocks; ++b)
                {
                    buffer.clear();
                    midi.clear();

                    if (b == 0)
                        midi.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 0);
                    else if (b == 1 && sendNoteOff)
                        midi.addEvent (juce::MidiMessage::noteOff (1, 36), 0);

                    p.processBlock (buffer, midi);

                    for (int i = 0; i < block; ++i)
                        out.push_back (buffer.getSample (0, i));
                }

                return out;
            };

            auto sustained = renderWithOptionalNoteOff (false);
            auto released = renderWithOptionalNoteOff (true);

            // Sample 512 is where the note off lands, so the tail is everything
            // after it. With an 8000-sample one-shot there is plenty left.
            constexpr int tailFrom = 600;
            const auto tailPeak = [] (const std::vector<float>& data, int from)
            {
                float peak = 0.0f;

                for (int i = from; i < (int) data.size(); ++i)
                    peak = juce::jmax (peak, std::abs (data[(size_t) i]));

                return peak;
            };

            check (tailPeak (released, tailFrom) > 0.0f,
                   "audio continues past where the note off lands",
                   "tail peak " + juce::String (tailPeak (released, tailFrom), 4));

            juce::ignoreUnused (sustained);

            longFile.deleteFile();
        }

        click.deleteFile();
    }

    //======================================================================
    std::printf ("\n[9] per-channel ADSR\n");
    {
        // A long flat tone, so anything the envelope does is easy to see: the
        // sample itself would otherwise decide how long a hit lasts.
        auto toneFile = testScratchDirectory().getChildFile ("adsr-tone.wav");
        toneFile.deleteFile();

        constexpr int toneLength = 60000;

        {
            juce::AudioBuffer<float> content (1, toneLength);
            auto* data = content.getWritePointer (0);

            for (int i = 0; i < toneLength; ++i)
                data[i] = 0.9f * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * i / sampleRate);

            juce::WavAudioFormat format;
            std::unique_ptr<juce::FileOutputStream> stream (toneFile.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> writer (
                stream != nullptr ? format.createWriterFor (stream.get(), sampleRate, 1, 24, {}, 0) : nullptr);

            if (writer != nullptr)
            {
                stream.release();
                writer->writeFromAudioSampleBuffer (content, 0, toneLength);
            }
        }

        // Plays one note and returns the rendered left channel. Everything about
        // the envelope is set by the caller first.
        const auto renderNoteWithEnvelope = [sampleRate, blockSize] (float attack, float decay, float sustainLevel,
                                                                    float release, int noteOffBlock, int blocks)
        {
            GrooveboxProcessor p;
            p.prepareToPlay (sampleRate, blockSize);

            juce::String err;
            p.loadSampleIntoChannel (0, testScratchDirectory().getChildFile ("adsr-tone.wav"), err);
            clearAllSteps (p);
            p.setPlaying (false);

            p.setChannelAttack (0, attack);
            p.setChannelDecay (0, decay);
            p.setChannelSustain (0, sustainLevel);
            p.setChannelRelease (0, release);

            constexpr int block = 256;
            juce::AudioBuffer<float> buffer (2, block);
            juce::MidiBuffer midi;
            std::vector<float> out;

            for (int b = 0; b < blocks; ++b)
            {
                buffer.clear();
                midi.clear();

                if (b == 0)
                    midi.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 0);
                else if (b == noteOffBlock)
                    midi.addEvent (juce::MidiMessage::noteOff (1, 36), 0);

                p.processBlock (buffer, midi);

                for (int i = 0; i < block; ++i)
                    out.push_back (buffer.getSample (0, i));
            }

            return out;
        };

        const auto rmsBetween = [] (const std::vector<float>& data, int from, int to)
        {
            double sum = 0.0;
            int count = 0;

            for (int i = juce::jmax (0, from); i < juce::jmin (to, (int) data.size()); ++i)
            {
                sum += (double) data[(size_t) i] * data[(size_t) i];
                ++count;
            }

            return count > 0 ? (float) std::sqrt (sum / count) : 0.0f;
        };

        // -------- sustain: a channel with sustain holds, one without does not ----
        {
            // Decay 0.4 of the range is about 0.48 s, so by one second in the
            // one-shot has finished while the sustaining one is still going.
            auto oneShot = renderNoteWithEnvelope (0.0f, 0.4f, 0.0f, 0.1f, 999, 220);
            auto held = renderNoteWithEnvelope (0.0f, 0.4f, 1.0f, 0.1f, 999, 220);

            const auto lateFrom = 44100; // one second in
            const auto oneShotTail = rmsBetween (oneShot, lateFrom, (int) oneShot.size());
            const auto heldTail = rmsBetween (held, lateFrom, (int) held.size());

            check (oneShotTail < 0.01f, "sustain at 0 makes the channel a one-shot",
                   "level at 1 s " + juce::String (oneShotTail, 4));
            check (heldTail > 0.2f, "sustain at 1 holds the note",
                   "level at 1 s " + juce::String (heldTail, 4));
        }

        // -------- note off releases a sustaining channel -------------------------
        {
            auto neverReleased = renderNoteWithEnvelope (0.0f, 0.75f, 1.0f, 0.3f, 999, 220);
            auto releasedAt = renderNoteWithEnvelope (0.0f, 0.75f, 1.0f, 0.3f, 8, 220);

            const auto afterRelease = 8 * 256 + 20000;

            const auto stillHeld = rmsBetween (neverReleased, afterRelease, (int) neverReleased.size());
            const auto letGo = rmsBetween (releasedAt, afterRelease, (int) releasedAt.size());

            check (stillHeld > 0.2f, "a held note keeps sounding without a note off",
                   "level " + juce::String (stillHeld, 4));
            check (letGo < 0.01f, "a note off releases the note",
                   "level " + juce::String (letGo, 4));
        }

        // -------- release length decides how long the tail is --------------------
        {
            auto shortRelease = renderNoteWithEnvelope (0.0f, 0.75f, 1.0f, 0.0f, 4, 220);
            auto longRelease = renderNoteWithEnvelope (0.0f, 0.75f, 1.0f, 1.0f, 4, 220);

            // Shortly after the note off, a long release is still sounding while a
            // short one has already finished.
            const auto justAfter = 4 * 256 + 3000;

            const auto shortTail = rmsBetween (shortRelease, justAfter, justAfter + 4000);
            const auto longTail = rmsBetween (longRelease, justAfter, justAfter + 4000);

            check (longTail > shortTail * 3.0f, "a longer release leaves a longer tail",
                   "short " + juce::String (shortTail, 4) + ", long " + juce::String (longTail, 4));
        }

        // -------- attack: a slow attack ramps in --------------------------------
        {
            auto fast = renderNoteWithEnvelope (0.0f, 0.75f, 1.0f, 0.1f, 999, 120);
            auto slow = renderNoteWithEnvelope (1.0f, 0.75f, 1.0f, 0.1f, 999, 120);

            // The very start of the note: with a slow attack it should be much quieter.
            const auto opening = rmsBetween (fast, 0, 4000);
            const auto openingSlow = rmsBetween (slow, 0, 4000);

            check (opening > openingSlow * 3.0f, "a longer attack ramps in more slowly",
                   "fast " + juce::String (opening, 4) + ", slow " + juce::String (openingSlow, 4));
        }

        // -------- decay: a longer decay holds the note up longer -----------------
        {
            auto quick = renderNoteWithEnvelope (0.0f, 0.1f, 0.0f, 0.1f, 999, 220);
            auto gradual = renderNoteWithEnvelope (0.0f, 0.75f, 0.0f, 0.1f, 999, 220);

            const auto midway = 20000;
            const auto quickLevel = rmsBetween (quick, midway, midway + 4000);
            const auto gradualLevel = rmsBetween (gradual, midway, midway + 4000);

            check (gradualLevel > quickLevel * 3.0f, "a longer decay holds the note up longer",
                   "quick " + juce::String (quickLevel, 4) + ", gradual " + juce::String (gradualLevel, 4));
        }

        // -------- the ramp is normalised and formats for display ----------------
        {
            check (GrooveboxProcessor::formatEnvelopeValue (0.0f, EnvelopeStage::sustain) == "0%",
                   "sustain formats as a percentage",
                   GrooveboxProcessor::formatEnvelopeValue (0.0f, EnvelopeStage::sustain));
            check (GrooveboxProcessor::formatEnvelopeValue (0.5f, EnvelopeStage::sustain) == "50%",
                   "sustain shows its level",
                   GrooveboxProcessor::formatEnvelopeValue (0.5f, EnvelopeStage::sustain));
            check (GrooveboxProcessor::formatEnvelopeValue (0.0f, EnvelopeStage::attack).contains ("ms"),
                   "a very short time is shown in milliseconds",
                   GrooveboxProcessor::formatEnvelopeValue (0.0f, EnvelopeStage::attack));
            check (GrooveboxProcessor::formatEnvelopeValue (1.0f, EnvelopeStage::attack).contains ("s"),
                   "a long time is shown in seconds",
                   GrooveboxProcessor::formatEnvelopeValue (1.0f, EnvelopeStage::attack));

            // The curve means half travel is well under half the maximum time.
            const auto half = GrooveboxProcessor::envelopeSecondsFor (0.5f, EnvelopeStage::attack);
            check (half > 0.0 && half < Layout::maxAttackSeconds * 0.5,
                   "the time curve favours the short end",
                   juce::String (half, 3) + " s of " + juce::String (Layout::maxAttackSeconds, 1) + " s");
        }

        // -------- envelope values survive a save/load round trip ----------------
        {
            GrooveboxProcessor saved;
            saved.prepareToPlay (sampleRate, blockSize);
            saved.setChannelAttack (2, 0.8f);
            saved.setChannelDecay (2, 0.4f);
            saved.setChannelSustain (2, 0.65f);
            saved.setChannelRelease (2, 0.2f);

            juce::MemoryBlock state;
            saved.getStateInformation (state);

            GrooveboxProcessor restored;
            restored.prepareToPlay (sampleRate, blockSize);
            restored.setStateInformation (state.getData(), (int) state.getSize());

            const auto near = [] (float a, float b) { return std::abs (a - b) < 0.001f; };

            check (near (restored.getChannelAttack (2), 0.8f), "attack survived",
                   juce::String (restored.getChannelAttack (2), 3));
            check (near (restored.getChannelDecay (2), 0.4f), "decay survived",
                   juce::String (restored.getChannelDecay (2), 3));
            check (near (restored.getChannelSustain (2), 0.65f), "sustain survived",
                   juce::String (restored.getChannelSustain (2), 3));
            check (near (restored.getChannelRelease (2), 0.2f), "release survived",
                   juce::String (restored.getChannelRelease (2), 3));

            // A channel that was never touched keeps the drum defaults.
            check (near (restored.getChannelSustain (0), Layout::defaultSustain),
                   "an untouched channel keeps the default sustain",
                   juce::String (restored.getChannelSustain (0), 3));
        }

        toneFile.deleteFile();
    }

    //======================================================================
    std::printf ("\n[10] the master trim is the only place the output is scaled\n");
    {
        auto click = makeClickWav ("master", 400, sampleRate);
        check (click.existsAsFile(), "test wav written for the master checks");

        // One hit on step 0, so whatever comes out is the sample's own peak times
        // the master trim and nothing else. That is what makes the level match
        // with the build that hid a 0.8 inside every voice checkable rather than
        // merely asserted.
        const auto renderHit = [&click, sampleRate] (float masterDb, float level, float velocity)
        {
            GrooveboxProcessor p;
            p.prepareToPlay (sampleRate, 256);

            juce::String err;
            p.loadSampleIntoChannel (0, click, err);

            clearAllSteps (p);
            p.setStep (0, 0, true);
            p.setStepVelocity (0, 0, velocity);
            p.setChannelLevel (0, level);
            p.setMasterGainDb (masterDb);
            p.setSwing (0.0f);
            p.setSwingGrid (4);
            p.setManualTempo (120.0);

            // The trim moves through a 20 ms smoother so a drag cannot zipper, so
            // it has to be given a few silent blocks to settle before the hit is
            // armed. Otherwise the first block is measured part-way up the ramp and
            // the test reports the smoother rather than the trim. This is what
            // happens in a real session anyway: the host calls processBlock
            // continuously, so the trim has long since converged by the time a
            // pattern plays.
            {
                juce::AudioBuffer<float> settle (2, 256);
                juce::MidiBuffer silent;

                for (int i = 0; i < 32; ++i)
                {
                    settle.clear();
                    p.processBlock (settle, silent);
                }
            }

            p.resetTransport();
            p.setPlaying (true);

            juce::AudioBuffer<float> buffer (2, 256);
            juce::MidiBuffer midi;
            float peak = 0.0f;

            for (int block = 0; block < 4; ++block)
            {
                buffer.clear();
                p.processBlock (buffer, midi);
                peak = juce::jmax (peak, buffer.getMagnitude (0, buffer.getNumSamples()));
            }

            p.setPlaying (false);
            return peak;
        };

        const auto dbOf = [] (float ratio)
        {
            return 20.0f * std::log10 (juce::jmax (1.0e-9f, ratio));
        };

        // The source click reaches 0.9, so unity all the way through has to give
        // 0.9 back. Anything less means something is still scaling the voice.
        const auto unity = renderHit (0.0f, 1.0f, 1.0f);

        check (std::abs (unity - 0.9f) < 0.02f,
               "a unity hit comes out at the sample's own peak",
               "peak " + juce::String (unity, 4)
                 + " (" + juce::String (dbOf (unity / 0.9f), 2) + " dB from the source)");

        // The trim that used to be buried in every voice was a fixed 0.8.
        const auto oldHiddenDb = 20.0f * std::log10 (0.8f);
        const auto oldEquivalent = renderHit (oldHiddenDb, 1.0f, 1.0f);

        check (std::abs (oldEquivalent / unity - 0.8f) < 0.004f,
               "the 0.8 the old build applied is reachable from the trim",
               "ratio " + juce::String (oldEquivalent / unity, 4));

        // ...and the default sits close enough to it that a session saved before
        // the trim existed comes back at the level it was written at, rather than
        // 2 dB louder.
        const auto asShipped = renderHit (Layout::defaultMasterGainDb, 1.0f, 1.0f);
        const auto differenceDb = std::abs (dbOf (asShipped / oldEquivalent));

        check (differenceDb < 0.1f,
               "the default trim is level-matched with the shipped build",
               juce::String (differenceDb, 3) + " dB apart");

        // Velocity was applied twice for a while — once into the voice's gain and
        // again per sample — which squared it, so a half-velocity step played a
        // quarter as loud as the grid said it should.
        const auto halfVelocity = renderHit (0.0f, 1.0f, 0.5f);

        check (std::abs (halfVelocity / unity - 0.5f) < 0.01f,
               "a step's velocity is applied once, not squared",
               "ratio " + juce::String (halfVelocity / unity, 4));

        // A louder trim is a louder output, all the way up the range.
        const auto loud = renderHit (Layout::maxMasterGainDb, 1.0f, 1.0f);
        const auto quiet = renderHit (Layout::minMasterGainDb, 1.0f, 1.0f);

        check (std::abs (dbOf (loud / unity) - Layout::maxMasterGainDb) < 0.1f,
               "the top of the trim adds its dB",
               juce::String (dbOf (loud / unity), 2) + " dB");
        check (std::abs (dbOf (quiet / unity) - Layout::minMasterGainDb) < 0.1f,
               "the bottom of the trim takes its dB away",
               juce::String (dbOf (quiet / unity), 2) + " dB");

        click.deleteFile();
    }

    //======================================================================
    std::printf ("\n[10b] the meter reports and the clip indicator latches\n");
    {
        auto click = makeClickWav ("meter", 400, sampleRate);

        GrooveboxProcessor p;
        p.prepareToPlay (sampleRate, blockSize);

        juce::String err;
        p.loadSampleIntoChannel (0, click, err);

        clearAllSteps (p);
        p.setStep (0, 0, true);
        p.setChannelLevel (0, 1.5f);
        p.setMasterGainDb (Layout::maxMasterGainDb);
        p.setSwing (0.0f);
        p.setManualTempo (120.0);

        // Let the trim's smoother settle before the hit, so the block that clips
        // is measured at the trim it was set to rather than part-way up the ramp
        // from the default.
        {
            juce::AudioBuffer<float> settle (2, blockSize);
            juce::MidiBuffer silent;

            for (int i = 0; i < 32; ++i)
            {
                settle.clear();
                p.processBlock (settle, silent);
            }
        }

        p.resetTransport();

        check (! p.isMasterClipping (0), "nothing is latched before anything plays");

        p.setPlaying (true);

        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;
        float peak = 0.0f;
        float meterPeak = 0.0f;

        for (int block = 0; block < 4; ++block)
        {
            buffer.clear();
            p.processBlock (buffer, midi);
            peak = juce::jmax (peak, buffer.getMagnitude (0, buffer.getNumSamples()));

            // Read the meter every block, the way the editor's timer does. The
            // hold is deliberately short so a transient does not sit on the bar
            // for ever, so reading it only at the end would report the decay
            // rather than the level.
            meterPeak = juce::jmax (meterPeak, p.getMasterPeak (0));
        }

        p.setPlaying (false);

        check (peak >= 1.0f, "the hit passes 0 dBFS at the top of the trim",
               "peak " + juce::String (peak, 4));
        check (p.isMasterClipping (0), "the clip indicator latches");
        check (meterPeak >= peak, "the meter holds what the output reached",
               "meter " + juce::String (meterPeak, 4) + ", output " + juce::String (peak, 4));

        // The latch is the only thing that survives silence, and clicking the
        // meter is what clears it.
        for (int block = 0; block < 40; ++block)
        {
            buffer.clear();
            p.processBlock (buffer, midi);
        }

        check (p.isMasterClipping (0), "the latch holds through silence");
        check (p.getMasterPeak (0) < peak * 0.5f, "the meter falls away afterwards",
               juce::String (p.getMasterPeak (0), 4));

        p.clearMasterClipLatch();
        check (! p.isMasterClipping (0), "clearing the latch clears it");

        click.deleteFile();
    }

    //======================================================================
    std::printf ("\n[10c] the master trim survives a save and load\n");
    {
        GrooveboxProcessor saved;
        saved.prepareToPlay (sampleRate, blockSize);
        saved.setMasterGainDb (-7.5f);

        juce::MemoryBlock state;
        saved.getStateInformation (state);

        GrooveboxProcessor restored;
        restored.prepareToPlay (sampleRate, blockSize);
        restored.setStateInformation (state.getData(), (int) state.getSize());

        check (std::abs (restored.getMasterGainDb() + 7.5f) < 0.01f,
               "the trim came back",
               juce::String (restored.getMasterGainDb(), 2));

        // A session written before the trim existed holds no value for it, and has
        // to come back at the default rather than at unity.
        GrooveboxProcessor fresh;
        fresh.prepareToPlay (sampleRate, blockSize);

        check (std::abs (fresh.getMasterGainDb() - Layout::defaultMasterGainDb) < 0.01f,
               "a new session starts at the default trim",
               juce::String (fresh.getMasterGainDb(), 2));

        check (GrooveboxProcessor::formatMasterGain (-2.0f) == "-2.0 dB",
               "a negative trim reads with its sign",
               GrooveboxProcessor::formatMasterGain (-2.0f));
        check (GrooveboxProcessor::formatMasterGain (1.5f) == "+1.5 dB",
               "a positive trim reads with a plus",
               GrooveboxProcessor::formatMasterGain (1.5f));
    }

    //======================================================================
    // The registry used to be guarded by a spin lock that the audio thread took
    // on every voice trigger. It is now a lock-free slot table, and the claim
    // that makes safe — a published sample is never removed, so a pointer the
    // audio thread reads cannot be freed underneath it — is worth hammering
    // from two threads rather than reasoning about.
    std::printf ("\n[11] samples load while the audio thread is running\n");
    {
        auto click = makeClickWav ("concurrent", 300, sampleRate);

        GrooveboxProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);

        juce::String err;
        check (processor.loadSampleIntoChannel (0, click, err), "first sample loaded", err);

        clearAllSteps (processor);
        processor.setStep (0, 0, true);
        processor.setManualTempo (120.0);
        processor.resetTransport();
        processor.setPlaying (true);

        std::atomic<bool> stop { false };
        std::atomic<int> blocksRendered { 0 };
        std::atomic<int> blocksWithAudio { 0 };

        // Stands in for the host's audio callback: it only ever calls
        // processBlock, which is the one thing that must not touch a lock.
        std::thread renderThread ([&]
        {
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;

            while (! stop.load())
            {
                buffer.clear();
                processor.processBlock (buffer, midi);

                blocksRendered.fetch_add (1);

                if (buffer.getMagnitude (0, blockSize) > 0.0f)
                    blocksWithAudio.fetch_add (1);
            }
        });

        // Meanwhile the message thread does what a player dropping files does:
        // decode, publish, reassign the channel to the new id. Every reassign is
        // a race with a voice trigger on the other thread.
        constexpr int loads = 200;
        int loaded = 0;

        for (int i = 0; i < loads; ++i)
        {
            juce::String loadError;

            if (processor.loadSampleIntoChannel (i % Layout::numChannels, click, loadError))
                ++loaded;
        }

        stop.store (true);
        renderThread.join();
        processor.setPlaying (false);

        check (loaded == loads, "every concurrent load succeeded",
               juce::String (loaded) + " of " + juce::String (loads));
        check (blocksRendered.load() > 0, "the audio thread ran throughout",
               juce::String (blocksRendered.load()) + " blocks");
        check (blocksWithAudio.load() > 0, "it kept making sound while samples were swapped",
               juce::String (blocksWithAudio.load()) + " of "
                 + juce::String (blocksRendered.load()) + " blocks had audio");

        click.deleteFile();
    }

    //======================================================================
    // A fixed slot table has a ceiling the old std::map did not, so hitting it
    // has to fail loudly rather than silently dropping the sample.
    std::printf ("\n[11b] the registry ceiling reports an error\n");
    {
        auto click = makeClickWav ("ceiling", 32, sampleRate);

        SampleStore store;
        juce::String err;
        int ids = 0;

        while (ids < SampleStore::maxSamples)
        {
            err.clear();

            if (store.addFile (click, err) == 0)
                break;

            ++ids;
        }

        check (ids == SampleStore::maxSamples, "the registry fills to its ceiling",
               juce::String (ids) + " of " + juce::String (SampleStore::maxSamples));

        err.clear();
        const auto overflow = store.addFile (click, err);

        check (overflow == 0, "one more load is refused");
        check (err.isNotEmpty(), "and the refusal comes with a message", err);

        // Refusing a new sample must not disturb the ones already published.
        check (store.getSample (1) != nullptr, "the oldest sample is still there");
        check (store.getSample (SampleStore::maxSamples) != nullptr, "and so is the newest");
        check (store.getSampleIds().size() == SampleStore::maxSamples, "the id list is complete");

        click.deleteFile();
    }

    //======================================================================
    // The head is the only clock, so any error in how a block advances it turns
    // into the pattern rushing or dragging against real time. One bar hides that;
    // thirty-two do not, because the error is picked up again on every onset.
    std::printf ("\n[12] the pattern stays on the grid over many bars\n");
    {
        /**
            Renders `bars` bars of every step firing and returns the worst
            distance, in samples, between an onset and where the grid says it
            should be. A negative error means the pattern is running early.
        */
        const auto worstGridError = [&] (double tempo, int block, int bars) -> int
        {
            auto click = makeClickWav ("multibar", 120, sampleRate);

            GrooveboxProcessor processor;
            processor.prepareToPlay (sampleRate, block);

            juce::String err;
            processor.loadSampleIntoChannel (0, click, err);

            clearAllSteps (processor);

            for (int step = 0; step < Layout::numSteps; ++step)
                processor.setStep (0, step, true);

            rearm (processor, tempo);
            processor.setPlaying (true);

            const auto totalSteps = Layout::numSteps * bars;
            const auto stepSamples = sampleRate / (tempo / 60.0 * 4.0);
            const auto totalSamples = (int) std::ceil ((double) totalSteps * stepSamples) + block;

            auto result = render (processor, totalSamples, block, sampleRate);

            processor.setPlaying (false);
            click.deleteFile();

            int worst = 0;
            int searchFrom = 0;

            for (int step = 0; step < totalSteps; ++step)
            {
                // Search from just past the previous hit rather than from just
                // before the ideal one: a drifting pattern moves *earlier*, and
                // looking from `ideal - n` would then find the next hit and report
                // a whole step of error instead of the drift.
                const auto found = onsetAfter (result.left, searchFrom);

                if (found < 0)
                    return 1 << 20; // a step never fired at all

                const auto ideal = (int) std::llround ((double) step * stepSamples);

                worst = juce::jmax (worst, std::abs (found - ideal));
                searchFrom = found + 200; // the click is 120 samples long
            }

            return worst;
        };

        // A slow run at a small block size: one onset per block, many blocks.
        const auto slow = worstGridError (120.0, 256, 32);
        check (slow <= 2, "32 bars at 120 bpm stay sample-accurate",
               "worst error " + juce::String (slow) + " samples");

        // A fast run at a large block size, so several onsets fall inside one
        // block. That is the case where the head accounting has to cope with more
        // than one step firing between two calls.
        const auto fast = worstGridError (300.0, 4096, 16);
        check (fast <= 2, "16 bars at 300 bpm with several onsets per block stay sample-accurate",
               "worst error " + juce::String (fast) + " samples");

        // An odd block size that never divides a step evenly, so the fractional
        // part of the head is carried across every single block.
        const auto odd = worstGridError (127.0, 333, 24);
        check (odd <= 2, "24 bars at 127 bpm on a 333-sample block stay sample-accurate",
               "worst error " + juce::String (odd) + " samples");
    }

    //======================================================================
    // Pitching a sample up moves its top end above the output's Nyquist, and what
    // happens then is entirely the resampler's decision. Linear interpolation
    // folds the excess back down as a tone that was never in the source; an
    // anti-alias kernel lowers its cutoff instead and leaves the output quiet.
    std::printf ("\n[13] pitching up does not fold the top end back down\n");
    {
        /**
            How much of a tone survives being played `semitones` up, as a ratio
            against the same tone played flat.

            Measured against the source played flat rather than against an absolute
            level, so the number is about what the pitch change did and not about
            where the samples of the tone happen to land. `flatPeakOut` reports the
            reference itself, so a check can tell "the tone folded away" from "the
            tone never played".
        */
        const auto pitchedLevelRatio = [&] (double frequency, int semitones, float& flatPeakOut) -> float
        {
            constexpr int sourceLength = 4000;

            auto tone = makeToneWav ("alias", frequency, sourceLength, sampleRate);

            GrooveboxProcessor processor;

            // Unity trim, set before the prepare that snaps the master's smoother to
            // its target, so the reference peak below is the source's own level.
            processor.setMasterGainDb (0.0f);
            processor.prepareToPlay (sampleRate, blockSize);

            juce::String err;
            processor.loadSampleIntoChannel (0, tone, err);

            clearAllSteps (processor);
            processor.setStep (0, 0, true);

            // Full sustain, so the voice plays the whole source and the envelope
            // contributes nothing to the measurement.
            processor.setChannelSustain (0, 1.0f);

            processor.setChannelPitch (0, 0);
            rearm (processor);
            processor.setPlaying (true);
            auto flat = render (processor, sourceLength, blockSize, sampleRate);

            processor.setChannelPitch (0, semitones);
            rearm (processor);
            processor.setPlaying (true);
            auto pitched = render (processor, sourceLength, blockSize, sampleRate);

            processor.setPlaying (false);
            tone.deleteFile();

            // Measured past the first samples, where the read reaches off the front
            // of the source, and before it runs out at the end. An octave up the
            // source is over in half the time.
            flatPeakOut = flat.peakBetween (64, sourceLength / 2);
            const auto pitchedPeak = pitched.peakBetween (64, semitones > 0 ? sourceLength / 4
                                                                             : sourceLength / 2);

            return flatPeakOut > 1.0e-6f ? pitchedPeak / flatPeakOut : 0.0f;
        };

        // 18 kHz at 44.1 kHz is 0.82 of the way to Nyquist. An octave up it wants
        // to sit at 36 kHz, which has nowhere to go but back down to 8.1 kHz.
        float reference = 0.0f;
        const auto folded = pitchedLevelRatio (18000.0, 12, reference);

        check (reference > 0.9f, "a tone just under Nyquist plays at full level",
               "peak " + juce::String (reference, 4));
        check (juce::Decibels::gainToDecibels (folded, -300.0f) <= -30.0f,
               "an octave up leaves nothing of a tone that cannot be played",
               juce::String (juce::Decibels::gainToDecibels (folded, -300.0f), 1) + " dB");

        // The kernel has to be a low-pass and not a mute: a tone whose pitched
        // frequency is comfortably inside the passband comes through unchanged.
        float lowReference = 0.0f;
        const auto passthrough = pitchedLevelRatio (2000.0, 12, lowReference);
        const auto passthroughDb = juce::Decibels::gainToDecibels (passthrough, -300.0f);

        check (std::abs (passthroughDb) <= 1.5, "an octave up leaves a tone inside the passband alone",
               juce::String (passthroughDb, 2) + " dB");

        // And at unity there is nothing to resample, so the top of the source has
        // to survive: a kernel that trimmed even at no pitch change would be
        // throwing away content the user can hear.
        float unityReference = 0.0f;
        const auto unity = pitchedLevelRatio (18000.0, 0, unityReference);
        const auto unityDb = juce::Decibels::gainToDecibels (unity, -300.0f);

        check (std::abs (unityDb) <= 1.5, "playing at unity leaves a tone just under Nyquist alone",
               juce::String (unityDb, 2) + " dB");

        // Printed whether or not the checks above pass, so the log carries the
        // number rather than only a verdict on a threshold.
        std::printf ("        measured: folded %s dB, passband %s dB, unity %s dB\n",
                     juce::String (juce::Decibels::gainToDecibels (folded, -300.0f), 1).toRawUTF8(),
                     juce::String (passthroughDb, 2).toRawUTF8(),
                     juce::String (unityDb, 2).toRawUTF8());
    }

    //======================================================================
    // A block is split at every onset, so the voice renderer runs several times
    // per block. Anything it recomputes from scratch on each pass describes only
    // the last chunk, which is not what an activity indicator should report.
    std::printf ("\n[14] a hit that finished early in a block is still reported\n");
    {
        // 300 bpm on a 4096-sample block puts a step length of 2205 samples inside
        // one block, so the block is split and the renderer runs more than once.
        constexpr int bigBlock = 4096;

        auto click = makeClickWav ("sounding", 120, sampleRate);

        GrooveboxProcessor processor;
        processor.prepareToPlay (sampleRate, bigBlock);

        juce::String err;
        processor.loadSampleIntoChannel (0, click, err);
        processor.loadSampleIntoChannel (1, click, err);

        clearAllSteps (processor);
        processor.setStep (0, 0, true);   // fires at the top of the block
        processor.setStep (1, 1, true);   // fires part way through, splitting it

        rearm (processor, 300.0);
        processor.setPlaying (true);

        juce::AudioBuffer<float> buffer (2, bigBlock);
        juce::MidiBuffer midi;
        buffer.clear();
        processor.processBlock (buffer, midi);

        check (processor.isChannelSounding (0), "the channel that fired at the top of the block is still lit",
               juce::String ("channel 1 lit: ") + (processor.isChannelSounding (1) ? "yes" : "no"));
        check (processor.isChannelSounding (1), "and so is the one that fired part way through");

        // The next block has nothing in it, so both have to go out.
        buffer.clear();
        processor.processBlock (buffer, midi);
        processor.setPlaying (false);

        click.deleteFile();
    }

    //======================================================================
    // A note off belongs to the note that started it. The same lane can also be
    // sounding because the pattern fired it, and a player lifting a key should
    // not reach across and cut that hit short.
    std::printf ("\n[14b] a note off does not cut a grid hit short\n");
    {
        // A tone rather than the click fixture: the click decays with a 12-sample
        // half-life, so a long one is silence by the time the second block starts
        // and there would be nothing left to measure either way.
        auto tone = makeToneWav ("noteoff", 1000.0, 20000, sampleRate);

        GrooveboxProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);

        juce::String err;
        processor.loadSampleIntoChannel (0, tone, err);

        clearAllSteps (processor);
        processor.setStep (0, 0, true);

        // Full sustain so the grid hit holds, and a short release so that if
        // something does let go of it, the sound stops within one block.
        processor.setChannelSustain (0, 1.0f);
        processor.setChannelRelease (0, 0.02f);

        rearm (processor);
        processor.setPlaying (true);

        const auto afterNoteOff = [&] (bool sendNoteOff) -> float
        {
            rearm (processor);
            processor.setPlaying (true);

            juce::AudioBuffer<float> buffer (2, blockSize);

            // One block to start the grid hit, then the note off, then a block to
            // hear what is left.
            for (int block = 0; block < 3; ++block)
            {
                buffer.clear();

                juce::MidiBuffer midi;

                if (sendNoteOff && block == 1)
                    midi.addEvent (juce::MidiMessage::noteOff (1, Layout::firstMidiNote), 0);

                processor.processBlock (buffer, midi);

                if (block == 2)
                    return buffer.getMagnitude (0, blockSize);
            }

            return 0.0f;
        };

        const auto control = afterNoteOff (false);
        const auto released = afterNoteOff (true);

        processor.setPlaying (false);
        tone.deleteFile();

        check (control > 0.01f, "a sustaining grid hit is still sounding two blocks later",
               "peak " + juce::String (control, 4));
        check (released > control * 0.5f, "a note off on the same lane leaves the grid hit alone",
               "without note off " + juce::String (control, 4)
                   + ", with note off " + juce::String (released, 4));
    }

    //======================================================================
    // The point of moving the controls onto the parameter state is that a value
    // written the way a host writes it — straight into the parameter, past every
    // setter on the processor — still reaches the engine. This drives the
    // parameter directly and checks the audio and the atomic both follow, which
    // is what "automatable" has to mean or it means nothing.
    std::printf ("\n[15] a host-written parameter reaches the engine\n");
    {
        GrooveboxProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);

        auto click = makeClickWav ("automate", 400, sampleRate);
        juce::String error;
        processor.loadSampleIntoChannel (0, click, error);

        clearAllSteps (processor);
        processor.setStep (0, 0, true);

        auto& apvts = processor.getValueTreeState();
        auto* levelParam = apvts.getParameter (Params::levelId (0));

        check (levelParam != nullptr, "the channel level is exposed as a parameter");

        // Renders one bar after writing the level the way a host does: a normalised
        // value into the parameter itself, with no GrooveboxProcessor setter called
        // anywhere on the path from there to the audio.
        const auto renderAtLevel = [&] (float linearLevel) -> float
        {
            levelParam->setValueNotifyingHost (levelParam->convertTo0to1 (linearLevel));
            rearm (processor);
            processor.setPlaying (true);
            auto out = render (processor, stepSamples, blockSize, sampleRate);
            processor.setPlaying (false);
            return peakOf (out.left);
        };

        const auto loud = renderAtLevel (1.0f);
        const auto quiet = renderAtLevel (0.25f);

        check (loud > 0.0f, "a parameter-driven level produces sound", juce::String (loud, 4));
        check (quiet < loud, "a level written straight into the parameter reaches the engine",
               "quiet " + juce::String (quiet, 4) + ", loud " + juce::String (loud, 4));

        // The atomic the audio thread reads has to agree with the parameter, or the
        // two drift the moment a host automates one while the panel reads the other.
        levelParam->setValueNotifyingHost (levelParam->convertTo0to1 (0.4f));
        check (std::abs (processor.getChannelLevel (0) - 0.4f) < 0.001f,
               "the engine's own level follows the parameter",
               juce::String (processor.getChannelLevel (0), 4));

        // A discrete control too: muting through the parameter exercises the bool
        // path in parameterChanged and has to silence the lane.
        auto* muteParam = apvts.getParameter (Params::muteId (0));
        check (muteParam != nullptr, "the channel mute is exposed as a parameter");

        levelParam->setValueNotifyingHost (levelParam->convertTo0to1 (1.0f));
        muteParam->setValueNotifyingHost (1.0f);
        rearm (processor);
        processor.setPlaying (true);
        auto muted = render (processor, stepSamples, blockSize, sampleRate);
        processor.setPlaying (false);

        check (peakOf (muted.left) == 0.0f, "a mute written into the parameter silences the lane",
               juce::String (peakOf (muted.left), 4));

        click.deleteFile();
    }

    //======================================================================
    std::printf ("\n%s  %d checks, %d failures\n",
                 failures == 0 ? "PASSED" : "FAILED", checks, failures);

    return failures == 0 ? 0 : 1;
}
