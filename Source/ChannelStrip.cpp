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
    clearRowButton.setTooltip ("Remove every step on this channel");
    addAndMakeVisible (clearRowButton);

    pitchSlider.setSliderStyle (juce::Slider::IncDecButtons);
    pitchSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 44, 18);
    pitchSlider.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
    pitchSlider.setRange (-36.0, 12.0, 1.0);
    pitchSlider.setDoubleClickReturnValue (true, 0.0);
    pitchSlider.setTooltip ("Pitch offset in semitones (-36 to +12)");
    addAndMakeVisible (pitchSlider);

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

    pitchSlider.onValueChange = [this]
    {
        if (editingPitch)
            return;

        processor.beginTransaction ("Change channel pitch");
        processor.setChannelPitch (channel, (int) pitchSlider.getValue());
        refresh();
    };

    pitchSlider.onDragStart = [this] { editingPitch = true; };
    pitchSlider.onDragEnd   = [this] { editingPitch = false; };

    levelSlider.onValueChange = [this]
    {
        if (editingLevel)
            return;

        processor.setChannelLevel (channel, (float) levelSlider.getValue());
    };

    levelSlider.onDragStart = [this] { editingLevel = true; };
    levelSlider.onDragEnd   = [this] { editingLevel = false; };

    muteButton.onClick = [this] { processor.setChannelMuted (channel, muteButton.getToggleState()); };

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

    // Never fight a control the user is currently holding.
    if (! editingPitch)
        pitchSlider.setValue ((double) processor.getChannelPitch (channel), juce::dontSendNotification);

    if (! editingLevel)
        levelSlider.setValue ((double) processor.getChannelLevel (channel), juce::dontSendNotification);

    muteButton.setToggleState (processor.isChannelMuted (channel), juce::dontSendNotification);

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

    auto header = area.removeFromTop (headerHeight);
    colourButton.setBounds (header.removeFromLeft (16).withSizeKeepingCentre (14, 14));
    header.removeFromLeft (4);
    soundingDotArea = header.removeFromRight (8).withSizeKeepingCentre (6, 6);
    nameLabel.setBounds (header);

    area.removeFromTop (gapSize);
    soundButton.setBounds (area.removeFromTop (soundHeight));
    area.removeFromTop (2);
    clearRowButton.setBounds (area.removeFromTop (clearRowHeight));

    area.removeFromTop (gapSize);
    pitchCaptionArea = area.removeFromTop (captionHeight); // drawn in paint()
    auto pitchRow = area.removeFromTop (pitchRowHeight);
    pitchDown12Button.setBounds (pitchRow.removeFromLeft (24));
    pitchRow.removeFromLeft (2);
    pitchDownButton.setBounds (pitchRow.removeFromLeft (18));
    pitchRow.removeFromLeft (2);
    pitchUpButton.setBounds (pitchRow.removeFromRight (18));
    pitchRow.removeFromRight (2);
    pitchUp12Button.setBounds (pitchRow.removeFromRight (24));
    pitchRow.removeFromLeft (2);
    pitchRow.removeFromRight (2);
    pitchSlider.setBounds (pitchRow);

    area.removeFromTop (gapSize);
    levelCaptionArea = area.removeFromTop (captionHeight); // drawn in paint()
    levelSlider.setBounds (area.removeFromTop (levelHeight));

    area.removeFromTop (gapSize);
    muteButton.setBounds (area.removeFromTop (muteHeight));
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

void ChannelStrip::nudgePitch (int delta)
{
    processor.beginTransaction ("Nudge channel pitch");
    processor.nudgeChannelPitch (channel, delta);
    refresh();
}

} // namespace gb
