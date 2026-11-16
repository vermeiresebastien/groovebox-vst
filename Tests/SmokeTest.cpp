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
        //
        // "Meanwhile" has to be paid for. Two hundred loads can finish before the
        // scheduler has even started the render thread, and then this section races
        // nothing at all while still printing three greens — it was seen to do exactly
        // that, 0 blocks beside a pass, under a build that had just finished linking.
        // So wait for the first block before the hammering starts, bounded so a thread
        // that never runs is a failure this section reports rather than one it hangs on.
        for (auto waited = 0; blocksRendered.load() == 0 && waited < 200000; ++waited)
            std::this_thread::yield();

        check (blocksRendered.load() > 0,
               "the render thread was actually running before the loads began, which is what"
               " makes the overlap below an overlap rather than a hope",
               juce::String (blocksRendered.load()) + " blocks after 200000 yields");

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
        struct StoredStep { bool on = false; int level = -1; int chance = 255; int strikes = 1; };

        auto storedStep = [&processor] (int channel, int step) -> StoredStep
        {
            const auto pattern = processor.getValueTreeState().state
                                     .getProperty ("pattern").toString();

            const auto stride = pattern.startsWith ("v4:") ? 4
                              : pattern.startsWith ("v3:") ? 3
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
                     stride > 2 ? (int) raw[at + 2] : 255,
                     stride > 3 ? (int) raw[at + 3] : 1 };
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
        check ((int) state.getProperty ("stateVersion") == 4,
               "and the version it names is the one this build writes",
               "got " + juce::String ((int) state.getProperty ("stateVersion")));

        check (payload.startsWith ("v4:"),
               "the payload names its own codec, so a length cannot be mistaken for a version",
               "starts with " + payload.substring (0, 4));

        // v4 is four bytes a step: the flag, the 7-bit level, the chance the step gets as
        // 255ths, and the number of strikes that hit is spread into. Base64 of 192 steps is
        // 4 characters per 3 bytes, rounded up.
        // Naming the width here is the point - a payload that quietly changes size
        // changes what every session means. This is the line that moves when it does.
        constexpr int stepBytes = 4;
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

                // Two generations down, and the same claim about the field this build
                // added: a v2 step never mentioned a strike count either, and what it
                // means is one hit on the onset. Counted separately from the probability
                // above because a reader that defaulted one and not the other is exactly
                // the mistake, and one counter could not tell them apart.
                auto neverRatcheted = 0;

                for (int channel = 0; channel < Layout::numChannels; ++channel)
                    for (int step = 0; step < Layout::numSteps; ++step)
                        if (fromV2.getStepRatchet (channel, step) != 1)
                            ++neverRatcheted;

                check (neverRatcheted == 0,
                       "and every step of a v2 session fires once, on the onset",
                       juce::String (neverRatcheted)
                           + " step(s) came in with a strike count that is not 1");
            }

            // v3: three bytes a step, the generation immediately behind this one. Every
            // session saved since 4.2 is this shape, so it is not hypothetical, and what it
            // lacks is again a *width* rather than a value. The chance has to arrive as it
            // was written -- if the reader shifted by one byte it would read the chance as
            // the count and the count as whatever followed -- and the count has to default.
            {
                std::array<juce::uint8, (size_t) Layout::numChannels * Layout::numSteps * 3> v3bytes {};

                auto putV3 = [&v3bytes] (int channel, int step, int on, int level, int chance)
                {
                    const auto at = (size_t) (channel * Layout::numSteps + step) * 3;
                    v3bytes[at] = (juce::uint8) on;
                    v3bytes[at + 1] = (juce::uint8) level;
                    v3bytes[at + 2] = (juce::uint8) chance;
                };

                putV3 (0, 4, 1, 100, 128);
                putV3 (3, 15, 1, 64, 255);
                putV3 (11, 0, 0, 33, 200);   // switched off, but still carrying three numbers

                GrooveboxProcessor fromV3;
                fromV3.prepareToPlay (sampleRate, blockSize);
                clearAllSteps (fromV3);

                loadSession (fromV3, "v3:" + juce::Base64::toBase64 (v3bytes.data(), v3bytes.size()), 3);

                check (fromV3.isStepOn (0, 4) && fromV3.isStepOn (3, 15) && ! fromV3.isStepOn (11, 0),
                       "a v3 session still loads, flag and all",
                       "got " + juce::String ((int) fromV3.isStepOn (0, 4))
                           + juce::String ((int) fromV3.isStepOn (3, 15))
                           + juce::String ((int) fromV3.isStepOn (11, 0)));

                const auto chanceOffBy = [&fromV3] (int channel, int step, int want)
                {
                    return std::abs (fromV3.getStepChance (channel, step)
                                      - (float) want / 255.0f) * 255.0f;
                };

                check (chanceOffBy (0, 4, 128) < 1.0e-3f && chanceOffBy (11, 0, 200) < 1.0e-3f,
                       "at the probability its own third byte named, off step 11 included",
                       "off by " + juce::String (chanceOffBy (0, 4, 128), 4) + " and "
                           + juce::String (chanceOffBy (11, 0, 200), 4));

                auto spread = 0;

                for (int channel = 0; channel < Layout::numChannels; ++channel)
                    for (int step = 0; step < Layout::numSteps; ++step)
                        if (fromV3.getStepRatchet (channel, step) != 1)
                            ++spread;

                check (spread == 0,
                       "and every step of it fires once, because that is what a byte it never wrote means",
                       juce::String (spread) + " step(s) came in on more than one strike");
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

            check (payloadOf (future).startsWith ("v4:"),
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
        // What a row turn does to the fields, and what each of them is in the record:
        // the probability is the third byte, the strike count the fourth.
        {
            GrooveboxProcessor held;
            held.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (held);

            held.setStep (2, 7, true);
            held.setStepVelocity (2, 7, 64.0f / 127.0f);
            held.setStepChance (2, 7, 100.0f / (float) Layout::chanceLevels);
            held.setStepRatchet (2, 7, 5);

            held.shiftChannelRow (2, 3);

            check (! held.isStepOn (2, 7) && held.isStepOn (2, 10)
                       && held.getStepChance (2, 10) == 100.0f / (float) Layout::chanceLevels,
                   "turning a row carries the probability with the step it belongs to",
                   "(2,7) " + juce::String (held.getStepChance (2, 7), 4)
                       + ", (2,10) " + juce::String (held.getStepChance (2, 10), 4));

            // The fourth field, set on the same cell *before* the row turns. Everything
            // below writes the roll at (2,10) itself, so a build whose shift moves three
            // of the four stores would leave this block green apart from this line — the
            // enumeration in `shiftChannelRow` is four loads and four stores, and only a
            // field that was already elsewhere when the row moved can show one missing.
            check (held.getStepRatchet (2, 10) == 5 && held.getStepRatchet (2, 7) == 1,
                   "and the strike count travels with the step rather than staying behind",
                   "(2,7) " + juce::String (held.getStepRatchet (2, 7))
                       + ", (2,10) " + juce::String (held.getStepRatchet (2, 10))
                       + " strikes");

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
            //
            // Set on the same cell the other three bytes are being read from, and to a
            // number that is not the default: the array the writer fills is zero-
            // initialised, so a build that forgot the fourth byte would report 0 here
            // rather than agree with an expectation of 1.
            //
            // Written again although the turn above already put five strikes on this
            // cell: an offset probe that depended on the shift would give two gates one
            // failure to report, and a broken shift is not a broken writer.
            held.setStepRatchet (2, 10, 5);

            const auto pattern = held.getValueTreeState().state.getProperty ("pattern").toString();
            juce::MemoryBlock bytes;
            juce::MemoryOutputStream sink (bytes, false);

            const auto at = (size_t) (2 * Layout::numSteps + 10) * 4;
            const auto wantBytes = (size_t) Layout::numChannels * Layout::numSteps * 4;

            /**
                The sink is flushed before the block is measured, and the pointer taken
                after that — not before.

                Until the flush, `bytes` is holding the *allocation*, which a
                MemoryOutputStream rounds up as it grows -- 832 bytes were allocated for
                v3's 576-byte record -- and the number moves with the format. [19]'s probe
                only ever asks for a lower bound, so it never
                noticed, but a check that a field lands at the right offset has to say how
                wide the record is, and that number only exists once the writer says it is
                done. Taking `raw` before the flush would be worse still: shrinking the
                block can move it, and the bytes would then be read from freed memory.
            */
            const auto tagged = pattern.startsWith ("v4:");

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
                       && byteAt (at) == 1 && byteAt (at + 1) == 64 && byteAt (at + 2) == 128
                       && byteAt (at + 3) == 5,
                   "the four bytes of a step are itself, its level, its chance and its strikes",
                   (flushed ? juce::String ("v4 tag, ") : juce::String ("not a decodable v4 tag, "))
                       + juce::String ((int) decoded) + " of "
                       + juce::String ((int) wantBytes) + " bytes decoded, and at byte "
                       + juce::String ((int) at) + ": flag " + juce::String (byteAt (at))
                       + ", level " + juce::String (byteAt (at + 1))
                       + ", chance " + juce::String (byteAt (at + 2)) + " of 255"
                       + ", strikes " + juce::String (byteAt (at + 3)) + " of 8");

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
        // A row turn is one slot's business, and the fourth field is what proves it.
        {
            GrooveboxProcessor banked;
            banked.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (banked);

            // Slot 1 holds step 7 with three strikes and a level that is not the default;
            // slot 0, which is where the lane points, holds the same step with five. The
            // grid only ever shows slot 0, so a shift that walked the whole bank would
            // move a take nobody asked it to move and nothing on screen would say so —
            // and with only the on/off and the level to look at, that is exactly what a
            // build reading `slots[*]` instead of the selected one would get away with.
            banked.setChannelSlot (4, 1);
            banked.setStep (4, 7, true);
            banked.setStepRatchet (4, 7, 3);
            banked.setStepVelocity (4, 7, 50.0f / 127.0f);

            banked.setChannelSlot (4, 0);
            banked.setStep (4, 7, true);
            banked.setStepRatchet (4, 7, 5);

            banked.shiftChannelRow (4, 1);

            check (banked.isStepOn (4, 8) && banked.getStepRatchet (4, 8) == 5
                       && ! banked.isStepOn (4, 7),
                   "the slot the lane points at turns, strikes and all",
                   "(4,8) on " + juce::String ((int) banked.isStepOn (4, 8))
                       + " at " + juce::String (banked.getStepRatchet (4, 8))
                       + " strikes, (4,7) on " + juce::String ((int) banked.isStepOn (4, 7)));

            banked.setChannelSlot (4, 1);

            check (banked.isStepOn (4, 7) && banked.getStepRatchet (4, 7) == 3
                       && ! banked.isStepOn (4, 8)
                       && std::abs (banked.getStepVelocity (4, 7) - 50.0f / 127.0f) < 1.0e-6f,
                   "and the slot it is not editing stays exactly where it was written",
                   "slot 1 now reads (4,7) on " + juce::String ((int) banked.isStepOn (4, 7))
                       + " at " + juce::String (banked.getStepRatchet (4, 7))
                       + " strikes, level " + juce::String (banked.getStepVelocity (4, 7), 4)
                       + ", (4,8) on " + juce::String ((int) banked.isStepOn (4, 8)));
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
    /**
        The timing half of variation, measured the way the probability half was.

        Three things have to be true and none of them follows from the last section:
        the draw must be a take (the same key, the same lateness, in a bounce and in a
        live pass), it must be bounded by something other than taste (a hit may never
        pass the one after it, which turns the knob from a feel control into a pattern
        editor), and it must reach the samples — because every arithmetic check above
        could pass next to an engine that never reads the number it computes. The seam
        is the engine's own `humanizeLateInSteps`, the same call `fireStep` makes, so a
        green here is about the machine and not about a model of it in this file.

        The audio half is where the design shows. The offset lives on the voice, not in
        the clock, so it has to survive a block boundary, a region boundary and a
        tempo — and the measurement is therefore repeated at three block sizes, the
        lesson from the pitch drop, whose defect only appeared at a 1024 block and
        nothing in the suite could see it.
    */
    std::printf ("\n[30] a late hit is late on purpose\n");
    {
        constexpr int lateBars  = 64;
        constexpr int lateLanes = 4;
        constexpr int lateCells = lateBars * Layout::numSteps * lateLanes;

        const auto lateClick = makeClickWav ("late-click", 1200, sampleRate);

        /**
            Four lanes painted right through, so every cell of the take is asked.

            The same shape as [29]: the knobs go in through the parameters a host would
            use, and the tempo is 120 so `exactStepSamples` is the ruler the offsets are
            measured against.

            The globals go in *before* `prepareToPlay`, and that order is load-bearing.
            The master trim moves through a 20 ms one-pole so a drag cannot zipper, and
            the smoother is set to whatever the target is when the sample rate is set
            (`MasterBus::setSampleRate`) — change the trim after that and it ramps for
            roughly thirteen time constants, about a third of a second, snapping to the
            target only inside 1e-6. This section's whole claim is that a late hit is the
            *same samples*, which is only a fair claim when both hits are measured at one
            trim. With the trim moved after the prepare, the gate went red by 0.0004 on a
            head sample 482 output samples apart: the wait had not changed the level, it
            had changed *when the level was reached*, and the smoother was the thing
            moving. At rest, shut and open come out bit-identical.
        */
        const auto arm = [&] (GrooveboxProcessor& p, int forBlockSize)
        {
            p.setManualTempo (120.0);
            p.setSwingGrid (4);
            p.setSwing (0.0f);
            p.setMasterGainDb (-3.5f);
            p.setSeed (1234);
            p.prepareToPlay (sampleRate, forBlockSize);

            juce::String error;
            for (int channel = 0; channel < lateLanes; ++channel)
                p.loadSampleIntoChannel (channel, lateClick, error);

            clearAllSteps (p);

            for (int channel = 0; channel < lateLanes; ++channel)
                for (int step = 0; step < Layout::numSteps; ++step)
                {
                    p.setStep (channel, step, true);
                    p.setStepVelocity (channel, step, 1.0f);
                    p.setStepChance (channel, step, 128.0f / (float) Layout::chanceLevels);
                }
        };

        /**
            Every key's lateness, in a fixed slot per cell.

            Indexed rather than appended, and walkable in either direction, for the
            reason [29] records: an answer that depends on the answers before it lands
            in different slots when the grid is walked backwards, and a `push_back`
            would hide that behind a difference in order.
        */
        const auto lateness = [&] (GrooveboxProcessor& p, bool backwards)
        {
            std::vector<double> cells ((size_t) lateCells, -1.0);

            for (int i = 0; i < lateBars; ++i)
                for (int j = 0; j < Layout::numSteps; ++j)
                    for (int k = 0; k < lateLanes; ++k)
                    {
                        const auto bar  = backwards ? lateBars - 1 - i : i;
                        const auto step = backwards ? Layout::numSteps - 1 - j : j;
                        const auto lane = backwards ? lateLanes - 1 - k : k;

                        cells[(size_t) ((bar * Layout::numSteps + step) * lateLanes + lane)]
                            = p.wouldThisStepStartLate (bar, step, lane);
                    }

            return cells;
        };

        const auto hashOf = [] (const std::vector<double>& cells)
        {
            return fnv1a (0xcbf29ce484222325ull, cells.data(), cells.size() * sizeof (double));
        };

        GrooveboxProcessor probe;
        arm (probe, blockSize);

        //---- a. shut is shut, and open is bounded ---------------------------------
        {
            probe.setHumanize (0.0f);

            const auto shut = lateness (probe, false);

            auto drawn = 0;
            auto worstShut = 0.0;

            for (const auto value : shut)
            {
                if (std::abs (value) > 1.0e-12)
                {
                    ++drawn;
                    worstShut = juce::jmax (worstShut, value);
                }
            }

            check (drawn == 0,
                   "with the knob shut no hit is drawn off its onset",
                   juce::String (drawn) + " of " + juce::String (lateCells)
                       + " keys moved, worst " + juce::String (worstShut, 6) + " steps");

            probe.setHumanize (1.0f);

            const auto open = lateness (probe, false);

            auto low = 1.0e9;
            auto high = -1.0e9;
            auto sum = 0.0;
            auto nearGrid = 0;
            auto offGrid = 0;

            for (const auto value : open)
            {
                low = juce::jmin (low, value);
                high = juce::jmax (high, value);
                sum += value;

                if (value > 1.0e-12)
                    ++offGrid;

                if (value <= Layout::maxHumanizeLateSteps * 0.25)
                    ++nearGrid;
            }

            const auto mean = sum / (double) lateCells;

            std::printf ("        draw over %d keys: min %s, max %s, mean %s steps; "
                         "%d off the grid, %d within a quarter of the travel\n",
                         lateCells, juce::String (low, 5).toRawUTF8(),
                         juce::String (high, 5).toRawUTF8(), juce::String (mean, 5).toRawUTF8(),
                         offGrid, nearGrid);

            check (low >= 0.0 && high <= Layout::maxHumanizeLateSteps + 1.0e-12,
                   "every hit is late by at least nothing and at most the bound",
                   "min " + juce::String (low, 6) + ", max " + juce::String (high, 6)
                       + ", bound " + juce::String (Layout::maxHumanizeLateSteps, 6));

            // The skew is a claim about the distribution, so it is measured rather than
            // assumed: a uniform draw would average half the travel and put a quarter of
            // the keys in the near band. Squaring predicts a third and a half.
            check (mean > Layout::maxHumanizeLateSteps * 0.27
                       && mean < Layout::maxHumanizeLateSteps * 0.38,
                   "the average offset is a third of the travel, not half of it",
                   "mean " + juce::String (mean, 5) + " of "
                       + juce::String (Layout::maxHumanizeLateSteps, 5));

            check (nearGrid > lateCells * 4 / 10 && nearGrid < lateCells * 6 / 10,
                   "half the hits land within a quarter of the travel",
                   juce::String (nearGrid) + " of " + juce::String (lateCells));

            // The knob scales the one draw rather than drawing again: every key at half
            // has to sit at half of the same key at full.
            probe.setHumanize (0.5f);

            const auto half = lateness (probe, false);

            auto scaled = 0;

            for (size_t i = 0; i < (size_t) lateCells; ++i)
                if (std::abs (half[i] - open[i] * 0.5) > 1.0e-12)
                    ++scaled;

            check (scaled == 0,
                   "and the amount scales that draw instead of replacing it",
                   juce::String (scaled) + " keys did not halve");
        }

        //---- b. one key, one take --------------------------------------------------
        {
            probe.setHumanize (1.0f);

            const auto forward = lateness (probe, false);
            const auto backward = lateness (probe, true);
            const auto again = lateness (probe, false);

            check (hashOf (forward) == hashOf (again),
                   "the same seed asked twice gives the same take",
                   hexOf (hashOf (forward)) + " vs " + hexOf (hashOf (again)));

            check (hashOf (forward) == hashOf (backward),
                   "and the answer does not depend on the order it was asked in",
                   hexOf (hashOf (forward)) + " forwards, " + hexOf (hashOf (backward)) + " backwards");

            probe.setSeed (1235);

            const auto other = lateness (probe, false);

            auto moved = 0;

            for (size_t i = 0; i < (size_t) lateCells; ++i)
                if (std::abs (other[i] - forward[i]) > 1.0e-12)
                    ++moved;

            check (moved >= lateCells - 1,
                   "a new seed re-draws the timing of the take, not a tweak of it",
                   juce::String (moved) + " of " + juce::String (lateCells) + " keys");

            // ...and a *different* take, not a different law: the new seed has to sit
            // inside the same distribution the first one does, or the seed would be a
            // second amount knob hiding in the reroll button.
            auto otherSum = 0.0;

            for (const auto value : other)
                otherSum += value;

            const auto otherMean = otherSum / (double) lateCells;

            std::printf ("        seed 1235 draws a mean of %s steps over %d keys\n",
                         juce::String (otherMean, 5).toRawUTF8(), lateCells);

            check (otherMean > Layout::maxHumanizeLateSteps * 0.27
                       && otherMean < Layout::maxHumanizeLateSteps * 0.38,
                   "and the new take obeys the same law as the old one",
                   "mean " + juce::String (otherMean, 5) + " of "
                       + juce::String (Layout::maxHumanizeLateSteps, 5));

            probe.setSeed (1234);

            /**
                The two draws share a key and not a bit.

                This is the check that says a timing knob is a timing knob: opening it
                must leave *which hits play* exactly where it was. The cells walked here
                are the roll's own answers, taken with the timing knob at both ends of
                its travel.
            */
            probe.setMutate (1.0f);

            juce::uint64 rollHash = 0;
            auto first = true;

            for (const auto amount : { 0.0f, 1.0f })
            {
                probe.setHumanize (amount);

                std::vector<juce::uint8> fired ((size_t) lateCells, 0);

                for (int bar = 0; bar < lateBars; ++bar)
                    for (int step = 0; step < Layout::numSteps; ++step)
                        for (int lane = 0; lane < lateLanes; ++lane)
                            fired[(size_t) ((bar * Layout::numSteps + step) * lateLanes + lane)]
                                = probe.wouldThisStepHit (bar, step, lane) ? 1 : 0;

                const auto hash = fnv1a (0xcbf29ce484222325ull, fired.data(), fired.size());

                if (first)
                {
                    rollHash = hash;
                    first = false;
                }
                else
                {
                    check (hash == rollHash,
                           "opening the timing knob does not change which hits play",
                           hexOf (rollHash) + " shut, " + hexOf (hash) + " open");
                }
            }

            /**
                And the two draws do not lean on each other.

                The check above says the timing knob cannot move the roll. The other
                half of "one seed, two independent takes" is that the roll's *outcome*
                does not predict the timing's — because the two read the same 64-bit key,
                and if they read the same *bits* of it, every hit that survived the roll
                would be late by exactly as much as it was lucky, which is a groove that
                steers itself: the dropped hits and the pushed hits would line up.

                So the correlation between "did this one fire" and how late it is drawn
                has to be nothing. Measured as the point-biserial between the roll's
                answer and the unit the offset came from, over the same 4096 keys: with
                shared bits this is ~0.8, with disjoint ones it is the sampling noise of
                a finite take, and the two are not close.
            */
            std::vector<juce::uint8> fired ((size_t) lateCells, 0);

            for (int bar = 0; bar < lateBars; ++bar)
                for (int step = 0; step < Layout::numSteps; ++step)
                    for (int lane = 0; lane < lateLanes; ++lane)
                        fired[(size_t) ((bar * Layout::numSteps + step) * lateLanes + lane)]
                            = probe.wouldThisStepHit (bar, step, lane) ? 1 : 0;

            auto sumAll = 0.0, sumFired = 0.0, squareAll = 0.0;
            auto countFired = 0;

            for (int i = 0; i < lateCells; ++i)
            {
                // Undo the two mappings the draw applies, to get back at the unit the
                // key actually produced: the amount is at full, so only the travel and
                // the square stand between the key and the number reported.
                const auto unit = std::sqrt (forward[(size_t) i] / Layout::maxHumanizeLateSteps);

                sumAll += unit;
                squareAll += unit * unit;

                if (fired[(size_t) i] != 0)
                {
                    ++countFired;
                    sumFired += unit;
                }
            }

            const auto n = (double) lateCells;
            const auto meanAll = sumAll / n;
            const auto meanFired = sumFired / (double) countFired;
            const auto meanMissed = (sumAll - sumFired) / (n - (double) countFired);
            const auto sdAll = std::sqrt (squareAll / n - meanAll * meanAll);
            const auto share = (double) countFired / n;
            const auto correlation = (meanFired - meanMissed) * std::sqrt (share * (1.0 - share)) / sdAll;

            std::printf ("        %d of %d cells fired; r between firing and lateness %s\n",
                         countFired, lateCells, juce::String (correlation, 4).toRawUTF8());

            check (countFired > lateCells * 3 / 10 && countFired < lateCells * 7 / 10,
                   "the roll leaves about half the take standing, so a correlation could show",
                   juce::String (countFired) + " of " + juce::String (lateCells));

            check (std::abs (correlation) < 0.1,
                   "which hits fire and how late they are are drawn from separate bits",
                   "r = " + juce::String (correlation, 4));

            probe.setMutate (0.0f);
        }

        //---- c. the bound is arithmetic, not taste ---------------------------------
        {
            /**
                The closest two onsets the engine can put a bar's steps, in steps.

                Asked of `getStepOnsetInSteps`, which is the function that decides where
                a step is played — not of a formula in this file. Swing is what squeezes
                the gap: a straight grid is a whole step apart everywhere, and the
                heaviest shuffle puts a pair at half. So the bound has to be checked at
                the shuffle, and the straight grid is here to show what the pressure is.
            */
            const auto closestGap = [&] (int grid, float swing)
            {
                probe.setSwingGrid (grid);
                probe.setSwing (swing);

                auto worst = 1.0e9;

                for (int step = 0; step < Layout::numSteps; ++step)
                {
                    const auto next = (step + 1) % Layout::numSteps;
                    auto gap = probe.getStepOnsetInSteps (next) - probe.getStepOnsetInSteps (step);

                    if (next == 0)
                        gap += (double) Layout::numSteps;

                    worst = juce::jmin (worst, gap);
                }

                return worst;
            };

            const auto shuffle = closestGap (4, 1.0f);
            const auto sixteenthShuffle = closestGap (2, 1.0f);
            const auto straight = closestGap (4, 0.0f);

            std::printf ("        closest onsets: %s steps at 1/8 shuffle, %s at 1/16, %s straight; "
                         "the bound is %s steps\n",
                         juce::String (shuffle, 4).toRawUTF8(),
                         juce::String (sixteenthShuffle, 4).toRawUTF8(),
                         juce::String (straight, 4).toRawUTF8(),
                         juce::String (Layout::maxHumanizeLateSteps, 4).toRawUTF8());

            check (Layout::maxHumanizeLateSteps * 2.0 <= shuffle + 1.0e-12,
                   "the most a hit can be late is half the closest gap at a shuffle",
                   "bound " + juce::String (Layout::maxHumanizeLateSteps, 4)
                       + " against a gap of " + juce::String (shuffle, 4));

            check (Layout::maxHumanizeLateSteps * 2.0 <= sixteenthShuffle + 1.0e-12,
                   "and that holds on the 1/16 swing grid too",
                   "bound " + juce::String (Layout::maxHumanizeLateSteps, 4)
                       + " against a gap of " + juce::String (sixteenthShuffle, 4));

            probe.setSwing (0.0f);
            probe.setSwingGrid (4);
        }

        //---- d. the number reaches the samples -------------------------------------
        {
            /**
                One hit, rendered, and where it lands.

                A single cell on one lane, so the first sample above the threshold can
                only be that hit, and the difference between the shut render and the open
                one is the offset and nothing else. The seed is *searched* rather than
                picked: what the check needs is a draw long enough to see, and a
                hard-coded seed is a number that stops meaning anything the moment the
                generator moves.
            */
            auto pickSeed = 0;
            auto wantSamples = 0;

            probe.setHumanize (1.0f);

            for (int candidate = 0; candidate < 1000; ++candidate)
            {
                probe.setSeed (candidate);

                const auto samples = (int) std::lround (probe.wouldThisStepStartLate (0, 0, 0)
                                                        * exactStepSamples);

                if (samples >= 160 && samples <= 600)
                {
                    pickSeed = candidate;
                    wantSamples = samples;
                    break;
                }
            }

            check (wantSamples > 0,
                   "a seed exists whose first hit is drawn between 160 and 600 samples late",
                   "searched 1000, found none");

            probe.setSeed (pickSeed);

            const auto take = [&] (GrooveboxProcessor& p, int block, int total)
            {
                {
                    juce::AudioBuffer<float> settle (2, block);
                    juce::MidiBuffer silent;

                    for (int i = 0; i < 16; ++i)
                    {
                        settle.clear();
                        p.processBlock (settle, silent);
                    }
                }

                p.resetTransport();
                p.setPlaying (true);

                auto out = render (p, total, block, sampleRate);
                p.setPlaying (false);

                return out;
            };

            /** One cell on one lane, at the given amount, from the top of the bar. */
            const auto oneHit = [&] (GrooveboxProcessor& p, int block, float amount)
            {
                clearAllSteps (p);
                p.setStep (0, 0, true);
                p.setStepChance (0, 0, 1.0f);
                p.setMutate (0.0f);
                p.setSeed (pickSeed);
                p.setHumanize (amount);

                return take (p, block, 40000);
            };

            GrooveboxProcessor shifter;
            arm (shifter, blockSize);

            const auto open = oneHit (shifter, blockSize, 1.0f);

            GrooveboxProcessor still;
            arm (still, blockSize);

            const auto shut = oneHit (still, blockSize, 0.0f);

            const auto firstUp = onsetAfter (open.left, 0, 1.0e-4f);
            const auto firstOn = onsetAfter (shut.left, 0, 1.0e-4f);
            const auto shifted = firstUp - firstOn;

            std::printf ("        seed %d: drawn %d samples, onset %d against %d = %d late\n",
                         pickSeed, wantSamples, firstUp, firstOn, shifted);

            check (shifted == wantSamples,
                   "the hit arrives the number of samples the draw said",
                   "measured " + juce::String (shifted) + ", drawn " + juce::String (wantSamples));

            check (firstDifference (shut.left, open.left, 0, (int) shut.left.size()) != -1,
                   "and opening the knob actually moves the audio");

            // A wait is a shift. Anything else — a level change, a retune, a hit that
            // reads the source from a different place — shows up here, and it is the
            // check that keeps this a timing control rather than a new sound.
            auto worstDiff = 0.0;
            auto worstAt = -1;

            for (int i = 4; i + shifted < 1600 && i + shifted < (int) open.left.size(); ++i)
            {
                const auto diff = (double) std::abs (shut.left[(size_t) i]
                                                     - open.left[(size_t) (i + shifted)]);

                if (diff > worstDiff)
                {
                    worstDiff = diff;
                    worstAt = i;
                }
            }

            std::printf ("        same hit, shifted: worst sample difference %s%s\n",
                         juce::String (worstDiff, 9).toRawUTF8(),
                         worstAt < 0 ? " (no sample differs)"
                                     : (" at " + juce::String (worstAt)).toRawUTF8());

            check (worstDiff < 1.0e-6,
                   "the late hit is the same samples, only later",
                   "worst " + juce::String (worstDiff, 9) + " at sample " + juce::String (worstAt)
                       + " of " + juce::String (shifted) + " shifted");

            std::printf ("        level: peak %s on the grid, %s late\n",
                         juce::String ((double) peakOf (shut.left), 9).toRawUTF8(),
                         juce::String ((double) peakOf (open.left), 9).toRawUTF8());

            check (std::abs (peakOf (shut.left) - peakOf (open.left)) < 1.0e-6f,
                   "and it is not a different level",
                   juce::String (peakOf (shut.left), 6) + " on the grid, "
                       + juce::String (peakOf (open.left), 6) + " late");

            /**
                The same measurement at three block sizes.

                This is where the design either holds or does not: the wait is a
                countdown on the voice, so a 64-sample block has to carry it across
                several blocks and a 1024-sample block has to finish inside one, and both
                must put the hit on the same sample. A stale countdown was the pitch
                drop's bug, invisible at 256 and 570 samples of the hit at 1024.
            */
            auto sameAtEveryBlock = true;
            juce::String perBlock;

            for (const auto block : { 64, 256, 1024 })
            {
                GrooveboxProcessor walker;
                arm (walker, block);

                const auto late = oneHit (walker, block, 1.0f);
                const auto up = onsetAfter (late.left, 0, 1.0e-4f);

                perBlock += juce::String (block) + "->" + juce::String (up) + " ";

                if (up != firstUp)
                    sameAtEveryBlock = false;
            }

            check (sameAtEveryBlock,
                   "the wait survives a block boundary at 64, 256 and 1024 alike",
                   perBlock + ", expected all " + juce::String (firstUp));
        }

        //---- e. a hit that has not started has not started -------------------------
        {
            /**
                The lane stays dark through the wait, and lights on the block it ends on.

                `isChannelSounding` is what the strips and the grid's activity glow read,
                so a hit that lit its lane when it was *scheduled* rather than when it
                *sounded* would show a flash on a row that is still silent — the kind of
                thing that reads as a bug in the meter and is really a second clock. The
                wait has to be longer than a block for this to be able to bite, and that
                is asserted rather than assumed: a control that cannot go red is the
                failure mode this suite already documents twice.
            */
            constexpr int watchBlock = 64;

            GrooveboxProcessor dark;
            arm (dark, watchBlock);

            // Open before searching: the knob is what makes the draw non-zero, and a
            // seed scan done with it shut finds nothing and says so as a pass.
            dark.setHumanize (1.0f);

            auto darkSeed = 0;
            auto darkDrawn = 0;

            for (int candidate = 0; candidate < 1000; ++candidate)
            {
                dark.setSeed (candidate);

                const auto samples = (int) std::lround (dark.wouldThisStepStartLate (0, 0, 0)
                                                        * exactStepSamples);

                if (samples >= 3 * watchBlock && samples <= 8 * watchBlock)
                {
                    darkSeed = candidate;
                    darkDrawn = samples;
                    break;
                }
            }

            std::printf ("        wait under test: %d samples at a %d-sample block\n",
                         darkDrawn, watchBlock);

            check (darkDrawn >= 3 * watchBlock,
                   "the wait spans at least three blocks, so this can go red",
                   juce::String (darkDrawn) + " samples at a "
                       + juce::String (watchBlock) + "-sample block");

            /**
                Renders block by block, reporting what each block said.

                The peak is what the block *did*, the sounding flag what the engine
                *claims* — and the whole point of the check is that those two are the
                same fact, block for block.
            */
            const auto watch = [&] (GrooveboxProcessor& p, int total)
            {
                std::vector<float> peaks;
                std::vector<int> lit;

                juce::AudioBuffer<float> buffer (2, watchBlock);
                juce::MidiBuffer midi;

                p.resetTransport();
                p.setPlaying (true);

                for (int rendered = 0; rendered < total; rendered += watchBlock)
                {
                    buffer.clear();
                    p.processBlock (buffer, midi);

                    auto loudest = 0.0f;

                    for (int i = 0; i < watchBlock; ++i)
                        loudest = juce::jmax (loudest, std::abs (buffer.getSample (0, i)));

                    peaks.push_back (loudest);
                    lit.push_back (p.isChannelSounding (0) ? 1 : 0);
                }

                p.setPlaying (false);

                return std::make_pair (peaks, lit);
            };

            const auto oneCell = [&] (GrooveboxProcessor& p, float amount)
            {
                clearAllSteps (p);
                p.setStep (0, 0, true);
                p.setStepChance (0, 0, 1.0f);
                p.setMutate (0.0f);
                p.setSeed (darkSeed);
                p.setHumanize (amount);
            };

            oneCell (dark, 1.0f);

            const auto late = watch (dark, 8 * watchBlock + 4000);

            auto firstSound = (int) late.first.size();
            auto firstLit = (int) late.second.size();

            for (size_t i = 0; i < late.first.size(); ++i)
            {
                if (late.first[i] > 1.0e-4f && (int) i < firstSound)
                    firstSound = (int) i;

                if (late.second[i] != 0 && (int) i < firstLit)
                    firstLit = (int) i;
            }

            std::printf ("        %d blocks of %d: sound from block %d, lane lit from block %d\n",
                         (int) late.first.size(), watchBlock, firstSound, firstLit);

            check (firstLit == firstSound && firstSound >= 1,
                   "the lane lights in the block the hit starts sounding, not the one it was fired in",
                   "lit at block " + juce::String (firstLit) + ", sound at block "
                       + juce::String (firstSound));

            // The same probe with the knob shut, because the check above is only worth
            // something if the light still arrives with the sound when there is no wait
            // at all — otherwise "never lit during a wait" could be paid for with
            // "never lit".
            oneCell (dark, 0.0f);

            const auto onGrid = watch (dark, 4 * watchBlock + 4000);

            auto shutSound = (int) onGrid.first.size();
            auto shutLit = (int) onGrid.second.size();

            for (size_t i = 0; i < onGrid.first.size(); ++i)
            {
                if (onGrid.first[i] > 1.0e-4f && (int) i < shutSound)
                    shutSound = (int) i;

                if (onGrid.second[i] != 0 && (int) i < shutLit)
                    shutLit = (int) i;
            }

            check (shutLit == shutSound && shutSound >= 0 && shutSound < 4,
                   "with no wait the lane still lights on the block it sounds in",
                   "lit at block " + juce::String (shutLit) + ", sound at block "
                       + juce::String (shutSound));
        }

        //---- f. what a session from before this parameter opens as -----------------
        {
            /**
                Three arms of one question: what does a saved session do about a knob
                that did not exist when it was written?

                The round trip goes through `getStateInformation`/`setStateInformation`,
                not by handing `setStateInformation` bare XML — `getXmlFromBinary` only
                recognises its own eight-byte header and answers anything else with
                nullptr, which the processor reads as "no session" and acts on not at
                all. That is a green that tested nothing, and this file already documents
                catching it once.
            */
            const auto roundTrip = [] (GrooveboxProcessor& from, GrooveboxProcessor& to)
            {
                juce::MemoryBlock blob;
                from.getStateInformation (blob);
                to.setStateInformation (blob.getData(), (int) blob.getSize());
            };

            GrooveboxProcessor fresh;
            fresh.prepareToPlay (sampleRate, blockSize);

            check (fresh.getHumanize() == 0.0f,
                   "a processor built now starts with the knob shut",
                   juce::String (fresh.getHumanize(), 4));

            GrooveboxProcessor mover;
            mover.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (mover);
            mover.setStep (3, 7, true);
            mover.setSwing (0.4f);
            mover.setSeed (13579);
            mover.setHumanize (0.75f);

            GrooveboxProcessor back;
            back.prepareToPlay (sampleRate, blockSize);
            roundTrip (mover, back);

            check (std::abs (back.getHumanize() - 0.75f) < 1.0e-4f,
                   "and the amount comes back with the session",
                   juce::String (back.getHumanize(), 4));

            check (back.wouldThisStepStartLate (0, 0, 0)
                       == mover.wouldThisStepStartLate (0, 0, 0),
                   "the restored session draws the same timing as the one that saved it",
                   juce::String (back.wouldThisStepStartLate (0, 0, 0), 6)
                       + " against " + juce::String (mover.wouldThisStepStartLate (0, 0, 0), 6));

            /**
                The same session as the build before this parameter wrote it: the
                `humanize` entry cut out of the state, everything else left alone.

                Deleting the child is the honest way to age a session — the point of the
                claim is a reader that has never seen the property, and a value of 0 in
                the file is a value the old build could have written, not an absence.

                It lands in a processor with the knob *open*, because the failure worth
                catching is a restore that leaves the live value where it was: the host
                loads an old set and keeps a timing knob the session never asked for.
            */
            juce::MemoryBlock modern;
            mover.getStateInformation (modern);

            auto xml = juce::AudioProcessor::getXmlFromBinary (modern.getData(), (int) modern.getSize());
            auto stripped = 0;

            if (xml != nullptr)
                for (auto* child : xml->getChildIterator())
                    if (child->getStringAttribute ("id") == Params::humanizeId)
                    {
                        xml->removeChildElement (child, true);
                        ++stripped;
                        break;
                    }

            check (stripped == 1,
                   "a session this build wrote does name the parameter in its state",
                   juce::String (stripped) + " humanize entries found in the saved XML");

            juce::MemoryBlock aged;

            if (xml != nullptr)
                juce::AudioProcessor::copyXmlToBinary (*xml, aged);

            GrooveboxProcessor inherits;
            inherits.prepareToPlay (sampleRate, blockSize);
            inherits.setHumanize (1.0f);
            inherits.setStateInformation (aged.getData(), (int) aged.getSize());

            check (inherits.getHumanize() == 0.0f,
                   "a session that predates the parameter opens with it shut, not with whatever was live",
                   juce::String (inherits.getHumanize(), 4));

            // The rest of the same session has to survive the same restore, or "the new
            // parameter defaults" could be paid for with "the whole state was dropped".
            check (inherits.isStepOn (3, 7),
                   "and the grid of that session still comes through",
                   "step 3:7 was lost restoring a session with no humanize entry");

            check (std::abs (inherits.getSwing() - 0.4f) < 1.0e-4f
                       && inherits.getSeed() == 13579,
                   "so do the neighbouring globals, which the old build did write",
                   "swing " + juce::String (inherits.getSwing(), 4)
                       + ", seed " + juce::String (inherits.getSeed()));
        }

        lateClick.deleteFile();
    }

    //======================================================================
    /**
        The level half of variation, measured the way the other two were.

        Three things have to be true and none of them follows from the sections above.
        The departure has to be bounded by arithmetic and centred on nothing: a level
        that can only go down is a trim, and a level that wanders is a re-roll of the
        dynamics the user painted. The take has to survive a save, because the record
        stores a velocity as one of 127 and a law applied to the float before that
        rounding answers differently on the two sides of a write. And the draw has to
        reach the samples as the level it said and nothing else, because every
        proportional gate above could sit next to an engine that never applies it.

        Velocity in this machine is two laws — gain and tone — so a hit struck
        differently is also a hit coloured differently. That is the mechanism working as
        designed, and it is why the audio checks below do not ask for the same samples at
        a different level. They ask for the samples the engine itself says it struck,
        which is the stronger claim and the one a hidden renormalisation cannot pass.
    */
    std::printf ("\n[31] a hit is struck a little differently\n");
    {
        // `static` so the capture-less helpers below can read them: MSVC calls a use of a
        // block-scope constexpr in a `[]` lambda odr-use and answers C3493, and static
        // storage is the honest way to say these are fixtures, not locals.
        static constexpr int touchBars  = 64;
        static constexpr int touchLanes = 4;
        static constexpr int touchCells = touchBars * Layout::numSteps * touchLanes;
        static constexpr int ladder     = touchLanes * Layout::numSteps;

        const auto touchClick = makeClickWav ("touch-click", 1200, sampleRate);

        const auto indexOf = [] (int bar, int step, int lane)
        {
            return (size_t) ((bar * Layout::numSteps + step) * touchLanes + lane);
        };

        /**
            The rung each cell of the grid was drawn at.

            Sixty-four rungs from nothing to full, one per cell, so the take is asked
            about the whole range at once: the bottom of it is where "a proportion, not
            an offset" is visible, the top is where the clamp is, and the middle is where
            the law has enough outcomes to be measured rather than counted.
        */
        const auto rungOf = [] (int lane, int step)
        {
            return (lane * Layout::numSteps + step) * 127 / (ladder - 1);
        };

        /**
            How far a rung is allowed to move, in rungs.

            The engine multiplies the rung by a draw that cannot exceed one in magnitude
            and rounds the product, so this is the largest magnitude its own clamp can
            reach — and twice this plus one is how many outcomes that rung admits, which
            is what every "did a new seed really move it" limit below is derived from
            rather than guessed at.
        */
        const auto maxDelta = [] (int rung)
        {
            return (int) std::floor ((double) rung * Layout::maxTouchRatio + 0.5);
        };

        /**
            Four lanes painted through, one rung per cell, at one trim.

            The globals go in before `prepareToPlay` for the reason [30] records: the
            master trim moves through a 20 ms one-pole that only converges at the sample
            rate, so a trim changed after the prepare would have the smoother still
            travelling while the level was being measured.
        */
        const auto arm = [&] (GrooveboxProcessor& p, int forBlockSize)
        {
            p.setManualTempo (120.0);
            p.setSwingGrid (4);
            p.setSwing (0.0f);
            p.setMasterGainDb (-3.5f);
            p.setSeed (1234);
            p.prepareToPlay (sampleRate, forBlockSize);

            juce::String error;

            for (int lane = 0; lane < touchLanes; ++lane)
                p.loadSampleIntoChannel (lane, touchClick, error);

            clearAllSteps (p);

            for (int lane = 0; lane < touchLanes; ++lane)
                for (int step = 0; step < Layout::numSteps; ++step)
                {
                    p.setStep (lane, step, true);
                    p.setStepVelocity (lane, step, (float) rungOf (lane, step) / 127.0f);

                    // Half, and `mutate` open in the checks that need a roll to have an
                    // answer to correlate against; shut everywhere else, where the grid
                    // is law and only the level moves.
                    p.setStepChance (lane, step, 128.0f / (float) Layout::chanceLevels);
                }

            /**
                One cell deliberately off the lattice.

                0.5 is 63.5 of 127, so the float in memory and the rung the record writes
                are two different numbers that round to the same place. The two claims
                that need that are the shut knob handing back the first untouched, and the
                open one handing back something a save cannot change — and without a cell
                like this both of them are true of every cell for free, which is a green
                that measured nothing.
            */
            p.setStepVelocity (2, 0, 0.5f);
        };

        /** Every key's struck level, in rungs, in a fixed slot per cell. */
        const auto struck = [&] (GrooveboxProcessor& p, bool backwards)
        {
            std::vector<double> cells ((size_t) touchCells, -1.0);

            for (int i = 0; i < touchBars; ++i)
                for (int j = 0; j < Layout::numSteps; ++j)
                    for (int k = 0; k < touchLanes; ++k)
                    {
                        const auto bar  = backwards ? touchBars - 1 - i : i;
                        const auto step = backwards ? Layout::numSteps - 1 - j : j;
                        const auto lane = backwards ? touchLanes - 1 - k : k;

                        cells[indexOf (bar, step, lane)]
                            = (double) p.wouldThisStepBeStruckAt (bar, step, lane) * 127.0;
                    }

            return cells;
        };

        const auto hashOf = [] (const std::vector<double>& cells)
        {
            return fnv1a (0xcbf29ce484222325ull, cells.data(), cells.size() * sizeof (double));
        };

        /**
            The signed proportional departure of one key, or null when the key cannot
            answer: a rung of nothing has no proportion, and the full rung is held by the
            clamp, so its departure is the clamp's answer rather than the draw's.
        */
        const auto departure = [] (double rung, double got) -> double
        {
            if (rung <= 0.0 || rung >= 127.0)
                return std::numeric_limits<double>::quiet_NaN ();

            return ((double) std::llround (got) - rung) / rung;
        };

        std::vector<int> rungAt ((size_t) touchCells, 0);

        for (int bar = 0; bar < touchBars; ++bar)
            for (int step = 0; step < Layout::numSteps; ++step)
                for (int lane = 0; lane < touchLanes; ++lane)
                    rungAt[indexOf (bar, step, lane)] = rungOf (lane, step);

        // The rungs with enough travel for the law to be *measured* rather than counted:
        // below this, a rung admits so few outcomes that the rounding, not the draw, is
        // what decides the answer.
        auto fineCells = 0;

        for (const auto rung : rungAt)
            if (rung >= 64 && rung < 127)
                ++fineCells;

        GrooveboxProcessor probe;
        arm (probe, blockSize);

        //---- a. shut hands the number back; open is bounded, centred, on the lattice --
        {
            probe.setTouch (0.0f);

            auto moved = 0;

            for (int bar = 0; bar < touchBars; ++bar)
                for (int step = 0; step < Layout::numSteps; ++step)
                    for (int lane = 0; lane < touchLanes; ++lane)
                        if (probe.wouldThisStepBeStruckAt (bar, step, lane)
                                != probe.getStepVelocity (lane, step))
                            ++moved;

            check (moved == 0,
                   "shut, every hit is struck at exactly the float its step holds",
                   juce::String (moved) + " of " + juce::String (touchCells) + " keys moved");

            // The precondition the two claims above and the save test below rest on:
            // the fixture cell really is off the record's lattice while still rounding
            // onto the rung the ladder gave it.
            check (std::abs (0.5 * 127.0 - std::round (0.5 * 127.0)) > 0.1
                       && rungAt[indexOf (0, 0, 2)] == (int) std::lround (0.5 * 127.0),
                   "the fixture cell is off the lattice and still on rung 64",
                   "63.5 of 127, ladder rung " + juce::String (rungAt[indexOf (0, 0, 2)]));

            probe.setTouch (1.0f);

            const auto open = struck (probe, false);

            auto offLattice = 0, pastBound = 0, pastFull = 0, restMoved = 0;
            auto fullHeld = 0, fullStruckLighter = 0, freeCells = 0;
            auto sumFree = 0.0, sumAbsFine = 0.0;
            auto nearFine = 0;

            for (size_t i = 0; i < (size_t) touchCells; ++i)
            {
                const auto rung = (double) rungAt[i];
                const auto steps = (double) std::llround (open[i]);

                if (std::abs (open[i] - steps) > 1.0e-5)
                    ++offLattice;

                if (std::abs (steps - rung) > (double) maxDelta ((int) rung) + 1.0e-9)
                    ++pastBound;

                if (steps > 127.0)
                    ++pastFull;

                if (rung == 0.0 && steps != 0.0)
                    ++restMoved;

                if (rung == 127.0)
                {
                    if (steps == 127.0)
                        ++fullHeld;
                    else
                        ++fullStruckLighter;
                }

                const auto away = departure (rung, open[i]);

                if (! std::isnan (away))
                {
                    ++freeCells;
                    sumFree += away;
                }

                if (rung >= 64.0 && rung < 127.0)
                {
                    sumAbsFine += std::abs (away);

                    if (std::abs (away) <= Layout::maxTouchRatio * 0.25)
                        ++nearFine;
                }
            }

            const auto meanSigned = sumFree / (double) freeCells;
            const auto meanAbs = sumAbsFine / (double) fineCells;

            std::printf ("        %d keys: mean signed departure %s, mean absolute %s of travel %s;\n"
                         "        %d of %d fine rungs within a quarter of the travel; full rung: %d held,"
                         " %d struck lighter\n",
                         touchCells, juce::String (meanSigned, 5).toRawUTF8(),
                         juce::String (meanAbs, 5).toRawUTF8(),
                         juce::String (Layout::maxTouchRatio, 5).toRawUTF8(),
                         nearFine, fineCells, fullHeld, fullStruckLighter);

            check (offLattice == 0,
                   "open, what the law hands out is on the lattice the record stores",
                   juce::String (offLattice) + " of " + juce::String (touchCells)
                       + " departures were off it");

            check (pastBound == 0,
                   "and no hit departs by more than the travel of its own rung",
                   juce::String (pastBound) + " past "
                       + juce::String (Layout::maxTouchRatio, 3) + " of themselves");

            // The clamp, not the bound: a rung of 127 admits departures up to 32 by the
            // arithmetic above, so `pastBound` alone would let a law that overshoots the
            // top of the range through. This is the check that says opening Touch cannot
            // make the loudest thing in the set louder.
            check (pastFull == 0,
                   "and the full rung is never pushed past full",
                   juce::String (pastFull) + " keys above 127");

            check (restMoved == 0,
                   "a step drawn at nothing is still drawn at nothing",
                   juce::String (restMoved) + " rests given a level");

            // Both halves of the clamp have to be present, or "never past full" could be
            // paid for with "nothing ever reaches the top".
            check (fullHeld > 0 && fullStruckLighter > 0,
                   "the full rung shows the clamp biting and not only existing",
                   juce::String (fullHeld) + " held at 127, "
                       + juce::String (fullStruckLighter) + " taken below it");

            // A uniform draw about zero would average half the travel in absolute terms;
            // squaring the centred draw puts the average at a third, which is the whole
            // claim that most hits are struck nearly as drawn. Measured on the fine rungs
            // only, because a coarse one rounds to nothing and would flatter the law.
            check (meanAbs > Layout::maxTouchRatio * 0.27
                       && meanAbs < Layout::maxTouchRatio * 0.38,
                   "the average departure is a third of the travel, not half of it",
                   "mean " + juce::String (meanAbs, 5) + " of "
                       + juce::String (Layout::maxTouchRatio, 5));

            check (nearFine > fineCells * 4 / 10 && nearFine < fineCells * 6 / 10,
                   "half the fine rungs move by less than a quarter of the travel",
                   juce::String (nearFine) + " of " + juce::String (fineCells));

            /**
                Centred, and gated against its neighbours rather than against a saying.

                The mean of the distribution is exactly nothing, but a take is 4096 keys,
                not an integral, so the honest limit is how far the *other* seeds get from
                zero. Seed 1234 is deliberately left out of that sample: a tolerance
                estimated from the thing it is meant to judge would always be big enough.
            */
            auto worstNeighbour = 0.0;

            for (int seed = 1; seed <= 48; ++seed)
            {
                if (seed == 1234)
                    continue;

                probe.setSeed (seed);

                auto sum = 0.0;
                auto n = 0;
                const auto other = struck (probe, false);

                for (size_t i = 0; i < (size_t) touchCells; ++i)
                {
                    const auto away = departure ((double) rungAt[i], other[i]);

                    if (! std::isnan (away))
                    {
                        sum += away;
                        ++n;
                    }
                }

                if (n > 0)
                    worstNeighbour = juce::jmax (worstNeighbour, std::abs (sum / (double) n));
            }

            probe.setSeed (1234);

            std::printf ("        seed 1234 sits %s from centred; the worst of 47 neighbours sits %s\n",
                         juce::String (std::abs (meanSigned), 5).toRawUTF8(),
                         juce::String (worstNeighbour, 5).toRawUTF8());

            check (std::abs (meanSigned) <= worstNeighbour,
                   "the tested take is no further off centred than its neighbours are",
                   juce::String (std::abs (meanSigned), 5) + " against "
                       + juce::String (worstNeighbour, 5));

            // Half the knob halves the departure, to within the one step the rounding can
            // cost. The tolerance is not a band around a number: a law that rounded the
            // delta to an integer step can never be further from half than half a step at
            // each end, and one step is what the difference of two of those is.
            probe.setTouch (0.5f);

            const auto half = struck (probe, false);

            /**
                Twice the half-knob departure has to equal the full-knob one to within one
                step — except where the ceiling has already taken the hit, which is the one
                place the ratio is allowed to break, and it is allowed to break only in the
                direction the clamp can go.

                The engine rounds the product, so away from the ends of the range the two
                arms are `round (d)` and `round (d / 2)` of the same real departure, and the
                largest disagreement rounding can manufacture is one step at each end of the
                halving, which is one step in the difference. Asked of the floats rather than
                the steps the bound lands exactly on its own limit, and the last bit of the
                `/ 127` in the seam's return is enough to put a hundred keys over it — the
                harness being loose about units, not the law being wrong, and the lattice
                gate above has already proven the values are whole rungs.

                The exemption is not slack. A rung near the top has its positive departures
                cut off by 127, so the half arm and the full arm can both land on the ceiling
                and be equal rather than halved; that is the asymmetry the control ships with,
                stated here instead of averaged away. It is counted, and the claim that
                survives it is the weaker but still exact one: opening the knob wider never
                brings a hit *closer* to the rung it was drawn at.
            */
            auto misaligned = 0, voidByCeiling = 0, notMonotone = 0;

            for (size_t i = 0; i < (size_t) touchCells; ++i)
            {
                const auto rung = rungAt[i];

                const auto fullStep = (int) std::llround (open[i]) - rung;
                const auto halfStep = (int) std::llround (half[i]) - rung;

                const auto atCeiling = rung + fullStep >= 127 || rung + fullStep <= 0;

                if (std::abs (halfStep) > std::abs (fullStep))
                    ++notMonotone;

                if (std::abs (2 * halfStep - fullStep) > 1)
                {
                    if (atCeiling)
                        ++voidByCeiling;
                    else
                        ++misaligned;
                }
            }

            std::printf ("        half the knob: %d of %d keys agree to one step of halving, %d are"
                         " exempt at the ceiling, %d are not monotone\n",
                         touchCells - misaligned - voidByCeiling, touchCells,
                         voidByCeiling, notMonotone);

            check (misaligned == 0,
                   "away from the ceiling, half the knob is half the departure to one step",
                   juce::String (misaligned) + " keys did not halve");

            check (notMonotone == 0,
                   "and wherever the clamp bites, opening the knob further still moves away"
                   " from the rung, never back towards it",
                   juce::String (notMonotone) + " keys got closer with the knob open");

            check (voidByCeiling > 0 && voidByCeiling < touchCells / 4,
                   "the ceiling is doing real work in this take, and is not the whole of it",
                   juce::String (voidByCeiling) + " of " + juce::String (touchCells)
                       + " keys exempt");

            probe.setTouch (1.0f);
        }

        //---- b. one key, one take -------------------------------------------------
        {
            probe.setTouch (1.0f);

            const auto forward = struck (probe, false);
            const auto again = struck (probe, false);
            const auto backward = struck (probe, true);

            check (hashOf (forward) == hashOf (again),
                   "the same seed asked twice gives the same levels",
                   hexOf (hashOf (forward)) + " vs " + hexOf (hashOf (again)));

            check (hashOf (forward) == hashOf (backward),
                   "and the answer does not depend on the order it was asked in",
                   hexOf (hashOf (forward)) + " forwards, " + hexOf (hashOf (backward)) + " backwards");

            probe.setSeed (1235);

            const auto other = struck (probe, false);

            auto moved = 0, agreed = 0, coarsest = 1 << 30;

            for (size_t i = 0; i < (size_t) touchCells; ++i)
            {
                const auto rung = rungAt[i];

                if (rung < 64 || rung >= 127)
                    continue;

                if (std::abs (other[i] - forward[i]) > 1.0e-9)
                    ++moved;
                else
                    ++agreed;

                coarsest = juce::jmin (coarsest, 2 * maxDelta (rung) + 1);
            }

            /**
                Three quarters of the fine rungs have to change, and the reason that is
                the right shape of claim is the coarsest one.

                A rung of 64 admits seventeen outcomes and a rung of 126 admits
                sixty-three, so two seeds agreeing on some cells is what independent draws
                *predict* — one agreement in seventeen at the coarse end — and a check that
                demanded a clean sweep would be a check that the law must ignore its own
                lattice. What it must not do is leave the take where it was, which is what
                the three quarters says, against the one in `coarsest` that luck predicts.
            */
            std::printf ("        seed 1235 moved %d of %d fine keys; the coarsest fine rung admits "
                         "%d outcomes, so independent draws agree on about one in %d\n",
                         moved, fineCells, coarsest, coarsest);

            check (moved >= fineCells * 3 / 4,
                   "a new seed re-draws the levels of the take, not a tweak of it",
                   juce::String (moved) + " of " + juce::String (fineCells) + " fine keys");

            auto sum = 0.0, absSum = 0.0;
            auto n = 0;

            for (size_t i = 0; i < (size_t) touchCells; ++i)
            {
                const auto away = departure ((double) rungAt[i], other[i]);

                if (std::isnan (away))
                    continue;

                sum += away;
                ++n;

                if ((double) rungAt[i] >= 64.0)
                    absSum += std::abs (away);
            }

            const auto otherMeanAbs = absSum / (double) fineCells;

            std::printf ("        seed 1235: mean absolute departure %s, mean signed %s\n",
                         juce::String (otherMeanAbs, 5).toRawUTF8(),
                         juce::String (sum / (double) n, 5).toRawUTF8());

            check (otherMeanAbs > Layout::maxTouchRatio * 0.27
                       && otherMeanAbs < Layout::maxTouchRatio * 0.38,
                   "and the new take obeys the same law as the old one",
                   "mean " + juce::String (otherMeanAbs, 5) + " of "
                       + juce::String (Layout::maxTouchRatio, 5));

            probe.setSeed (1234);
        }

        //---- c. three readers, three slices of one key ----------------------------
        {
            /**
                The two claims that make this a level knob rather than a second machine.

                Opening it must leave *which* hits play exactly where they were and *when*
                they land exactly where they were; closing the timing knob must leave the
                levels where they were. Each is a hash over the whole take at both ends of
                the other knob's travel, because a shared slice of the key does not change
                the average of anything — it changes which cells move together, which is
                only visible cell by cell.
            */
            probe.setMutate (1.0f);
            probe.setHumanize (1.0f);
            probe.setTouch (0.0f);

            std::vector<juce::uint8> fired ((size_t) touchCells, 0);
            std::vector<double> late ((size_t) touchCells, 0.0);
            auto worstLate = 0.0;

            const auto rollAndTime = [&] ()
            {
                for (int bar = 0; bar < touchBars; ++bar)
                    for (int step = 0; step < Layout::numSteps; ++step)
                        for (int lane = 0; lane < touchLanes; ++lane)
                        {
                            fired[indexOf (bar, step, lane)]
                                = probe.wouldThisStepHit (bar, step, lane) ? 1 : 0;

                            late[indexOf (bar, step, lane)]
                                = probe.wouldThisStepStartLate (bar, step, lane);
                        }
            };

            rollAndTime();

            const auto firedShut = fnv1a (0xcbf29ce484222325ull, fired.data(), fired.size());
            const auto lateShut = hashOf (late);

            for (const auto value : late)
                worstLate = juce::jmax (worstLate, value);

            probe.setTouch (1.0f);
            rollAndTime();

            const auto firedOpen = fnv1a (0xcbf29ce484222325ull, fired.data(), fired.size());
            const auto lateOpen = hashOf (late);

            auto worstLateOpen = 0.0;

            for (const auto value : late)
                worstLateOpen = juce::jmax (worstLateOpen, value);

            check (firedOpen == firedShut,
                   "opening the level knob does not change which hits play",
                   hexOf (firedShut) + " shut, " + hexOf (firedOpen) + " open");

            check (lateOpen == lateShut,
                   "and it does not change when they land",
                   hexOf (lateShut) + " shut, " + hexOf (lateOpen) + " open");

            // 4.3's own limit, re-measured with 4.3b's knob wide open: the bound is
            // `Layout::maxHumanizeLateSteps` and nothing in the level law gets to move
            // it, so the worst lateness is the same number on both sides.
            check (worstLateOpen == worstLate
                       && worstLate <= Layout::maxHumanizeLateSteps + 1.0e-12,
                   "the timing bound is the same with the level knob open as with it shut",
                   juce::String (worstLate, 6) + " against " + juce::String (worstLateOpen, 6)
                       + ", bound " + juce::String (Layout::maxHumanizeLateSteps, 6));

            probe.setHumanize (0.0f);
            const auto levelsStraight = hashOf (struck (probe, false));

            probe.setHumanize (1.0f);
            const auto levelsShuffled = hashOf (struck (probe, false));

            check (levelsStraight == levelsShuffled,
                   "opening the timing knob does not change how hard a hit is struck",
                   hexOf (levelsStraight) + " straight, " + hexOf (levelsShuffled) + " swung late");

            probe.setHumanize (0.0f);
            probe.setMutate (0.0f);

            /**
                And the slices do not lean on each other.

                A level knob that read the same bits as the timing knob would make every
                late hit also a loud or a quiet one, which reads as a groove that steers
                itself: the two numbers are drawn from one 64-bit key, so the only thing
                that keeps them apart is which bits each one looks at. Recovered here by
                undoing the two mappings — the travel and the square for timing, the
                travel, the proportion and the square for level — over the keys where both
                are recoverable, and gated against three sigma of the sampling noise of a
                take this size rather than against a number picked to pass.
            */
            const auto timingUnit = [&] (int bar, int step, int lane)
            {
                return std::sqrt (probe.wouldThisStepStartLate (bar, step, lane)
                                  / Layout::maxHumanizeLateSteps);
            };

            const auto levelUnit = [&] (int bar, int step, int lane)
            {
                const auto rung = (double) rungOf (lane, step);
                const auto away = departure (rung,
                                             (double) probe.wouldThisStepBeStruckAt (bar, step, lane) * 127.0);

                if (std::isnan (away))
                    return away;

                const auto signedPart = away < 0.0 ? -std::sqrt (-away / Layout::maxTouchRatio)
                                                   :  std::sqrt ( away / Layout::maxTouchRatio);

                return (signedPart + 1.0) * 0.5;
            };

            probe.setHumanize (1.0f);
            probe.setTouch (1.0f);

            auto n = 0;
            auto sumA = 0.0, sumB = 0.0, squareA = 0.0, squareB = 0.0, cross = 0.0;

            for (int bar = 0; bar < touchBars; ++bar)
                for (int step = 0; step < Layout::numSteps; ++step)
                    for (int lane = 0; lane < touchLanes; ++lane)
                    {
                        const auto rung = (double) rungOf (lane, step);

                        if (rung < 64.0 || rung >= 127.0)
                            continue;

                        const auto a = timingUnit (bar, step, lane);
                        const auto b = levelUnit (bar, step, lane);

                        if (std::isnan (b))
                            continue;

                        ++n;
                        sumA += a; sumB += b;
                        squareA += a * a; squareB += b * b;
                        cross += a * b;
                    }

            const auto count = (double) n;
            const auto meanA = sumA / count, meanB = sumB / count;
            const auto cov = cross / count - meanA * meanB;
            const auto rTime = cov / std::sqrt ((squareA / count - meanA * meanA)
                                                 * (squareB / count - meanB * meanB));
            const auto noise = 3.0 / std::sqrt (count);

            std::printf ("        %d keys where both are recoverable: r between lateness and level %s"
                         " (three sigma of a take this size is %s)\n",
                         n, juce::String (rTime, 4).toRawUTF8(), juce::String (noise, 4).toRawUTF8());

            check (std::abs (rTime) < noise,
                   "how late a hit is and how hard it is struck are drawn from separate bits",
                   "r = " + juce::String (rTime, 4) + ", limit " + juce::String (noise, 4));

            // The same question against the roll, as a point-biserial between firing and
            // the level unit: with the roll's bits and the level's bits shared, the hits
            // that survive would be systematically the loud ones.
            probe.setMutate (1.0f);

            auto firedCount = 0, pairs = 0;
            auto sumAll = 0.0, sumFired = 0.0, squareAll = 0.0;

            for (int bar = 0; bar < touchBars; ++bar)
                for (int step = 0; step < Layout::numSteps; ++step)
                    for (int lane = 0; lane < touchLanes; ++lane)
                    {
                        const auto rung = (double) rungOf (lane, step);

                        if (rung < 64.0 || rung >= 127.0)
                            continue;

                        const auto b = levelUnit (bar, step, lane);

                        if (std::isnan (b))
                            continue;

                        ++pairs;
                        sumAll += b;
                        squareAll += b * b;

                        if (probe.wouldThisStepHit (bar, step, lane))
                        {
                            ++firedCount;
                            sumFired += b;
                        }
                    }

            const auto share = (double) firedCount / (double) pairs;
            const auto meanAll = sumAll / (double) pairs;
            const auto meanFired = sumFired / (double) firedCount;
            const auto meanMissed = (sumAll - sumFired) / (double) (pairs - firedCount);
            const auto sdAll = std::sqrt (squareAll / (double) pairs - meanAll * meanAll);
            const auto rRoll = (meanFired - meanMissed) * std::sqrt (share * (1.0 - share)) / sdAll;
            const auto rollNoise = 3.0 / std::sqrt ((double) pairs);

            std::printf ("        %d of %d fine keys fired; r between firing and level %s"
                         " (limit %s)\n",
                         firedCount, pairs, juce::String (rRoll, 4).toRawUTF8(),
                         juce::String (rollNoise, 4).toRawUTF8());

            check (firedCount > pairs * 3 / 10 && firedCount < pairs * 7 / 10,
                   "the roll leaves about half the fine keys standing, so a correlation could show",
                   juce::String (firedCount) + " of " + juce::String (pairs));

            check (std::abs (rRoll) < rollNoise,
                   "which hits survive the roll and how hard they are struck are separate bits too",
                   "r = " + juce::String (rRoll, 4) + ", limit " + juce::String (rollNoise, 4));

            /**
                And the slices themselves, which no correlation can see.

                Both gates above ask whether one draw *leans* on another, and a wrong slice
                need not lean: every 16-bit window of a splitmix64 output is uniform on its
                own, and taking the level from the roll's low half moves its mean by nothing
                once the mask wraps the top of the roll back over it. The anti-vacuity run
                proved it — arming either swap left all 362 checks green. So the claim
                "three readers, three slices" needs a gate that names the bits, and the only
                way to have one from outside the engine is to re-derive the key and predict
                the answer cell by cell, which is the layout as an executable statement: a
                shared or moved slice, a changed travel or a changed clamp all land on the
                same number of mismatches, and the number is zero or it is not.
            */
            const auto keyAt = [] (int bar, int step, int lane)
            {
                const auto where = ((juce::uint64) (bar & 0x0fffffffull) << 8)
                                   | ((juce::uint64) (step & 0xf) << 4)
                                   | (juce::uint64) (lane & 0xf);

                auto value = (juce::uint64) 1234 ^ (where * 0x9E3779B97F4A7C15ull);

                value += 0x9E3779B97F4A7C15ull;
                value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
                value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;

                return value ^ (value >> 31);
            };

            auto levelWrong = 0, timingWrong = 0, rollWrong = 0;

            const auto chance = 128.0f / (float) Layout::chanceLevels;
            const auto through = 1.0f - 1.0f * (1.0f - chance);

            for (int bar = 0; bar < touchBars; ++bar)
                for (int step = 0; step < Layout::numSteps; ++step)
                    for (int lane = 0; lane < touchLanes; ++lane)
                    {
                        const auto key = keyAt (bar, step, lane);

                        // Level: bits 24..39, squared about zero, a quarter of the rung.
                        // The association follows the engine's own lines, so a mismatch
                        // can only be a moved slice or a changed law, not a last ulp.
                        const auto levelUnit = (double) ((key >> 24) & 0xffffull) * (1.0 / 65536.0);
                        const auto away = levelUnit * 2.0 - 1.0;
                        const auto deviation = away * std::abs (away);
                        const auto rung = rungOf (lane, step);

                        const auto delta = juce::roundToInt ((double) rung * deviation
                                                             * Layout::maxTouchRatio * (double) 1.0f);

                        if (juce::jlimit (0, 127, rung + delta)
                            != juce::roundToInt ((double) probe.wouldThisStepBeStruckAt (bar, step, lane)
                                                 * 127.0))
                            ++levelWrong;

                        // Timing: bits 0..23, squared one-sided, the bound scaled by it.
                        const auto timingUnit = (double) (key & 0xffffffull) * (1.0 / 16777216.0);

                        if (std::abs (Layout::maxHumanizeLateSteps * (double) 1.0f
                                      * (timingUnit * timingUnit)
                                      - probe.wouldThisStepStartLate (bar, step, lane)) > 1.0e-12)
                            ++timingWrong;

                        // Roll: bits 40..63, against what the knob lets the cell's own
                        // chance ask for -- the same two numbers `wouldThisHitFire` uses.
                        const auto rollUnit = (float) ((double) (key >> 40) * (1.0 / 16777216.0));

                        if ((rollUnit < through) != probe.wouldThisStepHit (bar, step, lane))
                            ++rollWrong;
                    }

            std::printf ("        %d cells predicted from the named slices: level %d wrong, timing %d wrong,"
                         " roll %d wrong\n",
                         touchCells, levelWrong, timingWrong, rollWrong);

            check (levelWrong == 0,
                   "the level draw is the middle 16 bits, as named, cell for cell",
                   juce::String (levelWrong) + " of " + juce::String (touchCells) + " disagree");

            check (timingWrong == 0,
                   "and the timing draw is the bottom 24, so the two never share one",
                   juce::String (timingWrong) + " of " + juce::String (touchCells) + " disagree");

            check (rollWrong == 0,
                   "and the roll is the top 24, so all three are one key with nothing left over",
                   juce::String (rollWrong) + " of " + juce::String (touchCells) + " disagree");

            probe.setMutate (0.0f);
            probe.setHumanize (0.0f);
            probe.setTouch (1.0f);
        }

        //---- d. the level reaches the samples -------------------------------------
        {
            /**
                One cell, one hit, and the two ways of asking for its level.

                The claim cannot be "the same samples, quieter": velocity is two laws in
                this engine, so a hit struck at 64 is not a hit struck at 80 turned down —
                it is also darker. What can be claimed, and what is worth far more than a
                ratio, is that the take with the knob open is bit-for-bit the grid's hit
                drawn at the level the engine itself names. A renormalisation, a floor, a
                rounding of the answer or an extra filter anywhere in the path breaks it.
            */
            constexpr int hitLane = 2;
            constexpr int hitStep = 8;
            const auto hitRung = (double) rungOf (hitLane, hitStep);

            auto pickSeed = 0;
            auto wantSteps = 0;

            // Open before the search: the knob is what makes the departure non-zero, and
            // a seed scan run with it shut finds nothing and reports that as a pass.
            probe.setTouch (1.0f);

            for (int candidate = 0; candidate < 2000; ++candidate)
            {
                probe.setSeed (candidate);

                const auto got = (double) std::llround (
                    (double) probe.wouldThisStepBeStruckAt (0, hitStep, hitLane) * 127.0);

                const auto away = (int) std::llround (got - hitRung);

                if (std::abs (away) >= 8)
                {
                    pickSeed = candidate;
                    wantSteps = away;
                    break;
                }
            }

            std::printf ("        precondition: Touch open for the search; seed %d strikes rung %d "
                         "%d steps off it (rung %d, travel %d)\n",
                         pickSeed, (int) hitRung, wantSteps, (int) hitRung, maxDelta ((int) hitRung));

            check (wantSteps != 0,
                   "a seed exists whose first hit is struck eight steps off its rung",
                   "searched 2000, found none");

            // Read at `pickSeed`, which is the seed the search left the probe on: the rung
            // the engine says it struck is what the second arm is built from, so it has to
            // come from the same seed the audio is rendered at.
            const auto struckRung = (int) std::llround (
                (double) probe.wouldThisStepBeStruckAt (0, hitStep, hitLane) * 127.0);

            probe.setSeed (1234);

            /**
                Everything that renders happens on its own processor.

                The probe is the machine the seam checks run against, and its ladder is
                still needed intact by the save test below; an audio arm clears the grid
                down to one cell, so sharing it would quietly switch those arms off.
            */
            GrooveboxProcessor player;
            arm (player, blockSize);

            const auto take = [&] (GrooveboxProcessor& p, int block, int total)
            {
                {
                    juce::AudioBuffer<float> settle (2, block);
                    juce::MidiBuffer silent;

                    for (int i = 0; i < 16; ++i)
                    {
                        settle.clear();
                        p.processBlock (settle, silent);
                    }
                }

                p.resetTransport();
                p.setPlaying (true);

                auto out = render (p, total, block, sampleRate);
                p.setPlaying (false);

                return out;
            };

            /** The one cell on, at the rung asked for, with the knob where it is asked. */
            const auto oneHit = [&] (GrooveboxProcessor& p, int block, int rung, float amount)
            {
                clearAllSteps (p);
                p.setStep (hitLane, hitStep, true);
                p.setStepVelocity (hitLane, hitStep, (float) rung / 127.0f);
                p.setStepChance (hitLane, hitStep, 1.0f);
                p.setMutate (0.0f);
                p.setHumanize (0.0f);
                p.setSeed (pickSeed);
                p.setTouch (amount);

                // The cell is at step 8, which is a second in at this tempo, so the
                // render has to be longer than the 40000 a hit at step 0 needs.
                return take (p, block, 60000);
            };

            const auto drawn = oneHit (player, blockSize, (int) hitRung, 1.0f);
            const auto asDrawn = oneHit (player, blockSize, (int) hitRung, 0.0f);
            const auto asStruck = oneHit (player, blockSize, struckRung, 0.0f);

            std::printf ("        rung %d struck at %d: peak %s as drawn, %s jittered, %s at that level\n",
                         (int) hitRung, struckRung,
                         juce::String ((double) peakOf (asDrawn.left), 9).toRawUTF8(),
                         juce::String ((double) peakOf (drawn.left), 9).toRawUTF8(),
                         juce::String ((double) peakOf (asStruck.left), 9).toRawUTF8());

            check (firstDifference (drawn.left, asStruck.left, 0, (int) drawn.left.size()) == -1
                       && firstDifference (drawn.right, asStruck.right, 0, (int) drawn.right.size()) == -1,
                   "the jittered hit is bit-for-bit that hit drawn at the level the draw said",
                   "rung " + juce::String (struckRung) + ", first difference at sample "
                       + juce::String (firstDifference (drawn.left, asStruck.left, 0, (int) drawn.left.size())));

            check (firstDifference (asDrawn.left, drawn.left, 0, (int) asDrawn.left.size()) != -1,
                   "and opening the knob actually moves the audio");

            /**
                The same at three block sizes.

                A level cannot in principle depend on a block — there is no countdown in
                it — which is exactly why this is cheap to assert and why the pitch drop's
                570-sample hole would have been caught by a line like it. What is being
                ruled out is a jitter that lands on a voice mid-block, or an envelope that
                is rebuilt with a different velocity for the tail of one. Each arm gets its
                own processor armed at its own block, because a `prepareToPlay` at 256 and a
                `processBlock` at 1024 is the harness lying about the host, not the engine
                being tested.
            */
            auto sameAtEveryBlock = true;
            juce::String perBlock;

            for (const auto block : { 64, 256, 1024 })
            {
                GrooveboxProcessor walker;
                arm (walker, block);

                const auto jittered = oneHit (walker, block, (int) hitRung, 1.0f);
                const auto matched = oneHit (walker, block, struckRung, 0.0f);

                const auto diff = firstDifference (jittered.left, matched.left, 0, (int) jittered.left.size());

                perBlock += juce::String (block) + "->" + (diff == -1 ? "same" : "differs") + " ";

                if (diff != -1)
                    sameAtEveryBlock = false;
            }

            check (sameAtEveryBlock,
                   "the same at a 64, 256 and 1024 sample block",
                   perBlock);

            /**
                A rest, in audio.

                The proportionality claim is what separates a touch from an offset, and an
                offset cannot be shown any other way: a step drawn at nothing, with the
                knob wide open, has to come out of the machine at exactly nothing. Not
                near nothing — the peak of a voice summed with a gain of zero is a bit
                pattern, and one non-zero bit is a hit the grid never asked for.
            */
            const auto rest = [&] (GrooveboxProcessor& p, int block)
            {
                clearAllSteps (p);
                p.setStep (0, 0, true);
                p.setStepVelocity (0, 0, 0.0f);
                p.setStepChance (0, 0, 1.0f);
                p.setMutate (0.0f);
                p.setHumanize (0.0f);
                p.setSeed (pickSeed);
                p.setTouch (1.0f);

                return take (p, block, 88200);
            };

            const auto silence = rest (player, blockSize);

            std::printf ("        a rung of nothing with the knob open: peak %s over %d samples\n",
                         juce::String ((double) peakOf (silence.left), 9).toRawUTF8(),
                         (int) silence.left.size());

            check (peakOf (silence.left) == 0.0f && peakOf (silence.right) == 0.0f,
                   "a step drawn at nothing makes no sound at all, however far the knob is open",
                   "peak " + juce::String ((double) peakOf (silence.left), 9));

            /**
                One bin of the spectrum, as a magnitude a ratio can be taken of.

                A single Goertzel bin rather than an FFT because the number being asked
                for is relative, both arms are the same length, and the same bin read on
                the same samples is the same measurement twice.
            */
            const auto goertzel = [&] (const std::vector<float>& data, double hz)
            {
                const auto omega = 2.0 * juce::MathConstants<double>::pi * hz / sampleRate;
                const auto coeff = 2.0 * std::cos (omega);
                auto s0 = 0.0, s1 = 0.0, s2 = 0.0;

                for (const auto value : data)
                {
                    s0 = (double) value + coeff * s1 - s2;
                    s2 = s1;
                    s1 = s0;
                }

                return std::sqrt (s1 * s1 + s2 * s2 - coeff * s1 * s2)
                         / (double) juce::jmax (1, (int) data.size());
            };

            /** The mean of seven bins across the band the README quotes. */
            const auto bandOf = [&] (const std::vector<float>& data)
            {
                auto sum = 0.0;

                for (const auto hz : { 2000.0, 2750.0, 3500.0, 4250.0, 5000.0, 6000.0, 7000.0 })
                    sum += goertzel (data, hz);

                return sum / 7.0;
            };

            /**
                What the take costs, measured and stated.

                Neither arm moves a fader: same trim, same seed, same grid, same tempo, and
                what differs is only how hard the engine was told to strike the hits it was
                already going to strike. That is the question a producer asks, and the
                answer is the number below. The level-matched A/B is the block above, where
                the second arm is the very same level arrived at by a route that never
                touches the knob; nothing here buys the cost back with a trim, a sum gain
                or a limiter, because the machine has no third thing for it to hide in.

                The prediction is the law's own, re-evaluated on this grid rather than
                quoted. A squared draw about zero leaves the mean where it was and adds its
                mean square, so the power of one hit rises by (travel squared) / 5. Four
                lanes at one step do not show that: the same *proportion* on a rung of 30
                and a rung of 127 is dominated by the cross terms, so the rise for the take
                is travel squared over five, times sum of rungs squared over the square of
                the sum — a factor below what four bars of two hundred and fifty-six draws
                can resolve. So the integrated number is reported, not gated against a
                rounded saying of it, and the gate is the arithmetic ceiling below, where
                one voice at a time makes the comparison exact.
            */
            const auto ladderTake = [&] (GrooveboxProcessor& p, int seed, float amount)
            {
                clearAllSteps (p);

                for (int lane = 0; lane < touchLanes; ++lane)
                    for (int step = 0; step < Layout::numSteps; ++step)
                    {
                        p.setStep (lane, step, true);
                        p.setStepVelocity (lane, step, (float) rungOf (lane, step) / 127.0f);
                        p.setStepChance (lane, step, 1.0f);
                    }

                p.setStepVelocity (2, 0, 0.5f);
                p.setMutate (0.0f);
                p.setHumanize (0.0f);
                p.setSeed (seed);
                p.setTouch (amount);

                return take (p, blockSize, (int) (4 * (sampleRate * 2.0)));
            };

            auto rungSumSquared = 0.0, rungSquaresSummed = 0.0;

            for (int step = 0; step < Layout::numSteps; ++step)
            {
                auto acrossLanes = 0.0;

                for (int lane = 0; lane < touchLanes; ++lane)
                {
                    const auto rung = (double) rungOf (lane, step);
                    acrossLanes += rung;
                    rungSquaresSummed += rung * rung;
                }

                rungSumSquared += acrossLanes * acrossLanes;
            }

            const auto travel = Layout::maxTouchRatio * Layout::maxTouchRatio / 5.0;
            const auto predicted = std::sqrt (1.0 + travel * rungSquaresSummed / rungSumSquared);
            const auto singleVoice = std::sqrt (1.0 + travel);

            const auto quietTake = ladderTake (player, 1234, 0.0f);
            const auto touchTake = ladderTake (player, 1234, 1.0f);

            const auto levelRatio = (double) rmsOf (touchTake.left) / (double) rmsOf (quietTake.left);

            std::printf ("        take of 4 bars, both at the same -3.5 dB trim: peak %s shut / %s open\n"
                         "        rms %s shut / %s open = x%s (%s dB). One voice's own expectation is"
                         " x%s (%s dB); on this ladder, x%s (%s dB)\n"
                         "        2..7 kHz bins: %s shut / %s open = x%s (%s dB)\n",
                         juce::String ((double) peakOf (quietTake.left), 9).toRawUTF8(),
                         juce::String ((double) peakOf (touchTake.left), 9).toRawUTF8(),
                         juce::String ((double) rmsOf (quietTake.left), 9).toRawUTF8(),
                         juce::String ((double) rmsOf (touchTake.left), 9).toRawUTF8(),
                         juce::String (levelRatio, 6).toRawUTF8(),
                         juce::String (20.0 * std::log10 (levelRatio), 4).toRawUTF8(),
                         juce::String (singleVoice, 6).toRawUTF8(),
                         juce::String (20.0 * std::log10 (singleVoice), 4).toRawUTF8(),
                         juce::String (predicted, 6).toRawUTF8(),
                         juce::String (20.0 * std::log10 (predicted), 4).toRawUTF8(),
                         juce::String ((double) bandOf (quietTake.left), 9).toRawUTF8(),
                         juce::String ((double) bandOf (touchTake.left), 9).toRawUTF8(),
                         juce::String ((double) bandOf (touchTake.left) / (double) bandOf (quietTake.left), 6).toRawUTF8(),
                         juce::String (20.0 * std::log10 ((double) bandOf (touchTake.left)
                                                          / (double) bandOf (quietTake.left)), 4).toRawUTF8());

            /**
                The ceiling, asked where it is exact.

                One voice, drawn at the top rung of the ladder, so the shut arm holds the
                loudest and brightest hit the kit can make and the open arm can only meet it
                or come down: the clamp says no struck rung is above 127, and both halves of
                the velocity path are monotone in it — the gain is a multiply, and
                `brightnessMixFor` is a straight line up to the bypass at full velocity. So
                opening the knob cannot buy level or top end back at the top of the travel,
                which is the same claim the pitch-drop and tone-law entries in the README
                make about their own controls.

                Asked of the four-lane take above it would be false, and saying so is why
                this arm exists: three of four voices at one step pulling up is enough to
                beat a shut step whose loudest rung is only one, so there is no exact
                ceiling for a summed take and only a measured one.

                The seed is searched with the knob already open, and only for a draw that
                takes the top rung *down*, because an arm that came out at 127 again would
                compare a hit with itself and report the ceiling as proven.
            */
            probe.setTouch (1.0f);

            auto topSeed = 0;
            auto topDown = 0;

            for (int candidate = 0; candidate < 500; ++candidate)
            {
                probe.setSeed (candidate);

                const auto got = (int) std::llround (
                    (double) probe.wouldThisStepBeStruckAt (0, 15, 3) * 127.0);

                if (got < 127)
                {
                    topSeed = candidate;
                    topDown = got;
                    break;
                }
            }

            std::printf ("        precondition: Touch open for the search; seed %d strikes the top rung "
                         "at %d of 127\n", topSeed, topDown);

            check (topDown > 0 && topDown < 127,
                   "a seed exists that takes the full rung down at all",
                   "searched 500, rung came back " + juce::String (topDown));

            const auto topHit = [&] (GrooveboxProcessor& p, float amount)
            {
                clearAllSteps (p);
                p.setStep (3, 15, true);
                p.setStepVelocity (3, 15, 1.0f);
                p.setStepChance (3, 15, 1.0f);
                p.setMutate (0.0f);
                p.setHumanize (0.0f);
                p.setSeed (topSeed);
                p.setTouch (amount);

                // Step 15 is seven eighths of a bar in, so the render has to clear the
                // 88200 samples a 120 bpm bar takes before the hit even arrives.
                return take (p, blockSize, 90000);
            };

            const auto topShut = topHit (player, 0.0f);
            const auto topOpen = topHit (player, 1.0f);

            const auto shutPeak = (double) peakOf (topShut.left);
            const auto openPeak = (double) peakOf (topOpen.left);
            const auto shutAir = goertzel (topShut.left, 6000.0);
            const auto openAir = goertzel (topOpen.left, 6000.0);

            std::printf ("        the rung-127 hit at seed %d: peak %s shut / %s open,"
                         " 6 kHz %s / %s\n",
                         topSeed,
                         juce::String (shutPeak, 9).toRawUTF8(), juce::String (openPeak, 9).toRawUTF8(),
                         juce::String (shutAir, 9).toRawUTF8(), juce::String (openAir, 9).toRawUTF8());

            check (openPeak < shutPeak && openPeak > 0.0,
                   "opening the knob takes the loudest hit in the kit down and never up",
                   juce::String (shutPeak, 6) + " shut against " + juce::String (openPeak, 6) + " open");

            check (openAir < shutAir && openAir > 0.0,
                   "and the same at the top of the band, so Touch is not a tone control either",
                   juce::String (shutAir, 6) + " shut against " + juce::String (openAir, 6) + " at 6 kHz");

            // A second side of the same coin, stated as arithmetic: no struck rung anywhere
            // in the take is above the ladder's own top -- the number a hidden renormalisation
            // would break to hide. Not a headroom gate: nothing here bounds the summed peak.
            check (peakOf (touchTake.left) <= peakOf (quietTake.left) * (float) (1.0 + Layout::maxTouchRatio)
                       && peakOf (quietTake.left) > 0.0f,
                   "the summed take stays inside one travel of where it was drawn",
                   juce::String ((double) peakOf (quietTake.left), 6) + " -> "
                       + juce::String ((double) peakOf (touchTake.left), 6)
                       + ", ceiling x" + juce::String (1.0 + Layout::maxTouchRatio, 3));

            probe.setSeed (1234);
        }

        //---- e. a session before this parameter, and one that goes through the record
        {
            const auto roundTrip = [] (GrooveboxProcessor& from, GrooveboxProcessor& to)
            {
                juce::MemoryBlock blob;
                from.getStateInformation (blob);
                to.setStateInformation (blob.getData(), (int) blob.getSize());
            };

            GrooveboxProcessor fresh;
            fresh.prepareToPlay (sampleRate, blockSize);

            check (fresh.getTouch() == 0.0f,
                   "a processor built now starts with the level knob shut",
                   juce::String (fresh.getTouch(), 4));

            GrooveboxProcessor mover;
            mover.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (mover);
            mover.setStep (3, 7, true);
            mover.setSwing (0.4f);
            mover.setSeed (13579);
            mover.setTouch (0.75f);

            GrooveboxProcessor back;
            back.prepareToPlay (sampleRate, blockSize);
            roundTrip (mover, back);

            check (std::abs (back.getTouch() - 0.75f) < 1.0e-4f,
                   "and the amount comes back with the session",
                   juce::String (back.getTouch(), 4));

            /**
                The take on both sides of a save.

                This is the payoff of applying the law to the rung rather than to the
                float: cell 2:0 was drawn at 0.5, which the record writes as 64 and reads
                back as 64/127. A law of the float would have perturbed 0.5 before the
                quantisation and 64/127 after it, and the same seed would stop meaning the
                same take the moment the set was saved. The precondition is asserted first
                — if the record had kept the float, this check would be vacuous.
            */
            probe.setTouch (1.0f);
            probe.setSeed (1234);

            std::vector<double> before ((size_t) touchBars, 0.0);

            for (int bar = 0; bar < touchBars; ++bar)
                before[(size_t) bar] = (double) probe.wouldThisStepBeStruckAt (bar, 0, 2) * 127.0;

            const auto beforeFloat = probe.getStepVelocity (2, 0);

            GrooveboxProcessor saved;
            saved.prepareToPlay (sampleRate, blockSize);
            roundTrip (probe, saved);

            const auto afterFloat = saved.getStepVelocity (2, 0);

            auto quantised = 0;
            auto changed = 0;

            for (int bar = 0; bar < touchBars; ++bar)
            {
                if (std::abs (afterFloat - (double) beforeFloat) > 1.0e-9)
                    ++quantised;

                if (std::abs ((double) saved.wouldThisStepBeStruckAt (bar, 0, 2) * 127.0
                                  - before[(size_t) bar]) > 1.0e-9)
                    ++changed;
            }

            std::printf ("        2:0 held %s through the session, restored %s; the take moved on %d"
                         " of %d bars\n",
                         juce::String ((double) beforeFloat, 9).toRawUTF8(),
                         juce::String ((double) afterFloat, 9).toRawUTF8(), changed, touchBars);

            check (quantised > touchBars / 2,
                   "the record really did quantise the off-lattice velocity on the way out",
                   juce::String (quantised) + " of " + juce::String (touchBars)
                       + " bars show a different float");

            check (changed == 0,
                   "and the take it draws is the same take, rung for rung",
                   juce::String (changed) + " of " + juce::String (touchBars) + " bars differ");

            /**
                The same session as the build before this parameter wrote it.

                Deleting the child is the honest way to age it: the claim is about a reader
                that has never seen the property, and a zero in the file is a value the old
                build could have written, not an absence. It lands in a processor with the
                knob open, because the failure worth catching is a restore that keeps a
                level knob the session never asked for.
            */
            juce::MemoryBlock modern;
            mover.getStateInformation (modern);

            auto xml = juce::AudioProcessor::getXmlFromBinary (modern.getData(), (int) modern.getSize());
            auto stripped = 0;

            if (xml != nullptr)
                for (auto* child : xml->getChildIterator())
                    if (child->getStringAttribute ("id") == Params::touchId)
                    {
                        xml->removeChildElement (child, true);
                        ++stripped;
                        break;
                    }

            check (stripped == 1,
                   "a session this build wrote does name the parameter in its state",
                   juce::String (stripped) + " touch entries found in the saved XML");

            juce::MemoryBlock aged;

            if (xml != nullptr)
                juce::AudioProcessor::copyXmlToBinary (*xml, aged);

            GrooveboxProcessor inherits;
            inherits.prepareToPlay (sampleRate, blockSize);
            inherits.setTouch (1.0f);
            inherits.setStateInformation (aged.getData(), (int) aged.getSize());

            check (inherits.getTouch() == 0.0f,
                   "a session that predates the parameter opens with it shut, not with whatever was live",
                   juce::String (inherits.getTouch(), 4));

            check (inherits.isStepOn (3, 7),
                   "and the grid of that session still comes through",
                   "step 3:7 was lost restoring a session with no touch entry");

            check (std::abs (inherits.getSwing() - 0.4f) < 1.0e-4f
                       && inherits.getSeed() == 13579,
                   "so do the neighbouring globals, which the old build did write",
                   "swing " + juce::String (inherits.getSwing(), 4)
                       + ", seed " + juce::String (inherits.getSeed()));
        }

        touchClick.deleteFile();
    }

    //======================================================================
    /**
        The bank: four pattern slots per lane, and a bar counter that walks a lane over
        the filled ones.

        The claims, each with the check that can go red on it:

          * a session that has never heard of the bank loads, plays and saves exactly as
            it did. The listening half of that is [28]'s fingerprint; this section holds
            the storage half — one record, of the same width as before, no bank lists;
          * every cell seam writes the lane's *selected* slot and no other slot;
          * the selection and the period are session data, so both round-trip;
          * the rotation is a pure function of the bar, walks only the slots that hold a
            record, starts from the selected one, and does not care about the block size
            or about the variation knobs;
          * a slot this build cannot read is held rather than zeroed onto disk, and an
            edit in it is what takes it over.
    */
    std::printf ("\n[32] a lane has four patterns and a bar counter\n");
    {
        /**
            Every step record the tree holds, sorted by name.

            Sorted, and then read positionally as slot order, because `ValueTree` makes no
            promise about the order its properties come back in — and because a count of
            them would be satisfied by any four names. The census checks below state the
            names, which is what makes reading `names[2]` as slot 2 a claim rather than a
            hope: this build's four names (`pattern`, `pattern1`, `pattern2`, `pattern3`)
            sort into slot order, and the check that says so is the first one in part b.

            The alternative — restating `slotPropertyName` here — is what makes an absence
            check unfalsifiable: a wrong name makes "there is no pattern2" trivially true.
        */
        auto recordNames = [] (GrooveboxProcessor& p)
        {
            juce::StringArray names;
            const auto& state = p.getValueTreeState().state;

            for (auto i = 0; i < state.getNumProperties(); ++i)
            {
                const auto name = state.getPropertyName (i).toString();
                if (name.startsWith ("pattern"))
                    names.add (name);
            }

            names.sort (true);
            return names;
        };

        auto recordOf = [] (GrooveboxProcessor& p, const juce::String& name)
        {
            return p.getValueTreeState().state.getProperty (juce::Identifier (name)).toString();
        };

        auto hasNamedProperty = [] (GrooveboxProcessor& p, const juce::String& name)
        {
            return p.getValueTreeState().state.hasProperty (juce::Identifier (name));
        };

        /** One cell as the *stored record* sees it, decoded with test-local arithmetic. */
        struct StoredCell
        {
            bool on = false;
            int level = -1;
            int chance = -1;
            int stride = 0;
            int bytesWritten = 0;
            int strikes = 1;
        };

        auto storedCell = [] (const juce::String& record, int channel, int step) -> StoredCell
        {
            const auto stride = record.startsWith ("v4:") ? 4
                              : record.startsWith ("v3:") ? 3
                              : record.startsWith ("v2:") ? 2 : 0;

            if (stride == 0)
                return {};

            juce::MemoryBlock bytes;
            juce::MemoryOutputStream sink (bytes, false);

            if (! juce::Base64::convertFromBase64 (sink, record.fromFirstOccurrenceOf (":", false, false)))
                return {};

            /**
                Flush before the size and the pointer, and take the pointer after it.

                The stream rounds the block up as it grows and only shrinks it to what was
                written in `flush`, which its destructor has not run yet — so a size read
                before the flush is the allocator's, and a data pointer read before it can
                point at memory the shrink has since moved. [19] got away with it for
                months because it only ever asked for a lower bound; this asks for the
                exact width, which is only a measurement after the flush.
            */
            sink.flush();

            const auto at = (size_t) (channel * Layout::numSteps + step) * (size_t) stride;

            if (bytes.getSize() < at + (size_t) stride)
                return { false, -1, -1, stride, (int) bytes.getSize() };

            const auto* raw = static_cast<const juce::uint8*> (bytes.getData());

            return { raw[at] != 0, (int) raw[at + 1],
                     stride > 2 ? (int) raw[at + 2] : 255, stride, (int) bytes.getSize(),
                     stride > 3 ? (int) raw[at + 3] : 1 };
        };

        /** Feeds a hand-written session the way a host would, with these root properties. */
        auto loadSession = [] (GrooveboxProcessor& p,
                               const std::initializer_list<std::pair<const char*, juce::String>> properties)
        {
            juce::ValueTree tree ("GROOVEBOX");

            for (const auto& [name, value] : properties)
                tree.setProperty (juce::Identifier (name), value, nullptr);

            if (auto xml = tree.createXml())
            {
                juce::MemoryBlock blob;
                juce::AudioProcessor::copyXmlToBinary (*xml, blob);
                p.setStateInformation (blob.getData(), (int) blob.getSize());
            }
        };

        auto saveInto = [] (GrooveboxProcessor& from, GrooveboxProcessor& to)
        {
            juce::MemoryBlock blob;
            from.getStateInformation (blob);
            to.setStateInformation (blob.getData(), (int) blob.getSize());
        };

        /** A list of lane numbers, as the `slots` and `evolve` properties hold them. */
        auto laneList = [] (std::initializer_list<int> values)
        {
            juce::StringArray list;

            for (auto i = 0; i < Layout::numChannels; ++i)
                list.add (juce::String (i < (int) values.size() ? *(values.begin() + i) : 0));

            return list.joinIntoString (",");
        };

        //======================================================================
        // a. the resting machine is one grid wide
        {
            GrooveboxProcessor machine;
            machine.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (machine);

            auto wrongSelection = 0, wrongPeriod = 0, wrongFilled = 0;

            for (int channel = 0; channel < Layout::numChannels; ++channel)
            {
                wrongSelection += machine.getChannelSlot (channel) != 0;
                wrongPeriod    += machine.getChannelEvolveBars (channel) != 0;

                for (int slot = 1; slot < Layout::numPatternSlots; ++slot)
                    wrongFilled += machine.isChannelSlotFilled (channel, slot);
            }

            check (wrongSelection == 0 && wrongPeriod == 0,
                   "every lane starts pointed at slot 0 with its counter shut",
                   juce::String (wrongSelection) + " selections and " + juce::String (wrongPeriod)
                       + " periods off, of " + juce::String (Layout::numChannels * 2));

            check (wrongFilled == 0,
                   "and no other slot of any lane holds anything",
                   juce::String (wrongFilled) + " of "
                       + juce::String (Layout::numChannels * (Layout::numPatternSlots - 1)) + " slots filled");

            check (recordNames (machine).joinIntoString (" ") == "pattern",
                   "the tree carries one step record, under the name it carried before the bank",
                   "the records are: " + recordNames (machine).joinIntoString (" "));

            const auto hasSlots = hasNamedProperty (machine, "slots");
            const auto hasEvolve = hasNamedProperty (machine, "evolve");

            check (! hasSlots && ! hasEvolve,
                   "and the two bank lists are absent while every lane is at their default",
                   "a fresh session now carries a " + juce::String (hasSlots ? "slots" : "")
                       + (hasSlots && hasEvolve ? " and an " : "")
                       + juce::String (hasEvolve ? "evolve" : "") + " property");

            const auto record = recordOf (machine, "pattern");
            const auto cell = storedCell (record, 0, 0);

            // The width is the claim that the bank did not widen slot 0's record: four
            // slots of the same grid are four properties, not one longer property, so a
            // reader that indexes a record by cell never runs off the end of a slot into
            // the next one. It stopped being a claim about the *generation* the day the
            // fourth byte joined: 4.5 made every record a stride wider than the build
            // before it wrote, and that migration is gated in [27] against a v3 payload,
            // which is where a claim about the previous build belongs.
            check (cell.bytesWritten == Layout::numChannels * Layout::numSteps * 4,
                   "and one slot's record is exactly one grid wide, not four",
                   juce::String (cell.bytesWritten) + " decoded bytes against "
                       + juce::String (Layout::numChannels * Layout::numSteps * 4)
                       + " (" + juce::String ((int) record.length()) + " characters)");
        }

        //======================================================================
        // b. an edit writes the slot the lane points at, and nothing else
        {
            GrooveboxProcessor machine;
            machine.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (machine);

            const float level = 48.0f / 127.0f;

            machine.setChannelSlot (3, 2);
            machine.setStep (3, 5, true);
            machine.setStepVelocity (3, 6, level);
            machine.setStepChance (3, 7, 96.0f / 255.0f);

            /**
                The three seams of one cell, read out of the records.

                A slot is not a runtime handle here — it is a payload — so the honest
                question "which slot did the write land in?" is asked of the session, not
                of an atomics getter that could be pointed at the wrong row by the very
                selection this check is about. Slot 0's record is the control: it was
                already on the tree, and an edit in slot 2 has no business moving it.
            */
            const auto landed = storedCell (recordOf (machine, "pattern2"), 3, 6);
            const auto control = storedCell (recordOf (machine, "pattern"), 3, 6);

            check (landed.stride == 4 && landed.level == 48,
                   "the level seam writes the slot the lane points at",
                   "pattern2 says " + juce::String (landed.level) + " of 127 over a stride of "
                       + juce::String (landed.stride));

            check (control.level == 127,
                   "and slot 0's untouched record is still the one that was there before",
                   "pattern now says " + juce::String (control.level) + " of 127");

            const auto on = storedCell (recordOf (machine, "pattern2"), 3, 5);
            const auto off = storedCell (recordOf (machine, "pattern"), 3, 5);

            check (on.on && ! off.on,
                   "the step seam does the same",
                   "pattern2 has it " + juce::String ((int) on.on) + ", pattern "
                       + juce::String ((int) off.on));

            const auto roll = storedCell (recordOf (machine, "pattern2"), 3, 7);
            const auto plain = storedCell (recordOf (machine, "pattern"), 3, 7);

            check (roll.chance == 96 && plain.chance == 255,
                   "and so does the probability seam",
                   juce::String (roll.chance) + " of 255 in slot 2, " + juce::String (plain.chance)
                       + " in slot 0");

            auto wrongFilled = 0;

            for (auto slot = 0; slot < Layout::numPatternSlots; ++slot)
                wrongFilled += slot == 2 ? ! machine.isChannelSlotFilled (3, slot)
                                          : machine.isChannelSlotFilled (3, slot);

            check (wrongFilled == 0,
                   "and only that slot now counts as holding a record",
                   juce::String (wrongFilled) + " of " + juce::String (Layout::numPatternSlots)
                       + " slots disagree");

            check (recordNames (machine).joinIntoString (" ") == "pattern pattern2",
                   "the sparse rule writes the touched slot and leaves the two it did not touch off the tree",
                   "the records are: " + recordNames (machine).joinIntoString (" "));

            check (hasNamedProperty (machine, "slots"),
                   "one lane off slot 0 is enough to put the selection list on the tree",
                   "there is no slots property with lane 3 on slot 2");

            machine.setStep (3, 5, false);
            machine.setStepVelocity (3, 6, 1.0f);
            machine.setStepChance (3, 7, 1.0f);

            check (recordNames (machine).joinIntoString (" ") == "pattern",
                   "clearing that slot takes its record back off, so an empty bank slot costs a session nothing",
                   "the records are still: " + recordNames (machine).joinIntoString (" "));

            machine.setChannelSlot (3, 0);

            check (! hasNamedProperty (machine, "slots"),
                   "and so does the list, once every lane is back on slot 0",
                   "a slots property is still on the tree with every lane at its default");
        }

        //======================================================================
        // b2. "somewhere empty" is the three-field answer, not the step-only one
        {
            GrooveboxProcessor copier;
            copier.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (copier);

            copier.setStep (0, 4, true);
            copier.setStepChance (0, 4, 102.0f / 255.0f);      // slot 0: a real pattern
            copier.setStepRatchet (0, 4, 3);

            copier.setChannelSlot (0, 1);
            copier.setStepVelocity (0, 9, 64.0f / 127.0f);     // slot 1: a level, no steps
            copier.setChannelSlot (0, 0);

            check (copier.isChannelSlotFilled (0, 1),
                   "a row of off steps that are not at full level counts as held",
                   "slot 1 has a number in it and says it is empty");

            const auto landed = copier.duplicateChannelRowToNextSlot (0);

            check (landed == 2,
                   "so the copy walks past it rather than destroying it",
                   "landed on slot " + juce::String (landed) + ", expected 2");

            check (copier.getChannelSlot (0) == 2,
                   "and the lane follows the copy to where it landed",
                   "the selection is " + juce::String (copier.getChannelSlot (0)));

            const auto survived = storedCell (recordOf (copier, "pattern1"), 0, 9);

            check (survived.level == 64 && ! survived.on,
                   "the level that made slot 1 occupied is still there",
                   "pattern1 now says level " + juce::String (survived.level)
                       + ", on " + juce::String ((int) survived.on));

            // Read out of the record rather than off the atomics: a copy that reached the
            // engine but not the session would play right and save wrong.
            const auto copied = storedCell (recordOf (copier, "pattern2"), 0, 4);

            check (copied.stride == 4 && copied.on && copied.level == 127 && copied.chance == 102
                       && copied.strikes == 3,
                   "and the copy carries all four fields into the new record",
                   "on " + juce::String ((int) copied.on) + ", level " + juce::String (copied.level)
                       + ", chance " + juce::String (copied.chance) + ", strikes "
                       + juce::String (copied.strikes) + " over a "
                       + juce::String (copied.stride) + "-byte stride");
        }

        //======================================================================
        // c. four patterns, one selection and one period per lane survive a save
        {
            GrooveboxProcessor machine, restored;
            machine.prepareToPlay (sampleRate, blockSize);
            restored.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (machine);

            const int steps[Layout::numPatternSlots]  = { 1, 5, 9, 13 };
            const int levels[Layout::numPatternSlots] = { 111, 96, 64, 32 };

            for (auto slot = 0; slot < Layout::numPatternSlots; ++slot)
            {
                machine.setChannelSlot (5, slot);
                machine.setStep (5, steps[slot], true);
                machine.setStepVelocity (5, steps[slot], (float) levels[slot] / 127.0f);
            }

            machine.setChannelSlot (2, 3);
            machine.setChannelEvolveBars (2, 4);
            machine.setChannelEvolveBars (7, 1);

            const auto names = recordNames (machine);

            // Positional reading of `names` as slot order is only safe because these four
            // sort into it, and that is the first claim below rather than an assumption.
            check (names.joinIntoString (" ") == "pattern pattern1 pattern2 pattern3",
                   "a lane with all four slots filled puts all four records on the tree, in slot order",
                   "the records are: " + names.joinIntoString (" "));

            saveInto (machine, restored);

            // Indexed off the restored tree's own census, so the decode cannot be steered
            // by the name table the writer was built from; the equality check at the end
            // of this part is what says those names are still the four slot records.
            const auto restoredNames = recordNames (restored);
            auto wrongCells = 0;

            for (auto slot = 0; slot < Layout::numPatternSlots; ++slot)
            {
                // A tree that does not hold four records fails here rather than by
                // indexing off the end of the census.
                if (slot >= restoredNames.size())
                {
                    wrongCells += Layout::numSteps;
                    continue;
                }

                const auto record = recordOf (restored, restoredNames[slot]);

                for (auto step = 0; step < Layout::numSteps; ++step)
                {
                    const auto cell = storedCell (record, 5, step);
                    const auto expectOn = step == steps[slot];

                    // The fourth column is the control rather than the exercise: this
                    // fixture never touches the roll count, so what it proves is that a
                    // record round-trips the neutral value at the new offset instead of
                    // reading whatever the stride shift left in byte 3.
                    wrongCells += cell.stride != 4
                                    || cell.on != expectOn
                                    || cell.level != (expectOn ? levels[slot] : 127)
                                    || cell.chance != 255
                                    || cell.strikes != 1;
                }
            }

            check (wrongCells == 0,
                   "each of the four records comes back as the grid that was written into it",
                   juce::String (wrongCells) + " of "
                       + juce::String (Layout::numPatternSlots * Layout::numSteps) + " cells differ");

            auto wrongBank = 0;

            for (auto channel = 0; channel < Layout::numChannels; ++channel)
            {
                const auto wantSlot = (channel == 2 || channel == 5) ? 3 : 0;
                const auto wantPeriod = channel == 2 ? 4 : channel == 7 ? 1 : 0;

                wrongBank += restored.getChannelSlot (channel) != wantSlot
                               || restored.getChannelEvolveBars (channel) != wantPeriod;
            }

            check (wrongBank == 0,
                   "and the two bank lists are session data, not runtime scratch",
                   juce::String (wrongBank) + " of " + juce::String (Layout::numChannels)
                       + " lanes come back elsewhere");

            check (hasNamedProperty (restored, "slots") && hasNamedProperty (restored, "evolve"),
                   "both lists are on the tree while any lane is off their default",
                   "restored carries slots=" + juce::String ((int) hasNamedProperty (restored, "slots"))
                       + " evolve=" + juce::String ((int) hasNamedProperty (restored, "evolve")));

            check (recordNames (restored).joinIntoString (" ") == names.joinIntoString (" "),
                   "and a save/load changes nothing about which records exist",
                   "restored has: " + recordNames (restored).joinIntoString (" "));
        }

        //======================================================================
        // d. the rotation is a function of the bar, and of nothing else
        {
            GrooveboxProcessor machine;
            machine.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (machine);

            /**
                One step, four levels: which slot is sounding reads off one number.

                The rungs are 111, 96, 64 and 32 rather than 128ths so the comparison is
                exact, and deliberately none of them is 127 — because 127 is what an
                *empty* slot says, and a check that could not tell "visited the empty
                slot" from "skipped it" would not be a check of the walk at all.
            */
            const int rungs[Layout::numPatternSlots] = { 111, 96, 64, 32 };

            for (auto slot = 0; slot < Layout::numPatternSlots; ++slot)
            {
                machine.setChannelSlot (0, slot);
                machine.setStep (0, 4, true);
                machine.setStepVelocity (0, 4, (float) rungs[slot] / 127.0f);
            }

            machine.setChannelSlot (0, 0);

            auto slotAt = [&] (juce::int64 bar)
            {
                const auto struck = machine.wouldThisStepBeStruckAt (bar, 4, 0);

                for (auto slot = 0; slot < Layout::numPatternSlots; ++slot)
                    if (std::abs (struck - (float) rungs[slot] / 127.0f) < 1.0e-6f)
                        return slot;

                return -1;   // 127ths is nothing on this ladder: an empty slot, or a rest
            };

            auto trace = [&] (int bars)
            {
                juce::String t;
                for (juce::int64 bar = 0; bar < bars; ++bar)
                    t << juce::String (slotAt (bar));
                return t;
            };

            /** Compares 32 bars against a hand-written walk, and counts the misses. */
            auto walk = [&] (const juce::String& expect)
            {
                auto wrong = 0;

                for (juce::int64 bar = 0; bar < 32; ++bar)
                    wrong += slotAt (bar) != expect.substring ((int) bar % expect.length(),
                                                               ((int) bar % expect.length()) + 1).getIntValue();

                return wrong;
            };

            auto emptied = [&] (int slot)
            {
                machine.setChannelSlot (0, slot);
                machine.setStep (0, 4, false);
                machine.setStepVelocity (0, 4, 1.0f);   // back to the default: the row says nothing
            };

            machine.setChannelEvolveBars (0, 0);

            check (walk ("0") == 0,
                   "with the counter shut the selected slot plays at every bar of thirty-two",
                   "bars answered " + trace (12));

            machine.setChannelEvolveBars (0, 1);
            check (walk ("0123") == 0,
                   "one bar a slot walks the bank in order",
                   "bars answered " + trace (12));

            machine.setChannelEvolveBars (0, 2);
            check (walk ("00112233") == 0,
                   "two bars a slot holds each one for exactly two",
                   "bars answered " + trace (16));

            machine.setChannelEvolveBars (0, 3);
            check (walk ("000111222333") == 0,
                   "a period the panel never offers still rotates, and rotates evenly",
                   "bars answered " + trace (15));

            machine.setChannelEvolveBars (0, 8);
            check (walk ("00000000111111112222222233333333") == 0,
                   "eight bars a slot covers a whole thirty-two-bar span in four turns",
                   "bars answered " + trace (20));

            machine.setChannelEvolveBars (0, 2);
            machine.setChannelSlot (0, 2);
            check (walk ("22330011") == 0,
                   "the cycle starts where the panel points, not at slot 0",
                   "from slot 2 the bars answered " + trace (10));

            machine.setChannelSlot (0, 0);
            machine.setChannelEvolveBars (0, 1);

            check (slotAt (-1) == 3,
                   "a host parked before bar zero lands inside the cycle rather than outside it",
                   "bar -1 answered slot " + juce::String (slotAt (-1)));

            emptied (1);

            check (! machine.isChannelSlotFilled (0, 1),
                   "a slot cleared back to its defaults says it holds nothing",
                   "slot 1 still counts as filled after being emptied");

            check (walk ("023") == 0,
                   "and the walk visits only the slots that hold a record",
                   "with slot 1 emptied the bars answered " + trace (12));

            machine.setChannelSlot (0, 2);
            check (walk ("230") == 0,
                   "the shortened cycle still starts at the selected slot of the shortened list",
                   "from slot 2 the bars answered " + trace (9));

            machine.setChannelSlot (0, 1);           // the empty one
            check (walk ("023") == 0,
                   "pointing the cycle at an empty slot costs no silent bar",
                   "from empty slot 1 the bars answered " + trace (12));

            emptied (2);
            emptied (3);

            machine.setChannelSlot (0, 0);
            machine.setChannelEvolveBars (0, 3);

            check (walk ("0") == 0,
                   "with one pattern left in the bank a lane never leaves it, at any period",
                   "one filled slot, thirty-two bars, answered " + trace (12) + " and should be all zeros");

            /**
                The last case is deliberately not checked.

                With every slot emptied the walk has nothing to choose from and answers
                `selected`, and the cell it reads is then at 127/127 with its step off —
                so the only observable is silence, which is also what every other answer
                would produce. A gate there would be a tautology; what the `count == 0`
                branch is really for is not indexing `filled[0]` of an empty list, which is
                a memory claim rather than a musical one, and the walk above has already
                exercised every index that list can hold.
            */
            emptied (0);
            check (! machine.isChannelSlotFilled (0, 0) && slotAt (7) == -1,
                   "an entirely empty bank is silent, which is the only honest answer",
                   "slot 0 still claims to hold a record with nothing in it");
        }

        //======================================================================
        // e. the machine that plays it agrees with the law above
        {
            auto click = makeClickWav ("bank", 200, sampleRate);
            check (click.existsAsFile(), "test wav written for the bank render");

            const int steps[Layout::numPatternSlots] = { 0, 8, 12, 14 };

            /**
                The absolute step index of every hit in four bars, at a given block size.

                All three variation knobs stay at their defaults, so nothing about a take
                can move an onset: where a hit lands is entirely which slot that bar
                sounds. That is also what makes this the arm that sees a dropped `||
                evolves` in `fireStep`'s fast path — with the knobs shut the bar would
                come back 0 for every bar, and a rotating lane would render the same four
                hits as a shut one. Which is why the shut arm below is rendered too, and
                why the two expectations are different numbers.
            */
            auto stepsOf = [&] (int block, int period)
            {
                GrooveboxProcessor machine;
                machine.prepareToPlay (sampleRate, block);

                for (auto slot = 0; slot < Layout::numPatternSlots; ++slot)
                {
                    machine.setChannelSlot (0, slot);
                    clearAllSteps (machine);
                }

                machine.setChannelSlot (0, 0);

                juce::String error;
                machine.loadSampleIntoChannel (0, click, error);

                for (auto slot = 0; slot < Layout::numPatternSlots; ++slot)
                {
                    machine.setChannelSlot (0, slot);
                    machine.setStep (0, steps[slot], true);
                }

                machine.setChannelSlot (0, 0);
                machine.setChannelEvolveBars (0, period);

                rearm (machine);
                machine.setPlaying (true);

                const auto result = render (machine, stepSamples * 64, block, sampleRate);

                std::vector<int> hits;

                for (auto at = 0; at >= 0 && at < (int) result.left.size(); )
                {
                    const auto onset = onsetAfter (result.left, at);

                    if (onset < 0)
                        break;

                    hits.push_back ((int) std::lround ((double) onset / exactStepSamples));
                    at = onset + 300;
                }

                return hits;
            };

            auto spelled = [] (const std::vector<int>& hits)
            {
                juce::StringArray list;
                for (auto h : hits)
                    list.add (juce::String (h));
                return list.joinIntoString (" ");
            };

            const auto rotating = stepsOf (blockSize, 1);

            check (rotating == std::vector<int> { 0, 24, 44, 62 },
                   "one bar per slot renders the four slots' own steps, at four bars",
                   "hits at steps " + spelled (rotating) + ", expected 0 24 44 62");

            const auto coarsely = stepsOf (1024, 1);

            check (coarsely == rotating,
                   "and the take is identical at a 1024 block, so the bar is not read off the block count",
                   "at 1024: " + spelled (coarsely));

            const auto shut = stepsOf (blockSize, 0);

            check (shut == std::vector<int> { 0, 16, 32, 48 },
                   "with the counter shut the same bank plays slot 0's pattern all four bars",
                   "hits at steps " + spelled (shut) + ", expected 0 16 32 48");

            check (shut != rotating,
                   "the two arms disagree, which is what says the render resolved a slot and not just a bar",
                   "both came out " + spelled (shut));
        }

        //======================================================================
        // f. the bank through the doors a host, an undo and a newer build use
        {
            GrooveboxProcessor machine;
            machine.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (machine);

            machine.setStep (4, 3, true);

            check (machine.isStepOn (4, 3),
                   "the step is on where the lane points",
                   "lane 4 step 3 is off before anything moved it");

            auto& undo = machine.getUndoManager();

            undo.beginNewTransaction();
            machine.setChannelSlot (4, 2);
            undo.beginNewTransaction();

            check (machine.getChannelSlot (4) == 2,
                   "pointing the lane elsewhere moves the grid the panel sees",
                   "the selection is " + juce::String (machine.getChannelSlot (4)));

            check (! machine.isStepOn (4, 3),
                   "and slot 2 really is a different grid, not another name for the same one",
                   "the step followed the selection, which means the slots are not separate");

            undo.undo();

            // The tree is the record, so an undo that only moved the atomics would put the
            // string back and leave the sound elsewhere. This is the door the pattern
            // listener exists to close, and the bank lists have to go through it too.
            check (machine.getChannelSlot (4) == 0 && machine.isStepOn (4, 3),
                   "undoing the selection comes back through the tree, not just the atomics",
                   "after undo: slot " + juce::String (machine.getChannelSlot (4))
                       + ", step " + juce::String ((int) machine.isStepOn (4, 3)));
        }

        {
            GrooveboxProcessor machine, restored;
            machine.prepareToPlay (sampleRate, blockSize);
            restored.prepareToPlay (sampleRate, blockSize);

            // A period and a selection this build has never offered, and a slot number
            // past the end of the bank: all three stay as they came in.
            loadSession (machine,
                         { { "slots",  laneList ({ 0, 0, 0, 0, 9 }) },
                           { "evolve", laneList ({ 0, 0, 0, 0, 0, 0, 3 }) } });

            check (machine.getChannelSlot (4) == Layout::numPatternSlots - 1,
                   "a lane pointed past the end of the bank is clamped for reading",
                   "lane 4 answers " + juce::String (machine.getChannelSlot (4)));

            check (machine.getChannelEvolveBars (6) == 3,
                   "a period off the ladder is kept as a number, not rounded onto a rung",
                   "lane 6 answers " + juce::String (machine.getChannelEvolveBars (6)));

            // Forcing a push is what makes this a claim rather than a restatement of the
            // load: the property would still say 9 if nothing ever rewrote it.
            machine.setChannelEvolveBars (5, 2);

            const auto slots = juce::StringArray::fromTokens (recordOf (machine, "slots"), ",", "");

            check (slots.size() == Layout::numChannels && slots[4].trim().getIntValue() == 9,
                   "and the next save writes back the number that came in, not the clamped one",
                   "slots now reads " + recordOf (machine, "slots"));

            machine.cycleChannelEvolve (6);

            check (machine.getChannelEvolveBars (6) == 4,
                   "one click on an off-ladder period lands on the rung above where it started",
                   "3 cycled to " + juce::String (machine.getChannelEvolveBars (6)));

            saveInto (machine, restored);

            check (restored.getChannelSlot (4) == Layout::numPatternSlots - 1
                       && restored.getChannelEvolveBars (6) == 4
                       && restored.getChannelEvolveBars (5) == 2,
                   "and the round trip carries them all",
                   "restored: slot " + juce::String (restored.getChannelSlot (4))
                       + ", period " + juce::String (restored.getChannelEvolveBars (6))
                       + "/" + juce::String (restored.getChannelEvolveBars (5)));

            // A record this build cannot name is held, not repaired — for slot 1 of a tree
            // whose slot 0 is perfectly readable. That pairing is the point: holding is a
            // per-slot decision, and a build that skipped the flush by name would also be
            // skipping the slot it can talk about.
            GrooveboxProcessor author;
            author.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (author);
            author.setStep (1, 6, true);

            const auto readable = recordOf (author, "pattern");
            const juce::String held = "v99:" + readable.fromFirstOccurrenceOf (":", false, false);

            GrooveboxProcessor newer;
            newer.prepareToPlay (sampleRate, blockSize);

            loadSession (newer, { { "stateVersion", "99" },
                                  { "pattern",  readable },
                                  { "pattern1", held } });

            check (recordOf (newer, "pattern1") == held,
                   "an unreadable slot passes straight back out",
                   "came in as v99 and left as " + recordOf (newer, "pattern1").substring (0, 4));

            check (! newer.isChannelSlotFilled (0, 1) && ! newer.isChannelSlotFilled (1, 1),
                   "and the slot it describes reads as empty, which is the true picture of what this build can hear",
                   "an unreadable slot claims to hold something");

            check (newer.isStepOn (1, 6),
                   "the readable slot came through untouched beside it",
                   "lane 1 step 6 was lost while slot 1 was being held");

            newer.setChannelSlot (1, 1);

            check (! newer.isStepOn (1, 6) && std::abs (newer.getStepVelocity (1, 6) - 1.0f) < 1.0e-6f,
                   "and the slot it cannot read really is an empty grid, not the one from before",
                   "the held slot came back with a cell in it");

            const auto beforeTakeover = recordOf (newer, "pattern");

            // Lane 1 is the one still pointed at slot 1, and a record is a whole grid, so
            // an edit in any lane of that slot is what takes the held payload over.
            newer.setStep (1, 2, true);

            check (recordOf (newer, "pattern1").startsWith ("v4:"),
                   "an edit in that slot is what takes it over, in this build's own format",
                   "after an edit slot 1 writes " + recordOf (newer, "pattern1").substring (0, 4));

            check (recordOf (newer, "pattern") == beforeTakeover,
                   "and taking one slot over leaves the other slot's bytes alone",
                   "slot 0's record moved when only slot 1 was edited");
        }
    }

    //======================================================================
    std::printf ("\n[33] one hit can be several strikes\n");
    {
        /**
            A roll is one hit that happens several times, not several hits.

            Every expectation below is taken from a render of the machine rather than from
            `getStrikeDelayInSteps`, because a spacing gate whose expected pitch came back
            from the function being gated would be asking the predicate what it expects. The
            ruler is the grid's own output: run a pass in which every step is a single
            strike, read the onsets off it, and the room a step has is the distance from its
            onset to the next one's, in samples, with no formula and no constant in it. The
            law then says where strike `k` of `n` lands, and the render either agrees or not.

            What the law claims, in the order the arms check it: the spacing follows that
            measured room and nothing else; a strike can never reach into the next onset; the
            first strike does not move, so a cell at one strike is the cell that shipped
            before the field existed; the extra strikes carry the one draw of every dice, not
            new ones; they are one choke, not eight; and the pool survives ninety-six of them
            on every lane at once.
        */
        constexpr int probeStep = 4;
        constexpr int strikes = 8;

        /**
            One bar, and deliberately not a sample into the next.

            The arms below count onsets, so a render that reaches the top of the second bar
            fires step nought again and every count comes out one too high -- which is exactly
            how the first run of this section failed. The last onset it has to hold is step
            fifteen's, plus the click's own hundred-odd samples.
        */
        const auto barSamples = (int) (exactStepSamples * ((double) Layout::numSteps - 0.05));

        /**
            Onsets in order, each searched for only after the last one has died away.

            `ringOut` is how far past an onset the source is still loud enough to cross the
            threshold. Without it one long source reads as a run of onsets at the same place.
        */
        auto scanOnsets = [] (const std::vector<float>& data, int ringOut)
        {
            std::vector<int> found;
            int at = 0;

            while (true)
            {
                const auto next = onsetAfter (data, at);

                if (next < 0)
                    break;

                found.push_back (next);
                at = next + ringOut;
            }

            return found;
        };

        auto rmsBetween = [] (const std::vector<float>& data, int from, int to)
        {
            const auto last = juce::jmin (to, (int) data.size());
            double sum = 0.0;
            int count = 0;

            for (int i = juce::jmax (0, from); i < last; ++i, ++count)
                sum += (double) data[(size_t) i] * (double) data[(size_t) i];

            return count > 0 ? (float) std::sqrt (sum / (double) count) : 0.0f;
        };

        /** Click on lane 0, every step of it on, certain, full level, one strike. */
        auto armEveryStep = [] (GrooveboxProcessor& p, const juce::File& sample)
        {
            juce::String error;
            p.loadSampleIntoChannel (0, sample, error);
            clearAllSteps (p);

            for (int step = 0; step < Layout::numSteps; ++step)
            {
                p.setStep (0, step, true);
                p.setStepVelocity (0, step, 1.0f);
                p.setStepChance (0, step, 1.0f);
                p.setStepRatchet (0, step, 1);
            }

            p.setMutate (0.0f);
        };

        /**
            One bar, from the top, every time.

            A render is a measurement and the transport is a cursor, so passing the same
            machine to two arms without rewinding makes the second one a second bar -- which
            moves every onset in it and quietly turns "within two samples of its share of the
            room" into a comparison against a bar that has drifted a whole step along.
        */
        auto playBar = [&] (GrooveboxProcessor& p, int totalSamples)
        {
            p.setPlaying (true);
            auto out = render (p, totalSamples, blockSize, sampleRate);
            p.setPlaying (false);
            return out;
        };

        /** The same, but with how many voices were still sounding at the last block. */
        auto playBarAndCount = [&] (GrooveboxProcessor& p, int totalSamples, int& voicesStillOn)
        {
            p.setPlaying (true);
            auto out = render (p, totalSamples, blockSize, sampleRate);
            voicesStillOn = p.debugActiveVoices();   // before the stop, which is not what this asks
            p.setPlaying (false);
            return out;
        };

        auto roundTrip = [] (GrooveboxProcessor& from, GrooveboxProcessor& to)
        {
            juce::MemoryBlock blob;
            from.getStateInformation (blob);
            to.setStateInformation (blob.getData(), (int) blob.getSize());
        };

        //==================================================================
        // a. the strikes fill the room the grid gave the step, and no more
        //==================================================================
        std::printf ("    a. strikes spread across the step's own room\n");
        {
            GrooveboxProcessor p;
            p.prepareToPlay (sampleRate, blockSize);

            auto click = makeClickWav ("ratchet-straight", 120, sampleRate);
            armEveryStep (p, click);

            auto straightBar = [&] ()
            {
                rearm (p);
                return playBar (p, barSamples);
            };

            const auto flat = straightBar();
            const auto onsets = scanOnsets (flat.left, 120);
            const bool ruler = (int) onsets.size() == Layout::numSteps;

            // The precondition everything else in this arm rests on: sixteen steps armed at
            // one strike have to give sixteen onsets, or there is no room to measure.
            check (ruler,
                   "a single-strike pass puts one onset on every step, which is the ruler the"
                   " rest of the section measures against",
                   "found " + juce::String ((int) onsets.size()) + " onsets in a straight"
                                                                   " single-strike bar");

            const auto room = ruler ? onsets[probeStep + 1] - onsets[probeStep] : 0;

            std::printf ("        step %d sits at %d, the next onset at %d: the room is %d"
                         " samples (%s steps)\n",
                         probeStep, ruler ? onsets[probeStep] : -1,
                         ruler ? onsets[probeStep + 1] : -1, room,
                         juce::String ((double) room / exactStepSamples, 4).toRawUTF8());

            check (room > 4000 && room < 7000,
                   "and the room is the step itself, which is what a straight grid means and"
                   " keeps a roll of eight from being measured against a broken ruler",
                   "the room off the grid is " + juce::String (room) + " samples");

            p.setStepRatchet (0, probeStep, strikes);

            const auto rolled = straightBar();
            const auto marks = scanOnsets (rolled.left, 120);

            // One onset became eight, so the bar now carries fifteen plus eight.
            check ((int) marks.size() == Layout::numSteps + strikes - 1,
                   "eight strikes replace the one onset rather than adding to a bar that"
                   " already had it",
                   "the rolled bar carries " + juce::String ((int) marks.size())
                       + " onsets, expected " + juce::String (Layout::numSteps + strikes - 1));

            const bool enough = (int) marks.size() >= probeStep + strikes;
            int worstSpacing = 0;

            for (int k = 1; k < strikes && enough; ++k)
            {
                const auto expected = onsets[probeStep]
                                        + (int) std::llround ((double) room * (double) k / (double) strikes);
                worstSpacing = juce::jmax (worstSpacing, std::abs (marks[probeStep + k] - expected));
            }

            check (enough && worstSpacing <= 2,
                   "every strike lands within two samples of its share of that measured room,"
                   " so the spacing is the grid's and not a milliseconds-per-roll constant",
                   "worst spacing error " + juce::String (worstSpacing) + " samples against "
                       + juce::String (room) + " divided eight ways");

            // The other half of the room: the last strike stops short of the next onset by
            // the same fraction the first started it. That is "a roll can fill a step, it
            // cannot reach into the next one".
            check (enough && marks[probeStep + strikes - 1] < onsets[probeStep + 1] - 50,
                   "and the last of them is still clear of the next onset by more than a"
                   " rounding, so eight cannot become nine by leaning on the step after",
                   "the eighth strike reached "
                       + juce::String (enough ? marks[probeStep + strikes - 1] : -1)
                       + ", against the next onset at "
                       + juce::String (ruler ? onsets[probeStep + 1] : -1));

            // The count is the control's, not the knob's: two strikes have to cost two
            // onsets, or "eight" would only be proving that some number works.
            p.setStepRatchet (0, probeStep, 2);
            const auto pair = straightBar();
            const auto pairMarks = scanOnsets (pair.left, 120);

            check ((int) pairMarks.size() == Layout::numSteps + 1 && ruler
                     && std::abs (pairMarks[probeStep + 1] - (onsets[probeStep] + room / 2)) <= 2,
                   "and two strikes cost two onsets half a room apart, so the number on the"
                   " cell is the number that sounds",
                   "two strikes produced " + juce::String ((int) pairMarks.size())
                       + " onsets, second at "
                       + juce::String ((int) pairMarks.size() > probeStep + 1 ? pairMarks[probeStep + 1] : -1)
                       + ", expected "
                       + juce::String (ruler ? onsets[probeStep] + room / 2 : -1));

            // Strike nought is the hit itself. Everything up to and including its onset
            // sample is the bytes the old build wrote, which is the one claim that makes the
            // feature additive rather than a retuning.
            check (ruler && firstDifference (flat.left, rolled.left, 0, onsets[probeStep] + 1) == -1,
                   "and the first strike has not moved at all: up to its onset the rolled bar"
                   " is the single-strike bar, sample for sample",
                   "a cell at eight strikes differs from the same cell at one before its own"
                   " onset, at "
                       + juce::String (ruler ? firstDifference (flat.left, rolled.left, 0, onsets[probeStep] + 1) : -2));

            // And the rest of the bar, which is what stops a voice-pool side effect from
            // passing as correct spacing.
            check (enough && ruler && std::abs (marks[probeStep + strikes] - onsets[probeStep + 1]) <= 2,
                   "and the step after the roll still starts where it did, so eight strikes did"
                   " not push the grid sideways",
                   "step " + juce::String (probeStep + 1) + " moved "
                       + juce::String (enough && ruler
                                         ? std::abs (marks[probeStep + strikes] - onsets[probeStep + 1]) : -1)
                       + " samples when its neighbour was rolled");

            click.deleteFile();
        }

        //==================================================================
        // b. on a shuffled grid the short room gets the short roll
        //==================================================================
        std::printf ("\n    b. the roll follows swing, because it follows the room\n");
        {
            GrooveboxProcessor p;
            p.prepareToPlay (sampleRate, blockSize);

            auto click = makeClickWav ("ratchet-shuffle", 120, sampleRate);
            armEveryStep (p, click);

            auto shuffledBar = [&] ()
            {
                rearm (p);
                p.setSwing (1.0f);
                p.setSwingGrid (4);
                return playBar (p, barSamples);
            };

            const auto flat = shuffledBar();
            const auto onsets = scanOnsets (flat.left, 120);

            // Step 4 is the head of a shuffled pair and step 6 its off-beat. Both rooms come
            // from the same render, so the arm cannot be handed a grid it did not measure.
            const int head = probeStep, off = 6;
            const bool ruler = (int) onsets.size() == Layout::numSteps;

            check (ruler,
                   "the shuffled bar still shows one onset per step, so the rooms below are"
                   " measured and not assumed",
                   "found " + juce::String ((int) onsets.size()) + " onsets");

            const auto headRoom = ruler ? onsets[head + 1] - onsets[head] : 0;
            const auto offRoom  = ruler ? onsets[off + 1] - onsets[off] : 0;

            std::printf ("        shuffled rooms: step %d holds %d samples, step %d holds %d"
                         " (ratio %s)\n",
                         head, headRoom, off, offRoom,
                         juce::String ((double) headRoom / (double) juce::jmax (1, offRoom), 3).toRawUTF8());

            check (ruler && headRoom > 2 * offRoom,
                   "the head of a shuffled pair really does have more than twice the room of"
                   " its off-beat, which is what makes this a swing arm and not arm a twice",
                   "the two rooms are " + juce::String (headRoom) + " and " + juce::String (offRoom)
                       + " samples");

            p.setStepRatchet (0, head, strikes);
            p.setStepRatchet (0, off,  strikes);

            const auto rolled = shuffledBar();
            const auto marks = scanOnsets (rolled.left, 120);

            const auto expectedCount = Layout::numSteps + 2 * (strikes - 1);
            const bool complete = (int) marks.size() == expectedCount && ruler;

            check ((int) marks.size() == expectedCount,
                   "two rolled steps in one bar produce exactly fourteen extra onsets, eight to"
                   " each, and no third roll appears anywhere",
                   "the shuffled roll produced " + juce::String ((int) marks.size())
                       + " onsets, expected " + juce::String (expectedCount));

            if (complete)
            {
                // Where step `step`'s first strike sits in the scan: every rolled step before
                // it contributed strikes-1 extra marks. Derived from the step order rather
                // than remembered, which is what keeps this a scan and not a second law.
                const auto startOf = [&] (int step)
                {
                    int start = step;
                    if (step > head) start += strikes - 1;
                    if (step > off)  start += strikes - 1;
                    return start;
                };

                int worstHead = 0, worstOff = 0;

                for (int k = 1; k < strikes; ++k)
                {
                    worstHead = juce::jmax (worstHead,
                        std::abs (marks[startOf (head) + k]
                                   - (onsets[head] + (int) std::llround ((double) headRoom * k / strikes))));
                    worstOff = juce::jmax (worstOff,
                        std::abs (marks[startOf (off) + k]
                                   - (onsets[off] + (int) std::llround ((double) offRoom * k / strikes))));
                }

                check (worstHead <= 2 && worstOff <= 2,
                       "and each roll spreads over its own room: the head within "
                           + juce::String (worstHead) + " samples of an eighth of "
                           + juce::String (headRoom) + ", the off-beat within "
                           + juce::String (worstOff) + " of an eighth of " + juce::String (offRoom),
                       "the shuffled spacing is wrong by " + juce::String (worstHead) + " and "
                           + juce::String (worstOff) + " samples");

                const auto headStep = (double) (marks[startOf (head) + strikes - 1] - marks[startOf (head)])
                                        / (double) (strikes - 1);
                const auto offStep  = (double) (marks[startOf (off) + strikes - 1] - marks[startOf (off)])
                                        / (double) (strikes - 1);

                std::printf ("        mean strike spacing: head %.1f samples, off-beat %.1f\n",
                             headStep, offStep);

                check (headStep > offStep * 1.5,
                       "and the two rolls are audibly different machines: the same knob on the"
                       " off-beat plays more than half again as fast, because the room it was"
                       " given is smaller",
                       "off-beat " + juce::String (offStep, 1) + " samples apart against the"
                         " head's " + juce::String (headStep, 1) + ", so the count is being"
                         " spread over a fixed time and not the grid");

                check (marks[startOf (off) + strikes - 1] < onsets[off + 1] - 50,
                       "and the tightest room the grid offers still holds all eight without"
                       " leaning on the onset after it",
                       "eight strikes in a " + juce::String (offRoom) + " sample room reached "
                           + juce::String (marks[startOf (off) + strikes - 1]) + ", past the next"
                             " onset at " + juce::String (onsets[off + 1]));
            }

            click.deleteFile();
        }

        //==================================================================
        // c. eight strikes are one choke
        //==================================================================
        std::printf ("\n    c. the strikes do not cut each other off\n");
        {
            /**
                A lane that holds, rolled to `hits`, in choke group `group`.

                Broadband noise rather than a tone, because seven copies of one sine at
                arbitrary phases can cancel to almost nothing and a level check on them would
                be a check on their phases. Noise sums by power whatever the spacing, so the
                growth this arm asks for is the accumulation and not a coincidence.
            */
            GrooveboxProcessor p;
            p.prepareToPlay (sampleRate, blockSize);

            auto source = makeNoiseWav ("ratchet-choke", makeNoiseSamples ((int) (2.0 * sampleRate)), sampleRate);

            const auto hold = [&] (int hits, int group)
            {
                juce::String error;
                p.loadSampleIntoChannel (0, source, error);
                clearAllSteps (p);

                p.setStep (0, probeStep, true);
                p.setStepVelocity (0, probeStep, 1.0f);
                p.setStepChance (0, probeStep, 1.0f);
                p.setStepRatchet (0, probeStep, hits);

                p.setChannelLevel (0, 0.12f);
                p.setChannelDecay (0, 0.0f);
                p.setChannelSustain (0, 1.0f);
                p.setChannelChokeGroup (0, group);

                p.setMutate (0.0f);
                rearm (p);
            };

            const auto tailFrom = (int) (exactStepSamples * 5.5);
            const auto tailTo   = (int) (exactStepSamples * 9.0);
            int voices = 0;

            // Group 0 is the control: nothing can choke anything there, so whatever this
            // leaves ringing is what eight strikes cost. It is also the precondition for the
            // voice count the claim arm compares against -- without it a pool that never
            // filled would look like a choke that worked.
            hold (strikes, 0);
            const auto ungrouped = playBarAndCount (p, barSamples, voices);
            const auto freeVoices = voices;
            const auto ungroupedTail = rmsBetween (ungrouped.left, tailFrom, tailTo);

            std::printf ("        %d strikes, no choke group: %d voices still sounding, tail"
                         " rms %s\n", strikes, freeVoices, juce::String (ungroupedTail, 5).toRawUTF8());

            check (freeVoices == strikes,
                   "eight strikes leave eight voices ringing, which is what makes the count"
                   " below a measurement of the roll and not of an empty pool",
                   "a rolled lane with nothing to choke it holds " + juce::String (freeVoices)
                       + " voices, expected " + juce::String (strikes));

            // One and three strikes, so the count is seen to track the knob. A build that
            // reported eight for any roll would pass the claim arm and fail here.
            hold (1, 0);
            const auto single = playBarAndCount (p, barSamples, voices);
            const auto singleVoices = voices;
            const auto singleTail = rmsBetween (single.left, tailFrom, tailTo);

            hold (3, 0);
            playBarAndCount (p, barSamples, voices);
            const auto tripleVoices = voices;

            check (singleVoices == 1 && tripleVoices == 3,
                   "and the count is the count: one strike leaves one voice, three leave three",
                   "strike counts of 1 and 3 left " + juce::String (singleVoices) + " and "
                       + juce::String (tripleVoices) + " voices");

            hold (strikes, 1);
            const auto choked = playBarAndCount (p, barSamples, voices);
            const auto chokedVoices = voices;
            const auto chokedTail = rmsBetween (choked.left, tailFrom, tailTo);
            const auto chokedPeak = peakOf (choked.left);

            std::printf ("        %d strikes inside a choke group: %d voices still sounding,"
                         " tail rms %s against %s ungrouped and %s for one hit, peak %s\n",
                         strikes, chokedVoices, juce::String (chokedTail, 5).toRawUTF8(),
                         juce::String (ungroupedTail, 5).toRawUTF8(),
                         juce::String (singleTail, 5).toRawUTF8(),
                         juce::String (chokedPeak, 4).toRawUTF8());

            check (chokedVoices == freeVoices,
                   "a lane in a choke group still holds all eight, because the strikes after"
                   " the first are the same hit and a hit cannot cut itself up",
                   "the choke group's roll kept " + juce::String (chokedVoices) + " of "
                       + juce::String (freeVoices) + " voices, so every strike is firing the"
                         " choke and the roll is a fade");

            // The audible half, with the ceiling that keeps it honest: eight voices of
            // incoherent noise add about the square root of eight (2.83, and 2.81 measured
            // here), so the tail of a roll is far louder than the tail of a hit -- and none
            // of it is the bus being overdriven.
            check (chokedTail > singleTail * 1.5f && ungroupedTail > singleTail * 1.5f
                     && std::abs (chokedTail - ungroupedTail) < ungroupedTail * 0.25f,
                   "and you can hear it: the tail of a choked roll is more than half again as"
                   " loud as a single hit, and within a fifth of the same roll with nothing to"
                   " choke it, so the group costs no level",
                   "tails " + juce::String (singleTail, 5) + " for one hit, "
                       + juce::String (ungroupedTail, 5) + " ungrouped, "
                       + juce::String (chokedTail, 5) + " choked");

            check (chokedPeak < 0.9f,
                   "and none of that is clipping: the whole render peaks under -1 dB, so the"
                   " growth is voices summing and not the bus folding",
                   "the accumulation fixture peaks at " + juce::String (chokedPeak, 4));

            //==================================================================
            // The other direction: a *different* hit in the same group is still cut. A roll
            // that declined to choke anything at all would pass everything above and quietly
            // switch the choke off. Lane 0 is a short click so that the late window belongs
            // to the victim alone.
            //==================================================================
            {
                GrooveboxProcessor q;
                q.prepareToPlay (sampleRate, blockSize);

                auto click = makeClickWav ("ratchet-choke-victim", 120, sampleRate);

                juce::String error;
                q.loadSampleIntoChannel (0, click, error);
                q.loadSampleIntoChannel (1, source, error);
                clearAllSteps (q);

                q.setStep (0, probeStep, true);
                q.setStepRatchet (0, probeStep, strikes);
                q.setStep (1, 0, true);

                q.setChannelLevel (0, 0.9f);
                q.setChannelSustain (0, 0.0f);
                q.setChannelChokeGroup (0, 1);

                q.setChannelLevel (1, 1.0f);
                q.setChannelDecay (1, 0.0f);
                q.setChannelSustain (1, 1.0f);
                q.setChannelChokeGroup (1, 1);
                q.setMutate (0.0f);

                const auto victimRms = [&] (int groupForVictim)
                {
                    q.setChannelChokeGroup (1, groupForVictim);
                    rearm (q);
                    return playBar (q, barSamples);
                };

                const auto cutRender = victimRms (1);
                const auto heldRender = victimRms (0);

                const auto lateFrom = (int) (exactStepSamples * 8.0);
                const auto lateTo   = (int) (exactStepSamples * 11.0);

                const auto cut  = rmsBetween (cutRender.left, lateFrom, lateTo);
                const auto held = rmsBetween (heldRender.left, lateFrom, lateTo);

                std::printf ("        a held lane in the group, late rms %s with the roll"
                             " allowed to choke it, %s with it not\n",
                             juce::String (cut, 5).toRawUTF8(), juce::String (held, 5).toRawUTF8());

                check (held > 0.05f && cut < held * 0.1f,
                       "and the choke is not gone: a held lane in the same group is still cut"
                       " by the roll's first strike, so eight strikes decline to choke"
                       " themselves and not the hit next to them",
                       "the grouped victim reads " + juce::String (cut, 5) + " against "
                           + juce::String (held, 5) + " when it was left alone");

                click.deleteFile();
            }

            source.deleteFile();
        }

        //==================================================================
        // d. one draw of each dice, eight strikes carrying it
        //==================================================================
        std::printf ("\n    d. the roll is one hit for the variation too\n");
        {
            GrooveboxProcessor p;
            p.prepareToPlay (sampleRate, blockSize);

            auto click = makeClickWav ("ratchet-dice", 120, sampleRate);

            armEveryStep (p, click);
            p.setStepRatchet (0, probeStep, strikes);

            rearm (p);
            const auto certain = playBar (p, barSamples);
            const auto certainMarks = scanOnsets (certain.left, 120);
            const auto spacing = (int) certainMarks.size() >= probeStep + strikes
                                   ? certainMarks[probeStep + 1] - certainMarks[probeStep] : 0;

            std::printf ("        an eighth of the straight room is %d samples\n", spacing);

            check (spacing >= 600 && spacing <= 800,
                   "the straight-grid strike spacing is an eighth of a step, which is the ruler"
                   " the humanised arm is about to be compared with",
                   "the strike spacing off a straight grid is " + juce::String (spacing)
                       + " samples");

            // Now open the timing knob. If every strike re-rolled its own lateness the
            // spacing would jitter by up to a quarter of a step -- more than a third of the
            // spacing itself. The claim is that it does not, because one hit is drawn once
            // and all eight strikes carry that one draw.
            rearm (p);
            p.setHumanize (1.0f);

            const auto late = playBar (p, barSamples);
            const auto lateMarks = scanOnsets (late.left, 120);

            check ((int) lateMarks.size() == Layout::numSteps + strikes - 1,
                   "opening the timing knob does not change how many strikes sound",
                   "the humanised roll shows " + juce::String ((int) lateMarks.size())
                       + " onsets, expected " + juce::String (Layout::numSteps + strikes - 1));

            if ((int) lateMarks.size() == Layout::numSteps + strikes - 1 && spacing > 0)
            {
                int worst = 0;

                for (int k = 1; k < strikes; ++k)
                    worst = juce::jmax (worst, std::abs ((lateMarks[probeStep + k] - lateMarks[probeStep])
                                                         - k * spacing));

                check (worst <= 2,
                       "and with the timing knob wide open the eight strikes are still exactly"
                       " as evenly spaced as they were shut, off by at most two samples over"
                       " seven of them",
                       "opening the timing knob spread the roll by up to " + juce::String (worst)
                           + " samples, so each strike draws its own lateness and a roll is"
                             " eight hits instead of one");

                const auto drawn = p.wouldThisStepStartLate (0, probeStep, 0);
                const auto expected = certainMarks[probeStep]
                                        + (int) std::llround (drawn * exactStepSamples);
                const auto error = std::abs (lateMarks[probeStep] - expected);

                std::printf ("        the take says step %d is %.4f steps late (%d samples); the"
                             " roll starts at %d, the certain one at %d\n",
                             probeStep, drawn, (int) std::llround (drawn * exactStepSamples),
                             lateMarks[probeStep], certainMarks[probeStep]);

                check (error <= 2,
                       "and the first strike sits on the lateness this take was already going to"
                       " have, so the hit that got rolled is the hit that would have played"
                       " anyway",
                       "the rolled hit starts " + juce::String (error)
                           + " samples away from the lateness its own take reports");
            }

            // And the probability: a step the dice reject has to cost nothing at all, eight
            // strikes or one.
            p.setHumanize (0.0f);
            p.setMutate (1.0f);

            for (int step = 0; step < Layout::numSteps; ++step)
                p.setStepChance (0, step, 0.5f);

            int dropped = 0, kept = 0;

            for (int step = 0; step < Layout::numSteps; ++step)
                p.wouldThisStepHit (0, step, 0) ? ++kept : ++dropped;

            std::printf ("        at half chance with the knob wide open the first bar keeps %d"
                         " steps and drops %d\n", kept, dropped);

            check (dropped > 0 && kept > 0,
                   "the dice are actually rolling here, so a check that a dropped step is silent"
                   " is a check about something that happens",
                   "half chance over sixteen steps dropped " + juce::String (dropped)
                       + " and kept " + juce::String (kept));

            // Arm one kept step early in the bar and one dropped step late in it, so the
            // silence around the second cannot be the tail of the first.
            int quietStep = -1, loudStep = -1;

            for (int step = 1; step < Layout::numSteps; ++step)
            {
                const auto hits = p.wouldThisStepHit (0, step, 0);
                if (hits && loudStep < 0)  loudStep = step;
                if (! hits && step >= 9)   quietStep = step;
            }

            check (loudStep >= 0 && quietStep >= 0,
                   "and there is a kept step early in the bar and a dropped one late in it to"
                   " compare",
                   "kept " + juce::String (loudStep) + ", dropped " + juce::String (quietStep));

            if (loudStep >= 0 && quietStep >= 0 && spacing > 0)
            {
                clearAllSteps (p);
                p.setStep (0, loudStep, true);
                p.setStep (0, quietStep, true);
                p.setStepRatchet (0, loudStep, strikes);
                p.setStepRatchet (0, quietStep, strikes);

                rearm (p);
                p.setMutate (1.0f);

                const auto take = playBar (p, barSamples);
                const auto marks = scanOnsets (take.left, 120);

                const auto centre = (int) std::llround (exactStepSamples * (double) quietStep);

                int inWindow = 0;

                for (auto m : marks)
                    if (m >= centre - 500 && m <= centre + 500)
                        ++inWindow;

                std::printf ("        step %d is kept and rolled, step %d is dropped and rolled;"
                             " %d onsets around the dropped one, %d in the bar\n",
                             loudStep, quietStep, inWindow, (int) marks.size());

                check (inWindow == 0 && (int) marks.size() == strikes,
                       "a step the dice reject sounds nothing at eight strikes either, and the"
                       " step they kept still sounds all eight",
                       "the rejected step showed " + juce::String (inWindow)
                           + " onsets and the bar " + juce::String ((int) marks.size())
                           + ", so the probability is being drawn per strike");
            }

            click.deleteFile();
        }

        //==================================================================
        // e. the pool, asked for ninety-six strikes at once
        //==================================================================
        std::printf ("\n    e. every lane rolled at once still fits\n");
        {
            int worstVoices = 0;
            bool finite = true;

            /**
                Renders in blocks and watches the pool while it goes.

                `playBar` cannot be used here: the claim is about the highest the count ever
                reaches, and a number read only once the pass is over has already been brought
                back down by every voice that finished.
            */
            auto watch = [&] (GrooveboxProcessor& proc, int totalSamples)
            {
                juce::AudioBuffer<float> buffer (2, blockSize);
                juce::MidiBuffer midi;

                for (int done = 0; done < totalSamples; done += blockSize)
                {
                    buffer.clear();
                    proc.processBlock (buffer, midi);

                    worstVoices = juce::jmax (worstVoices, proc.debugActiveVoices());

                    for (int channel = 0; channel < 2; ++channel)
                        for (int i = 0; i < blockSize; ++i)
                            if (! std::isfinite (buffer.getSample (channel, i)))
                                finite = false;
                }
            };

            GrooveboxProcessor p;
            p.prepareToPlay (sampleRate, blockSize);

            // A source long enough that a hit is still ringing four steps on, or the pool
            // would drain between strikes and the ceiling would never be reached.
            auto source = makeNoiseWav ("ratchet-pool", makeNoiseSamples (22050), sampleRate);

            for (int channel = 0; channel < Layout::numChannels; ++channel)
            {
                juce::String error;
                p.loadSampleIntoChannel (channel, source, error);
                p.setChannelLevel (channel, 0.02f);
                p.setChannelDecay (channel, 0.0f);
                p.setChannelSustain (channel, 1.0f);
            }

            clearAllSteps (p);

            for (int channel = 0; channel < Layout::numChannels; ++channel)
                for (int step = 0; step < Layout::numSteps; ++step)
                {
                    p.setStep (channel, step, true);
                    p.setStepRatchet (channel, step, Layout::maxRatchetHits);
                }

            p.setMutate (0.0f);
            rearm (p);

            std::printf ("        %d lanes x %d strikes at every step = %d hits a bar, against"
                         " a pool of %d\n",
                         Layout::numChannels, Layout::maxRatchetHits,
                         Layout::numChannels * Layout::maxRatchetHits, Layout::maxVoices);

            check (Layout::numChannels * Layout::maxRatchetHits > Layout::maxVoices,
                   "the fixture is deliberately over the pool: one step of it alone wants more"
                   " voices than the engine owns, so the ceiling below is tested and not"
                   " admired",
                   "the load is only " + juce::String (Layout::numChannels * Layout::maxRatchetHits)
                       + " against " + juce::String (Layout::maxVoices));

            p.setPlaying (true);
            watch (p, (int) (exactStepSamples * Layout::numSteps * 2.0));
            p.setPlaying (false);

            std::printf ("        highest the pool ever got: %d voices\n", worstVoices);

            check (worstVoices == Layout::maxVoices,
                   "a fully rolled kit does fill the pool, which is the precondition for"
                   " calling the next line a ceiling",
                   "the pool peaked at " + juce::String (worstVoices) + ", expected to reach "
                       + juce::String (Layout::maxVoices));

            check (worstVoices <= Layout::maxVoices,
                   "and it never once asks for a voice the engine does not own, so a dense roll"
                   " steals rather than overrunning the array",
                   "the pool peaked at " + juce::String (worstVoices)
                       + " against a ceiling of " + juce::String (Layout::maxVoices));

            check (finite,
                   "and nothing went non-finite on the way, which is what a pool overrun would"
                   " leave behind",
                   "a fully rolled bar produced a non-finite sample");

            source.deleteFile();
        }

        //==================================================================
        // f. what the count can be, and where it is remembered
        //==================================================================
        std::printf ("\n    f. what the count can be, and what survives a reset\n");
        {
            GrooveboxProcessor p;
            p.prepareToPlay (sampleRate, blockSize);

            p.setStepRatchet (3, 7, 999);
            p.setStepRatchet (3, 8, -3);
            p.setStepRatchet (3, 9, 0);
            p.setStepRatchet (3, 10, 5);

            check (p.getStepRatchet (3, 7) == Layout::maxRatchetHits
                     && p.getStepRatchet (3, 8) == 1 && p.getStepRatchet (3, 9) == 1
                     && p.getStepRatchet (3, 10) == 5,
                   "the count is clamped at both ends rather than trusted: a thousand asks for"
                   " eight, and nothing above one asks for one",
                   "999, -3 and 0 came back as " + juce::String (p.getStepRatchet (3, 7)) + ", "
                       + juce::String (p.getStepRatchet (3, 8)) + " and "
                       + juce::String (p.getStepRatchet (3, 9)));

            /** The strike byte of one cell, straight out of the record the session carries. */
            const auto recordOf = [] (GrooveboxProcessor& proc)
            {
                return proc.getValueTreeState().state.getProperty (juce::Identifier ("pattern")).toString();
            };

            const auto storedStrikes = [] (const juce::String& payload, int channel, int step)
            {
                juce::MemoryBlock bytes;
                juce::MemoryOutputStream sink (bytes, false);
                juce::Base64::convertFromBase64 (sink, payload.fromFirstOccurrenceOf (":", false, false));
                sink.flush();

                const auto* at = static_cast<const juce::uint8*> (bytes.getData())
                                   + (size_t) ((channel * Layout::numSteps) + step) * 4;

                return (int) at[3];
            };

            const auto payload = recordOf (p);

            check (payload.startsWith ("v4:")
                     && storedStrikes (payload, 3, 7) == Layout::maxRatchetHits
                     && storedStrikes (payload, 3, 8) == 1,
                   "and the clamp is in the bytes the session carries, not only in the getter"
                   " that reads them back, so a record written by something that believed a"
                   " hundred was possible cannot get a hundred strikes out of one cell",
                   "the stored strike bytes for the clamped cells are "
                       + juce::String (storedStrikes (payload, 3, 7)) + " and "
                       + juce::String (storedStrikes (payload, 3, 8)) + " in a record tagged "
                       + payload.substring (0, 3));

            p.resetRatchets();

            bool allOne = true;

            for (int channel = 0; channel < Layout::numChannels && allOne; ++channel)
                for (int step = 0; step < Layout::numSteps; ++step)
                    if (p.getStepRatchet (channel, step) != 1)
                        allOne = false;

            check (allOne,
                   "the reset returns every cell to one, which is the value that means this"
                   " build never made a roll at all",
                   "after the reset at least one cell still holds a roll");

            // A reset only counts if the neutral value it wrote is the one the next save
            // carries. Otherwise the button would undo the screen and leave the file.
            GrooveboxProcessor restored;
            restored.prepareToPlay (sampleRate, blockSize);
            roundTrip (p, restored);

            bool storedOnes = true;

            for (int channel = 0; channel < Layout::numChannels && storedOnes; ++channel)
                for (int step = 0; step < Layout::numSteps; ++step)
                    if (restored.getStepRatchet (channel, step) != 1)
                        storedOnes = false;

            check (storedOnes,
                   "and a session saved after the reset opens with no rolls in it, so the"
                   " button is an edit and not a view",
                   "the reset did not reach the state tree: the restored grid still has a roll");

            // The one place the field is worth storing on its own: a cell that is switched
            // off, at full level and certain still carries a roll, so the count belongs to the
            // cell and not to the hit it is not making.
            p.setStep (5, 2, false);
            p.setStepRatchet (5, 2, 6);

            GrooveboxProcessor carried;
            carried.prepareToPlay (sampleRate, blockSize);
            roundTrip (p, carried);

            check (carried.getStepRatchet (5, 2) == 6 && ! carried.isStepOn (5, 2),
                   "and a switched-off step keeps the roll it was given, so the fourth field"
                   " survives on its own rather than only riding on a hit",
                   "an off step gave up its strikes on the round trip (reads "
                       + juce::String (carried.getStepRatchet (5, 2)) + ")");
        }
    }


    //======================================================================
    std::printf ("\n[34] a lane can be auditioned from the panel\n");
    {
        /**
            The claim this section is there to hold: what a click starts *is* what a key
            starts, sample for sample.

            5.3 moved the audition off the message thread and into the render walk, which
            is the only place the fade-in, the anti-alias band picked at note-on, the choke
            group, the voice stealing, the lane's own level and the trim are all already
            applied to a note. A preview reaching them through a second door would be a
            second sound engine — and the second engine is what the old direct call was: it
            started the voice from whatever thread the UI happened to be on, beside samples
            the render was already writing.

            So the arms ask whether the two doors are one door. The same samples for the
            same struck level. A sound in the block it was asked for, not the next. A ring
            that is bounded and says so instead of waiting. A lane that cannot sound
            starting nothing. A pattern already playing not being retimed by a click. And a
            preview in a choke group cutting it the way a note cuts it.
        */
        constexpr int auditionLane = 0;
        constexpr int otherLane = 1;
        const int auditionNote = Layout::firstMidiNote + auditionLane;

        /**
            One audition, either by click or by key, from a machine nobody has rendered yet.

            Two fresh processors rather than one rendered twice, for the reason [30]
            records: the trim moves through a 20 ms smoother, so a second pass on the same
            machine starts with the smoother already at target while the first does not, and
            the difference that comes back is the fixture climbing rather than the seam.
        */
        auto auditionedRender = [&] (GrooveboxProcessor& p, bool byKey, float velocity,
                                     int totalSamples, int queueAtBlock = 0)
        {
            RenderResult result;
            result.left.reserve ((size_t) totalSamples);
            result.right.reserve ((size_t) totalSamples);

            juce::AudioBuffer<float> buffer (2, blockSize);
            int block = 0;

            for (int rendered = 0; rendered < totalSamples; rendered += blockSize, ++block)
            {
                const auto thisBlock = juce::jmin (blockSize, totalSamples - rendered);

                buffer.setSize (2, thisBlock, false, true, true);
                buffer.clear();

                juce::MidiBuffer midi;

                if (block == queueAtBlock)
                {
                    if (byKey)
                        midi.addEvent (juce::MidiMessage::noteOn (1, auditionNote, velocity), 0);
                    else
                        p.previewChannel (auditionLane, velocity);
                }

                p.processBlock (buffer, midi);

                for (int i = 0; i < thisBlock; ++i)
                {
                    result.left.push_back (buffer.getSample (0, i));
                    result.right.push_back (buffer.getSample (1, i));
                }
            }

            return result;
        };

        /** A lane that can be auditioned: it holds a sample and nothing else is armed. */
        auto auditionReady = [] (GrooveboxProcessor& p, const juce::File& sample, int lane)
        {
            juce::String error;
            p.loadSampleIntoChannel (lane, sample, error);
            clearAllSteps (p);
            p.setPlaying (false);
            p.setMutate (0.0f);
            p.setHumanize (0.0f);
            p.setTouch (0.0f);
            return error.isEmpty();
        };

        //==================================================================
        // a. the click and the key are the same samples
        //==================================================================
        std::printf ("    a. an audition is the note it becomes\n");
        {
            const auto click = makeClickWav ("audition-identity", 4000, sampleRate);

            GrooveboxProcessor byClick;
            byClick.prepareToPlay (sampleRate, blockSize);
            const bool loaded = auditionReady (byClick, click, auditionLane);

            GrooveboxProcessor byKey;
            byKey.prepareToPlay (sampleRate, blockSize);
            auditionReady (byKey, click, auditionLane);

            const auto clicked = auditionedRender (byClick, false, 1.0f, 4096);
            const auto keyed = auditionedRender (byKey, true, 1.0f, 4096);

            // The precondition, printed either way: an identity between two renders that
            // are both silent is not an identity, it is a pair of zeros. [30]'s rule that a
            // gate's evidence has to appear on the green run too.
            const auto head = peakOf (clicked.left);

            check (loaded && head > 0.05f,
                   "an auditioned lane actually makes a sound, which is the precondition "
                   "every comparison below is measured against",
                   "peak " + juce::String (head, 6) + " from a click auditioned at full "
                   "velocity" + (loaded ? juce::String() : " (the sample refused to load)"));

            const auto sameLeft = firstDifference (clicked.left, keyed.left, 0, 4096);
            const auto sameRight = firstDifference (clicked.right, keyed.right, 0, 4096);

            check (sameLeft < 0 && sameRight < 0,
                   "a click and a key at the same struck level are the same samples on both "
                   "channels, so the audition has no second sound engine behind it",
                   sameLeft < 0 && sameRight < 0
                       ? "identical for 4096 samples, peak " + juce::String (head, 6)
                       : "first difference at sample " + juce::String (sameLeft < 0 ? sameRight : sameLeft));

            // What the queue costs: a level off the 7-bit lattice becomes the nearest
            // lattice value, which is the same thing a note-on does to the same number, so
            // the quantisation is not a price the click pays alone. Two more unused
            // processors for the same reason as the first pair: an arm that has already
            // rendered once starts with a smoother settled and a voice tail behind it, and
            // both of those belong to the fixture rather than to the seam.
            GrooveboxProcessor coarseP;
            coarseP.prepareToPlay (sampleRate, blockSize);
            auditionReady (coarseP, click, auditionLane);

            GrooveboxProcessor latticeP;
            latticeP.prepareToPlay (sampleRate, blockSize);
            auditionReady (latticeP, click, auditionLane);

            const auto coarse = auditionedRender (coarseP, false, 0.3f, 4096);
            const auto lattice = auditionedRender (latticeP, true, 38.0f / 127.0f, 4096);
            const auto coarseDiff = firstDifference (coarse.left, lattice.left, 0, 4096);

            check (coarseDiff < 0,
                   "and a struck level that no note-on could carry arrives as the nearest one "
                   "that can, which is what lets the two be compared bit for bit at all",
                   coarseDiff < 0 ? "0.3 auditioned is the 38/127 note, identical"
                                  : "differs at sample " + juce::String (coarseDiff));

            // And the same claim from the other side of a tie. 0.3 x 127 is 38.1, so a
            // quantiser that truncated, ceiled or simply copied the number it was handed
            // would still land on 38 here -- this pair is the one with a shape to it. Half
            // velocity is 63.5 exactly, which is the only ordinary value where rounding up
            // and cutting down disagree, and the disagreeing answer is not this file's
            // opinion: a real note-on at 0.5 stores 64 because `MidiMessage::noteOn` rounds
            // the same product, so the queue is held to the keyboard's own answer rather
            // than to a number typed next to it. Out of `roundToInt` the arm goes red;
            // towards `ceil` the 0.3 arm above goes red. Neither one alone sees both.
            GrooveboxProcessor tieP;
            tieP.prepareToPlay (sampleRate, blockSize);
            auditionReady (tieP, click, auditionLane);

            GrooveboxProcessor tieNoteP;
            tieNoteP.prepareToPlay (sampleRate, blockSize);
            auditionReady (tieNoteP, click, auditionLane);

            const auto tie = auditionedRender (tieP, false, 0.5f, 4096);
            const auto tieNote = auditionedRender (tieNoteP, true, 64.0f / 127.0f, 4096);
            const auto tieDiff = firstDifference (tie.left, tieNote.left, 0, 4096);

            check (tieDiff < 0,
                   "and half velocity arrives as the 64/127 a note-on stores rather than the "
                   "63/127 a truncating quantiser would cut, which is the side of the tie the "
                   "0.3 pair above cannot see",
                   tieDiff < 0 ? "0.5 auditioned is the 64/127 note, identical"
                               : "differs at sample " + juce::String (tieDiff));
        }

        //==================================================================
        // b. it sounds in the block it was asked for
        //==================================================================
        std::printf ("    b. the request is drained by the block that receives it\n");
        {
            const auto click = makeClickWav ("audition-block", 4000, sampleRate);

            GrooveboxProcessor p;
            p.prepareToPlay (sampleRate, blockSize);
            auditionReady (p, click, auditionLane);

            // Asked for at the top of block three: an audition drained in the block that
            // received it starts at sample 768, and one that waits a block starts at 1024.
            constexpr int queueBlock = 3;
            const auto askedLate = auditionedRender (p, false, 1.0f, 2048, queueBlock);
            const auto onset = onsetAfter (askedLate.left, 0);

            // The 32 samples of slack are the fade-in every voice gets, which is the only
            // thing that can move a start off the block boundary here. The block is 256
            // samples wide, so an arm that waits for the next drain lands two hundred and
            // twenty-four samples outside this window rather than just inside it.
            check (onset >= queueBlock * blockSize && onset < (queueBlock + 1) * blockSize,
                   "the hit starts inside the block the request arrived in, not the one after it",
                   "onset at " + juce::String (onset) + ", block " + juce::String (queueBlock)
                       + " spans " + juce::String (queueBlock * blockSize) + " to "
                       + juce::String ((queueBlock + 1) * blockSize - 1));

            check (askedLate.peakBetween (0, queueBlock * blockSize) == 0.0f,
                   "and nothing sounds before the ask, so what drained is the request that was "
                   "made and not one the machine was already holding",
                   "peak up to the ask "
                       + juce::String (askedLate.peakBetween (0, queueBlock * blockSize), 6));
        }

        //==================================================================
        // c. the ring is bounded, and says so instead of waiting
        //==================================================================
        std::printf ("    c. a full ring drops the audition rather than the caller\n");
        {
            const auto click = makeClickWav ("audition-ring", 4000, sampleRate);

            GrooveboxProcessor p;
            p.prepareToPlay (sampleRate, blockSize);
            auditionReady (p, click, auditionLane);

            // Asked here, on an empty ring, and that placement is the whole check: the
            // first version of this arm put it under the ceiling loop below, where the
            // answer is `false` because there is no room — so a build with the range guard
            // deleted passed it, and the mutation probe came back green. On an open ring
            // the only way to be refused is to be refused.
            check (! p.previewChannel (-1, 1.0f) && ! p.previewChannel (Layout::numChannels, 1.0f),
                   "a lane outside the twelve is refused before it can be queued, so the drain "
                   "never has an index to trust",
                   "an out-of-range lane was accepted");

            int accepted = 0;

            for (int i = 0; i < SequencerState::maxPreviewRequests * 3 / 2; ++i)
                if (p.previewChannel (auditionLane, 1.0f))
                    ++accepted;

            const auto offered = SequencerState::maxPreviewRequests * 3 / 2;

            check (accepted == SequencerState::maxPreviewRequests,
                   "exactly the ceiling is taken with nothing rendered in between, so the "
                   "producer is bounded by the ring and not by the caller's patience",
                   juce::String (accepted) + " of " + juce::String (offered) + " accepted, ceiling "
                       + juce::String (SequencerState::maxPreviewRequests));

            // One block drains the lot, and the room is free again — which is what makes
            // this a ring and not a counter that runs out.
            render (p, blockSize, blockSize, sampleRate);

            check (p.previewChannel (auditionLane, 1.0f),
                   "and after one block the ring has room again, so the ceiling is a queue and "
                   "not a lifetime budget",
                   "a request still refused after the drain");

            // What the drain reached: twelve lanes asked for one at a time have to start
            // twelve voices, which is the count the queue alone cannot fake by dropping
            // everything on the floor.
            GrooveboxProcessor all;
            all.prepareToPlay (sampleRate, blockSize);

            for (int lane = 0; lane < Layout::numChannels; ++lane)
                auditionReady (all, click, lane);

            for (int lane = 0; lane < Layout::numChannels; ++lane)
                all.previewChannel (lane, 1.0f);

            render (all, blockSize, blockSize, sampleRate);

            check (all.debugActiveVoices() == Layout::numChannels,
                   "twelve requests on twelve lanes become twelve voices in the block that "
                   "drains them, one per lane",
                   juce::String (all.debugActiveVoices()) + " voices sounding of "
                       + juce::String (Layout::numChannels));

            // ...and sixteen on one lane have to leave the machine in the state sixteen
            // notes on that lane leave it in. That is the honest shape of the claim: the
            // queue is not allowed a de-dupe the keyboard does not have, and it is not
            // allowed to invent voices the keyboard would refuse. What that state is --
            // one voice, or sixteen stacked on one lane -- belongs to the voice allocator,
            // and this arm deliberately does not say, because a number typed here would be
            // a second engine's behaviour being asserted as the click's.
            GrooveboxProcessor one;
            one.prepareToPlay (sampleRate, blockSize);
            auditionReady (one, click, auditionLane);

            for (int i = 0; i < SequencerState::maxPreviewRequests; ++i)
                one.previewChannel (auditionLane, 1.0f);

            render (one, blockSize, blockSize, sampleRate);

            GrooveboxProcessor keys;
            keys.prepareToPlay (sampleRate, blockSize);
            auditionReady (keys, click, auditionLane);

            {
                juce::AudioBuffer<float> buffer (2, blockSize);
                juce::MidiBuffer midi;

                buffer.clear();

                for (int i = 0; i < SequencerState::maxPreviewRequests; ++i)
                    midi.addEvent (juce::MidiMessage::noteOn (1, auditionNote, 1.0f), 0);

                keys.processBlock (buffer, midi);
            }

            check (one.debugActiveVoices() > 0 && one.debugActiveVoices() == keys.debugActiveVoices(),
                   "sixteen asks on one lane leave exactly the voices sixteen notes on that "
                   "lane leave, so the queue neither dedupes nor invents",
                   juce::String (one.debugActiveVoices()) + " clicked against "
                       + juce::String (keys.debugActiveVoices()) + " keyed");
        }

        //==================================================================
        // d. a lane that cannot sound starts nothing
        //==================================================================
        std::printf ("    d. mute and emptiness are obeyed, not bypassed\n");
        {
            const auto click = makeClickWav ("audition-muted", 4000, sampleRate);

            GrooveboxProcessor muted;
            muted.prepareToPlay (sampleRate, blockSize);
            auditionReady (muted, click, auditionLane);
            muted.setChannelMuted (auditionLane, true);

            const auto mutedPeak = peakOf (auditionedRender (muted, false, 1.0f, 1024).left);

            check (mutedPeak == 0.0f,
                   "a muted lane gives a click nothing: the audition goes through the same "
                   "gate a note goes through rather than around it",
                   "peak " + juce::String (mutedPeak, 6));

            GrooveboxProcessor emptyLane;
            emptyLane.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (emptyLane);
            emptyLane.setPlaying (false);

            // The request is still taken — the lane decides, not the queue — so this is a
            // check that the silence comes from having nothing to play, not from a refusal.
            const bool asked = emptyLane.previewChannel (auditionLane, 1.0f);
            const auto emptyPeak = peakOf (render (emptyLane, 1024, blockSize, sampleRate).left);

            check (asked && emptyPeak == 0.0f,
                   "and an empty lane stays silent to the last sample rather than playing a "
                   "half-decoded buffer",
                   juce::String (asked ? "queued, " : "refused, ") + "peak " + juce::String (emptyPeak, 6));
        }

        //==================================================================
        // e. a click does not retime a pattern that is playing
        //==================================================================
        std::printf ("    e. an audition does not move the grid\n");
        {
            // Short clicks on both lanes: a bar is rendered twice here and a four-thousand
            // sample source would still be ringing when the second pass began, which the
            // onset scan would read as a hit at sample nought.
            const auto click = makeClickWav ("audition-grid", 200, sampleRate);

            GrooveboxProcessor p;
            p.prepareToPlay (sampleRate, blockSize);
            auditionReady (p, click, otherLane);
            auditionReady (p, click, auditionLane);

            for (int step = 0; step < Layout::numSteps; step += 4)
            {
                p.setStep (otherLane, step, true);
                p.setStepVelocity (otherLane, step, 1.0f);
                p.setStepChance (otherLane, step, 1.0f);
            }

            /** Onsets in order, each searched for only after the last one has died away. */
            auto scan = [] (const std::vector<float>& data, int ringOut)
            {
                std::vector<int> found;
                int at = 0;

                while (true)
                {
                    const auto next = onsetAfter (data, at);

                    if (next < 0)
                        break;

                    found.push_back (next);
                    at = next + ringOut;
                }

                return found;
            };

            // Fifteen and a half steps: the bar has to stop before step sixteen fires, or
            // the next bar's step nought joins the onset list and the ruler counts five.
            // That is what the first run of this arm caught, and it is a fixture bug, not
            // an engine one -- which is why the count is stated as a precondition.
            const auto bar = (int) (exactStepSamples * (Layout::numSteps - 0.5));

            auto playBarClicked = [&] (int queueAt)
            {
                rearm (p);
                p.setPlaying (true);

                RenderResult result;
                juce::AudioBuffer<float> buffer (2, blockSize);
                int block = 0;

                for (int rendered = 0; rendered < bar; rendered += blockSize, ++block)
                {
                    const auto thisBlock = juce::jmin (blockSize, bar - rendered);

                    buffer.setSize (2, thisBlock, false, true, true);
                    buffer.clear();

                    juce::MidiBuffer midi;

                    if (block == queueAt)
                        p.previewChannel (auditionLane, 1.0f);

                    p.processBlock (buffer, midi);

                    for (int i = 0; i < thisBlock; ++i)
                        result.left.push_back (buffer.getSample (0, i));
                }

                p.setPlaying (false);
                return result;
            };

            const auto plain = scan (playBarClicked (-1).left, 120);
            const auto clicked = scan (playBarClicked (5).left, 120);

            // The ruler first, and it is a precondition rather than a finding: four armed
            // steps at quarter notes have to give four onsets, or there is nothing for the
            // next two checks to say stayed where it was.
            check ((int) plain.size() == 4,
                   "four steps armed at quarter notes give four onsets with nothing clicked, "
                   "which is the ruler this arm measures against",
                   juce::String ((int) plain.size()) + " onsets in an unclicked bar");

            // The one insertion this arm expects is at index one: the ask lands at the top
            // of block five, and the grid onsets either side of it are step nought at sample
            // one and step four at twenty-two thousand. So compare by dropping that element
            // and requiring the rest to be the unclicked bar's list, item for item -- which
            // says both halves at once, that nothing moved and that only one thing was added.
            //
            // The drop is `i < insertAt`, not `i <=`: `insertAt` indexes the *clicked* list,
            // so it names the element that has no counterpart in the plain one. Asking
            // whether plain[insertAt] equals clicked[insertAt] compares the first grid onset
            // after the hole against the audition itself, which is a check that cannot pass
            // when the thing it gates has worked -- it went red on a correct build, and the
            // failure line was the proof of the insertion rather than of a moved onset.
            constexpr int insertAt = 1;

            bool moved = (int) clicked.size() != (int) plain.size() + 1;

            for (size_t i = 0; i < plain.size() && ! moved; ++i)
            {
                const size_t from = i < (size_t) insertAt ? i : i + 1;

                if (from >= clicked.size() || clicked[from] != plain[i])
                    moved = true;
            }

            auto listed = [] (const std::vector<int>& values)
            {
                juce::String text;

                for (auto value : values)
                    text << value << " ";

                return text;
            };

            check (! moved,
                   "and every pattern onset is on the same sample it was on before the click, "
                   "so an audition is a note laid on top of the grid rather than a second clock",
                   moved ? "grid " + listed (plain) + "| clicked " + listed (clicked)
                         : "grid onsets " + listed (plain) + "all unmoved around sample "
                             + juce::String (clicked[(size_t) insertAt]));

            check (! clicked.empty() && clicked[(size_t) insertAt] / blockSize == 5,
                   "and the extra hit is inside the block that was asked for it, in the middle "
                   "of a bar that was already running",
                   "onset " + (clicked.empty() ? juce::String ("none")
                       : juce::String (clicked[(size_t) insertAt]) + " sits in block "
                           + juce::String (clicked[(size_t) insertAt] / blockSize)
                             + ", asked for in block 5"));
        }

        //==================================================================
        // f. a preview in a choke group cuts it like a note
        //==================================================================
        std::printf ("    f. an audition chokes the way a note chokes\n");
        {
            // A tone for the lane being cut, because the question is whether something is
            // still sounding eight blocks in. A click has died by then in both arms, and a
            // ratio of two silences is a check that cannot go red.
            const auto heldSource = makeToneWav ("audition-choke-held", 200.0, 4000, sampleRate);
            const auto cutSource = makeClickWav ("audition-choke-cut", 200, sampleRate);

            GrooveboxProcessor held;
            held.prepareToPlay (sampleRate, blockSize);
            auditionReady (held, heldSource, otherLane);
            auditionReady (held, cutSource, auditionLane);

            GrooveboxProcessor choked;
            choked.prepareToPlay (sampleRate, blockSize);
            auditionReady (choked, heldSource, otherLane);
            auditionReady (choked, cutSource, auditionLane);

            for (auto* p : { &held, &choked })
            {
                p->setChannelChokeGroup (auditionLane, 1);
                p->setChannelChokeGroup (otherLane, 1);
            }

            /** Lane one auditioned in block nought; lane zero joins in block one when choking. */
            auto tailPeak = [&] (GrooveboxProcessor& p, bool choke)
            {
                RenderResult result;
                juce::AudioBuffer<float> buffer (2, blockSize);
                int block = 0;

                for (int rendered = 0; rendered < 4096; rendered += blockSize, ++block)
                {
                    const auto thisBlock = juce::jmin (blockSize, 4096 - rendered);

                    buffer.setSize (2, thisBlock, false, true, true);
                    buffer.clear();

                    juce::MidiBuffer midi;

                    if (block == 0)
                        p.previewChannel (otherLane, 1.0f);

                    if (block == 1 && choke)
                        p.previewChannel (auditionLane, 1.0f);

                    p.processBlock (buffer, midi);

                    for (int i = 0; i < thisBlock; ++i)
                        result.left.push_back (buffer.getSample (0, i));
                }

                // The choke fade is Layout::chokeFadeSeconds — twenty milliseconds — and the
                // cut lands at sample 256, so by 2048 the ramp is more than four times its
                // own length behind it. The cutting lane's own voice is a two hundred sample
                // click, gone by 456: whatever is loud in this window is the held lane.
                return result.peakBetween (2048, 2560);
            };

            const auto heldPeak = tailPeak (held, false);
            const auto chokedPeak = tailPeak (choked, true);

            check (heldPeak > 0.05f,
                   "the lane that is meant to be cut is still loud eight blocks in, which is "
                   "the precondition the ratio below needs to mean anything",
                   "held tail peak " + juce::String (heldPeak, 6));

            check (chokedPeak < heldPeak * 0.25f,
                   "a click on the other member of the group takes the ramp out, so auditioning "
                   "an open hat stops the one that was already ringing",
                   "tail peak " + juce::String (chokedPeak, 6) + " against "
                       + juce::String (heldPeak, 6) + " held");

            check (chokedPeak < 0.005f,
                   "and it goes to the floor rather than to a residual that only looks like a "
                   "cut, which is the difference between a choke and a quieter hit",
                   "choked tail peak " + juce::String (chokedPeak, 6));
        }
    }

    //======================================================================
    std::printf ("\n[35] a whole pattern can be written into one slot in one push\n");
    {
        /**
            What this seam exists for: a generated grid arrives as twelve rows at once, aims at
            a slot the user is not looking at, and has to be one edit: one record in the
            session, one pass through the codec, one thing said to the host.

            The cell seams cannot serve that shape. `setStep`, `setStepVelocity`, `setStepChance`
            and `setStepRatchet` each resolve `selectedSlotOf (channel)` for themselves, so they
            can only paint the slot a lane is already playing, and each ends in
            `pushPatternToState`, which re-serialises the whole row. Twelve lanes by sixteen
            steps by four fields through those doors is 768 encodings of the same 768 bytes for
            one pattern — 240 of them move the record, and part d counts both — and the half
            the plan was written in is audible on the way there because the slot being filled is
            the slot being heard.

            So the checks below are about four claims: a refusal is only ever about the shape of
            the ask and leaves nothing behind; an accepted fill lands where it was aimed and
            nowhere else, including not at the selection; it costs one serialisation rather than
            240 notifications' worth of them, counted as the notifications a state listener
            receives and as the changes the one open transaction holds; and the record
            it writes obeys the bank's own sparseness rule, which is the same rule the dots use.
        */
        auto click = makeClickWav ("slotwrite", 4000, sampleRate);

        /** Every property the root holds, as sorted `name=value` pairs. */
        auto treeCensus = [] (GrooveboxProcessor& p)
        {
            juce::StringArray lines;
            const auto& state = p.getValueTreeState().state;

            for (auto i = 0; i < state.getNumProperties(); ++i)
            {
                const auto name = state.getPropertyName (i);
                lines.add (name.toString() + "=" + state.getProperty (name).toString());
            }

            lines.sort (false);

            return lines.joinIntoString (";");
        };

        /**
            The slot's own record, read by the name production gives that slot.

            A census-and-index read is what [32] uses, and it is right to there: the claim
            is about the four names, and indexing the present properties is how a wrong
            grammar gets seen. Here the claim is usually about a slot that is *absent* —
            that is the bank's sparseness rule, and it is the subject of parts c and e — and
            an absent slot shifts every later name down one index, so `names[2]` is slot 1's
            record in a tree that holds three. A name taken from `slotPropertyName` cannot
            fail that way, and it is not an unfalsifiable restatement either: the census
            below is a literal list of what the tree holds, checked as a string in parts a
            and c, so a build that renamed its slots would go red there rather than agreeing
            with itself in both directions. An absent slot comes back as an empty string.
        */
        auto slotRecord = [] (GrooveboxProcessor& p, int slot) -> juce::String
        {
            return p.getValueTreeState().state
                .getProperty (juce::Identifier (p.slotPropertyName (slot))).toString();
        };

        /**
            The `pattern*` names the tree actually holds, sorted.

            Kept as a literal census beside the lookup above on purpose: if a future build
            renamed the slots, `slotPropertyName` would be wrong on both sides of a save and
            every read here would still agree with it. This is the one list that cannot agree
            with a mistake, and the two checks that use it state the names as strings.
        */
        auto recordCensus = [] (GrooveboxProcessor& p)
        {
            juce::StringArray names;
            const auto& state = p.getValueTreeState().state;

            for (auto i = 0; i < state.getNumProperties(); ++i)
            {
                const auto name = state.getPropertyName (i).toString();
                if (name.startsWith ("pattern"))
                    names.add (name);
            }

            names.sort (false);
            return names;
        };

        /** One cell as the record holds it, decoded with test-local arithmetic. */
        struct StoredCell
        {
            int on = -1, level = -1, chance = -1, strikes = -1;
            int bytes = 0;
        };

        auto storedCell = [] (const juce::String& record, int channel, int step) -> StoredCell
        {
            StoredCell cell;
            const int stride = 4;

            if (! record.startsWith ("v4:"))
                return cell;

            juce::MemoryBlock bytes;
            juce::MemoryOutputStream sink (bytes, false);

            if (! juce::Base64::convertFromBase64 (sink, record.fromFirstOccurrenceOf (":", false, false)))
                return cell;

            // Flush before the size and the pointer: the stream rounds the block up as it
            // grows and only shrinks it to what was written in `flush` (which is exactly
            // `trimExternalBlockSize`, and it can move the data pointer).
            sink.flush();

            const auto at = (channel * Layout::numSteps + step) * stride;
            if (bytes.getSize() < (size_t) (at + stride))
                return cell;

            const auto* raw = static_cast<const juce::uint8*> (bytes.getData());
            cell.on = raw[at];
            cell.level = raw[at + 1];
            cell.chance = raw[at + 2];
            cell.strikes = raw[at + 3];
            cell.bytes = (int) bytes.getSize();
            return cell;
        };

        auto row = [] (bool hit)
        {
            GrooveboxProcessor::SlotRowWrite r;
            for (auto s = 0; s < Layout::numSteps; ++s)
                r.on[(size_t) s] = hit;
            return r;
        };

        auto liveGrid = [] (GrooveboxProcessor& p)
        {
            juce::String out;
            for (auto c = 0; c < Layout::numChannels; ++c)
                for (auto s = 0; s < Layout::numSteps; ++s)
                    out += juce::String (" ") + juce::String ((int) p.isStepOn (c, s))
                         + juce::String (Layout::levelByte (p.getStepVelocity (c, s)))
                         + juce::String (Layout::chanceByte (p.getStepChance (c, s)))
                         + juce::String (p.getStepRatchet (c, s)) + "|";
            return out;
        };

        auto selections = [] (GrooveboxProcessor& p)
        {
            juce::String out;
            for (auto c = 0; c < Layout::numChannels; ++c)
                out += juce::String (p.getChannelSlot (c));
            return out;
        };

        // Counts what a host or the panel is told about the tree, which is the currency the
        // economy claim is actually about.
        struct TreeCounter : juce::ValueTree::Listener
        {
            int changes = 0;
            void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override
            {
                ++changes;
            }
        };

        GrooveboxProcessor machine;
        machine.prepareToPlay (sampleRate, blockSize);
        clearAllSteps (machine);
        rearm (machine);

        for (auto c = 0; c < Layout::numChannels; ++c)
        {
            juce::String error;
            machine.loadSampleIntoChannel (c, click, error);
        }

        TreeCounter counter;
        machine.getValueTreeState().state.addListener (&counter);

        int repaints = 0;
        machine.onStateChanged = [&repaints] { ++repaints; };

        //======================================================================
        // a. the shape of the bank this build writes
        {
            // The fixture every later part reads its absence from: a session that has
            // never touched a second slot carries one record, not four. Without this,
            // "the neutral fill removed slot 2's record" would be an absence compared
            // against an absence — which is not the same trap as [32]'s positional
            // census, gated there on all four names being present.
            check (recordCensus (machine).joinIntoString (" ") == "pattern",
                   "a fresh session holds slot 0's record under that literal name and nothing "
                   "else, which is what makes a later emptiness a removal rather than a never-was",
                   "records on the tree: " + recordCensus (machine).joinIntoString (" "));

            check (selections (machine) == "000000000000" && slotRecord (machine, 2).isEmpty(),
                   "the fixture starts where a fresh session starts: every lane on slot 0, and "
                   "slot 2 holding nothing a save would bother to write",
                   "selections " + selections (machine) + ", slot 2 record length "
                       + juce::String (slotRecord (machine, 2).length()));

            check (! slotRecord (machine, 0).isEmpty(),
                   "and the same helper finds the record that is there, so the emptiness above "
                    "is the helper reading a real tree rather than never matching anything",
                   "slot 0 record length " + juce::String (slotRecord (machine, 0).length()));
        }

        //======================================================================
        // b. every way to be refused, asked while nothing else could say no
        {
            GrooveboxProcessor refuser;
            refuser.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (refuser);
            rearm (refuser);

            for (auto c = 0; c < Layout::numChannels; ++c)
            {
                juce::String error;
                refuser.loadSampleIntoChannel (c, click, error);
                refuser.setChannelSlot (c, 2);
                refuser.setStep (c, c, true);
                refuser.setStepVelocity (c, c, 0.5f);
            }

            int refusedRepaints = 0;
            refuser.onStateChanged = [&refusedRepaints] { ++refusedRepaints; };

            const auto beforeGrid = liveGrid (refuser);
            const auto beforeCensus = treeCensus (refuser);

            const int oneLane = 3;
            const int badLaneList[] = { 3, Layout::numChannels };
            const int twelveLanes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
            GrooveboxProcessor::SlotRowWrite rows [13];

            /**
                The refused asks have to be *shaped* before they are refused. A default row is
                every cell off, and every cell off is exactly what this grid already holds at
                step 0, so a store that landed ahead of the pre-scan would have written the
                value that was already there — the check below would then read its own fixture
                rather than the write, and stay green on the build it exists to catch. Step 0
                of every row is therefore asked for ON, at a level the grid does not hold: the
                grid at lane 3, step 0 is off and at full, and that difference is the control.
            */
            for (auto& r : rows)
            {
                r.on[0] = true;
                r.level[0] = 0.25f;
                r.strikes[0] = 2;
            }

            // The precondition next to the arms that depend on it: if the ask and the grid
            // already agree at the cell a leak would touch, then every "a refusal leaves
            // nothing behind" below is satisfied by the fixture and not by the code, and the
            // seven of them mean nothing. This line is the difference the controls spend.
            check (rows[0].on[0] && rows[0].level[0] != 1.0f && rows[0].strikes[0] != 1
                       && ! refuser.isStepOn (3, 0) && refuser.isStepOn (3, 3),
                   "the refused ask disagrees with the grid it aims at in the flag, the level and"
                   " the strike count, so a store made before the pre-scan cannot hide in equality",
                   "lane 3 holds step 0 at " + juce::String ((int) refuser.isStepOn (3, 0))
                       + " while the ask says 1, and holds step 3 at "
                       + juce::String ((int) refuser.isStepOn (3, 3)));

            struct Refusal
            {
                const char* what;
                int slot;
                const int* lanes;
                int count;
                bool nullRows = false;
            };

            const Refusal refusals[] =
            {
                { "slot below the bank", -1, &oneLane, 1 },
                { "slot above the bank", Layout::numPatternSlots, &oneLane, 1 },
                { "no row at all", 2, &oneLane, 1, true },
                { "null lane list", 2, nullptr, 1 },
                { "an empty lane list", 2, twelveLanes, 0 },
                { "more lanes than the machine has", 2, twelveLanes, Layout::numChannels + 1 },
                { "a lane outside the twelve", 2, badLaneList, 2 },
            };

            for (const auto& r : refusals)
            {
                const auto rowsPtr = (r.lanes == nullptr || r.nullRows) ? nullptr : rows;
                const auto answered = refuser.writeSlotRows (r.slot, r.lanes, rowsPtr, r.count);

                check (! answered,
                        juce::String ("a write refuses ") + r.what
                            + ", which is the only kind of reason this seam is allowed to give",
                        juce::String (r.what) + " came back " + (answered ? "accepted" : "refused"));

                check (liveGrid (refuser) == beforeGrid,
                        juce::String ("and the refusal leaves every cell of the slot it was aimed at "
                                      "as it found it (") + r.what + ")",
                        "the grid moved, which means the list was written before it was checked");

                check (treeCensus (refuser) == beforeCensus,
                        juce::String ("and a refusal writes no record at all (") + r.what + ")",
                        "the tree changed on a write that said no");
            }

            check (refusedRepaints == 0,
                   "seven refusals cost the panel nothing: the repaint is the fill's, not the ask's",
                   "repaints after the refusal sweep: " + juce::String (refusedRepaints));

            // The bad lane sat second in the list, so this is the arm that says the whole list
            // is checked before the first store rather than element by element.
            check (refuser.isStepOn (3, 3) && ! refuser.isStepOn (3, 0),
                   "the lane named first in a refused list is still holding its own grid",
                   "lane 3 step 3 is " + juce::String ((int) refuser.isStepOn (3, 3))
                       + ", which would be 0 if the fill had started before it was refused");
        }

        //======================================================================
        // c. an accepted fill lands where it was aimed, and nowhere else
        {
            for (auto c = 0; c < Layout::numChannels; ++c)
                machine.setStep (c, c, true);

            // Rearm *before* the counters are zeroed: `resetTransport` releases and
            // re-prepares the engine, and if it ever wrote state the fill's single
            // change below would be reading its own fixture.
            rearm (machine);
            machine.setPlaying (true);

            counter.changes = 0;
            repaints = 0;
            const auto censusBefore = treeCensus (machine);
            const auto gridBefore = liveGrid (machine);
            const auto untouched = render (machine, stepSamples * 32, blockSize, sampleRate);

            // Two silences are bit-identical too, so the equality below is only a control
            // if this render has something in it.
            check (lastAbove (untouched.left, 0.005f) >= 0,
                   "the grid the fixture plays actually sounds, which is what makes 'the fill "
                   "changed nothing audible' a comparison rather than a pair of absences",
                   "the 32-step render of the selected slots is silence");

            int lanes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
            GrooveboxProcessor::SlotRowWrite full [Layout::numChannels];
            for (auto c = 0; c < Layout::numChannels; ++c)
                for (auto s = 0; s < Layout::numSteps; ++s)
                {
                    full[(size_t) c].on[(size_t) s] = (s % (c + 1)) == 0;
                    full[(size_t) c].level[(size_t) s] = 0.25f + 0.05f * (float) c;
                    full[(size_t) c].strikes[(size_t) s] = (c % Layout::maxRatchetHits) + 1;
                }

            const auto filled = machine.writeSlotRows (2, lanes, full, Layout::numChannels);

            check (filled,
                   "a twelve-lane fill of a slot the bank has is accepted",
                   "writeSlotRows (2, all twelve lanes) said no");

            check (counter.changes == 1,
                   "one fill of the whole grid is one property change on the tree, which is the "
                   "number the cell seams would have paid sixteen times over for the same row",
                   "state changes heard: " + juce::String (counter.changes));

            check (repaints == 1,
                   "and the panel is told once, because a generated grid is one edit",
                   "repaints: " + juce::String (repaints));

            const auto record2 = slotRecord (machine, 2);
            check (! record2.isEmpty() && record2.startsWith ("v4:"),
                   "the slot now holds a v4 record of its own",
                   "record length " + juce::String (record2.length()));

            // The hole that makes the census-and-index route unsafe, stated as a literal:
            // slot 2 written while slot 1 was never touched leaves two records, and the
            // second name in sorted order is `pattern2`, not slot 2's index in a list of
            // what happens to exist. `slotRecord` reads the name production gives the slot,
            // and this is what says that name is the one the file is supposed to have.
            check (recordCensus (machine).joinIntoString (" ") == "pattern pattern2",
                   "a bank with only slot 2 filled names its records `pattern` and `pattern2`, "
                   "so the lookup above cannot be a second copy of a wrong grammar",
                   "records on the tree: " + recordCensus (machine).joinIntoString (" "));

            int wrongCells = 0;
            for (auto c = 0; c < Layout::numChannels; ++c)
                for (auto s = 0; s < Layout::numSteps; ++s)
                {
                    const auto cell = storedCell (record2, c, s);
                    if (cell.on != (full[(size_t) c].on[(size_t) s] ? 1 : 0)
                        || cell.level != Layout::levelByte (full[(size_t) c].level[(size_t) s])
                        || cell.chance != Layout::chanceByte (1.0f)
                        || cell.strikes != full[(size_t) c].strikes[(size_t) s])
                        ++wrongCells;
                }

            check (wrongCells == 0,
                   "every one of the 192 cells the fill asked for is in the record, with the "
                   "neutral chance and the strike count it was given",
                   juce::String (wrongCells) + " of 192 cells disagree with the ask");

            check (storedCell (record2, 0, 0).bytes == Layout::numChannels * Layout::numSteps * 4,
                   "and it is a whole-grid record, not a row: the bank writes all twelve lanes "
                   "or none, which is what makes an empty slot removable",
                   "payload bytes " + juce::String (storedCell (record2, 0, 0).bytes));

            check (selections (machine) == "000000000000",
                   "filling slot 2 moved no lane's pointer, which is the half of the claim that "
                   "makes a generated grid non-destructive",
                   "selections after the fill: " + selections (machine));

            check (liveGrid (machine) == gridBefore,
                   "and the grid the user is looking at — slot 0, the slot every lane points at — "
                   "is exactly the one it was",
                   "the slot being edited changed when a different slot was written");

            check (treeCensus (machine) != censusBefore,
                   "the tree did change, so the record above is not a comparison of two absences",
                   "census identical, which means nothing was serialised");

            rearm (machine);
            machine.setPlaying (true);
            const auto after = render (machine, stepSamples * 32, blockSize, sampleRate);
            check (after.left == untouched.left && after.right == untouched.right,
                   "and nothing sounded different: a fill of a slot no lane points at is silent "
                   "while it is being built, sample for sample",
                   "the two renders diverge at sample "
                       + juce::String (firstDifference (untouched.left, after.left, 0,
                                                       (int) untouched.left.size())));
        }

        //======================================================================
        // d. what the one push bought, measured against the door a drag uses
        {
            GrooveboxProcessor seams;
            seams.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (seams);

            for (auto c = 0; c < Layout::numChannels; ++c)
                seams.setChannelSlot (c, 3);

            TreeCounter seamCounter;
            seams.getValueTreeState().state.addListener (&seamCounter);
            seams.getUndoManager().clearUndoHistory();

            for (auto c = 0; c < Layout::numChannels; ++c)
                for (auto s = 0; s < Layout::numSteps; ++s)
                {
                    seams.setStep (c, s, s % 4 == 0);
                    seams.setStepVelocity (c, s, 0.5f);
                    seams.setStepChance (c, s, 1.0f);
                    seams.setStepRatchet (c, s, 1);
                }

            GrooveboxProcessor bulk;
            bulk.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (bulk);

            for (auto c = 0; c < Layout::numChannels; ++c)
                bulk.setChannelSlot (c, 3);

            TreeCounter bulkCounter;
            bulk.getValueTreeState().state.addListener (&bulkCounter);
            bulk.getUndoManager().clearUndoHistory();

            int lanes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
            GrooveboxProcessor::SlotRowWrite same [Layout::numChannels];
            for (auto c = 0; c < Layout::numChannels; ++c)
                for (auto s = 0; s < Layout::numSteps; ++s)
                {
                    same[(size_t) c].on[(size_t) s] = (s % 4 == 0);
                    same[(size_t) c].level[(size_t) s] = 0.5f;
                }

            const auto wrote = bulk.writeSlotRows (3, lanes, same, Layout::numChannels);

            check (wrote && bulkCounter.changes == 1,
                   "the same grid through the bulk seam is one property change",
                   "changes " + juce::String (bulkCounter.changes));

            /**
                The measured price of the same grid through the four cell seams.

                192 level writes all move a byte (every cell starts at full), and of the 192
                flag writes only the 48 that turn a step on do — the other 144 ask for the off
                the cell already holds. The chance and ratchet writes ask for 1.0 and 1, which
                is what `PatternCells` starts at, so they never move the record either. A
                `ValueTree::setProperty` of a value the property already has sends no
                notification, and the listener counts notifications, not calls: 192 + 48 = 240.

                What that number does *not* cover is the CPU, and it is the bigger half. Each
                of the 768 seam calls ends in `pushPatternToState`, with no early return for an
                unchanged cell (`setStep`, `setStepVelocity`, `setStepChance`, `setStepRatchet`
                at PluginProcessor.cpp:374, 443, 482, 533), so the row is encoded to base64 768
                times — 589,824 bytes of payload through the codec — to arrive at one record.
                The bulk seam encodes it once. The notification count is what a host and the
                panel feel; the encode count is what the audio-thread-facing code does not,
                because none of this runs on that thread.
            */
            check (seamCounter.changes == Layout::numChannels * Layout::numSteps
                                     + Layout::numChannels * 4,
                   "the same grid through the cell seams is 240 property changes — the 192 level "
                   "moves and the 48 flags that actually flip — which is what 'pushes once per "
                   "slot' is paying for",
                   "cell-seam changes " + juce::String (seamCounter.changes)
                       + ", bulk " + juce::String (bulkCounter.changes));

            check (slotRecord (seams, 3) == slotRecord (bulk, 3),
                   "and the two records agree, so the economy is not bought by writing less",
                   "the bulk record differs from the drag record for the same ask");

            // The undo half, measured rather than assumed — and the assumption does not
            // survive. Nothing in the processor begins a transaction; that is the editor's,
            // once per gesture (StepGridComponent.cpp:935 "Paint steps"), so a dragged grid
            // is already one undo. And `UndoManager::perform` coalesces a new action with
            // the last one in the same transaction when `createCoalescedAction` says they
            // are the same property — which `SetPropertyAction` does for every pair of
            // `pattern3` writes that are not an add or a remove. So 240 writes to one
            // property land as two actions: the add that created the record, then everything
            // after it merged. The fill is one, because it only ever adds.
            //
            // Which means the seam does not save the undo history in any way worth claiming.
            // What it saves is the 768 encodings and the 240 notifications, and this check
            // exists to stop a future comment from restating the cheaper-sounding story.
            const auto seamActions = seams.getUndoManager().getNumActionsInCurrentTransaction();
            const auto bulkActions = bulk.getUndoManager().getNumActionsInCurrentTransaction();

            check (seamActions == 2 && bulkActions == 1,
                   "the dragged grid's 240 writes are two undo actions and the fill's one write "
                   "is one, because the undo manager coalesces a property with itself",
                   "actions " + juce::String (seamActions) + " against " + juce::String (bulkActions));

            check (seamActions < seamCounter.changes,
                   "and the action count is not the notification count, which is the claim this "
                   "section's own summary had to drop once it was measured",
                   "actions " + juce::String (seamActions)
                       + " against changes " + juce::String (seamCounter.changes));

            const auto seamsBack = seams.getUndoManager().undo()
                                     && slotRecord (seams, 3).isEmpty();
            const auto bulkBack = bulk.getUndoManager().undo()
                                    && slotRecord (bulk, 3).isEmpty();

            check (seamsBack && bulkBack,
                   "and one undo restores both to the absent record they came from: the merged "
                   "action puts back what the add left, then the add's own undo removes the "
                   "property, which is the pair the coalescing has to keep straight",
                   "seams " + juce::String ((int) seamsBack) + ", bulk " + juce::String ((int) bulkBack));
        }

        //======================================================================
        // e. an empty slot still costs no record, in either direction
        {
            int lanes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
            GrooveboxProcessor::SlotRowWrite neutral [Layout::numChannels];

            check (! slotRecord (machine, 2).isEmpty(),
                   "the precondition the removal needs: slot 2 holds something before the "
                   "neutral fill takes it away",
                   "slot 2 was already empty, so the absence below would prove nothing");

            machine.writeSlotRows (2, lanes, neutral, Layout::numChannels);

            check (slotRecord (machine, 2).isEmpty(),
                   "a fill that leaves every lane at off, full, every-time and one strike "
                   "removes the slot's record, exactly as clearing a row does",
                   "record length " + juce::String (slotRecord (machine, 2).length()));

            int dotsLit = 0;
            for (auto c = 0; c < Layout::numChannels; ++c)
                if (machine.isChannelSlotFilled (c, 2))
                    ++dotsLit;

            check (dotsLit == 0,
                   "and the bank's marks agree with the record rather than with the gesture that "
                   "made it, which is the pairing 4.4 had to learn the hard way",
                   juce::String (dotsLit) + " lanes still say slot 2 holds something");

            GrooveboxProcessor zero;
            zero.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (zero);

            for (auto c = 0; c < Layout::numChannels; ++c)
            {
                int one[] = { c };
                GrooveboxProcessor::SlotRowWrite neutralOne;
                zero.writeSlotRows (0, one, &neutralOne, 1);
            }

            const auto record0 = slotRecord (zero, 0);
            check (! record0.isEmpty(),
                   "slot 0 is exempt, as the save path says it is: a neutral fill there still "
                   "writes the all-zeros record an empty machine has always saved",
                   "slot 0's record vanished");

            bool everyCellNeutral = true;
            for (auto c = 0; c < Layout::numChannels && everyCellNeutral; ++c)
                for (auto s = 0; s < Layout::numSteps; ++s)
                {
                    const auto cell = storedCell (record0, c, s);
                    if (cell.on != 0 || cell.level != 127 || cell.chance != 255 || cell.strikes != 1)
                    {
                        everyCellNeutral = false;
                        break;
                    }
                }

            check (everyCellNeutral,
                   "and it says nothing in every one of its 192 cells, which is what the neutral "
                   "defaults of a row write are there to make true",
                   "a neutral fill left a byte behind in slot 0");
        }

        //======================================================================
        // f. it sounds when a lane is pointed at it, and only then
        {
            GrooveboxProcessor loud;
            loud.prepareToPlay (sampleRate, blockSize);
            clearAllSteps (loud);

            juce::String error;
            const auto loaded = loud.loadSampleIntoChannel (3, click, error);

            check (click.existsAsFile() && loaded,
                   "the lane this part sounds has a sample in it, which is the precondition every "
                   "render below rests on", error.isNotEmpty() ? error : "the wav is not on disk");

            // One hit in the slot the lane *does* point at. Without it the two unpointed
            // renders are two silences, and "the fill changed nothing" would be an equality
            // between absences — which is the trap [32]'s census was written against.
            loud.setStep (3, 2, true);
            rearm (loud);
            loud.setPlaying (true);

            int lane3[] = { 3 };
            GrooveboxProcessor::SlotRowWrite fourOnTheFloor;
            fourOnTheFloor.on[0] = true;
            fourOnTheFloor.on[4] = true;
            fourOnTheFloor.on[8] = true;
            fourOnTheFloor.on[12] = true;

            const auto before = render (loud, stepSamples * 16, blockSize, sampleRate);

            check (lastAbove (before.left, 0.005f) >= 0,
                   "the grid the lane is pointing at plays, so the identity below is a control",
                   "the pointed-at grid renders silence");

            const auto acceptedFill = loud.writeSlotRows (2, lane3, &fourOnTheFloor, 1);
            rearm (loud);
            loud.setPlaying (true);
            const auto whileUnselected = render (loud, stepSamples * 16, blockSize, sampleRate);

            check (acceptedFill,
                   "the one-lane fill of slot 2 is accepted",
                   "writeSlotRows (2, one lane) said no");

            check (firstDifference (before.left, whileUnselected.left, 0, (int) before.left.size()) == -1,
                   "a fill of the slot nobody points at changes nothing audible, even on a lane "
                   "that holds a sample and is armed",
                   "samples differ at "
                       + juce::String (firstDifference (before.left, whileUnselected.left, 0,
                                                       (int) before.left.size())));

            loud.setChannelSlot (3, 2);
            rearm (loud);
            loud.setPlaying (true);
            const auto armed = render (loud, stepSamples * 17, blockSize, sampleRate);

            std::vector<int> hits;
            for (auto at = 0; at >= 0 && at < (int) armed.left.size(); )
            {
                const auto onset = onsetAfter (armed.left, at);
                if (onset < 0)
                    break;
                hits.push_back ((int) std::lround ((double) onset / exactStepSamples));
                at = onset + 300;
            }

            // The render is one step longer than a bar, so step 16 is the *next* bar's step
            // 0 — the pattern repeating, not a cell firing twice. Stated in the expectation
            // rather than trimmed out of it, because the fifth entry is the evidence that
            // the fill is on the same grid the transport is on.
            check (hits == std::vector<int> { 0, 4, 8, 12, 16 },
                   "pointing the lane at the filled slot sounds exactly the grid the fill asked "
                   "for, through the render path and not a second door, with the bar's wrap at "
                   "16 rather than a fifth cell",
                   "hits at steps " + [&] { juce::StringArray l; for (auto h : hits) l.add (juce::String (h));
                                            return l.joinIntoString (" "); } ());

            check (firstDifference (before.left, armed.left, 0, (int) before.left.size()) != -1,
                   "and the two arms disagree, which is what says the render resolved the slot "
                   "rather than the fixture being silent throughout",
                   "the pointed and unpointed renders are identical");

            int lane7[] = { 7 };
            GrooveboxProcessor::SlotRowWrite wild;
            wild.on[2] = true;
            wild.level[2] = 3.0f;
            wild.chance[2] = 0.3f;
            wild.strikes[2] = 60;
            wild.level[9] = -2.0f;

            loud.writeSlotRows (1, lane7, &wild, 1);
            loud.setChannelSlot (7, 1);

            check (loud.getStepVelocity (7, 2) == 1.0f && loud.getStepVelocity (7, 9) == 0.0f,
                   "a level outside 0..1 is clamped the way the fader's own seam clamps it, not "
                   "written as a number the record cannot hold",
                   "levels " + juce::String (loud.getStepVelocity (7, 2)) + " and "
                       + juce::String (loud.getStepVelocity (7, 9)));

            check (loud.getStepChance (7, 2) == Layout::chanceRungFor (0.3f),
                   "a probability is snapped to the rung the record can hold before it reaches "
                   "the engine, which is the same rule setStepChance applies to a drag",
                   "chance " + juce::String (loud.getStepChance (7, 2), 6) + " against rung "
                       + juce::String (Layout::chanceRungFor (0.3f), 6));

            /**
                What this check does and does not gate. It says the end-to-end rule holds for
                this door — a caller cannot ask a cell for sixty hits — and it holds. It is
                NOT a gate on the clamp inside `writeSlotRows`: arm w8 of `out/mut14.py`
                deletes that clamp and this check stays green, because the value is clamped
                again on the way out of `getStepRatchet`, again by the codec, and again at the
                one place the number becomes sound (`SequencerEngine.cpp:874`, where 4.5's own
                `setStepRatchet (3, 7, 999)` arm does gate it). The seam's clamp is there so
                both write doors leave the atomic holding the same representation, and this
                line is here so the next reader does not count it as the control for that.
            */
            check (loud.getStepRatchet (7, 2) == Layout::maxRatchetHits,
                   "and a roll of sixty strikes is clamped to the ceiling the codec uses, rather "
                   "than becoming sixty hits on one onset",
                   "strikes " + juce::String (loud.getStepRatchet (7, 2)));

            int twice[] = { 9, 9 };
            GrooveboxProcessor::SlotRowWrite two [2];
            two[0].on[1] = true;
            two[1].on[1] = false;
            two[1].on[5] = true;

            const auto dupChanges = counter.changes;
            const auto dup = machine.writeSlotRows (1, twice, two, 2);

            // Read the record rather than the lane: the seam takes a slot, so the proof has to
            // be asked of a named slot, not of whatever a lane happens to point at. A record
            // that is not there at all comes back as -1s, which fails both of these.
            const auto dupRecord = slotRecord (machine, 1);
            const auto leftBehind = storedCell (dupRecord, 9, 1);
            const auto laterRow = storedCell (dupRecord, 9, 5);

            check (dup && laterRow.on == 1 && leftBehind.on == 0,
                   "a lane named twice in one fill is written twice and the later row wins, "
                   "which is the rule a caller building a lane list can rely on",
                   "step 1 held " + juce::String (leftBehind.on) + " and step 5 "
                       + juce::String (laterRow.on) + " of the record slot 1 carries");

            check (counter.changes == dupChanges + 1,
                   "and even a fill that names one lane twice is one property change",
                   "changes " + juce::String (counter.changes - dupChanges));
        }

        click.deleteFile();
    }

    //======================================================================
    std::printf ("\n%s  %d checks, %d failures\n",
                 failures == 0 ? "PASSED" : "FAILED", checks, failures);

    return failures == 0 ? 0 : 1;
}
