#pragma once

#include "Common.h"
#include "MasterBus.h"
#include "Parameters.h"
#include "SampleStore.h"
#include "SequencerEngine.h"

namespace gb
{

/** Which part of the amplitude envelope a value belongs to. */
enum class EnvelopeStage
{
    attack,
    decay,
    sustain,
    release,
};

class GrooveboxEditor;

/**
    The plugin: a 16-step, 12-channel sample drum machine.

    State layout mirrors the web app's drum page — each channel has a sample, a
    name, a colour and a pitch offset, plus a row of 16 steps. The processor
    exposes the atomics the editor writes directly, and keeps a ValueTree copy
    in sync so sessions and presets persist.
*/
class GrooveboxProcessor : public juce::AudioProcessor,
                           private juce::AudioProcessorValueTreeState::Listener,
                           private juce::ValueTree::Listener
{
public:
    GrooveboxProcessor();
    ~GrooveboxProcessor() override;

    //==========================================================================
    // AudioProcessor
    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Groovebox"; }
    /** The grid drives the sequencer rather than incoming notes, but the plugin
        still has to report MIDI input: a VST3 instrument with no event input bus
        is rejected outright by hosts such as Ableton Live. */
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    /** A channel can be given the longest release in `Layout`, and a host that
        stops rendering before that has run out will cut the tail off. */
    double getTailLengthSeconds() const override { return Layout::maxReleaseSeconds; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    // Transport
    //==========================================================================

    /** True when the host drives transport and tempo (any plugin format). */
    bool isHostDriven() const noexcept { return ! isStandalone; }

    double getCurrentTempo() const noexcept { return displayedTempo.load(); }

    /** Play/stop for the built-in transport; ignored when the host drives it. */
    void setPlaying (bool shouldPlay);
    bool isPlaying() const noexcept;

    /** Called from a timer so the editor can follow host transport. */
    int getCurrentStep() const noexcept { return engine.getCurrentStep(); }

    /** Stops playback and silences every voice, returning the sequencer to the
        top of the bar. Used when testing and when the transport is reset. */
    void resetTransport();

    //==========================================================================
    // Pattern editing
    //==========================================================================

    bool isStepOn (int channel, int step) const;
    void setStep (int channel, int step, bool shouldBeOn);
    void toggleStep (int channel, int step);

    /** Velocity of one step, 0..1. Steps default to full velocity. */
    float getStepVelocity (int channel, int step) const;
    void setStepVelocity (int channel, int step, float velocity);

    /** Puts every step's velocity back to full. */
    void resetVelocities();

    /**
        How often one step is allowed to fire, 0..1. Steps default to every time.

        A step's own wish, not the machine's: what actually gets rolled is this
        discounted by `getMutate()`, and the value is held on the rung of 255 that the
        saved record can carry, so what the panel shows is what the session stores.
    */
    float getStepChance (int channel, int step) const;
    void setStepChance (int channel, int step, float chance);

    /** Puts every step's probability back to every time. */
    void resetChances();

    /**
        Does the hit at (bar, step, channel) survive the roll as the grid, the knob
        and the seed stand right now?

        A pass-through to `SequencerEngine::wouldThisHitFire`, which is the same call
        the audio thread makes — so this answers "what is this take", not "what would a
        model of this take say".
    */
    bool wouldThisStepHit (juce::int64 bar, int step, int channel) const
    {
        return engine.wouldThisHitFire (bar, step, channel);
    }

    /**
        Rotates one channel's row, carrying each step's velocity with it.

        A shift of +1 moves every step one place later, wrapping the last step back
        to the first, so the pattern is turned rather than pushed off the end. The
        step and its velocity always travel together, which is the whole point of
        shifting the row rather than retyping it.
    */
    void shiftChannelRow (int channel, int steps);

    void clearPattern();
    void clearChannelRow (int channel);

    //==========================================================================
    // Channels
    //==========================================================================

    static constexpr int getNumChannels_() { return Layout::numChannels; }
    static constexpr int getNumSteps_() { return Layout::numSteps; }

    juce::String getChannelName (int channel) const { return samples.getChannelName (channel); }
    void setChannelName (int channel, const juce::String& name);

    juce::Colour getChannelColour (int channel) const { return samples.getChannelColour (channel); }
    void setChannelColour (int channel, juce::Colour colour);

    /** Id of the sample on a channel, or 0 when the channel is empty. */
    int getChannelSampleId (int channel) const { return samples.getChannelSampleId (channel); }
    juce::String getChannelSampleName (int channel) const;

    int getChannelPitch (int channel) const;
    void setChannelPitch (int channel, int semitones);
    void nudgeChannelPitch (int channel, int delta);

    /** How far above its own note a hit on this lane starts, in semitones.

        Zero means the hit is at its pitch from its first sample. Anything above
        it glides down to the note over `Layout::pitchDropSweepSeconds` and stays
        there, so the tail of the hit — and therefore the tuning of the kit — is
        what the Pitch control says, never this one. See `Layout::maxPitchDropSemitones`.
    */
    int getChannelPitchDrop (int channel) const;
    void setChannelPitchDrop (int channel, int semitones);

    float getChannelLevel (int channel) const;
    void setChannelLevel (int channel, float linearGain);

    bool isChannelMuted (int channel) const;
    void setChannelMuted (int channel, bool shouldBeMuted);

    /** Which choke group a lane is in: 0 for none, 1..`Layout::numChokeGroups`.

        A hit on a lane in a group takes a ramp out of every hit already sounding
        in it, which is what makes a closed hat stop an open one. It is a lane
        setting on purpose: what two sounds do to each other does not change from
        one step to the next.
    */
    int getChannelChokeGroup (int channel) const;
    void setChannelChokeGroup (int channel, int group);

    //==========================================================================
    // Envelope
    //
    // Each channel has its own ADSR. A, D and R are held normalised (0..1) and
    // mapped onto Layout::maxAttackSeconds and friends, so the editor only ever
    // deals with 0..1; S is the sustain level, also 0..1.
    //
    // Sustain 0 makes the channel a one-shot, which is what a drum channel wants
    // by default: the envelope lets go as soon as it reaches the sustain stage.
    //==========================================================================

    float getChannelAttack (int channel) const;
    void setChannelAttack (int channel, float normalised);

    float getChannelDecay (int channel) const;
    void setChannelDecay (int channel, float normalised);

    float getChannelSustain (int channel) const;
    void setChannelSustain (int channel, float level);

    float getChannelRelease (int channel) const;
    void setChannelRelease (int channel, float normalised);

    /** Puts one channel's envelope back to the defaults for a drum hit. */
    void resetChannelEnvelope (int channel);

    /** Attack, decay and release in seconds; sustain as a level. For display. */
    static double envelopeSecondsFor (float normalised, EnvelopeStage stage);

    /** "12 ms", "1.20 s", "45 %" — a display string for an envelope value. */
    static juce::String formatEnvelopeValue (float normalised, EnvelopeStage stage);

    /** Loads a file as the sound for a channel. Returns false and fills
        `errorOut` when the file cannot be decoded. */
    bool loadSampleIntoChannel (int channel, const juce::File& file, juce::String& errorOut);

    /** Decodes dropped bytes into a channel (drag-and-drop from a browser). */
    bool loadSampleDataIntoChannel (int channel, const void* data, size_t numBytes,
                                    const juce::String& nameHint, juce::String& errorOut);

    /** True when the given channel is producing sound right now. */
    bool isChannelSounding (int channel) const { return engine.isChannelSounding (channel); }

    //==========================================================================
    // MIDI
    //==========================================================================

    /** The note that triggers a channel, e.g. 36 (C1) for channel 0. */
    static int getMidiNoteForChannel (int channel) { return SequencerEngine::midiNoteForChannel (channel); }

    /** Name of that note, e.g. "C1". */
    static juce::String getMidiNoteNameForChannel (int channel);

    /** Plays a channel as if a note had arrived, for previewing from the editor. */
    void previewChannel (int channel, float velocity = 1.0f);

    /** Voice state, for diagnostics and tests. */
    SequencerEngine::VoiceDebug debugVoice (int index) const { return engine.getVoiceDebug (index); }
    int debugActiveVoices() const noexcept { return engine.getActiveVoiceCount(); }

    //==========================================================================
    // Groove
    //==========================================================================

    float getSwing() const { return sequencerState.swing.load(); }
    void setSwing (float amount);

    /** 4 = swing the 1/8 notes, 2 = swing the 1/16 notes. */
    int getSwingGrid() const { return sequencerState.swingGrid.load(); }
    void setSwingGrid (int stepsPerPair);

    void setManualTempo (double bpm);

    /**
        How much of the grid's probabilities the machine may use, 0..1.

        Closed, the pattern is exactly what was drawn, whatever the cells say about
        probability. Open, each step is rolled against its own. See
        `SequencerState::mutate` for why this is one number for the whole machine.
    */
    float getMutate() const noexcept;
    void setMutate (float amount);

    /** Which sequence of rolls this session is. Same number, same grid, same drops. */
    int getSeed() const noexcept;
    void setSeed (int seed);

    /**
        Picks a new seed and hands it back, for the panel's Reroll.

        The grid does not move. What changes is which of its hits the machine is
        allowed to drop, which is the difference between a take and a shimmer.
    */
    int rerollSeed();

    //==========================================================================
    // Master
    //
    // Voices sum at unity, so this trim is the only place the output level is
    // decided. It used to be a fixed 0.8 inside every voice, which no control
    // could reach and no meter could explain.
    //==========================================================================

    float getMasterGainDb() const noexcept { return master.getGainDb(); }
    void setMasterGainDb (float db);

    /** Held peak of one output channel, linear, for the editor's meter. */
    float getMasterPeak (int channel) const noexcept { return master.getPeak (channel); }

    /** Held true (inter-sample) peak of one output channel, linear. Drawn as the
        tick on the meter; it latches, gates and trims nothing. */
    float getMasterTruePeak (int channel) const noexcept { return master.getTruePeak (channel); }

    /** True once the output has reached 0 dBFS, until the latch is cleared. */
    bool isMasterClipping (int channel) const noexcept { return master.isClipping (channel); }

    void clearMasterClipLatch() noexcept { master.clearClipLatch(); }

    /** "-2.0 dB", "+1.5 dB" — how the trim reads on the panel. */
    static juce::String formatMasterGain (float db);

    //==========================================================================
    // Persistence helpers used by the editor
    //==========================================================================

    void beginTransaction (const juce::String& name);
    juce::UndoManager& getUndoManager() noexcept { return undoManager; }

    /** Notifies the editor that something it displays has changed. */
    std::function<void()> onStateChanged;

    SampleStore& getSampleStore() noexcept { return samples; }

    /** The parameter state, so the editor can attach controls to it and a host's
        automation reaches the same values the panel writes. */
    juce::AudioProcessorValueTreeState& getValueTreeState() noexcept { return apvts; }

private:
    /** Writes one parameter into the atomics the audio thread reads.

        Called by the parameter state whenever a value moves, whichever way it
        moved: a host automating it, the panel writing it, or a preset replacing
        the whole tree. It can therefore arrive on the audio thread, so it only
        ever stores atomics — and it finds out which lane and field the id names
        by looking that up in `Params::laneTargets()`, because reading the id by
        cutting it into pieces would allocate, which is what the static guard in
        Tests/RtPathGuard.ps1 exists to keep true.
    */
    void parameterChanged (const juce::String& parameterID, float newValue) override;

    /** Copies every parameter into the state, for the initial sync and for
        anything that replaced the tree without going through the listener. */
    void syncStateFromParameters();

    /** Writes one denormalised value into a parameter and tells the host.

        The panel's route in, so a drag here and a drawn automation ramp arrive at
        the engine by the same path. JUCE 8 dropped the parameter state's own
        notifying setter, so this goes through the parameter directly.
    */
    void writeParameter (juce::StringRef parameterID, float denormalisedValue);

    /** Lane name, colour and sample path — the parts of a lane that are not
        parameters, stored as children of the parameter state's tree. */
    void pushLaneToState (int channel);

    void pushPatternToState();

    /** Reads the pattern property back into the atomics the engine plays.

        This is the whole of what an undo has to do to be real. The grid writes the
        atomics and pushes a copy of them into the state, so the string is the
        record; the audio thread never reads the string, so a restored string is
        only a picture of the pattern until something puts it back where the sound
        comes from.
    */
    void applyPatternFromState();

    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override;

    /** Reads the pattern and the lane metadata back out of a loaded tree. */
    void applyMetadata (const juce::ValueTree& tree);

    /** Turns a session saved before parameters existed into the current shape. */
    static juce::ValueTree migrateLegacyState (const juce::ValueTree& tree);

    SampleStore samples;
    SequencerState sequencerState;
    SequencerEngine engine;
    MasterBus master;

    juce::UndoManager undoManager;

    /** Declared after `undoManager` so it is constructed after it, and after
        `sequencerState` so its listener can write into it. */
    juce::AudioProcessorValueTreeState apvts;

    std::atomic<double> displayedTempo { 120.0 };
    double lastKnownSampleRate = 44100.0;

    /** True only while `pushPatternToState` writes the property.
        The write comes from the atomics, so reading it back would be a round trip
        through the same data — and with the undo manager attached to that write, a
        round trip that also fires the panel would make a single drag repaint itself
        as many times as it has cells. */
    bool pushingPatternToState = false;

    /**
        True while the pattern property holds a record this build cannot read.

        Set by `applyPatternFromState` when the payload's own codec tag names a
        generation it does not know, and cleared by the next `pushPatternToState` —
        which is the moment this build starts owning the grid. Between those two, the
        property is passed back out untouched instead of being rewritten from a grid
        that has no idea what the newer session meant.
    */
    bool holdingUnreadablePattern = false;

    /** What the host last asked for, so a transport reset re-prepares the engine
        at the size it will actually be given. */
    int lastKnownBlockSize = 512;

    bool isStandalone = false;
    bool prepared = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrooveboxProcessor)
};

} // namespace gb
