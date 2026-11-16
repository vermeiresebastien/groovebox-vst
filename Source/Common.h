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
        How many pattern slots one channel owns.

        Four rather than the eight the nearest thing to this in the family ships
        (`drumgen`/SliceForge: 16 pads x 32 steps, 8 banks), because a bank the user never
        fills is a row of controls that overstates the machine. Four is one slot a bar over
        a four-bar phrase — the shortest cycle that still reads as an arrangement instead of
        a flip — and it is the number the panel can show as one small square per slot inside
        the label column without pushing anything else off it.

        A slot is a whole grid for one channel: the same sixteen steps with their own level
        and their own chance, so switching slots switches the pattern and everything drawn
        on it together. See `SequencerEngine::slotPlaying` for which slot is audible.
    */
    inline constexpr int numPatternSlots = 4;

    /**
        The periods the evolve control cycles through, in bars.

        Zero first and off, so what a lane does before anyone touches this is play the slot
        it is pointing at. 1, 2, 4 and 8 after it because those are the lengths a bar-counted
        change stays musical against a 4-bar loop; a 3-bar step deliberately walks out of
        phase with it, which is a different idea and not what this control is for.

        The ladder belongs to the control, not to the law: `slotPlaying` divides by whatever
        number of bars the state holds, so a session edited outside the panel is read rather
        than corrected. Only `cycleChannelEvolve` knows this list.
    */
    inline constexpr std::array<int, 5> evolvePeriods { 0, 1, 2, 4, 8 };

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
        How far a jittered hit may depart from the velocity its step was written at.

        A fraction of *that step's own level*, not a number of rungs of the range, and
        the reason is what keeps a rest a rest. An additive wobble gives every cell a
        floor of its own: a step drawn at zero — which is not a quiet hit but the
        absence of one — comes back sounding, and a machine that plays hits the grid
        says not to play is not varying the take, it is writing a second pattern behind
        it. Multiplied, a soft hit moves by a small absolute amount and a loud one by a
        large one, in the same proportion, which is the relationship a hand actually
        has to a pad.

        A quarter of the written level. What that costs the drawn shape is arithmetic:
        a quieter hit can overtake a louder one only when it is more than
        (1 - r) / (1 + r) of it, which at a quarter is 0.6 — so any accent drawn more
        than 1.67 times its neighbour keeps its place in every take, and the travel
        would have to reach a third before a plain 2:1 accent could be inverted. A
        quarter is inside that with room, and it is the top of the knob rather than the
        typical hit: the draw is squared about zero, so the average departure is a third
        of the travel — eight per cent of the level, under a decibel.

        A judgement about a kit, like `softestHitCutoffHz` above. Two consequences of it
        are gated rather than argued: the deviation can push a hit past the top of the
        7-bit lattice but never past 127, so opening this cannot make the loudest thing
        in the set louder, and because velocity is two laws here — gain and tone — a
        jittered hit changes brightness along with its level. That is the mechanism
        working as designed, and its cost is measured and stated rather than bought
        back.
    */
    inline constexpr double maxTouchRatio = 0.25;

    /**
        How many strikes one grid hit can be spread into.

        A ratchet, and the count is the whole control: a cell at 1 is one hit on its own
        onset, which is what every cell of every session drawn before this feature means,
        and a cell at 8 is eight. The top is eight because that is where the spacing stops
        being placeable by this machine rather than a taste. The strikes are spread over the
        gap to the next onset, whose shortest form is half a step — the off-beat pair at
        full shuffle, see `maxHumanizeLateSteps` — so eight of them are 0.0625 of a step
        apart, which at the top of the tempo range is 103 samples. Past that a roll is not
        eight hits but a buzz, and it is eating the voice pool to do it: twelve lanes at
        eight are 96 strikes against `maxVoices` of 64.

        What a roll is not allowed to do is overtake the next onset. The gap it spreads
        across is the one the grid actually gave this step, so the last strike starts
        (n-1)/n of the way along it and lands strictly inside the space the step was given.
        A roll can fill a step and cannot reorder the pattern — the same guarantee
        `maxHumanizeLateSteps` makes about the timing knob, for the same reason: what the
        user drew stays the clock, and only the sound inside one slot of it moves.
    */
    inline constexpr int maxRatchetHits = 8;

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
        before this feature came back from reads as. Version 4 is four: the fourth byte is
        how many strikes that cell asks for, held as the number itself and not as a rung of
        anything, so 1 — one hit, on the onset, exactly as before — is what every cell
        written before rolls existed comes back as. A v3 record therefore needs no
        migration either, for the same reason a v2 one needed none: the roll count of a step
        it does not mention is one, which is exactly what a v3 machine played. It does come
        back short of the grid this build writes, and the length of a payload is a fact the
        tests care about.

        A record whose tag this build cannot name is not decoded, not repaired and not
        re-encoded until something changes the grid, so opening a newer session in an
        older plugin costs the user the pattern rather than the file.
    */
    inline constexpr int stateVersion = 4;

    /**
        How many rungs a step's probability has, both in the saved record and on the
        grid: the byte a cell is written as is a number of these, so 255 is a step that
        fires every time and 0 is one that never does.

        255 rather than 100 because a byte is the field, and a lattice that fills it is
        one the record cannot round. The panel still says it as a percentage.
    */
    inline constexpr int chanceLevels = 255;

    /**
        The three bytes a cell is written as, apart from its flag.

        Named, and in the header rather than beside the writer, because the same three
        roundings are needed by a second question: "is there anything in this slot worth
        writing down". A slot that carries a level of 0.999 would be written as 127, which
        is what full is, and a test that compared floats would spend a record on a
        difference the record cannot hold.

        One definition is also the whole reason the bank's marks and its copy rule cannot
        drift apart. `SequencerEngine::isSlotFilled` and the save path's
        `slotCarriesAnything` ask about the same three fields of the same cell, and two
        spellings of that question is how a duplicate starts overwriting a level the user
        set on a step that is switched off — data that no dot on screen would have
        admitted was there.

        The count of strikes is a `int` and not a normalised float like the two above it,
        because it is a number of things rather than an amount of something: there is no
        roll of 3.4 hits, and a rounding law for a field the panel can only set to a whole
        number is a second place for the lattice to be wrong.
    */
    inline juce::uint8 levelByte (float level) noexcept
    {
        return (juce::uint8) juce::jlimit (0, 127, juce::roundToInt (level * 127.0f));
    }

    inline juce::uint8 chanceByte (float chance) noexcept
    {
        return (juce::uint8) juce::jlimit (0, chanceLevels,
                                           juce::roundToInt (chance * (float) chanceLevels));
    }

    /**
        The one rung of the probability lattice a byte means, and the snap of any float onto
        the rung it would be written as.

        These were spelled out at each call site — the panel's `setStepChance` did the
        round-and-divide inline, the codec's reader divided the raw byte by the lattice — and
        a bulk writer asking for the same thing would have been the third spelling of a
        lattice that exists in exactly one number, `chanceLevels`. The snapping rule is not
        cosmetic: a probability is the one field the panel shows as a percentage, so a value
        that reaches the engine unrounded is a screen that lies about the file.
    */
    inline float chanceFromByte (juce::uint8 byte) noexcept
    {
        return (float) byte / (float) chanceLevels;
    }

    inline float chanceRungFor (float chance) noexcept
    {
        return chanceFromByte (chanceByte (chance));
    }

    /**
        The fourth byte: the number of strikes, written as itself.

        Clamped rather than validated, because the field it comes from is an atomic a host
        or a half-read record can have left anywhere, and one byte that meant 60 has to
        still cost one hit rather than sixty.
    */
    inline juce::uint8 ratchetByte (int hits) noexcept
    {
        return (juce::uint8) juce::jlimit (1, maxRatchetHits, hits);
    }

    /**
        Does one cell say anything a session without it does not already mean?

        Off, full, every-time and one strike is the state a missing record means, so a cell
        counts as carrying information when it disagrees with that in any of the four ways.
        The comparison is on the bytes for the reason above: a level that rounds to 127 says
        nothing the defaults do not, however many digits the float has.
    */
    inline bool cellSaysAnything (bool on, float velocity, float chance, int roll) noexcept
    {
        return on || levelByte (velocity) != 127
                   || chanceByte (chance) != (juce::uint8) chanceLevels
                   || ratchetByte (roll) != 1;
    }

    //==========================================================================
    // The channel strip's height rule.
    //
    // One strip holds seventeen controls in eight rows, and the editor hands the strip
    // whatever the window has left after the title, the transport bar and the grid. So
    // the rows have to give way somehow, and until now they did it by a comparison that
    // could not be satisfied: the two sets of sizes cost 268 px and 228 px of room, and
    // the switch between them was at 219. Above 268 the roomy set fits; in [219, 268) it
    // does not; and below 219 the compact set is chosen, which needs 228 and so has never
    // fitted either. There is no strip height at which the old compact branch worked,
    // which is why a 500 px window put two of every strip's seventeen controls outside it
    // (`out/layout-tight-probe.log`) while the check meant to catch that reported
    // `spare 0` -- JUCE's `removeFromTop` clamps, so a row that ran out of space got a
    // zero-height rectangle *inside* the strip and the leftover was always empty.
    //
    // The rule below is therefore not two sets of numbers and a guess at where one ends.
    // It is one budget: the stage is chosen by what is actually available, so what the
    // plan asks for is never more than the room it was computed from, and the deficit it
    // can still report is honest. Three things give way, in the order the strip's own
    // comment has always claimed -- space between the blocks, then the blocks themselves,
    // and only then the painted section names, because a caption is decoration and a row
    // at its floor is still a control you can drag.
    inline constexpr int stripFrame = 6;   // `getLocalBounds().reduced (6)`

    /** The height the rows and captions are actually laid out in. */
    inline constexpr int stripRoomFor (int stripHeight) noexcept
    {
        return stripHeight - 2 * stripFrame;
    }

    /**
        The four budgets the layout gives way at, in px of room.

        Each is the summed cost of the stage it opens, so the number is not a threshold
        someone chose: it is what that stage needs, and `stripPlan` picks the first one
        that fits. `stripHardRoom` is the last of them, below which even the floors
        overflow and a control loses its rectangle -- the strip cannot be made honest
        there by any rule, only by the editor refusing to be that short.

        The chain is 268 = 192 rows + 44 captions + 32 gaps, 246 with the gaps at 10,
        210 with the rows at their 156 px floors, 166 with the captions gone, and
        123 = 156 - 43 + 1 + 10 once the ADSR row has spent the whole of its travel.
    */
    inline constexpr int stripFullRoom  = 268;
    inline constexpr int stripTightRoom = 246;
    inline constexpr int stripFloorRoom = 210;
    inline constexpr int stripBareRoom  = 166;
    inline constexpr int stripHardRoom  = 123;

    /** One row of one strip, at one available height. See `stripPlan`. */
    struct StripPlan
    {
        int sectionGap = 0;   ///< space between two blocks, taken five times
        int soundGap   = 0;   ///< the extra space under the sample button
        int caption    = 0;   ///< one of the four painted section names, taken four times

        int header = 0, sound = 0, clearRow = 0, pitchRow = 0;
        int dropRow = 0, level = 0, envelope = 0, muteRow = 0;

        int stage = 0;    ///< 0 full, 1 gaps in, 2 rows at their floors, 3 no captions
        int wanted = 0;   ///< everything at the stage's own sizes, before any squeeze
        int spare = 0;    ///< room - wanted; negative says the strip is short
    };

    /**
        What one channel strip should do with `room` px of height.

        The answer is a number per row rather than a flag, so the strip lays out from it
        without re-deciding anything, and the layout test can ask the same function what
        it expected at each size instead of borrowing the strip's arithmetic -- the
        mistake AGENTS.md records for the bar's wrap rule, where a gate that compared the
        bar's height with `heightForWidth (its width)` stayed green on a rule that had been
        turned into `return false`.

        Monotone by construction: a taller room never buys a shorter row. `envelope` is
        where a shortfall lands, because `EnvelopeControls` divides its own height by four
        and has no child components (`boundsFor` uses `jmax (1, height / 4)`), so it degrades
        into a shorter drag band instead of a zero-sized control -- which is the failure this
        rule exists to prevent. Every other row *is* a child, so squeezing one of those is
        how the mute button went missing.
    */
    inline StripPlan stripPlan (int room) noexcept
    {
        StripPlan p;

        p.stage = room >= stripFullRoom    ? 0
                  : room >= stripTightRoom ? 1
                  : room >= stripFloorRoom ? 2
                                           : 3;

        const auto rowsAtFloor = p.stage >= 2;
        const auto gapsRoomy = p.stage == 0;

        p.sectionGap = gapsRoomy ? 6 : 2;
        p.soundGap = gapsRoomy ? 2 : 0;

        // The caption's 11 px is not a taste: `paintCaption` draws a 12 px font into it,
        // so it is either that or nothing, which is why it is the third thing to give and
        // not a stage of its own.
        p.caption = p.stage >= 3 ? 0 : 11;

        p.header   = rowsAtFloor ? 16 : 20;
        p.sound    = rowsAtFloor ? 18 : 22;
        p.clearRow = rowsAtFloor ? 18 : 20;
        p.pitchRow = rowsAtFloor ? 16 : 20;
        p.dropRow  = rowsAtFloor ? 12 : 14;
        p.level    = rowsAtFloor ? 14 : 18;
        p.envelope = rowsAtFloor ? 44 : 56;
        p.muteRow  = rowsAtFloor ? 18 : 22;

        p.wanted = 5 * p.sectionGap + p.soundGap + 4 * p.caption
                   + p.header + p.sound + p.clearRow + p.pitchRow
                   + p.dropRow + p.level + p.envelope + p.muteRow;

        p.spare = room - p.wanted;

        // Below the bare stage's 166 the ADSR row pays for the rest, a px at a time, until
        // it reaches `stripHardRoom` and has nothing left to give.
        if (p.spare < 0)
            p.envelope = juce::jmax (1, p.envelope + p.spare);

        return p;
    }

    //==============================================================================
    //  The editor's own vertical budget. See `editorPlan`.
    //==============================================================================
    inline constexpr int editorMargin   = 18;   // chrome all the way round
    inline constexpr int editorGap      = 10;   // under the bar, and above the grid
    inline constexpr int editorTitleAir = 4;    // between the wordmark row and the bar
    inline constexpr int wordmarkHeight = 34;   // the GROOVEBOX row: mark, hint, rule
    inline constexpr int stripDesignHeight = 285;   // a strip at stage 0, spare included
    inline constexpr int gridGuardHeight = 120; // the least the grid is ever left with

    /**
        How tall a channel strip has to be to be laid out at `stage` of `stripPlan`.

        Stage 0 keeps the design height, 5 px more than the strip's own 268 px budget,
        because that slack is what it has always been given and what it still spends on
        nothing. Stages 2 and 3 are priced exactly: each of those budgets *is* the stage's
        own `wanted`, so the room and the cost are the same number, and the strip frame
        goes on top. Stage 1 gives up the wordmark, not the strip, so it pays 285 too.
    */
    inline int stripHeightForStage (int stage) noexcept
    {
        return stage <= 1 ? stripDesignHeight
                          : (stage == 2 ? stripFloorRoom : stripBareRoom) + 2 * stripFrame;
    }

    /** Everything but the wordmark and the strip: air, bar, two gaps and the grid's floor. */
    inline int editorChromeHeight (int barHeight, int gridFloor) noexcept
    {
        return editorTitleAir + barHeight + editorGap * 2 + gridFloor;
    }

    /** What the editor spends at `stage`, once the bar and the grid floor are known. */
    inline int editorCostForStage (int stage, int barHeight, int gridFloor) noexcept
    {
        return editorChromeHeight (barHeight, gridFloor) + stripHeightForStage (stage)
               + (stage == 0 ? wordmarkHeight : 0);
    }

    /** What the editor's `setResizeLimits` floor has to be: the cheapest stage, worst bar. */
    inline int editorFloorHeight (int barHeight, int gridFloor) noexcept
    {
        return editorCostForStage (3, barHeight, gridFloor) + editorMargin * 2;
    }

    /** One window height, laid out. See `editorPlan`. */
    struct EditorPlan
    {
        bool wordmark = true;   ///< draw the GROOVEBOX row at all?
        int  strip    = 0;      ///< px handed to every channel strip
        int  wanted   = 0;      ///< what this stage costs, below the margins
        int  spare    = 0;      ///< interior - wanted; negative says the window is short
        int  stage    = 0;      ///< 0 full, 1 no wordmark, 2 strip at floors, 3 no captions
    };

    /**
        What the editor should do with `interior` px of height under a `barHeight` px bar.

        Four stages, each giving up one thing and in this order: the GROOVEBOX row (34 px
        of branding, the one-line load/MIDI hint and the rule, all three drawn inside it),
        then the strip's slack and its rows down to the floors `stripPlan` already prices,
        then the strip's captions. The grid is never asked to pay: it is left its own floor,
        derived from the row height the bank strip needs for a name line, so a short window
        loses decoration rather than losing a control or a bank mark.

        Stated as costs, not thresholds, for the reason AGENTS.md records about the bar's
        wrap rule -- a gate that compared the bar's height with the same function `resized()`
        had just called proved only that both halves read one number, and stayed green when
        that number was made a constant. The layout test therefore builds each boundary out
        of the terms it is made of -- the bar's own `twoRowHeight`, the strip's own budget
        for the stage, the grid's row rule read backwards -- and calls nothing here except
        where monotonicity is the claim. The floor the editor publishes is this function's
        stage 3 with the *wrapped* bar, which is the worst case the published minimum width
        allows.

        Monotone: no stage is ever cheaper than the one above it, because each drops a term.
    */
    inline EditorPlan editorPlan (int interior, int barHeight, int gridFloor) noexcept
    {
        EditorPlan p;

        p.stage = interior >= editorCostForStage (0, barHeight, gridFloor) ? 0
                : interior >= editorCostForStage (1, barHeight, gridFloor) ? 1
                : interior >= editorCostForStage (2, barHeight, gridFloor) ? 2
                                                                          : 3;

        p.wordmark = p.stage == 0;
        p.strip    = stripHeightForStage (p.stage);
        p.wanted   = editorCostForStage (p.stage, barHeight, gridFloor);
        p.spare    = interior - p.wanted;

        return p;
    }
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
