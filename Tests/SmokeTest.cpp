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
    std::printf ("\n%s  %d checks, %d failures\n",
                 failures == 0 ? "PASSED" : "FAILED", checks, failures);

    return failures == 0 ? 0 : 1;
}
