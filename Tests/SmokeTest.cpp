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

#include <cmath>
#include <cstdio>
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

/** Writes a short click into a temporary wav so we can load it like a real sample.

    The click stays exactly at +/-1.0 for its first three samples and then decays,
    so the first sample above any small threshold is the onset itself and the
    measurement needs no interpolation guesswork.
*/
juce::File makeClickWav (const juce::String& name, int lengthSamples, double sampleRate)
{
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("groovebox-test-" + name + ".wav");

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
        const auto expected = stepSamples * 4;

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
            const auto expected = stepSamples * step;
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
                                                            * stepSamples);
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
    std::printf ("\n%s  %d checks, %d failures\n",
                 failures == 0 ? "PASSED" : "FAILED", checks, failures);

    return failures == 0 ? 0 : 1;
}
