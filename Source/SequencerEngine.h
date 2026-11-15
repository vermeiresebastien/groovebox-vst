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
    Everything the audio thread reads while sequencing.

    The editor and the processor's parameter layer write these atomics; the
    engine only ever reads them, so there is no lock on the audio path.
*/
struct SequencerState
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

    SequencerState()
    {
        for (auto& row : pattern)
            for (auto& cell : row)
                cell.store (false);

        // Full velocity by default, so a new step behaves exactly as it did before
        // velocities existed.
        for (auto& row : velocity)
            for (auto& cell : row)
                cell.store (1.0f);

        for (int i = 0; i < Layout::numChannels; ++i)
        {
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

    /** Plays one channel directly, with `velocity` in 0..1, as a played note does.
        Used by `GrooveboxProcessor::previewChannel`, and by tests that do not want
        to build a MIDI buffer. */
    void triggerChannel (int channel, float velocity);

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

    /** The step whose onset is armed next, or -1. For diagnostics and tests. */
    int getOnsetStep() const noexcept { return onsetStep; }

    /** Playback head position, in steps from the start of the song. */
    double getPositionInSteps() const noexcept { return position; }

    /** Where the engine places one step inside its bar, in steps. */
    double getStepOnsetInSteps (int step) const { return stepOnsetInSteps (step); }

    /** Absolute position in steps of one step, given the bar the head is in. */
    double getOnsetPositionInSteps (int step) const { return onsetPositionInSteps (step); }

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

    /** The sample the next unapplied event starts on, or `numSamples` if none are left. */
    int nextNoteEventOffset (int numSamples) const;

    /** Starts or releases every pending event that arrives at or before `offset`. */
    void applyNoteEventsAt (int offset);

    /** Fires the armed step and arms the next one. */
    void fireNextOnset();

    /** Starts one hit, working out its playback ratio from the channel's own
        sample rate and pitch. Muted, empty and silent channels simply do not start
        a voice, so callers do not have to check first. */
    void trigger (int channel, float velocity, bool fromMidi);
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
        state only: it is filled from the MIDI buffer the processor hands over and
        emptied again on the next call, so nothing here is ever shared.
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

    /** Last step that actually fired, or -1 when stopped. */
    int lastFired = -1;

    /** True while following a host's absolute musical time. */
    bool hostDriven = false;

    double samplesPerStep = 0.0;
    double stepsPerSecond = 120.0 / 60.0;

    std::atomic<int> currentStep { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SequencerEngine)
};

} // namespace gb
