#include "LookAndFeel.h"

namespace gb
{

GrooveboxLookAndFeel::GrooveboxLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, Colours::background);
    setColour (juce::DocumentWindow::textColourId, Colours::text);

    setColour (juce::TextButton::buttonColourId, Colours::chip);
    setColour (juce::TextButton::buttonOnColourId, Colours::text);
    setColour (juce::TextButton::textColourOffId, Colours::text);
    setColour (juce::TextButton::textColourOnId, Colours::background);

    setColour (juce::Label::textColourId, Colours::text);
    setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::outlineWhenEditingColourId, Colours::rule);

    setColour (juce::Slider::trackColourId, Colours::text);
    setColour (juce::Slider::backgroundColourId, Colours::chip);
    setColour (juce::Slider::thumbColourId, Colours::text);
    setColour (juce::Slider::textBoxTextColourId, Colours::text);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);

    setColour (juce::TextEditor::backgroundColourId, Colours::chip);
    setColour (juce::TextEditor::textColourId, Colours::text);
    setColour (juce::TextEditor::highlightColourId, Colours::rule);
    setColour (juce::TextEditor::outlineColourId, Colours::chipBorder);
    setColour (juce::CaretComponent::caretColourId, Colours::text);

    setColour (juce::PopupMenu::backgroundColourId, Colours::background);
    setColour (juce::PopupMenu::textColourId, Colours::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Colours::chipBorder);

    setColour (juce::TooltipWindow::backgroundColourId, Colours::background);
    setColour (juce::TooltipWindow::textColourId, Colours::text);
    setColour (juce::TooltipWindow::outlineColourId, Colours::rule);
}

//==============================================================================

void GrooveboxLookAndFeel::drawButtonBackground (juce::Graphics& g,
                                                 juce::Button& button,
                                                 const juce::Colour& backgroundColour,
                                                 bool shouldDrawButtonAsHighlighted,
                                                 bool shouldDrawButtonAsDown)
{
    const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    const auto corner = juce::jmin (6.0f, bounds.getHeight() * 0.25f);

    auto fill = backgroundColour;

    if (shouldDrawButtonAsDown)
        fill = fill.brighter (0.25f);
    else if (shouldDrawButtonAsHighlighted)
        fill = fill.brighter (0.12f);

    g.setColour (fill);
    g.fillRoundedRectangle (bounds, corner);

    g.setColour (button.getToggleState() ? juce::Colours::transparentBlack : Colours::chipBorder);
    g.drawRoundedRectangle (bounds, corner, 1.0f);

    if (! button.isEnabled())
    {
        g.setColour (Colours::background.withAlpha (0.5f));
        g.fillRoundedRectangle (bounds, corner);
    }
}

void GrooveboxLookAndFeel::drawButtonText (juce::Graphics& g,
                                           juce::TextButton& button,
                                           bool,
                                           bool)
{
    g.setFont (getTextButtonFont (button, button.getHeight()));
    g.setColour (button.findColour (button.getToggleState() ? juce::TextButton::textColourOnId
                                                           : juce::TextButton::textColourOffId)
                    .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));

    g.drawFittedText (button.getButtonText(), button.getLocalBounds().reduced (4, 0),
                      juce::Justification::centred, 1, 0.9f);
}

void GrooveboxLookAndFeel::drawLinearSlider (juce::Graphics& g,
                                             int x, int y, int width, int height,
                                             float sliderPos, float minSliderPos, float maxSliderPos,
                                             juce::Slider::SliderStyle style,
                                             juce::Slider& slider)
{
    juce::ignoreUnused (minSliderPos, maxSliderPos);

    const auto isHorizontal = style == juce::Slider::LinearHorizontal
                                || style == juce::Slider::LinearBar
                                || style == juce::Slider::LinearBarVertical;

    if (! isHorizontal)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos,
                                          minSliderPos, maxSliderPos, style, slider);
        return;
    }

    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const auto trackHeight = juce::jmax (3.0f, bounds.getHeight() * 0.22f);
    const auto track = juce::Rectangle<float> (bounds.getX(),
                                               bounds.getCentreY() - trackHeight * 0.5f,                                               bounds.getWidth(),
                                               trackHeight);

    // Track background plus the filled portion up to the thumb.
    g.setColour (Colours::chip);
    g.fillRoundedRectangle (track, trackHeight * 0.5f);

    const auto filledWidth = juce::jlimit (0.0f, track.getWidth(), sliderPos - track.getX());

    if (filledWidth > 0.0f)
    {
        g.setColour (Colours::text.withAlpha (0.85f));
        g.fillRoundedRectangle (track.withWidth (filledWidth), trackHeight * 0.5f);
    }

    const auto thumbSize = juce::jmax (10.0f, bounds.getHeight() * 0.6f);
    const auto thumb = juce::Rectangle<float> (thumbSize, thumbSize)
                           .withCentre ({ sliderPos, bounds.getCentreY() });

    g.setColour (Colours::text);
    g.fillEllipse (thumb);
}

void GrooveboxLookAndFeel::drawLabel (juce::Graphics& g, juce::Label& label)
{
    g.fillAll (label.findColour (juce::Label::backgroundColourId));

    if (! label.isBeingEdited())
    {
        const auto alpha = label.isEnabled() ? 1.0f : 0.5f;
        const auto font = getLabelFont (label);

        g.setColour (label.findColour (juce::Label::textColourId).withMultipliedAlpha (alpha));
        g.setFont (font);

        auto textArea = label.getBorderSize().subtractedFrom (label.getLocalBounds());
        g.drawFittedText (label.getText(), textArea, label.getJustificationType(),
                          juce::jmax (1, (int) ((float) textArea.getHeight() / font.getHeight())),
                          label.getMinimumHorizontalScale());
    }

    if (label.isBeingEdited())
        return;

    g.setColour (label.findColour (juce::Label::outlineColourId));
    g.drawRect (label.getLocalBounds());
}

//==============================================================================

juce::Font GrooveboxLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return juce::Font (juce::FontOptions (juce::jlimit (11.0f, 15.0f, (float) buttonHeight * 0.42f)));
}

juce::Font GrooveboxLookAndFeel::getLabelFont (juce::Label& label)
{
    return label.getFont();
}

} // namespace gb
