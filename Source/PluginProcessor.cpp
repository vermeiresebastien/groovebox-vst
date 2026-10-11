#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace gb
{

GrooveboxProcessor::GrooveboxProcessor()
    : juce::AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      engine (samples, sequencerState)
{
    isStandalone = (wrapperType == wrapperType_Standalone);

    // Start with an empty pattern but a sensible eight-piece layout: the web app
    // ships three lanes, so give the user the classic kick/snare/hat plus named
    // slots they can fill by dropping samples on them.
    static const char* extraNames[] = { "Kick", "Snare", "Hat", "Clap", "Tom", "Rim",
                                        "Cowbell", "Conga", "Shaker", "Crash", "Ride", "Perc" };

    for (int i = 0; i < Layout::numChannels; ++i)
        samples.setChannelName (i, extraNames[i]);

    rebuildValueTree();
}

GrooveboxProcessor::~GrooveboxProcessor() = default;

//==============================================================================
// Audio

void GrooveboxProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    lastKnownSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    engine.prepare (lastKnownSampleRate, samplesPerBlock);
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
    engine.prepare (lastKnownSampleRate, 512);
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

    // Pattern edits are deliberately outside the undo manager: painting a run of
    // steps would otherwise create one undo entry per cell.
    sequencerState.pattern[(size_t) channel][(size_t) step].store (shouldBeOn);
    pushPatternToTree();
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

    pushPatternToTree();

    if (onStateChanged != nullptr)
        onStateChanged();
}

void GrooveboxProcessor::clearChannelRow (int channel)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    for (int step = 0; step < Layout::numSteps; ++step)
        sequencerState.pattern[(size_t) channel][(size_t) step].store (false);

    pushPatternToTree();

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

    // Velocity edits are as frequent as pattern edits, so they stay out of the
    // undo manager for the same reason: a drag would create one entry per cell.
    sequencerState.velocity[(size_t) channel][(size_t) step].store (juce::jlimit (0.0f, 1.0f, velocity));
    pushPatternToTree();
}

void GrooveboxProcessor::resetVelocities()
{
    for (int channel = 0; channel < Layout::numChannels; ++channel)
        for (int step = 0; step < Layout::numSteps; ++step)
            sequencerState.velocity[(size_t) channel][(size_t) step].store (1.0f);

    pushPatternToTree();
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

    pushPatternToTree();

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
    pushChannelToTree (channel);

    if (onStateChanged != nullptr)
        onStateChanged();
}

void GrooveboxProcessor::setChannelColour (int channel, juce::Colour colour)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    samples.setChannelColour (channel, colour);
    pushChannelToTree (channel);

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

    sequencerState.pitch[(size_t) channel].store (juce::jlimit (-36, 12, semitones));
    pushChannelToTree (channel);

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

    sequencerState.level[(size_t) channel].store (juce::jlimit (0.0f, 1.5f, linearGain));
    pushChannelToTree (channel);
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

    sequencerState.channelMuted[channel].store (shouldBeMuted);
    pushChannelToTree (channel);
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

    sequencerState.attack[(size_t) channel].store (juce::jlimit (0.0f, 1.0f, normalised));
    pushChannelToTree (channel);
}

void GrooveboxProcessor::setChannelDecay (int channel, float normalised)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    sequencerState.decay[(size_t) channel].store (juce::jlimit (0.0f, 1.0f, normalised));
    pushChannelToTree (channel);
}

void GrooveboxProcessor::setChannelSustain (int channel, float level)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    sequencerState.sustain[(size_t) channel].store (juce::jlimit (0.0f, 1.0f, level));
    pushChannelToTree (channel);
}

void GrooveboxProcessor::setChannelRelease (int channel, float normalised)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    sequencerState.release[(size_t) channel].store (juce::jlimit (0.0f, 1.0f, normalised));
    pushChannelToTree (channel);
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
    pushChannelToTree (channel);

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
    pushChannelToTree (channel);

    if (onStateChanged != nullptr)
        onStateChanged();

    return true;
}

//==============================================================================
// Groove

void GrooveboxProcessor::setSwing (float amount)
{
    sequencerState.swing.store (juce::jlimit (0.0f, 1.0f, amount));
    stateTree.setProperty ("swing", sequencerState.swing.load(), &undoManager);
}

void GrooveboxProcessor::setSwingGrid (int stepsPerPair)
{
    sequencerState.swingGrid.store (stepsPerPair == 2 ? 2 : 4);
    stateTree.setProperty ("swingGrid", sequencerState.swingGrid.load(), &undoManager);
}

void GrooveboxProcessor::setManualTempo (double bpm)
{
    sequencerState.manualTempo.store (juce::jlimit (20.0, 400.0, bpm));
    stateTree.setProperty ("tempo", sequencerState.manualTempo.load(), &undoManager);
}

//==============================================================================
// Undo helper

void GrooveboxProcessor::beginTransaction (const juce::String& name)
{
    undoManager.beginNewTransaction (name);
}

//==============================================================================
// Persistence

void GrooveboxProcessor::rebuildValueTree()
{
    stateTree = juce::ValueTree ("GROOVEBOX");
    stateTree.setProperty ("swing", sequencerState.swing.load(), nullptr);
    stateTree.setProperty ("swingGrid", sequencerState.swingGrid.load(), nullptr);
    stateTree.setProperty ("tempo", sequencerState.manualTempo.load(), nullptr);
    stateTree.setProperty ("pattern", "", nullptr);

    for (int channel = 0; channel < Layout::numChannels; ++channel)
        pushChannelToTree (channel);

    pushPatternToTree();
}

void GrooveboxProcessor::pushChannelToTree (int channel)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    const auto id = "channel" + juce::String (channel);
    auto child = stateTree.getChildWithName (id);

    if (! child.isValid())
    {
        child = juce::ValueTree (id);
        stateTree.appendChild (child, nullptr);
    }

    child.setProperty ("sampleId", samples.getChannelSampleId (channel), &undoManager);
    child.setProperty ("sampleName", samples.getSampleName (samples.getChannelSampleId (channel)), &undoManager);
    child.setProperty ("samplePath", samples.getSamplePath (samples.getChannelSampleId (channel)), &undoManager);
    child.setProperty ("name", samples.getChannelName (channel), &undoManager);
    child.setProperty ("colour", samples.getChannelColour (channel).toString(), &undoManager);
    child.setProperty ("pitch", sequencerState.pitch[(size_t) channel].load(), &undoManager);
    child.setProperty ("level", sequencerState.level[(size_t) channel].load(), &undoManager);
    child.setProperty ("mute", sequencerState.channelMuted[channel].load(), &undoManager);
    child.setProperty ("attack", sequencerState.attack[(size_t) channel].load(), &undoManager);
    child.setProperty ("decay", sequencerState.decay[(size_t) channel].load(), &undoManager);
    child.setProperty ("sustain", sequencerState.sustain[(size_t) channel].load(), &undoManager);
    child.setProperty ("release", sequencerState.release[(size_t) channel].load(), &undoManager);
}

void GrooveboxProcessor::pushPatternToTree()
{
    // Two characters per step: 00 for an off step, 01..99 for a step that fires,
    // carrying its velocity. Fixed width, so a row is always 32 characters and the
    // decoder never has to guess where it is.
    //
    // An on step never stores 00, so that value keeps meaning "off" and a step can
    // never be silently switched off by dragging its velocity to the bottom.
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

    stateTree.setProperty ("pattern", encoded, &undoManager);
}

void GrooveboxProcessor::applyTree (const juce::ValueTree& tree)
{
    if (! tree.hasType ("GROOVEBOX"))
        return;

    sequencerState.swing.store ((float) tree.getProperty ("swing", 0.0));
    sequencerState.swingGrid.store ((int) tree.getProperty ("swingGrid", 4));
    sequencerState.manualTempo.store ((double) tree.getProperty ("tempo", 120.0));

    // Start from nothing, then read whatever the pattern property holds.
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
            sequencerState.velocity[(size_t) channel][(size_t) step].store (on ? (float) level / 100.0f : 1.0f);        }
    }

    for (int channel = 0; channel < Layout::numChannels; ++channel)
    {
        const auto child = tree.getChildWithName ("channel" + juce::String (channel));

        if (! child.isValid())
            continue;

        samples.setChannelName (channel, child.getProperty ("name", samples.getChannelName (channel)).toString());
        samples.setChannelColour (channel, juce::Colour::fromString (child.getProperty ("colour", "fffd8eff").toString()));
        sequencerState.pitch[(size_t) channel].store ((int) child.getProperty ("pitch", 0));
        sequencerState.level[(size_t) channel].store ((float) (double) child.getProperty ("level", 1.0));
        sequencerState.channelMuted[channel].store ((bool) child.getProperty ("mute", false));

        // Envelopes were added after the first release, so a session saved before
        // that has no values here and falls back to the drum defaults.
        sequencerState.attack[(size_t) channel].store (
            (float) (double) child.getProperty ("attack", (double) Layout::defaultAttack));
        sequencerState.decay[(size_t) channel].store (
            (float) (double) child.getProperty ("decay", (double) Layout::defaultDecay));
        sequencerState.sustain[(size_t) channel].store (
            (float) (double) child.getProperty ("sustain", (double) Layout::defaultSustain));
        sequencerState.release[(size_t) channel].store (
            (float) (double) child.getProperty ("release", (double) Layout::defaultRelease));

        // Samples themselves are not stored inside the session: the file path is
        // remembered and reloaded from disk when it still exists.
        const auto path = child.getProperty ("samplePath").toString();

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

    stateTree = tree.createCopy();

    if (onStateChanged != nullptr)
        onStateChanged();
}

void GrooveboxProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    rebuildValueTree();

    if (auto xml = stateTree.createXml())
        copyXmlToBinary (*xml, destData);
}

void GrooveboxProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        const auto tree = juce::ValueTree::fromXml (*xml);

        if (tree.isValid())
            applyTree (tree);
    }
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
