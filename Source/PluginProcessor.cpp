#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace gb
{

namespace
{
    /**
        Every parameter id, so listeners can be registered and removed in one pass.

        Both halves come out of `Params` rather than being written out again here. A
        hand-list of lane ids is how the choke parameter came to be built, offered to
        the host and read by the engine without anything ever listening for it: the
        value moved, the atomics never did, and nothing except a test that writes every
        id could see the difference.

        The same trap has a global shape, and Mutate walked straight into it: a fourth
        hand-written list of the machine-wide ids meant that adding a control to
        `createLayout` gave it a parameter, a host entry and a restore path while
        leaving it deaf to every write. `Params::globalIds()` is the one list, and this
        is the only thing that says what the set is.
    */
    juce::StringArray allParameterIds()
    {
        juce::StringArray ids;

        for (const auto* id : Params::globalIds())
            ids.add (id);

        for (const auto& target : Params::laneTargets())
            ids.add (target.id);

        return ids;
    }
}

GrooveboxProcessor::GrooveboxProcessor()
    : juce::AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      engine (samples, sequencerState),
      apvts (*this, &undoManager, "GROOVEBOX", Params::createLayout())
{
    isStandalone = (wrapperType == wrapperType_Standalone);

    for (const auto& id : allParameterIds())
        apvts.addParameterListener (id, this);

    // Resolve the id tables here, on the message thread, rather than letting the
    // first parameter change do it: their static storage is built on whichever call
    // reaches it first, and this is the only thread that is guaranteed not to be
    // rendering. While we are here, check the two halves of the arrangement still
    // agree — every row names a parameter that exists, and the parameter list has
    // nothing the tables do not know about.
    const auto& targets = Params::laneTargets();

    for (const auto& target : targets)
    {
        jassert (apvts.getParameter (target.id) != nullptr);
        juce::ignoreUnused (target); // the loop is a jassert, so in a release build it reads nothing
    }

    for (const auto* id : Params::globalIds())
    {
        jassert (apvts.getParameter (id) != nullptr);
        juce::ignoreUnused (id);
    }

    jassert (apvts.parameters.size()
             == (int) Params::globalIds().size() + Params::numLaneTargets);

    // The parameters were built before anything was listening, so the atomics the
    // audio thread reads still hold their own defaults rather than the parameters'.
    syncStateFromParameters();

    // Watching the state tree is how a restored pattern gets back into the engine.
    // It goes here rather than in the initialiser list because the tree the state
    // owns only exists once `apvts` has been built.
    apvts.state.addListener (this);

    // Start with an empty pattern but a sensible eight-piece layout: the web app
    // ships three lanes, so give the user the classic kick/snare/hat plus named
    // slots they can fill by dropping samples on them.
    static const char* extraNames[] = { "Kick", "Snare", "Hat", "Clap", "Tom", "Rim",
                                        "Cowbell", "Conga", "Shaker", "Crash", "Ride", "Perc" };

    for (int i = 0; i < Layout::numChannels; ++i)
        samples.setChannelName (i, extraNames[i]);

    for (int channel = 0; channel < Layout::numChannels; ++channel)
        pushLaneToState (channel);

    pushPatternToState();
}

GrooveboxProcessor::~GrooveboxProcessor()
{
    // Detached before the parameter state goes, so a late notification cannot find
    // a processor on the way out.
    apvts.state.removeListener (this);

    for (const auto& id : allParameterIds())
        apvts.removeParameterListener (id, this);
}

//==============================================================================
// Parameters
//
// The parameters are what a host automates; the atomics in `sequencerState` are
// what the audio thread reads. This is the only bridge between the two, and it
// runs whichever way the value moved, so a drawn automation ramp and a fader drag
// arrive at the engine by the same route.

void GrooveboxProcessor::parameterChanged (const juce::String& id, float value)
{
    if (id == Params::masterGainId) { master.setGainDb (value); return; }
    if (id == Params::swingId)      { sequencerState.swing.store (value); return; }
    if (id == Params::swingGridId)  { sequencerState.swingGrid.store (juce::roundToInt (value)); return; }
    if (id == Params::tempoId)      { sequencerState.manualTempo.store (value); return; }
    if (id == Params::mutateId)     { sequencerState.mutate.store (juce::jlimit (Params::mutateMin, Params::mutateMax, value)); return; }
    if (id == Params::humanizeId)   { sequencerState.humanize.store (juce::jlimit (Params::humanizeMin, Params::humanizeMax, value)); return; }
    if (id == Params::touchId)      { sequencerState.touch.store (juce::jlimit (Params::touchMin, Params::touchMax, value)); return; }

    // The seed reaches the engine as the integer it is, because the key of a roll is
    // the number the user wrote down: letting it arrive as a fraction of a million
    // and rounding at the other end would make 1 and 2 the same take.
    if (id == Params::seedId)
    {
        sequencerState.seed.store (juce::jlimit (Params::seedMin, Params::seedMax,
                                                 juce::roundToInt (value)));
        return;
    }

    // A lane id is "ch" then digits then the control name, so the digits say which
    // lane moved and the rest says what moved on it — but neither half means
    // anything on its own, and reading them apart means building strings.
    //
    // This callback runs on the thread that wrote the parameter, which for a host
    // automating a ramp is the audio thread, so it must not allocate: taking a
    // substring of the id would do exactly that, twice per change, in the one place
    // the header promises it will not. The id therefore gets looked up rather than
    // parsed. `Params::laneTargets()` is the prebuilt answer, built once from the
    // same id functions that made the parameters, and comparing two strings is
    // pointer arithmetic over storage that already exists.
    //
    // The scan is over 108 short names, and it is a scan of a table that never
    // changes rather than a search through a live container, so its cost is a few
    // hundred byte comparisons in the worst case — against a block of audio that
    // costs tens of thousands of samples of resampling.
    for (const auto& target : Params::laneTargets())
    {
        if (target.id != id)
            continue;

        switch (target.control)
        {
            case Params::LaneControl::level:
                sequencerState.level[(size_t) target.channel].store (value);
                break;

            case Params::LaneControl::pitch:
                sequencerState.pitch[(size_t) target.channel].store (juce::roundToInt (value));
                break;

            case Params::LaneControl::pitchDrop:
                sequencerState.pitchDrop[(size_t) target.channel]
                    .store (juce::jlimit (Params::pitchDropMin, Params::pitchDropMax,
                                          juce::roundToInt (value)));
                break;

            case Params::LaneControl::attack:
                sequencerState.attack[(size_t) target.channel].store (value);
                break;

            case Params::LaneControl::decay:
                sequencerState.decay[(size_t) target.channel].store (value);
                break;

            case Params::LaneControl::sustain:
                sequencerState.sustain[(size_t) target.channel].store (value);
                break;

            case Params::LaneControl::release:
                sequencerState.release[(size_t) target.channel].store (value);
                break;

            case Params::LaneControl::mute:
                sequencerState.channelMuted[target.channel].store (value >= 0.5f);
                break;

            case Params::LaneControl::choke:
                sequencerState.chokeGroup[(size_t) target.channel]
                    .store (juce::jlimit (0, Layout::numChokeGroups, juce::roundToInt (value)));
                break;
        }

        return;
    }
}

void GrooveboxProcessor::syncStateFromParameters()
{
    const auto valueOf = [this] (juce::StringRef id) -> float
    {
        auto* raw = apvts.getRawParameterValue (id);
        return raw != nullptr ? raw->load() : 0.0f;
    };

    master.setGainDb (valueOf (Params::masterGainId));
    sequencerState.swing.store (valueOf (Params::swingId));
    sequencerState.swingGrid.store (juce::roundToInt (valueOf (Params::swingGridId)));
    sequencerState.manualTempo.store (valueOf (Params::tempoId));
    sequencerState.mutate.store (valueOf (Params::mutateId));
    sequencerState.humanize.store (valueOf (Params::humanizeId));
    sequencerState.touch.store (valueOf (Params::touchId));
    sequencerState.seed.store (juce::jlimit (Params::seedMin, Params::seedMax,
                                             juce::roundToInt (valueOf (Params::seedId))));

    for (int channel = 0; channel < Layout::numChannels; ++channel)
    {
        sequencerState.level[(size_t) channel].store (valueOf (Params::levelId (channel)));
        sequencerState.pitch[(size_t) channel].store (juce::roundToInt (valueOf (Params::pitchId (channel))));
        sequencerState.pitchDrop[(size_t) channel].store (
            juce::jlimit (Params::pitchDropMin, Params::pitchDropMax,
                          juce::roundToInt (valueOf (Params::pitchDropId (channel)))));
        sequencerState.attack[(size_t) channel].store (valueOf (Params::attackId (channel)));
        sequencerState.decay[(size_t) channel].store (valueOf (Params::decayId (channel)));
        sequencerState.sustain[(size_t) channel].store (valueOf (Params::sustainId (channel)));
        sequencerState.release[(size_t) channel].store (valueOf (Params::releaseId (channel)));
        sequencerState.channelMuted[channel].store (valueOf (Params::muteId (channel)) >= 0.5f);
        sequencerState.chokeGroup[(size_t) channel].store (
            juce::jlimit (0, Layout::numChokeGroups, juce::roundToInt (valueOf (Params::chokeId (channel)))));
    }
}

void GrooveboxProcessor::writeParameter (juce::StringRef parameterID, float denormalisedValue)
{
    auto* parameter = apvts.getParameter (parameterID);

    if (parameter == nullptr)
        return;

    // The parameter speaks normalised 0..1 to the host, but the caller and the
    // atomics both speak the real-world value, so translate through the range.
    const auto range = apvts.getParameterRange (parameterID);
    parameter->setValueNotifyingHost (range.convertTo0to1 (denormalisedValue));
}

//==============================================================================
// Audio

void GrooveboxProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    lastKnownSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    lastKnownBlockSize = samplesPerBlock > 0 ? samplesPerBlock : 512;
    engine.prepare (lastKnownSampleRate, lastKnownBlockSize);
    master.prepare (lastKnownSampleRate);
    prepared = true;
}

void GrooveboxProcessor::releaseResources()
{
    engine.releaseResources();
    prepared = false;
}

bool GrooveboxProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& main = layouts.getMainOutputChannelSet();
    return main == juce::AudioChannelSet::stereo() || main == juce::AudioChannelSet::mono();
}

void GrooveboxProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    // Clear the output; the engine only ever adds to it.
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        buffer.clear (channel, 0, buffer.getNumSamples());

    const auto position = getPlayHead() != nullptr
                            ? getPlayHead()->getPosition()
                            : juce::Optional<juce::AudioPlayHead::PositionInfo>();

    const auto havePosition = position.hasValue();

    // One call, one pass over the buffer: the engine splits the block at the notes
    // as well as at the step onsets, so a voice started by a note is rendered
    // exactly once alongside everything already sounding.
    engine.processBlock (buffer,
                         havePosition ? *position : juce::AudioPlayHead::PositionInfo(),
                         havePosition,
                         midiMessages);

    midiMessages.clear();

    // Last, so the trim and the meter see everything that was summed.
    master.process (buffer);

    if (havePosition)
    {
        const auto bpm = position->getBpm();

        if (bpm.hasValue())
            displayedTempo.store (*bpm);
    }
    else
    {
        displayedTempo.store (sequencerState.manualTempo.load());
    }
}

//==============================================================================
// Transport

void GrooveboxProcessor::setPlaying (bool shouldPlay)
{
    // The host's transport takes priority where it exists; this flag is what
    // drives playback when it does not (the standalone build).
    sequencerState.playing.store (shouldPlay);
}

void GrooveboxProcessor::resetTransport()
{
    sequencerState.playing.store (false);
    engine.releaseResources();

    // The host's own block size, not a guessed one: re-preparing at a size the
    // host never asked for would leave the engine configured for a block it will
    // not be given.
    engine.prepare (lastKnownSampleRate, lastKnownBlockSize);
}

bool GrooveboxProcessor::isPlaying() const noexcept
{
    if (isStandalone)
        return sequencerState.playing.load();

    if (auto* head = const_cast<GrooveboxProcessor*> (this)->getPlayHead())
    {
        const auto position = head->getPosition();

        if (position.hasValue() && position->getIsPlaying())
            return true;
    }

    return false;
}

//==============================================================================
// Pattern

int GrooveboxProcessor::selectedSlotOf (int channel) const
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return 0;

    // Clamped on the way out rather than trusted on the way in: the value arrives from
    // a string in a session file, and a lane that claims to point at slot nine should
    // play one of the four the machine has rather than read past the end of them.
    return juce::jlimit (0, Layout::numPatternSlots - 1,
                         sequencerState.selectedSlot[(size_t) channel].load());
}

bool GrooveboxProcessor::isStepOn (int channel, int step) const
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels)
        || ! juce::isPositiveAndBelow (step, Layout::numSteps))
        return false;

    return sequencerState.slots[(size_t) selectedSlotOf (channel)]
                             .pattern[(size_t) channel][(size_t) step].load();
}

void GrooveboxProcessor::setStep (int channel, int step, bool shouldBeOn)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels)
        || ! juce::isPositiveAndBelow (step, Layout::numSteps))
        return;

    // One undo entry per gesture, not per cell: the grid begins a transaction when
    // the pointer goes down, so everything a single drag paints undoes together.
    sequencerState.slots[(size_t) selectedSlotOf (channel)]
                     .pattern[(size_t) channel][(size_t) step].store (shouldBeOn);

    pushPatternToState (selectedSlotOf (channel));
}

void GrooveboxProcessor::toggleStep (int channel, int step)
{
    setStep (channel, step, ! isStepOn (channel, step));
}

void GrooveboxProcessor::clearPattern()
{
    // The whole bank, not the rows on screen: this is the button that says the machine
    // has nothing in it, and a slot that survived it would come back on the next bar of
    // a rotating lane as a pattern the user just cleared.
    for (auto& cells : sequencerState.slots)
        for (auto& row : cells.pattern)
            for (auto& cell : row)
                cell.store (false);

    pushPatternToState();

    if (onStateChanged != nullptr)
        onStateChanged();
}

void GrooveboxProcessor::clearChannelRow (int channel)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    const auto slot = selectedSlotOf (channel);

    for (int step = 0; step < Layout::numSteps; ++step)
        sequencerState.slots[(size_t) slot].pattern[(size_t) channel][(size_t) step].store (false);

    pushPatternToState (slot);

    if (onStateChanged != nullptr)
        onStateChanged();
}

//==============================================================================
// Velocity

float GrooveboxProcessor::getStepVelocityInSlot (int channel, int step, int slot) const
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels)
        || ! juce::isPositiveAndBelow (step, Layout::numSteps))
        return 1.0f;

    return sequencerState.slots[(size_t) juce::jlimit (0, Layout::numPatternSlots - 1, slot)]
                             .velocity[(size_t) channel][(size_t) step].load();
}

float GrooveboxProcessor::getStepVelocity (int channel, int step) const
{
    return getStepVelocityInSlot (channel, step, selectedSlotOf (channel));
}

void GrooveboxProcessor::setStepVelocity (int channel, int step, float velocity)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels)
        || ! juce::isPositiveAndBelow (step, Layout::numSteps))
        return;

    // Velocity shares the pattern property, so it shares the grid's gesture
    // transaction: a drag that reshapes a row undoes as one step.
    const auto slot = selectedSlotOf (channel);

    sequencerState.slots[(size_t) slot].velocity[(size_t) channel][(size_t) step]
        .store (juce::jlimit (0.0f, 1.0f, velocity));

    pushPatternToState (slot);
}

void GrooveboxProcessor::resetVelocities()
{
    for (int channel = 0; channel < Layout::numChannels; ++channel)
        for (int step = 0; step < Layout::numSteps; ++step)
            sequencerState.slots[(size_t) selectedSlotOf (channel)]
                .velocity[(size_t) channel][(size_t) step].store (1.0f);

    pushPatternToState();
}

//==============================================================================
// Probability

float GrooveboxProcessor::getStepChance (int channel, int step) const
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels)
        || ! juce::isPositiveAndBelow (step, Layout::numSteps))
        return 1.0f;

    return sequencerState.slots[(size_t) selectedSlotOf (channel)]
                             .chance[(size_t) channel][(size_t) step].load();
}

void GrooveboxProcessor::setStepChance (int channel, int step, float chance)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels)
        || ! juce::isPositiveAndBelow (step, Layout::numSteps))
        return;

    /**
        Snapped to the rungs the record can hold, before it reaches the engine.

        A step is saved as one byte of 255, so a probability that is not a whole number
        of those comes back rounded — and unlike velocity, which the panel draws as a
        bar nobody reads a number off, the probability is displayed as a percentage.
        Snapping here is what makes the number on screen the number in the file, rather
        than the nearest thing to it that survives a save.
    */
    const auto rung = (float) juce::jlimit (0, Layout::chanceLevels,
                                            juce::roundToInt (juce::jlimit (0.0f, 1.0f, chance)
                                                              * (float) Layout::chanceLevels))
                      / (float) Layout::chanceLevels;

    const auto slot = selectedSlotOf (channel);

    sequencerState.slots[(size_t) slot].chance[(size_t) channel][(size_t) step].store (rung);

    pushPatternToState (slot);
}

void GrooveboxProcessor::resetChances()
{
    for (int channel = 0; channel < Layout::numChannels; ++channel)
        for (int step = 0; step < Layout::numSteps; ++step)
            sequencerState.slots[(size_t) selectedSlotOf (channel)]
                .chance[(size_t) channel][(size_t) step].store (1.0f);

    pushPatternToState();
}

//==============================================================================
// Row shifting

void GrooveboxProcessor::shiftChannelRow (int channel, int steps)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels) || steps == 0)
        return;

    std::array<bool, (size_t) Layout::numSteps> on {};
    std::array<float, (size_t) Layout::numSteps> velocity {};
    std::array<float, (size_t) Layout::numSteps> chance {};

    // One slot, read and written under the same reference: the row that turns is the one
    // the lane points at, and the other three are other takes on the same drum.
    auto& cells = sequencerState.slots[(size_t) selectedSlotOf (channel)];

    for (int step = 0; step < Layout::numSteps; ++step)
    {
        on[(size_t) step] = cells.pattern[(size_t) channel][(size_t) step].load();
        velocity[(size_t) step] = cells.velocity[(size_t) channel][(size_t) step].load();
        chance[(size_t) step] = cells.chance[(size_t) channel][(size_t) step].load();
    }

    // Positive shifts move later; the modulo keeps it inside the bar both ways.
    for (int step = 0; step < Layout::numSteps; ++step)
    {
        const auto from = ((step - steps) % Layout::numSteps + Layout::numSteps) % Layout::numSteps;

        // Every field the cell has, not the two this function knew about when the
        // grid had two. A step that moves to a new place and loses its probability
        // arrives somewhere else than where it was written.
        cells.pattern[(size_t) channel][(size_t) step].store (on[(size_t) from]);
        cells.velocity[(size_t) channel][(size_t) step].store (velocity[(size_t) from]);
        cells.chance[(size_t) channel][(size_t) step].store (chance[(size_t) from]);
    }

    pushPatternToState (selectedSlotOf (channel));

    if (onStateChanged != nullptr)
        onStateChanged();
}

//==============================================================================
// Pattern bank

int GrooveboxProcessor::getChannelSlot (int channel) const
{
    return selectedSlotOf (channel);
}

void GrooveboxProcessor::setChannelSlot (int channel, int slot)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    const auto next = juce::jlimit (0, Layout::numPatternSlots - 1, slot);

    if (selectedSlotOf (channel) == next)
        return;

    sequencerState.selectedSlot[(size_t) channel].store (next);

    // The selection is a fact about the session, so it goes on the tree the same way
    // the cells do — and through the same guard, because the door back from the tree is
    // `applyPatternFromState`, which would otherwise read this write straight back in.
    pushBankToState();

    if (onStateChanged != nullptr)
        onStateChanged();
}

int GrooveboxProcessor::getChannelEvolveBars (int channel) const
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return 0;

    return sequencerState.evolveBars[(size_t) channel].load();
}

void GrooveboxProcessor::setChannelEvolveBars (int channel, int bars)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    const auto next = juce::jmax (0, bars);

    if (getChannelEvolveBars (channel) == next)
        return;

    sequencerState.evolveBars[(size_t) channel].store (next);
    pushBankToState();

    if (onStateChanged != nullptr)
        onStateChanged();
}

void GrooveboxProcessor::cycleChannelEvolve (int channel)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    const auto current = getChannelEvolveBars (channel);

    // Walked over the rungs the panel offers, wrapping back to off. A period that is not
    // on the ladder — one a newer session brought in — is not on the walk either, so the
    // first click lands on the rung above wherever it started rather than on itself.
    for (size_t i = 0; i < Layout::evolvePeriods.size(); ++i)
        if (Layout::evolvePeriods[i] > current)
            return setChannelEvolveBars (channel, Layout::evolvePeriods[i]);

    setChannelEvolveBars (channel, Layout::evolvePeriods.front());
}

bool GrooveboxProcessor::isChannelSlotFilled (int channel, int slot) const
{
    return engine.isSlotFilled (channel, slot);
}

int GrooveboxProcessor::getChannelSlotPlaying (int channel) const
{
    return engine.slotPlaying (engine.getCurrentBar(), channel);
}

int GrooveboxProcessor::duplicateChannelRowToNextSlot (int channel)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return -1;

    const auto from = selectedSlotOf (channel);

    for (int offset = 1; offset < Layout::numPatternSlots; ++offset)
    {
        const auto to = (from + offset) % Layout::numPatternSlots;

        if (engine.isSlotFilled (channel, to))
            continue;

        auto& cells = sequencerState.slots[(size_t) to];
        const auto& source = sequencerState.slots[(size_t) from];

        auto& pattern = cells.pattern[(size_t) channel];
        auto& velocity = cells.velocity[(size_t) channel];
        auto& chance = cells.chance[(size_t) channel];

        const auto& fromPattern = source.pattern[(size_t) channel];
        const auto& fromVelocity = source.velocity[(size_t) channel];
        const auto& fromChance = source.chance[(size_t) channel];

        // All three fields, because a copy of a row is a copy of what that row sounds
        // like: its steps, their levels and their probabilities. A duplicate that left
        // the chance behind would arrive as a different pattern than the one on screen.
        for (int step = 0; step < Layout::numSteps; ++step)
        {
            pattern[(size_t) step].store (fromPattern[(size_t) step].load());
            velocity[(size_t) step].store (fromVelocity[(size_t) step].load());
            chance[(size_t) step].store (fromChance[(size_t) step].load());
        }

        sequencerState.selectedSlot[(size_t) channel].store (to);

        pushPatternToState (to);
        pushBankToState();

        if (onStateChanged != nullptr)
            onStateChanged();

        return to;
    }

    return -1;
}

//==============================================================================
// Channels

void GrooveboxProcessor::setChannelName (int channel, const juce::String& name)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    samples.setChannelName (channel, name);
    pushLaneToState (channel);

    if (onStateChanged != nullptr)
        onStateChanged();
}

void GrooveboxProcessor::setChannelColour (int channel, juce::Colour colour)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    samples.setChannelColour (channel, colour);
    pushLaneToState (channel);

    if (onStateChanged != nullptr)
        onStateChanged();
}

juce::String GrooveboxProcessor::getChannelSampleName (int channel) const
{
    return samples.getSampleName (samples.getChannelSampleId (channel));
}

int GrooveboxProcessor::getChannelPitch (int channel) const
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return 0;

    return sequencerState.pitch[(size_t) channel].load();
}

void GrooveboxProcessor::setChannelPitch (int channel, int semitones)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    writeParameter (Params::pitchId (channel),
                                     (float) juce::jlimit (Params::pitchMin, Params::pitchMax, semitones));

    if (onStateChanged != nullptr)
        onStateChanged();
}

void GrooveboxProcessor::nudgeChannelPitch (int channel, int delta)
{
    setChannelPitch (channel, getChannelPitch (channel) + delta);
}

int GrooveboxProcessor::getChannelPitchDrop (int channel) const
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return 0;

    return sequencerState.pitchDrop[(size_t) channel].load();
}

void GrooveboxProcessor::setChannelPitchDrop (int channel, int semitones)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    writeParameter (Params::pitchDropId (channel),
                    (float) juce::jlimit (Params::pitchDropMin, Params::pitchDropMax, semitones));

    if (onStateChanged != nullptr)
        onStateChanged();
}

float GrooveboxProcessor::getChannelLevel (int channel) const
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return 1.0f;

    return sequencerState.level[(size_t) channel].load();
}

void GrooveboxProcessor::setChannelLevel (int channel, float linearGain)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    writeParameter (Params::levelId (channel),
                                     juce::jlimit (Params::levelMin, Params::levelMax, linearGain));
}

bool GrooveboxProcessor::isChannelMuted (int channel) const
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return false;

    return sequencerState.channelMuted[channel].load();
}

void GrooveboxProcessor::setChannelMuted (int channel, bool shouldBeMuted)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    writeParameter (Params::muteId (channel), shouldBeMuted ? 1.0f : 0.0f);
}

int GrooveboxProcessor::getChannelChokeGroup (int channel) const
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return 0;

    return sequencerState.chokeGroup[(size_t) channel].load();
}

void GrooveboxProcessor::setChannelChokeGroup (int channel, int group)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    writeParameter (Params::chokeId (channel),
                    (float) juce::jlimit (0, Layout::numChokeGroups, group));
}

//==============================================================================
// Envelope

float GrooveboxProcessor::getChannelAttack (int channel) const
{
    return juce::isPositiveAndBelow (channel, Layout::numChannels)
             ? sequencerState.attack[(size_t) channel].load() : 0.0f;
}

float GrooveboxProcessor::getChannelDecay (int channel) const
{
    return juce::isPositiveAndBelow (channel, Layout::numChannels)
             ? sequencerState.decay[(size_t) channel].load() : 0.0f;
}

float GrooveboxProcessor::getChannelSustain (int channel) const
{
    return juce::isPositiveAndBelow (channel, Layout::numChannels)
             ? sequencerState.sustain[(size_t) channel].load() : 0.0f;
}

float GrooveboxProcessor::getChannelRelease (int channel) const
{
    return juce::isPositiveAndBelow (channel, Layout::numChannels)
             ? sequencerState.release[(size_t) channel].load() : 0.0f;
}

void GrooveboxProcessor::setChannelAttack (int channel, float normalised)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    writeParameter (Params::attackId (channel), juce::jlimit (0.0f, 1.0f, normalised));
}

void GrooveboxProcessor::setChannelDecay (int channel, float normalised)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    writeParameter (Params::decayId (channel), juce::jlimit (0.0f, 1.0f, normalised));
}

void GrooveboxProcessor::setChannelSustain (int channel, float level)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    writeParameter (Params::sustainId (channel), juce::jlimit (0.0f, 1.0f, level));
}

void GrooveboxProcessor::setChannelRelease (int channel, float normalised)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    writeParameter (Params::releaseId (channel), juce::jlimit (0.0f, 1.0f, normalised));
}

void GrooveboxProcessor::resetChannelEnvelope (int channel)
{
    setChannelAttack (channel, Layout::defaultAttack);
    setChannelDecay (channel, Layout::defaultDecay);
    setChannelSustain (channel, Layout::defaultSustain);
    setChannelRelease (channel, Layout::defaultRelease);
}

double GrooveboxProcessor::envelopeSecondsFor (float normalised, EnvelopeStage stage)
{
    switch (stage)
    {
        case EnvelopeStage::attack:  return gb::envelopeSeconds (normalised, Layout::maxAttackSeconds);
        case EnvelopeStage::decay:   return gb::envelopeSeconds (normalised, Layout::maxDecaySeconds);
        case EnvelopeStage::release: return gb::envelopeSeconds (normalised, Layout::maxReleaseSeconds);
        case EnvelopeStage::sustain: return (double) juce::jlimit (0.0f, 1.0f, normalised);
    }

    return 0.0;
}

juce::String GrooveboxProcessor::formatEnvelopeValue (float normalised, EnvelopeStage stage)
{
    if (stage == EnvelopeStage::sustain)
        return juce::String (juce::roundToInt (juce::jlimit (0.0f, 1.0f, normalised) * 100.0f)) + "%";

    const auto seconds = envelopeSecondsFor (normalised, stage);

    if (seconds < 1.0)
        return juce::String (juce::roundToInt (seconds * 1000.0)) + " ms";

    return juce::String (seconds, 2) + " s";
}

//==============================================================================
// Sample loading

bool GrooveboxProcessor::loadSampleIntoChannel (int channel, const juce::File& file, juce::String& errorOut)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
    {
        errorOut = "Channel out of range";
        return false;
    }

    const auto id = samples.addFile (file, errorOut);

    if (id == 0)
        return false;

    samples.setChannelSample (channel, id);
    pushLaneToState (channel);

    if (onStateChanged != nullptr)
        onStateChanged();

    return true;
}

bool GrooveboxProcessor::loadSampleDataIntoChannel (int channel, const void* data, size_t numBytes,
                                                    const juce::String& nameHint, juce::String& errorOut)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
    {
        errorOut = "Channel out of range";
        return false;
    }

    const auto id = samples.addFromMemory (data, numBytes, nameHint, errorOut);

    if (id == 0)
        return false;

    samples.setChannelSample (channel, id);
    pushLaneToState (channel);

    if (onStateChanged != nullptr)
        onStateChanged();

    return true;
}

//==============================================================================
// Groove

void GrooveboxProcessor::setSwing (float amount)
{
    writeParameter (Params::swingId, juce::jlimit (0.0f, 1.0f, amount));
}

void GrooveboxProcessor::setSwingGrid (int stepsPerPair)
{
    writeParameter (Params::swingGridId, (float) (stepsPerPair == 2 ? 2 : 4));
}

void GrooveboxProcessor::setManualTempo (double bpm)
{
    writeParameter (Params::tempoId,
                                     (float) juce::jlimit ((double) Params::tempoMin, (double) Params::tempoMax, bpm));
}

float GrooveboxProcessor::getMutate() const noexcept
{
    return sequencerState.mutate.load();
}

void GrooveboxProcessor::setMutate (float amount)
{
    writeParameter (Params::mutateId,
                                     juce::jlimit (Params::mutateMin, Params::mutateMax, amount));
}

float GrooveboxProcessor::getHumanize() const noexcept
{
    return sequencerState.humanize.load();
}

void GrooveboxProcessor::setHumanize (float amount)
{
    writeParameter (Params::humanizeId,
                                     juce::jlimit (Params::humanizeMin, Params::humanizeMax, amount));
}

float GrooveboxProcessor::getTouch() const noexcept
{
    return sequencerState.touch.load();
}

void GrooveboxProcessor::setTouch (float amount)
{
    writeParameter (Params::touchId,
                                     juce::jlimit (Params::touchMin, Params::touchMax, amount));
}

int GrooveboxProcessor::getSeed() const noexcept
{
    return sequencerState.seed.load();
}

void GrooveboxProcessor::setSeed (int seed)
{
    writeParameter (Params::seedId,
                                     (float) juce::jlimit (Params::seedMin, Params::seedMax, seed));
}

int GrooveboxProcessor::rerollSeed()
{
    // A new number rather than a new pattern: what changes is which hits the machine
    // is allowed to drop, and the grid the user painted is exactly where it was.
    setSeed (juce::Random::getSystemRandom().nextInt (Params::seedMax + 1));

    return getSeed();
}

//==============================================================================
// Master

void GrooveboxProcessor::setMasterGainDb (float db)
{
    writeParameter (Params::masterGainId,
                                     juce::jlimit (Layout::minMasterGainDb, Layout::maxMasterGainDb, db));
}

juce::String GrooveboxProcessor::formatMasterGain (float db)
{
    return (db > 0.0f ? "+" : "") + juce::String (db, 1) + " dB";
}

//==============================================================================
// Undo helper

void GrooveboxProcessor::beginTransaction (const juce::String& name)
{
    undoManager.beginNewTransaction (name);
}

//==============================================================================
// Persistence

void GrooveboxProcessor::pushLaneToState (int channel)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    // Only what is not a parameter. Level, pitch, mute and the envelope live in the
    // parameter state already, and writing them here too would give a session two
    // copies of the same number to disagree about.
    const auto id = "lane" + juce::String (channel);
    auto child = apvts.state.getChildWithName (id);

    if (! child.isValid())
    {
        child = juce::ValueTree (id);
        apvts.state.appendChild (child, nullptr);
    }

    child.setProperty ("sampleId", samples.getChannelSampleId (channel), &undoManager);
    child.setProperty ("sampleName", samples.getSampleName (samples.getChannelSampleId (channel)), &undoManager);
    child.setProperty ("samplePath", samples.getSamplePath (samples.getChannelSampleId (channel)), &undoManager);
    child.setProperty ("name", samples.getChannelName (channel), &undoManager);
    child.setProperty ("colour", samples.getChannelColour (channel).toString(), &undoManager);
}

namespace
{
    /**
        How wide a step is in each generation of the record, and the tag that says so.

        `v<n>:` is the same number as `Layout::stateVersion`, built from it rather than
        written next to it, because the two disagreeing is the one bug in this design
        that no test of the payload itself would catch.

        The width is a function of the codec rather than a constant, because the
        question a reader has to answer is "how many bytes is a step in *this* record",
        and the answer differs between the generations it can read: v2 has no
        probability to carry. Writing the current width everywhere instead would mean a
        v2 session is read with the wrong stride, which is not a silent truncation but
        a grid of nonsense — the worst kind of mistake a codec can make.
    */
    constexpr int strideForCodec (int codec) noexcept
    {
        switch (codec)
        {
            case 3:  return 3;   // flag, level, probability
            case 2:  return 2;   // flag, level
            default: return 0;   // untagged rows, or a generation this build cannot name
        }
    }

    juce::String stepPayloadTag()
    {
        return "v" + juce::String (Layout::stateVersion) + ":";
    }

    /**
        The codec a payload names for itself, or 0 when it names none.

        Only a `v<digits>:` at the very front counts. Sessions from before there was a
        tag are rows of digits and commas, so they answer 0 here and are read by their
        shape the way they always were — which is exactly the guessing the tag exists
        to end for anything written from now on.
    */
    int stepPayloadCodecOf (const juce::String& encoded)
    {
        if (encoded.isEmpty() || encoded[0] != 'v')
            return 0;

        auto at = 1;

        while (at < encoded.length() && encoded[at] >= '0' && encoded[at] <= '9')
            ++at;

        if (at == 1 || at >= encoded.length() || encoded[at] != ':')
            return 0;

        return encoded.substring (1, at).getIntValue();
    }
}

juce::String GrooveboxProcessor::slotPropertyName (int slot)
{
    // Slot 0 keeps the name, and so keeps the bytes: see the header for what that buys.
    return slot == 0 ? juce::String ("pattern")
                     : juce::String ("pattern") + juce::String (slot);
}

bool GrooveboxProcessor::slotCarriesAnything (int slot) const
{
    const auto& cells = sequencerState.slots[(size_t) juce::jlimit (0, Layout::numPatternSlots - 1, slot)];

    // The same per-cell question the engine asks a single lane's row before it paints a
    // dot or walks a rotation, so "this slot has a record" and "this slot is not empty"
    // cannot be two different answers — see `Layout::cellSaysAnything`.
    for (int channel = 0; channel < Layout::numChannels; ++channel)
        for (int step = 0; step < Layout::numSteps; ++step)
            if (Layout::cellSaysAnything (cells.pattern[(size_t) channel][(size_t) step].load(),
                                          cells.velocity[(size_t) channel][(size_t) step].load(),
                                          cells.chance[(size_t) channel][(size_t) step].load()))
                return true;

    return false;
}

void GrooveboxProcessor::pushPatternToState()
{
    // Every slot this build is responsible for. The ones it is holding are skipped, and
    // skipped by name rather than by a flag on the whole machine: a session can carry one
    // record from a build that knows more about a step than this one does while the user
    // edits another slot that came from a build this one can talk to.
    for (auto slot = 0; slot < Layout::numPatternSlots; ++slot)
        if (! holdingUnreadableSlot[(size_t) slot])
            pushPatternToState (slot);
}

void GrooveboxProcessor::pushPatternToState (int slot)
{
    slot = juce::jlimit (0, Layout::numPatternSlots - 1, slot);

    // A write is this build taking responsibility for that grid, so a payload it could
    // not read stops being kept here. This is the only place that decides, which is what
    // makes it an edit, and not a save, that takes a session over.
    holdingUnreadableSlot[(size_t) slot] = false;

    const auto name = slotPropertyName (slot);

    if (slot != 0 && ! slotCarriesAnything (slot))
    {
        // An empty bank slot has no record: a pattern that is all off at full level and
        // every-time probability is exactly what a session without the property means, so
        // writing it would cost 771 characters to say nothing and clearing a slot would
        // leave its bytes in the file. Slot 0 is exempt because an empty machine has
        // always been one all-zeros payload, and taking that away would change what every
        // fresh session saves as to buy nothing.
        pushingPatternToState = true;
        apvts.state.removeProperty (juce::Identifier (name.toRawUTF8()), &undoManager);
        pushingPatternToState = false;
        return;
    }

    /**
        Three bytes a step, base64'd behind the codec's own name.

        The v1 rows this replaced spent two characters on a hundredth of a step, gave
        an off step no level at all, and left the format to be guessed from how long a
        row happened to be. v2 put the flag and the level in separate bytes, so a step
        that is switched off keeps the level it will come back at and a level is a MIDI
        number. v3 adds the probability, as a number of 255ths so that the byte fills
        the field: 255 is a step that fires every time, which is also what a step from
        a v2 record means and what every cell of a new session starts as.

        Base64 rather than digits or hex: 576 bytes is 768 characters against v1's 395,
        which is the price of the two extra fields, and every one of those characters is
        safe inside an XML attribute.

        A bank slot is written with the same record and the same tag, because the bank is
        a third dimension of the same grid and not a new format: a build that cannot see
        slot two still reads slot one exactly as it always did.
    */
    constexpr auto stride = strideForCodec (Layout::stateVersion);

    std::array<juce::uint8, (size_t) Layout::numChannels * Layout::numSteps * stride> bytes {};

    const auto& cells = sequencerState.slots[(size_t) slot];

    for (int channel = 0; channel < Layout::numChannels; ++channel)
    {
        for (int step = 0; step < Layout::numSteps; ++step)
        {
            const auto at = (size_t) ((channel * Layout::numSteps) + step) * stride;

            bytes[at] = (juce::uint8) (cells.pattern[(size_t) channel][(size_t) step].load() ? 1 : 0);
            bytes[at + 1] = Layout::levelByte (cells.velocity[(size_t) channel][(size_t) step].load());
            bytes[at + 2] = Layout::chanceByte (cells.chance[(size_t) channel][(size_t) step].load());
        }
    }

    const auto encoded = stepPayloadTag()
                           + juce::Base64::toBase64 (bytes.data(), bytes.size());

    // The flag says this write is the atomics going *out*, not a pattern arriving.
    // Without it the listener below would read it straight back in on every cell of
    // a drag, and the undo manager would see a change the grid had just made.
    pushingPatternToState = true;

    // The root's version belongs with the record it was written for. It is a header on
    // `pattern`, not on the bank: nothing reads it to decide how to decode anything —
    // that is the tag inside each payload — so stamping it here would claim authorship of
    // a v99 record still sitting unread in the same tree.
    if (slot == 0)
        apvts.state.setProperty ("stateVersion", Layout::stateVersion, &undoManager);

    apvts.state.setProperty (juce::Identifier (name.toRawUTF8()), encoded, &undoManager);

    pushingPatternToState = false;
}

void GrooveboxProcessor::applyPatternFromState()
{
    // Whole-grid, whichever property moved. One door for "the tree changed" is what keeps
    // an undo of a slot selection from leaving behind the cells of the slot it selected,
    // and the bank lists and the records are read by the same hand for the same reason.
    for (auto slot = 0; slot < Layout::numPatternSlots; ++slot)
        applySlotFromState (slot);

    applyBankFromState();
}

void GrooveboxProcessor::applySlotFromState (int slot)
{
    // Start from nothing, then read whatever this slot's record holds: a payload shorter
    // than the grid leaves the steps it does not reach off, and a slot with no record at
    // all is an empty grid rather than a stale one.
    auto& cells = sequencerState.slots[(size_t) slot];

    for (int channel = 0; channel < Layout::numChannels; ++channel)
    {
        for (int step = 0; step < Layout::numSteps; ++step)
        {
            cells.pattern[(size_t) channel][(size_t) step].store (false);
            cells.velocity[(size_t) channel][(size_t) step].store (1.0f);

            // Off every time is not the neutral value for a probability — every time
            // is, and that is what a record that never mentions one means.
            cells.chance[(size_t) channel][(size_t) step].store (1.0f);
        }
    }

    // Re-derived from the tree on every read rather than latched, because a flag that
    // only ever goes true would still be saying "hold those bytes" about a slot whose
    // bytes have since been replaced by something this build can read.
    holdingUnreadableSlot[(size_t) slot] = false;

    const auto encoded = apvts.state.getProperty (slotPropertyName (slot)).toString();

    if (encoded.isEmpty())
        return;

    if (const auto codec = stepPayloadCodecOf (encoded); codec != 0)
    {
        const auto stride = strideForCodec (codec);

        if (stride == 0)
        {
            // A record written by a build that knows more about a step than this one
            // does. Decoding it wrong and saving it back would destroy the session, so
            // nothing here touches it: the grid stays empty, because an empty grid is
            // the true picture of what this build can hear, and the bytes go back out
            // as they came in until an edit makes this build responsible for the
            // pattern — at which point `pushPatternToState` takes the row over.
            //
            // Note that a v2 record is not in this branch. It is narrower, not
            // stranger: everything it does say is readable, and the one field it does
            // not have is the one whose absence means "always".
            holdingUnreadableSlot[(size_t) slot] = true;
            return;
        }

        juce::MemoryBlock bytes;
        juce::MemoryOutputStream sink (bytes, false);

        if (! juce::Base64::convertFromBase64 (sink, encoded.fromFirstOccurrenceOf (":", false, false)))
        {
            // Tagged as ours but not decodable, which is the same problem wearing a
            // different name: this code cannot read it, so it keeps it.
            holdingUnreadableSlot[(size_t) slot] = true;
            return;
        }

        const auto* raw = static_cast<const juce::uint8*> (bytes.getData());
        const auto available = (int) bytes.getSize();

        for (int channel = 0; channel < Layout::numChannels; ++channel)
        {
            for (int step = 0; step < Layout::numSteps; ++step)
            {
                const auto at = (channel * Layout::numSteps) + step;

                // Indexed by step, not walked, so a record of any length can only ever
                // describe the steps it reaches — the same bound v1 rows were read under.
                if ((at + 1) * stride > available)
                    continue;

                const auto atByte = (size_t) at * stride;

                cells.pattern[(size_t) channel][(size_t) step].store (raw[atByte] != 0);
                cells.velocity[(size_t) channel][(size_t) step].store ((float) raw[atByte + 1] / 127.0f);

                // A v2 record has no third byte, and what it says about probability is
                // the value the loop above already stored. Reading past the stride of
                // the codec in hand is how a narrower generation gets turned into a
                // grid of nonsense.
                if (stride > 2)
                    cells.chance[(size_t) channel][(size_t) step]
                        .store ((float) raw[atByte + 2] / (float) Layout::chanceLevels);
            }
        }

        return;
    }

    if (slot != 0)
    {
        // No tag, and not the property that carries one: only slot 0 has a shape from
        // before the tag existed, so a `pattern2` that does not name its codec is a record
        // this build cannot guess its way into. Holding it costs nothing — the slot is
        // empty on screen, and an edit takes it over the same way an edit takes over an
        // unreadable `pattern`.
        holdingUnreadableSlot[(size_t) slot] = true;
        return;
    }

    const auto rows = juce::StringArray::fromTokens (encoded, ",", "");

    for (int channel = 0; channel < juce::jmin (rows.size(), Layout::numChannels); ++channel)
    {
        const auto row = rows[channel].trim();

        // Sessions written before velocities existed hold one character per step,
        // where 1 meant on. Those steps come back at full velocity.
        const auto legacy = row.length() == Layout::numSteps;

        for (int step = 0; step < Layout::numSteps; ++step)
        {
            if (legacy)
            {
                const auto on = step < row.length() && row[step] == '1';
                cells.pattern[(size_t) channel][(size_t) step].store (on);
                cells.velocity[(size_t) channel][(size_t) step].store (1.0f);
                continue;
            }

            const auto at = step * 2;

            if (at + 1 >= row.length())
                break;

            const auto level = row.substring (at, at + 2).getIntValue();
            const auto on = level > 0;

            cells.pattern[(size_t) channel][(size_t) step].store (on);
            cells.velocity[(size_t) channel][(size_t) step].store (on ? (float) level / 100.0f : 1.0f);
        }
    }
}

void GrooveboxProcessor::pushBankToState()
{
    auto writes = [this] (const juce::String& name, const std::atomic<int>* values, int defaultFor)
    {
        juce::StringArray list;
        auto anyDifferent = false;

        for (int channel = 0; channel < Layout::numChannels; ++channel)
        {
            const auto value = values[(size_t) channel].load();

            anyDifferent = anyDifferent || value != defaultFor;
            list.add (juce::String (value));
        }

        const auto id = juce::Identifier (name.toRawUTF8());

        // Sparsely, for the same reason the bank records are: twelve zeroes that mean
        // "every lane where it has always been" are not worth 23 characters of a session
        // that has never heard of them, and an absent list reads back as all-defaults.
        if (anyDifferent)
            apvts.state.setProperty (id, list.joinIntoString (","), &undoManager);
        else
            apvts.state.removeProperty (id, &undoManager);
    };

    pushingPatternToState = true;
    writes ("slots", sequencerState.selectedSlot.data(), 0);
    writes ("evolve", sequencerState.evolveBars.data(), 0);
    pushingPatternToState = false;
}

void GrooveboxProcessor::applyBankFromState()
{
    auto reads = [this] (const juce::String& name, std::atomic<int>* values)
    {
        const auto encoded = apvts.state.getProperty (juce::Identifier (name.toRawUTF8())).toString();
        const auto list = juce::StringArray::fromTokens (encoded, ",", "");

        for (int channel = 0; channel < Layout::numChannels; ++channel)
        {
            // Only the sign is repaired. A lane that points past slot three, or a period
            // this build has never offered, stays as it came in: the readers clamp for
            // themselves, and the number the next save writes back is the one a newer
            // build wrote — which is the same bargain the records strike with a v99 tag.
            const auto value = channel < list.size() ? list[channel].trim().getIntValue() : 0;

            values[(size_t) channel].store (juce::jmax (0, value));
        }
    };

    reads ("slots", sequencerState.selectedSlot.data());
    reads ("evolve", sequencerState.evolveBars.data());
}

bool GrooveboxProcessor::isGridProperty (const juce::Identifier& property)
{
    // Built from `slotPropertyName` rather than written out beside it: the names the
    // listener watches and the names the writer writes have to be the same list, and a
    // second copy of it is where the fourth slot gets left off.
    for (auto slot = 0; slot < Layout::numPatternSlots; ++slot)
        if (property == juce::Identifier (slotPropertyName (slot).toRawUTF8()))
            return true;

    return property == juce::Identifier ("slots") || property == juce::Identifier ("evolve");
}

void GrooveboxProcessor::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    /**
        The pattern is the one thing in the state that is not a parameter, which
        makes it the one thing an undo cannot put back on its own: the undo manager
        restores the string, and the engine plays the atomics. Watching the property
        is what closes that gap, and it is what makes every later per-step feature —
        probability, ratchet, a payload wide enough to hold them — undoable through
        the same door, because none of them will be a parameter either.

        The bank added five more of those strings — three slot records and the two lane
        lists — and every one of them needs the same door, which is why the test is a
        function of the names rather than a comparison against one of them.

        The parameters need no watching: moving one already calls
        `parameterChanged`, which is where the atomics for level, pitch and the
        envelope live.
    */
    // Spelled as an Identifier rather than a string literal: `Identifier` converts
    // to and from `const char*`, so a bare literal on the right of `!=` leaves MSVC
    // with two equally good ways to make the comparison.
    if (! tree.hasType ("GROOVEBOX") || ! isGridProperty (property)
         || pushingPatternToState)
        return;

    applyPatternFromState();

    // The grid painted the cell it was asked to and knows nothing about an undo, so
    // this is the only way the panel finds out the pattern moved under it.
    if (onStateChanged != nullptr)
        onStateChanged();
}

juce::ValueTree GrooveboxProcessor::migrateLegacyState (const juce::ValueTree& legacy)
{
    auto migrated = juce::ValueTree ("GROOVEBOX");

    // The pattern was never a parameter, so it carries across as it is.
    migrated.setProperty ("pattern", legacy.getProperty ("pattern", ""), nullptr);

    /**
        One parameter child, in the shape the parameter state reads.

        "PARAM" with "id" and "value" is how AudioProcessorValueTreeState writes its
        own values to XML, so a translated session is indistinguishable from one
        saved by this build.
    */
    const auto addParam = [&migrated] (juce::StringRef id, double value)
    {
        auto child = juce::ValueTree ("PARAM");
        child.setProperty ("id", juce::String (id), nullptr);
        child.setProperty ("value", value, nullptr);
        migrated.appendChild (child, nullptr);
    };

    if (legacy.hasProperty ("swing"))
        addParam (Params::swingId, (double) (float) legacy.getProperty ("swing"));

    if (legacy.hasProperty ("swingGrid"))
        addParam (Params::swingGridId, (double) (int) legacy.getProperty ("swingGrid"));

    if (legacy.hasProperty ("tempo"))
        addParam (Params::tempoId, (double) (float) legacy.getProperty ("tempo"));

    // A session from before the trim existed has nothing here, and the parameter's
    // own default is the level the hidden 0.8 used to give it.
    if (legacy.hasProperty ("masterGainDb"))
        addParam (Params::masterGainId, (double) (float) legacy.getProperty ("masterGainDb"));

    for (int channel = 0; channel < Layout::numChannels; ++channel)
    {
        const auto old = legacy.getChildWithName ("channel" + juce::String (channel));

        if (! old.isValid())
            continue;

        auto lane = juce::ValueTree ("lane" + juce::String (channel));

        for (const auto* key : { "name", "colour", "sampleId", "sampleName", "samplePath" })
            if (old.hasProperty (key))
                lane.setProperty (key, old.getProperty (key), nullptr);

        migrated.appendChild (lane, nullptr);

        if (old.hasProperty ("level"))   addParam (Params::levelId (channel),   (double) (float) old.getProperty ("level"));
        if (old.hasProperty ("pitch"))   addParam (Params::pitchId (channel),   (double) (int) old.getProperty ("pitch"));
        if (old.hasProperty ("attack"))  addParam (Params::attackId (channel),  (double) (float) old.getProperty ("attack"));
        if (old.hasProperty ("decay"))   addParam (Params::decayId (channel),   (double) (float) old.getProperty ("decay"));
        if (old.hasProperty ("sustain")) addParam (Params::sustainId (channel), (double) (float) old.getProperty ("sustain"));
        if (old.hasProperty ("release")) addParam (Params::releaseId (channel), (double) (float) old.getProperty ("release"));

        if (old.hasProperty ("mute"))
            addParam (Params::muteId (channel), (bool) old.getProperty ("mute") ? 1.0 : 0.0);
    }

    return migrated;
}

void GrooveboxProcessor::applyMetadata (const juce::ValueTree& tree)
{
    if (! tree.hasType ("GROOVEBOX"))
        return;

    // Start from nothing, then read whatever the pattern property holds. The
    // parameters are not read here: replacing the state already pushed them through
    // the listener and into the atomics. The pattern goes through the same reader an
    // undo uses, so a loaded session and a taken-back edit cannot end up disagreeing
    // about what a row of digits means.
    applyPatternFromState();

    for (int channel = 0; channel < Layout::numChannels; ++channel)
    {
        const auto lane = tree.getChildWithName ("lane" + juce::String (channel));

        if (! lane.isValid())
            continue;

        samples.setChannelName (channel, lane.getProperty ("name", samples.getChannelName (channel)).toString());
        samples.setChannelColour (channel, juce::Colour::fromString (lane.getProperty ("colour", "fffd8eff").toString()));

        // Samples themselves are not stored inside the session: the file path is
        // remembered and reloaded from disk when it still exists.
        const auto path = lane.getProperty ("samplePath").toString();

        if (path.isNotEmpty())
        {
            const juce::File file (path);

            if (file.existsAsFile())
            {
                // Ask the registry first. A recall hands the same twelve paths back
                // every time, and decoding them again would spend twelve append-only
                // slots per recall — the kit keeps working, but the ceiling arrives
                // after forty-odd of them and from then on nothing the user drops on
                // a lane can be loaded at all. A file that is already registered
                // under the name, length and modification time it had when it was
                // read is the same audio, so the id it already has is the answer.
                //
                // A file that changed on disk does not match, and is decoded afresh
                // under a new id, which is what a sample edited elsewhere deserves.
                juce::String error;
                const auto already = samples.findSampleForFile (file);

                if (const auto id = already > 0 ? already : samples.addFile (file, error))
                    samples.setChannelSample (channel, id);
            }
        }
    }

    if (onStateChanged != nullptr)
        onStateChanged();
}

void GrooveboxProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // `copyState` flushes the parameters into the tree, so everything the host can
    // automate is in there; the grid is a plain property and has to be current — every
    // slot of it, because a bank slot with no record is one the atomics agree is empty.
    //
    // The records this build is *holding* are the exception, and `pushPatternToState`
    // leaves them alone by name: the property already contains exactly what came in, and
    // flushing the grid over it would be the save destroying the session it was asked to
    // preserve. That has been a per-slot decision since the bank — a session can hold one
    // record from a build that knows more about a step than this one does while the user
    // edits another slot this build can talk to — and skipping the one is what keeps the
    // other.
    //
    // The two bank lists need no flushing: only `setChannelSlot`,
    // `setChannelEvolveBars` and `duplicateChannelRowToNextSlot` move them, and all three
    // push as they go.
    pushPatternToState();

    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void GrooveboxProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr)
        return;

    auto tree = juce::ValueTree::fromXml (*xml);

    if (! tree.isValid() || ! tree.hasType ("GROOVEBOX"))
        return;

    // A session written before there were parameters holds its values as ordinary
    // properties, which the parameter state would ignore — so it comes back at the
    // defaults, silently losing the mix. Translate it instead.
    const auto isLegacy = tree.hasProperty ("swing")
                            || tree.hasProperty ("masterGainDb")
                            || tree.getChildWithName ("channel0").isValid();

    if (isLegacy)
        tree = migrateLegacyState (tree);

    // Replacing the state moves every parameter, and each one calls back into
    // `parameterChanged`, so the atomics the audio thread reads follow on their own.
    apvts.replaceState (tree);

    // `replaceState` hands back a tree that has never heard of this processor: the
    // listener went with the old one. Re-attach before anything reads the new tree,
    // or an undo of a pattern loaded from a session would move the string and leave
    // the sound where it was.
    apvts.state.addListener (this);

    syncStateFromParameters();

    applyMetadata (apvts.state);
}

void GrooveboxProcessor::previewChannel (int channel, float velocity)
{
    engine.triggerChannel (channel, velocity);
}

juce::String GrooveboxProcessor::getMidiNoteNameForChannel (int channel)
{
    const auto note = SequencerEngine::midiNoteForChannel (channel);

    if (note < 0)
        return {};

    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    // Octave numbering follows the convention DAWs use, where middle C (MIDI 60)
    // is C3 and the first channel is C1. Scientific pitch notation would call
    // these C2 and C4.
    return juce::String (names[note % 12]) + juce::String (note / 12 - 2);
}

juce::AudioProcessorEditor* GrooveboxProcessor::createEditor()
{
    return new GrooveboxEditor (*this);
}

} // namespace gb

//==============================================================================
// Plugin entry point used by every JUCE plugin wrapper.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new gb::GrooveboxProcessor();
}
