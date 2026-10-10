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

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrooveboxLookAndFeel)
};

} // namespace gb
