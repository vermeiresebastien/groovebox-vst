#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <array>
#include <atomic>
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
