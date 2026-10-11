#pragma once

#include "Common.h"
#include "PluginProcessor.h"

namespace gb
{

/**
    The bar above the grid: play/stop, tempo, swing and clear pattern.

    In a plugin build the host owns transport and tempo, so the play button and
    the tempo steppers are disabled and a "Host transport" caption is shown
    instead. `refresh()` applies that state and is also called from a timer so
    host tempo changes show up while the editor is open.
*/
class TransportBar : public juce::Component,
                     private juce::Timer
{
public:
    explicit TransportBar (GrooveboxProcessor& processorToUse);
    ~TransportBar() override;

    /** Re-reads tempo, transport state and swing settings. Safe to call any time. */
    void refresh();

    /** Called when the velocity toggle changes, so the editor can tell the grid. */
    std::function<void (bool)> onVelocityModeChanged;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;

    void handlePlayClicked();
    void handleTempoMinus();
    void handleTempoPlus();
    void handleClearPatternClicked();
    void handleVelocityClicked();
    void handleResetVelocitiesClicked();
    void paintCaption (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area) const;

    static constexpr int swingRadioGroupId = 1;

    GrooveboxProcessor& processor;

    juce::TextButton playButton { "Play" };
    juce::Label hostCaption;
    juce::Label midiHintLabel;
    juce::TextButton tempoDownButton { "-" };
    juce::Label tempoLabel;
    juce::TextButton tempoUpButton { "+" };
    juce::Slider swingSlider;
    juce::Label swingValueLabel;
    juce::TextButton swingEighthButton { "1/8" };   // grid of 4 steps per pair
    juce::TextButton swingSixteenthButton { "1/16" }; // grid of 2 steps per pair
    juce::TextButton clearPatternButton { "Clear pattern" };

    /** Switches the grid into velocity editing; toggles, so it stays lit. */
    juce::TextButton velocityButton { "Velocity" };
    juce::TextButton resetVelocitiesButton { "100%" };

    // Caption strips filled in by resized() and drawn by paint().
    juce::Rectangle<int> tempoCaptionArea;
    juce::Rectangle<int> swingCaptionArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportBar)
};

} // namespace gb
