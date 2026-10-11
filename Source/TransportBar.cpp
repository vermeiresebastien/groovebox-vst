#include "TransportBar.h"

namespace gb
{

namespace
{
    /** Small helper so every caption uses the same (non-deprecated) font call. */
    juce::Font captionFont()
    {
        return juce::Font (juce::FontOptions (12.0f));
    }
}

TransportBar::TransportBar (GrooveboxProcessor& processorToUse)
    : processor (processorToUse)
{
    addAndMakeVisible (playButton);

    // Caption for the plugin build: the host owns transport there. The note range
    // tells a player which notes the channels answer to.
    hostCaption.setText ("Host transport", juce::dontSendNotification);
    hostCaption.setJustificationType (juce::Justification::centred);
    hostCaption.setColour (juce::Label::textColourId, Colours::muted);
    hostCaption.setFont (captionFont());
    addAndMakeVisible (hostCaption);

    midiHintLabel.setJustificationType (juce::Justification::centredLeft);
    midiHintLabel.setColour (juce::Label::textColourId, Colours::muted);
    midiHintLabel.setFont (captionFont());
    midiHintLabel.setTooltip ("Incoming notes play the channels: the first channel answers C1, "
                              "and the rest follow upwards.");
    midiHintLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (midiHintLabel);

    tempoDownButton.setTooltip ("Decrease tempo by 1 BPM");
    tempoUpButton.setTooltip ("Increase tempo by 1 BPM");
    addAndMakeVisible (tempoDownButton);
    addAndMakeVisible (tempoUpButton);

    tempoLabel.setJustificationType (juce::Justification::centred);
    tempoLabel.setColour (juce::Label::textColourId, Colours::text);
    tempoLabel.setFont (juce::Font (juce::FontOptions (20.0f)));
    tempoLabel.setMinimumHorizontalScale (0.6f);
    addAndMakeVisible (tempoLabel);

    swingSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    swingSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    swingSlider.setRange (0.0, 1.0, 0.01);
    swingSlider.setDoubleClickReturnValue (true, 0.0);
    swingSlider.setTooltip ("Swing amount");
    addAndMakeVisible (swingSlider);

    swingValueLabel.setJustificationType (juce::Justification::centredLeft);
    swingValueLabel.setColour (juce::Label::textColourId, Colours::text);
    swingValueLabel.setFont (captionFont());
    addAndMakeVisible (swingValueLabel);

    // The radio group makes the two grid buttons mutually exclusive; the "on"
    // colours invert the button so the active grid is obvious.
    for (auto* button : { &swingEighthButton, &swingSixteenthButton })
    {
        button->setClickingTogglesState (true);
        button->setRadioGroupId (swingRadioGroupId);
        button->setColour (juce::TextButton::buttonOnColourId, Colours::text);
        button->setColour (juce::TextButton::textColourOnId, Colours::background);
        addAndMakeVisible (*button);
    }

    swingEighthButton.setTooltip ("Swing the 1/8 note grid");
    swingSixteenthButton.setTooltip ("Swing the 1/16 note grid");

    addAndMakeVisible (clearPatternButton);

    // Velocity editing: a toggle, so it stays lit while the grid is showing bars.
    velocityButton.setClickingTogglesState (true);
    velocityButton.setTooltip ("Edit how hard each step hits: drag up and down inside a step");
    velocityButton.setColour (juce::TextButton::buttonOnColourId, Colours::text);
    velocityButton.setColour (juce::TextButton::textColourOnId, Colours::background);
    addAndMakeVisible (velocityButton);

    resetVelocitiesButton.setTooltip ("Put every step back to full velocity");
    addAndMakeVisible (resetVelocitiesButton);

    // Values first, callbacks afterwards: refresh() therefore cannot trigger a
    // write back into the processor while the bar is being built.
    refresh();

    playButton.onClick        = [this] { handlePlayClicked(); };
    tempoDownButton.onClick   = [this] { handleTempoMinus(); };
    tempoUpButton.onClick     = [this] { handleTempoPlus(); };
    swingEighthButton.onClick = [this]
    {
        processor.setSwingGrid (4);
        refresh();
    };
    swingSixteenthButton.onClick = [this]
    {
        processor.setSwingGrid (2);
        refresh();
    };
    clearPatternButton.onClick = [this] { handleClearPatternClicked(); };
    velocityButton.onClick    = [this] { handleVelocityClicked(); };
    resetVelocitiesButton.onClick = [this] { handleResetVelocitiesClicked(); };

    // Written straight through, for the same reason as the channel faders: an
    // earlier version skipped the write while a drag was in progress and relied
    // on the release to commit it, which never happened because the 200 ms
    // refresh repopulated the slider from the processor and pulled the thumb
    // back. Swing then appeared to do nothing at all.
    swingSlider.onValueChange = [this]
    {
        processor.setSwing ((float) swingSlider.getValue());
    };

    // Host tempo and transport can change at any time, so follow them.
    startTimer (200);
}

TransportBar::~TransportBar()
{
    stopTimer();
}

//==============================================================================
void TransportBar::refresh()
{
    const auto hostDriven = processor.isHostDriven();

    // In a plugin the DAW owns transport, so the button is disabled and a
    // caption explains why. Standalone builds get the real transport button.
    playButton.setEnabled (! hostDriven);
    playButton.setButtonText (processor.isPlaying() ? "Stop" : "Play");
    hostCaption.setVisible (hostDriven);

    tempoLabel.setText (juce::String (juce::roundToInt (processor.getCurrentTempo())), juce::dontSendNotification);
    tempoDownButton.setEnabled (! hostDriven);
    tempoUpButton.setEnabled (! hostDriven);

    swingSlider.setValue ((double) processor.getSwing(), juce::dontSendNotification);

    swingValueLabel.setText (juce::String (juce::roundToInt (processor.getSwing() * 100.0f)) + "%",
                             juce::dontSendNotification);

    const auto grid = processor.getSwingGrid();
    swingEighthButton.setToggleState (grid == 4, juce::dontSendNotification);
    swingSixteenthButton.setToggleState (grid == 2, juce::dontSendNotification);

    // Name the note range the channels answer to, using this build's mapping.
    const auto first = GrooveboxProcessor::getMidiNoteNameForChannel (0);
    const auto last = GrooveboxProcessor::getMidiNoteNameForChannel (Layout::numChannels - 1);

    midiHintLabel.setText ("MIDI notes " + first + "-" + last + " play the channels",
                           juce::dontSendNotification);
}

void TransportBar::resized()
{
    auto area = getLocalBounds().reduced (8, 6);
    area.removeFromBottom (2); // leave room for the separator rule

    playButton.setBounds (area.removeFromLeft (76).reduced (0, 2));
    area.removeFromLeft (6);
    hostCaption.setBounds (area.removeFromLeft (92).reduced (0, 2));
    area.removeFromLeft (8);

    auto tempoArea = area.removeFromLeft (124);
    tempoCaptionArea = tempoArea.removeFromTop (12); // drawn in paint()

    auto tempoRow = tempoArea.withSizeKeepingCentre (tempoArea.getWidth(), 22);
    tempoDownButton.setBounds (tempoRow.removeFromLeft (22));
    tempoUpButton.setBounds (tempoRow.removeFromRight (22));
    tempoLabel.setBounds (tempoRow);
    area.removeFromLeft (8);

    auto swingArea = area.removeFromLeft (300);
    swingCaptionArea = swingArea.removeFromTop (12); // drawn in paint()
    swingArea.removeFromTop (2);
    swingSlider.setBounds (swingArea.removeFromLeft (140).withSizeKeepingCentre (140, 22));
    swingArea.removeFromLeft (6);
    swingValueLabel.setBounds (swingArea.removeFromLeft (44).withSizeKeepingCentre (44, 18));
    swingArea.removeFromLeft (4);
    swingEighthButton.setBounds (swingArea.removeFromLeft (34).withSizeKeepingCentre (34, 22));
    swingArea.removeFromLeft (2);
    swingSixteenthButton.setBounds (swingArea.removeFromLeft (40).withSizeKeepingCentre (40, 22));
    area.removeFromLeft (8);

    clearPatternButton.setBounds (area.removeFromLeft (108).withSizeKeepingCentre (108, 24));
    area.removeFromLeft (8);

    velocityButton.setBounds (area.removeFromLeft (76).withSizeKeepingCentre (76, 24));
    area.removeFromLeft (4);
    resetVelocitiesButton.setBounds (area.removeFromLeft (52).withSizeKeepingCentre (52, 22));
    area.removeFromLeft (10);

    // Whatever horizontal space is left names the incoming-note mapping.
    if (area.getWidth() > 150)
        midiHintLabel.setBounds (area.withSizeKeepingCentre (area.getWidth(), 18));
    else
        midiHintLabel.setBounds ({});
}

//==============================================================================
void TransportBar::paint (juce::Graphics& g)
{
    paintCaption (g, "Tempo", tempoCaptionArea);
    paintCaption (g, "Swing", swingCaptionArea);

    // Subtle rule along the bottom edge.
    g.setColour (Colours::rule);
    g.fillRect (0, getHeight() - 2, getWidth(), 2);
}

void TransportBar::paintCaption (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area) const
{
    if (area.isEmpty())
        return;

    g.setColour (Colours::muted);
    g.setFont (captionFont());
    g.drawText (text, area, juce::Justification::centredLeft, false);
}

//==============================================================================
void TransportBar::timerCallback()
{
    refresh();
}

//==============================================================================
void TransportBar::handlePlayClicked()
{
    // The button text (`Play` / `Stop`) is derived from isPlaying() in refresh().
    processor.setPlaying (! processor.isPlaying());
    refresh();
}

void TransportBar::handleTempoMinus()
{
    processor.setManualTempo (processor.getCurrentTempo() - 1.0);
    refresh();
}

void TransportBar::handleTempoPlus()
{
    processor.setManualTempo (processor.getCurrentTempo() + 1.0);
    refresh();
}

void TransportBar::handleClearPatternClicked()
{
    processor.beginTransaction ("Clear pattern");
    processor.clearPattern();

    if (processor.onStateChanged != nullptr)
        processor.onStateChanged();

    refresh();
}

void TransportBar::handleVelocityClicked()
{
    if (onVelocityModeChanged != nullptr)
        onVelocityModeChanged (velocityButton.getToggleState());
}

void TransportBar::handleResetVelocitiesClicked()
{
    processor.resetVelocities();

    if (processor.onStateChanged != nullptr)
        processor.onStateChanged();

    refresh();
}

} // namespace gb
