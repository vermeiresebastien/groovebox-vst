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

    /**
        Puts one of the two grid overlays away, and tells whoever is listening.

        The bar enforces this rather than leaving it to the grid. The grid can only edit
        one number at a time and closes the other one for itself, but only the bar can
        see both buttons, and a button still lit for an overlay that has been closed is
        a panel that lies about what a drag is going to write.
    */
    void closeOverlay (juce::TextButton& button, std::function<void (bool)>& callback)
    {
        if (! button.getToggleState())
            return;

        button.setToggleState (false, juce::dontSendNotification);

        if (callback != nullptr)
            callback (false);
    }
}

//==============================================================================
MasterMeter::MasterMeter (GrooveboxProcessor& processorToUse)
    : processor (processorToUse)
{
    setTooltip ("Master output level. Bar = sample peak, tick = true (inter-sample) "
                "peak. Click to clear the clip indicator.");
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

        // True peak, on the same scale as the bar: where the two markers part
        // company is the inter-sample headroom, and a tick past 0 dBFS says the
        // waveform leaves the chip above full scale even when no sample did. It is
        // drawn over the fill and changes nothing else — the latch below is still
        // the sample reaching 0 dBFS, which is a different claim.
        const auto truePeak = processor.getMasterTruePeak (channel);
        const auto truePeakDb = juce::Decibels::gainToDecibels (truePeak, floorDb);
        const auto truePeakFraction
            = juce::jlimit (0.0f, 1.0f, (truePeakDb - floorDb) / (ceilingDb - floorDb));

        if (truePeakFraction > 0.0f)
        {
            const auto tick = inner.withX (inner.getX() + inner.getWidth() * truePeakFraction - 1.0f)
                                   .withWidth (2.0f);

            g.setColour (truePeakDb >= 0.0f ? Colours::meterOver : Colours::muted);
            g.fillRect (tick);
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

    // Named, because three of the bar's faders now run 0..1 and two of them run in
    // dB, and a control that is identified by its range is identified by a number it
    // happens to share with someone else. The layout test used to tell swing from the
    // trim that way; it cannot tell three apart.
    swingSlider.setName ("Swing");
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

    /**
        The two numbers each step carries, one button each.

        A toggle rather than a mode switch with somewhere else to go back to: the
        grid's default gesture is the one everybody uses, so editing has to be able to
        stop. The "on" colours invert the button, which is how the lit one reads as
        "the grid is showing you this" from across the room.
    */
    for (auto* button : { &velocityButton, &chanceButton })
    {
        button->setClickingTogglesState (true);
        button->setColour (juce::TextButton::buttonOnColourId, Colours::text);
        button->setColour (juce::TextButton::textColourOnId, Colours::background);
        addAndMakeVisible (*button);
    }

    velocityButton.setTooltip ("Edit how hard each step hits: drag up and down inside a step");
    chanceButton.setTooltip ("Edit how often each step fires: drag down inside a step to drop some of them");

    // The reset belongs to whichever of the two is open, so its tooltip says which
    // before you click it rather than after. refresh() keeps it in step.
    resetValuesButton.setTooltip ("Put every step back to full velocity");
    addAndMakeVisible (resetValuesButton);

    /**
        Variation: how much of the grid's probabilities the machine may use.

        Shut, the pattern is law — what you drew is what plays, whatever the cells say
        about probability, and the knob costs nothing. Open, each step is rolled against
        its own chance. Double-click returns it to shut, which is the state that cannot
        surprise anyone.
    */
    mutateSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    mutateSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    mutateSlider.setRange (0.0, 1.0, 0.01);
    mutateSlider.setDoubleClickReturnValue (true, 0.0);
    mutateSlider.setTooltip ("Mutate: how much of each step's probability the machine may use."
                             " Shut, the grid is law.");
    mutateSlider.setName ("Mutate");
    addAndMakeVisible (mutateSlider);

    mutateValueLabel.setJustificationType (juce::Justification::centredLeft);
    mutateValueLabel.setColour (juce::Label::textColourId, Colours::text);
    mutateValueLabel.setFont (captionFont());
    addAndMakeVisible (mutateValueLabel);

    rerollButton.setTooltip ("Take a new seed: a different set of drops, the same grid");
    addAndMakeVisible (rerollButton);

    // The number is the take's name. Without it a good drop is unreproducible, which
    // is the difference between a machine that deals and one that shimmers.
    seedLabel.setJustificationType (juce::Justification::centredLeft);
    seedLabel.setColour (juce::Label::textColourId, Colours::text);
    seedLabel.setFont (captionFont());
    seedLabel.setMinimumHorizontalScale (0.6f);
    seedLabel.setTooltip ("Seed: same number, same grid, same drops. Reroll picks a new one.");
    seedLabel.setName ("Seed"); // the same naming rule as the faders, so a test can ask for it
    addAndMakeVisible (seedLabel);

    // The trim is in dB, which is already the scale the ear works in, so it needs
    // no skew: half the travel is half the range. Double-click returns it to the
    // default, which is the level the hidden 0.8 used to give every voice.
    masterSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    masterSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    masterSlider.setRange (Layout::minMasterGainDb, Layout::maxMasterGainDb, 0.1);
    masterSlider.setDoubleClickReturnValue (true, Layout::defaultMasterGainDb);
    masterSlider.setTooltip ("Master output trim");
    masterSlider.setName ("Master");
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
    chanceButton.onClick      = [this] { handleChanceClicked(); };
    resetValuesButton.onClick = [this] { handleResetValuesClicked(); };
    rerollButton.onClick      = [this] { handleRerollClicked(); };

    // Written straight through, for the same reason as the channel faders: an
    // earlier version skipped the write while a drag was in progress and relied
    // on the release to commit it, which never happened because the 200 ms
    // refresh repopulated the slider from the processor and pulled the thumb
    // back. Swing then appeared to do nothing at all.
    swingSlider.onValueChange = [this]
    {
        processor.setSwing ((float) swingSlider.getValue());
    };

    // Same straight-through rule as swing: the knob is what a drag is for, and a
    // value that only committed on release would be pulled back by this bar's own
    // 200 ms refresh halfway through the gesture.
    mutateSlider.onValueChange = [this]
    {
        processor.setMutate ((float) mutateSlider.getValue());

        mutateValueLabel.setText (juce::String (juce::roundToInt (mutateSlider.getValue() * 100.0)) + "%",
                                  juce::dontSendNotification);
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

    // The grid the engine is following, not the number the parameter holds: a host
    // that types a 3 gets the 1/8 pair, and this is where the panel says so. Without
    // the same rule on both sides the two buttons would go dark on a value the panel
    // itself cannot write, which reads as a control that has stopped working.
    const auto grid = Layout::usableSwingGrid (processor.getSwingGrid());
    swingEighthButton.setToggleState (grid == 4, juce::dontSendNotification);
    swingSixteenthButton.setToggleState (grid == 2, juce::dontSendNotification);

    masterSlider.setValue ((double) processor.getMasterGainDb(), juce::dontSendNotification);
    masterValueLabel.setText (GrooveboxProcessor::formatMasterGain (processor.getMasterGainDb()),
                              juce::dontSendNotification);

    mutateSlider.setValue ((double) processor.getMutate(), juce::dontSendNotification);
    mutateValueLabel.setText (juce::String (juce::roundToInt (processor.getMutate() * 100.0f)) + "%",
                              juce::dontSendNotification);
    seedLabel.setText (juce::String (processor.getSeed()), juce::dontSendNotification);

    // The shared reset follows the overlay that is open, so the hover text names the
    // number it is about to overwrite. With neither open it does velocities, which is
    // what the button said before there was a choice — and what the tooltip says.
    resetValuesButton.setTooltip (chanceButton.getToggleState()
                                      ? "Put every step back to firing every time"
                                      : "Put every step back to full velocity");
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

    // Variation sits next to swing, because both of them bend what was drawn rather
    // than what is stored: swing moves the hits in time, this one drops some of them.
    auto variationArea = area.removeFromLeft (272);
    variationCaptionArea = variationArea.removeFromTop (12); // drawn in paint()
    variationArea.removeFromTop (2);
    mutateSlider.setBounds (variationArea.removeFromLeft (96).withSizeKeepingCentre (96, 22));
    variationArea.removeFromLeft (6);
    mutateValueLabel.setBounds (variationArea.removeFromLeft (42).withSizeKeepingCentre (42, 18));
    variationArea.removeFromLeft (6);
    rerollButton.setBounds (variationArea.removeFromLeft (58).withSizeKeepingCentre (58, 24));
    variationArea.removeFromLeft (6);
    seedLabel.setBounds (variationArea.withSizeKeepingCentre (variationArea.getWidth(), 18));
    area.removeFromLeft (8);

    clearPatternButton.setBounds (area.removeFromLeft (108).withSizeKeepingCentre (108, 24));
    area.removeFromLeft (8);

    velocityButton.setBounds (area.removeFromLeft (76).withSizeKeepingCentre (76, 24));
    area.removeFromLeft (4);
    chanceButton.setBounds (area.removeFromLeft (76).withSizeKeepingCentre (76, 24));
    area.removeFromLeft (4);
    resetValuesButton.setBounds (area.removeFromLeft (46).withSizeKeepingCentre (46, 22));
}

//==============================================================================
void TransportBar::paint (juce::Graphics& g)
{
    paintCaption (g, "Tempo", tempoCaptionArea);
    paintCaption (g, "Swing", swingCaptionArea);
    paintCaption (g, "Variation", variationCaptionArea);
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
    /**
        Every step on every lane goes at once, so this asks first.

        `AlertWindow::showAsync` rather than `show`: `show` runs a modal loop, which
        is the thing that deadlocks a host that is still rendering, and the answer
        comes back as the index of the button that was pressed instead of as a
        return value. The box is parented to the plugin window for the same reason
        a child window is dangerous in an AU.

        The strip keeps its own `beginTransaction`, which is what makes the clear a
        single undo rather than a hundred and ninety-two.
    */
    juce::Component::SafePointer<TransportBar> safeThis (this);

    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::WarningIcon)
                                      .withTitle ("Clear the whole pattern?")
                                      .withMessage ("This switches off every step on every lane. "
                                                    "One undo brings them all back.")
                                      .withButton ("Clear")
                                      .withButton ("Keep it")
                                      .withParentComponent (this),
                                  [safeThis] (int button)
    {
        if (safeThis == nullptr || button != 0)
            return;

        safeThis->processor.beginTransaction ("Clear pattern");
        safeThis->processor.clearPattern();

        if (safeThis->processor.onStateChanged != nullptr)
            safeThis->processor.onStateChanged();

        safeThis->refresh();
    });
}

void TransportBar::handleVelocityClicked()
{
    if (velocityButton.getToggleState())
        closeOverlay (chanceButton, onChanceModeChanged);

    if (onVelocityModeChanged != nullptr)
        onVelocityModeChanged (velocityButton.getToggleState());
}

void TransportBar::handleChanceClicked()
{
    if (chanceButton.getToggleState())
        closeOverlay (velocityButton, onVelocityModeChanged);

    if (onChanceModeChanged != nullptr)
        onChanceModeChanged (chanceButton.getToggleState());
}

void TransportBar::handleResetValuesClicked()
{
    /**
        One button, two numbers, and it does the one that is on the grid.

        That is only honest because the tooltip and the lit toggle both say which it is
        before the click; a button whose target moved silently would be the same control
        doing something different, which is the thing this project does not do to a
        number the user is watching.

        Either way it is one gesture, so it is one transaction: without
        `beginTransaction` a hundred and ninety-two cell writes land as a hundred and
        ninety-two undoes.
    */
    const auto editingChance = chanceButton.getToggleState();

    processor.beginTransaction (editingChance ? "Reset probabilities" : "Reset velocities");

    if (editingChance)
        processor.resetChances();
    else
        processor.resetVelocities();

    if (processor.onStateChanged != nullptr)
        processor.onStateChanged();

    refresh();
}

void TransportBar::handleRerollClicked()
{
    /**
        A new seed, and nothing else.

        The grid does not move and no probability is re-drawn: what changes is which of
        the hits the machine is allowed to drop. That is also why this is not an undo —
        an undo of a reroll would put back a *number*, and the take the user heard is
        not recoverable from that anyway once the knob has moved. The seed is displayed
        for exactly this reason: write it down instead of undoing it.
    */
    processor.rerollSeed();
    refresh();
}

} // namespace gb
