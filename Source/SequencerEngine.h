#pragma once

#include "Common.h"
#include "Resampler.h"
#include "SampleStore.h"

namespace gb
{

/** A normalised envelope value mapped onto a time range, in seconds.

    The curve makes the low end of the travel usable: without it, almost all of a
    linear control's useful range would sit in its first few percent, which is
    exactly the part a drum envelope needs.
*/
inline double envelopeSeconds (float normalised, double maximum) noexcept
{
    const auto clamped = juce::jlimit (0.0f, 1.0f, normalised);
    return (double) clamped * (double) clamped * maximum;
}

/**
    A small amplitude envelope with an exponential feel.

    Written by hand rather than using juce::ADSR because the sequencer has to know
    when the decay has reached the sustain stage: a channel with no sustain is a
    one-shot, and that is the moment it should let go. juce::ADSR keeps its stage
    private, so it cannot answer that question.

    Attack ramps linearly (an exponential rise from zero is inaudible for most of
    its length), while decay and release fall towards their target the way an
    analogue envelope does, which is what the web app's envelope does too. They
    fall in a straight line only as far as the eye is concerned: a real one gives up
    most of its distance first and the last little bit over a long tail, which is why
    a linear decay on a drum reads as a gate closing rather than as a skin stopping
    moving. See `fallTimeConstants` for how a curve that never quite arrives is still
    bounded by the duration the parameter names.
*/
class Envelope
{
public:
    enum class Stage { idle, attack, decay, sustain, release };

    void setSampleRate (double newRate) noexcept
    {
        sampleRate = newRate > 0.0 ? newRate : 44100.0;
    }

    void setParameters (float attackSeconds, float decaySeconds, float sustainLevel, float releaseSeconds) noexcept
    {
        /**
            The shortest fall the envelope is allowed.

            A parameter at its bottom asks for a decay or a release that finishes
            in one sample, and one sample is a step: see
            `Layout::minimumRampSeconds`. The attack keeps its own floor of one
            sample because it rises from silence, which cuts nothing.
        */
        const auto minimumFall = juce::jmax (1.0, Layout::minimumRampSeconds * sampleRate);

        attackSamples  = juce::jmax (1.0, (double) attackSeconds * sampleRate);
        decaySamples   = juce::jmax (minimumFall, (double) decaySeconds * sampleRate);
        releaseSamples = juce::jmax (minimumFall, (double) releaseSeconds * sampleRate);
        sustain = juce::jlimit (0.0f, 1.0f, sustainLevel);

        decayCoefficient = fallCoefficientFor (decaySamples);
        releaseCoefficient = fallCoefficientFor (releaseSamples);
    }

    void noteOn() noexcept
    {
        value = 0.0f;
        stage = Stage::attack;
        stagePosition = 0.0;
    }

    void noteOff() noexcept
    {
        if (stage == Stage::idle || stage == Stage::release)
            return;

        stage = Stage::release;
        stagePosition = 0.0;
    }

    float getNextSample() noexcept
    {
        switch (stage)
        {
            case Stage::idle:
                value = 0.0f;
                break;

            case Stage::attack:
                value = (float) juce::jmin (1.0, stagePosition / attackSamples);
                stagePosition += 1.0;

                if (value >= 1.0f)
                {
                    stage = Stage::decay;
                    stagePosition = 0.0;
                }
                break;

            /**
                Decay and release: one curve, two targets.

                The level reported for this sample is the one the previous step left
                behind, and the move towards the target happens on the way out. That
                order is the whole reason the two stages share a case: it makes the
                corner between two stages continuous by construction, because the
                first sample of a fall is still the level the fall started from. A
                curve that stepped *then* reported would turn every stage change into
                a jump of its own, and a jump at the top of a hit is a click.

                The fall is bounded by the duration the parameter names: the last
                sample of it is the target itself, see `fallTimeConstants`.
            */
            case Stage::decay:
            case Stage::release:
            {
                const auto decaying = stage == Stage::decay;
                const auto target = decaying ? (double) sustain : 0.0;

                stagePosition += 1.0;

                if (stagePosition >= (decaying ? decaySamples : releaseSamples))
                {
                    value = decaying ? sustain : 0.0f;
                    stage = decaying ? Stage::sustain : Stage::idle;
                }
                else
                {
                    value = (float) (target + ((double) value - target)
                                            * (decaying ? decayCoefficient : releaseCoefficient));
                }
                break;
            }

            case Stage::sustain:
                value = sustain;
                break;
        }

        return value;
    }

    Stage getStage() const noexcept { return stage; }
    bool isActive() const noexcept { return stage != Stage::idle; }
    float getCurrentValue() const noexcept { return value; }

private:
    /**
        How far a fall has come by the sample its own duration says it is over.

        An exponential never arrives, so the stage counts down to the length the
        parameter asked for and steps onto the target for the last sample. What is
        left to travel at that moment is e to the minus this, so five is 0.7 % of the
        distance still to fall — forty three decibels below where the fall started,
        far too quiet to hear as a step, and short enough that the number on the knob
        stays the number you hear. Three would put the residual at five percent,
        which is audible as the fade being cut off; eight costs the tail its shape
        for a duration nobody asked to stretch.
    */
    static constexpr double fallTimeConstants = 5.0;

    static double fallCoefficientFor (double samples) noexcept
    {
        return std::exp (-fallTimeConstants / samples);
    }

    double sampleRate = 44100.0;
    double attackSamples = 1.0;
    double decaySamples = 1.0;
    double releaseSamples = 1.0;

    /** Per-sample multipliers for the two falls, precomputed from their lengths. */
    double decayCoefficient = 0.0;
    double releaseCoefficient = 0.0;

    float sustain = 0.0f;
    float value = 0.0f;

    double stagePosition = 0.0;
    Stage stage = Stage::idle;
};

/**
    One pattern slot of the machine: the whole grid, as the audio thread reads it.

    A slot holds all four of the fields a cell carries — whether it fires, how hard, how
    often and how many times over — rather than just the on/off the word "pattern"
    suggests. That is deliberate: the level, the chance and the strike count are facts
    *about the step*, they are drawn on the same grid, and a slot that switched the pattern
    but carried the level across from another slot would play a hit that was never written
    anywhere. `shiftChannelRow` moves all four for the same reason, and the saved record
    carries all four for the third.

    The defaults are the ones a session from before each field existed means: off, full
    level, fires every time, once.
*/
struct PatternCells
{
    /** pattern[channel][step] — true when that drum fires on that step. */
    std::array<std::array<std::atomic<bool>, (size_t) Layout::numSteps>, (size_t) Layout::numChannels> pattern {};

    /**
        velocity[channel][step], 0..1, used when that step fires.

        Kept alongside the pattern rather than inside it so the grid stays a plain
        on/off view and the velocity overlay can read the same data. A step that is
        off can still carry a velocity, which is what makes it possible to set the
        level before switching the step on.
    */
    std::array<std::array<std::atomic<float>, (size_t) Layout::numSteps>, (size_t) Layout::numChannels> velocity {};

    /**
        chance[channel][step], 0..1: how often that step is allowed to fire.

        Kept beside the pattern and the velocity for the same reason they are: it is a
        fact about one cell, it is not a parameter, and a cell that is switched off
        still carries it. One is a step that fires every time, which is both the
        default and what every step drawn before this feature existed means. The
        machine only consults a cell's chance while `mutate` is open — see that.
    */
    std::array<std::array<std::atomic<float>, (size_t) Layout::numSteps>, (size_t) Layout::numChannels> chance {};

    /**
        ratchet[channel][step]: how many strikes one hit on that cell is spread into.

        The fourth fact about a cell, kept beside the other three for the reasons they are:
        it is drawn on the same grid, it is not a parameter, and a step that is switched off
        still carries it. An `int` rather than a normalised float like its two neighbours,
        because it counts things — there is no roll of 3.4 strikes — and the byte the record
        holds is the number itself, not a rung of a lattice.

        One is a hit on its onset, which is both the default and what every cell of every
        session drawn before this field means, and is also the state in which the engine
        makes one `trigger` call exactly where it used to make one. The top is
        `Layout::maxRatchetHits`, and `Layout::ratchetByte` is the one clamp both the
        writer and the reader go through.
    */
    std::array<std::array<std::atomic<int>, (size_t) Layout::numSteps>, (size_t) Layout::numChannels> ratchet {};

    PatternCells()
    {
        for (auto& row : velocity)
            for (auto& cell : row)
                cell.store (1.0f);

        // And every step fires every time, which is what a machine that never had a
        // probability meant, and what a session from before it still says.
        for (auto& row : chance)
            for (auto& cell : row)
                cell.store (1.0f);

        // And one strike per hit, which is what a machine that never had a ratchet played
        // and what a record without the fourth byte still says.
        for (auto& row : ratchet)
            for (auto& cell : row)
                cell.store (1);
    }

    JUCE_DECLARE_NON_COPYABLE (PatternCells)
};

/**
    Everything the audio thread reads while sequencing.

    The editor and the processor's parameter layer write these atomics; the
    engine only ever reads them, so there is no lock on the audio path.
*/
struct SequencerState
{
    /**
        The banks: `Layout::numPatternSlots` whole grids per channel.

        Indexed slot-first, because that is the axis the audio thread asks about — it
        already knows the channel and the step, and `slotPlaying` hands it the slot. A
        lane-by-lane array of slots would have made the read a double indirection to save
        nothing.

        Slot 0 is the one a session from before the bank existed holds, and the one every
        lane points at when nothing has been changed, which is what keeps the default
        machine one grid wide.
    */
    std::array<PatternCells, (size_t) Layout::numPatternSlots> slots {};

    /**
        Which slot each channel's controls are pointed at.

        The panel's answer to "what am I editing", and — with `evolveBars` at zero — also
        the machine's answer to what it plays. It is state and not a parameter for the same
        reason the grid is: it is not a number anyone draws an automation ramp against, and
        a parameter per lane would put twelve more rows in a table the host already shows.
    */
    std::array<std::atomic<int>, (size_t) Layout::numChannels> selectedSlot {};

    /**
        How often each channel moves on to its next filled slot, in bars; zero is off.

        Off is the default and the honest resting state: a bank that rotated by itself the
        moment it shipped would change what an old session sounds like on the first load
        after the update.
    */
    std::array<std::atomic<int>, (size_t) Layout::numChannels> evolveBars {};

    /**
        How much of the grid's chances the machine is allowed to use, 0..1.

        Zero plays exactly the pattern that was drawn, whatever the cells say; one
        hands every cell its own probability. It is one number for the whole machine
        rather than a per-cell amount because the thing a producer wants to set is how
        much of the arrangement is *not* written down, and because at zero the engine
        does not reach for a generator at all — the audio path is the one that was
        measured before this existed.

        What it can do is take hits away. A closed cell never comes back, and a step at
        100% is never dropped however far this is opened.
    */
    std::atomic<float> mutate { 0.0f };

    /**
        How much of the grid's per-hit timing slack the machine is allowed to use, 0..1.

        The other half of variation, and it sits beside `mutate` because it works the
        same way: one number for the whole machine, zero means "exactly what was
        drawn", and the amount is applied per hit from the same key the roll uses — the
        seed and where in the song the hit is, never a counter. So a session with a
        given seed is one take in both senses: the same hits drop out and the same hits
        land late. Change the seed and both change together, which is what makes an A/B
        of two bounces an A/B.

        What it can do is start a hit later than its step. Never earlier, and never by
        more than `Layout::maxHumanizeLateSteps`, which is half the closest two onsets
        this machine can make: the drawn pattern keeps its order however far this is
        opened.
    */
    std::atomic<float> humanize { 0.0f };

    /**
        How much of the grid's per-hit dynamic slack the machine is allowed to use, 0..1.

        The level half of the pair above, so it is a proportion rather than a decibel
        figure: it says how far a hit may depart from the velocity *its own step was
        written at*, which is the same relationship to the grid that swing and the
        timing offset are, and it is why a step at zero stays a rest instead of growing
        a floor of its own. Drawn out of the middle slice of the same key, so one seed
        names one take in all three senses — which hits happen, how late they land, how
        hard each is struck.

        What it can do is move a hit off the level it was given, by at most
        `Layout::maxTouchRatio` of that level, and never off the 7-bit lattice the cell
        is stored on. It cannot make a hit louder than full: the top of the range is the
        top of the lattice, so the loudest thing in the set can only come down. Because
        velocity is two laws in this engine — gain and tone — a struck-differently hit
        is also brighter or darker, which is the point of the control rather than a side
        effect, and is measured.
    */
    std::atomic<float> touch { 0.0f };

    /**
        Which sequence of rolls this session is.

        A hit is decided by where it sits in the song — bar, step, lane — and this
        number is what makes one song's decisions different from another's. Same seed,
        same pattern, same place: the same hits drop out, in a bounce and in a live
        pass, which is what makes an A/B of two takes an A/B rather than a coin toss.
    */
    std::atomic<int> seed { 0 };

    /** Per-channel output level, linear gain. */
    std::array<std::atomic<float>, (size_t) Layout::numChannels> level {};

    /** Per-channel pitch offset in semitones, -36 .. +12. */
    std::array<std::atomic<int>, (size_t) Layout::numChannels> pitch {};

    /**
        Per-channel pitch drop in semitones, 0 .. 12: how far above its own note a
        hit on this lane starts, and therefore how far it falls to reach it.

        A lane setting rather than a step one, for the same reason the choke group
        is: it describes how the sound is made, not when it happens. Zero is the
        default and the whole mechanism is inert at it — see
        `Layout::maxPitchDropSemitones`.
    */
    std::array<std::atomic<int>, (size_t) Layout::numChannels> pitchDrop {};

    std::atomic<bool> channelMuted[Layout::numChannels] {};

    /** Internal transport, used when the host has no transport (standalone). */
    std::atomic<bool> playing { false };

    /** Swing pushes the off-beat steps later. 0 = straight, 1 = heavy shuffle. */
    std::atomic<float> swing { 0.0f };

    /**
        Which choke group a lane belongs to: 0 is none, 1..`Layout::numChokeGroups`.

        A hit on a lane in a group takes a ramp out of every hit already sounding in
        it, which is how an open hat knows to stop when the closed hat arrives. It is
        a lane setting and not a step setting because it is about how two *sounds*
        interfere, not about when either one starts.
    */
    std::array<std::atomic<int>, (size_t) Layout::numChannels> chokeGroup {};

    /** Swing pair size in steps: 4 gives a 1/8 grid, 2 gives a 1/16 grid. */
    std::atomic<int> swingGrid { 4 };

    /** Fallback tempo used when no host transport is available (standalone). */
    std::atomic<double> manualTempo { 120.0 };

    /**
        Per-channel amplitude envelope, each parameter normalised to 0..1.

        Normalised rather than in seconds because the editor stores these and a
        curve should feel even across its whole travel; `Layout::attackSeconds`
        and its neighbours give the ranges. Defaults are short and sustain-free,
        so a channel behaves like a one-shot drum until you open it up.
    */
    std::array<std::atomic<float>, (size_t) Layout::numChannels> attack {};
    std::array<std::atomic<float>, (size_t) Layout::numChannels> decay {};
    std::array<std::atomic<float>, (size_t) Layout::numChannels> sustain {};
    std::array<std::atomic<float>, (size_t) Layout::numChannels> release {};

    /**
        Audition requests, handed from the message thread to the audio one.

        Clicking a lane's name asks to hear it. The click happens on the message
        thread and a voice may only start inside the render walk — that walk is what
        splits the block at every start point so a hit lands click-free, at the right
        sample, and steals and chokes like any other hit — so a request is parked here
        and picked up by the next block as a note event. The one way in before this was
        a method that called the engine's voice start straight from the editor, which
        began the hit on whatever thread the UI happened to be on and raced the render;
        it is gone, and `SequencerEngine::requestPreview` is the only way to audition.

        A ring, not a flag: `produced` is only ever advanced by the caller and
        `consumed` only ever by the render, so each slot has one writer and one reader
        and the pair needs no lock. Full means dropped, never waited on — an audition
        is a diagnostic, and a UI thread blocking on an audio thread is the worse bug
        by a wide margin. Sixteen is more than a hand can click between two blocks at
        any block size this plugin will ever see.
    */
    static constexpr int maxPreviewRequests = 16;

    std::array<std::atomic<std::uint32_t>, (size_t) maxPreviewRequests> previewSlots {};
    std::atomic<std::uint32_t> previewProduced { 0 };
    std::atomic<std::uint32_t> previewConsumed { 0 };

    SequencerState()
    {
        for (int i = 0; i < Layout::numChannels; ++i)
        {
            // Slot A, and no rotation: the machine a session describes is the one it
            // drew, and a bank that moved on its own is a change to old sets that nobody
            // asked for.
            selectedSlot[(size_t) i].store (0);
            evolveBars[(size_t) i].store (0);

            level[(size_t) i].store (1.0f);
            pitch[(size_t) i].store (0);
            pitchDrop[(size_t) i].store (0);
            channelMuted[i].store (false);
            chokeGroup[(size_t) i].store (0);

            attack[(size_t) i].store (Layout::defaultAttack);
            decay[(size_t) i].store (Layout::defaultDecay);
            sustain[(size_t) i].store (Layout::defaultSustain);
            release[(size_t) i].store (Layout::defaultRelease);
        }
    }

    JUCE_DECLARE_NON_COPYABLE (SequencerState)
};

/**
    Turns the step pattern into audio.

    This is a port of the drum half of the web app's scheduler: a 16-step
    pattern where one step is a 16th note (tempo * 4 steps per minute), with the
    off-beat of every swing pair pushed later by the same fraction the web app
    uses. Blocks are split at step boundaries, so the timing does not depend on
    the host's block size.
*/
class SequencerEngine
{
public:
    SequencerEngine (SampleStore& store, SequencerState& state);

    void prepare (double sampleRate, int maximumBlockSize);
    void releaseResources();

    /**
        Renders one host block, once, applying the host transport and any notes
        that arrived with it.

        Notes are part of this pass rather than a pass of their own. The block is
        already split at every step onset so a hit starts on its exact sample, and
        a note's position in the block is the same kind of fact, so it becomes
        another split point: the note starts on its own sample, everything already
        sounding is rendered through it exactly once, and nothing is summed twice.

        Lanes map to consecutive notes from `Layout::firstMidiNote`, so the default
        layout answers to C1 upwards. A note off releases only the voices a note
        started: each drum voice is a one-shot whose own envelope decides how long
        it rings, which is what a drum machine does, and a grid hit on the same lane
        is the sequencer's to finish.

        The block is not consumed here — the processor clears `midi` after the call,
        as it does for every other input, because a host's note queue is not the
        sequencer's to keep.
    */
    void processBlock (juce::AudioBuffer<float>& buffer,
                       const juce::AudioPlayHead::PositionInfo& position,
                       bool haveHostPosition,
                       const juce::MidiBuffer& midi);

    /** Channel a note maps to, or -1 when the note is outside the range. */
    static int channelForMidiNote (int midiNote) noexcept;

    /** Note that triggers a channel, or -1 when the channel is out of range. */
    static int midiNoteForChannel (int channel) noexcept;

    /**
        Asks the audio thread to start one hit on `channel` with `velocity` in 0..1.

        The audition path, and safe to call from the message thread: it parks a
        request in `SequencerState` and returns, and the next block collects it as a
        note event, so a preview goes through `trigger` with `fromMidi` true exactly as
        a keyboard note does — same choke, same voice stealing, same level law, and no
        humanize delay, which belongs to the grid alone. Nothing here starts a voice.

        False when the lane is not one of the twelve, or when the ring is full, which
        is the request being dropped: see the note on `SequencerState::previewSlots`.
        The range check is the only thing protecting the drain, because a packed request
        carries a raw channel index and `drainPreviewRequests` writes it straight into a
        note event without looking at it again.
    */
    bool requestPreview (int channel, float velocity = 1.0f) noexcept;

    /** Voices currently sounding; used by the editor for channel activity. */
    int getActiveVoiceCount() const noexcept { return activeVoices; }

    /** Voice state, for diagnostics and tests. */
    struct VoiceDebug
    {
        bool active = false;
        int channel = -1;
        double position = 0.0;
        double increment = 1.0;
        float gain = 0.0f;
        float velocity = 0.0f;
        int sampleLength = 0;
        float envelopeValue = 0.0f;
        bool envelopeActive = false;
        bool released = false;
    };

    VoiceDebug getVoiceDebug (int index) const;

    /** Step that is sounding right now, or -1 when stopped. */
    int getCurrentStep() const noexcept { return currentStep.load(); }

    /** Where the engine places one step inside its bar, in steps. */
    double getStepOnsetInSteps (int step) const { return stepOnsetInSteps (step); }

    /**
        How much room one onset has before the next one, in steps.

        Public for the same reason `getStepOnsetInSteps` is: the spacing of a ratchet is
        derived from this, so a test that wants to know where strike four of a roll starts
        asks the function the engine asked, rather than redoing the swing arithmetic in the
        test file and calling an agreement between two spellings a check.
    */
    double getGapToNextOnsetInSteps (int step) const { return gapToNextOnsetInSteps (step); }

    /** How far after its own onset strike `strike` of `strikes` starts, in steps. */
    double getStrikeDelayInSteps (int step, int strike, int strikes) const
    {
        return strikeDelayInSteps (step, strike, strikes);
    }

    /**
        The bar the machine last fired in, as the panel is allowed to know it.

        Not `position / 16` read live: `position` belongs to the audio thread, and this is
        the number it published when it derived one (`fireStep`), so the dot in the label
        column and the slot that is actually sounding cannot be two different answers about
        the same bar. With nothing rotating it is never consulted — `slotPlaying` returns
        the lane's selected slot before it looks at the bar.
    */
    juce::int64 getCurrentBar() const noexcept { return currentBar.load(); }

    /**
        Does channel's slot `slot` hold anything at all?

        Public because the answer belongs to the panel as much as to the law below: the
        bank control has to mark the slots that hold something, and a second definition of
        "filled" — one painted, one played — is how a dot says *empty* about a slot that
        then turns up in the rotation.

        It is one definition for a second reason as well. "Anything" is the same test the
        save path uses, so a row whose steps are all switched off but whose levels or
        probabilities differ from full counts as holding something: that row is written to
        the session, and a copy that called it free would overwrite data the file keeps —
        losing a level the user set on a step they had not yet switched on. The price is
        the honest one: a rotation can visit such a slot and play nothing for a bar, and
        the solid dot is then telling the truth about a record rather than about a sound.
    */
    bool isSlotFilled (int channel, int slot) const noexcept;

    /**
        Which of a channel's slots the machine plays at one bar.

        The whole of the bank's audible half, and a function of (bar, channel) alone: no
        counter, no memory of the previous bar, nothing that has to be reset on a seek. That
        is the same rule the rolls are built on — see `wouldThisHitFire` — and for the same
        reason, which is that a bounce and a live pass have to agree. Start a take at bar 17
        in a host and the hats land on the slot they would have landed on, because the bar
        number is the only input.

        With the channel's period at zero the answer is the slot its controls point at, and
        that is the default state of every lane in every session written before this existed.

        When it does rotate it steps over the *filled* slots, not over all four, and starts
        from the selected one. Filling two slots of four therefore alternates those two; it
        does not go quiet for the two that hold no record at all, which would read as a bug
        in the pattern rather than as the empty bank it is. What "holds a record" means —
        and why it is the save path's answer rather than a listening one — is on
        `isSlotFilled`.
    */
    int slotPlaying (juce::int64 bar, int channel) const noexcept;

    /**
        True when at least one channel rotates, which is what decides whether the audio
        path has to know what bar it is in at all.

        Asked rather than cached, because a flag kept beside the twelve periods is a second
        answer to the same question — the failure this repo already documented for the
        parameter id lists. Twelve atomic loads once per onset is nothing next to the
        resampling it precedes.
    */
    bool anyChannelEvolves() const noexcept;

    /**
        Does the hit painted at (bar, step, channel) survive the roll as things stand?

        Public for the same reason `StepGridComponent::getCellBounds` is: a test that
        asks a question has to ask the machine that answers it. This is that question and
        nothing else decides it: it resolves the slot with `slotPlaying` and hands the
        roll to `wouldHitInSlotFire`, which is the function `fireStep` calls with the
        slot it resolved for the hit itself — see that for why the two are not one call.
        There is no clock in it and no counter — the answer is a function of the seed,
        the grid and where in the bar the hit is, which is what makes an offline bounce
        and a live pass the same take.
    */
    bool wouldThisHitFire (juce::int64 bar, int step, int channel) const;

    /**
        How far behind its onset the hit at (bar, step, channel) starts, in steps.

        The same reason `wouldThisHitFire` is public, and the same generator: this is
        the function `fireStep` calls, so a check that the take is deterministic,
        bounded and never early is a check about the engine rather than about a copy of
        its arithmetic. The draw is a slice of the key that the roll does not use, so
        the two are independent numbers from one seed — a hit that drops out is not
        biased towards one that lands late.

        In steps, not samples, on purpose: the offset is a relationship to the grid and
        therefore follows the tempo, and a seam that returned samples would answer a
        different question depending on whether the transport had told the engine its
        tempo yet. `fireStep` does the one multiply, at the moment it knows both halves.

        Zero whenever the knob is shut, and zero for every key, which is why opening
        the whole feature leaves the fingerprint of the closed one alone.
    */
    double humanizeLateInSteps (juce::int64 bar, int step, int channel) const;

    /**
        The level the hit at (bar, step, channel) is actually struck at, given the
        velocity its step was written at.

        The third public seam of the same take, asked for the same reason as the two
        above: it is the call `fireStep` makes, so a check that the jitter is bounded,
        centred, on the lattice and reproducible is about the engine rather than about a
        model of it. The draw is the middle slice of the key — the roll has the top and
        the offset the bottom — so a hit's level, its lateness and whether it happens at
        all are three independent numbers out of one seed.

        The deviation is a proportion of the level as the *record stores it*, rounded to
        the 7-bit lattice the saved cell carries, rather than of the float in memory.
        That is what makes the take survive a save: the format quantises a written
        velocity once, and a law applied to the number before that quantisation would
        answer differently on the two sides of it. A step at zero is therefore struck at
        zero — the deviation of nothing is nothing — and a step at full can only come
        down, because the top of the lattice is the top of what the machine plays.

        Returns the written value untouched when the knob is shut, without asking the
        generator: the branch that ran before this feature exists is the branch that
        runs with it switched off, which is what keeps `[28]`'s fingerprint a statement
        about code and not about rounding.
    */
    float struckVelocityFor (juce::int64 bar, int step, int channel, float written) const;

    /** True when the given channel is currently producing sound. */
    bool isChannelSounding (int channel) const noexcept
    {
        return juce::isPositiveAndBelow (channel, Layout::numChannels)
                 && soundingChannels[(size_t) channel].load();
    }

private:
    struct Voice
    {
        Sample::Ptr sample;
        int channel = 0;
        double position = 0.0;
        double increment = 1.0;

        /**
            Where the hit's read rate started above `increment`, and what is left of
            the glide that brings it back.

            `increment` stays the rate the hit *settles* at, which is the pitch the
            lane is tuned to, so everything that reads a voice's rate after the sweep
            — the tail pool, a diagnostic — sees the note rather than the transient.
            The excess is the multiplier above that rate minus one, and it is stepped
            down by `pitchDropCoefficient` per sample the same way an envelope fall is
            stepped, so the glide is one recursion rather than an `exp` per sample. It
            reaches exactly zero on the sample the sweep ends on, so a hit with a drop
            in it finishes at the pitch of a hit without one and cannot detune a kit.

            All three are carried into the tail pool: a voice stolen halfway down its
            glide goes on gliding as it fades, rather than jumping the rest of the way
            in the two milliseconds it has left.
        */
        double dropExcess = 0.0;
        double dropRemaining = 0.0;

        /**
            Output samples this hit has left to wait before it makes a sound.

            The humanize offset, counted down. It lives here rather than in the clock
            because the clock is what decides the order of things: a step that fires a
            third of a step early is a step that fired before the head reached it, and
            the only answer the sequencer's arithmetic has for that is to try the
            occurrence a whole bar on. A voice can be late without asking anyone where
            it is: it starts its own read this many samples into the region its onset
            opened, and everything else about it — the source, the envelope, the pitch
            — is untouched, so the hit is the same hit a quarter of a step later.

            A countdown rather than a start sample, because a block is split at every
            onset and the wait can outlast the chunk it began in; this is the only form
            that survives both a region boundary and a block boundary without a queue.

            Zero is what every hit gets when the knob is shut, and what every tail
            gets always: the delay is not a property of the sound, and a stolen hit is
            already sounding, so it hands over where it stands. `renderVoice` does not
            light the lane while this is positive, so a hit that has not started has
            not started, and nothing reads it as having.
        */
        int startDelay = 0;

        float gain = 1.0f;

        /**
            Cutoff band this hit reads its source with, set at note-on and tracked while it falls.

            At note-on it is the band for the fastest the read is ever going to get, so
            nothing aliases on the way down. `renderVoice` then re-picks it from the rate
            it is about to read at, and stops when the fall does: held at the entry value
            it cost 3.3 dB of top end over a drop of 12, and a cutoff that opens with the
            pitch is not a filter sweep the way a per-sample one would be, because every
            band in the table passes everything below the tightest of them identically.
        */
        int sincBand = SincKernelTable::cutoffBands - 1;

        /**
            True when the ratio is exactly one, so the read lands on whole source
            samples and needs no kernel at all.

            A ratio of exactly two also lands on whole samples, and reading every
            second one directly is the very thing the kernel exists to prevent: it
            folds everything above the new Nyquist straight back into the output.
        */
        bool integerPlayback = true;

        /** How hard the hit was struck, 0..1. Already folded into `gain`; kept
            separately so a diagnostic can report what the voice was played with. */
        float velocity = 1.0f;

        /**
            How open this hit's top end is, as the one-pole coefficient it reads with.

            The other half of `velocity`: see `Layout::softestHitCutoffHz`. One is the
            raw source, and the filter is skipped entirely at that value, which is what
            keeps a hard hit bit-identical to a hard hit before this existed.

            The two states are the filter's memory, and are carried into the tail pool
            with the coefficient so a stolen hit does not change tone on its way out.
        */
        float brightnessMix = 1.0f;
        float brightnessLeft = 0.0f;
        float brightnessRight = 0.0f;

        int age = 0;

        /**
            The choke group this hit belongs to, taken from its lane at note-on.

            Read by the next hit in the same group, which is what decides whether
            this voice is on its way out. Zero is no group, and a voice keeps the
            group it started in even if the lane's setting moves on, because the hit
            that is sounding was made under the old one.
        */
        int chokeGroup = 0;

        /** The hit's amplitude envelope, stepped per sample. */
        Envelope envelope;

        /** Set once the envelope has been told to let go, so it happens once. */
        bool released = false;

        bool active = false;

        /**
            True when the voice came from an incoming note rather than the grid.

            A note has to survive the transport being stopped: playing pads while
            the sequencer is idle is the whole point. Grid hits, by contrast, are
            faded out when the transport stops so that stopping is not abrupt.
        */
        bool fromMidi = false;

        /**
            True when this is a hit that has already been given up by a voice slot
            and is on its way out.

            A tail is not a voice: it is not counted, it is not stolen from, and it
            does not step its envelope — it reads the rest of its source under a
            ramp and disappears. `gain` holds the level it was taken over at, so the
            ramp starts exactly where the hit was interrupted rather than at unity.
        */
        bool inTail = false;

        /** Samples of output time the tail ramp has left; unused by a live voice. */
        double fadeRemaining = 0.0;

        /** The same ramp's total length, kept so the ramp is a division by a constant. */
        double fadeLength = 0.0;
    };

    void renderVoices (juce::AudioBuffer<float>& buffer, int startSample, int numSamples);

    /**
        Renders one voice into a region of the block.

        Shared by the voice pool and the tail pool because the read is the same
        work — same source, same kernel, same position bookkeeping — and the only
        difference is what multiplies it: a voice is scaled by its own envelope, a
        tail by the ramp it was handed over with.
    */
    void renderVoice (Voice& voice, float* outLeft, float* outRight, int numSamples);

    /**
        Hands a voice whose slot is wanted by a new hit to the tail pool.

        The alternative, which is what this replaced, was to overwrite it: the old
        hit stopped mid-cycle at whatever amplitude it had reached, and a step in a
        waveform is a click.

        `fadeSamples` is whose fade: the steal's, because the engine is out of
        voices, or the choke's, because the player asked for this hit to end.
    */
    void handOffToTail (Voice& victim, double fadeSamples);

    /**
        Puts every hit still sounding in `group` on its way out.

        Called by the hit that replaces them, before the voice pool is searched, so
        a choke also frees the slots the new hit needs — which is the difference
        between this and stealing: a choke is asked for, and it makes room.
    */
    void chokeVoicesInGroup (int group);

    /**
        Renders `buffer` from 0 to `numSamples` exactly once, splitting it at every
        point where something starts.

        Those points are the step onsets — when `followGrid` is true, so a stopped
        transport walks no pattern — and the sample offsets of the note events
        collected for this block. Each region between two of them is rendered once,
        and every voice that was already sounding is carried through all of them,
        so a note costs nothing but its own samples.

        Rendering the whole block and then rendering it again from each note's
        offset is what the old two-call shape did, and it summed everything twice
        and spent twice the source in the block a note arrived in.
    */
    void renderBlock (juce::AudioBuffer<float>& buffer, int numSamples, bool followGrid);

    /** Picks the note events out of this block's MIDI input, in arrival order. */
    int collectNoteEvents (const juce::MidiBuffer& midi, int numSamples);

    /**
        Moves every queued audition request into this block's note events.

        Returns the count taken, which is how many leading slots of `noteEvents` they
        hold. This is the reader half of the ring declared in `SequencerState`: audio
        thread only, and it neither allocates nor locks nor reads the clock, so a
        request costs the same as the note it becomes.
    */
    int drainPreviewRequests();

    /** The sample the next unapplied event starts on, or `numSamples` if none are left. */
    int nextNoteEventOffset (int numSamples) const;

    /** Starts or releases every pending event that arrives at or before `offset`. */
    void applyNoteEventsAt (int offset);

    /** Fires the armed step and arms the next one. */
    void fireNextOnset();

    /**
        Whether one strike is allowed to cut its lane's choke group.

        A ratchet's strikes are one hit spread across the room of one step, and a choke
        group is cut by a *hit* — so the first strike does the choking and the rest of them
        do not. Left to the plain rule, a roll of eight in a choked lane would silence
        itself on strike two and end as a click.

        Everything that is not a second or later strike of one roll asks for `apply`, which
        is the rule that shipped before this word existed: a note from a keyboard, a
        preview, and every hit from a cell that says one.
    */
    enum class StrikeChoke
    {
        apply,  ///< cuts the group, as a hit has always done
        skip    ///< a later strike of one ratchet, whose group the first strike already cut
    };

    /** Starts one hit, working out its playback ratio from the channel's own
        sample rate and pitch. Muted, empty and silent channels simply do not start
        a voice, so callers do not have to check first.

        `startDelaySamples` is the humanize offset for this hit and belongs to the grid
        alone: a note from a keyboard is already late or early by however the player
        struck it, and a preview is a diagnostic, so both ask for zero. The voice waits
        that many output samples before it reads anything, which is why a nonzero value
        also leaves the channel unlit until the wait is over.

        It is also how a ratchet's extra strikes arrive, which is what `choke` is for: see
        `StrikeChoke`. */
    void trigger (int channel, float velocity, bool fromMidi, int startDelaySamples = 0,
                  StrikeChoke choke = StrikeChoke::apply);
    void stopAllVoices();

    /** True when a voice's channel currently has sustain, so it holds rather than
        decaying away. A channel with no sustain is a one-shot: its envelope is
        released the moment it reaches the sustain stage. */
    bool channelHolds (int channel) const;

    /** Lets go of every voice on a channel that is still holding. */
    void releaseChannel (int channel);

    /** Where one step starts inside its bar, in steps from the top of the bar.

        With no swing this is simply the step number; swing pushes the off-beat
        of every swing pair later by the same fraction the web app uses.
    */
    double stepOnsetInSteps (int step) const;

    /** Steps from `head` to the next armed onset. */
    double distanceToNextOnsetInSteps (double head) const;

    /** Absolute position, in steps from the start of the song, of one step. */
    double onsetPositionInSteps (int step) const;

    /** Steps from the onset of `step` to the onset after it, on this bar's swing grid. */
    double gapToNextOnsetInSteps (int step) const;

    /** How far after its own onset one strike of a rolled cell starts, in steps. */
    double strikeDelayInSteps (int step, int strike, int strikes) const;

    /**
        The first step whose swung onset falls at or after `head`.

        Used whenever the head is not where the engine's own walk left it, which is
        the only moment the step number cannot be reached by counting on from the
        last one. It searches rather than divides, because at swing the onsets
        inside a pair are not evenly spaced — an eighth-note pair at swing 1 sits at
        0, 3 and 3.5 steps — so no arithmetic turns a position back into a step.
    */
    int firstStepAtOrAfter (double head) const;

    /**
        How far a host position may disagree with the engine's own head before the
        disagreement counts as the head having moved rather than as rounding.
    */
    double positionSlipInSteps() const;

    void beginBar (double tempo);
    void fireStep (int step);

    /**
        The half of `wouldThisHitFire` that does not have to work out what is playing.

        `fireStep` has already resolved the slot for this lane at this onset, and the
        pattern, the chance and the level of one hit have to be read out of that one
        slot: asking twice would let a panel change between the two reads roll a hit
        against the probability of a pattern that is not the one it came from. The
        public seam is this plus the resolution, so a test that asks the seam is asking
        the body the engine runs.
    */
    bool wouldHitInSlotFire (juce::int64 bar, int step, int channel, int slot) const;

    /**
        Whether one cell's roll lets the hit through.

        The answer comes out of a generator keyed on the seed, the bar and step the
        onset sits at, and the lane — nothing else. No counter, no state carried from
        the previous hit, so the same session played twice drops the same hits, and a
        bounce of sixty-four bars is the same take as the sixty-four bars the user
        heard. That is also why the key is the *absolute* onset: a hit three bars in
        does not inherit the history of the two before it.

        The draw is the top 24 bits of the mixed value, which is a number a `float`
        holds exactly, so the comparison is the same comparison on every machine that
        takes the same bits.
    */
    bool rollLetsHitThrough (juce::int64 bar, int step, int channel, float probability) const;

    /**
        The one-pole coefficient a hit of this strength reads its source through.

        A straight line from the darkest corner to unity, because the top of the
        travel has to be the recorded sample itself: see `softestHitMix` and
        `Layout::softestHitCutoffHz`.
    */
    float brightnessMixFor (float velocity) const noexcept
    {
        return velocity >= 1.0f ? 1.0f
                                : softestHitMix + (1.0f - softestHitMix) * juce::jmax (0.0f, velocity);
    }

    SampleStore& samples;
    SequencerState& state;

    /**
        Interpolation coefficients for pitched playback.

        A member rather than a shared or lazily built table because it is read from
        the audio thread and built once, off it, when the engine is constructed.
    */
    SincKernelTable kernelTable;

    double currentSampleRate = 44100.0;

    /**
        The one-pole coefficient of the softest hit the velocity law can express.

        Derived once per sample rate from `Layout::softestHitCutoffHz` rather than
        per hit: it is a property of the rate, not of the note.
    */
    float softestHitMix = 0.0f;

    std::array<Voice, (size_t) Layout::maxVoices> voices;

    /**
        The hits that have been stolen or choked, still fading.

        Sixteen, because the most a single onset can do is take a slot from each of
        the twelve lanes, the notes that arrive at the same sample need somewhere to
        go too, and one choke can empty a whole sustaining lane at once. A tail only
        lives for its fade — two milliseconds for a steal, twenty for a choke — so a
        bar later the pool is empty again; what it has to survive is one crowded
        sample, and overflow takes the slot from whichever tail is furthest down its
        ramp — the quietest thing in there.

        It is a pool and not an allocation because `trigger` runs on the audio
        thread, and a voice that gets stolen is exactly the moment the machine is
        busiest.
    */
    static constexpr int maxTails = 16;
    std::array<Voice, (size_t) maxTails> tails {};

    /** The three ramps in `Layout`, converted to output samples at `prepare`. */
    double endFadeSamples = 0.0;
    double stealFadeSamples = 0.0;
    double chokeFadeSamples = 0.0;

    /**
        The pitch drop's sweep, in samples, and the per-sample step of its glide.

        Both come from the rate alone — a 30 ms sweep is a different number of
        samples at 44.1 kHz than at 96 — so they are worked out once in `prepare`
        the way the fades are, and `trigger` only has to turn the lane's semitones
        into a starting multiplier.
    */
    double pitchDropSamples = 0.0;
    double pitchDropCoefficient = 1.0;
    std::array<std::atomic<bool>, (size_t) Layout::numChannels> soundingChannels {};
    int activeVoices = 0;
    int voiceCounter = 0;

    /**
        A note waiting to be started inside the block being rendered.

        Collected before the block is rendered, because the render walk has to know
        where they all are before it can split the block at them. Audio-thread
        state only: it is filled from the MIDI buffer the processor hands over, and
        from whatever audition requests were queued when the block began, and emptied
        again on the next call, so nothing here is ever shared.
    */
    struct NoteEvent
    {
        int sampleOffset;
        int channel;
        float velocity;
        bool isNoteOn;
    };

    /**
        How many notes one block can hold.

        The same ceiling the old MIDI pass had, so a block that arrives with more
        notes than this loses the extras rather than allocating for them — at 5 ms
        blocks there are only ever a handful, and a pad that sends more is sending
        more than twelve lanes worth of notes, which the mapping cannot use.
    */
    static constexpr int maxNoteEvents = 64;

    std::array<NoteEvent, (size_t) maxNoteEvents> noteEvents {};
    int numNoteEvents = 0;

    /** The next event to apply; everything before it has already started. */
    int nextNoteEvent = 0;

    /**
        The one clock: how far into the pattern we are, in steps, from the start
        of the song. Onsets are expressed in the same unit, so nothing can drift.
    */
    double position = 0.0;

    /**
        Where the head was expected to be when the next block started, and whether
        there is such an expectation.

        The host owns the position, so the only thing that says whether this engine
        is still in step with it is the comparison between where its own walk left
        the head at the end of the last block and where the host now says the block
        begins. Anything else — a stop, a scrub, a loop point, the first block after
        taking over — means the armed onset has to be re-derived from the new head,
        which is `firstStepAtOrAfter`.
    */
    double predictedHead = 0.0;
    bool havePrediction = false;

    /** The step whose onset has been armed, or -1 before the first one. */
    int onsetStep = -1;

    /** True while following a host's absolute musical time. */
    bool hostDriven = false;

    double samplesPerStep = 0.0;
    double stepsPerSecond = 120.0 / 60.0;

    std::atomic<int> currentStep { -1 };

    /**
        The bar of the last onset that fired, published for the panel.

        A mirror of what `fireStep` derived rather than a second clock: the GUI has no
        safe way to read `position`, and the one number it needs is the bar the machine was
        in when it chose the slot it is showing. Only written while some lane is rotating.
    */
    std::atomic<juce::int64> currentBar { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SequencerEngine)
};

} // namespace gb
