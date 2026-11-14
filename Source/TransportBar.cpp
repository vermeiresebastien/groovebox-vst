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

    /** Where 0 dBFS falls on the meter's scale, so the fill goes hot from there. */
    constexpr float unityFraction = (0.0f - MasterMeter::floorDb)
                                    / (MasterMeter::ceilingDb - MasterMeter::floorDb);
}

//==============================================================================
MasterMeter::MasterMeter (GrooveboxProcessor& processorToUse)
    : processor (processorToUse)
{
    setTooltip ("Master output level. Click to clear the clip indicator.");
    startTimerHz (30);
}

MasterMeter::~MasterMeter()
{
    stopTimer();
}

void MasterMeter::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    const auto gap = 3.0f;
    const auto barHeight = (area.getHeight() - gap) * 0.5f;

    for (int channel = 0; channel < MasterBus::numMeterChannels; ++channel)
    {
        auto bar = area.withHeight (barHeight)
                       .withY (area.getY() + (float) channel * (barHeight + gap));

        g.setColour (Colours::cellOff);
        g.fillRect (bar);

        const auto peak = processor.getMasterPeak (channel);
        const auto db = juce::Decibels::gainToDecibels (peak, floorDb);
        const auto fraction = juce::jlimit (0.0f, 1.0f, (db - floorDb) / (ceilingDb - floorDb));

        auto inner = bar.reduced (1.0f);

        if (fraction > 0.0f)
        {
            const auto fill = fraction >= 1.0f            ? Colours::meterOver
                              : fraction >= unityFraction ? Colours::meterHot
                                                          : Colours::meterSafe;

            g.setColour (fill);
            g.fillRect (inner.withWidth (inner.getWidth() * fraction));
        }

        // The latch is drawn over the fill rather than instead of it, so a channel
        // that clipped still shows how loud the rest of the block was.
        if (processor.isMasterClipping (channel))
        {
            g.setColour (Colours::meterOver);
            g.fillRect (inner.removeFromRight (5.0f));
        }

        g.setColour (Colours::chipBorder);
        g.drawRect (bar, 1.0f);
    }
}

void MasterMeter::mouseDown (const juce::MouseEvent& event)
{
    juce::ignoreUnused (event);

    processor.clearMasterClipLatch();
    repaint();
}

void MasterMeter::timerCallback()
{
    repaint();
}

//==============================================================================
TransportBar::TransportBar (GrooveboxProcessor& processorToUse)
    : processor (processorToUse),
      masterMeter (processorToUse)
{
    addAndMakeVisible (playButton);

    // Caption for the plugin build: the host owns transport there.
    hostCaption.setText ("Host transport", juce::dontSendNotification);
    hostCaption.setJustificationType (juce::Justification::centred);
    hostCaption.setColour (juce::Label::textColourId, Colours::muted);
    hostCaption.setFont (captionFont());
    addAndMakeVisible (hostCaption);

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

    // The trim is in dB, which is already the scale the ear works in, so it needs
    // no skew: half the travel is half the range. Double-click returns it to the
    // default, which is the level the hidden 0.8 used to give every voice.
    masterSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    masterSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    masterSlider.setRange (Layout::minMasterGainDb, Layout::maxMasterGainDb, 0.1);
    masterSlider.setDoubleClickReturnValue (true, Layout::defaultMasterGainDb);
    masterSlider.setTooltip ("Master output trim");
    addAndMakeVisible (masterSlider);

    masterValueLabel.setJustificationType (juce::Justification::centredLeft);
    masterValueLabel.setColour (juce::Label::textColourId, Colours::text);
    masterValueLabel.setFont (captionFont());
    addAndMakeVisible (masterValueLabel);

    addAndMakeVisible (masterMeter);

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

    masterSlider.onValueChange = [this]
    {
        processor.setMasterGainDb ((float) masterSlider.getValue());
        masterValueLabel.setText (GrooveboxProcessor::formatMasterGain (processor.getMasterGainDb()),
                                  juce::dontSendNotification);
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

    masterSlider.setValue ((double) processor.getMasterGainDb(), juce::dontSendNotification);
    masterValueLabel.setText (GrooveboxProcessor::formatMasterGain (processor.getMasterGainDb()),
                              juce::dontSendNotification);
}

void TransportBar::resized()
{
    auto area = getLocalBounds().reduced (8, 6);
    area.removeFromBottom (2); // leave room for the separator rule

    // The master section comes off the right first, so it keeps its width however
    // narrow the window gets and the tools to its left are what give way.
    auto masterArea = area.removeFromRight (212);
    masterCaptionArea = masterArea.removeFromTop (12); // drawn in paint()
    masterArea.removeFromTop (2);
    masterSlider.setBounds (masterArea.removeFromLeft (110).withSizeKeepingCentre (110, 22));
    masterArea.removeFromLeft (6);
    masterValueLabel.setBounds (masterArea.removeFromLeft (52).withSizeKeepingCentre (52, 18));
    masterArea.removeFromLeft (6);
    masterMeter.setBounds (masterArea.removeFromLeft (38).withSizeKeepingCentre (38, 22));
    area.removeFromRight (10);

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
}

//==============================================================================
void TransportBar::paint (juce::Graphics& g)
{
    paintCaption (g, "Tempo", tempoCaptionArea);
    paintCaption (g, "Swing", swingCaptionArea);
    paintCaption (g, "Master", masterCaptionArea);

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
    processor.beginTransaction ("Reset velocities");
    processor.resetVelocities();

    if (processor.onStateChanged != nullptr)
        processor.onStateChanged();

    refresh();
}

} // namespace gb
