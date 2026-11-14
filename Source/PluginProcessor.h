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
                           private juce::AudioProcessorValueTreeState::Listener
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

    float getChannelLevel (int channel) const;
    void setChannelLevel (int channel, float linearGain);

    bool isChannelMuted (int channel) const;
    void setChannelMuted (int channel, bool shouldBeMuted);

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
        ever stores atomics.
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

    /** What the host last asked for, so a transport reset re-prepares the engine
        at the size it will actually be given. */
    int lastKnownBlockSize = 512;

    bool isStandalone = false;
    bool prepared = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrooveboxProcessor)
};

} // namespace gb
