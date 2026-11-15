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
#include <tuple>
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
    Integrated level of a render, for reporting beside a slew measurement.

    A taper fixes a discontinuity by taking amplitude away, so a slew gate can be
    satisfied by a change that quietly makes the machine softer. Peak and RMS
    printed next to the verdict are what tells a fade that only touches the part
    that was already ending from one that eats the hit.
*/
float rmsOf (const std::vector<float>& data)
{
    if (data.empty())
        return 0.0f;

    double sum = 0.0;

    for (auto value : data)
        sum += (double) value * (double) value;

    return (float) std::sqrt (sum / (double) data.size());
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

/** Loudest sample from `from` to the end of the render. */
float peakFrom (const std::vector<float>& data, int from)
{
    float peak = 0.0f;

    for (size_t i = (size_t) juce::jmax (0, from); i < data.size(); ++i)
        peak = juce::jmax (peak, std::abs (data[i]));

    return peak;
}

/**
    The first sample where two renders of the same pattern part company, or -1.

    Exact equality on purpose: two renders that differ only in a parameter the
    engine has not read yet must be the same bits, and a tolerance would let a
    change that leaked into the earlier part of the bar pass as "nothing changed".
*/
int firstDifference (const std::vector<float>& a, const std::vector<float>& b, int from, int to)
{
    const auto last = juce::jmin (to, juce::jmin ((int) a.size(), (int) b.size()));

    for (size_t i = (size_t) juce::jmax (0, from); i < (size_t) last; ++i)
        if (a[i] != b[i])
            return (int) i;

    return -1;
}

/** One step of FNV-1a, 64 bits. */
juce::uint64 fnv1a (juce::uint64 hash, const void* data, size_t numBytes)
{
    const auto* bytes = static_cast<const juce::uint8*> (data);

    for (size_t i = 0; i < numBytes; ++i)
        hash = (hash ^ bytes[i]) * 0x100000001b3ull;

    return hash;
}

/**
    A fingerprint of a whole render: the length, then the exact bits of both channels.

    One number that stands for an entire pass through the machine, so that a change
    which *claims* not to touch the audio path can be checked instead of argued. It is
    a hash and not a peak because the claim it has to answer — "these samples are the
    same ones" — is about every sample, including the ones nobody thought to look at.

    The length goes in first so that a render which stops early cannot match one that
    does not, and the float bits are hashed raw: no rounding, no tolerance, nothing
    that would let a slowly accumulating difference hide inside a digit.
*/
juce::uint64 fingerprintOf (const RenderResult& result)
{
    juce::uint64 hash = 0xcbf29ce484222325ull;

    const int numSamples = (int) result.left.size();
    const auto numBytes = (size_t) numSamples * sizeof (float);

    hash = fnv1a (hash, &numSamples, sizeof (numSamples));
    hash = fnv1a (hash, result.left.data(), numBytes);
    hash = fnv1a (hash, result.right.data(), numBytes);

    return hash;
}

/** The fingerprint printed the way it was recorded, so the two can be compared by eye. */
juce::String hexOf (juce::uint64 value)
{
    char text[24];
    std::snprintf (text, sizeof (text), "%016llX", (unsigned long long) value);

    return { text };
}

/**
    The noise burst `makeNoiseWav` writes, in the order it writes it.

    Returned as samples as well as written to disk because the claim being tested is
    that a hit at full velocity comes out with the balance the source went in with —
    which needs the source to be measurable, not just loadable.

    A plain integer recurrence rather than `std::rand`: these numbers go in the run
    log, and they have to mean the same thing on the next machine.
*/
std::vector<float> makeNoiseSamples (int lengthSamples)
{
    std::vector<float> data ((size_t) lengthSamples);

    juce::uint32 state = 20261115u;

    for (int i = 0; i < lengthSamples; ++i)
    {
        state = state * 1664525u + 1013904223u;
        data[(size_t) i] = 0.9f * ((float) ((state >> 8) & 0xffffff) / 8388608.0f - 1.0f);
    }

    return data;
}

/**
    Writes a noise burst into a scratch wav.

    Brightness is a statement about the top of the spectrum, so the fixture has to
    *have* a top: the tone and click sources used elsewhere are one frequency and a
    handful of them, and a lowpass has almost nothing to take away from either.
*/
juce::File makeNoiseWav (const juce::String& name, const std::vector<float>& samples, double sampleRate)
{
    auto file = testScratchDirectory().getChildFile ("noise-" + name + ".wav");

    file.deleteFile();

    const auto lengthSamples = (int) samples.size();

    juce::AudioBuffer<float> buffer (1, lengthSamples);
    buffer.copyFrom (0, 0, samples.data(), lengthSamples);

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

/**
    How much of a render's energy sits at the top — the ratio the ear calls brightness.

    The low band is a 32-sample moving average, which first nulls at a thirty-second
    of the sample rate (about 1.4 kHz here); whatever that average removes is the high
    band. Both are counted as power over the same samples, so what comes back is a
    *ratio*: scaling the output by any amount at all leaves it exactly where it was.

    That is the whole reason for measuring it this way. Velocity is about to move two
    things at once, level and tone, and a number that a fader could also have produced
    would not be evidence of anything. `tiltOf` can only be moved by a change in the
    balance between the bands, so a claim built on it is a claim about colour.
*/
float tiltOf (const std::vector<float>& data, int from, int to)
{
    constexpr int width = 32;
    const auto start = juce::jmax (0, from) + width;
    const auto last = juce::jmin (to, (int) data.size());

    if (last - start <= width)
        return 0.0f;

    double running = 0.0, lowPower = 0.0, highPower = 0.0;

    for (int i = start - width; i < start; ++i)
        running += data[(size_t) i];

    for (int i = start; i < last; ++i)
    {
        running += data[(size_t) i] - data[(size_t) (i - width)];

        const auto low = (float) (running / (double) width);
        const auto high = data[(size_t) i] - low;

        lowPower += (double) low * (double) low;
        highPower += (double) high * (double) high;
    }

    return lowPower > 1.0e-18 ? (float) (highPower / lowPower) : 0.0f;
}

/**
    A stand-in for the host's transport.

    Nothing in this file had one until now: every timing test drives the built-in
    transport through `setPlaying`, which is the branch the engine takes when there
    is no play head at all. In a real host there always is one, and the engine then
    takes its position and tempo from the host instead of keeping its own — so
    everything that only goes wrong while the host is driving, meaning a stop in the
    middle of a bar or a jump to a different position, is invisible to those tests.
    This makes the host branch reachable.

    The position advances by exactly the samples rendered, which is what a running
    transport does.
*/
struct FakePlayHead final : public juce::AudioPlayHead
{
    double ppq = 0.0;
    double bpm = 120.0;
    bool playing = false;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (bpm);
        info.setPpqPosition (ppq);
        info.setIsPlaying (playing);
        return info;
    }

    /** Moves the head on by `numSamples`, at the transport's own tempo. */
    void advance (int numSamples, double sampleRate)
    {
        ppq += (double) numSamples * bpm / (60.0 * sampleRate);
    }
};

/** Writes a run of digital silence, for filling voices without adding any signal. */
juce::File makeSilenceWav (const juce::String& name, int lengthSamples, double sampleRate)
{
    auto file = testScratchDirectory().getChildFile ("silence-" + name + ".wav");

    file.deleteFile();

    juce::AudioBuffer<float> buffer (1, lengthSamples);
    buffer.clear();

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

/** The largest step between one sample and the next — the signal's own slew. */
float maxDelta (const std::vector<float>& data)
{
    float worst = 0.0f;

    for (size_t i = 1; i < data.size(); ++i)
        worst = juce::jmax (worst, std::abs (data[i] - data[i - 1]));

    return worst;
}

/**
    The worst sample-to-sample jump in a render, and where it happened.

    A discontinuity is anything much larger than the waveform's own slew: a voice
    started, stolen or stopped at an arbitrary phase of its cycle moves the output by
    most of its amplitude in one sample, which is what a click is.
*/
struct SlewReport
{
    float worst = 0.0f;
    int atSample = -1;
};

SlewReport worstSlew (const std::vector<float>& data)
{
    SlewReport report;

    for (size_t i = 1; i < data.size(); ++i)
    {
        const auto delta = std::abs (data[i] - data[i - 1]);

        if (delta > report.worst)
        {
            report.worst = delta;
            report.atSample = (int) i;
        }
    }

    return report;
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

        //==================================================================
        // A grid the bar cannot divide. The panel only ever writes 2 or 4, but
        // `swingGrid` is an int parameter from 2 to 4, so a host automating or
        // typing it reaches 3 — and a 3-step pair cannot tile a 16-step bar: the
        // last pair of the bar is one step short, so the second half swings against
        // the first and the next bar starts a sixteenth later than this one ended.
        // The engine has to answer the value, not follow it.
        {
            rearm (processor);
            processor.setSwing (1.0f);

            auto* gridParam = processor.getValueTreeState().getParameter (Params::swingGridId);
            check (gridParam != nullptr, "the swing grid is exposed to the host");

            if (gridParam != nullptr)
                gridParam->setValueNotifyingHost (gridParam->convertTo0to1 (3.0f));

            check (processor.getSwingGrid() == 3, "a host can put the swing grid at 3",
                   juce::String (processor.getSwingGrid()));

            processor.setPlaying (true);
            auto result = render (processor, stepSamples * Layout::numSteps + 256, blockSize, sampleRate);

            int worstError = 0;
            juce::String measured;

            for (int step = 0; step < Layout::numSteps; ++step)
            {
                // Expected as the wider pair, which is what 3 is answered with.
                const auto expected = (int) std::llround (expectedOnset (step, 1.0, 4) * exactStepSamples);
                const auto found = onsetAfter (result.left, expected - 32);

                measured += juce::String (found) + " ";

                if (found < 0 || found > expected + 32)
                {
                    worstError = 1 << 20;
                    break;
                }

                worstError = juce::jmax (worstError, std::abs (found - expected));
            }

            check (worstError <= 2, "a grid the bar cannot divide is answered as the pair that does",
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

        /**
            -------- the shape of the two falls -----------------------------------

            Decay and release used to travel in a straight line to their target,
            whatever the header claimed: a level that gives up half its distance in
            half its time. Skins, strings and cymbals stop moving the other way round —
            most of the distance goes first, then a long tail — and that is not a
            nicety. A linear decay is why a short one reads as a gate closing.

            So the test asks the one question that separates a curve from a straight
            line without either implementation having to be known: does every *equal
            slice* of the fall multiply the level by the same factor? On an exponential
            it is the same number by definition; on a straight line it keeps shrinking,
            because the level you are dividing by keeps falling. Four windows a fifth
            of the fall apart give three ratios, and the ratio five time constants
            over the labelled duration predicts — e to the minus one, 0.37 — is what
            they have to land on. That number is `Envelope::fallTimeConstants` showing
            up in the audio, which is the only place it means anything.

            Measured, both shapes, on the same windows and the same fixture: this curve
            reads 0.368, 0.368, 0.368, and the straight line this replaced reads 0.778,
            0.715, 0.603 (`out/smoke-33-linear.txt`, from a build with only the two fall
            cases put back the way they were). The two claims below are the two halves of
            that difference: the band 0.28..0.47, which the curve sits in the middle of
            and whose top edge the line clears by 0.13 (0.603), and the equality of the
            three ratios, which the line fails at 1.29 against a limit of 1.1.

            The windows are read as RMS of a sine. For an exponential that is not an
            approximation worth worrying about: the average over a window of fixed
            width is the value at its centre times a factor that depends on the width
            alone, so the *ratios* are exact whatever the window does to the level.
        */
        const auto fallsAsItSays = [&rmsBetween] (const std::vector<float>& data,
                                                  int origin, double length,
                                                  const juce::String& what)
        {
            const auto halfWindow = 2000; // forty cycles of the 220 Hz fixture
            const auto samples = (int) length;
            float level[4] = {};

            for (int i = 0; i < 4; ++i)
            {
                const auto centre = origin + (int) (((double) i * 0.2 + 0.1) * (double) samples);
                level[i] = rmsBetween (data, centre - halfWindow, centre + halfWindow);
            }

            const auto ratio = [&level] (int from) { return level[from + 1] / juce::jmax (1.0e-9f, level[from]); };

            const auto r1 = ratio (0), r2 = ratio (1), r3 = ratio (2);
            const auto worst = juce::jmax (r1, juce::jmax (r2, r3));
            const auto best = juce::jmin (r1, juce::jmin (r2, r3));

            std::printf ("        %s: level at a tenth, three, five and seven tenths %s, %s, %s, %s\n"
                         "          so per fifth of the travel: %s, %s, %s — a curve predicts 0.368 each,"
                         " and the straight line this replaced measured 0.778, 0.715, 0.603 here\n",
                         what.toRawUTF8(),
                         juce::String (level[0], 5).toRawUTF8(), juce::String (level[1], 5).toRawUTF8(),
                         juce::String (level[2], 5).toRawUTF8(), juce::String (level[3], 5).toRawUTF8(),
                         juce::String (r1, 3).toRawUTF8(), juce::String (r2, 3).toRawUTF8(),
                         juce::String (r3, 3).toRawUTF8());

            const auto band = [&] (float r) { return r > 0.28f && r < 0.47f; };

            check (band (r1) && band (r2) && band (r3),
                   what + " gives up the same share of its level over every equal slice",
                   "per fifth of the travel: " + juce::String (r1, 3) + ", " + juce::String (r2, 3)
                       + ", " + juce::String (r3, 3) + " (0.28..0.47; e^-1 = 0.368, a straight line is 0.603 at its nearest)");

            check (worst < best * 1.1f, what + " is a curve, not a straight line",
                   "widest ratio over narrowest: " + juce::String (worst / juce::jmax (1.0e-9f, best), 3)
                       + " (under 1.1; a straight line measures 1.29 here)");

            check (level[3] < level[0] * 0.15f, what + " is most of the way down in its first third",
                   "seven tenths in sits at " + juce::String (level[3] / juce::jmax (1.0e-9f, level[0]), 4)
                       + " of one tenth in (a straight line measures 0.335 here)");
        };

        // A decay from the top of the envelope to no sustain, with the release at its
        // floor so the voice lets go the moment the fall is over and nothing after it
        // can be mistaken for the curve being measured.
        {
            const auto decaySamples = envelopeSeconds (0.5f, Layout::maxDecaySeconds) * sampleRate;
            auto oneShot = renderNoteWithEnvelope (0.0f, 0.5f, 0.0f, 0.0f, 999, 200);

            fallsAsItSays (oneShot, 0, decaySamples, "a decay to no sustain");
        }

        // The same question of the release, which is the fall the player hears on
        // every pad they lift off. Held to full sustain first, so it starts at unity.
        {
            const auto offAt = 4;
            const auto origin = offAt * 256;
            const auto releaseSamples = envelopeSeconds (0.5f, Layout::maxReleaseSeconds) * sampleRate;
            auto letGo = renderNoteWithEnvelope (0.0f, 0.0f, 1.0f, 0.5f, offAt, 210);

            fallsAsItSays (letGo, origin, releaseSamples, "a release from full sustain");
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

        /**
            Half velocity is half gain, and no more than that.

            The band used to be a hundredth either side of 0.5, which is what it is
            while velocity means gain alone. It also means tone now — see
            `Layout::softestHitCutoffHz` — and a dimmed hit has less snap on its
            front, so its loudest sample lands a shade under the gain-scaled one:
            0.4807 measured here, against 0.5 for the gain on its own. The band still
            separates one application of velocity from two, which is the bug this
            check was written for, and the gain itself is pinned exactly at the top of
            the travel, where the law is bypassed: every full-velocity peak in this
            file is the sample at the trim it asks for, to the last digit.
        */
        check (halfVelocity / unity > 0.44f && halfVelocity / unity < 0.52f,
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
    // Everything so far has been rendered through the internal transport. This and
    // the next section drive the host branch, which is the one a real session uses
    // and the one no test could reach until FakePlayHead existed.
    //
    // `processBlock` renders the block once, then `processMidi` walks the same
    // samples again from the note's offset — and `renderVoices` adds into the buffer
    // and advances every voice as it goes. So any note event at all, even one on a
    // lane with nothing loaded, doubles what the ring-out of an earlier hit puts in
    // the buffer and plays every sounding voice at twice the rate for that block.
    std::printf ("\n[16] a note on an empty lane cannot re-render the block\n");
    {
        auto tone = makeToneWav ("redouble", 997.0, 20000, sampleRate);

        // Two identical processors, because the comparison has to start from the
        // same state on both sides and the second render would inherit the first
        // one's advanced voices.
        const auto renderOneBlock = [&] (bool noteOnEmptyLane)
        {
            GrooveboxProcessor processor;
            processor.prepareToPlay (sampleRate, blockSize);

            juce::String error;
            processor.loadSampleIntoChannel (0, tone, error); // lane 1 stays empty

            clearAllSteps (processor);
            processor.setStep (0, 0, true);
            processor.setChannelSustain (0, 1.0f);

            rearm (processor);
            processor.setPlaying (true);

            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            buffer.clear();

            if (noteOnEmptyLane)
                midi.addEvent (juce::MidiMessage::noteOn (1, Layout::firstMidiNote + 1, 1.0f), 0);

            processor.processBlock (buffer, midi);

            std::vector<float> out ((size_t) blockSize);

            for (int i = 0; i < blockSize; ++i)
                out[(size_t) i] = buffer.getSample (0, i);

            processor.setPlaying (false);
            return out;
        };

        const auto without = renderOneBlock (false);
        const auto with = renderOneBlock (true);

        check (peakOf (without) > 0.01f, "there is a sustaining grid hit to compare against",
               "peak " + juce::String (peakOf (without), 4));

        int differing = 0;
        float worst = 0.0f;

        for (size_t i = 0; i < without.size(); ++i)
        {
            const auto delta = std::abs (with[i] - without[i]);

            if (delta > 1.0e-7f)
                ++differing;

            worst = juce::jmax (worst, delta);
        }

        check (differing == 0, "a note on a lane with no sample leaves the block untouched",
               juce::String (differing) + " of " + juce::String ((int) without.size())
                   + " samples differ, worst " + juce::String (worst, 5)
                   + " (peaks " + juce::String (peakOf (without), 4) + " vs "
                   + juce::String (peakOf (with), 4) + ")");

        // The second half of the same fault: a re-render does not just add level, it
        // also spends the source again, so the hit plays at double speed for a block.
        {
            GrooveboxProcessor processor;
            processor.prepareToPlay (sampleRate, blockSize);

            juce::String error;
            processor.loadSampleIntoChannel (0, tone, error);

            clearAllSteps (processor);
            processor.setStep (0, 0, true);
            processor.setChannelSustain (0, 1.0f);

            rearm (processor);
            processor.setPlaying (true);

            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, Layout::firstMidiNote + 1, 1.0f), 0);
            processor.processBlock (buffer, midi);
            processor.setPlaying (false);

            int stolen = -1;

            for (int i = 0; i < processor.debugActiveVoices(); ++i)
            {
                const auto voice = processor.debugVoice (i);

                if (voice.channel == 0)
                    stolen = (int) voice.position;
            }

            // One block of a 44.1 kHz source at unity consumes 256 source samples,
            // not the 512 a second pass over the same block spends.
            check (stolen >= 0 && stolen <= blockSize + 2,
                   "the grid hit's voice is advanced once per block",
                   "source position " + juce::String (stolen) + " after one "
                       + juce::String (blockSize) + "-sample block");
        }

        tone.deleteFile();
    }

    //======================================================================
    // The engine re-derives its head from the host on every block, which is right,
    // but it decides *which onset comes next* from a counter it only ever moves
    // forward. So when the head jumps — a stop in mid-bar, a scrub, a loop point —
    // the counter is left where it was, and the pattern resumes from the wrong step.
    std::printf ("\n[17] the pattern resumes where the host actually is\n");
    {
        constexpr double stepAt120 = sampleRate / 8.0; // 16ths at 120 bpm

        auto click = makeClickWav ("phase", 200, sampleRate);

        struct Fire
        {
            int step = -1;
            double stepsLater = -1.0;
        };

        /**
            Plays from `fromPpq`, moves the head to `toPpq` without warning, and
            reports the first step that fires afterwards and how far past the move it
            arrived. `blocks` is long enough to cover a whole bar, so a pattern that
            has lost its phase still shows up — as a very late fire.
        */
        const auto fireAfterMove = [&] (double fromPpq, double toPpq, bool stopAcrossTheMove)
        {
            FakePlayHead head;
            GrooveboxProcessor processor;

            head.bpm = 120.0;
            head.ppq = fromPpq;
            head.playing = ! stopAcrossTheMove;

            processor.prepareToPlay (sampleRate, blockSize);
            processor.setPlayHead (&head);

            juce::String error;
            processor.loadSampleIntoChannel (0, click, error);

            clearAllSteps (processor);

            for (int step = 0; step < Layout::numSteps; ++step)
                processor.setStep (0, step, step == (int) std::floor (toPpq * 4.0));

            rearm (processor);

            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;

            // One block at the starting position, so the engine is running from there.
            processor.processBlock (buffer, midi);
            head.advance (blockSize, sampleRate);

            if (stopAcrossTheMove)
            {
                // The host stops here, then moves, then plays again: three separate
                // blocks, which is how it happens in a live set.
                head.playing = false;
                processor.processBlock (buffer, midi);
            }

            const auto previousStep = processor.getCurrentStep();
            head.ppq = toPpq;
            head.playing = true;

            for (int block = 0; block < 400; ++block)
            {
                buffer.clear();
                processor.processBlock (buffer, midi);
                head.advance (blockSize, sampleRate);

                const auto fired = processor.getCurrentStep();

                if (fired != previousStep)
                    return Fire { fired, ((double) (block + 1) * blockSize) / stepAt120 };
            }

            return Fire {};
        };

        // Stopped at 3.6 quarters = step 14.4, so the next onset is step 15, roughly
        // six tenths of a step away. What happens today is step 0 firing on the spot,
        // two steps early and out of phase with the bar the host is sitting in.
        const auto resumed = fireAfterMove (3.6, 3.6, true);

        check (resumed.step == 14 || resumed.step == 15,
               "resuming mid-bar picks up the bar position the host is at",
               "fired step " + juce::String (resumed.step) + ", expected 14 or 15");
        check (resumed.stepsLater >= 0.0 && resumed.stepsLater < 2.0,
               "and it arrives where the grid says, not a step early",
               "arrived " + juce::String (resumed.stepsLater, 2) + " steps after the resume");

        // Scrubbed back to step 3 while playing: the next onset is step 3's, at once.
        // Today the counter is left on step 1, whose onset this bar has already
        // passed, so the pattern waits a whole bar for the next one.
        const auto back = fireAfterMove (0.0, 0.75, false);

        check (back.step == 3 || back.step == 4,
               "scrubbing back resumes on the step the head landed on",
               "fired step " + juce::String (back.step) + ", expected 3 or 4");
        check (back.stepsLater >= 0.0 && back.stepsLater < 2.0,
               "the step after a backward seek is not a bar away",
               "arrived " + juce::String (back.stepsLater, 2) + " steps after the seek");

        // And forward: the head lands on step 10, so step 10 or 11 should sound. A
        // wrong step number here is the pattern playing in the wrong place in the bar
        // for the rest of the pass, which is what a loop point does to a groove.
        const auto forward = fireAfterMove (0.0, 2.5, false);

        check (forward.step == 10 || forward.step == 11,
               "a jump forward lands on the pattern position that belongs there",
               "fired step " + juce::String (forward.step) + ", expected 10 or 11");
        check (forward.stepsLater >= 0.0 && forward.stepsLater < 2.0,
               "and it sounds at once, not after the gap",
               "arrived " + juce::String (forward.stepsLater, 2) + " steps after the jump");

        std::printf ("        measured: resume step %d at %s steps, back step %d at %s, "
                     "forward step %d at %s\n",
                     resumed.step, juce::String (resumed.stepsLater, 2).toRawUTF8(),
                     back.step, juce::String (back.stepsLater, 2).toRawUTF8(),
                     forward.step, juce::String (forward.stepsLater, 2).toRawUTF8());

        click.deleteFile();
    }

    //======================================================================
    // A lane keeps the sample it loaded, and recalling a state loads every lane's
    // file again from disk. Nothing compares them, so each recall spends registry
    // slots the session already has, and the registry is capped: the kit goes quiet
    // one recall before the user finds out why.
    std::printf ("\n[18] recalling the same state does not spend the sample registry\n");
    {
        auto click = makeClickWav ("recall", 200, sampleRate);

        GrooveboxProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);

        juce::String error;

        for (int channel = 0; channel < Layout::numChannels; ++channel)
            processor.loadSampleIntoChannel (channel, click, error);

        juce::MemoryBlock state;
        processor.getStateInformation (state);

        const auto before = processor.getSampleStore().getSampleIds().size();
        check (before > 0, "every lane has a sample before the recall",
               juce::String ((int) before) + " registry entries");

        processor.setStateInformation (state.getData(), (int) state.getSize());

        const auto after = processor.getSampleStore().getSampleIds().size();

        check (after == before, "one recall of the state that was just saved adds no samples",
               juce::String ((int) before) + " before, " + juce::String ((int) after)
                   + " after (" + juce::String (Layout::numChannels) + " lanes per recall)");

        // The consequence that actually bites. Every recall spends twelve slots, so
        // the ceiling arrives after maxSamples / numChannels of them — and past the
        // ceiling nothing loads at all, which is the user dropping a sample and
        // hearing nothing, with no way to see that the registry is what refused it.
        const auto recallsToEnd = SampleStore::maxSamples / Layout::numChannels + 3;

        for (int recall = 0; recall < recallsToEnd; ++recall)
            processor.setStateInformation (state.getData(), (int) state.getSize());

        int silent = 0;

        for (int channel = 0; channel < Layout::numChannels; ++channel)
            if (processor.getSampleStore().getSample (processor.getChannelSampleId (channel)) == nullptr)
                ++silent;

        check (silent == 0, "a recall does not break the lanes that are already loaded",
               juce::String (silent) + " lanes went silent");

        auto fresh = makeClickWav ("recall-fresh", 300, sampleRate);
        error.clear();

        const auto stillLoads = processor.loadSampleIntoChannel (0, fresh, error);

        check (stillLoads, "a new sample can still be loaded after "
                               + juce::String (recallsToEnd) + " recalls",
               "refused: \"" + error + "\", registry holds "
                   + juce::String ((int) processor.getSampleStore().getSampleIds().size())
                   + " of " + juce::String (SampleStore::maxSamples));

        // And the lane it was loaded into still plays what it played before, rather
        // than whatever a half-applied recall leaves behind.
        check (processor.getSampleStore().getSample (processor.getChannelSampleId (0)) != nullptr,
               "lane 1 has a sample after all of that");

        fresh.deleteFile();
        click.deleteFile();
    }

    //======================================================================
    // The grid writes its pattern into the parameter state through the undo
    // manager, so an undo does register — and it does put the old string back. But
    // the engine plays the atomics, and only `applyMetadata` reads the string, which
    // happens when a host loads a state and not when one is undone. The panel and
    // the audio therefore disagree, silently, from the first undo onwards.
    std::printf ("\n[19] undo takes a grid edit back everywhere\n");
    {
        GrooveboxProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);

        clearAllSteps (processor);
        processor.setStep (0, 0, true);
        processor.setStepVelocity (0, 0, 0.5f);

        processor.beginTransaction ("paint step 3");
        processor.setStep (0, 3, true);

        check (processor.isStepOn (0, 3), "the edit itself happened");

        // The cell as the *stored record* sees it: the step's own bytes, pulled out of
        // the payload rather than read back through the processor's decoder.
        // Comparing the string against the grid is the whole point of the section, and
        // re-using the production reader would only be the same code agreeing with
        // itself.
        //
        // The stride is derived from the prefix the way a decoder has to derive it, in
        // test-local arithmetic: a section that hard-coded v2's two bytes would agree
        // with a build that still wrote two bytes and say nothing about one that does
        // not. What is fixed here is the expectation, not the width.
        struct StoredStep { bool on = false; int level = -1; int chance = 255; };

        auto storedStep = [&processor] (int channel, int step) -> StoredStep
        {
            const auto pattern = processor.getValueTreeState().state
                                     .getProperty ("pattern").toString();

            const auto stride = pattern.startsWith ("v3:") ? 3
                              : pattern.startsWith ("v2:") ? 2 : 0;

            if (stride == 0)
                return {};

            juce::MemoryBlock bytes;
            juce::MemoryOutputStream sink (bytes, false);

            const auto body = pattern.fromFirstOccurrenceOf (":", false, false);
            const auto at = (size_t) (channel * Layout::numSteps + step) * (size_t) stride;

            if (! juce::Base64::convertFromBase64 (sink, body)
                || bytes.getSize() < at + (size_t) stride)
                return {};

            const auto* raw = static_cast<const juce::uint8*> (bytes.getData());

            return { raw[at] != 0, (int) raw[at + 1],
                     stride > 2 ? (int) raw[at + 2] : 255 };
        };

        check (storedStep (0, 3).on && storedStep (0, 3).level == 127,
               "the edit is in the stored pattern too",
               "the record holds " + juce::String ((int) storedStep (0, 3).on) + "/"
                   + juce::String (storedStep (0, 3).level));

        const auto undid = processor.getUndoManager().undo();

        check (undid, "undo reports that it did something");
        check (! storedStep (0, 3).on, "undo takes the stored pattern back",
               "the record still has the step switched on");
        check (! processor.isStepOn (0, 3), "undo takes the engine's pattern back too",
               "the cell is cleared in the state but still fires");

        processor.getUndoManager().redo();
        check (processor.isStepOn (0, 3), "redo puts the step back on in the engine",
               "the record has " + juce::String ((int) storedStep (0, 3).on));

        // An undo must not disturb the neighbouring edit either: step 0 predates the
        // transaction, so it stays on through both — and at the level it was set to,
        // which is the half of the record the old two-digit rows could not hold apart
        // from the flag.
        check (processor.isStepOn (0, 0), "the step from before the transaction survives it");

        check (storedStep (0, 0).on && std::abs (storedStep (0, 0).level - 64) <= 1,
               "and keeps the level it was given, in the record and not only on the panel",
               juce::String ((int) storedStep (0, 0).on) + "/" + juce::String (storedStep (0, 0).level)
                   + " against the 0.5 it was set to");
    }

    //======================================================================
    // A voice that stops is switched off where it stands: at the end of its source
    // the sample index runs out and the voice goes inactive mid-cycle, and when all
    // 64 slots are busy the longest-running one is overwritten without a ramp. Both
    // leave a step in the output, which is a click.
    //
    // The measurement is the sample-to-sample slew. A steady tone has a known, small
    // one, so anything several times larger can only be a discontinuity the engine
    // put there — and because the numbers are printed either way, the run log carries
    // the size of the click rather than only a verdict.
    std::printf ("\n[20] nothing cuts a sounding waveform mid-cycle\n");
    {
        constexpr double toneFrequency = 997.0;

        // The fixture's own slew, from the same formula makeToneWav writes. Every
        // limit below is a multiple of it, so the gate follows the signal it measures
        // instead of an absolute constant that would have to be re-guessed for a
        // different tone.
        std::vector<float> reference (4096);

        for (size_t i = 0; i < reference.size(); ++i)
            reference[i] = (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                             * toneFrequency * (double) i / sampleRate);

        const auto sourceSlew = maxDelta (reference);
        const auto limit = sourceSlew * 1.5f;

        std::printf ("        reference: the fixture's own slew is %s, so the limit is %s\n",
                     juce::String (sourceSlew, 4).toRawUTF8(), juce::String (limit, 4).toRawUTF8());

        check (sourceSlew > 0.1f && sourceSlew < 0.2f, "the tone fixture's own slew is what it should be",
               juce::String (sourceSlew, 4));

        /**
            Renders a single sustaining hit from sources of several lengths, so the
            point where the source runs out lands at a different phase of the wave
            each time. A fade has to work at every phase; testing one of them would
            pass by luck.
        */
        {
            int breaches = 0;
            float worst = 0.0f;
            int worstAt = -1;
            float lowestPeak = 1.0e9f;
            float expectedPeak = 0.0f;

            for (int variant = 0; variant < 8; ++variant)
            {
                const auto length = 4000 + variant * 517; // 517 is coprime with a cycle
                auto tone = makeToneWav ("endsrc", toneFrequency, length, sampleRate);

                GrooveboxProcessor processor;
                processor.prepareToPlay (sampleRate, blockSize);

                juce::String error;
                processor.loadSampleIntoChannel (0, tone, error);

                clearAllSteps (processor);
                processor.setStep (0, 0, true);
                processor.setChannelSustain (0, 1.0f);

                rearm (processor);
                processor.setPlaying (true);
                auto out = render (processor, length + 4 * blockSize, blockSize, sampleRate);
                processor.setPlaying (false);

                const auto report = worstSlew (out.left);
                const auto peak = peakOf (out.left);

                // The source at this processor's own trim, which nothing here moves.
                expectedPeak = peakOf (reference)
                                 * std::pow (10.0f, processor.getMasterGainDb() / 20.0f);

                if (peak < lowestPeak)
                    lowestPeak = peak;

                if (report.worst > worst)
                {
                    worst = report.worst;
                    worstAt = report.atSample;
                }

                if (report.worst > limit)
                    ++breaches;

                std::printf ("        source %5d samples: worst slew %s at sample %d, peak %s, rms %s\n",
                             length, juce::String (report.worst, 4).toRawUTF8(), report.atSample,
                             juce::String (peak, 4).toRawUTF8(), juce::String (rmsOf (out.left), 4).toRawUTF8());

                tone.deleteFile();
            }

            check (breaches == 0, "a hit that runs out of source never jumps the waveform",
                   juce::String (breaches) + " of 8 sources cut mid-cycle, worst "
                       + juce::String (worst, 4) + " at sample " + juce::String (worstAt)
                       + " (limit " + juce::String (limit, 4) + ")");

            // The other half of the claim. A taper that satisfied the gate above by
            // taking level off the whole hit would be a quieter machine wearing the
            // same pattern, so the loudest sample of every variant has to still be
            // the source at full trim.
            check (lowestPeak > expectedPeak * 0.98f,
                   "the end of a hit is what fades, not the hit",
                   "lowest peak across the 8 sources " + juce::String (lowestPeak, 4)
                       + ", source at the default trim peaks at " + juce::String (expectedPeak, 4));
        }

        /**
            The steal, isolated.

            Eleven lanes of digital silence fill the voice pool behind one lane of
            tone, so the output carries exactly one audible waveform and the sixty-fifth
            trigger has nowhere to put a voice except over the oldest one — the tone.

            Silence is what makes this measurable. With 64 voices of the same tone
            summing, the block's own slew is 64 times a single voice's and a one-voice
            jump disappears into it; that is also the honest scale of the fault, and
            why a steal on a full kit is far less audible than a steal on a single hit.

            Everything here runs off the grid rather than off MIDI on purpose: an
            incoming note is a discontinuity of its own — it starts a voice on a
            sample no onset was waiting on — and what would be measured would be
            that, not the steal. Section 16 checks the note's arrival, [14b] checks
            what a note off does to a hit the grid started, and neither of them can
            fill the pool, which is the only way a steal happens.
        */
        {
            auto tone = makeToneWav ("steal", toneFrequency, 40000, sampleRate);
            auto hush = makeSilenceWav ("steal", 40000, sampleRate);

            GrooveboxProcessor processor;
            processor.prepareToPlay (sampleRate, blockSize);

            juce::String error;
            processor.loadSampleIntoChannel (0, tone, error);

            for (int channel = 1; channel < Layout::numChannels; ++channel)
                processor.loadSampleIntoChannel (channel, hush, error);

            clearAllSteps (processor);

            // One audible hit a bar, eleven silent ones on every step: by the fifth
            // step the pool is full, and the tone is the oldest thing in it.
            processor.setStep (0, 0, true);

            for (int channel = 1; channel < Layout::numChannels; ++channel)
            {
                processor.setChannelSustain (channel, 1.0f);

                for (int step = 0; step < Layout::numSteps; ++step)
                    processor.setStep (channel, step, true);
            }

            processor.setChannelSustain (0, 1.0f);

            rearm (processor);
            processor.setPlaying (true);

            // Two bars, so the second bar's hit gets stolen as well and one lucky
            // phase cannot carry the result.
            auto out = render (processor, (int) (exactStepSamples * Layout::numSteps * 2.0),
                               blockSize, sampleRate);

            processor.setPlaying (false);

            const auto report = worstSlew (out.left);
            const auto peak = peakOf (out.left);
            const auto stealTrim = std::pow (10.0f, processor.getMasterGainDb() / 20.0f);

            std::printf ("        steal: worst slew %s at sample %d (step %s), peak %s, rms %s,"
                         " active voices %d of %d\n",
                         juce::String (report.worst, 4).toRawUTF8(), report.atSample,
                         juce::String ((float) report.atSample / (float) exactStepSamples, 2).toRawUTF8(),
                         juce::String (peak, 4).toRawUTF8(), juce::String (rmsOf (out.left), 4).toRawUTF8(),
                         processor.debugActiveVoices(), Layout::maxVoices);

            check (processor.debugActiveVoices() == Layout::maxVoices,
                   "the voice pool really was full, so a steal was forced",
                   juce::String (processor.debugActiveVoices()) + " active");
            check (report.worst <= limit, "stealing a voice fades the old one out",
                   "worst slew " + juce::String (report.worst, 4) + " at sample "
                       + juce::String (report.atSample) + " (limit " + juce::String (limit, 4) + ")");

            // The hit that gets to keep its slot is the one that must not have lost
            // anything: a fade on the outgoing voice that also dragged the level of
            // the incoming one down would show up here, not in the slew.
            check (peak > peakOf (reference) * stealTrim * 0.98f,
                   "a steal takes the slot, not the level",
                   "peak " + juce::String (peak, 4) + " against the source at trim, which peaks at "
                       + juce::String (peakOf (reference) * stealTrim, 4));

            tone.deleteFile();
            hush.deleteFile();
        }

        /**
            The third way a sounding hit ends: the transport stopping, or a note off.

            A voice that is holding is let go rather than switched off, and letting go
            is the envelope's release — so the thing standing between a stop and a
            click is the release ramp. The shortest release the parameter can ask for
            is where that ramp is thinnest, which is why both settings are measured.

            This is a guard, not a gate: it passes on the code it was written against,
            because the release path already ramped. It is here for the day a choke, a
            floor on the envelope, or a rewrite of the release makes a let-go into a
            cut — the failure that would otherwise be heard first and measured never.
        */
        {
            auto tone = makeToneWav ("release", toneFrequency, 20000, sampleRate);

            const auto stopSlew = [&] (float releaseNormalised)
            {
                GrooveboxProcessor processor;
                processor.prepareToPlay (sampleRate, blockSize);

                juce::String error;
                processor.loadSampleIntoChannel (0, tone, error);

                clearAllSteps (processor);
                processor.setStep (0, 0, true);
                processor.setChannelSustain (0, 1.0f);
                processor.setChannelRelease (0, releaseNormalised);

                rearm (processor);
                processor.setPlaying (true);

                // Two steps, so the hit is well into its decay before it is let go.
                auto holding = render (processor, (int) (exactStepSamples * 2.0), blockSize, sampleRate);

                processor.setPlaying (false);
                auto after = render (processor, 4 * blockSize, blockSize, sampleRate);

                // One measurement across both renders: the moment the transport
                // stops is the last sample of the first and the first of the second,
                // and a cut there is exactly what is being looked for.
                std::vector<float> continuous = holding.left;
                continuous.insert (continuous.end (), after.left.begin(), after.left.end());

                return worstSlew (continuous);
            };

            const auto atDefault = stopSlew (Layout::defaultRelease);
            const auto atShortest = stopSlew (0.0f);

            /**
                The same floor seen from the other control that falls: a one-shot with
                its decay at nothing, which is the shortest envelope a lane can be set
                to and the shape a drum kit is left in.
            */
            const auto oneShotSlew = [&] (float decayNormalised)
            {
                GrooveboxProcessor processor;
                processor.prepareToPlay (sampleRate, blockSize);

                juce::String error;
                processor.loadSampleIntoChannel (0, tone, error);

                clearAllSteps (processor);
                processor.setStep (0, 0, true);
                processor.setChannelSustain (0, 0.0f);
                processor.setChannelDecay (0, decayNormalised);
                processor.setChannelRelease (0, Layout::defaultRelease);

                rearm (processor);
                processor.setPlaying (true);

                auto out = render (processor, (int) (exactStepSamples * 20.0), blockSize, sampleRate);
                processor.setPlaying (false);

                return worstSlew (out.left);
            };

            const auto atNoDecay = oneShotSlew (0.0f);

            std::printf ("        stopped while holding: worst slew %s at the default release, %s at the shortest\n"
                         "        one-shot at no decay: worst slew %s\n",
                         juce::String (atDefault.worst, 4).toRawUTF8(),
                         juce::String (atShortest.worst, 4).toRawUTF8(),
                         juce::String (atNoDecay.worst, 4).toRawUTF8());

            check (atDefault.worst <= limit, "stopping lets a holding voice go the way an envelope does",
                   "worst slew " + juce::String (atDefault.worst, 4) + " at sample "
                       + juce::String (atDefault.atSample) + " (limit " + juce::String (limit, 4) + ")");
            check (atShortest.worst <= limit, "a release of nothing is still a ramp, not a cut",
                   "worst slew " + juce::String (atShortest.worst, 4) + " at sample "
                       + juce::String (atShortest.atSample) + " (limit " + juce::String (limit, 4) + ")");
            check (atNoDecay.worst <= limit, "a decay of nothing still falls, it does not stop",
                   "worst slew " + juce::String (atNoDecay.worst, 4) + " at sample "
                       + juce::String (atNoDecay.atSample) + " (limit " + juce::String (limit, 4) + ")");

            tone.deleteFile();
        }

        /**
            Every corner the curve turns, at every setting the falling parameters reach.

            A straight line has no corners. An exponential has two per fall: the moment
            it is stepped onto, where it is falling fastest of all, and the moment its
            counted duration ends and it is stepped onto its target over whatever
            residual was left. Both are places where the level can move further in one
            sample than the waveform does on its own, which is a click — and the
            release corner has the extra business of starting from wherever the note
            happened to be let go, at whatever phase.

            So all three settings the corners matter at — nothing, middle, maximum —
            for attack, decay, sustain and release: 81 renders, each measured across
            the whole of itself rather than over the window the theory is comfortable
            in. The combinations are swept rather than sampled because the failure
            being looked for is the one that only bites at a particular pairing, and
            there is no way to know in advance which.
        */
        {
            auto tone = makeToneWav ("corners", toneFrequency, 60000, sampleRate);
            const float corner[] = { 0.0f, 0.5f, 1.0f };

            int breaches = 0;
            float worst = 0.0f;
            int renders = 0;
            float highestPeak = 0.0f;
            float levelTarget = 0.0f;
            juce::String offenders;

            for (const auto attack : corner)
            {
                for (const auto decay : corner)
                {
                    for (const auto sustainLevel : corner)
                    {
                        for (const auto release : corner)
                        {
                            GrooveboxProcessor processor;
                            processor.prepareToPlay (sampleRate, blockSize);

                            juce::String error;
                            processor.loadSampleIntoChannel (0, tone, error);

                            clearAllSteps (processor);
                            processor.setStep (0, 0, true);
                            processor.setChannelAttack (0, attack);
                            processor.setChannelDecay (0, decay);
                            processor.setChannelSustain (0, sustainLevel);
                            processor.setChannelRelease (0, release);

                            rearm (processor);
                            processor.setPlaying (true);

                            // Two steps of the pattern, then the transport stops and
                            // lets every sounding lane go with it. One render walks a
                            // hit through attack, decay, whatever sustain it lands on
                            // and the release, in the order a player meets them.
                            auto holding = render (processor, (int) (exactStepSamples * 2.0),
                                                   blockSize, sampleRate);

                            processor.setPlaying (false);
                            auto after = render (processor, 8 * blockSize, blockSize, sampleRate);

                            std::vector<float> continuous = holding.left;
                            continuous.insert (continuous.end (), after.left.begin(), after.left.end());

                            const auto report = worstSlew (continuous);
                            const auto peak = peakOf (continuous);

                            ++renders;

                            // The corners must cost nothing to the hits that are not
                            // ending: A at nothing and sustain at full holds at unity
                            // for the whole render, so its peak is the source at the
                            // trim and no less.
                            if (attack == 0.0f && sustainLevel == 1.0f)
                            {
                                highestPeak = juce::jmax (highestPeak, peak);
                                levelTarget = peakOf (reference)
                                                * std::pow (10.0f, processor.getMasterGainDb() / 20.0f);
                            }

                            if (report.worst > limit)
                            {
                                ++breaches;
                                offenders << "\n          a" << attack << " d" << decay
                                          << " s" << sustainLevel << " r" << release
                                          << ": slew " << report.worst << " at sample "
                                          << report.atSample;
                            }

                            if (report.worst > worst)
                                worst = report.worst;
                        }
                    }
                }
            }

            std::printf ("        %d envelope corner renders: worst slew %s, limit %s,"
                         " loudest hold at %s against the source at the trim, %s\n",
                         renders, juce::String (worst, 4).toRawUTF8(), juce::String (limit, 4).toRawUTF8(),
                         juce::String (highestPeak, 4).toRawUTF8(), juce::String (levelTarget, 4).toRawUTF8());

            check (renders == 81, "the corner sweep really did run the whole matrix",
                   juce::String (renders) + " renders");
            check (breaches == 0, "every corner of the envelope is a curve, not a step",
                   juce::String (breaches) + " of " + juce::String (renders)
                       + " combinations jumped the waveform, worst " + juce::String (worst, 4)
                       + " against a limit of " + juce::String (limit, 4) + offenders);
            check (highestPeak > levelTarget * 0.98f,
                   "the corners take nothing off a hit that is not ending",
                   "loudest hold in the sweep " + juce::String (highestPeak, 4)
                       + ", source at the default trim peaks at " + juce::String (levelTarget, 4));

            tone.deleteFile();
        }
    }

    //======================================================================
    // Section 15 checks that one host-written parameter reaches the engine. This
    // checks every parameter there is, because the id is the only thing that says
    // which lane and which control moved: parsing it wrongly is silent, and it is
    // exactly what a rewrite of that parsing would get wrong. It passes on the
    // current code by design — it is the gate on the fix, not on the defect.
    std::printf ("\n[21] every parameter id reaches the lane and field it names\n");
    {
        GrooveboxProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);

        auto& apvts = processor.getValueTreeState();
        juce::String problems;
        int missing = 0;

        const auto write = [&] (const juce::String& id, float value)
        {
            auto* param = apvts.getParameter (id);

            if (param == nullptr)
            {
                ++missing;
                return;
            }

            param->setValueNotifyingHost (param->convertTo0to1 (value));
        };

        const auto agrees = [&] (bool equal, const juce::String& what)
        {
            if (! equal)
                problems << " " << what;
        };

        const auto near = [] (float a, float b) { return std::abs (a - b) < 1.0e-4f; };

        write (Params::masterGainId, -6.5f);
        write (Params::swingId, 0.42f);
        write (Params::swingGridId, 2.0f);
        write (Params::tempoId, 137.0f);
        write (Params::mutateId, 0.5f);
        write (Params::seedId, 12345.0f);

        for (int channel = 0; channel < Layout::numChannels; ++channel)
        {
            const auto c = (float) channel;

            // Values that differ per lane and per field, so a mapping that is off by
            // one lane or one field shows up as a disagreement rather than by luck.
            const auto level = 0.1f + 0.05f * c;
            const auto pitch = channel - 5;
            const auto drop = channel % (Params::pitchDropMax + 1);
            const auto attack = 0.1f + 0.01f * c;
            const auto decay = 0.2f + 0.02f * c;
            const auto sustain = 0.3f + 0.01f * c;
            const auto release = 0.4f + 0.01f * c;
            const auto muted = (channel % 2) == 0;
            const auto choke = channel % (Layout::numChokeGroups + 1);

            write (Params::levelId (channel), level);
            write (Params::pitchId (channel), (float) pitch);
            write (Params::pitchDropId (channel), (float) drop);
            write (Params::attackId (channel), attack);
            write (Params::decayId (channel), decay);
            write (Params::sustainId (channel), sustain);
            write (Params::releaseId (channel), release);
            write (Params::muteId (channel), muted ? 1.0f : 0.0f);
            write (Params::chokeId (channel), (float) choke);

            agrees (near (processor.getChannelLevel (channel), level), "ch" + juce::String (channel) + " level");
            agrees (processor.getChannelPitch (channel) == pitch, "ch" + juce::String (channel) + " pitch");
            agrees (processor.getChannelPitchDrop (channel) == drop,
                    "ch" + juce::String (channel) + " pitch drop");
            agrees (near (processor.getChannelAttack (channel), attack), "ch" + juce::String (channel) + " attack");
            agrees (near (processor.getChannelDecay (channel), decay), "ch" + juce::String (channel) + " decay");
            agrees (near (processor.getChannelSustain (channel), sustain), "ch" + juce::String (channel) + " sustain");
            agrees (near (processor.getChannelRelease (channel), release), "ch" + juce::String (channel) + " release");
            agrees (processor.isChannelMuted (channel) == muted, "ch" + juce::String (channel) + " mute");

            // Last, so it sees the mute write too: if the choke id were parsed as
            // something else, the field it landed on would disagree as well.
            if (processor.getChannelChokeGroup (channel) != choke)
            {
                auto* param = apvts.getParameter (Params::chokeId (channel));
                const auto held = param != nullptr ? param->convertFrom0to1 (param->getValue()) : -1.0f;
                const auto top = param != nullptr ? param->getNormalisableRange().convertFrom0to1 (1.0f) : -1.0f;

                problems << " ch" << channel << " asked for " << choke
                         << ", engine says " << processor.getChannelChokeGroup (channel)
                         << ", parameter holds " << held << ", its top of range is " << top;
            }
        }

        // The count is derived, not remembered. A hardcoded number was the only
        // thing that said how many parameters there were, so it went stale in
        // silence when the choke group joined them; this pair says the table has
        // what it claims and that nothing was left out of the table. The global half
        // comes from `Params::globalIds()`, whose size is part of its type, so a
        // control that joins the layout has to be written into both lists or the two
        // checks below disagree instead of passing.
        const auto expectedParams = (int) Params::globalIds().size() + Params::numLaneTargets;
        const auto hostedParams = (int) processor.getParameters().size();

        check (missing == 0, "all " + juce::String (expectedParams) + " parameters are present",
               juce::String (missing) + " not found");
        check (hostedParams == expectedParams,
               "and every parameter the host is offered is in that table",
               "host offers " + juce::String (hostedParams));
        check (problems.isEmpty(), "each id moved the field it names, on that lane", problems);
        check (near (processor.getSwing(), 0.42f), "swing reaches the engine",
               juce::String (processor.getSwing(), 4));
        check (processor.getSwingGrid() == 2, "the swing grid reaches the engine",
               juce::String (processor.getSwingGrid()));
        check (near (processor.getMasterGainDb(), -6.5f), "the master trim reaches the bus",
               juce::String (processor.getMasterGainDb(), 3));

        // Both of these read the engine's own atom rather than a panel value, so the
        // pair proves the whole route: parameter -> parameterChanged -> SequencerState.
        check (near (processor.getMutate(), 0.5f), "mutate reaches the engine",
               juce::String (processor.getMutate(), 4));
        check (processor.getSeed() == 12345, "the seed reaches the engine",
               juce::String (processor.getSeed()));

        // Tempo has no getter of its own — the engine reads it as it runs — so it is
        // checked through one rendered block.
        processor.setPlaying (true);
        render (processor, blockSize, blockSize, sampleRate);
        processor.setPlaying (false);

        check (std::abs (processor.getCurrentTempo() - 137.0) < 0.01,
               "the tempo parameter is what the engine plays at",
               juce::String ((float) processor.getCurrentTempo(), 3));
    }

    //======================================================================
    // A choke group is the one termination that is not the voice's own: a hit in a
    // group cuts short every hit that is already sounding in that group, which is
    // how a closed hi-hat stops an open one. Four renders of the same pattern say
    // what that means in audio:
    //
    //   free       — no lane is in a group. The reference every other one is held
    //                against, so the level claim is measured, not assumed.
    //   choked     — the held hit and the cutting hit share a group.
    //   otherGroup — they are in different groups, so nothing is cut.
    //   cutter     — as choked, but the cutting hit is audible too.
    //
    // The cutting hit is digital silence unless a run needs to hear it, because the
    // measurement is the level of the thing being choked: with a second waveform in
    // there, a sum or a cancellation would be indistinguishable from a fade.
    std::printf ("\n[22] a choke fades the hit it is paired with, and only that one\n");
    {
        constexpr double toneFrequency = 997.0;
        constexpr int sourceLength = 100000;   // longer than a bar: nothing here may
                                               // be the source running out
        constexpr int barSamples = (int) (exactStepSamples * Layout::numSteps);
        constexpr int chokeStep = 4;

        const auto onset = (int) std::llround (exactStepSamples * (double) chokeStep);
        const auto fadeSamples = (int) std::llround (Layout::chokeFadeSeconds * sampleRate);

        std::vector<float> reference ((size_t) fadeSamples * 2);

        for (size_t i = 0; i < reference.size(); ++i)
            reference[i] = (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                             * toneFrequency * (double) i / sampleRate);

        // The same ruler [20] uses: the fixture's own slew, times 1.5.
        const auto limit = maxDelta (reference) * 1.5f;

        auto held = makeToneWav ("choke", toneFrequency, sourceLength, sampleRate);
        auto choker = makeSilenceWav ("choke", sourceLength, sampleRate);

        /**
            The cutter run needs a source of its own, and it has to be a different
            pitch. Two hits of the same sine, one starting 22050 samples into the
            other, are exactly out of phase — half a cycle apart at this frequency —
            and the pair cancels, which would be read as "the cut voice is gone"
            when nothing of the sort happened.
        */
        auto cutterTone = makeToneWav ("choke-cut", 1489.0, sourceLength, sampleRate);

        const auto run = [&] (int heldGroup, int chokerGroup, const juce::File& chokerSource)
        {
            GrooveboxProcessor processor;
            processor.prepareToPlay (sampleRate, blockSize);

            juce::String error;
            processor.loadSampleIntoChannel (0, held, error);
            processor.loadSampleIntoChannel (1, chokerSource, error);

            clearAllSteps (processor);

            // Both lanes hold, so the only thing that can bring a level down in the
            // window being measured is the fade.
            processor.setChannelSustain (0, 1.0f);
            processor.setChannelSustain (1, 1.0f);

            processor.setStep (0, 0, true);           // the hit that holds
            processor.setStep (1, chokeStep, true);   // the one that cuts it

            processor.setChannelChokeGroup (0, heldGroup);
            processor.setChannelChokeGroup (1, chokerGroup);

            rearm (processor);
            processor.setPlaying (true);
            auto out = render (processor, barSamples, blockSize, sampleRate);
            processor.setPlaying (false);

            return out;
        };

        const auto freeRender = run (0, 0, choker);
        const auto choked = run (1, 1, choker);
        const auto otherGroup = run (1, 2, choker);
        const auto cutter = run (1, 1, cutterTone);

        const auto full = peakOf (freeRender.left);

        // Quiet from here on, if the fade did its job: the window plus a block, so a
        // fade that overshoots its own length by a few samples is still seen.
        const auto tailStart = onset + fadeSamples + blockSize;

        std::printf ("        held hit peaks at %s; step %d starts at sample %d, fade %d samples (%.0f ms)\n",
                     juce::String (full, 4).toRawUTF8(), chokeStep, onset, fadeSamples,
                     Layout::chokeFadeSeconds * 1000.0);
        std::printf ("        unchoked tail peaks at %s, choked tail at %s, other group at %s, audible cutter at %s\n",
                     juce::String (peakFrom (freeRender.left, tailStart), 4).toRawUTF8(),
                     juce::String (peakFrom (choked.left, tailStart), 5).toRawUTF8(),
                     juce::String (peakFrom (otherGroup.left, tailStart), 4).toRawUTF8(),
                     juce::String (peakFrom (cutter.left, tailStart), 4).toRawUTF8());

        const auto before = firstDifference (freeRender.left, choked.left, 0, onset);

        check (before < 0, "a choke changes nothing before the hit that causes it",
               before < 0 ? "the two are identical for " + juce::String (onset) + " samples"
                          : "they part at sample " + juce::String (before)
                                + " (" + juce::String (freeRender.left[(size_t) before], 5)
                                + " against " + juce::String (choked.left[(size_t) before], 5) + ")");

        // The precondition the code without a choke cannot meet: the held hit was
        // still at full level in the very window the choked one is gone from.
        check (peakFrom (freeRender.left, tailStart) > full * 0.9f,
               "and it was still sounding until the cut",
               "unchoked tail peaks at " + juce::String (peakFrom (freeRender.left, tailStart), 4)
                   + " of " + juce::String (full, 4));

        check (peakFrom (choked.left, tailStart) < full * 0.01f,
               "the choked hit is gone by the end of the fade",
               "tail peak " + juce::String (peakFrom (choked.left, tailStart), 6) + ", which is "
                   + juce::String (peakFrom (choked.left, tailStart) / juce::jmax (1.0e-9f, full) * 100.0f, 3)
                   + "% of the hit");

        const auto lastLoud = lastAbove (choked.left, full * 0.01f);

        check (std::abs (lastLoud - (onset + fadeSamples)) <= 60,
               "the fade is the length it says it is",
               "the last sample above 1% is " + juce::String (lastLoud - onset)
                   + " samples after the cut, the fade is " + juce::String (fadeSamples));

        const auto report = worstSlew (choked.left);

        check (report.worst <= limit, "a choke is a ramp, not a cut",
               "worst slew " + juce::String (report.worst, 4) + " at sample "
                   + juce::String (report.atSample) + " (limit " + juce::String (limit, 4) + ")");

        const auto anywhere = firstDifference (freeRender.left, otherGroup.left, 0, barSamples);

        check (anywhere < 0, "a group only reaches the lanes that are in it",
               anywhere < 0 ? "the whole bar is identical to the ungrouped render"
                            : "a different group changed sample " + juce::String (anywhere));

        // One measurement, two halves of the claim. The tail carries the hit that
        // arrived at step 4, so it has to be a voice at full level — not nothing,
        // which would mean the choke ate the incoming hit as well, and not two
        // voices, which would mean it never let go of the old one.
        const auto cutterTail = peakFrom (cutter.left, tailStart);

        check (cutterTail > full * 0.5f && cutterTail < full * 1.4f,
               "a choke takes the voice that was sounding, not the one arriving",
               "tail peak " + juce::String (cutterTail, 4) + "; one voice sits at "
                   + juce::String (full, 4) + ", two would reach "
                   + juce::String (full * 2.0f, 4));

        held.deleteFile();
        choker.deleteFile();
        cutterTone.deleteFile();
    }

    //======================================================================
    // Velocity used to mean one thing: how loud. A drum answers a harder hit with
    // more than that — the beater clicks, the shell rings, the top end comes up — so
    // every velocity of one sample was the same recording turned down rather than a
    // different way of playing it. Velocity now opens a one-pole on the way through,
    // and what is checked here is the half of that a fader could not have produced.
    std::printf ("\n[23] a harder hit is brighter, and a full-velocity hit is untouched\n");
    {
        const auto source = makeNoiseSamples (30000);
        auto noise = makeNoiseWav ("brightness", source, sampleRate);

        /**
            The window the balance is read from.

            Two tenths of a second inside a hit whose envelope was deliberately made
            flat — decay at its floor onto full sustain, so the level has arrived at
            unity and stays there — which leaves the velocity law as the only thing in
            the render that can move energy between the two bands.
        */
        constexpr int windowFrom = 2000, windowTo = 22000;

        const auto strike = [&] (float level, float velocity)
        {
            GrooveboxProcessor processor;
            processor.prepareToPlay (sampleRate, blockSize);

            juce::String error;
            processor.loadSampleIntoChannel (0, noise, error);

            clearAllSteps (processor);
            processor.setStep (0, 0, true);
            processor.setStepVelocity (0, 0, velocity);
            processor.setChannelLevel (0, level);
            processor.setChannelDecay (0, 0.0f);
            processor.setChannelSustain (0, 1.0f);

            rearm (processor);
            processor.setPlaying (true);

            auto out = render (processor, windowTo + 4 * blockSize, blockSize, sampleRate);

            processor.setPlaying (false);
            return out;
        };

        const auto full    = strike (1.0f, 1.0f);
        const auto fader   = strike (0.25f, 1.0f);  // the same gain as `quarter`, no velocity change
        const auto half    = strike (1.0f, 0.5f);
        const auto quarter = strike (1.0f, 0.25f);
        const auto ghost   = strike (1.0f, 1.0f / 16.0f);

        const auto tilt = [&] (const RenderResult& hit) { return tiltOf (hit.left, windowFrom, windowTo); };
        const auto peak = [] (const RenderResult& hit) { return peakOf (hit.left); };

        const auto sourceTilt = tiltOf (source, windowFrom, windowTo);
        const auto trim = std::pow (10.0f, Layout::defaultMasterGainDb / 20.0f);
        const auto expectedFull = peakOf (source) * trim;

        std::printf ("        fixture: tilt %s (broadband is about 31), peak %s, at the trim %s\n",
                     juce::String (sourceTilt, 2).toRawUTF8(), juce::String (peakOf (source), 4).toRawUTF8(),
                     juce::String (expectedFull, 4).toRawUTF8());
        std::printf ("        level   velocity      peak      rms     tilt (top / bottom)\n"
                     "          %5s     %5s   %8s  %8s  %8s\n"
                     "          %5s     %5s   %8s  %8s  %8s   <- the fader, not the stick\n"
                     "          %5s     %5s   %8s  %8s  %8s\n"
                     "          %5s     %5s   %8s  %8s  %8s\n"
                     "          %5s     %5s   %8s  %8s  %8s\n",
                     "1.00", "1.00", juce::String (peak (full), 5).toRawUTF8(),
                     juce::String (rmsOf (full.left), 5).toRawUTF8(), juce::String (tilt (full), 2).toRawUTF8(),
                     "0.25", "1.00", juce::String (peak (fader), 5).toRawUTF8(),
                     juce::String (rmsOf (fader.left), 5).toRawUTF8(), juce::String (tilt (fader), 2).toRawUTF8(),
                     "1.00", "0.50", juce::String (peak (half), 5).toRawUTF8(),
                     juce::String (rmsOf (half.left), 5).toRawUTF8(), juce::String (tilt (half), 2).toRawUTF8(),
                     "1.00", "0.25", juce::String (peak (quarter), 5).toRawUTF8(),
                     juce::String (rmsOf (quarter.left), 5).toRawUTF8(), juce::String (tilt (quarter), 2).toRawUTF8(),
                     "1.00", "0.06", juce::String (peak (ghost), 5).toRawUTF8(),
                     juce::String (rmsOf (ghost.left), 5).toRawUTF8(), juce::String (tilt (ghost), 2).toRawUTF8());

        // The precondition everything else rests on: the fixture has a top end for a
        // lowpass to take away. A tone would let every claim below pass on a law that
        // does nothing.
        check (sourceTilt > 5.0f, "the noise fixture really is broadband",
               "tilt " + juce::String (sourceTilt, 2) + " (a flat spectrum is about 31, a single tone about 0)");

        /**
            The top of the travel is the recording.

            This is the claim that keeps the rest honest: a hit struck as hard as the
            parameter can ask for comes out at the source's own level and the source's
            own balance, to the last digit — so adding a law about colour cannot have
            quietly retuned the kit, and the 170 checks above that measure a
            full-velocity hit are measuring what they measured yesterday.
        */
        check (std::abs (peak (full) / expectedFull - 1.0f) < 0.002f,
               "a full-velocity hit is the sample at the trim",
               "peak " + juce::String (peak (full), 6) + " against " + juce::String (expectedFull, 6));

        check (std::abs (tilt (full) / sourceTilt - 1.0f) < 0.01f,
               "and it keeps the sample's own balance",
               "tilt " + juce::String (tilt (full), 3) + " against the source's " + juce::String (sourceTilt, 3));

        // The measure has to be blind to level, and the way to prove it is the product
        // itself: the fader takes exactly as much off as a quarter velocity does, and
        // moves nothing else.
        check (std::abs (tilt (fader) / tilt (full) - 1.0f) < 0.02f,
               "the fader is level, not colour: the same gain by the fader leaves the tilt alone",
               "tilt at a quarter of the level " + juce::String (tilt (fader), 3)
                   + ", at full " + juce::String (tilt (full), 3));

        check (std::abs (peak (fader) / (peak (full) * 0.25f) - 1.0f) < 0.02f,
               "and the fader did take the level down, so that is not a render of nothing",
               "peak " + juce::String (peak (fader), 5) + " against a quarter of "
                   + juce::String (peak (full), 5));

        /**
            Down the travel, the top end goes with it.

            The bands are deliberately not equal-ratio steps: the law is a straight
            line in the filter's coefficient, so most of the velocity range is spent
            getting brighter and the last tenth of it reaches the raw sample. What has
            to hold is that every step down is measurably darker than the one above,
            by more than the noise of the measurement, and that the bottom of the
            travel is a different instrument rather than the same one 12 dB down.
        */
        check (tilt (half) < tilt (full) * 0.90f,
               "half velocity is measurably darker than full",
               "tilt " + juce::String (tilt (half), 3) + " against " + juce::String (tilt (full), 3)
                   + " (ratio " + juce::String (tilt (half) / tilt (full), 3) + ")");

        check (tilt (quarter) < tilt (half) * 0.85f,
               "a quarter is darker again",
               "tilt " + juce::String (tilt (quarter), 3) + " against " + juce::String (tilt (half), 3)
                   + " (ratio " + juce::String (tilt (quarter) / tilt (half), 3) + ")");

        check (tilt (ghost) < tilt (quarter) * 0.60f,
               "and the bottom of the travel is played, not turned down",
               "tilt " + juce::String (tilt (ghost), 3) + " against " + juce::String (tilt (quarter), 3)
                   + " (ratio " + juce::String (tilt (ghost) / tilt (quarter), 3) + ")");

        /**
            What the colour law costs, said out loud.

            A dimmed hit also loses whatever part of its level lived above the corner,
            so a soft hit comes out under what the gain law alone predicts: on this
            fixture, 1.9 dB of peak and 6.9 dB of integrated level at a quarter of a
            strike, 4.0 and 10.1 at a sixteenth. The peak is the number that matters for
            a drum and the integrated level is the worst case of a broadband one, because
            a burst that is flat to Nyquist keeps most of its energy above any corner
            anybody would set — a sample with a body loses less, and a kick loses almost
            nothing. It is written here rather than left to be noticed by ear, because a
            law about tone that quietly also turns the machine down is the thing this
            project has been caught out by before.
        */
        check (peak (quarter) < peak (full) * 0.25f * 1.02f
                 && peak (quarter) > peak (full) * 0.25f * 0.40f,
               "the gain law still applies, and the dimming only ever takes a little more",
               "a quarter velocity peaks at " + juce::String (peak (quarter), 5) + ", which is "
                   + juce::String (peak (quarter) / (peak (full) * 0.25f) * 100.0f, 1)
                   + "% of what the gain law alone would give");

        check (peak (ghost) < peak (full) * 0.0625f * 1.02f
                 && peak (ghost) > peak (full) * 0.0625f * 0.10f,
               "the same at the bottom of the travel",
               "a sixteenth peaks at " + juce::String (peak (ghost), 5) + ", which is "
                   + juce::String (peak (ghost) / (peak (full) * 0.0625f) * 100.0f, 1)
                   + "% of the gain law alone");

        noise.deleteFile();
    }

    //======================================================================
    // The windowed sinc is normalised so that a constant input comes out at the same
    // constant, which is what stops a resampled hit from being a gain change. That
    // promise only held while all 24 taps had something to read: over the first few
    // output samples of a hit — and the last few — the taps that hung off the end of the
    // source were skipped and the missing mass was never put back.
    //
    // Measured, on a source that is flat at 0.6355 and stays there (the row printed
    // below, from `out/smoke-34-no-renorm.txt`): 0.6824, 0.6176, 0.6409, 0.6345, then the
    // correct level. Not the straight attenuation one would have guessed, because what
    // goes missing at the outer edge of a kernel is mostly negative sidelobe — the first
    // read came out 7.4% *hot*, the second 2.8% cold, and the step from one to the other
    // is 0.0648 of full scale in a single sample out of a signal that has no right to
    // move at all. On a kick that reads as a thump and a hollow at the top of the hit,
    // which is the one part of a drum that carries its size.
    //
    // A constant is the only fixture that measures this without an argument about
    // statistics: whatever the phase and whatever the band, a read with all its taps
    // inside outputs the constant, and a truncated one owes the difference exactly.
    std::printf ("\n[24] a pitched hit is at its own level from its first sample\n");
    {
        /**
            A flat source, written through the noise fixture's writer because that writer
            writes whatever samples it is handed. 0.8 rather than 1.0 so that nothing in
            the path can be mistaken for a limit.
        */
        std::vector<float> flat (30000, 0.8f);
        auto flatWav = makeNoiseWav ("edge-flat", flat, sampleRate);

        /**
            A stand-in for a kick: loud at sample 0 and only ever getting quieter.

            The shape is the point. A source that cannot rise again turns any rise at the
            top of a render into the engine's own doing, with no statistics and no window
            length to argue about.
        */
        std::vector<float> kick (30000);

        for (int i = 0; i < 30000; ++i)
            kick[(size_t) i] = 0.9f * (float) std::exp (-(double) i / 400.0);

        auto kickWav = makeNoiseWav ("edge-kick", kick, sampleRate);

        const auto trim = std::pow (10.0f, Layout::defaultMasterGainDb / 20.0f);

        const auto strike = [&] (const juce::File& source, int semitones)
        {
            GrooveboxProcessor processor;
            processor.prepareToPlay (sampleRate, blockSize);

            juce::String error;
            processor.loadSampleIntoChannel (0, source, error);

            clearAllSteps (processor);
            processor.setStep (0, 0, true);
            processor.setChannelPitch (0, semitones);
            processor.setChannelLevel (0, 1.0f);

            // Flat envelope: attack at nothing reaches unity in two samples, and decay
            // at its floor onto full sustain stays there, so the level the engine reports
            // is the level the kernel produced and nothing else.
            processor.setChannelAttack (0, 0.0f);
            processor.setChannelDecay (0, 0.0f);
            processor.setChannelSustain (0, 1.0f);

            rearm (processor);
            processor.setPlaying (true);

            auto out = render (processor, 30000, blockSize, sampleRate);

            processor.setPlaying (false);
            return out;
        };

        /**
            The first samples that are actually sounding.

            An attack of nothing still costs one sample — the envelope's first step is
            from zero, not to it — so the measurement starts at the crossing rather than
            at index 0, which is where the kernel's own edge begins.
        */
        const auto headOf = [] (const std::vector<float>& out, int count)
        {
            std::vector<float> head;
            const auto onset = onsetAfter (out, 0, 1.0e-4f);

            if (onset < 0)
                return head;

            for (int i = 0; i < count && onset + i < (int) out.size(); ++i)
                head.push_back (out[(size_t) (onset + i)]);

            return head;
        };

        const auto octave = strike (flatWav, 12);   // a whole-number rate: the phase never moves
        const auto fifth  = strike (flatWav, 7);    // any other rate: the phase walks the table
        const auto unity  = strike (flatWav, 0);    // no kernel at all, so the reference

        const auto windowOf = [] (const std::vector<float>& data, int from, int to)
        { return std::vector<float> (data.begin() + from, data.begin() + to); };

        const auto body = [&] (const RenderResult& hit) { return rmsOf (windowOf (hit.left, 8000, 8400)); };
        const auto expected = 0.8f * trim;

        std::printf ("        the first eight sounding samples of a hit that is flat at %s\n",
                     juce::String (expected, 4).toRawUTF8());

        for (const auto& row : std::vector<std::pair<juce::String, const RenderResult*>>
                                 { { "pitch  0 (no kernel)", &unity },
                                   { "pitch +12 (rate 2.0) ", &octave },
                                   { "pitch  +7 (rate 1.50)", &fifth } })
        {
            const auto head = headOf (row.second->left, 8);
            std::printf ("        %s:", row.first.toRawUTF8());

            for (auto value : head)
                std::printf (" %6s", juce::String (value, 4).toRawUTF8());

            std::printf ("   body %s\n", juce::String (body (*row.second), 4).toRawUTF8());
        }

        const auto octaveHead = headOf (octave.left, 8);
        const auto fifthHead  = headOf (fifth.left, 8);
        const auto octaveBody = body (octave), fifthBody = body (fifth), unityBody = body (unity);

        // The precondition, before anything about the edge: a hit that never changes
        // pitch comes out of the engine at the source times the trim, and so does one
        // that does. If either of these were wrong, the ratio below would be measuring
        // the test rather than the kernel.
        check (std::abs (unityBody / expected - 1.0f) < 0.002f,
               "a flat source at unity is the sample at the trim",
               "body " + juce::String (unityBody, 5) + " against " + juce::String (expected, 5));

        check (std::abs (octaveBody / expected - 1.0f) < 0.002f
                 && std::abs (fifthBody / expected - 1.0f) < 0.002f,
               "and so is a pitched one away from its ends, so the fix did not move the level",
               "body at +12 " + juce::String (octaveBody, 5) + ", at +7 " + juce::String (fifthBody, 5));

        /**
            The claim itself: the first sounding sample of a pitched hit is at the level
            the rest of the hit is at, and not merely under it.

            Two-sided because that is what the measurement said. A one-sided bound — the
            first draft of this check, `is the head not quieter than the body` — passed on
            the broken build, because the truncated kernel was 7% hot on its first read
            rather than short. A whole-number rate is the case the edge makes worst: the
            same eleven taps hang off the start of every one of those samples, so the
            error is a flat mislevel rather than something that averages away between
            phases.
        */
        const auto atOwnLevel = [] (const std::vector<float>& head, float bodyLevel)
        { return head.empty() ? 999.0f : std::abs (head[0] / juce::jmax (1.0e-6f, bodyLevel) - 1.0f); };

        const auto octaveError = atOwnLevel (octaveHead, octaveBody);
        const auto fifthError = atOwnLevel (fifthHead, fifthBody);

        check (octaveError < 0.02f,
               "the first sample of an octave-up hit is at the hit's own level",
               "it reads " + juce::String (octaveHead.empty() ? 0.0f : octaveHead[0], 4)
                   + " against the " + juce::String (octaveBody, 4) + " it settles at — "
                   + juce::String (octaveError * 100.0f, 1) + "% off (the skipped taps used to be 7.4%)");

        check (fifthError < 0.02f,
               "the same on a rate that is not a whole number",
               "it reads " + juce::String (fifthHead.empty() ? 0.0f : fifthHead[0], 4)
                   + " against the " + juce::String (fifthBody, 4) + " it settles at — "
                   + juce::String (fifthError * 100.0f, 1) + "% off");

        // Restoring the mass by dividing could have put a step where the ramp used to
        // be, so the edge has to be flat too: on a constant source, any jump at all is
        // the engine's, and a tenth of the level in one sample is a click.
        {
            const auto worstEdge = [] (const RenderResult& hit)
            {
                const auto onset = onsetAfter (hit.left, 0, 1.0e-4f);
                float worst = 0.0f;

                for (int i = juce::jmax (1, onset); i < onset + 40 && i + 1 < (int) hit.left.size(); ++i)
                    worst = juce::jmax (worst, std::abs (hit.left[(size_t) (i + 1)]
                                                         - hit.left[(size_t) i]));

                return worst;
            };

            const auto octaveEdge = worstEdge (octave);
            const auto fifthEdge = worstEdge (fifth);

            std::printf ("        biggest step across the edge: %s at +12, %s at +7"
                         " (the level itself is %s)\n",
                         juce::String (octaveEdge, 6).toRawUTF8(), juce::String (fifthEdge, 6).toRawUTF8(),
                         juce::String (octaveBody, 4).toRawUTF8());

            check (octaveEdge < octaveBody * 0.02f && fifthEdge < fifthBody * 0.02f,
                   "and the level arrives without a step in it",
                   "worst sample-to-sample change across the first 40 samples: "
                       + juce::String (octaveEdge, 6) + " at +12, " + juce::String (fifthEdge, 6)
                       + " at +7, against a limit of " + juce::String (octaveBody * 0.02f, 6));
        }

        /**
            What the wobble meant on something with a transient in it.

            The constant above says every read now carries its full weight, which is a
            statement about any source; this is the same fact heard as the thing a player
            would notice. A kick that only ever decays has to *render* as only ever
            decaying, and the truncated reads put 0.0766 of a step down followed by 0.0227
            back up on the top of it — a thump and a hollow exactly where the size of the
            hit is carried. The source rising again is not possible here, so any rise in
            the render is the engine's.
        */
        {
            for (const auto& pitch : std::vector<std::pair<int, juce::String>> { { 12, "+12" }, { 7, "+7" } })
            {
                const auto hit = strike (kickWav, pitch.first);
                const auto head = headOf (hit.left, 8);

                std::printf ("        a decaying hit at %s:", pitch.second.toRawUTF8());

                for (auto value : head)
                    std::printf (" %6s", juce::String (value, 4).toRawUTF8());

                std::printf ("\n");

                float biggestRise = 0.0f;

                for (std::size_t i = 1; i < head.size(); ++i)
                    biggestRise = juce::jmax (biggestRise, head[i] - head[i - 1]);

                check (head.size() > 5 && biggestRise < head[0] * 0.005f,
                       "a decaying hit does not rise again inside the engine, at " + pitch.second,
                       "the largest step up across the first eight sounding samples is "
                           + juce::String (biggestRise, 4) + ", on a hit that starts at "
                           + juce::String (head.empty() ? 0.0f : head[0], 4)
                           + " and only ever comes down (the edge used to add 0.0227 back)");
            }
        }

        flatWav.deleteFile();
        kickWav.deleteFile();
    }

    //======================================================================
    // A pitch drop is a shape and not a tuning: the hit begins above its own note and
    // lands on it, so every claim about it is a claim about a frequency measured over
    // time. A sine is the only source that answers those exactly — the distance between
    // two upward zero crossings, interpolated to a fraction of a sample, *is* the pitch,
    // and there is nothing else in the file that could have moved it.
    //
    // Three things had to be true and none of them follows from "multiply the increment
    // by a ramp". That the glide lands on the note rather than somewhere near it, which
    // is the difference between a tuning and a detuned kit. That it only ever comes down,
    // and by an amount that tracks the parameter. And that a read which slows down still
    // plays to the end of the sample.
    //
    // That last one is the reason the glide re-derives the countdown to the end of the
    // source instead of counting down the number taken from the starting rate. Counting
    // down is not obviously wrong, because the countdown is re-derived at the top of
    // every call anyway, so the damage is bounded by one block rather than by the whole
    // hit — which is exactly why it needs measuring and not reasoning: on a 36 ms sample
    // at a 256-sample block the two versions agree to the sample, and at 1024 the naive
    // one throws away 570 of the 1576 samples that hit has. A hit whose length depends
    // on how the host chose to chunk it is the cut this whole phase exists to prevent.
    std::printf ("\n[25] a dropped hit falls to its own note, and plays the whole sample\n");
    {
        constexpr double toneHz = 220.0;
        constexpr int toneLength = 20000;   // 454 ms at rate 1, so the source runs out by itself
        constexpr int renderLength = 24000;
        const auto nominalPeriod = sampleRate / toneHz;

        auto tone = makeToneWav ("drop", toneHz, toneLength, sampleRate);

        /** The mean period, in samples, over `cycles` upward crossings from `from`. */
        const auto meanPeriod = [] (const std::vector<float>& data, int from, int cycles)
        {
            double first = -1.0, last = -1.0;
            int found = 0;

            for (int i = juce::jmax (1, from); i + 1 < (int) data.size() && found <= cycles; ++i)
            {
                const auto a = data[(size_t) (i - 1)];
                const auto b = data[(size_t) i];

                if (a >= 0.0f || b < 0.0f)
                    continue;

                const auto crossing = (double) (i - 1) + (double) (0.0f - a) / (double) (b - a);

                if (found == 0)
                    first = crossing;

                last = crossing;
                ++found;
            }

            return found > cycles ? (last - first) / (double) cycles : -1.0;
        };

        /** Every period between the crossings in [from, to), for the direction of travel. */
        const auto periodsIn = [] (const std::vector<float>& data, int from, int to)
        {
            std::vector<double> gaps;
            double previous = -1.0;

            for (int i = juce::jmax (1, from); i + 1 < juce::jmin (to, (int) data.size()); ++i)
            {
                const auto a = data[(size_t) (i - 1)];
                const auto b = data[(size_t) i];

                if (a >= 0.0f || b < 0.0f)
                    continue;

                const auto crossing = (double) (i - 1) + (double) (0.0f - a) / (double) (b - a);

                if (previous >= 0.0)
                    gaps.push_back (crossing - previous);

                previous = crossing;
            }

            return gaps;
        };

        const auto windowOf = [] (const std::vector<float>& data, int from, int to)
        { return std::vector<float> (data.begin() + from, data.begin() + to); };

        const auto strike = [&] (int drop, const juce::File& source, int block, int length)
        {
            GrooveboxProcessor processor;
            processor.prepareToPlay (sampleRate, block);

            juce::String error;
            processor.loadSampleIntoChannel (0, source, error);

            clearAllSteps (processor);
            processor.setStep (0, 0, true);
            processor.setChannelPitch (0, 0);
            processor.setChannelPitchDrop (0, drop);
            processor.setChannelLevel (0, 1.0f);

            // Ring until the source runs out: the end of the sound is then the end of
            // the sample, which is the only thing the falling rate can get wrong.
            processor.setChannelAttack (0, 0.0f);
            processor.setChannelDecay (0, 0.0f);
            processor.setChannelSustain (0, 1.0f);

            rearm (processor);
            processor.setPlaying (true);

            auto out = render (processor, length, block, sampleRate);

            processor.setPlaying (false);
            return out;
        };

        const auto flat   = strike (0,  tone, blockSize, renderLength);
        const auto fifth  = strike (5,  tone, blockSize, renderLength);
        const auto octave = strike (12, tone, blockSize, renderLength);

        const auto onsetOf = [] (const RenderResult& hit) { return onsetAfter (hit.left, 0, 1.0e-4f); };

        const auto onset = onsetOf (octave);
        const auto sweepSamples = (int) (Layout::pitchDropSweepSeconds * sampleRate);

        // Where the sound actually stops, so the claim is a number and not an argument.
        const auto lastSounding = [] (const RenderResult& hit)
        {
            for (int i = (int) hit.left.size() - 1; i > 0; --i)
                if (std::abs (hit.left[(size_t) i]) > 0.05f)
                    return i;

            return 0;
        };

        const auto octaveEarly   = meanPeriod (octave.left, onset, 3);
        const auto octaveSettled = meanPeriod (octave.left, onset + sweepSamples + 600, 8);
        const auto fifthEarly    = meanPeriod (fifth.left, onset, 3);
        const auto flatEarly     = meanPeriod (flat.left, onset, 3);
        const auto flatSettled   = meanPeriod (flat.left, onset + sweepSamples + 600, 8);

        std::printf ("        the note is %s Hz, so its period is %s samples\n",
                     juce::String (toneHz, 1).toRawUTF8(), juce::String (nominalPeriod, 3).toRawUTF8());
        std::printf ("        drop  0: early %s  settled %s\n",
                     juce::String (flatEarly, 3).toRawUTF8(), juce::String (flatSettled, 3).toRawUTF8());
        std::printf ("        drop  5: early %s  settled %s\n",
                     juce::String (fifthEarly, 3).toRawUTF8(),
                     juce::String (meanPeriod (fifth.left, onset + sweepSamples + 600, 8), 3).toRawUTF8());
        std::printf ("        drop 12: early %s  settled %s  (starts at %s of its own note's period)\n",
                     juce::String (octaveEarly, 3).toRawUTF8(), juce::String (octaveSettled, 3).toRawUTF8(),
                     juce::String (octaveEarly / juce::jmax (1.0, octaveSettled), 3).toRawUTF8());
        std::printf ("        the sample is %d long; the hit stops at %d with a drop of 12, at %d with none\n",
                     toneLength, lastSounding (octave), lastSounding (flat));

        // The precondition first: a lane that asked for no drop is at its own pitch from
        // its first sample. Without this, a glide that happened whatever the parameter
        // said would pass every check below.
        check (flatEarly > 0.0 && std::abs (flatEarly - nominalPeriod) < 0.5
                 && std::abs (flatSettled - nominalPeriod) < 0.5,
               "a lane with no drop is at its pitch from its first sample",
               "early " + juce::String (flatEarly, 3) + ", settled " + juce::String (flatSettled, 3)
                   + ", the note is " + juce::String (nominalPeriod, 3));

        check (octaveSettled > 0.0 && std::abs (octaveSettled - nominalPeriod) < 0.5,
               "and a dropped one lands on that same note, not near it",
               "settled period " + juce::String (octaveSettled, 3) + " against "
                   + juce::String (nominalPeriod, 3) + " (0.5 samples is a quarter of a percent)");

        check (octaveEarly > 0.0 && octaveEarly < octaveSettled * 0.85,
               "it starts well above the note it falls to",
               "the first three cycles measure " + juce::String (octaveEarly, 1) + " samples against the "
                   + juce::String (octaveSettled, 1) + " it settles at");

        // The margins here are the point, not the ordering: at +5 the first three cycles
        // measure 0.929 of the note's period and at +12 they measure 0.761, so a lane
        // that ignores its drop shows up as 1.000 and 1.000. Comparing the two against
        // each other without a gap is what let an earlier version of this check pass on
        // a build that had no glide in it at all.
        check (fifthEarly > 0.0 && fifthEarly < nominalPeriod * 0.95f
                 && fifthEarly > octaveEarly * 1.05,
               "and the amount is the amount: five semitons starts between none and twelve",
               "early " + juce::String (fifthEarly, 1) + " at +5, " + juce::String (octaveEarly, 1)
                   + " at +12, against the note at " + juce::String (nominalPeriod, 1));

        {
            const auto gaps = periodsIn (octave.left, onset, onset + sweepSamples + 200);
            double worst = 0.0;

            for (std::size_t i = 1; i < gaps.size(); ++i)
                worst = juce::jmax (worst, gaps[i - 1] - gaps[i]);

            std::printf ("        %d cycles across the sweep, lengths %s..%s, widest step back down: %s\n",
                         (int) gaps.size(),
                         juce::String (gaps.empty() ? 0.0 : gaps.front(), 1).toRawUTF8(),
                         juce::String (gaps.empty() ? 0.0 : gaps.back(), 1).toRawUTF8(),
                         juce::String (worst, 3).toRawUTF8());

            // Seven gaps is what eight crossings in a 1523-sample window gives, and eight
            // crossings is what the glide's own timing predicts: the sweep is 1323 samples
            // and the cycles inside it run 132, 154, 164, 173, 186, 192, 197 samples long.
            // Asserting the count separately keeps a failure here from reading as a failure
            // of the direction claim below it.
            check (gaps.size() >= 6, "the sweep is long enough to see a glide, not a jump",
                   "only " + juce::String ((int) gaps.size()) + " cycles across the sweep");

            check (worst < 0.5, "the glide only ever goes one way",
                   "a cycle came back " + juce::String (worst, 3)
                       + " samples shorter than the one before it, which means the pitch "
                         "went back up inside the sweep");

            // One-way is not the same as travelled: a build that never started its glide
            // passes the check above at exactly zero, so the size of the fall is asserted
            // beside its direction. Measured: 131.8 samples for the first cycle across
            // 198.7 for the last, against 200.5 across 200.5 for a lane that ignored it.
            check (gaps.size() >= 2 && gaps.back() > gaps.front() * 1.3,
                   "and it falls a bit, rather than holding one pitch and calling it a glide",
                   "the first cycle across the sweep is "
                       + juce::String (gaps.empty() ? 0.0 : gaps.front(), 1) + " samples, the last "
                       + juce::String (gaps.empty() ? 0.0 : gaps.back(), 1)
                       + "; a glide to the note has to lengthen the cycle by half again");
        }

        /**
            That a read which speeds up and then settles still plays to the end.

            The engine works out how many output samples are left before the read lands
            on the end of the source, and counts down from there while the rate is the
            one that countdown was derived at. A glide is exactly where that stops
            being true, so the middle of the *second half* of a long sample is measured
            here: a dropped hit has to be sounding 400 ms in, at the level the same
            sample played at its own note is. The block-size part below is where the
            same question gets asked of the case that can actually go wrong.
        */
        {
            const auto late = octave.peakBetween (14800, 15200);
            const auto afterEnd = octave.peakBetween (toneLength, renderLength);

            std::printf ("        peak over the sample's second half: %s; peak after the source is out: %s\n",
                         juce::String (late, 4).toRawUTF8(), juce::String (afterEnd, 6).toRawUTF8());

            check (late > 0.5f, "a dropped hit plays the second half of its sample",
                  "peak over output samples 14800..15200 is " + juce::String (late, 4)
                      + "; the countdown from the starting rate stops the voice halfway in");

            check (afterEnd == 0.0f, "and stops when the source is out rather than reading past it",
                   "peak after sample " + juce::String (toneLength) + " is " + juce::String (afterEnd, 6));
        }

        // A change of pitch shape that changed the level would be a gain law, so the
        // settled part of both hits has to be the same size. Over four cycles of a full
        // sine the rms is the same number whatever the phase.
        {
            const auto flatRms = rmsOf (windowOf (flat.left, 14000, 17500));
            const auto octaveRms = rmsOf (windowOf (octave.left, 14000, 17500));

            std::printf ("        level over the settled part: %s with no drop, %s with twelve\n",
                         juce::String (flatRms, 5).toRawUTF8(), juce::String (octaveRms, 5).toRawUTF8());

            check (flatRms > 0.0f && std::abs (octaveRms / flatRms - 1.0f) < 0.02f,
                   "the drop costs no level",
                   juce::String (octaveRms / juce::jmax (1.0e-9f, flatRms), 4) + " of the no-drop rms");
        }

        /**
            How long the hit is must not depend on how the host chunks it.

            The engine counts down to the end of the source from one divide, and a
            countdown only counts down while the rate covering the distance is the rate
            it was derived from. A glide breaks that, so the countdown is re-derived per
            sample while the pitch is falling — but a stale one is only stale until the
            next call, which re-derives it anyway, so the damage a missing re-derive can
            do is bounded by one block. That is why this is measured on a sample short
            enough to run out inside the sweep and at the largest block a host asks for:
            at 2048 samples a whole 30 ms sweep lands in one call, and a countdown left
            over from the starting rate loses a third of the hit.
        */
        {
            constexpr int stubLength = 1600;   // 36 ms: ends about one sweep after the strike
            constexpr int stubRender = 4096;
            auto stub = makeToneWav ("dropstub", toneHz, stubLength, sampleRate);

            const auto stubFlat = strike (0, stub, 256, stubRender);

            struct Sized { int block; RenderResult hit; };
            std::vector<Sized> sized;

            for (auto block : { 256, 512, 1024, 2048 })
                sized.push_back ({ block, strike (12, stub, block, stubRender) });

            const auto stubFlatRms = rmsOf (stubFlat.left);
            const auto reference   = rmsOf (sized.front().hit.left);

            std::printf ("        a %d-sample sample, 36 ms, struck with a drop of 12:\n", stubLength);
            std::printf ("          no drop:  ends at %5d, rms %7s\n",
                         lastSounding (stubFlat), juce::String (stubFlatRms, 5).toRawUTF8());

            for (const auto& s : sized)
                std::printf ("          block %4d: ends at %5d, rms %7s, late peak %7s\n",
                             s.block, lastSounding (s.hit),
                             juce::String (rmsOf (s.hit.left), 5).toRawUTF8(),
                             juce::String (s.hit.peakBetween (1200, 1400), 4).toRawUTF8());

            // Compared with the same strike at the smallest block, not with the same
            // sample played without a drop: a drop reads extra source while it falls, so
            // it genuinely shortens the hit, and holding a dropped hit to an undropped
            // one's length would be holding it to a number that is not its own.
            for (const auto& s : sized)
            {
                const auto ratio = rmsOf (s.hit.left) / juce::jmax (1.0e-9f, reference);
                const auto ends  = std::abs (lastSounding (s.hit) - lastSounding (sized.front().hit));

                check (std::abs (ratio - 1.0f) < 0.005f && ends <= 1,
                       "the same strike is the same hit at block " + juce::String (s.block),
                       "integrated level is " + juce::String (ratio, 4) + " of the 256-sample "
                           "render and its end is " + juce::String ((int) ends)
                           + " samples away; a countdown left from the starting rate ends the "
                             "voice inside the sweep, and the bigger the block the more of the "
                             "hit it throws away");
            }

            // What the whole sweep costs is stated rather than assumed: the glide reads
            // extra source at the top, so the hit ends sooner than the same sample played
            // at its own note — by the extra it read, which is the same 263 samples here
            // as on a 454 ms sample, because it is a property of the sweep and not of the
            // file. That is the drop doing its job; a length that moved with the block
            // size would be the countdown failing to keep up.
            const auto swept = lastSounding (sized.front().hit) - lastSounding (stubFlat);

            std::printf ("          the sweep costs %d samples of a 36 ms hit, and reads %s of its level\n",
                         -swept, juce::String (reference / juce::jmax (1.0e-9f, stubFlatRms), 4).toRawUTF8());

            stub.deleteFile();

            /**
                What the drop costs in colour, which is the part the engine promises.

                The kernel's cutoff is picked from the fastest the read ever gets and
                held for the whole hit, because a band that changed mid-hit would be a
                filter sweep nobody asked for. So the settled two thirds of a dropped hit
                is anti-aliased for a rate it is no longer running at. On a sine that is
                invisible — 220 Hz is far below either cutoff, which is why the level row
                above could be so flat. On broadband material it is not, and the honest
                thing to do is say by how much: `tiltOf` is a ratio, so no fader can
                imitate it and the comparison needs no level matching.
            */
            const auto broad = makeNoiseSamples (8000);
            auto noise = makeNoiseWav ("drop-broad", broad, sampleRate);

            constexpr int colourFrom = 2000, colourTo = 7000;   // past the sweep, inside both hits
            const auto dullNone  = tiltOf (strike (0,  noise, blockSize, 9000).left, colourFrom, colourTo);
            const auto dullStep  = tiltOf (strike (1,  noise, blockSize, 9000).left, colourFrom, colourTo);
            const auto dullFifth = tiltOf (strike (5,  noise, blockSize, 9000).left, colourFrom, colourTo);
            const auto dullFull  = tiltOf (strike (12, noise, blockSize, 9000).left, colourFrom, colourTo);

            /**
                The +1 row is what makes the other three mean something.

                A lane that reads at any rate other than exactly its own goes through the
                24-tap kernel, and a 24-tap kernel is not a wire: it costs a little top
                end whatever band it is in. Comparing a dropped hit with an undropped one
                therefore charges the drop with a cost that every pitched lane on this
                machine already pays. +1 semitone peaks at 1.059, which is the widest band
                the table has, so its tilt is the price of interpolating at all — and the
                rows above it are only evidence about the drop once that price is taken
                out of them.

                This is where the band stopped being a tone control. Holding the cutoff at
                the top of the sweep cost 3.3 dB of the high band at +12, so the tuning
                knob also dimmed the hit; following the rate as it falls brings +5 and +12
                back to within a rattle of +1, which is the level of nothing but the
                kernel itself. The amounts do not order monotonically any more, and they
                are not supposed to: what is left in the settled window is the same read
                at the same rate, and the spread between them is which slice of the noise
                the sweep's extra consumption landed the window on.
            */
            const auto dB = [] (double a, double b) { return 10.0 * std::log10 (b / juce::jmax (1.0e-9, a)); };

            std::printf ("          tilt of the settled part, %d..%d of a broadband hit:\n",
                         colourFrom, colourTo);
            std::printf ("            none  %6s   (the unity path, no kernel at all)\n",
                         juce::String (dullNone, 2).toRawUTF8());
            std::printf ("             +1  %6s   %5s dB   <- the kernel, widest band\n",
                         juce::String (dullStep, 2).toRawUTF8(), juce::String (dB (dullNone, dullStep), 2).toRawUTF8());
            std::printf ("             +5  %6s   %5s dB   (%s of the +1 row)\n",
                         juce::String (dullFifth, 2).toRawUTF8(), juce::String (dB (dullNone, dullFifth), 2).toRawUTF8(),
                         juce::String (dullFifth / juce::jmax (1.0e-9f, dullStep), 3).toRawUTF8());
            std::printf ("            +12  %6s   %5s dB   (%s of the +1 row)\n",
                         juce::String (dullFull, 2).toRawUTF8(), juce::String (dB (dullNone, dullFull), 2).toRawUTF8(),
                         juce::String (dullFull / juce::jmax (1.0e-9f, dullStep), 3).toRawUTF8());

            check (dullStep > 0.0f && std::abs (dullFifth / dullStep - 1.0f) < 0.06f
                     && std::abs (dullFull / dullStep - 1.0f) < 0.06f,
                   "the drop is a tuning and not a tone control: the tail is as bright as a lane that only had to interpolate",
                   "settled tilt " + juce::String (dullStep, 2) + " at +1 against "
                       + juce::String (dullFifth, 2) + " at +5 and " + juce::String (dullFull, 2)
                       + " at +12; a cutoff pinned to the top of the sweep measured 0.79 "
                         "and 0.47 of the +1 row");

            // The floor the whole thing has to clear: the kernel is allowed its own small
            // price (0.89 of the undropped hit at +1) but a dropped hit is not allowed to
            // fall far below an interpolated one, and both are far above the pinned band's
            // 0.40 at +12.
            check (dullStep > dullNone * 0.85f && dullFull > dullNone * 0.8f,
                   "and what is left is the kernel's own price, not a lowpass",
                   "the +1 row is " + juce::String (dullStep / juce::jmax (1.0e-9f, dullNone), 3)
                       + " of the undropped tilt and +12 is "
                       + juce::String (dullFull / juce::jmax (1.0e-9f, dullNone), 3)
                       + "; past 0.8 of the way down the hit is being filtered, not tuned");

            noise.deleteFile();
        }

        tone.deleteFile();
    }

    //======================================================================
    std::printf ("\n[26] the meter reports the level between the samples, too\n");
    {
        /** Feeds a signal straight through the master bus and reports what the two
            markers came back with, plus whether any sample was touched.

            `chunk` is deliberately a parameter: what is being proven here is that the
            reconstruction is a property of the signal and not of how the host happened
            to slice it up, so the same waveform goes in at 256 and at 2048.
        */
        auto throughBus = [] (const std::vector<float>& signal, int chunk, double rate)
        {
            struct Result { float samplePeak; float truePeak; float worstTouched; bool clipped; };

            gb::MasterBus bus;

            // The trim is set before the bus is prepared, so `currentGain` starts
            // exactly on it. A smoother still climbing from the default -2 dB would
            // be a ramp in the audio, and the first thing this section has to be able
            // to state is that nothing in the audio moved.
            bus.setGainDb (0.0f);
            bus.prepare (rate);

            auto touched = 0.0f;
            auto bar = 0.0f;
            auto tick = 0.0f;
            auto latched = false;
            const auto total = (int) signal.size();

            for (int start = 0; start < total; start += chunk)
            {
                const auto n = juce::jmin (chunk, total - start);
                juce::AudioBuffer<float> buffer (2, n);

                for (int i = 0; i < n; ++i)
                {
                    const auto value = signal[(std::size_t) (start + i)];
                    buffer.getWritePointer (0)[(std::size_t) i] = value;
                    buffer.getWritePointer (1)[(std::size_t) i] = value;
                }

                bus.process (buffer);

                // Read after every block, the way the editor's timer does: the hold
                // decays, so reading once at the end of a long stream reports the
                // decay rather than the level the block reached.
                bar = juce::jmax (bar, bus.getPeak (0));
                tick = juce::jmax (tick, bus.getTruePeak (0));
                latched = latched || bus.isClipping (0);

                for (int i = 0; i < n; ++i)
                    touched = juce::jmax (touched,
                                          std::abs (buffer.getReadPointer (0)[(std::size_t) i]
                                                        - signal[(std::size_t) (start + i)]));
            }

            return Result { bar, tick, touched, latched };
        };

        /** A sine at `frequency` and `amplitude`, with a phase offset.

            Faded in over the first 64 samples, and that fade is load-bearing: a tone
            whose first sample is at full amplitude is a step, and the band-limited
            reconstruction of a step rings above the wave's own crest for as long as
            the kernel is wide. Unfaded, a 0.8 tone at 441 Hz put its tick at 0.9042 on
            sample 12 — 13 % over the crest, from nothing but its own start, at every
            frequency in the sweep. The ring is real, and the square wave further down
            checks it on purpose; left in here it would read as meter error.
        */
        auto sine = [fadeIn = 64] (double frequency, float amplitude, int length,
                                   double rate, double phase)
        {
            std::vector<float> data ((std::size_t) length);

            for (int i = 0; i < length; ++i)
            {
                const auto envelope = i < fadeIn
                    ? 0.5f - 0.5f * (float) std::cos (juce::MathConstants<double>::pi
                                                        * (double) i / (double) fadeIn)
                    : 1.0f;

                data[(std::size_t) i]
                    = amplitude * envelope
                        * (float) std::sin (2.0 * juce::double_Pi * frequency * (double) i / rate
                                              + phase);
            }

            return data;
        };

        /** The tone `rate / periodSamples` with its samples straddling the crest.

            With an even number of samples per period and the phase half a step off
            the crest, no sample can land on it: the nearest ones sit at plus or minus
            pi / N, so the sample peak is `amplitude` x cos (pi / N) while the level of
            the wave itself is `amplitude`. Both numbers are known before anything
            runs, at every frequency in the sweep, and the gap the tick has to find is
            1 / cos (pi / N).
        */
        auto straddling = [sine] (int periodSamples, float amplitude, int length, double rate)
        {
            const auto step = juce::double_Pi / (double) periodSamples;
            return sine (rate / (double) periodSamples, amplitude, length, rate,
                         juce::MathConstants<double>::halfPi + step);
        };

        const float toneLevel = 0.8f;

        std::printf ("        tones whose samples straddle the crest, at amplitude %.2f:\n",
                     (double) toneLevel);
        std::printf ("            f(Hz)   bar      bar should be   tick     crest error    "
                     "gap    gap should be\n");

        auto worstBarError = 0.0f;
        auto worstCrestError = 0.0f;

        for (int periodSamples : { 100, 50, 20, 10, 8, 6, 4 })
        {
            const auto expectedBar = toneLevel
                                       * (float) std::cos (juce::double_Pi / (double) periodSamples);
            const auto expectedGap = 1.0f / (float) std::cos (juce::double_Pi / (double) periodSamples);
            const auto read = throughBus (straddling (periodSamples, toneLevel, 8192, sampleRate),
                                          256, sampleRate);

            const auto barError = std::abs (read.samplePeak / juce::jmax (1.0e-9f, expectedBar) - 1.0f);
            const auto crestError = std::abs (read.truePeak / toneLevel - 1.0f);

            worstBarError = juce::jmax (worstBarError, barError);
            worstCrestError = juce::jmax (worstCrestError, crestError);

            std::printf ("          %7.0f   %.4f   %.4f          %.4f   %+.4f         %.4f   %.4f\n",
                         (double) (sampleRate / (double) periodSamples),
                         (double) read.samplePeak, (double) expectedBar, (double) read.truePeak,
                         (double) (read.truePeak / toneLevel - 1.0f),
                         (double) (read.truePeak / juce::jmax (1.0e-9f, read.samplePeak)),
                         (double) expectedGap);
        }

        std::printf ("        worst across the sweep: bar off by %.5f, tick off by %.5f\n",
                    (double) worstBarError, (double) worstCrestError);

        check (worstBarError < 0.01f,
               "the bar is the sample peak, to the cos(pi/N) the sampling says it must be",
               "worst " + juce::String (worstBarError, 5) + " over the seven tones");

        check (worstCrestError < 0.005f,
               "and the tick is the level of the wave itself, at every one of them",
               "worst " + juce::String (worstCrestError, 5)
                   + "; reading a quarter of a sample past each one costs a 24-tap kernel "
                     "nothing it cannot resolve");

        // A third of the sample rate, where the answer is exact on the back of an
        // envelope and there is no fade to blame for anything: the samples land at 0
        // and at amplitude x sqrt(3)/2, so the bar reads 0.866 of the wave and the gap
        // the reconstruction has to report is 1 / (sqrt(3)/2) = 1.1547, +1.25 dB.
        // This is the row that used to read 1.2220, and the reason it did was the start
        // step described above, not the kernel: the same three samples stand just as
        // well for a tone at two thirds of the sample rate, and a 24-tap Hamming kernel
        // was expected to answer to both. Measured with a tone that begins as a tone,
        // it answers to neither, and gets the ideal number to three ten-thousandths.
        const auto thirdRate = sine (sampleRate / 3.0, toneLevel, 4096, sampleRate, 0.0);
        const auto third = throughBus (thirdRate, 256, sampleRate);
        const auto thirdWide = throughBus (thirdRate, 2048, sampleRate);

        const auto expectedThirdSamples = toneLevel * (float) (std::sqrt (3.0) / 2.0);
        const auto idealThirdGap = 1.0f / (float) (std::sqrt (3.0) / 2.0);
        const auto thirdGap = third.truePeak / juce::jmax (1.0e-9f, third.samplePeak);

        std::printf ("        a 0.8 sine at a third of the sample rate: bar %.4f (the sampling lands "
                     "on %.4f), tick %.4f, gap %.4f against the ideal %.4f\n",
                     (double) third.samplePeak, (double) expectedThirdSamples,
                     (double) third.truePeak, (double) thirdGap, (double) idealThirdGap);

        check (std::abs (third.samplePeak - expectedThirdSamples) < expectedThirdSamples * 0.01f,
               "the bar reads the level the sampling actually lands on",
               "bar " + juce::String (third.samplePeak, 4) + " against the "
                   + juce::String (expectedThirdSamples, 4) + " of a 0.8 wave at three samples a period");

        check (std::abs (third.truePeak / toneLevel - 1.0f) < 0.005f,
               "the tick finds the crest the samples missed, at the level the wave is",
               juce::String (third.truePeak, 4) + " for a " + juce::String (toneLevel, 2) + " sine");

        check (std::abs (thirdGap / idealThirdGap - 1.0f) < 0.01f,
               "and it reports the gap the fixture is built to have, which is all of it and no more",
               "gap " + juce::String (thirdGap, 4) + " against the "
                   + juce::String (idealThirdGap, 4) + " that sqrt(3)/2 implies");

        check (thirdWide.truePeak == third.truePeak && thirdWide.samplePeak == third.samplePeak,
               "and the same signal reads the same at a 2048-sample block",
               "tick " + juce::String (thirdWide.truePeak, 4) + " vs " + juce::String (third.truePeak, 4)
                   + ", bar " + juce::String (thirdWide.samplePeak, 4) + " vs "
                   + juce::String (third.samplePeak, 4));

        // The other half of the claim: a waveform with nothing between its samples
        // must not be given a headroom reading anyway. 1 kHz at this sample rate
        // puts a sample within a whisker of the crest.
        const auto plain = throughBus (sine (1000.0, 0.5f, 4096, sampleRate, 0.0), 256, sampleRate);

        std::printf ("        a 0.5 sine at 1 kHz: meter %.4f, tick %.4f (%.4f apart)\n",
                     (double) plain.samplePeak, (double) plain.truePeak,
                     (double) (plain.truePeak / juce::jmax (1.0e-9f, plain.samplePeak)));

        check (std::abs (plain.truePeak / juce::jmax (1.0e-9f, plain.samplePeak) - 1.0f) < 0.01f,
               "a wave that has nothing to show between samples is not given any",
               "tick/bar " + juce::String (plain.truePeak / juce::jmax (1.0e-9f, plain.samplePeak), 4)
                   + " on a 1 kHz sine, where the samples already land on the crest");

        // A single sample of nothing else: the reconstruction of an impulse is the
        // kernel, whose highest point is the sample. A meter that read more here was
        // inventing level out of its own filter.
        std::vector<float> impulse (4096, 0.0f);
        impulse[2048] = 0.9f;
        const auto shot = throughBus (impulse, 256, sampleRate);

        std::printf ("        an isolated 0.9 sample: meter %.4f, tick %.4f\n",
                    (double) shot.samplePeak, (double) shot.truePeak);

        check (std::abs (shot.samplePeak - 0.9f) < 0.9f * 0.001f,
               "the bar reads the impulse at the impulse's own level",
               juce::String (shot.samplePeak, 4) + " for a lone 0.9 sample");

        check (std::abs (shot.truePeak - shot.samplePeak) < 0.9f * 0.01f,
               "an impulse reads as the impulse, not as the kernel's own overshoot",
               "tick " + juce::String (shot.truePeak, 4) + " against a bar of "
                   + juce::String (shot.samplePeak, 4));

        // A hard edge, which is what a summed kit looks like at the top of a hit: the
        // band-limited version of a step has to overshoot, and the samples alone do
        // not say so.
        std::vector<float> square (4096);

        for (int i = 0; i < 4096; ++i)
            square[(std::size_t) i] = (i / 32) % 2 == 0 ? 0.9f : -0.9f;

        const auto edged = throughBus (square, 256, sampleRate);

        std::printf ("        a +/-0.9 square wave: meter %.4f, tick %.4f (%.4f apart)\n",
                    (double) edged.samplePeak, (double) edged.truePeak,
                    (double) (edged.truePeak / juce::jmax (1.0e-9f, edged.samplePeak)));

        check (std::abs (edged.samplePeak - 0.9f) < 0.9f * 0.001f,
               "and the bar reads the square's own flat level, which is all a sample can see",
               juce::String (edged.samplePeak, 4));

        // Two-sided, because both directions are defects: a tick that lands on the
        // bar here is a meter that is not reconstructing anything, and one that runs
        // away is a filter with gain. An infinitely banded square overshoots to 1.179
        // of its flat level; the 24-tap Hamming kernel has passband ripple on top of
        // that and measures 1.26, so the ceiling is that number with room, stated as
        // what was seen rather than as what was wanted.
        check (edged.truePeak > edged.samplePeak * 1.05f && edged.truePeak < edged.samplePeak * 1.35f,
               "a hard edge is shown as the overshoot it is, and no more than that",
               "tick " + juce::String (edged.truePeak, 4) + " against a bar of "
                   + juce::String (edged.samplePeak, 4) + ", ratio "
                   + juce::String (edged.truePeak / juce::jmax (1.0e-9f, edged.samplePeak), 4)
                   + " against Gibbs' 1.179 for a square with no band limit at all");

        // The claim the README makes for the editor: the tick is a report. Here it is
        // the case that makes the difference visible rather than merely asserted -- a
        // signal whose reconstruction is over 0 dBFS while not one of its samples
        // gets there. If the latch ever followed the tick, this is where it shows.
        std::printf ("        that square: tick %.4f is past 0 dBFS while the bar at %.4f is not; "
                     "clip latch %s\n",
                     (double) edged.truePeak, (double) edged.samplePeak,
                     edged.clipped ? "LATCHED" : "clear");

        check (edged.truePeak > 1.0f && edged.samplePeak < 1.0f && ! edged.clipped,
               "a true peak over 0 dBFS with no sample there latches nothing",
               "tick " + juce::String (edged.truePeak, 4) + ", bar "
                   + juce::String (edged.samplePeak, 4) + ", latch "
                   + (edged.clipped ? "set" : "clear"));

        // Report, not guard: none of this touches the samples. At a 0 dB trim the
        // smoother is already exactly at target, so anything other than zero here is
        // the meter changing what the user mixed.
        check (third.worstTouched == 0.0f && edged.worstTouched == 0.0f,
               "and the meter leaves the audio exactly as it found it",
               "worst difference " + juce::String (third.worstTouched, 9) + " and "
                   + juce::String (edged.worstTouched, 9));

        // In situ: the click that [10b] already clips, with both markers read the way
        // the editor's timer reads them.
        {
            auto click = makeClickWav ("true-peak", 400, sampleRate);

            GrooveboxProcessor p;
            p.prepareToPlay (sampleRate, blockSize);

            juce::String err;
            p.loadSampleIntoChannel (0, click, err);

            clearAllSteps (p);
            p.setStep (0, 0, true);
            p.setChannelLevel (0, 0.6f);
            p.setMasterGainDb (0.0f);
            p.setSwing (0.0f);
            p.setManualTempo (120.0);

            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            float bar = 0.0f;
            float tick = 0.0f;

            p.setPlaying (true);

            for (int block = 0; block < 16; ++block)
            {
                buffer.clear();
                p.processBlock (buffer, midi);
                bar = juce::jmax (bar, p.getMasterPeak (0));
                tick = juce::jmax (tick, p.getMasterTruePeak (0));
            }

            p.setPlaying (false);

            std::printf ("        a real hit through the bus: bar %.4f, tick %.4f\n",
                         (double) bar, (double) tick);

            check (tick >= bar, "the tick is never below the bar it is drawn against",
                   "tick " + juce::String (tick, 4) + ", bar " + juce::String (bar, 4));
            check (tick < 1.0f, "and a hit that does not clip is not made to look like one",
                   juce::String (tick, 4));
            check (! p.isMasterClipping (0), "nothing new latches: the clip indicator is still the sample",
                   juce::String (bar, 4) + " sample peak against " + juce::String (tick, 4) + " true");

            click.deleteFile();
        }
    }

    //======================================================================
    std::printf ("\n[27] the step payload describes a step, not a digit\n");
    {
        /** Flushes the grid through the save path and hands back what the tree holds.
            `getStateInformation` pushes the pattern first, so this is exactly the
            payload that would go to disk. */
        auto payloadOf = [] (GrooveboxProcessor& p)
        {
            juce::MemoryBlock blob;
            p.getStateInformation (blob);
            return p.getValueTreeState().state.getProperty ("pattern").toString();
        };

        /** Feeds a hand-written session the way a host would.

            The bytes go through `copyXmlToBinary` rather than being typed as XML:
            `getXmlFromBinary` only recognises that helper's own eight-byte header —
            a magic number and a length in front of the text — and answers bare XML
            with nullptr, which `setStateInformation` treats as a session that does
            not exist and acts on not at all. */
        auto loadSession = [] (GrooveboxProcessor& p, const juce::String& payload, int version = 0)
        {
            juce::ValueTree tree ("GROOVEBOX");

            if (version > 0)
                tree.setProperty ("stateVersion", version, nullptr);

            tree.setProperty ("pattern", payload, nullptr);

            if (auto xml = tree.createXml())
            {
                juce::MemoryBlock blob;
                juce::AudioProcessor::copyXmlToBinary (*xml, blob);
                p.setStateInformation (blob.getData(), (int) blob.getSize());
            }
        };

        /** v1 rows: two digits per step, 0 meaning off, rows comma-separated. */
        auto v1Rows = [] (int channel, int step, int level)
        {
            juce::StringArray rows;

            for (int ch = 0; ch < Layout::numChannels; ++ch)
            {
                juce::String row;

                for (int st = 0; st < Layout::numSteps; ++st)
                    row += juce::String (ch == channel && st == step ? level : 0).paddedLeft ('0', 2);

                rows.add (row);
            }

            return rows;
        };

        auto v1Set = [] (juce::StringArray& rows, int channel, int step, int level)
        {
            const auto at = step * 2;

            rows.getReference (channel) = rows[channel].substring (0, at)
                                    + juce::String (level).paddedLeft ('0', 2)
                                    + rows[channel].substring (at + 2);
        };

        auto saveAndLoad = [] (GrooveboxProcessor& from, GrooveboxProcessor& to)
        {
            juce::MemoryBlock blob;
            from.getStateInformation (blob);
            to.setStateInformation (blob.getData(), (int) blob.getSize());
        };

        /**
            A session as the build before this one wrote it: two bytes a step, the
            flag and the 7-bit level, and no room at all for a probability.

            The layout is spelled out here rather than handed back by the codec under
            test, because the claim being made is about a *reader*. Only the base64 is
            borrowed, and the alphabet is not what moved between these two builds — the
            width of a step is.
        */
        auto v2Payload = [] (std::initializer_list<std::tuple<int, int, int>> hits)
        {
            std::array<juce::uint8, (size_t) Layout::numChannels * Layout::numSteps * 2> bytes {};

            for (const auto& [channel, step, level] : hits)
            {
                const auto at = (size_t) (channel * Layout::numSteps + step) * 2;
                bytes[at] = 1;
                bytes[at + 1] = (juce::uint8) level;
            }

            return "v2:" + juce::Base64::toBase64 (bytes.data(), bytes.size());
        };

        /** One MIDI step of velocity, which is what a 7-bit level is worth. */
        constexpr auto stepOfOne = 1.0 / 127.0;

        GrooveboxProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);
        clearAllSteps (processor);

        const auto payload = payloadOf (processor);
        const auto& state = processor.getValueTreeState().state;

        std::printf ("        this build writes %d characters of payload: %.32s...\n",
                     (int) payload.length(), payload.toRawUTF8());

        check (state.hasProperty ("stateVersion"),
               "the root state says which format it is, instead of leaving a reader to guess",
               juce::String ("the saved tree holds no stateVersion at all, so a compatibility "
                              "decision has to be made from what the string looks like"));

        // Both numbers are written down rather than read back from `Layout::stateVersion`
        // and the codec's own stride. A gate that derived its expectation from the code
        // under test could not disagree with it, and a format change is exactly the kind
        // of thing this section exists to make expensive: these two lines are the edit.
        check ((int) state.getProperty ("stateVersion") == 3,
               "and the version it names is the one this build writes",
               "got " + juce::String ((int) state.getProperty ("stateVersion")));

        check (payload.startsWith ("v3:"),
               "the payload names its own codec, so a length cannot be mistaken for a version",
               "starts with " + payload.substring (0, 4));

        // v3 is three bytes a step: the flag, the 7-bit level and the chance the step
        // gets, as 255ths. Base64 of 192 steps is 4 characters per 3 bytes, rounded up.
        // Naming the width here is the point - a payload that quietly changes size
        // changes what every session means.
        constexpr int stepBytes = 3;
        const auto expectedLength = 3
            + ((Layout::numChannels * Layout::numSteps * stepBytes + 2) / 3) * 4;

        check (payload.length() == expectedLength,
               "and it is exactly wide enough for " + juce::String (stepBytes)
                   + " bytes a step, no wider",
               juce::String (payload.length()) + " against " + juce::String (expectedLength));

        //======================================================================
        // Resolution: a step's velocity is a MIDI number, and 128 of them have to
        // survive being written down. The v1 rows hold 0..99, so most of these were
        // never expressible at all.
        {
            GrooveboxProcessor wide, wideBack;
            wide.prepareToPlay (sampleRate, blockSize);
            wideBack.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (wide);

            for (int k = 0; k < 128; ++k)
            {
                wide.setStep (k / Layout::numSteps, k % Layout::numSteps, true);
                wide.setStepVelocity (k / Layout::numSteps, k % Layout::numSteps, (float) k / 127.0f);
            }

            saveAndLoad (wide, wideBack);

            auto lost = 0;
            auto worst = 0.0f;

            for (int k = 0; k < 128; ++k)
            {
                const auto channel = k / Layout::numSteps;
                const auto step = k % Layout::numSteps;
                const auto want = (float) k / 127.0f;

                if (! wideBack.isStepOn (channel, step))
                {
                    ++lost;
                    continue;
                }

                const auto error = std::abs (wideBack.getStepVelocity (channel, step) - want);
                worst = juce::jmax (worst, error);

                if (error > 1.0e-6f)
                    ++lost;
            }

            std::printf ("        all 128 velocities k/127 across eight lanes: %d wrong, worst off by %.6f\n",
                         lost, (double) worst);

            check (lost == 0,
                   "every velocity a MIDI number can name comes back exactly",
                   juce::String (lost) + " of 128 come back elsewhere, worst by "
                       + juce::String (worst, 6) + " ("
                       + juce::String (worst / (float) stepOfOne, 3) + " MIDI steps)");
        }

        //======================================================================
        // A step that is off still carries the level it will come back at - the
        // engine header has said so all along, and the grid paints velocity on off
        // cells - so the payload has to keep the two fields apart rather than encode
        // off as "zero velocity".
        {
            GrooveboxProcessor quiet, quietBack;
            quiet.prepareToPlay (sampleRate, blockSize);
            quietBack.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (quiet);

            quiet.setStep (5, 9, false);
            quiet.setStepVelocity (5, 9, 64.0f / 127.0f);

            saveAndLoad (quiet, quietBack);

            std::printf ("        an off step at 64/127: on %d, velocity %.6f\n",
                         (int) quietBack.isStepOn (5, 9),
                         (double) quietBack.getStepVelocity (5, 9));

            check (! quietBack.isStepOn (5, 9), "an off step comes back off",
                   juce::String ((int) quietBack.isStepOn (5, 9)));

            check (std::abs (quietBack.getStepVelocity (5, 9) - 64.0f / 127.0f) < 1.0e-6f,
                   "and keeps the level it was given while it was off",
                   "left at 64/127, came back at " + juce::String (quietBack.getStepVelocity (5, 9), 6)
                       + " -- " + juce::String ((quietBack.getStepVelocity (5, 9) - 64.0f / 127.0f) * 127.0f, 1)
                       + " of a MIDI step away from it");
        }

        //======================================================================
        // The other half of that, through the two operations a drummer actually
        // uses: turning a row and saving it. A shift that moved only the lit cells
        // would look right in the grid and quietly drop the levels waiting behind
        // them, and a payload that cannot hold a level for an off step would do the
        // same on the way to disk. So the row is shifted first and the round trip
        // is what is measured.
        {
            GrooveboxProcessor moved, movedBack;
            moved.prepareToPlay (sampleRate, blockSize);
            movedBack.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (moved);

            moved.setStep (7, 2, true);
            moved.setStepVelocity (7, 2, 100.0f / 127.0f);
            moved.setStepVelocity (7, 3, 33.0f / 127.0f);
            moved.setStep (7, 15, true);
            moved.setStepVelocity (7, 15, 1.0f);

            moved.shiftChannelRow (7, 3);

            saveAndLoad (moved, movedBack);

            std::printf ("        a row turned by 3 and saved back: (7,5)=%d@%.4f (7,6)=%d@%.4f (7,2)=%d@%.4f\n",
                         (int) movedBack.isStepOn (7, 5), (double) movedBack.getStepVelocity (7, 5),
                         (int) movedBack.isStepOn (7, 6), (double) movedBack.getStepVelocity (7, 6),
                         (int) movedBack.isStepOn (7, 2), (double) movedBack.getStepVelocity (7, 2));

            check (movedBack.isStepOn (7, 5) && movedBack.isStepOn (7, 2) && ! movedBack.isStepOn (7, 6),
                   "a shifted row comes back with its steps where the turn put them",
                   "from 2, 3 and 15 by three, got " + juce::String ((int) movedBack.isStepOn (7, 5))
                       + juce::String ((int) movedBack.isStepOn (7, 6))
                       + juce::String ((int) movedBack.isStepOn (7, 2)));

            const auto offBy = [&movedBack] (int step, float want)
            {
                return std::abs (movedBack.getStepVelocity (7, step) - want) * 127.0f;
            };

            const auto travel = offBy (5, 100.0f / 127.0f) + offBy (6, 33.0f / 127.0f) + offBy (2, 1.0f);

            check (travel < 1.0e-4f,
                   "and every step's level travelled with it, including the one left off",
                   "steps 5, 6 and 2 came back " + juce::String (offBy (5, 100.0f / 127.0f), 1) + ", "
                       + juce::String (offBy (6, 33.0f / 127.0f), 1) + " and "
                       + juce::String (offBy (2, 1.0f), 1) + " of a MIDI step away from where they were left");
        }

        //======================================================================
        // Back, not just forward. Sessions already on this machine are v1 rows of
        // digits and v0 rows of ones and zeros; both have to keep loading, and the
        // rules the plan flagged - a short row invents nothing, a long row cannot
        // spill into the next channel - have to stay rules rather than accidents.
        {
            GrooveboxProcessor old, oldBack;
            old.prepareToPlay (sampleRate, blockSize);
            oldBack.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (old);

            auto rows = v1Rows (0, 0, 75);
            v1Set (rows, 0, 5, 5);
            v1Set (rows, 2, 15, 99);
            loadSession (oldBack, rows.joinIntoString (","));

            std::printf ("        a v1 session: (0,0)=%.4f (0,5)=%.4f (2,15)=%.4f\n",
                         (double) oldBack.getStepVelocity (0, 0),
                         (double) oldBack.getStepVelocity (0, 5),
                         (double) oldBack.getStepVelocity (2, 15));

            check (oldBack.isStepOn (0, 0) && oldBack.isStepOn (0, 5) && oldBack.isStepOn (2, 15),
                   "a session of v1 digits still loads",
                   "got " + juce::String ((int) oldBack.isStepOn (0, 0)) + juce::String ((int) oldBack.isStepOn (0, 5))
                       + juce::String ((int) oldBack.isStepOn (2, 15)));

            // Half a MIDI step, because a v1 level lives on a lattice of hundredths:
            // what is being promised is that the new format is finer than the old one,
            // not that it can hold a hundredth exactly.
            const auto v1Tolerance = 0.5f / 127.0f;

            check (std::abs (oldBack.getStepVelocity (0, 0) - 0.75f) <= v1Tolerance,
                   "at the level its own digits said, to half the new format's step",
                   juce::String (oldBack.getStepVelocity (0, 0), 6) + " against the 0.75 of '75'");

            check (std::abs (oldBack.getStepVelocity (0, 5) - 0.05f) <= v1Tolerance,
                   "including the quiet end, where a hundredth is nearly half a step",
                   juce::String (oldBack.getStepVelocity (0, 5), 6) + " against the 0.05 of '05'");

            check (std::abs (oldBack.getStepVelocity (2, 15) - 0.99f) <= v1Tolerance,
                   "and the last step of the last row, where a shift or an off-by-one shows",
                   juce::String (oldBack.getStepVelocity (2, 15), 6) + " against the 0.99 of '99'");

            check (! oldBack.isStepOn (1, 1) && ! oldBack.isStepOn (11, 0),
                   "and the cells the digits left at zero are still off");

            // v2, the codec one generation behind this one. It is not a hypothetical:
            // it is what every session saved since 4.1 holds. What it lacks is not a
            // field but a *width*, so a reader that indexed the payload by one
            // compile-time stride — the constant this build replaced with
            // `strideForCodec` — has no answer for it, and the answer it needs is
            // "a step that never mentioned a probability is rolled every time", not
            // "that byte was zero, so the lane is silent".
            {
                GrooveboxProcessor fromV2;
                fromV2.prepareToPlay (sampleRate, blockSize);
                clearAllSteps (fromV2);

                loadSession (fromV2, v2Payload ({ { 0, 4, 100 }, { 3, 15, 64 }, { 11, 0, 127 } }), 2);

                std::printf ("        a v2 session: (0,4)=%.4f@%.4f  (11,0)=%.4f@%.4f chance %.4f/%.4f\n",
                             (double) fromV2.isStepOn (0, 4), (double) fromV2.getStepVelocity (0, 4),
                             (double) fromV2.isStepOn (11, 0), (double) fromV2.getStepVelocity (11, 0),
                             (double) fromV2.getStepChance (0, 4), (double) fromV2.getStepChance (11, 0));

                check (fromV2.isStepOn (0, 4) && fromV2.isStepOn (3, 15) && fromV2.isStepOn (11, 0)
                           && ! fromV2.isStepOn (0, 5) && ! fromV2.isStepOn (11, 1),
                       "a session written by the previous build still loads, last lane included",
                       "got " + juce::String ((int) fromV2.isStepOn (0, 4))
                           + juce::String ((int) fromV2.isStepOn (3, 15))
                           + juce::String ((int) fromV2.isStepOn (11, 0)));

                const auto levelOffBy = [&fromV2] (int channel, int step, int want)
                {
                    return std::abs (fromV2.getStepVelocity (channel, step)
                                      - (float) want / 127.0f) * 127.0f;
                };

                const auto levelsTravelled = levelOffBy (0, 4, 100) + levelOffBy (3, 15, 64)
                                               + levelOffBy (11, 0, 127);

                check (levelsTravelled < 1.0e-4f,
                       "at the levels its own bytes named, to a thousandth of a MIDI step",
                       "off by " + juce::String (levelOffBy (0, 4, 100), 4) + ", "
                           + juce::String (levelOffBy (3, 15, 64), 4) + " and "
                           + juce::String (levelOffBy (11, 0, 127), 4));

                auto neverRolled = 0;

                for (int channel = 0; channel < Layout::numChannels; ++channel)
                    for (int step = 0; step < Layout::numSteps; ++step)
                        if (fromV2.getStepChance (channel, step) != 1.0f)
                            ++neverRolled;

                check (neverRolled == 0,
                       "and every step it says nothing about is rolled every time",
                       juce::String (neverRolled)
                           + " step(s) came in with a probability that is not 1.0");
            }

            // v0: one character per step, 1 meaning on at full velocity.
            GrooveboxProcessor v0;
            v0.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (v0);

            juce::String v0Rows;

            for (int channel = 0; channel < Layout::numChannels; ++channel)
            {
                v0Rows += (channel == 3 ? juce::String ("0000001000000000")
                                         : juce::String ("0000000000000000"));

                v0Rows += channel < Layout::numChannels - 1 ? ',' : ' ';
            }

            loadSession (v0, v0Rows.trim());

            check (v0.isStepOn (3, 6) && ! v0.isStepOn (3, 5),
                   "a v0 session of bare ones and zeros still loads",
                   "got " + juce::String ((int) v0.isStepOn (3, 6)));

            check (std::abs (v0.getStepVelocity (3, 6) - 1.0f) < 1.0e-6f,
                   "and its steps come in at full velocity, as they always did",
                   juce::String (v0.getStepVelocity (3, 6), 6));

            // Short row, long row. The truncation is the behaviour; what matters is
            // that it is bounded and cannot reach into a neighbouring channel.
            GrooveboxProcessor ragged, spilled;
            ragged.prepareToPlay (sampleRate, blockSize);
            spilled.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (ragged);
            clearAllSteps (spilled);

            auto shortRows = v1Rows (0, 0, 75);
            shortRows.getReference (0) = "75";
            shortRows.getReference (1) = v1Rows (1, 3, 50).getReference (1);

            loadSession (ragged, shortRows.joinIntoString (","));

            check (ragged.isStepOn (0, 0) && ! ragged.isStepOn (0, 1) && ! ragged.isStepOn (0, 15),
                   "a row shorter than the grid leaves the steps it does not reach off",
                   "got " + juce::String ((int) ragged.isStepOn (0, 1)) + " "
                       + juce::String ((int) ragged.isStepOn (0, 15)));

            check (ragged.isStepOn (1, 3),
                   "and the next row is read from where the short one stopped",
                   "row 1 step 3 came back " + juce::String ((int) ragged.isStepOn (1, 3)));

            auto longRows = v1Rows (0, 0, 75);
            longRows.getReference (0) += "5060708090";

            loadSession (spilled, longRows.joinIntoString (","));

            auto stray = 0;

            for (int step = 1; step < Layout::numSteps; ++step)
                if (spilled.isStepOn (0, step))
                    ++stray;

            check (stray == 0 && spilled.isStepOn (0, 0),
                   "and a row longer than the grid is truncated at the edge, not spliced into it",
                   juce::String (stray) + " step(s) beyond step 0 in a row that holds one");
        }

        //======================================================================
        // Forward. A payload from a newer build than this one has fields this code
        // cannot name, and there is exactly one safe answer to that: leave it alone.
        {
            GrooveboxProcessor future;
            future.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (future);
            future.setStep (0, 3, true);

            const auto ours = payloadOf (future);

            loadSession (future, "v99:" + ours.substring (3), 99);

            const auto afterLoad = payloadOf (future);

            check (afterLoad == "v99:" + ours.substring (3),
                   "a payload from a newer build passes straight back out, untouched",
                   "came in as v99 and left as " + afterLoad.substring (0, 4));

            // The header of the record agrees with the payload inside it: stamping 2 on
            // top of a v99 pattern would describe a record this build did not write.
            check ((int) future.getValueTreeState().state.getProperty ("stateVersion") == 99,
                   "and the version on the root is left at what came in",
                   "the root now says "
                       + juce::String ((int) future.getValueTreeState().state.getProperty ("stateVersion")));

            // What the machine plays while it is holding one is nothing at all. The
            // README claims an unreadable session comes back silent rather than as the
            // pattern that happened to be on screen before the load, so the step that
            // was lit a moment ago has to be out.
            check (! future.isStepOn (0, 3) && ! future.isStepOn (1, 1),
                   "and a build that cannot read it plays nothing it did not read",
                   "the grid still holds a step from before the load");

            future.setStep (1, 1, true);

            check (payloadOf (future).startsWith ("v3:"),
                   "and painting one cell takes the row over in this build's own format",
                   "after an edit it writes " + payloadOf (future).substring (0, 4));
        }
    }

    //======================================================================
    std::printf ("\n[28] a render has a fingerprint\n");
    {
        /**
            What the kit below sounds like, recorded as one number.

            A baseline, not a threshold: captured from this file on the tree before the
            variation features and printed on every run, so a red line names both halves
            of the hash that moved. What makes it a gate rather than a figure somebody
            re-records whenever it is inconvenient is the pair of checks around it — the
            same kit rendered twice has to give the same bits, a session saved and
            loaded back has to give the same bits, and one level changed has to give
            different ones. A fingerprint that could not tell those apart would be
            measuring nothing, whatever it matched.

            Recorded from `out/smoke-42-fingerprint.txt`: peak 0.68136, 161280 samples,
            on the tree that ships the v2 step payload and no variation features. It is
            pinned to this machine and this runtime, as every exact-sample claim in
            this file is.
        */
        constexpr juce::uint64 goldenFingerprint = 0x4BC3AA231178C071ull;

        const auto noiseSamples = makeNoiseSamples (30000);
        const auto noiseWav = makeNoiseWav ("fingerprint-noise", noiseSamples, sampleRate);
        const auto toneWav  = makeToneWav ("fingerprint-tone", 997.0, 22050, sampleRate);
        const auto clickWav = makeClickWav ("fingerprint-click", 2048, sampleRate);

        /**
            A bar that touches every part of the path on purpose.

            Four lanes rather than one, because this number has to stand for the whole
            machine: a sustained lane pitched up, which is the resampler's higher cutoff
            band and a tail that runs across the bar boundary; a lane dropped an octave,
            which is the glide and the renormalisation at the end of the source; two
            lanes in a choke group, which is the fade of a hit that is not the one being
            struck; and hits on the off-beats under 1/16 swing, so the sub-sample
            timings are in the bits rather than only in the transport.

            Every velocity is `k / 127`. Not a stylistic choice: the saved record stores
            a level as a 7-bit number, so an arbitrary float comes back on the nearest
            rung of that lattice and the render moves by up to half a MIDI step — which
            is what [27] measures. A fingerprint that intends to say "the session that
            came back is the machine that went in" has to start from levels the record
            can actually hold.

            `step0Level` is the one value the sensitivity check moves.
        */
        auto armKit = [&] (GrooveboxProcessor& p, int step0Level)
        {
            p.prepareToPlay (sampleRate, blockSize);

            juce::String error;
            const auto loaded = p.loadSampleIntoChannel (0, noiseWav, error)
                                  && p.loadSampleIntoChannel (1, toneWav, error)
                                  && p.loadSampleIntoChannel (2, clickWav, error)
                                  && p.loadSampleIntoChannel (3, noiseWav, error);

            if (! loaded)
                std::printf ("        !! the kit did not load: %s\n", error.toRawUTF8());

            clearAllSteps (p);

            // Every value below is a dyadic fraction on purpose. The pattern codec has
            // a 7-bit lattice spelled out in the comment above; the parameters are
            // floats written into the state as text, and a number like 0.95 would put
            // this check at the mercy of that text round trip for no reason except that
            // somebody liked the sound of the digit.
            for (int channel = 0; channel < 4; ++channel)
            {
                p.setChannelLevel (channel, 1.0f - 0.25f * (float) channel);
                p.setChannelPitch (channel, channel == 1 ? 7 : -2 * channel);
                p.setChannelAttack (channel, channel == 1 ? 0.25f : 0.0f);
                p.setChannelDecay (channel, 0.25f + 0.125f * (float) channel);
                p.setChannelSustain (channel, channel == 1 ? 0.5f : 0.125f);
                p.setChannelRelease (channel, 0.125f + 0.125f * (float) channel);
            }

            p.setChannelPitchDrop (3, 12);
            p.setChannelChokeGroup (2, 1);
            p.setChannelChokeGroup (3, 1);

            struct Hit
            {
                int channel, step, level;
            };

            const Hit hits[] = { { 0, 0, step0Level }, { 0, 6, 79 },   { 0, 10, 108 },
                                 { 1, 2, 114 },  { 1, 7, 70 },    { 1, 11, 127 },   { 1, 14, 44 },
                                 { 2, 0, 95 },   { 2, 4, 127 },   { 2, 8, 63 },     { 2, 12, 114 },
                                 { 3, 3, 88 },   { 3, 5, 50 },    { 3, 9, 127 },    { 3, 13, 76 } };

            for (const auto& hit : hits)
            {
                p.setStep (hit.channel, hit.step, true);
                p.setStepVelocity (hit.channel, hit.step, (float) hit.level / 127.0f);
            }

            p.setManualTempo (132.0);
            p.setSwingGrid (2);   // the 1/16s: every off-beat step then lands off-grid
            p.setSwing (0.25f);
            p.setMasterGainDb (-3.5f);
        };

        /** Just over two bars at 132, so a leak per onset has room to add up. */
        constexpr int fingerprintBlocks = 630;
        const int fingerprintSamples = fingerprintBlocks * blockSize;

        /**
            One render, from a processor somebody else armed.

            The trim arrives through a 20 ms smoother, so it is given silent blocks to
            converge in before the transport is armed. Without that the fingerprint
            records the ramp rather than the kit, and a change to the smoothing moves
            it for a reason that has nothing to do with the sound.
        */
        auto take = [&] (GrooveboxProcessor& p, int numBlocks)
        {
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
            p.setPlaying (true);

            auto out = render (p, numBlocks * blockSize, blockSize, sampleRate);
            p.setPlaying (false);

            return out;
        };

        GrooveboxProcessor processor;
        armKit (processor, 127);
        const auto taken = take (processor, fingerprintBlocks);
        const auto stamp = fingerprintOf (taken);

        GrooveboxProcessor again;
        armKit (again, 127);
        const auto repeat = fingerprintOf (take (again, fingerprintBlocks));

        GrooveboxProcessor softer;
        armKit (softer, 64);
        const auto moved = take (softer, fingerprintBlocks);

        GrooveboxProcessor halved;
        armKit (halved, 127);
        const auto shortStamp = fingerprintOf (take (halved, fingerprintBlocks / 2));

        std::printf ("        fingerprint %s over %d samples, peak %s, rms %s\n",
                     hexOf (stamp).toRawUTF8(), (int) taken.left.size(),
                     juce::String (peakOf (taken.left), 5).toRawUTF8(),
                     juce::String (rmsOf (taken.left), 5).toRawUTF8());

        // The precondition every claim under it rests on: a fingerprint of silence is
        // one number, and silence would reproduce it perfectly.
        check (peakOf (taken.left) > 0.1f && peakOf (taken.right) > 0.1f,
               "the kit is playing, so there is something to fingerprint",
               "peak " + juce::String (peakOf (taken.left), 5) + " / "
                   + juce::String (peakOf (taken.right), 5));

        // Energy in the second bar: a render that only holds the first one would let a
        // fingerprint of two samples pass as one of two bars.
        check (peakFrom (taken.left, fingerprintSamples / 2) > 0.05f,
               "and it is still playing in the second bar",
               "nothing above 0.05 after sample " + juce::String (fingerprintSamples / 2));

        check (stamp == repeat,
               "the same kit, armed the same way and rendered twice, is the same bits",
               hexOf (stamp) + " against " + hexOf (repeat));

        /**
            The claim the variation features have to answer.

            4.2 adds a per-step probability and one mutate amount, and promises that at
            a probability of one it changes nothing about what the engine renders. That
            promise is this line. It is checked against a number recorded from the tree
            before the feature, not against a tolerance invented afterwards, so
            "structurally untouched" is either true in the bits or it is red.
        */
        check (stamp == goldenFingerprint,
               "and the render is the one recorded before the variation features",
               "got " + hexOf (stamp) + ", recorded " + hexOf (goldenFingerprint)
                   + "; if the DSP moved on purpose, re-capture from a run you have read");

        check (fingerprintOf (moved) != stamp,
               "one level changed does change it, so the gate can see what it claims",
               "the first kick at 64 instead of 127 fingerprinted back as "
                   + hexOf (fingerprintOf (moved)));

        // The difference has to start where the change is, not eventually: a hash that
        // only disagreed in the last few samples would be recording the tails of a
        // render nobody could have heard a difference in.
        check (firstDifference (taken.left, moved.left, 0, fingerprintSamples) >= 0
                 && firstDifference (taken.left, moved.left, 0, fingerprintSamples)
                      < fingerprintSamples / 4,
               "and it parts company in the first bar, on the hit that was changed",
               "first difference at sample "
                   + juce::String (firstDifference (taken.left, moved.left, 0, fingerprintSamples)));

        check (shortStamp != stamp,
               "the length is folded in, so a render that stops early is not this record",
               hexOf (shortStamp));

        /**
            A session that comes back is the machine that went in.

            This earns its place by going through the save path and the audio rather
            than through getters. Every check in [27] reads the grid back through an
            accessor, so none of them can see a step that reads correctly and does not
            fire — and a field a codec forgets to carry, which is exactly what the
            probability of 4.2 risks, is one lane gone silent and nothing else.

            The two halves belong together: the record has to come back cell for cell,
            and the kit has to sound like it did. The first is the precondition, and
            without it a red line here would not say which of the two moved.
        */
        {
            juce::MemoryBlock blob;
            processor.getStateInformation (blob);

            GrooveboxProcessor restored;
            restored.prepareToPlay (sampleRate, blockSize);
            restored.setStateInformation (blob.getData(), (int) blob.getSize());

            auto cells = 0;

            for (int channel = 0; channel < Layout::numChannels; ++channel)
                for (int step = 0; step < Layout::numSteps; ++step)
                    if (processor.isStepOn (channel, step) != restored.isStepOn (channel, step)
                          || processor.getStepVelocity (channel, step)
                               != restored.getStepVelocity (channel, step))
                        ++cells;

            check (cells == 0,
                   "the saved record comes back cell for cell, on the lattice it stores",
                   juce::String (cells) + " step(s) read back different");

            const auto roundTrip = take (restored, fingerprintBlocks);
            const auto tripStamp = fingerprintOf (roundTrip);

            std::printf ("        restored %s, first difference at sample %d\n",
                         hexOf (tripStamp).toRawUTF8(),
                         firstDifference (taken.left, roundTrip.left, 0, fingerprintSamples));

            check (tripStamp == stamp,
                   "and saving the kit, loading it back and playing it gives the same bits",
                   "restored as " + hexOf (tripStamp) + " against " + hexOf (stamp)
                       + ", first difference at sample "
                       + juce::String (firstDifference (taken.left, roundTrip.left, 0, fingerprintSamples)));
        }

        noiseWav.deleteFile();
        toneWav.deleteFile();
        clickWav.deleteFile();
    }

    //======================================================================
    /**
        4.2's other half, and the half the plan actually named.

        [28] answers "did adding a probability change what a certain grid renders" —
        it is a baseline and it is a no-op proof. This section answers the question a
        listener would ask next: is a take a *take*. Two claims, both of which a naive
        `juce::Random` in the audio loop would fail — a counter that advances per roll
        makes every pass different, and a draw that reads the wall clock makes a bounce
        disagree with the live pass it came from.

        Everything here asks the engine's own `wouldThisHitFire`, which is the only
        thing that decides whether a painted step sounds, rather than a model of it in
        the test. The audio checks at the bottom are what tie the seam to the samples:
        a seam that was never called by `fireStep` would answer every question below
        perfectly and still play the grid as if the knob did not exist.
    */
    std::printf ("\n[29] a probability is a take, not a shimmer\n");
    {
        /** Sixteen bars is the plan's sixty-four, at the resolution this needs: every
            bar's own key must appear, and the aggregate has to be long enough that a
            generator which is not avalanching shows up in the fraction. */
        constexpr int rollBars = 64;
        constexpr int rollLanes = 4;
        constexpr int rollCells = rollBars * Layout::numSteps * rollLanes;

        const auto clickWav = makeClickWav ("roll-click", 1200, sampleRate);

        /**
            The grid every question below is asked against.

            Every step of four lanes painted on, so a drop can only ever come from the
            roll and never from a hole in the pattern, and every velocity at 127 so the
            hits are the same ones wherever they survive. The lanes are the machine's,
            not the test's: the knob and the seed go in through the parameters the host
            would use.
        */
        auto armGrid = [&] (GrooveboxProcessor& p, int forBlockSize)
        {
            p.prepareToPlay (sampleRate, forBlockSize);

            juce::String error;
            for (int channel = 0; channel < rollLanes; ++channel)
                p.loadSampleIntoChannel (channel, clickWav, error);

            clearAllSteps (p);

            for (int channel = 0; channel < rollLanes; ++channel)
                for (int step = 0; step < Layout::numSteps; ++step)
                {
                    p.setStep (channel, step, true);
                    p.setStepVelocity (channel, step, 1.0f);
                }

            p.setManualTempo (132.0);
            p.setSwingGrid (2);
            p.setSwing (0.25f);
            p.setMasterGainDb (-3.5f);
        };

        auto setEveryChance = [&] (GrooveboxProcessor& p, float chance)
        {
            for (int channel = 0; channel < rollLanes; ++channel)
                for (int step = 0; step < Layout::numSteps; ++step)
                    p.setStepChance (channel, step, chance);
        };

        /**
            The whole take, as one byte per cell.

            Every cell goes into the slot that names it — bar, then step, then lane —
            and the walk itself can run either direction. That is the point: an answer
            that depends on what was asked before it lands in different slots when the
            grid is walked backwards, and a vector built by `push_back` would hide that
            behind a difference in order. Indexed, the two walks have to agree cell for
            cell or something in the roll is remembering.
        */
        auto decisions = [&] (GrooveboxProcessor& p, bool backwards)
        {
            std::vector<juce::uint8> cells ((size_t) rollCells, 0);

            const auto slot = [&] (int bar, int step, int lane)
            {
                return (size_t) ((bar * Layout::numSteps + step) * rollLanes + lane);
            };

            /**
                The walk, mirrored rather than written twice.

                The first version of this counted down from `numSteps - (backwards ? 1
                : 0)`, which in the forward direction starts the counter one *past* its
                own end condition: the body ran zero times, every cell kept the zero it
                was allocated with, and seven checks went red against a machine that had
                been answering correctly all along. Worse are the three that went green —
                `no cell comes back as the knob opens` and `a cell that says never is
                never kept` are both satisfied by an empty answer, which is the same
                harness fault AGENTS.md already records for a load that never happened.
                Mirroring one index makes the two walks the same shape, so the only thing
                they can differ in is the answers themselves.
            */
            const auto atIndex = [backwards] (int count, int i) { return backwards ? count - 1 - i : i; };

            for (int i = 0; i < rollBars; ++i)
                for (int j = 0; j < Layout::numSteps; ++j)
                    for (int k = 0; k < rollLanes; ++k)
                    {
                        const auto bar  = atIndex (rollBars, i);
                        const auto step = atIndex (Layout::numSteps, j);
                        const auto lane = atIndex (rollLanes, k);

                        cells[slot (bar, step, lane)] = p.wouldThisStepHit (bar, step, lane) ? 1 : 0;
                    }

            return cells;
        };

        auto hashOf = [] (const std::vector<juce::uint8>& cells)
        {
            return fnv1a (0xcbf29ce484222325ull, cells.data(), cells.size());
        };

        auto firedOf = [] (const std::vector<juce::uint8>& cells)
        {
            auto fired = 0;

            for (const auto cell : cells)
                fired += cell != 0 ? 1 : 0;

            return fired;
        };

        /**
            One render of about two bars, so a bar's key is exercised more than once.
            The same settle-before-arming as [28]: the trim has to be still when the
            first onset happens, or the fingerprint records a ramp.
        */
        auto takeRoll = [&] (GrooveboxProcessor& p)
        {
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
            p.setPlaying (true);

            auto out = render (p, 320 * blockSize, blockSize, sampleRate);
            p.setPlaying (false);

            return out;
        };

        /** A probability a cell can be set to, on the 1/255 lattice the record holds. */
        const auto half = 128.0f / (float) Layout::chanceLevels;

        GrooveboxProcessor machine;
        armGrid (machine, blockSize);
        machine.setSeed (7);
        setEveryChance (machine, half);
        machine.setMutate (1.0f);

        GrooveboxProcessor mirror;
        armGrid (mirror, 256);
        mirror.setSeed (7);
        setEveryChance (mirror, half);
        mirror.setMutate (1.0f);

        const auto forward = decisions (machine, false);
        const auto reverse = decisions (mirror, true);

        GrooveboxProcessor shut;
        armGrid (shut, blockSize);
        shut.setSeed (7);
        setEveryChance (shut, half);
        shut.setMutate (0.0f);

        const auto closed = decisions (shut, false);

        std::printf ("        %d cells at mutate 1: %d fire (%.4f of them), "
                     "at mutate 0: %d fire, seed 7 forward %s\n",
                     rollCells, firedOf (forward), (float) firedOf (forward) / (float) rollCells,
                     firedOf (closed), hexOf (hashOf (forward)).toRawUTF8());

        // The precondition: a take that dropped everything, or nothing, would let every
        // claim under it pass on a degenerate answer.
        check (firedOf (closed) == rollCells,
               "with variation shut every cell the grid claims is rolled through",
               juce::String (firedOf (closed)) + " of " + juce::String (rollCells));

        check (firedOf (forward) > 0 && firedOf (forward) < rollCells,
               "and with it open some cells drop and some do not",
               juce::String (firedOf (forward)) + " of " + juce::String (rollCells) + " fire");

        check (hashOf (forward) == hashOf (reverse),
               "the same seed and the same grid give the same take, walked in either order",
               hexOf (hashOf (forward)) + " against " + hexOf (hashOf (reverse))
                   + ", on a second machine prepared at a 256-sample block");

        // The plan's phrase, made checkable: nothing in the answer is a clock. A bar
        // asked for twice on one machine is the same answer twice, and no cell knows
        // what was asked before it.
        check (hashOf (decisions (machine, false)) == hashOf (forward),
               "and asking the same machine twice does not advance anything",
               hexOf (hashOf (decisions (machine, false))) + " against " + hexOf (hashOf (forward)));

        GrooveboxProcessor otherSeed;
        armGrid (otherSeed, blockSize);
        otherSeed.setSeed (8);
        setEveryChance (otherSeed, half);
        otherSeed.setMutate (1.0f);

        const auto reseeded = decisions (otherSeed, false);

        auto disagreed = 0;

        for (size_t i = 0; i < (size_t) rollCells; ++i)
            if (forward[i] != reseeded[i])
                ++disagreed;

        std::printf ("        seed 7 against seed 8: %d of %d cells move (%.4f)\n",
                    disagreed, rollCells, (float) disagreed / (float) rollCells);

        check (hashOf (reseeded) != hashOf (forward),
               "a different seed is a different take", hexOf (hashOf (reseeded)));

        // Half the cells moving is what the avalanche is for. Below a fifth, a neighbour
        // of the seed leaves most of the grid where it was — a multiply that does not
        // mix; above four fifths, one step changes every answer — a key that is not
        // keyed on the cell at all.
        check (disagreed > rollCells / 5 && disagreed < rollCells * 4 / 5,
               "and neighbouring seeds disagree about about half of it, cell by cell",
               juce::String ((float) disagreed / (float) rollCells, 4) + " of cells move");

        /**
            Opening the knob can only ever take hits away.

            The draw is a function of the cell alone, and the threshold it is compared
            against falls as the knob opens, so the fired set at one setting has to be a
            subset of the fired set at every setting below it. That is a law about the
            vectors, not a tolerance on a count: a single cell that comes back when the
            knob opened further is the whole failure.
        */
        std::vector<std::vector<juce::uint8>> ladder;

        for (int rung = 0; rung <= 4; ++rung)
        {
            GrooveboxProcessor m;
            armGrid (m, blockSize);
            m.setSeed (7);
            setEveryChance (m, half);
            m.setMutate ((float) rung * 0.25f);

            ladder.push_back (decisions (m, false));
        }

        auto added = 0;

        for (size_t rung = 1; rung < ladder.size(); ++rung)
            for (size_t i = 0; i < (size_t) rollCells; ++i)
                if (ladder[rung][i] != 0 && ladder[rung - 1][i] == 0)
                    ++added;

        std::printf ("        mutate ladder at 0, .25, .5, .75, 1: %d fire",
                     firedOf (ladder[0]));

        for (size_t rung = 1; rung < ladder.size(); ++rung)
            std::printf (" then %d", firedOf (ladder[rung]));

        std::printf ("\n");

        check (added == 0,
               "and no setting of the knob lets a hit through that a lower one dropped",
               juce::String (added) + " cell(s) appear as the knob opens");

        /**
            The discount the knob is supposed to be applying.

            A cell at 128/255 with the knob half open asks for `1 - 0.5 * (1 - 128/255)`
            = 0.75098 — so the number it is compared against is the engine's own formula
            re-evaluated, not a rounded saying of it.

            The band is measured, not assumed. The first version here used four standard
            deviations of a fair coin over these 4096 cells — 0.0271 — and the take this
            seed actually gives sits 0.0200 from the expectation, inside by six thousandths
            of a cell. That is the same mistake as typing a constant: a hash is not a coin,
            and the spread of a hash over neighbouring seeds is a property of the hash that
            can simply be measured. So the band is three times the largest departure from
            their mean over 31 *other* seeds — seed 7 stays out of the sample, because a
            limit drawn around the point it then gates is a tautology — and it still leaves
            the nearest wrong discount, a quarter of the range away, far outside it.
        */
        constexpr int spreadSeeds = 31;

        GrooveboxProcessor spreader;
        armGrid (spreader, blockSize);
        setEveryChance (spreader, half);
        spreader.setMutate (0.5f);

        std::array<float, spreadSeeds> fractions {};

        for (int i = 0; i < spreadSeeds; ++i)
        {
            // Well-spaced seeds, not neighbours: this is measuring the generator's
            // spread over the space a reroll can actually land in.
            spreader.setSeed (1000 + i * 7717);

            fractions[(size_t) i] = (float) firedOf (decisions (spreader, false))
                                      / (float) rollCells;
        }

        auto total = 0.0f;

        for (const auto fraction : fractions)
            total += fraction;

        const auto mean = total / (float) spreadSeeds;

        auto radius = 0.0f;

        for (const auto fraction : fractions)
            radius = juce::jmax (radius, std::abs (fraction - mean));

        const auto want = 1.0f - 0.5f * (1.0f - half);
        const auto band = 3.0f * radius;
        const auto halfOpen = (float) firedOf (ladder[2]) / (float) rollCells;

        std::printf ("        half the knob on a half-chance cell: %.4f fired against %.4f asked;"
                     " %d other seeds mean %.4f, worst %.4f off it, band %.4f\n",
                     halfOpen, want, spreadSeeds, mean, radius, band);

        check (std::abs (halfOpen - want) <= band,
               "half the knob on a cell that wants three quarters fires about three quarters",
               juce::String (halfOpen, 4) + " fired against " + juce::String (want, 4)
                   + " asked, band +" + juce::String (band, 4) + " measured over "
                   + juce::String (spreadSeeds) + " other seeds");

        GrooveboxProcessor always;
        armGrid (always, blockSize);
        always.setSeed (7);
        setEveryChance (always, 1.0f);
        always.setMutate (1.0f);

        check (firedOf (decisions (always, false)) == rollCells,
               "a cell that says every time is never dropped, even with the knob wide open");

        GrooveboxProcessor never;
        armGrid (never, blockSize);
        never.setSeed (7);
        setEveryChance (never, 0.0f);
        never.setMutate (1.0f);

        check (firedOf (decisions (never, false)) == 0,
               "and a cell that says never is never kept");

        //======================================================================
        // The seam, tied to the samples.
        {
            GrooveboxProcessor base;
            armGrid (base, blockSize);
            setEveryChance (base, half);
            base.setMutate (0.0f);
            base.setSeed (7);

            GrooveboxProcessor open1;
            armGrid (open1, blockSize);
            setEveryChance (open1, half);
            open1.setMutate (1.0f);
            open1.setSeed (7);

            GrooveboxProcessor open1Again;
            armGrid (open1Again, blockSize);
            setEveryChance (open1Again, half);
            open1Again.setMutate (1.0f);
            open1Again.setSeed (7);

            GrooveboxProcessor open2;
            armGrid (open2, blockSize);
            setEveryChance (open2, half);
            open2.setMutate (1.0f);
            open2.setSeed (8);

            GrooveboxProcessor certain;
            armGrid (certain, blockSize);
            setEveryChance (certain, 1.0f);
            certain.setMutate (1.0f);
            certain.setSeed (7);

            GrooveboxProcessor barred;
            armGrid (barred, blockSize);
            setEveryChance (barred, 0.0f);
            barred.setMutate (1.0f);
            barred.setSeed (7);

            const auto baseRender = takeRoll (base);
            const auto baseStamp  = fingerprintOf (baseRender);
            const auto openStamp  = fingerprintOf (takeRoll (open1));
            const auto againStamp = fingerprintOf (takeRoll (open1Again));
            const auto twoStamp  = fingerprintOf (takeRoll (open2));
            const auto sureStamp = fingerprintOf (takeRoll (certain));
            const auto noneRender = takeRoll (barred);

            std::printf ("        two bars: shut %s, seed 7 %s, seed 7 again %s, seed 8 %s, "
                         "certain %s, barred peak %s\n",
                         hexOf (baseStamp).toRawUTF8(), hexOf (openStamp).toRawUTF8(),
                         hexOf (againStamp).toRawUTF8(), hexOf (twoStamp).toRawUTF8(),
                         hexOf (sureStamp).toRawUTF8(),
                         juce::String (peakOf (noneRender.left), 6).toRawUTF8());

            // The precondition every claim under it rests on: two bars of this kit with
            // nothing dropped is audible. A silent arming would let the roll look like
            // it reached the samples because everything else vanished.
            check (peakOf (baseRender.left) > 0.1f && peakOf (baseRender.right) > 0.1f,
                   "the shut machine is audible over two bars, so there is something to drop",
                   "peak " + juce::String (peakOf (baseRender.left), 5) + " / "
                       + juce::String (peakOf (baseRender.right), 5));

            check (openStamp != baseStamp,
                   "the roll reaches the samples, not only the seam",
                   "seed 7 at full variation fingerprinted back as the shut machine: "
                       + hexOf (openStamp));

            check (againStamp == openStamp,
                   "and the same seed renders the same take twice, in the bits",
                   hexOf (againStamp) + " against " + hexOf (openStamp));

            check (twoStamp != openStamp,
                   "a new seed is a new two bars, not the same one again",
                   hexOf (twoStamp));

            check (sureStamp == baseStamp,
                   "with every cell certain the knob is invisible in the output",
                   hexOf (sureStamp) + " against " + hexOf (baseStamp)
                       + " -- the gate the plan asked for, at two bars");

            check (peakOf (noneRender.left) == 0.0f && peakOf (noneRender.right) == 0.0f,
                   "and with every cell barred the machine is silent",
                   "peak " + juce::String (peakOf (noneRender.left), 6));
        }

        //======================================================================
        // The new field travels, and it is the third byte.
        {
            GrooveboxProcessor held;
            held.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (held);

            held.setStep (2, 7, true);
            held.setStepVelocity (2, 7, 64.0f / 127.0f);
            held.setStepChance (2, 7, 100.0f / (float) Layout::chanceLevels);

            held.shiftChannelRow (2, 3);

            check (! held.isStepOn (2, 7) && held.isStepOn (2, 10)
                       && held.getStepChance (2, 10) == 100.0f / (float) Layout::chanceLevels,
                   "turning a row carries the probability with the step it belongs to",
                   "(2,7) " + juce::String (held.getStepChance (2, 7), 4)
                       + ", (2,10) " + juce::String (held.getStepChance (2, 10), 4));

            held.setStepChance (2, 10, 0.5f);

            check (held.getStepChance (2, 10)
                       == 128.0f / (float) Layout::chanceLevels,
                   "and a probability is held on the rung of 255 the record can store",
                   "half became " + juce::String (held.getStepChance (2, 10), 6)
                       + ", which is " + juce::String (held.getStepChance (2, 10)
                                                          * (float) Layout::chanceLevels, 3)
                       + " of 255");

            juce::MemoryBlock blob;
            held.getStateInformation (blob);

            GrooveboxProcessor back;
            back.prepareToPlay (sampleRate, blockSize);
            back.setStateInformation (blob.getData(), (int) blob.getSize());

            check (back.getStepChance (2, 10) == held.getStepChance (2, 10)
                       && back.getStepChance (2, 7) == held.getStepChance (2, 7),
                   "it survives a save and a load on the same rung",
                   "went in at " + juce::String (held.getStepChance (2, 10), 6)
                       + ", came back at " + juce::String (back.getStepChance (2, 10), 6));

            // The byte itself, read out of the payload the way [19]'s probe does: a
            // codec that carries the number through the accessors but writes it to the
            // wrong offset is the failure this rules out, and no accessor can see it.
            const auto pattern = held.getValueTreeState().state.getProperty ("pattern").toString();
            juce::MemoryBlock bytes;
            juce::MemoryOutputStream sink (bytes, false);

            const auto at = (size_t) (2 * Layout::numSteps + 10) * 3;
            const auto wantBytes = (size_t) Layout::numChannels * Layout::numSteps * 3;

            /**
                The sink is flushed before the block is measured, and the pointer taken
                after that — not before.

                Until the flush, `bytes` is holding the *allocation*, which a
                MemoryOutputStream rounds up as it grows: 832 bytes for a 576-byte
                payload. [19]'s probe only ever asks for a lower bound, so it never
                noticed, but a check that a field lands at the right offset has to say how
                wide the record is, and that number only exists once the writer says it is
                done. Taking `raw` before the flush would be worse still: shrinking the
                block can move it, and the bytes would then be read from freed memory.
            */
            const auto tagged = pattern.startsWith ("v3:");

            const auto flushed = tagged
                                   && juce::Base64::convertFromBase64 (
                                          sink, pattern.fromFirstOccurrenceOf (":", false, false));

            sink.flush();   // void, and only shrinks the block's size to what was written

            const auto decoded = (size_t) bytes.getSize();

            const auto* raw = static_cast<const juce::uint8*> (bytes.getData());

            /**
                A byte of the decoded payload, or -1 when there is no such byte.

                Reading `raw[at]` unguarded would index past the block on exactly the
                build this check exists to catch — a payload that decoded short — and the
                detail line would then be reporting whatever was in the allocator's tail
                as if it were the record. Same reason the size is in the message: the
                first version of this check printed three correct bytes next to a failure
                and said nothing about which term was false.
            */
            const auto byteAt = [&] (size_t i) -> int
            {
                return flushed && i < decoded ? (int) raw[i] : -1;
            };

            check (flushed && decoded == wantBytes
                       && byteAt (at) == 1 && byteAt (at + 1) == 64 && byteAt (at + 2) == 128,
                   "the third byte of a step is its chance, and the first two are still itself",
                   (flushed ? juce::String ("v3 tag, ") : juce::String ("not a decodable v3 tag, "))
                       + juce::String ((int) decoded) + " of "
                       + juce::String ((int) wantBytes) + " bytes decoded, and at byte "
                       + juce::String ((int) at) + ": flag " + juce::String (byteAt (at))
                       + ", level " + juce::String (byteAt (at + 1))
                       + ", chance " + juce::String (byteAt (at + 2)) + " of 255");

            // An undo has to reach the third field too, and the record with it.
            GrooveboxProcessor undoer;
            undoer.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (undoer);
            undoer.setStep (0, 0, true);
            undoer.setStepChance (0, 0, 1.0f);

            undoer.beginTransaction ("open the chance");
            undoer.setStepChance (0, 0, 0.25f);

            check (undoer.getStepChance (0, 0) == 64.0f / (float) Layout::chanceLevels,
                   "the chance edit lands");

            undoer.getUndoManager().undo();

            check (undoer.getStepChance (0, 0) == 1.0f,
                   "and undo takes the probability back with the step, not only the step",
                   "back to " + juce::String (undoer.getStepChance (0, 0), 4));
        }

        //======================================================================
        // Reroll: a new number, the same grid.
        {
            /**
                A reroll asked of a grid where every cell is actually rolled.

                The first version of this armed one step and gave the other 191 cells
                their default chance of 1.0, so 4095 of the 4096 answers did not consult
                the seed at all and the only cell that did had a coin-flip's chance of
                landing on the same side twice. The check was really `will I get lucky`,
                which is not a gate. Here every cell of the four lanes is at half, so a
                new seed has to move about half the take, and how many cells moved is the
                number the line reports.
            */
            GrooveboxProcessor panel;
            armGrid (panel, blockSize);
            setEveryChance (panel, half);
            panel.setMutate (1.0f);
            panel.setSeed (7);

            const auto before = decisions (panel, false);
            const auto oldSeed = panel.getSeed();
            const auto newSeed = panel.rerollSeed();
            const auto after = decisions (panel, false);

            auto litBefore = 0;
            auto litAfter = 0;

            for (int channel = 0; channel < Layout::numChannels; ++channel)
                for (int step = 0; step < Layout::numSteps; ++step)
                {
                    litBefore += panel.isStepOn (channel, step) ? 1 : 0;
                    litAfter += panel.isStepOn (channel, step) ? 1 : 0;
                }

            auto moved = 0;

            for (size_t i = 0; i < (size_t) rollCells; ++i)
                if (before[i] != after[i])
                    ++moved;

            std::printf ("        reroll: seed %d -> %d, grid %d cells both ways, %d of %d "
                         "answers move\n",
                         oldSeed, newSeed, litAfter, moved, rollCells);

            check (newSeed != oldSeed
                       && newSeed >= Params::seedMin && newSeed <= Params::seedMax,
                   "a reroll hands back a different seed inside the range",
                   juce::String (oldSeed) + " -> " + juce::String (newSeed));

            // armGrid paints the first four lanes, so a cell outside them is off by
            // construction — the first version of this asserted (5,4), which the old
            // hand-armed one-step grid had lit and this one does not.
            check (litBefore == litAfter && litBefore == rollLanes * Layout::numSteps
                       && panel.isStepOn (3, 5) && panel.getStepChance (3, 5) == half
                       && ! panel.isStepOn (5, 4),
                   "and does not touch the grid it rerolled",
                   juce::String (litBefore) + " cells before, " + juce::String (litAfter)
                       + " after, " + juce::String (rollLanes * Layout::numSteps)
                       + " armed, (3,5) at " + juce::String (panel.getStepChance (3, 5), 4));

            // Half the take moving is the same avalanche the seed check above asks for,
            // from the other end: this one does not get to choose the new number.
            check (newSeed == oldSeed
                       || (moved > rollCells / 5 && moved < rollCells * 4 / 5),
                   "the take moves with it",
                   juce::String (moved) + " of " + juce::String (rollCells)
                       + " cells changed hands, " + juce::String (firedOf (before))
                       + " fired before and " + juce::String (firedOf (after)) + " after");
        }

        clickWav.deleteFile();
    }

    //======================================================================
    std::printf ("\n%s  %d checks, %d failures\n",
                 failures == 0 ? "PASSED" : "FAILED", checks, failures);

    return failures == 0 ? 0 : 1;
}
