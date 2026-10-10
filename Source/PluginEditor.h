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

private:
    void refreshAll();

    GrooveboxProcessor& processor;

    std::unique_ptr<GrooveboxLookAndFeel> lookAndFeel;

    TransportBar transportBar;
    StepGridComponent stepGrid;
    juce::OwnedArray<ChannelStrip> channelStrips;

    juce::Label titleLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrooveboxEditor)
};

} // namespace gb
