#pragma once

#include "Common.h"

namespace gb
{

/**
    A dark, flat look matching the Earworm Studio drum page: no gradients, thin
    borders, white text on #292929, and the lane colours used as accents.
*/
class GrooveboxLookAndFeel : public juce::LookAndFeel_V4
{
public:
    GrooveboxLookAndFeel();
    ~GrooveboxLookAndFeel() override = default;

    void drawButtonBackground (juce::Graphics&,
                               juce::Button&,
                               const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override;

    void drawButtonText (juce::Graphics&,
                         juce::TextButton&,
                         bool shouldDrawButtonAsHighlighted,
                         bool shouldDrawButtonAsDown) override;

    void drawLinearSlider (juce::Graphics&,
                           int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle,
                           juce::Slider&) override;

    void drawLabel (juce::Graphics&, juce::Label&) override;

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getLabelFont (juce::Label&) override;

    /** How many px a caption is held in from each vertical edge of a button this wide.

        Four px is right for a 40 px "Mute" and leaves 6 px for a 14 px stepper, which is how the
        pitch row's octave buttons came to paint one picture rather than two — measured at the old
        inset, both captions inked the box and 0 of 280 px differed. The inset is
        therefore scaled with the box and capped where it always was, so no button 27 px or wider
        sees any change. */
    static int captionInset (int buttonWidth);

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrooveboxLookAndFeel)
};

} // namespace gb
