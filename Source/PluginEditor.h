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
