#include "ChannelStrip.h"

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

ChannelStrip::ChannelStrip (GrooveboxProcessor& processorToUse, int channelIndexToUse)
    : processor (processorToUse),
      channel (juce::jlimit (0, GrooveboxProcessor::getNumChannels_() - 1, channelIndexToUse))
{
    colourButton.setTooltip ("Click to cycle the channel colour");
    colourButton.setColour (juce::TextButton::buttonColourId, processor.getChannelColour (channel));
    colourButton.setColour (juce::TextButton::buttonOnColourId, processor.getChannelColour (channel));
    addAndMakeVisible (colourButton);

    nameLabel.setJustificationType (juce::Justification::centredLeft);
    nameLabel.setColour (juce::Label::textColourId, Colours::text);
    nameLabel.setMinimumHorizontalScale (0.6f);
    // Double-click edits; a click on empty space starts an edit too.
    nameLabel.setEditable (true, true, false);
    addAndMakeVisible (nameLabel);

    soundButton.setTooltip ("Click to load a sample for this channel");
    addAndMakeVisible (soundButton);
    clearRowButton.setButtonText ("Clear");
    clearRowButton.setTooltip ("Remove every step on this channel");
    addAndMakeVisible (clearRowButton);

    shiftLeftButton.setTooltip ("Move this whole row one step earlier, with its velocities");
    shiftRightButton.setTooltip ("Move this whole row one step later, with its velocities");
    addAndMakeVisible (shiftLeftButton);
    addAndMakeVisible (shiftRightButton);

    // A 24 px button pair plus a readout is about as much as a strip this narrow
    // can hold. An IncDecButtons slider wanted 44 px for its text box alone and
    // was being squeezed to zero width once the octave buttons took their space,
    // so the readout is a plain label and the buttons do the stepping.
    pitchValueLabel.setJustificationType (juce::Justification::centred);
    pitchValueLabel.setMinimumHorizontalScale (0.7f);
    pitchValueLabel.setColour (juce::Label::backgroundColourId, Colours::chip);
    pitchValueLabel.setColour (juce::Label::textColourId, Colours::text);
    pitchValueLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    pitchValueLabel.setTooltip ("Pitch offset in semitones (-36 to +12)");
    pitchValueLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (pitchValueLabel);

    pitchDown12Button.setTooltip ("Down an octave");
    pitchDownButton.setTooltip ("Down a semitone");
    pitchUpButton.setTooltip ("Up a semitone");
    pitchUp12Button.setTooltip ("Up an octave");
    addAndMakeVisible (pitchDown12Button);
    addAndMakeVisible (pitchDownButton);
    addAndMakeVisible (pitchUpButton);
    addAndMakeVisible (pitchUp12Button);

    levelSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    levelSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    levelSlider.setRange (0.0, 1.5, 0.01);
    levelSlider.setDoubleClickReturnValue (true, 1.0);
    levelSlider.setTooltip ("Channel level");
    addAndMakeVisible (levelSlider);

    // The drop is a count of semitones, so the track has thirteen stops and every
    // one of them is a pixel-and-a-half apart at this width. Double-click puts it
    // back at none, which is where a lane that is only supposed to be tuned lives.
    dropSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    dropSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    dropSlider.setRange (0.0, (double) Params::pitchDropMax, 1.0);
    dropSlider.setDoubleClickReturnValue (true, 0.0);
    dropSlider.setTooltip ("Pitch drop: how far above its own note a hit starts, in semitones.\n"
                           "It falls to the note over 30 ms and stays there, so this shapes the "
                           "attack without retuning the lane. 0 = the hit is at its pitch from "
                           "its first sample. Double-click to reset.");
    addAndMakeVisible (dropSlider);

    dropValueLabel.setJustificationType (juce::Justification::centred);
    dropValueLabel.setMinimumHorizontalScale (0.7f);
    dropValueLabel.setColour (juce::Label::textColourId, Colours::text);
    dropValueLabel.setFont (juce::Font (juce::FontOptions (10.0f)));
    dropValueLabel.setTooltip (dropSlider.getTooltip());
    dropValueLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (dropValueLabel);

    muteButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffff5a5a));
    muteButton.setTooltip ("Mute this channel");
    addAndMakeVisible (muteButton);

    // One item per group, plus `0` for "not in one". The ids run one higher than
    // the group because JUCE reserves id 0 for "no selection", and a control that
    // cannot show its own value is not a control.
    chokeComboBox.setTooltip ("Choke group: a hit in a group cuts every other hit in that group short.\n"
                              "0 = this lane is not in a group.");
    chokeComboBox.addItem ("0", 1);

    for (int group = 1; group <= Layout::numChokeGroups; ++group)
        chokeComboBox.addItem (juce::String (group), group + 1);

    addAndMakeVisible (chokeComboBox);

    envelope.onValueChange = [this] (EnvelopeStage stage, float value)
    {
        switch (stage)
        {
            case EnvelopeStage::attack:  processor.setChannelAttack (channel, value); break;
            case EnvelopeStage::decay:   processor.setChannelDecay (channel, value); break;
            case EnvelopeStage::sustain: processor.setChannelSustain (channel, value); break;
            case EnvelopeStage::release: processor.setChannelRelease (channel, value); break;
        }
    };

    envelope.onReset = [this] (EnvelopeStage stage)
    {
        // Double-clicking one row resets just that row, which is more useful than
        // throwing away the other three.
        switch (stage)
        {
            case EnvelopeStage::attack:  processor.setChannelAttack (channel, Layout::defaultAttack); break;
            case EnvelopeStage::decay:   processor.setChannelDecay (channel, Layout::defaultDecay); break;
            case EnvelopeStage::sustain: processor.setChannelSustain (channel, Layout::defaultSustain); break;
            case EnvelopeStage::release: processor.setChannelRelease (channel, Layout::defaultRelease); break;
        }

        refresh();
    };

    addAndMakeVisible (envelope);

    formatManager.registerBasicFormats();

    // Values first, callbacks afterwards: refresh() therefore cannot trigger a
    // write back into the processor while the strip is being built.
    refresh();

    colourButton.onClick      = [this] { handleColourClicked(); };
    soundButton.onClick       = [this] { handleSoundClicked(); };
    clearRowButton.onClick    = [this] { handleClearRowClicked(); };
    pitchDown12Button.onClick = [this] { nudgePitch (-12); };
    pitchDownButton.onClick   = [this] { nudgePitch (-1); };
    pitchUpButton.onClick     = [this] { nudgePitch (1); };
    pitchUp12Button.onClick   = [this] { nudgePitch (12); };

    levelSlider.onValueChange = [this]
    {
        processor.setChannelLevel (channel, (float) levelSlider.getValue());
    };

    dropSlider.onValueChange = [this]
    {
        processor.setChannelPitchDrop (channel, juce::roundToInt (dropSlider.getValue()));
    };

    muteButton.onClick = [this] { processor.setChannelMuted (channel, muteButton.getToggleState()); };

    // JUCE 8's combo box calls this with no argument, so the value is read back
    // from the box. Zero is its "no selection" id, which means leave it alone —
    // it is not group -1, and writing that would land on a group nobody chose.
    chokeComboBox.onChange = [this]
    {
        if (const auto newId = chokeComboBox.getSelectedId(); newId > 0)
            processor.setChannelChokeGroup (channel, newId - 1);
    };
    clearRowButton.onClick = [this] { handleClearRowClicked(); };
    shiftLeftButton.onClick = [this] { handleShiftRow (-1); };
    shiftRightButton.onClick = [this] { handleShiftRow (1); };

    // Double-click the label to rename; the change is committed on editor close.
    nameLabel.onTextChange = [this] { handleNameChanged(); };

    // Poll the engine so the strip can light up while the channel fires.
    startTimer (30);
}

ChannelStrip::~ChannelStrip()
{
    stopTimer();
}

//==============================================================================
void ChannelStrip::refresh()
{
    refreshName();

    const auto channelColour = processor.getChannelColour (channel);
    colourButton.setColour (juce::TextButton::buttonColourId, channelColour);
    colourButton.setColour (juce::TextButton::buttonOnColourId, channelColour);

    const auto sampleName = processor.getChannelSampleName (channel);
    soundButton.setButtonText (processor.getChannelSampleId (channel) == 0 || sampleName.isEmpty()
                                   ? "Load sample..."
                                   : sampleName);

    // Mirror the processor into the controls. dontSendNotification is what keeps
    // this from fighting the user: it updates the visuals without re-entering the
    // value-change callbacks that write back to the processor. Skipping the write
    // during a drag instead (as an earlier version did) meant a fader move never
    // reached the processor at all.
    syncAutomatableControls();

    repaint();
}

void ChannelStrip::syncAutomatableControls()
{
    levelSlider.setValue ((double) processor.getChannelLevel (channel), juce::dontSendNotification);
    muteButton.setToggleState (processor.isChannelMuted (channel), juce::dontSendNotification);
    chokeComboBox.setSelectedId (processor.getChannelChokeGroup (channel) + 1,
                                 juce::dontSendNotification);

    const auto pitch = processor.getChannelPitch (channel);
    pitchValueLabel.setText (pitch > 0 ? "+" + juce::String (pitch) : juce::String (pitch),
                             juce::dontSendNotification);

    const auto drop = processor.getChannelPitchDrop (channel);
    dropSlider.setValue ((double) drop, juce::dontSendNotification);
    dropValueLabel.setText (drop > 0 ? "\xe2\x86\x93" + juce::String (drop)
                                     : juce::String ("\xe2\x80\x93"),   // en dash: no drop
                            juce::dontSendNotification);

    envelope.setValue (EnvelopeStage::attack,  processor.getChannelAttack (channel));
    envelope.setValue (EnvelopeStage::decay,   processor.getChannelDecay (channel));
    envelope.setValue (EnvelopeStage::sustain, processor.getChannelSustain (channel));
    envelope.setValue (EnvelopeStage::release, processor.getChannelRelease (channel));
}

void ChannelStrip::refreshName()
{
    const auto name = processor.getChannelName (channel);
    nameLabel.setText (name.isNotEmpty() ? name : juce::String ("Channel ") + juce::String (channel + 1),
                       juce::dontSendNotification);
}

void ChannelStrip::resized()
{
    auto area = getLocalBounds().reduced (Layout::stripFrame);
    area.removeFromLeft (2); // keep clear of the channel-colour edge

    // One budget, asked of one function. The sizes of every row come from
    // `Layout::stripPlan`, which picks its stage from what this strip actually has, so
    // the arithmetic that decides whether the mute button is on screen is the same
    // arithmetic the layout test checks — and not, as it was, a pair of size sets and a
    // threshold (219) that sat below the cost of both of them (268 and 228).
    const auto room = area.getHeight();
    const auto plan = Layout::stripPlan (room);

    const auto gapSize = plan.sectionGap;
    const auto captionHeight = plan.caption;

    auto header = area.removeFromTop (plan.header);
    colourButton.setBounds (header.removeFromLeft (16).withSizeKeepingCentre (14, 14));
    header.removeFromLeft (4);
    soundingDotArea = header.removeFromRight (8).withSizeKeepingCentre (6, 6);
    nameLabel.setBounds (header);

    area.removeFromTop (gapSize);
    soundButton.setBounds (area.removeFromTop (plan.sound));
    area.removeFromTop (plan.soundGap);

    // Clear and the two shift arrows share one row: they are all row operations,
    // and the arrows are small enough to stay clickable at this width.
    auto rowButtons = area.removeFromTop (plan.clearRow);
    constexpr int shiftWidth = 18;
    shiftLeftButton.setBounds (rowButtons.removeFromLeft (shiftWidth));
    rowButtons.removeFromLeft (3);
    shiftRightButton.setBounds (rowButtons.removeFromLeft (shiftWidth));
    rowButtons.removeFromLeft (3);
    clearRowButton.setBounds (rowButtons);

    area.removeFromTop (gapSize);
    pitchCaptionArea = area.removeFromTop (captionHeight); // drawn in paint()
    auto pitchRow = area.removeFromTop (plan.pitchRow);

    // 14 + 14 + 14 + 14 button widths = 56 of the 77 px available, leaving room
    // for the readout between them.
    constexpr int smallButton = 14;
    constexpr int buttonGap = 2;

    pitchDown12Button.setBounds (pitchRow.removeFromLeft (smallButton));
    pitchRow.removeFromLeft (buttonGap);
    pitchDownButton.setBounds (pitchRow.removeFromLeft (smallButton));
    pitchRow.removeFromLeft (buttonGap);
    pitchUp12Button.setBounds (pitchRow.removeFromRight (smallButton));
    pitchRow.removeFromRight (buttonGap);
    pitchUpButton.setBounds (pitchRow.removeFromRight (smallButton));
    pitchRow.removeFromRight (buttonGap);
    pitchValueLabel.setBounds (pitchRow.reduced (1, 1));

    // The drop's own line, inside the Pitch block and directly under it: a short
    // readout and a track, no caption row of its own, because the strip has no
    // height to give and the arrow says what the number is.
    auto dropRow = area.removeFromTop (plan.dropRow);
    constexpr int dropLabelWidth = 24;
    dropValueLabel.setBounds (dropRow.removeFromLeft (dropLabelWidth).reduced (1, 0));
    dropRow.removeFromLeft (2);
    dropSlider.setBounds (dropRow);

    area.removeFromTop (gapSize);
    levelCaptionArea = area.removeFromTop (captionHeight); // drawn in paint()
    levelSlider.setBounds (area.removeFromTop (plan.level));

    area.removeFromTop (gapSize);
    envelopeCaptionArea = area.removeFromTop (captionHeight); // drawn in paint()
    // Four ADSR rows in one band. The band is the plan's absorber: at 56 px each row is
    // 14, at the 44 px floor 11, and below that whatever the strip has left, because
    // `EnvelopeControls::boundsFor` floors its own row at 1 and owns no child components.
    envelope.setBounds (area.removeFromTop (plan.envelope));

    area.removeFromTop (gapSize);
    chokeCaptionArea = area.removeFromTop (captionHeight); // drawn in paint()

    // The choke box and the mute switch share the last row. The box takes 34 px,
    // which leaves 40 for "Mute" — the button's font is clamped to 11 px at this
    // height, and the word measures 24.
    auto muteRow = area.removeFromTop (plan.muteRow);
    chokeComboBox.setBounds (muteRow.removeFromLeft (34));
    muteRow.removeFromLeft (3);
    muteButton.setBounds (muteRow);

    // The plan's own deficit, not what is left of the rectangle: `removeFromTop` clamps,
    // so the leftover reads 0 on a strip that is 44 px short and the check built on it
    // could not go red. See `Layout::stripPlan`.
    lastSpareHeight = plan.spare;
    lastLayoutStage = plan.stage;
}

//==============================================================================
void ChannelStrip::paint (juce::Graphics& g)
{
    auto panel = getLocalBounds().toFloat().reduced (1.0f);

    g.setColour (Colours::chip);
    g.fillRoundedRectangle (panel, 4.0f);

    // 2 px accent down the left edge, in the channel colour.
    auto accent = panel.withWidth (2.0f);
    g.setColour (processor.getChannelColour (channel));
    g.fillRoundedRectangle (accent, 1.0f);

    g.setColour (Colours::chipBorder);
    g.drawRoundedRectangle (panel, 4.0f, 1.0f);

    // Section captions live in the gaps left by resized().
    paintCaption (g, "Pitch", pitchCaptionArea);
    paintCaption (g, "Level", levelCaptionArea);
    paintCaption (g, "Env", envelopeCaptionArea);
    paintCaption (g, "Choke", chokeCaptionArea);

    // Lit dot at the end of the header while this channel is firing.
    const auto dot = soundingDotArea.toFloat().withSizeKeepingCentre (6.0f, 6.0f);
    if (isSounding)
    {
        g.setColour (processor.getChannelColour (channel).withAlpha (0.3f));
        g.fillEllipse (dot.expanded (2.0f));
        g.setColour (processor.getChannelColour (channel));
        g.fillEllipse (dot);
    }
    else
    {
        g.setColour (Colours::chipBorder.withAlpha (0.35f));
        g.drawEllipse (dot, 1.0f);
    }
}

void ChannelStrip::paintCaption (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area) const
{
    if (area.isEmpty())
        return;

    g.setColour (Colours::muted);
    g.setFont (captionFont());
    g.drawText (text, area, juce::Justification::centredLeft, false);
}

//==============================================================================
void ChannelStrip::timerCallback()
{
    // Follow host automation: a parameter can move without the panel being
    // touched, and this is the only thing that re-reads it. Each control writes
    // straight through when the user moves it, so during a drag this reads back
    // the value under the pointer and leaves it alone.
    syncAutomatableControls();

    const auto nowSounding = processor.isChannelSounding (channel);

    if (nowSounding != isSounding)
    {
        isSounding = nowSounding;
        repaint();
    }
}

//==============================================================================
void ChannelStrip::handleNameChanged()
{
    const auto newName = nameLabel.getText().trim();

    if (newName.isEmpty())
    {
        // Empty names are not allowed; put the stored name back.
        refreshName();
        return;
    }

    if (newName == processor.getChannelName (channel))
        return;

    processor.beginTransaction ("Rename channel");
    processor.setChannelName (channel, newName);

    if (processor.onStateChanged != nullptr)
        processor.onStateChanged();
}

void ChannelStrip::handleColourClicked()
{
    const auto paletteSize = (int) lanePalette.size();
    const auto current = processor.getChannelColour (channel);

    // Start from the current colour when it is one of the palette entries.
    int nextIndex = 0;
    for (int i = 0; i < paletteSize; ++i)
    {
        if (juce::Colour (lanePalette[(size_t) i]) == current)
        {
            nextIndex = (i + 1) % paletteSize;
            break;
        }
    }

    processor.beginTransaction ("Change channel colour");
    processor.setChannelColour (channel, juce::Colour (lanePalette[(size_t) nextIndex]));

    if (processor.onStateChanged != nullptr)
        processor.onStateChanged();

    refresh();
}

void ChannelStrip::handleSoundClicked()
{
    const auto browserFlags = juce::FileBrowserComponent::openMode
                                | juce::FileBrowserComponent::canSelectFiles;

    // The wildcard is only populated for formats the manager knows about, so
    // the basic set is registered once in the constructor.
    chooser = std::make_unique<juce::FileChooser> ("Load a sample for this channel",
                                                   juce::File(),
                                                   formatManager.getWildcardForAllFormats());

    // SafePointer: the dialog outlives this component if the editor is closed.
    juce::Component::SafePointer<ChannelStrip> safeThis (this);

    chooser->launchAsync (browserFlags, [safeThis] (const juce::FileChooser& fc)
    {
        if (safeThis == nullptr)
            return;

        const auto file = fc.getResult();

        if (file == juce::File())
            return;

        juce::String error;

        if (! safeThis->processor.loadSampleIntoChannel (safeThis->channel, file, error))
        {
            if (error.isEmpty())
                error = "The file could not be decoded as audio.";

            juce::NativeMessageBox::showAsync (juce::MessageBoxOptions()
                                                   .withIconType (juce::MessageBoxIconType::WarningIcon)
                                                   .withTitle ("Could not load sample")
                                                   .withMessage (error),
                                               nullptr);
            return;
        }

        if (safeThis->processor.onStateChanged != nullptr)
            safeThis->processor.onStateChanged();

        safeThis->refresh();
    });
}

void ChannelStrip::handleClearRowClicked()
{
    // The row is this lane's whole part in the song, and one click takes it, so it
    // asks first. See `TransportBar::handleClearPatternClicked` for why the box is
    // asynchronous and parented.
    juce::Component::SafePointer<ChannelStrip> safeThis (this);

    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::WarningIcon)
                                      .withTitle ("Clear this row?")
                                      .withMessage (processor.getChannelName (channel)
                                                        + ": every step on it goes off. "
                                                        "One undo brings them back.")
                                      .withButton ("Clear")
                                      .withButton ("Keep it")
                                      .withParentComponent (this),
                                  [safeThis] (int button)
    {
        if (safeThis == nullptr || button != 0)
            return;

        safeThis->processor.beginTransaction ("Clear row");
        safeThis->processor.clearChannelRow (safeThis->channel);

        if (safeThis->processor.onStateChanged != nullptr)
            safeThis->processor.onStateChanged();

        safeThis->refresh();
    });
}

void ChannelStrip::handleShiftRow (int steps)
{
    processor.beginTransaction (steps > 0 ? "Shift row later" : "Shift row earlier");
    processor.shiftChannelRow (channel, steps);

    // The processor already told the editor; refreshing here keeps this strip's
    // own controls in step straight away.
    refresh();
}

void ChannelStrip::nudgePitch (int delta)
{
    processor.beginTransaction ("Nudge channel pitch");
    processor.nudgeChannelPitch (channel, delta);
    refresh();
}

} // namespace gb
