#pragma once

#include "Common.h"
#include "Resampler.h"

#include <array>
#include <cmath>
#include <cstring>

namespace gb
{

/**
    The master stage: the one place the summed output is trimmed and measured.

    Voices sum at unity, so the level a channel fader shows is the level it
    contributes. The trim lives here instead of inside the voices, where an
    earlier version hid a fixed 0.8 that no control could reach and no meter
    explained.

    Gain moves through a one-pole smoother so a drag cannot zipper. The peak is
    held on the audio side rather than in the editor, because the editor polls a
    few times a second — far too slow to catch a drum transient — so the bus
    keeps the highest value it has seen and lets it fall away between blocks.

    A sample peak is not the level the DAC puts out. Between two samples the
    band-limited waveform can be higher than either of them, and on a summed kit
    that difference is regularly a dB or two, so a bar that stops at 0 dBFS can
    still hand a host something that clips. The bus therefore reports a second
    number: the peak of the reconstruction read at four points per sample. It is
    a report and nothing else — it gates no level, changes no latch and reshapes
    no audio, because the house rule is that the trim and the faders are what the
    user has.
*/
class MasterBus
{
public:
    /** JUCE_DECLARE_NON_COPYABLE below counts as a user-declared constructor, so
        the default one has to be asked for explicitly. */
    MasterBus() = default;

    /** Channels the meter reports. The bus layout only offers mono or stereo. */
    static constexpr int numMeterChannels = 2;

    /** Level at which the clip indicator latches. */
    static constexpr float clipThreshold = 1.0f;

    /** Points per input sample the reconstruction is read at for the true peak.

        Four is what a BS.1770-style peak meter uses, and three dot products per
        sample per channel is what it costs. The fractions land on 256, 512 and 768
        of the kernel table's 1024 phases, so the coefficients are read exactly
        rather than interpolated between two phases.
    */
    static constexpr int truePeakOversampling = 4;

    /** Called from prepareToPlay, and again whenever the sample rate changes. */
    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;

        smoothing = 1.0f - std::exp (-1.0f / (float) (smoothingSeconds * juce::jmax (1.0, sampleRate)));

        currentGain = gainFor (targetDb.load());
        initialised = true;

        for (auto& peak : heldPeak)
            peak.store (0.0f);

        for (auto& peak : heldTruePeak)
            peak.store (0.0f);

        for (auto& window : tpWindow)
            for (auto& value : window)
                value = 0.0f;

        // Three fixed kernels, built here rather than in the render loop: the
        // reconstruction is only ever asked for at these three sub-sample steps.
        for (int step = 1; step < truePeakOversampling; ++step)
            SincKernelTable::interpolatingWeights ((double) step / (double) truePeakOversampling,
                                                   tpWeights[(std::size_t) step - 1].data());

        clearClipLatch();
    }

    void setGainDb (float db) noexcept
    {
        targetDb.store (juce::jlimit (Layout::minMasterGainDb, Layout::maxMasterGainDb, db));
    }

    float getGainDb() const noexcept { return targetDb.load(); }

    /** Applies the trim and updates the meters. Audio thread only. */
    void process (juce::AudioBuffer<float>& buffer) noexcept
    {
        const auto numSamples = buffer.getNumSamples();
        const auto numChannels = buffer.getNumChannels();

        if (numSamples <= 0 || numChannels <= 0)
            return;

        const auto target = gainFor (targetDb.load());

        if (! initialised)
        {
            currentGain = target;
            initialised = true;
        }

        float peaks[numMeterChannels] = {};
        float truePeaks[numMeterChannels] = {};
        bool clipped[numMeterChannels] = {};

        // One pointer per channel, taken before the loop: reaching for it per
        // sample is what makes a meter cost more than the audio it measures.
        float* data[numMeterChannels] = {};
        const auto metered = juce::jmin (numChannels, numMeterChannels);

        for (int channel = 0; channel < metered; ++channel)
            data[channel] = buffer.getWritePointer (channel);

        for (int i = 0; i < numSamples; ++i)
        {
            if (currentGain != target)
            {
                currentGain += (target - currentGain) * smoothing;

                // Snap once it is close enough to hear no difference, so a settled
                // trim stops costing a multiply and an add per sample.
                if (std::abs (target - currentGain) < 1.0e-6f)
                    currentGain = target;
            }

            const auto gain = currentGain;

            for (int channel = 0; channel < metered; ++channel)
            {
                auto value = data[channel][(size_t) i] * gain;

                // One NaN reaching the host takes the whole output down with it,
                // so it is caught here rather than trusted away.
                if (! std::isfinite (value))
                    value = 0.0f;

                data[channel][(size_t) i] = value;

                const auto magnitude = std::abs (value);

                peaks[channel] = juce::jmax (peaks[channel], magnitude);
                clipped[channel] = clipped[channel] || magnitude >= clipThreshold;

                // True peak: the highest point of the continuous waveform the
                // samples stand for. The window holds the last 24 output samples,
                // so when sample i arrives the interval that becomes measurable is
                // the one between i-12 and i-11 — the report is 12 samples (0.27 ms
                // at 44.1) behind the bar it is drawn against, which is why it can
                // afford a 24-tap kernel at all.
                auto& window = tpWindow[(std::size_t) channel];
                std::memmove (window.data(), window.data() + 1,
                              (SincKernelTable::taps - 1) * sizeof (float));
                window[(std::size_t) SincKernelTable::taps - 1] = value;

                auto interval = magnitude;

                for (int step = 0; step < truePeakOversampling - 1; ++step)
                {
                    const auto& weights = tpWeights[(std::size_t) step];
                    float sum = 0.0f;

                    for (int tap = 0; tap < SincKernelTable::taps; ++tap)
                        sum += weights[(std::size_t) tap] * window[(std::size_t) tap];

                    interval = juce::jmax (interval, std::abs (sum));
                }

                truePeaks[channel] = juce::jmax (truePeaks[channel], interval);
            }
        }

        // Anything past the metered channels still has to be trimmed, even though
        // the bus layout never asks for it.
        for (int channel = metered; channel < numChannels; ++channel)
            buffer.applyGain (channel, 0, numSamples, currentGain);

        const auto decay = (float) std::exp (-((double) numSamples / sampleRate) / holdSeconds);

        for (int channel = 0; channel < metered; ++channel)
        {
            heldPeak[(size_t) channel].store (
                juce::jmax (peaks[channel], heldPeak[(size_t) channel].load() * decay));

            // The same hold as the bar, so the two markers fall away together and
            // the gap between them is the inter-sample headroom and nothing else.
            heldTruePeak[(size_t) channel].store (
                juce::jmax (truePeaks[channel], heldTruePeak[(size_t) channel].load() * decay));

            if (clipped[channel])
                clipLatched[(size_t) channel].store (true);
        }
    }

    //==========================================================================
    // Read by the editor

    float getPeak (int channel) const noexcept
    {
        return juce::isPositiveAndBelow (channel, numMeterChannels)
                 ? heldPeak[(size_t) channel].load() : 0.0f;
    }

    /** True peak: the highest point of the waveform the output samples stand for.
        Never below the sample peak, because the sample peak is one of the points it
        is measured over. */
    float getTruePeak (int channel) const noexcept
    {
        return juce::isPositiveAndBelow (channel, numMeterChannels)
                 ? heldTruePeak[(size_t) channel].load() : 0.0f;
    }

    /** True once the output has reached 0 dBFS, until the latch is cleared. */
    bool isClipping (int channel) const noexcept
    {
        return juce::isPositiveAndBelow (channel, numMeterChannels)
                 && clipLatched[(size_t) channel].load();
    }

    void clearClipLatch() noexcept
    {
        for (auto& flag : clipLatched)
            flag.store (false);
    }

private:
    static float gainFor (float db) noexcept
    {
        return juce::Decibels::decibelsToGain (db);
    }

    /** Long enough that a drag is smooth, short enough that it is not sluggish. */
    static constexpr double smoothingSeconds = 0.020;

    /** How slowly a caught peak falls away, so a 200 ms poll can still see it. */
    static constexpr double holdSeconds = 0.12;

    double sampleRate = 44100.0;
    float smoothing = 1.0f;
    float currentGain = 1.0f;
    bool initialised = false;

    std::atomic<float> targetDb { Layout::defaultMasterGainDb };
    std::atomic<float> heldPeak[numMeterChannels] {};
    std::atomic<float> heldTruePeak[numMeterChannels] {};
    std::atomic<bool> clipLatched[numMeterChannels] {};

    /** The three sub-sample kernels, and the 24 output samples behind each one. */
    std::array<std::array<float, SincKernelTable::taps>, truePeakOversampling - 1> tpWeights {};
    std::array<std::array<float, SincKernelTable::taps>, numMeterChannels> tpWindow {};

    JUCE_DECLARE_NON_COPYABLE (MasterBus)
};

} // namespace gb
