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

    /**
        Every channel-and-step seam in this section reads and writes one lane's
        *selected* slot — see the bank below. Nothing here takes a slot, because the
        thing the grid, the strips and the overlays are always editing is the row the
        lane is pointing at, and a slot argument on eight functions would be a second
        way to say what `getChannelSlot` already says.
    */

    bool isStepOn (int channel, int step) const;
    void setStep (int channel, int step, bool shouldBeOn);
    void toggleStep (int channel, int step);

    /** Velocity of one step, 0..1. Steps default to full velocity. */
    float getStepVelocity (int channel, int step) const;
    void setStepVelocity (int channel, int step, float velocity);

    /** Puts every step of the slot each lane points at back to full. */
    void resetVelocities();

    /**
        How often one step is allowed to fire, 0..1. Steps default to every time.

        A step's own wish, not the machine's: what actually gets rolled is this
        discounted by `getMutate()`, and the value is held on the rung of 255 that the
        saved record can carry, so what the panel shows is what the session stores.
    */
    float getStepChance (int channel, int step) const;
    void setStepChance (int channel, int step, float chance);

    /** Puts every step's chance back to firing every time, in the slot each lane points at. */
    void resetChances();

    /**
        How many strikes one hit on this cell is spread into, 1..`Layout::maxRatchetHits`.

        The fourth field of a cell and the same rule as the other three: it is a fact about
        one step of one lane of one slot, it is deliberately not a parameter, and the seam
        is the *selected* slot because that is the row the overlay is editing. The engine
        asks the playing slot for itself, in `fireStep`.

        An `int` where `getStepChance` answers a float, because this one counts things: the
        panel that draws eight positions and the record that stores one byte both want the
        number, and a 0..1 amount here would be a third place for the lattice to live.
    */
    int getStepRatchet (int channel, int step) const;
    void setStepRatchet (int channel, int step, int strikes);

    /** Back to one strike everywhere — the neutral value of this field is not the top of it. */
    void resetRatchets();

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
        How far behind its onset the hit at (bar, step, channel) starts, in steps.

        The timing half of the same pair, and the same kind of pass-through: it is the
        call `fireStep` makes, in the unit the draw is in. So a check of the take's
        timing asks the machine that plays it — and because the answer is in steps it
        does not depend on whether the transport has told the engine a tempo yet.
    */
    double wouldThisStepStartLate (juce::int64 bar, int step, int channel) const
    {
        return engine.humanizeLateInSteps (bar, step, channel);
    }

    /**
        The level the hit at (bar, step, channel) is struck at, given what its step
        says right now.

        The third of the three seams, and the one that reads the grid rather than the
        clock: the draw is a proportion of the written velocity, so answering it needs
        the cell's own level, taken from the same storage the audio thread reads. A
        check asked through this therefore goes through `setStepVelocity`, which means it
        goes through the float the session holds and comes back off the lattice the
        session is written on — the two things the law is claimed not to depend on.

        The cell it reads is the one the bar says is playing, not the one the lane is
        pointed at: asked about bar 9 of a rotating lane, this has to answer for the slot
        bar 9 sounds, which is the same `slotPlaying` call the audio thread makes.
    */
    float wouldThisStepBeStruckAt (juce::int64 bar, int step, int channel) const
    {
        return engine.struckVelocityFor (bar, step, channel,
                                         getStepVelocityInSlot (channel, step,
                                                                engine.slotPlaying (bar, channel)));
    }

    /**
        Where one step sits inside its bar, in steps from the top of it.

        The engine's own `stepOnsetInSteps`, pass-through for the same reason as the
        two above: the claim that a late hit cannot overtake the next one is a claim
        about how close two onsets can get, and only this answers it. Swing moves them
        here, so a bound written against a straight grid is not a bound at all.
    */
    double getStepOnsetInSteps (int step) const { return engine.getStepOnsetInSteps (step); }

    /**
        How far the onset after this step sits, in steps: the span a roll is spread over.

        The fourth pass-through of the same kind. It is not here to be tested against — a
        spacing gate that took its expected pitch from this would be asking the predicate
        what it expects, which is the mistake AGENTS.md records for the bar's wrap check. It
        is here so a gate can state its expectation in *steps*, the unit the law is written
        in, while getting the number itself from where the hits actually land in a render.
        What these two answer is "how far apart are the onsets this build has", and swing
        moves the answer, so only the engine that plays it can give it.
    */
    double getGapToNextOnsetInSteps (int step) const { return engine.getGapToNextOnsetInSteps (step); }

    /** Where strike `strike` of `strikes` starts, in steps after the hit's own onset. */
    double getStrikeDelayInSteps (int step, int strike, int strikes) const
    {
        return engine.getStrikeDelayInSteps (step, strike, strikes);
    }

    /**
        Rotates one channel's row, carrying everything each step says with it.

        A shift of +1 moves every step one place later, wrapping the last step back
        to the first, so the pattern is turned rather than pushed off the end. The
        step and its level, chance and strike count always travel together, which is
        the whole point of shifting the row rather than retyping it.

        The row it turns is the lane's selected slot: the other slots are a different
        take on the same lane, and turning one should not move another.
    */
    void shiftChannelRow (int channel, int steps);

    /** Empties every slot of every lane. */
    void clearPattern();

    /** Empties one lane's selected slot, leaving its other three alone. */
    void clearChannelRow (int channel);

    //==========================================================================
    // Pattern bank
    //==========================================================================

    /**
        Which of a lane's `Layout::numPatternSlots` slots its controls point at.

        The one number that says what the grid is editing — and, while the lane's
        evolve period is shut, the slot the machine plays. Slot 0 is what every lane of
        every session written before the bank existed comes back as, which is what keeps
        an old file one grid wide.
    */
    int getChannelSlot (int channel) const;

    /** Points one lane's controls at another slot. Nothing else about the lane moves. */
    void setChannelSlot (int channel, int slot);

    /** How many bars one lane holds a slot for before moving on; 0 is off. */
    int getChannelEvolveBars (int channel) const;
    void setChannelEvolveBars (int channel, int bars);

    /**
        Moves one lane to the next rung of `Layout::evolvePeriods`, wrapping past the
        longest back to off.

        A cycle rather than a set because the panel has one chip for it and five answers,
        and off is one of the answers: the way back has to be the same gesture as the way
        in. See `setChannelEvolveBars` for the value this writes.
    */
    void cycleChannelEvolve (int channel);

    /**
        Does one slot of one lane hold a record?

        The engine's own `isSlotFilled`, passed through for the same reason the three
        variation seams are: the dot that says a slot holds something, the rotation that
        decides which slots to walk over, the copy that decides which one is free to land
        in, and the save path that decides whether to write the property at all are four
        claims about one row, and one of them being a second spelling is how a dot says
        *empty* about bytes a session keeps.
    */
    bool isChannelSlotFilled (int channel, int slot) const;

    /** Which slot one lane is sounding where the song is right now. */
    int getChannelSlotPlaying (int channel) const;

    /**
        Copies a lane's selected slot into the next empty one and moves the lane there.

        Empty, deliberately: a duplicate that went somewhere already occupied would
        destroy a pattern the user cannot see from the row they are looking at. Occupied
        is `isChannelSlotFilled`'s answer, which counts a row whose steps are all switched
        off but whose levels are not — the grid lets a level be set before a step is turned
        on, and that number is in the session. Returns the slot it landed on, or -1 when
        all four hold something and there is nowhere to copy to.
    */
    int duplicateChannelRowToNextSlot (int channel);

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

    /**
        Queues an audition of one channel for the next audio block.

        Safe from the message thread — it parks a request and returns, and the render
        picks it up as a note event, so what you hear is the lane as a played note, at
        the trim and level the lane has. False when the request ring is full, which is
        the audition being dropped rather than the caller being waited on.
    */
    bool previewChannel (int channel, float velocity = 1.0f) noexcept;

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

    /**
        How much of the grid's timing slack the machine may use, 0..1.

        The timing half of the same pair: closed, every hit starts on its onset; open,
        each one is drawn late out of the same seed the drops come out of, by at most
        `Layout::maxHumanizeLateSteps` of a step. Never early, and never enough to pass
        the hit after it. See `SequencerEngine::humanizeLateInSteps`, which is the
        function both this and the tests ask.
    */
    float getHumanize() const noexcept;
    void setHumanize (float amount);

    /**
        How much of the grid's dynamic slack the machine may use, 0..1.

        The level half of the same pair: closed, every hit is struck at the velocity its
        step was written at; open, each one departs from it by at most
        `Layout::maxTouchRatio` of that velocity, drawn out of the same seed the drops
        and the lateness come out of. Multiplicative, so a rest stays a rest, and the
        result stays on the lattice the record stores. See
        `SequencerEngine::struckVelocityFor`, which is the function both this and the
        tests ask.
    */
    float getTouch() const noexcept;
    void setTouch (float amount);

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

    /**
        Writes one slot's cells into the state tree, and the build's own version with it.

        Slot 0 always goes: it is the property every session shape since the beginning has
        had, and an empty machine is a record of all-zeros rather than the absence of one.
        The others are sparse — see `slotPropertyName` — so a bank nobody has filled costs
        a session nothing, and clearing one takes its record with it.
    */
    void pushPatternToState (int slot);

    /** Writes every slot this build is responsible for, and takes none it is not. */
    void pushPatternToState();

    /**
        The tree property one slot's cells live in: `pattern` for slot 0, `pattern1` to
        `pattern3` for the rest.

        Slot 0 keeps the name it has always had, and with it the bytes: a session written
        before the bank is loaded by this build unchanged, and this build's file is still
        readable by the build before it — which sees its own grid in `pattern` and ignores
        three properties it has never heard of. Renaming the working slot to make the bank
        symmetrical would have cost both of those.
    */
    static juce::String slotPropertyName (int slot);

    /** Does any lane have anything at all in this slot — a step on, or a cell that is not
        at full level and every-time probability? The test for whether a record is worth writing. */
    bool slotCarriesAnything (int slot) const;

    /** The lane's selected slot, clamped: what every channel-and-step seam above edits. */
    int selectedSlotOf (int channel) const;

    /** One cell's level out of a named slot, without going through the lane's selection. */
    float getStepVelocityInSlot (int channel, int step, int slot) const;

    /** Writes `slots` and `evolve`, or removes them when every lane is at its default. */
    void pushBankToState();

    /** Reads `slots` and `evolve` back into the atomics. */
    void applyBankFromState();

    /** Reads one slot's property back into that slot's atomics, defaults when it is absent. */
    void applySlotFromState (int slot);

    /** Reads the pattern property back into the atomics the engine plays.

        This is the whole of what an undo has to do to be real. The grid writes the
        atomics and pushes a copy of them into the state, so the string is the
        record; the audio thread never reads the string, so a restored string is
        only a picture of the pattern until something puts it back where the sound
        comes from.

        Whole-grid, every slot and both bank lists, whatever property moved: one door for
        "the tree changed" is what keeps an undo of a selection from leaving the cells of
        the slot it selected behind.
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
        True while one slot's property holds a record this build cannot read.

        Set by `applySlotFromState` when the payload's own codec tag names a generation it
        does not know — or names none, for a slot other than the one that predates the tag
        — and cleared for that slot by the next `pushPatternToState (slot)`, which is the
        moment this build starts owning that grid. Between those two the property is passed
        back out untouched instead of being rewritten from a slot that has no idea what the
        newer session meant. `pushPatternToState()` consults it, and that is the only place
        the answer is used.

        One flag per slot rather than one for the machine, because the bank makes the two
        halves of a session separable: a lane can be editing slot 0 while slot 2 carries
        something from a build that knows more about a step than this one does. A single
        flag there would either destroy the unreadable record on the next save or throw
        away the edit that has nothing to do with it.

        Re-derived from the tree on every read rather than latched, so it stays a report
        about what the properties hold rather than a memory of the last one to fail.
    */
    std::array<bool, (size_t) Layout::numPatternSlots> holdingUnreadableSlot {};

    /**
        Is this one of the properties whose contents the atomics are derived from?

        The whole of the undo door: the listener watches exactly these, and each is written
        by a `push...ToState` on this side. Built from `slotPropertyName`, so a slot cannot
        be stored under one list of names and restored under another.
    */
    static bool isGridProperty (const juce::Identifier& property);

    /** What the host last asked for, so a transport reset re-prepares the engine
        at the size it will actually be given. */
    int lastKnownBlockSize = 512;

    bool isStandalone = false;
    bool prepared = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrooveboxProcessor)
};

} // namespace gb
