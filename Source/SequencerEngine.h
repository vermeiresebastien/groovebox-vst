#pragma once

#include "Common.h"
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
    analogue envelope does, which is what the web app's envelope does too.
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
        attackSamples  = juce::jmax (1.0, (double) attackSeconds * sampleRate);
        decaySamples   = juce::jmax (1.0, (double) decaySeconds * sampleRate);
        releaseSamples = juce::jmax (1.0, (double) releaseSeconds * sampleRate);
        sustain = juce::jlimit (0.0f, 1.0f, sustainLevel);
    }

    void noteOn() noexcept
    {
        value = 0.0f;
        stage = Stage::attack;
        stagePosition = 0.0;
        releaseBegin = 0.0f;
    }

    void noteOff() noexcept
    {
        if (stage == Stage::idle || stage == Stage::release)
            return;

        stage = Stage::release;
        releaseBegin = value;
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

            case Stage::decay:
            {
                const auto progress = juce::jlimit (0.0, 1.0, stagePosition / decaySamples);
                value = 1.0f - (1.0f - sustain) * (float) progress;
                stagePosition += 1.0;

                if (progress >= 1.0)
                {
                    value = sustain;
                    stage = Stage::sustain;
                }
                break;
            }

            case Stage::sustain:
                value = sustain;
                break;

            case Stage::release:
            {
                const auto progress = juce::jlimit (0.0, 1.0, stagePosition / releaseSamples);
                value = releaseBegin * (1.0f - (float) progress);
                stagePosition += 1.0;

                if (progress >= 1.0)
                {
                    value = 0.0f;
                    stage = Stage::idle;
                }
                break;
            }
        }

        return value;
    }

    Stage getStage() const noexcept { return stage; }
    bool isActive() const noexcept { return stage != Stage::idle; }
    float getCurrentValue() const noexcept { return value; }

private:
    double sampleRate = 44100.0;
    double attackSamples = 1.0;
    double decaySamples = 1.0;
    double releaseSamples = 1.0;

    float sustain = 0.0f;
    float value = 0.0f;
    float releaseBegin = 0.0f;

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

    /** Per-channel output level, linear gain. */
    std::array<std::atomic<float>, (size_t) Layout::numChannels> level {};

    /** Per-channel pitch offset in semitones, -36 .. +12. */
    std::array<std::atomic<int>, (size_t) Layout::numChannels> pitch {};

    std::atomic<bool> channelMuted[Layout::numChannels] {};

    /** Internal transport, used when the host has no transport (standalone). */
    std::atomic<bool> playing { false };

    /** Swing pushes the off-beat steps later. 0 = straight, 1 = heavy shuffle. */
    std::atomic<float> swing { 0.0f };

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

        for (int i = 0; i < Layout::numChannels; ++i)
        {
            level[(size_t) i].store (1.0f);
            pitch[(size_t) i].store (0);
            channelMuted[i].store (false);

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

    /** Renders one host block, applying host transport when it is available. */
    void processBlock (juce::AudioBuffer<float>& buffer,
                       const juce::AudioPlayHead::PositionInfo& position,
                       bool haveHostPosition);

    /**
        Plays any incoming note-on through the channel it is mapped to.

        Lanes map to consecutive notes from `Layout::firstMidiNote`, so the
        default layout answers to C1 upwards. A note off is ignored: each drum
        voice is a one-shot whose own envelope decides how long it rings, which is
        what a drum machine does.

        `sampleOffset` is where in the block the message sits, so a note played a
        little late still starts on the right sample.
    */
    void processMidi (const juce::MidiBuffer& midi, int numSamples);

    /** Channel a note maps to, or -1 when the note is outside the range. */
    static int channelForMidiNote (int midiNote) noexcept;

    /** Note that triggers a channel, or -1 when the channel is out of range. */
    static int midiNoteForChannel (int channel) noexcept;

    /** Plays one channel directly, with `velocity` in 0..1. Used by the MIDI
        path, and by tests that do not want to build a MIDI buffer. */
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
        float gain = 1.0f;
        int age = 0;

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
    };

    void renderVoices (juce::AudioBuffer<float>& buffer, int startSample, int numSamples);
    void trigger (int channel, double increment, float velocity, bool fromMidi);
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

    void beginBar (double tempo);
    void fireStep (int step);

    SampleStore& samples;
    SequencerState& state;

    double currentSampleRate = 44100.0;

    std::array<Voice, (size_t) Layout::maxVoices> voices;
    std::array<std::atomic<bool>, (size_t) Layout::numChannels> soundingChannels {};
    int activeVoices = 0;
    int voiceCounter = 0;

    /**
        Velocity of the hit each voice is playing.

        Kept per voice rather than applied once, so rendering a voice mid-block —
        which happens when a note arrives part way through — keeps the velocity it
        was struck with.
    */
    std::array<float, (size_t) Layout::maxVoices> voiceVelocity {};

    /**
        The buffer being filled by the current `processBlock`.

        A note arriving part way through a block has to be rendered from its own
        sample onward, and `processMidi` is called by the processor after the
        sequencer has had its turn, so it needs to know which buffer to write to.
    */
    juce::AudioBuffer<float>* currentBlock = nullptr;

    /**
        The one clock: how far into the pattern we are, in steps, from the start
        of the song. Onsets are expressed in the same unit, so nothing can drift.
    */
    double position = 0.0;

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
