#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace gb
{

namespace
{
    /** Every parameter id, so listeners can be registered and removed in one pass. */
    juce::StringArray allParameterIds()
    {
        juce::StringArray ids { Params::masterGainId, Params::swingId,
                                Params::swingGridId, Params::tempoId };

        for (int channel = 0; channel < Layout::numChannels; ++channel)
        {
            ids.add (Params::levelId (channel));
            ids.add (Params::pitchId (channel));
            ids.add (Params::attackId (channel));
            ids.add (Params::decayId (channel));
            ids.add (Params::sustainId (channel));
            ids.add (Params::releaseId (channel));
            ids.add (Params::muteId (channel));
        }

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

    // The parameters were built before anything was listening, so the atomics the
    // audio thread reads still hold their own defaults rather than the parameters'.
    syncStateFromParameters();

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

    // A lane id is "ch" then digits then the control name, so the digits say which
    // lane moved and the rest says what moved on it.
    if (! id.startsWith ("ch"))
        return;

    auto digits = 2;

    while (digits < id.length() && juce::CharacterFunctions::isDigit (id[digits]))
        ++digits;

    const auto channel = id.substring (2, digits).getIntValue();

    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    const auto control = id.substring (digits);

    if      (control == "Level")   sequencerState.level[(size_t) channel].store (value);
    else if (control == "Pitch")   sequencerState.pitch[(size_t) channel].store (juce::roundToInt (value));
    else if (control == "Attack")  sequencerState.attack[(size_t) channel].store (value);
    else if (control == "Decay")   sequencerState.decay[(size_t) channel].store (value);
    else if (control == "Sustain") sequencerState.sustain[(size_t) channel].store (value);
    else if (control == "Release") sequencerState.release[(size_t) channel].store (value);
    else if (control == "Mute")    sequencerState.channelMuted[channel].store (value >= 0.5f);
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

    for (int channel = 0; channel < Layout::numChannels; ++channel)
    {
        sequencerState.level[(size_t) channel].store (valueOf (Params::levelId (channel)));
        sequencerState.pitch[(size_t) channel].store (juce::roundToInt (valueOf (Params::pitchId (channel))));
        sequencerState.attack[(size_t) channel].store (valueOf (Params::attackId (channel)));
        sequencerState.decay[(size_t) channel].store (valueOf (Params::decayId (channel)));
        sequencerState.sustain[(size_t) channel].store (valueOf (Params::sustainId (channel)));
        sequencerState.release[(size_t) channel].store (valueOf (Params::releaseId (channel)));
        sequencerState.channelMuted[channel].store (valueOf (Params::muteId (channel)) >= 0.5f);
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
    engine.processBlock (buffer, havePosition ? *position : juce::AudioPlayHead::PositionInfo(), havePosition);

    // Notes play after the sequencer has had its turn, so both land in the same
    // buffer and either can be used without the other.
    engine.processMidi (midiMessages, buffer.getNumSamples());
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

bool GrooveboxProcessor::isStepOn (int channel, int step) const
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels)
        || ! juce::isPositiveAndBelow (step, Layout::numSteps))
        return false;

    return sequencerState.pattern[(size_t) channel][(size_t) step].load();
}

void GrooveboxProcessor::setStep (int channel, int step, bool shouldBeOn)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels)
        || ! juce::isPositiveAndBelow (step, Layout::numSteps))
        return;

    // One undo entry per gesture, not per cell: the grid begins a transaction when
    // the pointer goes down, so everything a single drag paints undoes together.
    sequencerState.pattern[(size_t) channel][(size_t) step].store (shouldBeOn);
    pushPatternToState();
}

void GrooveboxProcessor::toggleStep (int channel, int step)
{
    setStep (channel, step, ! isStepOn (channel, step));
}

void GrooveboxProcessor::clearPattern()
{
    for (int channel = 0; channel < Layout::numChannels; ++channel)
        for (int step = 0; step < Layout::numSteps; ++step)
            sequencerState.pattern[(size_t) channel][(size_t) step].store (false);

    pushPatternToState();

    if (onStateChanged != nullptr)
        onStateChanged();
}

void GrooveboxProcessor::clearChannelRow (int channel)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    for (int step = 0; step < Layout::numSteps; ++step)
        sequencerState.pattern[(size_t) channel][(size_t) step].store (false);

    pushPatternToState();

    if (onStateChanged != nullptr)
        onStateChanged();
}

//==============================================================================
// Velocity

float GrooveboxProcessor::getStepVelocity (int channel, int step) const
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels)
        || ! juce::isPositiveAndBelow (step, Layout::numSteps))
        return 1.0f;

    return sequencerState.velocity[(size_t) channel][(size_t) step].load();
}

void GrooveboxProcessor::setStepVelocity (int channel, int step, float velocity)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels)
        || ! juce::isPositiveAndBelow (step, Layout::numSteps))
        return;

    // Velocity shares the pattern property, so it shares the grid's gesture
    // transaction: a drag that reshapes a row undoes as one step.
    sequencerState.velocity[(size_t) channel][(size_t) step].store (juce::jlimit (0.0f, 1.0f, velocity));
    pushPatternToState();
}

void GrooveboxProcessor::resetVelocities()
{
    for (int channel = 0; channel < Layout::numChannels; ++channel)
        for (int step = 0; step < Layout::numSteps; ++step)
            sequencerState.velocity[(size_t) channel][(size_t) step].store (1.0f);

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

    for (int step = 0; step < Layout::numSteps; ++step)
    {
        on[(size_t) step] = sequencerState.pattern[(size_t) channel][(size_t) step].load();
        velocity[(size_t) step] = sequencerState.velocity[(size_t) channel][(size_t) step].load();
    }

    // Positive shifts move later; the modulo keeps it inside the bar both ways.
    for (int step = 0; step < Layout::numSteps; ++step)
    {
        const auto from = ((step - steps) % Layout::numSteps + Layout::numSteps) % Layout::numSteps;

        sequencerState.pattern[(size_t) channel][(size_t) step].store (on[(size_t) from]);
        sequencerState.velocity[(size_t) channel][(size_t) step].store (velocity[(size_t) from]);
    }

    pushPatternToState();

    if (onStateChanged != nullptr)
        onStateChanged();
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

void GrooveboxProcessor::pushPatternToState()
{
    // Two characters per step: 00 for an off step, 01..99 for a step that fires,
    // carrying its velocity. Fixed width, so a row is always 32 characters and the
    // decoder never has to guess where it is.
    //
    // An on step never stores 00, so that value keeps meaning "off" and a step can
    // never be silently switched off by dragging its velocity to the bottom.
    //
    // The pattern is a property rather than 192 parameters: a host's automation
    // list would be buried by it, and nobody draws a grid cell as a lane.
    juce::String encoded;

    for (int channel = 0; channel < Layout::numChannels; ++channel)
    {
        for (int step = 0; step < Layout::numSteps; ++step)
        {
            const auto on = sequencerState.pattern[(size_t) channel][(size_t) step].load();
            const auto velocity = sequencerState.velocity[(size_t) channel][(size_t) step].load();

            const auto level = on
                                 ? juce::jlimit (1, 99, juce::roundToInt (velocity * 100.0f))
                                 : 0;

            encoded += juce::String (level).paddedLeft ('0', 2);
        }

        if (channel < Layout::numChannels - 1)
            encoded += ',';
    }

    apvts.state.setProperty ("pattern", encoded, &undoManager);
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
    // the listener and into the atomics.
    for (int channel = 0; channel < Layout::numChannels; ++channel)
    {
        for (int step = 0; step < Layout::numSteps; ++step)
        {
            sequencerState.pattern[(size_t) channel][(size_t) step].store (false);
            sequencerState.velocity[(size_t) channel][(size_t) step].store (1.0f);
        }
    }

    const auto encoded = tree.getProperty ("pattern").toString();
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
                sequencerState.pattern[(size_t) channel][(size_t) step].store (on);
                sequencerState.velocity[(size_t) channel][(size_t) step].store (1.0f);
                continue;
            }

            const auto at = step * 2;

            if (at + 1 >= row.length())
                break;

            const auto level = row.substring (at, at + 2).getIntValue();
            const auto on = level > 0;

            sequencerState.pattern[(size_t) channel][(size_t) step].store (on);
            sequencerState.velocity[(size_t) channel][(size_t) step].store (on ? (float) level / 100.0f : 1.0f);
        }
    }

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
                juce::String error;

                if (const auto id = samples.addFile (file, error))
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
    // automate is in there; the pattern is a plain property and has to be current.
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
