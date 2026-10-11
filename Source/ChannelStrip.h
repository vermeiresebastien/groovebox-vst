#pragma once

#include "Common.h"
#include "PluginProcessor.h"
#include "EnvelopeControls.h"

namespace gb
{

/**
    Shows and edits a single channel: name, colour, sample, pitch, level and mute.

    Every control is filled in from the processor by `refresh()`, which runs
    before any callback is attached (see the constructor), so refreshing the
    strip can never feed a value back into the processor.
*/
class ChannelStrip : public juce::Component,
                     private juce::Timer
{
public:
    ChannelStrip (GrooveboxProcessor& processorToUse, int channelIndexToUse);
    ~ChannelStrip() override;

    /** Pulls every displayed value back out of the processor. Safe to call at
        any time; callbacks are already attached by then, but each one is
        guarded so a programmatic update is not written back out again. */
    void refresh();

    /** Re-reads only the channel name (used when a rename is cancelled). */
    void refreshName();

    /** Room left at the bottom after the last control, for the layout test. */
    int getSpareHeight() const noexcept { return lastSpareHeight; }

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;

    void handleNameChanged();
    void handleColourClicked();
    void handleSoundClicked();
    void handleClearRowClicked();
    void handleShiftRow (int steps);
    void nudgePitch (int delta);
    void paintCaption (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area) const;

    GrooveboxProcessor& processor;

    // Clamped to the processor's channel range in the constructor.
    int channel = 0;

    juce::Label nameLabel;
    juce::TextButton colourButton;
    juce::TextButton soundButton;
    juce::TextButton clearRowButton;

    /** Rotate this row by one step, carrying each step's velocity with it. */
    juce::TextButton shiftLeftButton { juce::CharPointer_UTF8 ("\xe2\x86\x90") };
    juce::TextButton shiftRightButton { juce::CharPointer_UTF8 ("\xe2\x86\x92") };

    juce::TextButton pitchDown12Button { "-12" };
    juce::TextButton pitchDownButton { "-" };
    juce::Label pitchValueLabel;
    juce::TextButton pitchUpButton { "+" };
    juce::TextButton pitchUp12Button { "+12" };

    juce::Slider levelSlider;
    juce::TextButton muteButton { "Mute" };

    EnvelopeControls envelope;

    /** Reads one envelope value back out of the processor. */
    float envelopeValueFromProcessor (EnvelopeStage stage) const;

    // Kept alive for the lifetime of the async dialog.
    std::unique_ptr<juce::FileChooser> chooser;

    // Used to build the "all audio files" wildcard for the load dialog.
    juce::AudioFormatManager formatManager;

    // Caption strips filled in by resized() and drawn by paint().
    juce::Rectangle<int> pitchCaptionArea;
    juce::Rectangle<int> levelCaptionArea;
    juce::Rectangle<int> envelopeCaptionArea;
    juce::Rectangle<int> soundingDotArea;

    // True while the channel is firing, so the timer only repaints on a change.
    bool isSounding = false;

    int lastSpareHeight = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
};

} // namespace gb
