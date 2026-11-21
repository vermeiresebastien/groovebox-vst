#include "EnvelopeControls.h"

namespace gb
{

namespace
{
    /** How far the pointer must travel to cover the whole range. Vertical, so a
        control this short can still be set precisely. */
    constexpr int dragRangePixels = 140;

    /** The letter column and the gap before the track. The value column is next. */
    constexpr int letterWidthPx = 10;
    constexpr int valueGapPx = 2;

    /** Below this the track is not drawn at all, so it is also the smallest width it can be given. */
    constexpr int minimumTrackWidthPx = 10;

    /** How many px a reading occupies in the font these rows are drawn with. */
    int drawnWidth (const juce::String& text)
    {
        return (int) std::ceil (juce::TextLayout::getStringWidth (EnvelopeControls::valueFont(), text));
    }

    /** The last reading a time stage takes through its "N ms" branch.

        Bisected from the seconds law rather than sampled: the window that prints the branch's
        ceiling is the set of values whose seconds land in [0.9995, 1.0), about 1.3e-4 of the
        travel on the four-second stage, and a fixed step walks past it. The layout test walks it
        with a 5e-6 sweep, which is the other half of the same fact. */
    juce::String msBranchCeiling (EnvelopeStage stage)
    {
        float below = 0.0f, above = 1.0f;

        for (int i = 0; i < 60; ++i)
        {
            const auto middle = (below + above) * 0.5f;

            if (GrooveboxProcessor::envelopeSecondsFor (middle, stage) < 1.0)
                below = middle;
            else
                above = middle;
        }

        return GrooveboxProcessor::formatEnvelopeValue (below, stage);
    }
}

juce::Font EnvelopeControls::valueFont()
{
    return juce::Font (juce::FontOptions (10.0f));
}

int EnvelopeControls::valueColumnWidth (EnvelopeStage stage)
{
    // The column is whatever the widest reading needs, so a value is never drawn into a box
    // shorter than its own text. Two branch ceilings bound a time stage -- the top of its range
    // through the seconds form, and the ms form's own ceiling -- and sustain has the one percent
    // reading. Computed once: it is a property of the font and the law, not of the panel.
    static const auto widths = []
    {
        std::array<int, 4> measured {};

        for (int i = 0; i < 4; ++i)
        {
            const auto at = static_cast<EnvelopeStage> (i);
            auto widest = drawnWidth (GrooveboxProcessor::formatEnvelopeValue (1.0f, at));

            if (at != EnvelopeStage::sustain)
                widest = juce::jmax (widest, drawnWidth (msBranchCeiling (at)));

            measured[(size_t) i] = widest;
        }

        return measured;
    }();

    return widths[(int) stage];
}

int EnvelopeControls::reservedWidth (EnvelopeStage stage)
{
    return letterWidthPx + valueColumnWidth (stage) + valueGapPx;
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
        const auto clamped = juce::jlimit (0.0f, 1.0f, value);

        // Repaint only on an actual change: the channel strip re-reads these every
        // timer tick so the panel can follow host automation, and repainting four
        // rows twelve times over each tick when nothing moved is wasted work.
        if (std::abs (clamped - control->value) < 1.0e-4f)
            return;

        control->value = clamped;
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
    const auto font = valueFont();

    for (size_t i = 0; i < controls.size(); ++i)
    {
        const auto& control = controls[i];
        auto row = boundsFor ((int) i);
        const auto trackArea = row.removeFromRight (juce::jmax (minimumTrackWidthPx,
                                                                row.getWidth() - reservedWidth (control.stage)));

        // Letter.
        g.setColour (Colours::muted);
        g.setFont (font);
        g.drawText (control.label, row.removeFromLeft (letterWidthPx), juce::Justification::centredLeft, false);

        // Value, formatted the way the parameter actually behaves.
        g.setColour (Colours::text);
        g.drawText (GrooveboxProcessor::formatEnvelopeValue (control.value, control.stage),
                    row.removeFromLeft (valueColumnWidth (control.stage)),
                    juce::Justification::centredLeft, false);

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
