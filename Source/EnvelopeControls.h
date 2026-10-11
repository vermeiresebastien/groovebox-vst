#pragma once

#include "Common.h"
#include "PluginProcessor.h" // for EnvelopeStage and its display formatting

namespace gb
{

/**
    Four compact ADSR controls for one channel.

    A channel strip is only about 77 px wide, which is not enough for four usable
    sliders or knobs side by side, so each control is a single row: a letter, the
    value, and a short track. The track is dragged vertically as well as
    horizontally, which makes a control this short still precise — a horizontal
    drag across 30 px would otherwise cover the whole range in a few pixels.
*/
class EnvelopeControls : public juce::Component,
                         public juce::SettableTooltipClient
{
public:
    EnvelopeControls();

    /** Called with the stage and its new 0..1 value whenever a control moves. */
    std::function<void (EnvelopeStage, float)> onValueChange;

    /** Called when a control is reset by double-clicking it. */
    std::function<void (EnvelopeStage)> onReset;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    juce::String getTooltip() override;

    /** Sets a value without notifying, for refreshing from the processor. */
    void setValue (EnvelopeStage stage, float value);
    float getValue (EnvelopeStage stage) const;

private:
    struct Control
    {
        EnvelopeStage stage;
        const char* label;
        float value;
    };

    Control* controlFor (EnvelopeStage stage);
    const Control* controlFor (EnvelopeStage stage) const;

    /** Index of the control a point falls in, or -1. */
    int indexAt (juce::Point<int> position) const;

    juce::Rectangle<int> boundsFor (int index) const;

    std::array<Control, 4> controls
    {
        Control { EnvelopeStage::attack,  "A", 0.0f },
        Control { EnvelopeStage::decay,   "D", 0.0f },
        Control { EnvelopeStage::sustain, "S", 0.0f },
        Control { EnvelopeStage::release, "R", 0.0f },
    };

    /** Control being dragged, or -1. */
    int activeIndex = -1;

    /** Where the drag started, so the value moves with the pointer rather than
        jumping to wherever the pointer happens to be. */
    juce::Point<int> dragStart;
    float dragStartValue = 0.0f;

    static constexpr int rowHeight = 14;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EnvelopeControls)
};

} // namespace gb
