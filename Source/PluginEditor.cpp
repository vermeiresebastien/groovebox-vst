#include "PluginEditor.h"
#include "LookAndFeel.h"

namespace gb
{

namespace
{
    constexpr int editorWidth = 1240;
    constexpr int editorHeight = 830;

    constexpr int margin = 18;
    constexpr int titleHeight = 34;
    constexpr int transportHeight = 60;
    constexpr int gridHeight = 400;
    // Tall enough for every control in a channel strip: the sound and clear
    // buttons, the pitch row, the level fader, the four ADSR rows and mute. A
    // shorter strip still lays out, but the bottom control gets clipped.
    constexpr int stripHeight = 254;
    constexpr int gap = 10;
}

GrooveboxEditor::GrooveboxEditor (GrooveboxProcessor& p)
    : juce::AudioProcessorEditor (&p),
      processor (p),
      transportBar (p),
      stepGrid (p)
{
    lookAndFeel = std::make_unique<GrooveboxLookAndFeel>();
    setLookAndFeel (lookAndFeel.get());

    titleLabel.setText ("GROOVEBOX", juce::dontSendNotification);
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    titleLabel.setColour (juce::Label::textColourId, Colours::text);
    titleLabel.setFont (juce::Font (juce::FontOptions (22.0f)).boldened());
    addAndMakeVisible (titleLabel);

    addAndMakeVisible (stepGrid);
    addAndMakeVisible (transportBar);

    for (int channel = 0; channel < Layout::numChannels; ++channel)
        addAndMakeVisible (channelStrips.add (new ChannelStrip (p, channel)));

    // The grid owns the pattern, so let it know when something else changed it.
    processor.onStateChanged = [this]
    {
        refreshAll();
    };

    // The velocity toggle lives in the transport bar but changes how the grid
    // behaves, so the editor joins the two.
    transportBar.onVelocityModeChanged = [this] (bool editVelocity)
    {
        stepGrid.setVelocityMode (editVelocity);
    };

    setSize (editorWidth, editorHeight);
    setResizable (true, true);
    setResizeLimits (editorWidth, editorHeight, 2200, 1400);
}

GrooveboxEditor::~GrooveboxEditor()
{
    processor.onStateChanged = nullptr;
    transportBar.onVelocityModeChanged = nullptr;
    setLookAndFeel (nullptr);
}

void GrooveboxEditor::refreshAll()
{
    stepGrid.refresh();

    for (auto* strip : channelStrips)
        strip->refresh();

    transportBar.refresh();
    repaint();
}

void GrooveboxEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);

    // The rule under the title, matching the web app's header divider.
    g.setColour (Colours::rule);
    g.fillRect (margin, titleHeight + 8, getWidth() - margin * 2, 1);

    g.setColour (Colours::muted);
    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    g.drawText ("Drop a WAV or AIFF on any channel row to load it",
                margin, titleHeight - 2, getWidth() - margin * 2, 16,
                juce::Justification::centredRight);
}

void GrooveboxEditor::resized()
{
    auto area = getLocalBounds().reduced (margin);

    auto titleRow = area.removeFromTop (titleHeight);
    titleLabel.setBounds (titleRow);
    area.removeFromTop (4);

    transportBar.setBounds (area.removeFromTop (transportHeight));
    area.removeFromTop (gap);

    // The channel strips take their full height: the clamp that used to be here
    // (a third of the remaining area) silently squeezed the bottom control out of
    // the strip whenever the window was a little short.
    auto stripRow = area.removeFromBottom (juce::jmin (stripHeight, juce::jmax (0, area.getHeight() - 120)));
    const auto stripWidth = (stripRow.getWidth() - gap * (Layout::numChannels - 1)) / Layout::numChannels;

    for (int channel = 0; channel < channelStrips.size(); ++channel)
    {
        auto bounds = stripRow.removeFromLeft (channel == channelStrips.size() - 1 ? stripRow.getWidth()
                                                                                  : stripWidth);
        channelStrips[channel]->setBounds (bounds);

        if (channel < channelStrips.size() - 1)
            stripRow.removeFromLeft (gap);
    }

    area.removeFromBottom (gap);
    stepGrid.setBounds (area);
}

} // namespace gb
