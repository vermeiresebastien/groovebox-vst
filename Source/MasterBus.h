#pragma once

#include "Common.h"

#include <cmath>

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

    /** Called from prepareToPlay, and again whenever the sample rate changes. */
    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;

        smoothing = 1.0f - std::exp (-1.0f / (float) (smoothingSeconds * juce::jmax (1.0, sampleRate)));

        currentGain = gainFor (targetDb.load());
        initialised = true;

        for (auto& peak : heldPeak)
            peak.store (0.0f);

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
    std::atomic<bool> clipLatched[numMeterChannels] {};

    JUCE_DECLARE_NON_COPYABLE (MasterBus)
};

} // namespace gb
