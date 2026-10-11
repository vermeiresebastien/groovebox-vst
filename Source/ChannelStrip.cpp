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

    muteButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffff5a5a));
    muteButton.setTooltip ("Mute this channel");
    addAndMakeVisible (muteButton);

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

    muteButton.onClick = [this] { processor.setChannelMuted (channel, muteButton.getToggleState()); };
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
    levelSlider.setValue ((double) processor.getChannelLevel (channel), juce::dontSendNotification);
    muteButton.setToggleState (processor.isChannelMuted (channel), juce::dontSendNotification);

    const auto pitch = processor.getChannelPitch (channel);
    pitchValueLabel.setText (pitch > 0 ? "+" + juce::String (pitch) : juce::String (pitch),
                             juce::dontSendNotification);

    envelope.setValue (EnvelopeStage::attack,  processor.getChannelAttack (channel));
    envelope.setValue (EnvelopeStage::decay,   processor.getChannelDecay (channel));
    envelope.setValue (EnvelopeStage::sustain, processor.getChannelSustain (channel));
    envelope.setValue (EnvelopeStage::release, processor.getChannelRelease (channel));

    repaint();
}

void ChannelStrip::refreshName()
{
    const auto name = processor.getChannelName (channel);
    nameLabel.setText (name.isNotEmpty() ? name : juce::String ("Channel ") + juce::String (channel + 1),
                       juce::dontSendNotification);
}

void ChannelStrip::resized()
{
    auto area = getLocalBounds().reduced (6);
    area.removeFromLeft (2); // keep clear of the channel-colour edge

    // The host editor can give the strip less height than the ideal layout, so
    // the gaps between sections shrink first and the heights shrink after that.
    constexpr int captionHeight = 11;
    constexpr int wantedHeight = 188;

    const auto room = area.getHeight();
    const auto tight = room < wantedHeight;

    const int gapSize = tight ? 2 : 6;
    const int headerHeight = tight ? 16 : 20;
    const int soundHeight = tight ? 20 : 22;
    const int clearRowHeight = tight ? 18 : 20;
    const int pitchRowHeight = tight ? 18 : 20;
    const int levelHeight = tight ? 16 : 18;
    const int muteHeight = tight ? 20 : 22;

    // Four ADSR rows. At 14 px each they are short but still draggable, which is
    // what the strip's width allows.
    const int envelopeHeight = tight ? 52 : 56;

    auto header = area.removeFromTop (headerHeight);
    colourButton.setBounds (header.removeFromLeft (16).withSizeKeepingCentre (14, 14));
    header.removeFromLeft (4);
    soundingDotArea = header.removeFromRight (8).withSizeKeepingCentre (6, 6);
    nameLabel.setBounds (header);

    area.removeFromTop (gapSize);
    soundButton.setBounds (area.removeFromTop (soundHeight));
    area.removeFromTop (2);

    // Clear and the two shift arrows share one row: they are all row operations,
    // and the arrows are small enough to stay clickable at this width.
    auto rowButtons = area.removeFromTop (clearRowHeight);
    constexpr int shiftWidth = 18;
    shiftLeftButton.setBounds (rowButtons.removeFromLeft (shiftWidth));
    rowButtons.removeFromLeft (3);
    shiftRightButton.setBounds (rowButtons.removeFromLeft (shiftWidth));
    rowButtons.removeFromLeft (3);
    clearRowButton.setBounds (rowButtons);

    area.removeFromTop (gapSize);
    pitchCaptionArea = area.removeFromTop (captionHeight); // drawn in paint()
    auto pitchRow = area.removeFromTop (pitchRowHeight);

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

    area.removeFromTop (gapSize);
    levelCaptionArea = area.removeFromTop (captionHeight); // drawn in paint()
    levelSlider.setBounds (area.removeFromTop (levelHeight));

    area.removeFromTop (gapSize);
    envelopeCaptionArea = area.removeFromTop (captionHeight); // drawn in paint()
    envelope.setBounds (area.removeFromTop (envelopeHeight));

    area.removeFromTop (gapSize);
    muteButton.setBounds (area.removeFromTop (muteHeight));

    lastSpareHeight = area.getHeight();
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
    processor.beginTransaction ("Clear row");
    processor.clearChannelRow (channel);

    if (processor.onStateChanged != nullptr)
        processor.onStateChanged();

    refresh();
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
