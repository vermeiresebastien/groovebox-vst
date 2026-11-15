#include "SequencerEngine.h"

namespace gb
{

namespace
{
    /** How far the off-beat of a swing pair travels at swing = 1. */
    constexpr double swingDepth = 0.25;

    /**
        How far a host's position may disagree with where the engine left its own
        head before the difference means the head moved, rather than that the two
        of them round differently.

        Four samples, because that is comfortably more than either of the ordinary
        reasons for a disagreement — the sub-sample remainder an onset leaves behind
        is under one sample, and a host that derives its reported position from bars
        and beats rather than from samples can be a sample out at a loop point — and
        still far less than the shortest gap an engine can produce between two
        onsets, which is half a step, over two thousand samples. Anything bigger
        than this is a seek, a punch-in or a stop.

        The two ways to get this number wrong are not equal. Too large, and a real
        seek falls inside it: the head moves, the armed onset stays where it was,
        and the pattern resumes in the wrong place in the bar — which is the whole
        fault this is here to close. Too small, and an ordinary block is read as a
        jump, which costs a re-derive from a head that has not really moved: the
        search answers with the step the walk was already going to fire, so nothing
        is lost but the arithmetic. Note which way the tolerance is *used* as well:
        an onset that sits behind the head by less than this fires on the spot, a
        sample or two late, while one behind by more is treated as missed and the
        pattern moves on to the next step. Before that rule existed, a slip of two
        samples pushed the armed onset a whole bar forward and lost the bar.
    */
    constexpr double positionSlipInSamples = 4.0;
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

    // Both ramps counted in output samples, so the same taper means the same
    // number of milliseconds whatever rate the host is running at.
    endFadeSamples = Layout::endFadeSeconds * currentSampleRate;
    stealFadeSamples = Layout::stealFadeSeconds * currentSampleRate;
    chokeFadeSamples = Layout::chokeFadeSeconds * currentSampleRate;

    // The pitch drop's glide, in the same recursion the envelope falls use: five
    // time constants across the sweep, so what is left of the excess when the
    // sweep is over is 0.67% of it — and the last sample of the sweep is snapped to
    // nothing rather than chasing it, which is what makes a dropped hit land on the
    // pitch its lane says instead of just above it.
    pitchDropSamples = Layout::pitchDropSweepSeconds * currentSampleRate;
    pitchDropCoefficient = pitchDropSamples > 0.0
                               ? std::exp (-Layout::pitchDropTimeConstants / pitchDropSamples)
                               : 1.0;

    // The darkest corner of the velocity law, as a coefficient rather than a
    // frequency: a one-pole moves this fraction of the way to its input every
    // sample, and that fraction is what the render loop wants.
    softestHitMix = 1.0f - (float) std::exp (-2.0 * juce::MathConstants<double>::pi
                                             * Layout::softestHitCutoffHz / currentSampleRate);

    stopAllVoices();
    position = 0.0;
    predictedHead = 0.0;
    havePrediction = false;
    onsetStep = -1;
    lastFired = -1;
    hostDriven = false;
    numNoteEvents = 0;
    nextNoteEvent = 0;
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

    // A tail is a hit that had already been given up, so a stop cuts it short
    // rather than leaving it fading over whatever plays next.
    for (auto& tail : tails)
        tail = Voice();

    for (auto& flag : soundingChannels)
        flag.store (false);

    activeVoices = 0;
}

//==============================================================================
// Timing

double SequencerEngine::stepOnsetInSteps (int step) const
{
    const auto swing = (double) juce::jlimit (0.0f, 1.0f, state.swing.load());

    // Not whatever the parameter holds: a pair that does not divide the bar would
    // swing the second half of it against the first. See `Layout::usableSwingGrid`.
    const auto grid = Layout::usableSwingGrid (state.swingGrid.load());
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
    // A block being walked always has something armed: `processBlock` re-derives
    // the armed step from the head whenever it does not. The -1 case survives
    // because a stopped engine really does hold -1, and answering it the way a
    // re-derive would is what keeps this and `processBlock` from ever disagreeing
    // about where the next onset is — the old answer, "the first step of the bar
    // the head is in, clamped to now", is what made a mid-bar resume fire step 0
    // on the spot.
    const auto armed = onsetStep >= 0 ? onsetStep : firstStepAtOrAfter (head);

    return juce::jmax (0.0, onsetPositionInSteps (armed) - head);
}

double SequencerEngine::positionSlipInSteps() const
{
    return samplesPerStep > 0.0 ? positionSlipInSamples / samplesPerStep : 0.0;
}

int SequencerEngine::firstStepAtOrAfter (double head) const
{
    const auto barStart = std::floor (head / (double) Layout::numSteps) * (double) Layout::numSteps;

    // The search is over the bar the head is in, in step order. Onsets rise with
    // the step number — swing moves the whole pair pattern later, never past one
    // another — so the first step that answers is the nearest onset, and there is
    // no need to look further.
    for (int step = 0; step < Layout::numSteps; ++step)
        if (barStart + stepOnsetInSteps (step) >= head - positionSlipInSteps())
            return step;

    // Past the last onset of the bar, so the next one is step 0 of the bar after
    // this one. That is what `onsetPositionInSteps` hands back for step 0 from
    // here, since its own candidate is a whole bar behind the head.
    return 0;
}

double SequencerEngine::onsetPositionInSteps (int step) const
{
    // `bar` is recomputed from the head rather than cached, so the two can never
    // disagree after a wrap.
    const auto currentBarStart = std::floor (position / (double) Layout::numSteps) * (double) Layout::numSteps;
    const auto candidate = currentBarStart + stepOnsetInSteps (step);

    // An onset at or ahead of the head is the one to walk to. One just behind it is
    // still the one to fire, a sample or two late: it got there because the host's
    // reported position and this engine's own arithmetic round differently, not
    // because the head moved, and firing it late costs nothing that pushing it a
    // bar forward would not — see `positionSlipInSamples`.
    //
    // An onset further behind than that belongs to a bar the head has left, and the
    // next occurrence of that step is a whole bar on.
    return candidate >= position - positionSlipInSteps()
             ? candidate
             : candidate + (double) Layout::numSteps;
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

    /**
        How far above that rate the hit starts, and so how far it falls.

        Kept out of `increment` because `increment` is the rate the hit *settles*
        at, which is the note: the lane tuned to C1 with a drop of five still holds
        C1 for the rest of the sample. The band below is what the entry rate needs,
        and `renderVoice` follows the fall from there: a hit that starts an octave
        up has to be anti-aliased for an octave up, but a hit that settled thirty
        milliseconds ago does not, and holding the narrow cutoff over the tail cost
        3.3 dB of top end at +12 — a tuning control that also dims the sample.
    */
    const auto drop = state.pitchDrop[(size_t) channel].load();
    const auto dropStart = drop > 0 ? std::pow (2.0, (double) drop / 12.0) : 1.0;

    // A voice sums at unity: the level the channel fader shows is the level the
    // voice contributes, and the master trim is the only thing that scales the
    // result. Velocity scales the hit on top of that — the grid plays whatever
    // velocity its step carries, and a note from a keyboard or pad carries how
    // hard it was struck.
    const auto gain = level * juce::jlimit (0.0f, 1.0f, velocity);

    // A lane in a choke group cuts whatever is already sounding in it. Done before
    // the pool is searched, so the hits it releases are slots this hit can take:
    // that is the difference between a choke and running out of voices.
    const auto group = state.chokeGroup[(size_t) channel].load();

    if (group != 0)
        chokeVoicesInGroup (group);

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

        // Hand the displaced hit off before writing over it. Its own position and
        // increment are what keep the tail reading the source on the sample the
        // voice had reached, so this is the last moment they exist.
        handOffToTail (*target, stealFadeSamples);
    }

    target->sample = sample;
    target->channel = channel;
    target->position = 0.0;
    target->increment = increment;
    target->sincBand = SincKernelTable::bandForRatio (increment * dropStart);
    target->integerPlayback = drop <= 0 && std::abs (increment - 1.0) < 1.0e-12;
    target->dropExcess = dropStart - 1.0;
    target->dropRemaining = drop > 0 && pitchDropSamples > 0.0 ? pitchDropSamples : 0.0;
    target->gain = gain;
    target->velocity = juce::jlimit (0.0f, 1.0f, velocity);

    // The other half of how hard this was struck: see `brightnessMixFor`. Reset
    // with the hit rather than carried, so a note that starts where an old one ended
    // does not inherit its tone.
    target->brightnessMix = brightnessMixFor (target->velocity);
    target->brightnessLeft = 0.0f;
    target->brightnessRight = 0.0f;

    target->age = ++voiceCounter;
    target->active = true;
    target->fromMidi = fromMidi;
    target->released = false;
    target->chokeGroup = group;

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

/**
    Puts a displaced hit into the tail pool, to be faded out there.

    The voice keeps reading its own source from its own position; what changes is
    what multiplies it. Its envelope stops being stepped the moment it is handed
    over, because the ramp replaces it — so the level captured here is the one the
    ramp starts from, and the fade begins exactly where the hit was interrupted.
*/
void SequencerEngine::handOffToTail (Voice& victim, double fadeSamples)
{
    if (victim.sample == nullptr || victim.sample->buffer.getNumSamples() <= 0)
        return;

    // A voice already at silence cannot click, and taking a slot for it would
    // cost a fade from a hit that is not audible.
    const auto captured = victim.gain * victim.envelope.getCurrentValue();

    if (captured <= 0.0f)
        return;

    Voice* slot = nullptr;

    for (auto& tail : tails)
    {
        if (! tail.active)
        {
            slot = &tail;
            break;
        }
    }

    // Every slot is taken, which means more hits were stolen in this one sample
    // than there is room to fade. The tail furthest down its ramp is the quietest
    // thing in the pool, so cutting it costs the least of anything here; at
    // `Layout::stealFadeSeconds` a tail is only ever holding on for two
    // milliseconds, and twelve lanes cannot fill sixteen of them.
    if (slot == nullptr)
    {
        slot = &tails[0];

        for (auto& tail : tails)
            if (tail.fadeRemaining < slot->fadeRemaining)
                slot = &tail;
    }

    slot->sample = victim.sample;
    slot->channel = victim.channel;
    slot->position = victim.position;
    slot->increment = victim.increment;
    slot->dropExcess = victim.dropExcess;
    slot->dropRemaining = victim.dropRemaining;
    slot->sincBand = victim.sincBand;
    slot->integerPlayback = victim.integerPlayback;
    slot->velocity = victim.velocity;

    // The tone goes with the hit. A stolen voice is mid-sample and mid-filter, so the
    // memory has to cross with the coefficient: a lowpass that reopened from silence
    // is a step in the output, which is what the ramp two lines up exists to prevent.
    slot->brightnessMix = victim.brightnessMix;
    slot->brightnessLeft = victim.brightnessLeft;
    slot->brightnessRight = victim.brightnessRight;

    slot->gain = captured;
    slot->inTail = true;
    slot->active = true;
    slot->fadeLength = fadeSamples;
    slot->fadeRemaining = fadeSamples;
}

/**
    Lets every hit in `group` go, with the choke's fade rather than the engine's.

    Every voice in the group, including one on the lane that is about to be struck:
    a lane in a group cuts its own last hit, which is what a closed hat does to an
    open one and what a repeated hit on a choke-locked pad should do.
*/
void SequencerEngine::chokeVoicesInGroup (int group)
{
    for (auto& voice : voices)
    {
        if (! voice.active || voice.chokeGroup != group)
            continue;

        handOffToTail (voice, chokeFadeSamples);

        // Whatever the tail ended up with, the slot is free now — which is the
        // other half of a choke: it makes room rather than asking for it.
        voice.active = false;
        voice.sample = nullptr;
        voice.envelope = Envelope();
        voice.released = false;
        voice.chokeGroup = 0;
        voice.fadeRemaining = 0.0;
    }
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

int SequencerEngine::collectNoteEvents (const juce::MidiBuffer& midi, int numSamples)
{
    int numEvents = 0;

    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();

        if (! message.isNoteOn() && ! message.isNoteOff())
            continue; // the sequencer has no use for anything else yet

        const auto channel = channelForMidiNote (message.getNoteNumber());

        if (channel < 0)
            continue; // outside the mapped range

        if (numEvents >= maxNoteEvents)
            break;

        noteEvents[(size_t) numEvents++] = { juce::jlimit (0, juce::jmax (0, numSamples - 1), metadata.samplePosition),
                                             channel,
                                             message.getFloatVelocity(),
                                             message.isNoteOn() };
    }

    // Sorted by arrival sample so the render walk can treat them as split points.
    // `std::sort` rather than `std::stable_sort`: the latter asks the allocator for
    // scratch space on every call, and this runs on the audio thread.
    if (numEvents > 1)
        std::sort (noteEvents.begin(), noteEvents.begin() + numEvents,
                   [] (const NoteEvent& a, const NoteEvent& b) { return a.sampleOffset < b.sampleOffset; });

    return numEvents;
}

int SequencerEngine::nextNoteEventOffset (int numSamples) const
{
    return nextNoteEvent < numNoteEvents
             ? noteEvents[(size_t) nextNoteEvent].sampleOffset
             : numSamples;
}

void SequencerEngine::applyNoteEventsAt (int offset)
{
    while (nextNoteEvent < numNoteEvents
             && noteEvents[(size_t) nextNoteEvent].sampleOffset <= offset)
    {
        const auto& event = noteEvents[(size_t) nextNoteEvent];

        if (event.isNoteOn)
            trigger (event.channel, event.velocity, true);
        else
            releaseChannel (event.channel); // only bites once sustain is opened up

        ++nextNoteEvent;
    }
}

void SequencerEngine::fireNextOnset()
{
    const auto step = onsetStep >= 0 ? onsetStep : 0;

    onsetStep = (step + 1) % Layout::numSteps;
    currentStep.store (step);
    lastFired = step;

    fireStep (step);
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
        if (voice.active)
            renderVoice (voice, outLeft, outRight, numSamples);
    }

    // The stolen hits fade after the live ones, which is only the order of the
    // summing: everything lands in the same two output channels.
    for (auto& tail : tails)
    {
        if (tail.active)
            renderVoice (tail, outLeft, outRight, numSamples);
    }

    // Recounted from the voices rather than tallied during the pass, so the number
    // is right however many times this block was split. Tails are not counted:
    // they are hits the pool has already given up, and the voice budget is what
    // says whether a new hit has to steal.
    int stillActive = 0;

    for (auto& voice : voices)
        if (voice.active)
            ++stillActive;

    activeVoices = stillActive;
}

void SequencerEngine::renderVoice (Voice& voice, float* outLeft, float* outRight, int numSamples)
{
    const auto& source = voice.sample->buffer;

    if (source.getNumSamples() <= 0)
    {
        voice.active = false;
        voice.sample = nullptr;
        return;
    }

    // Lit whether or not the voice survives to the end of the chunk: a one-shot
    // that finishes half way through still made a sound. A tail lit its lane for
    // the two milliseconds it is still audible, which is what sounding means.
    soundingChannels[(size_t) voice.channel].store (true);

    const auto* left = source.getReadPointer (0);
    const auto* right = source.getNumChannels() > 1 ? source.getReadPointer (1) : left;
    const auto sourceLength = source.getNumSamples();

    auto samplePosition = voice.position;
    const auto nominalIncrement = voice.increment;
    const auto unityRate = voice.integerPlayback;
    int band = voice.sincBand;

    /**
        The rate the read is at *now*, which is not the lane's rate while a drop is running.

        `voice.increment` is the rate the hit settles at — the note — and the excess
        above it is what the glide spends. Both are local copies stepped per sample
        and written back at the end, so a hit that crosses a block boundary carries on
        from the sample it reached instead of starting its fall again.
    */
    auto dropExcess = voice.dropExcess;
    auto dropRemaining = voice.dropRemaining;
    const auto gliding = dropRemaining > 0.0;
    auto increment = nominalIncrement * (1.0 + dropExcess);

    /**
        The hit's own lowpass, opened by how hard it was struck.

        Hoisted with the other per-voice constants, and branched on once per sample
        instead of always run: a mix of one means the source passes through untouched,
        and stepping a one-pole by `y += 1.0 * (x - y)` is only *nearly* x in floating
        point. A hit at full velocity has to come out at the bits the sample was
        recorded at, or a law about the quiet hits is a change to all of them.
    */
    const auto brightness = voice.brightnessMix;
    const auto dimmed = brightness < 1.0f;
    auto brightLeft = voice.brightnessLeft;
    auto brightRight = voice.brightnessRight;

    /**
        Whether the kernel can be read once for the whole region instead of per sample.

        The read position advances by a constant, so the fraction *inside* the kernel
        is constant too whenever that constant is a whole number of source samples — an
        octave up, or a source recorded at twice the output rate. Any other rate walks
        through the table's 1024 phases and has to be looked up where it stands; only
        the phase step could be hoisted there, and a clamp and a multiply are not worth
        the state it would take to save them.

        A whole-number rate is also where the edge renormalisation below is cheapest:
        the kernel is looked up once for the whole region, so what an end of the source
        costs is one sum and one divide on the samples that need it.
    */
    alignas (16) float hoistedWeights[SincKernelTable::taps] {};
    alignas (16) float scratchWeights[SincKernelTable::taps];
    const auto constantPhase = ! unityRate
                               && ! gliding
                               && std::abs (increment - std::round (increment)) < 1.0e-9;

    if (constantPhase)
        kernelTable.getWeights (band, samplePosition - std::floor (samplePosition), hoistedWeights);

    /**
        How many output samples until the read lands on the end of the source.

        Derived once by one divide and then counted down by one, so the last six
        milliseconds of a hit have an exact countdown without paying for a divide
        on every sample. It reaches zero on the same sample the old
        `samplePosition >= sourceLength - 1` test fired on, so the taper changes
        what the listener hears at the end of a hit and nothing about when the
        voice stops.

        A countdown only counts down while the rate covering the distance is the rate
        it was derived from, so a hit that is gliding re-derives it per sample instead.
        Skipping that is not the obvious disaster it first looks like, because the
        countdown is re-derived here at the top of every call anyway: what a stale one
        costs is bounded by one block, and at a 256-sample block it costs nothing on the
        fixtures in the suite. At a 1024-sample block on a 36 ms sample it throws away
        570 of the 1576 samples that hit has — a hit whose length depends on how the
        host chunked it. One divide per sample for 30 ms is the price of not caring.
    */
    auto samplesToSourceEnd = ((double) (sourceLength - 1) - samplePosition) / increment;

    // A channel with no sustain is a one-shot: its envelope is let go the
    // moment it reaches the sustain stage, so the release setting shapes the
    // tail instead of the note ringing at a fixed level for ever.
    const auto holds = channelHolds (voice.channel);
    const auto fading = voice.inTail;

    for (int i = 0; i < numSamples; ++i)
    {
        // Out of source, or out of ramp: either way the level has already been
        // taken to nothing by the fade below, so stopping here is not a cut.
        if (samplesToSourceEnd <= 0.0 || (fading && voice.fadeRemaining <= 0.0))
        {
            voice.active = false;
            voice.sample = nullptr;
            voice.fadeRemaining = 0.0;
            break;
        }

        auto gain = voice.gain;

        if (fading)
        {
            gain *= (float) (voice.fadeRemaining / voice.fadeLength);
        }
        else
        {
            // Check the stage before taking the sample, so a one-shot is already
            // heading into its release as it arrives at the end of its decay.
            if (voice.envelope.getStage() == Envelope::Stage::sustain
                && (! holds || voice.released))
            {
                voice.envelope.noteOff();
                voice.released = true;
            }

            gain *= voice.envelope.getNextSample();
        }

        // The end of the source is an ending, not a cut: over the last few
        // milliseconds the level is taken to zero as the read approaches it.
        if (samplesToSourceEnd < endFadeSamples)
            gain *= (float) (samplesToSourceEnd / endFadeSamples);

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

            if (! constantPhase)
                kernelTable.getWeights (band, samplePosition - floorPosition, scratchWeights);

            const auto* weights = constantPhase ? hoistedWeights : scratchWeights;

            /**
                Tap 11 is the source sample just below the read position, so the kernel
                is centred on it and adds no delay of its own.

                Near either end of the source some taps have nowhere to read from. They
                used to be skipped and the result left as it stood, which broke the
                unity sum the table is normalised to, so the first samples of a pitched
                hit were not at the level the hit is at everywhere else. Measured on a
                source flat at 0.6355 read an octave up, the first four sounding
                samples came out 0.6824, 0.6176, 0.6409, 0.6345: 7.4% hot, then 2.8%
                cold, on a signal that cannot move. Hot because the taps that hang off
                the start are mostly negative sidelobe — dropping them raises the sum
                above unity — and the sequence is the kernel walking back towards its
                own normalisation as more of it lands inside. A one-sample step of
                0.0648 on a constant is the transient of every pitched hit, so the
                weights that were actually read are added up and the result divided
                back by them, which puts the kernel's DC gain at one again for a
                truncated window.

                Nothing here is a step. A tap leaves the window at the outer edge of the
                kernel, where the cosine window has already taken its weight to nothing,
                so the partial sum passes continuously through unity as the read moves in
                or out of an end — and while every tap is inside, the branch below neither
                sums nor divides, so the fix costs nothing over the body of a hit.
            */
            const auto firstIndex = (int) floorPosition - (SincKernelTable::halfWidth - 1);

            if (firstIndex >= 0 && firstIndex + SincKernelTable::taps <= sourceLength)
            {
                const auto* baseLeft = left + firstIndex;
                const auto* baseRight = right + firstIndex;

                for (int tap = 0; tap < SincKernelTable::taps; ++tap)
                {
                    leftSample  += baseLeft[tap]  * weights[tap];
                    rightSample += baseRight[tap] * weights[tap];
                }
            }
            else
            {
                float used = 0.0f;

                for (int tap = 0; tap < SincKernelTable::taps; ++tap)
                {
                    const auto index = firstIndex + tap;

                    if (index < 0 || index >= sourceLength)
                        continue;

                    const auto weight = weights[tap];
                    leftSample  += left[index]  * weight;
                    rightSample += right[index] * weight;
                    used += weight;
                }

                // Below unity whenever taps are simply missing, above it when what is
                // missing is a negative sidelobe — both are the same break in the DC
                // gain, so both are put back. The floor is what keeps the division honest:
                // a partial sum near nothing would amplify whatever remainder is left,
                // and it cannot get near nothing while the centre tap is readable, which
                // is every read the engine makes.
                if (used > 0.25f && std::abs (used - 1.0f) > 1.0e-6f)
                {
                    const auto restore = 1.0f / used;
                    leftSample  *= restore;
                    rightSample *= restore;
                }
            }
        }

        if (dimmed)
        {
            brightLeft  += brightness * (leftSample  - brightLeft);
            brightRight += brightness * (rightSample - brightRight);
            leftSample = brightLeft;
            rightSample = brightRight;
        }

        outLeft[i] += leftSample * gain;

        if (outRight != nullptr)
            outRight[i] += rightSample * gain;

        samplePosition += increment;

        if (dropRemaining > 0.0)
        {
            // The same recursion the envelope falls use, including the snap: the
            // excess is taken to exactly nothing on the last sample of the sweep so
            // that the rate the hit settles at is the lane's rate to the bit, and not
            // 0.67% of a semitone shy of it for the rest of the sample.
            dropExcess *= pitchDropCoefficient;
            dropRemaining -= 1.0;

            if (dropRemaining <= 0.0)
            {
                dropExcess = 0.0;
                dropRemaining = 0.0;
            }

            increment = nominalIncrement * (1.0 + dropExcess);
            samplesToSourceEnd = ((double) (sourceLength - 1) - samplePosition) / increment;

            // The filter follows the rate instead of staying where the top of the sweep
            // put it, so the tail is no longer anti-aliased for a rate it left thirty
            // milliseconds ago: pinned, a drop of 12 cost 3.3 dB of top end across the
            // whole hit, which makes a tuning control a tone control. It is still never
            // looser than the rate it is reading at, and the switch is not a step —
            // these kernels are symmetric about the same centre tap, so below the
            // tighter cutoff they are the same signal, which is why the sine in the
            // sweep still measures as one uninterrupted glide.
            band = SincKernelTable::bandForRatio (increment);
        }
        else
        {
            samplesToSourceEnd -= 1.0;
        }

        if (fading)
        {
            voice.fadeRemaining -= 1.0;
        }
        else
        {
            // Once the envelope has run out there is nothing left to hear, so the
            // voice stops even if the sample has more left in it.
            if (! voice.envelope.isActive())
            {
                voice.active = false;
                voice.sample = nullptr;
                break;
            }
        }
    }

    voice.position = samplePosition;
    voice.dropExcess = dropExcess;
    voice.dropRemaining = dropRemaining;

    // The band the sweep ended on is the band the tail is read with. Without this the
    // tracking only lasts as long as the sweep does: the next call would come back in
    // with the voice still holding the cutoff picked for the top of the fall, which is
    // the pinning the tracking was added to get rid of.
    if (gliding)
        voice.sincBand = band;

    voice.brightnessLeft = brightLeft;
    voice.brightnessRight = brightRight;
}

//==============================================================================
// Block processing

void SequencerEngine::renderBlock (juce::AudioBuffer<float>& buffer, int numSamples, bool followGrid)
{
    // One pass over the block, split at every sample where something starts: the
    // step onsets the pattern still owes, when the transport is running, and the
    // arrival samples of the notes collected for this block. Every voice already
    // sounding is carried through all of the regions, so a note costs its own
    // samples and nothing is summed twice.
    //
    // Each turn of the loop consumes at least one onset or one note event, so it
    // ends after at most a bar of onsets plus the events a block can hold; the
    // guard says so rather than trusting it.
    int rendered = 0;

    for (int guard = 0; guard < Layout::numSteps + maxNoteEvents + 2 && rendered < numSamples; ++guard)
    {
        const auto onset = followGrid
                             ? rendered + (int) std::floor (distanceToNextOnsetInSteps (position) * samplesPerStep)
                             : numSamples;

        const auto boundary = juce::jmin (onset, nextNoteEventOffset (numSamples));

        // A start has to land strictly inside what is left; one on the sample after
        // the block belongs to the next block.
        if (boundary >= numSamples)
            break;

        if (boundary > rendered)
        {
            renderVoices (buffer, rendered, boundary - rendered);

            // The head is the pattern's clock, so a block walked with no grid to
            // follow — a stopped transport, where only notes sound — advances nothing.
            if (followGrid)
                position += (double) (boundary - rendered) / samplesPerStep;

            rendered = boundary;
        }

        if (onset == boundary)
            fireNextOnset();

        applyNoteEventsAt (boundary);
    }

    // Whatever is left of the block, including a block with nothing due in it.
    if (rendered < numSamples)
    {
        const auto tail = numSamples - rendered;
        renderVoices (buffer, rendered, tail);

        if (followGrid)
            position += (double) tail / samplesPerStep;
    }
}

void SequencerEngine::processBlock (juce::AudioBuffer<float>& buffer,
                                    const juce::AudioPlayHead::PositionInfo& positionInfo,
                                    bool haveHostPosition,
                                    const juce::MidiBuffer& midi)
{
    const auto numSamples = buffer.getNumSamples();

    if (numSamples <= 0 || buffer.getNumChannels() == 0)
        return;

    // Once for the whole block, so a channel that sounds in any chunk of it stays
    // reported as sounding until the next block begins.
    for (auto& flag : soundingChannels)
        flag.store (false);

    // Tempo comes from the host when it has one, from the editor otherwise.
    const auto hostBpm = positionInfo.getBpm();
    const auto tempo = (haveHostPosition && hostBpm.hasValue()) ? *hostBpm : state.manualTempo.load();

    beginBar (tempo);

    // Collected before anything is rendered: the walk has to know where every note
    // starts before it can split the block at them.
    numNoteEvents = collectNoteEvents (midi, numSamples);
    nextNoteEvent = 0;

    // The host transport always wins when it is available.
    const auto transportRunning = haveHostPosition ? positionInfo.getIsPlaying() : state.playing.load();

    if (! transportRunning)
    {
        // Grid voices are let go so stopping is not abrupt. Voices played from a
        // note are left alone: they were not started by the transport, so stopping
        // it must not silence them. The notes collected above still sound.
        for (auto& voice : voices)
            if (voice.active && ! voice.fromMidi && ! voice.released)
            {
                voice.envelope.noteOff();
                voice.released = true;
            }

        renderBlock (buffer, numSamples, false);

        lastFired = -1;
        onsetStep = -1;
        currentStep.store (-1);

        // Nothing here predicts where the host puts the head next, so the block
        // that resumes has to re-derive the armed onset from wherever it lands.
        havePrediction = false;

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
    // Find the playback head, and whether this engine is still in step with it.
    //----------------------------------------------------------------------
    const auto ppq = positionInfo.getPpqPosition();

    if (haveHostPosition && ppq.hasValue())
    {
        const auto hostHead = (*ppq) * 4.0;

        // The engine's own walk is the only thing that knows where the head was
        // going to be, and the host is the only thing that knows where it is. When
        // the two disagree by more than rounding, the head moved — a seek, a
        // punch-in, a loop point, or the first block after a stop — and the armed
        // onset has to come from the new head rather than being counted on from the
        // old one.
        const auto slipped = ! havePrediction
                               || std::abs (hostHead - predictedHead) > positionSlipInSteps();

        // Taking over from the built-in transport is a jump even when the numbers
        // happen to agree: the two clocks were not running from the same start.
        const auto tookOver = ! hostDriven;

        position = hostHead;
        hostDriven = true;

        if (tookOver)
            lastFired = -1;

        if (slipped || tookOver)
            onsetStep = firstStepAtOrAfter (position);
    }
    else
    {
        // No host position, so the built-in transport owns the head and the walk
        // keeps going from where it left off; nothing has moved.
        hostDriven = false;
    }

    //----------------------------------------------------------------------
    // Render the block once, splitting it at each onset so a hit starts on the
    // exact sample.
    //
    // The head advances by nothing but whole rendered sample counts. An onset is
    // quantised to the sample it falls on and the sub-sample remainder is left
    // behind rather than folded into the head, because a remainder carried forward
    // is a remainder that accumulates: an earlier version added the ideal onset
    // distance and then subtracted the samples rendered so far, which leaked up to
    // one sample per onset and drifted a third of a step over thirty-two bars.
    //----------------------------------------------------------------------
    renderBlock (buffer, numSamples, true);

    // What the next block's head has to be if the host and this engine agree about
    // the tempo. Recorded here because this is the only place the walk ends.
    predictedHead = position;
    havePrediction = true;
}

} // namespace gb
