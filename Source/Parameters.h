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
