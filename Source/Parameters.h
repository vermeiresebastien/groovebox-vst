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
    inline juce::String attackId  (int channel) { return channelId ("Attack", channel); }
    inline juce::String decayId   (int channel) { return channelId ("Decay", channel); }
    inline juce::String sustainId (int channel) { return channelId ("Sustain", channel); }
    inline juce::String releaseId (int channel) { return channelId ("Release", channel); }
    inline juce::String muteId    (int channel) { return channelId ("Mute", channel); }

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

    inline constexpr float tempoMin = 20.0f;
    inline constexpr float tempoMax = 400.0f;

    /** Swing pair size in steps: 4 swings the 1/8 notes, 2 the 1/16s. */
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

            layout.add (std::move (group));
        }

        return layout;
    }
}

} // namespace gb
