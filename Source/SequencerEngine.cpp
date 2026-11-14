#include "SequencerEngine.h"

namespace gb
{

namespace
{
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

void SequencerEngine::trigger (int channel, float velocity, bool fromMidi)
{
    auto sample = samples.retainSampleForChannel (channel);

    if (sample == nullptr || sample->buffer.getNumSamples() <= 0)
        return;

    if (state.channelMuted[channel].load())
        return;

    const auto level = juce::jlimit (0.0f, 2.0f, state.level[(size_t) channel].load());

    if (level <= 0.0f)
        return;

    // The source's own rate against the output's, times the channel's pitch. Both
    // live here rather than at the call sites so the grid and a played note cannot
    // drift apart, and so the sample is retained once rather than twice per hit.
    const auto pitch = state.pitch[(size_t) channel].load();
    const auto increment = sample->rateRatioFor (currentSampleRate)
                             * std::pow (2.0, (double) pitch / 12.0);

    // A voice sums at unity: the level the channel fader shows is the level the
    // voice contributes, and the master trim is the only thing that scales the
    // result. Velocity scales the hit on top of that — the grid plays whatever
    // velocity its step carries, and a note from a keyboard or pad carries how
    // hard it was struck.
    const auto gain = level * juce::jlimit (0.0f, 1.0f, velocity);

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
    target->sincBand = SincKernelTable::bandForRatio (increment);
    target->integerPlayback = std::abs (increment - 1.0) < 1.0e-12;
    target->gain = gain;
    target->velocity = juce::jlimit (0.0f, 1.0f, velocity);
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

    soundingChannels[(size_t) channel].store (true);
}

bool SequencerEngine::channelHolds (int channel) const
{
    return state.sustain[(size_t) channel].load() > 0.0001f;
}

/** Lets go of every voice a note started on a channel. Used by note off. */
void SequencerEngine::releaseChannel (int channel)
{
    for (auto& voice : voices)
    {
        // Only the voices a note started. A grid hit on the same lane is the
        // sequencer's to finish, and a player lifting a key while the pattern runs
        // should not cut it short.
        if (voice.active && ! voice.released && voice.fromMidi && voice.channel == channel)
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
    info.velocity = voice.velocity;
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

    trigger (channel, velocity, true);
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

        // A grid hit carries the velocity stored against its step, so the grid has
        // the same dynamics a played note does. Muting and the sample lookup are
        // `trigger`'s business, not the grid's.
        trigger (channel, state.velocity[(size_t) channel][(size_t) step].load(), false);
    }
}

void SequencerEngine::renderVoices (juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
{
    if (numSamples <= 0)
        return;

    const auto numOutputChannels = buffer.getNumChannels();

    // Taken once for the whole call: reaching for a channel pointer inside the
    // per-sample loop costs more than the arithmetic around it.
    auto* outLeft = buffer.getWritePointer (0, startSample);
    auto* outRight = numOutputChannels > 1 ? buffer.getWritePointer (1, startSample) : nullptr;

    // Deliberately not cleared here. A block is split at every onset, so this runs
    // several times per block; clearing on each pass would wipe the channels that
    // sounded in the earlier ones, and the editor's activity indicators would miss
    // any hit that did not land in the last chunk. `processBlock` clears them once.
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

        // Lit whether or not the voice survives to the end of the chunk: a one-shot
        // that finishes half way through still made a sound.
        soundingChannels[(size_t) voice.channel].store (true);

        const auto* left = source.getReadPointer (0);
        const auto* right = source.getNumChannels() > 1 ? source.getReadPointer (1) : left;
        const auto sourceLength = source.getNumSamples();

        auto samplePosition = voice.position;
        const auto unityRate = voice.integerPlayback;
        const auto band = voice.sincBand;

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

            const auto gain = voice.gain * voice.envelope.getNextSample();

            float leftSample = 0.0f;
            float rightSample = 0.0f;

            if (unityRate)
            {
                const auto index = (int) samplePosition;
                leftSample  = left[index];
                rightSample = right[index];
            }
            else
            {
                const auto floorPosition = std::floor (samplePosition);
                const auto fraction = samplePosition - floorPosition;

                alignas (16) float weights[SincKernelTable::taps];
                kernelTable.getWeights (band, fraction, weights);

                // Tap 11 is the source sample just below the read position, so the
                // kernel is centred on it and adds no delay of its own. Taps that
                // fall off either end of the source are skipped rather than padded,
                // which costs a little level over the first and last few samples of
                // a hit and nothing anywhere else.
                const auto firstIndex = (int) floorPosition - (SincKernelTable::halfWidth - 1);

                for (int tap = 0; tap < SincKernelTable::taps; ++tap)
                {
                    const auto index = firstIndex + tap;

                    if (index < 0 || index >= sourceLength)
                        continue;

                    const auto weight = weights[tap];
                    leftSample  += left[index]  * weight;
                    rightSample += right[index] * weight;
                }
            }

            outLeft[i] += leftSample * gain;

            if (outRight != nullptr)
                outRight[i] += rightSample * gain;

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
    }

    // Recounted from the voices rather than tallied during the pass, so the number
    // is right however many times this block was split.
    int stillActive = 0;

    for (auto& voice : voices)
        if (voice.active)
            ++stillActive;

    activeVoices = stillActive;
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

    // Once for the whole block, so a channel that sounds in any chunk of it stays
    // reported as sounding until the next block begins.
    for (auto& flag : soundingChannels)
        flag.store (false);

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
    // The head advances by nothing but whole rendered sample counts. An onset is
    // quantised to the sample it falls on and the sub-sample remainder is left
    // behind rather than folded into the head, because a remainder carried forward
    // is a remainder that accumulates: an earlier version added the ideal onset
    // distance and then subtracted the samples rendered so far, which leaked up to
    // one sample per onset and drifted a third of a step over thirty-two bars.
    //
    // At most one bar of onsets can fall inside a single block, which is what the
    // guard allows for.
    //----------------------------------------------------------------------
    int rendered = 0;

    for (int guard = 0; guard < Layout::numSteps + 1 && rendered < numSamples; ++guard)
    {
        const auto distance = distanceToNextOnsetInSteps (position);
        const auto samplesToOnset = (int) std::floor (distance * samplesPerStep);
        const auto samplesLeft = numSamples - rendered;

        // The onset has to land strictly inside what is left of this block.
        if (samplesToOnset >= samplesLeft)
            break;

        if (samplesToOnset > 0)
        {
            renderVoices (buffer, rendered, samplesToOnset);
            rendered += samplesToOnset;
            position += (double) samplesToOnset / samplesPerStep;
        }

        const auto step = onsetStep >= 0 ? onsetStep : 0;

        onsetStep = (step + 1) % Layout::numSteps;
        currentStep.store (step);
        fireStep (step);
    }

    // Whatever is left of the block, including a block with nothing due in it.
    if (rendered < numSamples)
    {
        const auto tail = numSamples - rendered;
        renderVoices (buffer, rendered, tail);
        position += (double) tail / samplesPerStep;
    }
}

} // namespace gb
