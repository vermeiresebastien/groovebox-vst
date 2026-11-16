#pragma once

#include "Common.h"

namespace gb
{

/**
    The plugin's automatable parameters.

    Everything continuous lives here rather than in the hand-rolled ValueTree it
    replaced, which is what makes it reachable from a host: a lane's level, pitch,
    envelope and mute can now be drawn as automation instead of only being moved by
    hand. The pattern stays out of it — 192 cells as 192 parameters would swamp a
    host's automation list and is not what anyone automates.

    Ranges match the controls that already exist, so migrating changes what a host
    can reach and not what the panel does.
*/
namespace Params
{
    /** Per-channel ids. Built rather than written out so the twelve lanes cannot
        drift apart from one another. */
    inline juce::String channelId (juce::StringRef suffix, int channel)
    {
        return "ch" + juce::String (channel) + suffix;
    }

    inline juce::String levelId   (int channel) { return channelId ("Level", channel); }
    inline juce::String pitchId   (int channel) { return channelId ("Pitch", channel); }
    inline juce::String pitchDropId (int channel) { return channelId ("PitchDrop", channel); }
    inline juce::String attackId  (int channel) { return channelId ("Attack", channel); }
    inline juce::String decayId   (int channel) { return channelId ("Decay", channel); }
    inline juce::String sustainId (int channel) { return channelId ("Sustain", channel); }
    inline juce::String releaseId (int channel) { return channelId ("Release", channel); }
    inline juce::String muteId    (int channel) { return channelId ("Mute", channel); }
    inline juce::String chokeId   (int channel) { return channelId ("Choke", channel); }

    /** Which field of a lane an id names.

        A lane id carries both halves of the address on its own — "ch11Sustain"
        says lane eleven and the sustain field — and the two are only ever
        meaningful together. Splitting the string apart to find that out is what
        `parameterChanged` used to do, on the host's automation thread, so the
        answer is built here once instead and read as a table.

        Adding a control to `createLayout` means adding a row to `laneTargets`
        below, a case for it in `parameterChanged` and one in
        `syncStateFromParameters`; test [21] in the smoke suite writes every id
        the table holds and checks each one lands on the field of the lane it
        names, which is what catches it if one of the three is forgotten.
    */
    enum class LaneControl
    {
        level, pitch, pitchDrop, attack, decay, sustain, release, mute, choke
    };

    /** The last control a lane has. Adding one to the enum above means this is
        the line that says how many there are. */
    inline constexpr int numLaneControls = static_cast<int> (LaneControl::choke) + 1;

    /** One lane parameter, and where its value belongs. */
    struct LaneTarget
    {
        juce::String id;
        int channel = 0;
        LaneControl control = LaneControl::level;
    };

    inline constexpr int numLaneTargets = Layout::numChannels * numLaneControls;

    /**
        Every lane id, paired with the lane and field it names.

        Built from the id functions above rather than from a spelling of them, so
        the table and the parameters cannot drift apart: the row that says
        `levelId (channel)` is the parameter called Level is the same expression
        `createLayout` uses.

        A function-local static because there is exactly one table for the process
        and it is read from the audio thread: C++11 guarantees the initialiser
        runs once, and the constructor resolves it on the message thread so no
        host automation can ever be the call that builds it. After that the table
        is read-only, which is what makes reading it from two threads safe.
    */
    inline const std::array<LaneTarget, (size_t) numLaneTargets>& laneTargets()
    {
        static const std::array<LaneTarget, (size_t) numLaneTargets> targets = []
        {
            std::array<LaneTarget, (size_t) numLaneTargets> table {};
            int row = 0;

            const auto add = [&table, &row] (juce::String id, int channel, LaneControl control)
            {
                table[(size_t) (row++)] = { std::move (id), channel, control };
            };

            for (int channel = 0; channel < Layout::numChannels; ++channel)
            {
                add (levelId     (channel), channel, LaneControl::level);
                add (pitchId     (channel), channel, LaneControl::pitch);
                add (pitchDropId (channel), channel, LaneControl::pitchDrop);
                add (attackId    (channel), channel, LaneControl::attack);
                add (decayId     (channel), channel, LaneControl::decay);
                add (sustainId   (channel), channel, LaneControl::sustain);
                add (releaseId   (channel), channel, LaneControl::release);
                add (muteId      (channel), channel, LaneControl::mute);
                add (chokeId     (channel), channel, LaneControl::choke);
            }

            return table;
        }();

        return targets;
    }

    inline constexpr const char* masterGainId = "masterGain";
    inline constexpr const char* swingId      = "swing";
    inline constexpr const char* swingGridId  = "swingGrid";
    inline constexpr const char* tempoId      = "tempo";
    inline constexpr const char* mutateId     = "mutate";
    inline constexpr const char* humanizeId   = "humanize";
    inline constexpr const char* seedId       = "seed";

    /**
        The parameters that belong to the machine rather than to a lane.

        `createLayout` spells these one `add` call at a time, so this table exists to
        be counted against it. Test [21] writes every id here and every id in
        `laneTargets()`, then checks the host is offered exactly that many: a control
        added to the layout and left out of the tables shows up as a count that
        disagrees, which is the half the per-id writes cannot see. The size is written
        into the type for the same reason `numLaneControls` is — forgetting to extend
        it when a parameter joins is a compile error, not a silent stale number.
    */
    inline const std::array<const char*, 7>& globalIds()
    {
        static const std::array<const char*, 7> ids
            { masterGainId, swingId, swingGridId, tempoId, mutateId, humanizeId, seedId };

        return ids;
    }

    /** Channel level, linear. The panel's fader tops out at 1.5, so that is the
        range; the engine still clamps to 2.0 for anything that reaches it another
        way. */
    inline constexpr float levelMin = 0.0f;
    inline constexpr float levelMax = 1.5f;

    inline constexpr int pitchMin = -36;
    inline constexpr int pitchMax = 12;

    /**
        How far a hit starts above its own note, in whole semitones.

        Whole, because the number has to be readable on a 44 px track and because
        the thing being asked for is a shape, not a fine tuning — and an int is
        what a host can type and snap to something meaningful when a drop is drawn
        as automation. See `Layout::maxPitchDropSemitones`.
    */
    inline constexpr int pitchDropMin = 0;
    inline constexpr int pitchDropMax = (int) Layout::maxPitchDropSemitones;

    inline constexpr float tempoMin = 20.0f;
    inline constexpr float tempoMax = 400.0f;

    /**
        How much of the grid's per-step probabilities the machine may use.

        Zero is "play what is drawn", and it is the default: the pattern is the one
        the user painted until they say otherwise. One hands every cell the number it
        carries. It can only take hits away — a closed step never comes back, and a
        cell at 100% is never dropped — so opening this never adds a hit that the grid
        does not already claim.
    */
    inline constexpr float mutateMin = 0.0f;
    inline constexpr float mutateMax = 1.0f;
    inline constexpr float mutateDefault = 0.0f;

    /**
        How much of the grid's timing slack the machine may use.

        The same shape as `mutate`, and for the same reason: it is an amount of
        variation rather than an amount of time, because the time it means is a
        fraction of a step and therefore already follows the tempo. Zero is "play the
        grid where it is drawn", which is both the default and the whole of the reason
        a session saved before this parameter existed comes back exactly as it was —
        there is no value that means "the old behaviour" because the old behaviour is
        the value zero.

        The top of the travel is the engine's own limit: `Layout::maxHumanizeLateSteps`
        of a step, which is half the closest two onsets this machine can make. So a
        host that automates this to 1.0 cannot reorder the pattern, only loosen it.
    */
    inline constexpr float humanizeMin = 0.0f;
    inline constexpr float humanizeMax = 1.0f;
    inline constexpr float humanizeDefault = 0.0f;

    /**
        Which sequence of rolls a session is, as a number the user can write down.

        A parameter rather than a hidden field because the point of a take is that it
        can be named: two sessions with the same seed and the same grid drop the same
        hits, so an A/B of two bounce renders is an A/B. Automatable like everything
        else here, which a host can use to reshuffle a pattern over time — a seed ramp
        is a re-roll every time it crosses an integer, so it is a special effect and
        not a setting.
    */
    inline constexpr int seedMin = 0;
    inline constexpr int seedMax = 999999;
    inline constexpr int seedDefault = 0;

    /**
        Swing pair size in steps: 4 swings the 1/8 notes, 2 the 1/16s.

        The shape of an int parameter makes the range 2, 3, 4 with no way to skip
        the middle, and a 3 is not a swing grid the bar can repeat — see
        `Layout::usableSwingGrid`, which is what both the engine and the panel read
        this through. So the parameter takes 3 from a host and the plugin answers it
        as 4, with the 1/8 button lit to say which it is.
    */
    inline constexpr int swingGridMin = 2;
    inline constexpr int swingGridMax = 4;

    /** Deliberately not called `Layout`: inside this namespace that would shadow
        `gb::Layout`, which is where the channel count and the trim range live. */
    using ParamLayout = juce::AudioProcessorValueTreeState::ParameterLayout;

    /** The whole parameter set, grouped so a host lists the lanes sensibly. */
    inline ParamLayout createLayout()
    {
        auto layout = ParamLayout();

        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { masterGainId, 1 }, "Master Trim",
            Layout::minMasterGainDb, Layout::maxMasterGainDb, Layout::defaultMasterGainDb));

        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { swingId, 1 }, "Swing", 0.0f, 1.0f, 0.0f));

        layout.add (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { swingGridId, 1 }, "Swing Grid", swingGridMin, swingGridMax, 4));

        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { tempoId, 1 }, "Tempo", tempoMin, tempoMax, 120.0f));

        // Next to Swing, because it is the other thing that decides how much of what
        // is written down actually happens.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { mutateId, 1 }, "Mutate", mutateMin, mutateMax, mutateDefault));

        // Immediately after Mutate, because the two are the same decision taken twice:
        // one says how many of the drawn hits happen, this one says when the ones that
        // happen land. A host listing parameters sees them as neighbours, which is how
        // the panel sees them too. Inserting here rather than appending moves the index
        // of every parameter after it, and that is safe in this format because the VST3
        // id a host records automation under is a hash of the id string, not its row —
        // see `generateVSTParamIDForParam` in the JUCE wrapper.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { humanizeId, 1 }, "Humanize",
            humanizeMin, humanizeMax, humanizeDefault));

        layout.add (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { seedId, 1 }, "Seed", seedMin, seedMax, seedDefault));

        for (int channel = 0; channel < Layout::numChannels; ++channel)
        {
            auto group = std::make_unique<juce::AudioProcessorParameterGroup> (
                "channel" + juce::String (channel),
                "Channel " + juce::String (channel + 1), "|");

            group->addChild (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { levelId (channel), 1 }, "Level",
                levelMin, levelMax, 1.0f));

            group->addChild (std::make_unique<juce::AudioParameterInt> (
                juce::ParameterID { pitchId (channel), 1 }, "Pitch",
                pitchMin, pitchMax, 0));

            // Sits next to Pitch because that is what it modifies: the note the
            // lane is tuned to is where the hit lands, and this says how far above
            // it the hit began.
            group->addChild (std::make_unique<juce::AudioParameterInt> (
                juce::ParameterID { pitchDropId (channel), 1 }, "Pitch Drop",
                pitchDropMin, pitchDropMax, 0));

            group->addChild (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { attackId (channel), 1 }, "Attack",
                0.0f, 1.0f, Layout::defaultAttack));

            group->addChild (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { decayId (channel), 1 }, "Decay",
                0.0f, 1.0f, Layout::defaultDecay));

            group->addChild (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { sustainId (channel), 1 }, "Sustain",
                0.0f, 1.0f, Layout::defaultSustain));

            group->addChild (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { releaseId (channel), 1 }, "Release",
                0.0f, 1.0f, Layout::defaultRelease));

            group->addChild (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { muteId (channel), 1 }, "Mute", false));

            // Zero is not a fourth group: it is the lane being in none of them, so
            // it cuts nothing and is cut by nothing.
            group->addChild (std::make_unique<juce::AudioParameterInt> (
                juce::ParameterID { chokeId (channel), 1 }, "Choke Group",
                0, Layout::numChokeGroups, 0));

            layout.add (std::move (group));
        }

        return layout;
    }
}

} // namespace gb
