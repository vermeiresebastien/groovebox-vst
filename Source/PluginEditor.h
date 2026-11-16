#pragma once

#include "Common.h"
#include "PluginProcessor.h"
#include "StepGridComponent.h"
#include "ChannelStrip.h"
#include "TransportBar.h"

namespace gb
{

class GrooveboxLookAndFeel;

/**
    The plugin window: transport on top, the 16-step grid in the middle, and a
    strip per drum channel underneath.
*/
class GrooveboxEditor : public juce::AudioProcessorEditor
{
public:
    explicit GrooveboxEditor (GrooveboxProcessor&);
    ~GrooveboxEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /**
        What the editor spends its height on at its current size.

        Asked by `paint()` and `resized()` together, so the GROOVEBOX row cannot be drawn
        by one half and given no band by the other. Both the bar's height and the grid's
        floor come from the components that own those rules, which is what stops the editor
        paying for a budget the bar or the grid then spends differently. The layout test
        watches the components rather than calling this: a claim that asked the rule what
        it had decided would stay green on a plan that decided a stage and laid nothing out
        to match.
    */
    Layout::EditorPlan currentPlan() const noexcept;

    /** Ctrl/Cmd+Z and Ctrl/Cmd+Shift+Z (or Ctrl+Y), straight to the undo manager. */
    bool keyPressed (const juce::KeyPress&) override;

private:
    void refreshAll();

    /** Steps the undo manager one gesture either way; false when there was nothing. */
    bool undoOrRedo (bool undo);

    GrooveboxProcessor& processor;

    std::unique_ptr<GrooveboxLookAndFeel> lookAndFeel;

    TransportBar transportBar;
    StepGridComponent stepGrid;
    juce::OwnedArray<ChannelStrip> channelStrips;

    juce::Label titleLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrooveboxEditor)
};

} // namespace gb
