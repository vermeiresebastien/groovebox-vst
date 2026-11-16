#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

namespace gb
{

/** Fixed layout of the groovebox, mirroring the drum page of the web app. */
namespace Layout
{
    inline constexpr int numSteps    = 16;
    inline constexpr int numChannels = 12;
    inline constexpr int maxVoices   = 64;

    /**
        The note the first channel answers to.

        Channels are consecutive from here, so the default layout is C1 upwards
        (MIDI 36..47): what a General MIDI drum map uses for its first twelve
        pieces, and what most pad controllers send out of the box. Octave naming
        follows the DAW convention, where MIDI 36 is C1 and middle C (60) is C3.
    */
    inline constexpr int firstMidiNote = 36;

    /**
        Envelope ranges, in seconds, plus the defaults.

        A, D and R are stored normalised (0..1) and mapped onto these maxima, so
        the editor never has to know about seconds. The ranges are short enough
        that the control stays useful across its whole travel for percussion, and
        long enough to shape something sustained.
    */
    inline constexpr double maxAttackSeconds  = 2.0;
    inline constexpr double maxDecaySeconds   = 3.0;
    inline constexpr double maxReleaseSeconds = 4.0;

    /** Defaults: effectively a one-shot until sustain is raised. */
    inline constexpr float defaultAttack  = 0.0f;   // 0 s
    inline constexpr float defaultDecay   = 0.25f;  // ~0.21 s
    inline constexpr float defaultSustain = 0.0f;   // one-shot
    inline constexpr float defaultRelease = 0.15f;  // ~0.14 s

    /**
        Master trim range, in dB, and where it starts.

        Voices sum at unity, so this is the only place the output is scaled, and
        the default replaces the 0.8 (-1.94 dB) that used to be buried inside
        every voice. Rounded to -2 dB it sits 0.06 dB from that, which is well
        under an audible difference, so a session saved before the trim existed
        comes back at the level it was written at instead of 2 dB louder.

        +6 dB at the top is deliberately reachable: it is the point where the clip
        indicator earns its place, and a trim that could not clip could not warn.
    */
    inline constexpr float minMasterGainDb     = -24.0f;
    inline constexpr float maxMasterGainDb     = 6.0f;
    inline constexpr float defaultMasterGainDb = -2.0f;

    /**
        The swing pair size the engine actually honours, given what was asked for.

        A pair has to divide the bar. At grid 3 the pairs run 0-2, 3-5, 6-8 and so
        on, so the step that closes the bar is the last of an unfinished pair and
        the second half of the bar swings against the first — and because 16 is not
        a multiple of 3, the next bar starts a sixteenth later than this one did.
        That is not a grid, it is a drift, and the engine would be following it
        wherever a host's automation took it.

        The panel only ever writes 2 or 4, so this rule exists for the other way in:
        `swingGrid` is an int parameter from 2 to 4, and a host can automate or type
        a 3. Answering a 3 with the wider pair is what the two buttons already mean,
        and it keeps a ramp monotone instead of passing through a broken grid on the
        way. Both the engine and the transport bar call this, which is the only thing
        that can keep the light on the button matched to the timing you hear.
    */
    inline int usableSwingGrid (int requested) noexcept
    {
        return requested >= 3 ? 4 : 2;
    }

    /**
        How the last moments of a hit are ended, in seconds of output time.

        A voice stops for one of two reasons: the read ran off the end of the
        source, or the pool was full and the engine took the slot away from the
        oldest hit. Both used to happen at whatever amplitude the waveform was at,
        which is a step, and a step is a click — measured at 0.79 against a
        reference tone whose own sample-to-sample slew is 0.14.

        So both get a ramp instead. `endFadeSeconds` is longer because the end of a
        sample is the end of the *sound*, and a 6 ms taper is shorter than the
        smallest useful drum tail while still being far too fast to hear as a fade;
        `stealFadeSeconds` is shorter because the stolen voice is being replaced by
        another hit on the same lane, and a long overlap between two copies of one
        sample is its own artefact.

        Neither is a limiter, and neither is allowed to change the level of anything
        that is not already ending.
    */
    inline constexpr double endFadeSeconds   = 0.006;

    /**
        The shortest ramp the engine will play, and so the length of a steal fade.

        Every falling control has a bottom: decay to no sustain, release to
        nothing, a voice handed to the tail pool. At its bottom a parameter asks
        for a change that lands in a single sample, which is the same step this
        file is about — measured as 0.7943 of slew for a release set to nothing on
        a hit at full trim, against a limit of 0.2129 for the same fixture. Two
        milliseconds is far too fast to hear as a fade and about a hundred times
        too slow to click.

        The envelope's attack is deliberately not floored. A hit that starts from
        silence does not cut anything the listener could hear the sample before,
        and a 2 ms ramp on every onset would soften the transient: that is a
        change of sound nobody asked for, whereas this one only touches parts of
        the envelope that were already ending.
    */
    inline constexpr double minimumRampSeconds = 0.002;
    inline constexpr double stealFadeSeconds   = minimumRampSeconds;

    /**
        Choke groups: how many there are, and how long a choked hit takes to go out.

        A group is the statement that these lanes cut each other — an open hat and
        the closed hat that replaces it, a ride and whatever lands on it. Hitting a
        lane in a group fades every hit already sounding in that group, which is the
        one thing a drum machine has to do that a sampler's voice stealing cannot:
        stealing is about running out of voices, and a choke is about the note the
        player meant.

        Three groups, because every kit needs at most a handful — hats, cymbals,
        percussion — and a number written on a button has to be readable at 30 px
        wide. `0` means the lane is in no group and cuts nothing.

        The fade is a decisecond-scale choice measured against the steal's: twenty
        milliseconds is long enough that a choked crash sounds like a choke rather
        than a gate, and short enough that it does not muddy the hit that replaced
        it. It is a ramp, not a level change: nothing that is not already being cut
        is touched.
    */
    inline constexpr int   numChokeGroups    = 3;
    inline constexpr double chokeFadeSeconds = 0.020;

    /**
        The corner a hit struck as softly as the parameter can ask for is filtered at.

        A drum does not only get quieter when it is hit harder: the stick excites the
        top end too, so a soft hit is darker as well as smaller. Velocity has driven
        gain alone until now, which is why every velocity of one sample sounded like
        the same hit turned down rather than a different way of striking it.

        So velocity drives a one-pole as well, and travels from this corner at zero
        velocity to no filter at all at the top. The top being *no filter* is the
        point: a hard hit has to read the recorded sample exactly as it is, so the
        loudest thing the machine plays is bit-for-bit what it was before this law
        existed, and the law cannot be a way of quietly retuning the kit.

        900 Hz is a judgement about a kit, not a measurement: low enough that a soft
        hit is audibly played rather than turned down, high enough that it still has
        a beater in it and does not become a hand clap.
    */
    inline constexpr float softestHitCutoffHz = 900.0f;

    /**
        How far a hit may be asked to fall at its own start, and how fast.

        The 808's kick is not one pitch: the note arrives from slightly above and
        lands on its tuned frequency within a few tens of milliseconds, and that
        short glide is what makes it read as a *thump* rather than a click on top
        of a sine. The same shape on a hat or a tom is a one-parameter way to make
        a kit sound struck instead of triggered.

        Twelve semitones is the top because that is the point where the sweep stops
        being a tuning and becomes a siren, and the travel is in semitones rather
        than a vague amount so the number on the control says what you hear.

        The sweep is a fixed 30 ms and is not a parameter, on purpose: the shape is
        the effect, and a second knob for its length turns one honest control into
        an approximation of a synthesizer. Five time constants across it, which is
        the same convention the envelope falls use, so the glide is 99.3% done when
        the row says it is and exactly done on the sample it ends on.

        Nothing about this is automatic. A lane at zero glides nowhere, and the
        engine does not get to decide that a kick would have looked better with a
        drop in it.
    */
    inline constexpr double maxPitchDropSemitones = 12.0;
    inline constexpr double pitchDropSweepSeconds = 0.030;
    inline constexpr double pitchDropTimeConstants = 5.0;

    /**
        How far behind its own grid position a humanised hit may start, in steps.

        A fraction of a step rather than a number of milliseconds, for the same reason
        swing is one: the thing being imitated is a relationship to the grid, not a
        delay. A drummer who is 30 ms late at 120 is 15 ms late at 240 and still sounds
        the same way, and a fixed time would turn into a flam at slow tempos.

        A quarter of a step is the top because of the closest two onsets this machine
        can ever produce. Swing puts an off-beat pair at 0, 1.5, 3, 3.5 within its
        grid — see `swingDepth` — so two hits can be half a step apart, measured at the
        shuffle rather than assumed: 0.5000 steps on 1/8 and on 1/16, 1.0000 straight.
        Anything that can push one further than that gap behind itself can trade their
        order. Half of that gap — a quarter of a step — is the largest offset that
        cannot: a late hit can join the next one, it can never overtake it, so the
        pattern the user drew stays the pattern that comes out and only its feel moves.

        Late only, and that is a consequence of where the offset lives rather than a
        limitation of taste. A hit that arrives *before* its onset would have to be
        scheduled before the clock reaches it, which means a queue of pending hits and
        an onset that can be behind the playhead — the one arithmetic path in the
        sequencer that answers a late request by pushing the step a whole bar forward.
        Playing slightly ahead is what the attack of a fast sample does to the perceived
        onset anyway.
    */
    inline constexpr double maxHumanizeLateSteps = 0.25;

    /**
        Which generation of the saved record this build writes.

        The number goes on the root of the state as `stateVersion`, and the step
        payload also carries the same number as its own `v<n>:` tag. That is
        deliberately two places for one fact: the root says what generation the whole
        record belongs to, the tag says what codec the string in front of the reader
        is in, and the tag is the one that has to be trusted, because a payload can
        travel on its own — through an undo, a preset the user edited by hand, or a
        session this build only half-read.

        Version 1 was rows of two digits a step, with no version anywhere: a session's
        shape was its only label, so a row of sixteen characters meant one digit a
        step and a row of thirty-two meant two. Those rows still load. Version 2 is
        two bytes a step behind the tag, which is what lets a step keep a level while
        it is switched off and lets a level be a MIDI number rather than a hundredth.
        Version 3 is three: the third byte is how often the step is allowed to fire, as
        a number of 255ths, so 255 means every time and is what every cell written
        before this feature came back from reads as. A v2 record therefore needs no
        migration — the probability of a step it does not mention is one, which is
        exactly what a v2 machine played — but it does come back short of the grid this
        build writes, and the length of a payload is a fact the tests care about.

        A record whose tag this build cannot name is not decoded, not repaired and not
        re-encoded until something changes the grid, so opening a newer session in an
        older plugin costs the user the pattern rather than the file.
    */
    inline constexpr int stateVersion = 3;

    /**
        How many rungs a step's probability has, both in the saved record and on the
        grid: the byte a cell is written as is a number of these, so 255 is a step that
        fires every time and 0 is one that never does.

        255 rather than 100 because a byte is the field, and a lattice that fills it is
        one the record cannot round. The panel still says it as a percentage.
    */
    inline constexpr int chanceLevels = 255;
}

/** Palette lifted from the Earworm Studio drum page. */
inline const juce::Colour defaultLaneColour { 0xfffd8eff };

inline const std::array<juce::uint32, 8> lanePalette {
    0xfffd8eff, 0xff56beff, 0xff99ff71, 0xffc06bff,
    0xffff9a00, 0xffff5a5a, 0xff56ffd5, 0xffffe066
};

/** Background / chrome colours taken from styles.css. */
namespace Colours
{
    inline const juce::Colour background { 0xff292929 };
    inline const juce::Colour rule       { 0xff626262 };
    inline const juce::Colour text       { 0xffffffff };
    inline const juce::Colour muted      { 0xff9a9a9a };
    inline const juce::Colour cellOff    { 0x1affffff };
    inline const juce::Colour chip       { 0x14ffffff };
    inline const juce::Colour chipBorder { 0x66ffffff };

    /** Meter fill, by how close the level is to 0 dBFS: safe, hot, over. */
    inline const juce::Colour meterSafe  { 0xff99ff71 };
    inline const juce::Colour meterHot   { 0xffffe066 };
    inline const juce::Colour meterOver  { 0xffff5a5a };
}

} // namespace gb
