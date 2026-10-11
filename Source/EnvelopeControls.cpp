#include "EnvelopeControls.h"

namespace gb
{

namespace
{
    /** How far the pointer must travel to cover the whole range. Vertical, so a
        control this short can still be set precisely. */
    constexpr int dragRangePixels = 140;
}

EnvelopeControls::EnvelopeControls()
{
    setTooltip ("A / D / S / R for this channel. Drag up or down to change; "
                "double-click a row to reset it.");
}

juce::String EnvelopeControls::getTooltip()
{
    return "A / D / S / R for this channel. Drag up or down to change a value; "
           "double-click a row to reset it. Sustain 0 makes the channel a one-shot.";
}

//==============================================================================

EnvelopeControls::Control* EnvelopeControls::controlFor (EnvelopeStage stage)
{
    for (auto& control : controls)
        if (control.stage == stage)
            return &control;

    return nullptr;
}

const EnvelopeControls::Control* EnvelopeControls::controlFor (EnvelopeStage stage) const
{
    for (const auto& control : controls)
        if (control.stage == stage)
            return &control;

    return nullptr;
}

void EnvelopeControls::setValue (EnvelopeStage stage, float value)
{
    if (auto* control = controlFor (stage))
    {
        control->value = juce::jlimit (0.0f, 1.0f, value);
        repaint();
    }
}

float EnvelopeControls::getValue (EnvelopeStage stage) const
{
    if (const auto* control = controlFor (stage))
        return control->value;

    return 0.0f;
}

juce::Rectangle<int> EnvelopeControls::boundsFor (int index) const
{
    const auto height = juce::jmax (1, getHeight() / (int) controls.size());
    return { 0, index * height, getWidth(), height };
}

int EnvelopeControls::indexAt (juce::Point<int> position) const
{
    const auto index = position.y / juce::jmax (1, getHeight() / (int) controls.size());

    return juce::isPositiveAndBelow (index, (int) controls.size()) ? index : -1;
}

//==============================================================================
// Drag handling

void EnvelopeControls::mouseDown (const juce::MouseEvent& e)
{
    activeIndex = indexAt (e.getPosition());

    if (activeIndex < 0)
        return;

    dragStart = e.getPosition();
    dragStartValue = controls[(size_t) activeIndex].value;
}

void EnvelopeControls::mouseDrag (const juce::MouseEvent& e)
{
    if (activeIndex < 0 || activeIndex >= (int) controls.size())
        return;

    // Up increases, and horizontal movement counts too so the control responds
    // whichever way the pointer is taken.
    const auto delta = (dragStart.y - e.getPosition().y)
                         + (e.getPosition().x - dragStart.x);

    const auto value = juce::jlimit (0.0f, 1.0f,
                                     dragStartValue + (float) delta / (float) dragRangePixels);

    auto& control = controls[(size_t) activeIndex];

    if (std::abs (value - control.value) < 1.0e-4f)
        return;

    control.value = value;
    repaint();

    if (onValueChange != nullptr)
        onValueChange (control.stage, value);
}

void EnvelopeControls::mouseUp (const juce::MouseEvent&)
{
    activeIndex = -1;
}

void EnvelopeControls::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto index = indexAt (e.getPosition());

    if (index < 0)
        return;

    activeIndex = -1;

    if (onReset != nullptr)
        onReset (controls[(size_t) index].stage);
}

//==============================================================================

void EnvelopeControls::paint (juce::Graphics& g)
{
    const auto font = juce::Font (juce::FontOptions (10.0f));

    for (size_t i = 0; i < controls.size(); ++i)
    {
        const auto& control = controls[i];
        auto row = boundsFor ((int) i);
        const auto trackArea = row.removeFromRight (juce::jmax (10, row.getWidth() - 38));

        // Letter.
        g.setColour (Colours::muted);
        g.setFont (font);
        g.drawText (control.label, row.removeFromLeft (10), juce::Justification::centredLeft, false);

        // Value, formatted the way the parameter actually behaves.
        g.setColour (Colours::text);
        g.drawText (GrooveboxProcessor::formatEnvelopeValue (control.value, control.stage),
                    row.removeFromLeft (26), juce::Justification::centredLeft, false);

        // Track and fill. The fill starts at the left, so a glance shows how open
        // the control is.
        const auto track = trackArea.reduced (0, 4).toFloat();

        if (track.getWidth() < 4.0f)
            continue;

        g.setColour (Colours::chip);
        g.fillRoundedRectangle (track, 2.0f);

        const auto filled = juce::jmax (2.0f, track.getWidth() * control.value);

        g.setColour (juce::Colour (0xff56beff).withAlpha (0.75f));
        g.fillRoundedRectangle (track.withWidth (filled), 2.0f);
    }
}

} // namespace gb
