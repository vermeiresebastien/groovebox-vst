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
        voice.released = false;
        voice.envelope = Envelope();
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

void SequencerEngine::trigger (int channel, double increment, float velocity, bool fromMidi)
{
    auto sample = samples.retainSampleForChannel (channel);

    if (sample == nullptr || sample->buffer.getNumSamples() <= 0)
        return;

    if (state.channelMuted[channel].load())
        return;

    const auto level = juce::jlimit (0.0f, 2.0f, state.level[(size_t) channel].load());

    if (level <= 0.0f)
        return;

    // A little headroom so stacked hits do not clip the master bus outright.
    // Velocity scales the hit: the grid always plays at full velocity, while a
    // note from a keyboard or pad carries how hard it was struck.
    const auto gain = level * 0.8f * juce::jlimit (0.0f, 1.0f, velocity);

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
    target->active = true;
    target->fromMidi = fromMidi;
    target->released = false;

    // The channel's envelope, rebuilt per hit so an edit is heard on the next one.
    target->envelope.setSampleRate (currentSampleRate);
    target->envelope.setParameters (
        (float) envelopeSeconds (state.attack[(size_t) channel].load(), Layout::maxAttackSeconds),
        (float) envelopeSeconds (state.decay[(size_t) channel].load(), Layout::maxDecaySeconds),
        state.sustain[(size_t) channel].load(),
        (float) envelopeSeconds (state.release[(size_t) channel].load(), Layout::maxReleaseSeconds));
    target->envelope.noteOn();

    voiceVelocity[(size_t) (target - voices.data())] = velocity;

    soundingChannels[(size_t) channel].store (true);
}

bool SequencerEngine::channelHolds (int channel) const
{
    return state.sustain[(size_t) channel].load() > 0.0001f;
}

/** Lets go of every voice on a channel that is still holding. Used by note off. */
void SequencerEngine::releaseChannel (int channel)
{
    for (auto& voice : voices)
    {
        if (voice.active && ! voice.released && voice.channel == channel)
        {
            voice.envelope.noteOff();
            voice.released = true;
        }
    }
}

SequencerEngine::VoiceDebug SequencerEngine::getVoiceDebug (int index) const
{
    VoiceDebug info;

    if (! juce::isPositiveAndBelow (index, (int) voices.size()))
        return info;

    const auto& voice = voices[(size_t) index];

    info.active = voice.active;
    info.channel = voice.channel;
    info.position = voice.position;
    info.increment = voice.increment;
    info.gain = voice.gain;
    info.velocity = voiceVelocity[(size_t) index];
    info.sampleLength = voice.sample != nullptr ? voice.sample->buffer.getNumSamples() : 0;
    info.envelopeValue = voice.envelope.getCurrentValue();
    info.envelopeActive = voice.envelope.isActive();
    info.released = voice.released;

    return info;
}

//==============================================================================
// MIDI

int SequencerEngine::channelForMidiNote (int midiNote) noexcept
{
    const auto channel = midiNote - Layout::firstMidiNote;
    return juce::isPositiveAndBelow (channel, Layout::numChannels) ? channel : -1;
}

int SequencerEngine::midiNoteForChannel (int channel) noexcept
{
    return juce::isPositiveAndBelow (channel, Layout::numChannels)
             ? Layout::firstMidiNote + channel
             : -1;
}

void SequencerEngine::triggerChannel (int channel, float velocity)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    auto sample = samples.retainSampleForChannel (channel);

    if (sample == nullptr)
        return;

    const auto rateRatio = sample->rateRatioFor (currentSampleRate);
    const auto pitch = state.pitch[(size_t) channel].load();
    const auto pitchRatio = std::pow (2.0, (double) pitch / 12.0);

    trigger (channel, rateRatio * pitchRatio, velocity, true);
}

void SequencerEngine::processMidi (const juce::MidiBuffer& midi, int numSamples)
{
    if (midi.isEmpty())
        return;

    // Note events are collected before anything is played so the block can be
    // rendered in order: an extra voice must not be started before the samples
    // that precede it. At 5 ms blocks there are only ever a handful.
    struct NoteEvent
    {
        int sampleOffset;
        int channel;
        float velocity;
        bool isNoteOn;
    };

    std::array<NoteEvent, 64> events {};
    int numEvents = 0;

    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();

        if (! message.isNoteOn() && ! message.isNoteOff())
            continue; // the sequencer has no use for anything else yet

        const auto channel = channelForMidiNote (message.getNoteNumber());

        if (channel < 0)
            continue; // outside the mapped range

        if (numEvents >= (int) events.size())
            break;

        events[(size_t) numEvents++] = { juce::jlimit (0, juce::jmax (0, numSamples - 1), metadata.samplePosition),
                                         channel,
                                         message.getFloatVelocity(),
                                         message.isNoteOn() };
    }

    if (numEvents == 0)
        return;

    std::sort (events.begin(), events.begin() + numEvents,
               [] (const NoteEvent& a, const NoteEvent& b) { return a.sampleOffset < b.sampleOffset; });

    const auto renderUpTo = [this, numSamples] (int from, int to)
    {
        if (currentBlock != nullptr && to > from && from < numSamples)
            renderVoices (*currentBlock, from, juce::jmin (to, numSamples) - from);
    };

    int rendered = 0;

    for (int i = 0; i < numEvents; ++i)
    {
        const auto& event = events[(size_t) i];

        if (event.sampleOffset > rendered)
        {
            renderUpTo (rendered, event.sampleOffset);
            rendered = event.sampleOffset;
        }

        if (event.isNoteOn)
            triggerChannel (event.channel, event.velocity);
        else
            releaseChannel (event.channel); // only bites once sustain is opened up
    }

    renderUpTo (rendered, numSamples);
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

        trigger (channel, rateRatio * pitchRatio, 1.0f, false);
    }
}

void SequencerEngine::renderVoices (juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
{
    if (numSamples <= 0)
        return;

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

        // The hit's velocity travels with the voice, because rendering can be
        // split across a block when a note arrives part way through it.
        const auto targetGain = voice.gain * voiceVelocity[(size_t) (&voice - voices.data())];

        // A channel with no sustain is a one-shot: its envelope is let go the
        // moment it reaches the sustain stage, so the release setting shapes the
        // tail instead of the note ringing at a fixed level for ever.
        const auto holds = channelHolds (voice.channel);

        for (int i = 0; i < numSamples; ++i)
        {
            if (samplePosition >= (double) (sourceLength - 1))
            {
                voice.active = false;
                voice.sample = nullptr;
                break;
            }

            // Check the stage before taking the sample, so a one-shot is already
            // heading into its release as it arrives at the end of its decay.
            if (voice.envelope.getStage() == Envelope::Stage::sustain
                && (! holds || voice.released))
            {
                voice.envelope.noteOff();
                voice.released = true;
            }

            const auto gain = targetGain * voice.envelope.getNextSample();

            const auto index = (int) samplePosition;
            const auto fraction = (float) (samplePosition - (double) index);
            const auto nextIndex = juce::jmin (index + 1, sourceLength - 1);

            const auto leftSample  = left[index]  + fraction * (left[nextIndex]  - left[index]);
            const auto rightSample = right[index] + fraction * (right[nextIndex] - right[index]);

            buffer.getWritePointer (0, startSample + i)[0] += leftSample * gain;

            if (numOutputChannels > 1)
                buffer.getWritePointer (1, startSample + i)[0] += rightSample * gain;

            samplePosition += voice.increment;

            // Once the envelope has run out there is nothing left to hear, so the
            // voice stops even if the sample has more left in it.
            if (! voice.envelope.isActive())
            {
                voice.active = false;
                voice.sample = nullptr;
                break;
            }
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

    // Remembered so a note arriving part way through the block can be rendered
    // from its own sample onward.
    currentBlock = &buffer;

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
            // Let the tails ring out instead of cutting them dead. Voices played
            // from a note are left alone: they were not started by the transport,
            // so stopping the transport must not silence them.
            for (auto& voice : voices)
                if (voice.active && ! voice.fromMidi && ! voice.released)
                {
                    voice.envelope.noteOff();
                    voice.released = true;
                }

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
