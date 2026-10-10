#include "SequencerEngine.h"

namespace gb
{

namespace
{
    /** Ramp time applied when a voice is cut short, so stops do not click. */
    constexpr double releaseSeconds = 0.005;

    /** Ramp time at the start of a voice, which removes hard-edge clicks. */
    constexpr double attackSeconds = 0.001;

    /** How far the off-beat of a swing pair travels at swing = 1. */
    constexpr double swingDepth = 0.25;
}

SequencerEngine::SequencerEngine (SampleStore& store, SequencerState& sequencerState)
    : samples (store), state (sequencerState)
{
    for (auto& voice : voices)
        voice.active = false;

    for (auto& flag : soundingChannels)
        flag.store (false);
}

void SequencerEngine::prepare (double sampleRate, int /*maximumBlockSize*/)
{
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    stopAllVoices();
    position = 0.0;
    onsetStep = -1;
    lastFired = -1;
    hostDriven = false;
    currentStep.store (-1);
}

void SequencerEngine::releaseResources()
{
    stopAllVoices();
}

void SequencerEngine::stopAllVoices()
{
    for (auto& voice : voices)
    {
        voice.active = false;
        voice.releasing = 0;
        voice.sample = nullptr;
    }

    for (auto& flag : soundingChannels)
        flag.store (false);

    activeVoices = 0;
}

//==============================================================================
// Timing

double SequencerEngine::stepOnsetInSteps (int step) const
{
    const auto swing = (double) juce::jlimit (0.0f, 1.0f, state.swing.load());
    const auto grid = juce::jmax (2, state.swingGrid.load());
    const auto gridLength = (double) grid;

    // Where the off-beat sits inside its pair: 0.5 is dead centre (straight),
    // 0.75 is the heaviest shuffle.
    const auto fraction = 0.5 + swingDepth * swing;
    const auto positionInPair = (double) (step % grid);
    const auto pair = (double) (step / grid);

    if (positionInPair < gridLength * 0.5)
        return pair * gridLength + positionInPair * (2.0 * fraction);

    return pair * gridLength
             + fraction * gridLength
             + (positionInPair - gridLength * 0.5) * 2.0 * (1.0 - fraction);
}

void SequencerEngine::beginBar (double tempo)
{
    const auto bpm = juce::jlimit (20.0, 400.0, tempo);
    stepsPerSecond = bpm * 4.0 / 60.0;
    samplesPerStep = currentSampleRate / stepsPerSecond;
}

/** Steps from the playback head to the next armed onset. */
double SequencerEngine::distanceToNextOnsetInSteps (double head) const
{
    if (onsetStep < 0)
    {
        // Nothing armed yet: the first step of the bar the head is in.
        const auto barStart = std::floor (head / (double) Layout::numSteps) * (double) Layout::numSteps;
        return juce::jmax (0.0, barStart + stepOnsetInSteps (0) - head);
    }

    return juce::jmax (0.0, onsetPositionInSteps (onsetStep) - head);
}

double SequencerEngine::onsetPositionInSteps (int step) const
{
    // `bar` is recomputed from the head rather than cached, so the two can never
    // disagree after a wrap.
    const auto currentBarStart = std::floor (position / (double) Layout::numSteps) * (double) Layout::numSteps;
    const auto candidate = currentBarStart + stepOnsetInSteps (step);

    // An onset that already sits behind the head belongs to the following bar.
    return candidate >= position ? candidate : candidate + (double) Layout::numSteps;
}

//==============================================================================
// Voices

void SequencerEngine::trigger (int channel, double increment)
{
    auto sample = samples.retainSampleForChannel (channel);

    if (sample == nullptr || sample->buffer.getNumSamples() <= 0)
        return;

    const auto level = juce::jlimit (0.0f, 2.0f, state.level[(size_t) channel].load());

    if (level <= 0.0f)
        return;

    // A little headroom so stacked hits do not clip the master bus outright.
    const auto gain = level * 0.8f;

    Voice* target = nullptr;

    for (auto& voice : voices)
    {
        if (! voice.active)
        {
            target = &voice;
            break;
        }
    }

    // All voices busy: steal the one that has been running longest.
    if (target == nullptr)
    {
        target = &voices[0];

        for (auto& voice : voices)
            if (voice.age < target->age)
                target = &voice;
    }

    target->sample = sample;
    target->channel = channel;
    target->position = 0.0;
    target->increment = increment;
    target->gain = gain;
    target->age = ++voiceCounter;
    target->releasing = 0;
    target->attackTotal = juce::jmax (2, (int) (currentSampleRate * attackSeconds));
    target->attackCounter = 0;
    target->active = true;

    soundingChannels[(size_t) channel].store (true);
}

void SequencerEngine::fireStep (int step)
{
    for (int channel = 0; channel < Layout::numChannels; ++channel)
    {
        if (! state.pattern[(size_t) channel][(size_t) step].load())
            continue;

        if (state.channelMuted[channel].load())
            continue;

        auto sample = samples.retainSampleForChannel (channel);

        if (sample == nullptr)
            continue;

        const auto rateRatio = sample->rateRatioFor (currentSampleRate);
        const auto pitch = state.pitch[(size_t) channel].load();
        const auto pitchRatio = std::pow (2.0, (double) pitch / 12.0);

        trigger (channel, rateRatio * pitchRatio);
    }
}

void SequencerEngine::renderVoices (juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
{
    if (numSamples <= 0)
        return;

    const auto releaseSamples = juce::jmax (2, (int) (currentSampleRate * releaseSeconds));
    const auto numOutputChannels = buffer.getNumChannels();

    for (auto& flag : soundingChannels)
        flag.store (false);

    int sounding = 0;

    for (auto& voice : voices)
    {
        if (! voice.active || voice.sample == nullptr)
            continue;

        const auto& source = voice.sample->buffer;

        if (source.getNumSamples() <= 0)
        {
            voice.active = false;
            voice.sample = nullptr;
            continue;
        }

        const auto* left = source.getReadPointer (0);
        const auto* right = source.getNumChannels() > 1 ? source.getReadPointer (1) : left;
        const auto sourceLength = source.getNumSamples();

        auto samplePosition = voice.position;
        auto gain = voice.gain;

        for (int i = 0; i < numSamples; ++i)
        {
            if (samplePosition >= (double) (sourceLength - 1))
            {
                voice.active = false;
                voice.sample = nullptr;
                break;
            }

            if (voice.releasing > 0)
            {
                gain = voice.gain * ((float) voice.releasing / (float) releaseSamples);
                --voice.releasing;

                if (voice.releasing <= 0)
                {
                    voice.active = false;
                    voice.sample = nullptr;
                    break;
                }
            }
            else if (voice.attackCounter < voice.attackTotal)
            {
                // Ramp in over the attack, then hold the voice's own gain.
                gain = voice.gain * ((float) voice.attackCounter / (float) voice.attackTotal);
                ++voice.attackCounter;
            }
            else
            {
                gain = voice.gain;
            }

            const auto index = (int) samplePosition;
            const auto fraction = (float) (samplePosition - (double) index);
            const auto nextIndex = juce::jmin (index + 1, sourceLength - 1);

            const auto leftSample  = left[index]  + fraction * (left[nextIndex]  - left[index]);
            const auto rightSample = right[index] + fraction * (right[nextIndex] - right[index]);

            buffer.getWritePointer (0, startSample + i)[0] += leftSample * gain;

            if (numOutputChannels > 1)
                buffer.getWritePointer (1, startSample + i)[0] += rightSample * gain;

            samplePosition += voice.increment;
        }

        voice.position = samplePosition;

        if (voice.active)
        {
            soundingChannels[(size_t) voice.channel].store (true);
            ++sounding;
        }
    }

    activeVoices = sounding;
}

//==============================================================================
// Block processing

void SequencerEngine::processBlock (juce::AudioBuffer<float>& buffer,
                                    const juce::AudioPlayHead::PositionInfo& positionInfo,
                                    bool haveHostPosition)
{
    const auto numSamples = buffer.getNumSamples();

    if (numSamples <= 0 || buffer.getNumChannels() == 0)
        return;

    // Tempo comes from the host when it has one, from the editor otherwise.
    const auto hostBpm = positionInfo.getBpm();
    const auto tempo = (haveHostPosition && hostBpm.hasValue()) ? *hostBpm : state.manualTempo.load();

    beginBar (tempo);

    // The host transport always wins when it is available.
    const auto transportRunning = haveHostPosition ? positionInfo.getIsPlaying() : state.playing.load();

    if (! transportRunning)
    {
        if (activeVoices > 0 || lastFired >= 0)
        {
            // Let the tails ring out instead of cutting them dead.
            for (auto& voice : voices)
                if (voice.active && voice.releasing == 0)
                    voice.releasing = juce::jmax (2, (int) (currentSampleRate * releaseSeconds));

            renderVoices (buffer, 0, numSamples);
        }

        lastFired = -1;
        onsetStep = -1;
        currentStep.store (-1);

        const auto ppq = positionInfo.getPpqPosition();

        if (haveHostPosition && ppq.hasValue())
        {
            // A host can stop mid-bar; pick the position back up when it resumes.
            position = (*ppq) * 4.0;
            hostDriven = true;
        }
        else
        {
            // Playing again from the built-in transport starts at the top of the
            // bar, so the pattern always begins on step 0.
            position = 0.0;
            hostDriven = false;
        }

        return;
    }

    //----------------------------------------------------------------------
    // Find the playback head.
    //----------------------------------------------------------------------
    const auto ppq = positionInfo.getPpqPosition();

    if (haveHostPosition && ppq.hasValue())
    {
        position = (*ppq) * 4.0;

        if (! hostDriven)
        {
            // Taking over from the built-in transport: start cleanly on the grid.
            lastFired = -1;
            onsetStep = -1;
            hostDriven = true;
        }
    }
    else
    {
        hostDriven = false;
    }

    //----------------------------------------------------------------------
    // Fire the step whose swung onset this block has reached, splitting the block
    // at that onset so the hit starts on the exact sample.
    //
    // `position` is the only clock and onsets are expressed in the same steps, so
    // no second cursor can drift. At most one step fires per block, and a step
    // can never fire twice.
    //----------------------------------------------------------------------
    const auto blockSamples = (double) numSamples;
    int rendered = 0;

    for (int guard = 0; guard < 4 && rendered < numSamples; ++guard)
    {
        const auto distance = distanceToNextOnsetInSteps (position);

        // The onset has to land strictly inside what is left of this block.
        const auto remainingSteps = (blockSamples - (double) rendered) / samplesPerStep;

        if (distance >= remainingSteps)
            break;

        const auto step = onsetStep >= 0 ? onsetStep : 0;

        // Everything before the onset belongs to the previous step.
        const auto upToOnset = (int) std::floor (distance * samplesPerStep);

        if (upToOnset > 0)
        {
            const auto chunk = juce::jlimit (1, numSamples - rendered, upToOnset);
            renderVoices (buffer, rendered, chunk);
            rendered += chunk;
        }

        // Move the head exactly onto the onset, then start the step there.
        position += distance;
        onsetStep = (step + 1) % Layout::numSteps;
        currentStep.store (step);
        fireStep (step);

        // The advance at the end of the loop moves the head over these samples,
        // so the distance to the following onset stays measured from this onset.
        if (rendered > 0)
            position -= (double) rendered / samplesPerStep;
    }

    // Whatever is left of the block (including a block with nothing due).
    if (rendered < numSamples)
        renderVoices (buffer, rendered, numSamples - rendered);

    // Advance the head across the whole block. Onsets are recomputed from it
    // every block, so a rounding error can never accumulate into drift.
    position += blockSamples / samplesPerStep;
}

} // namespace gb
