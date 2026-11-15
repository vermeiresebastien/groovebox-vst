#pragma once

#include "Common.h"
#include "PluginProcessor.h"

namespace gb
{

/**
    The master output meter: one bar per output channel, a true-peak tick on each,
    plus a clip indicator.

    It has its own timer because the transport bar's 200 ms refresh is far too
    slow to catch a drum transient; the peak itself is held on the audio side, so
    all this does is read it and repaint. Clicking clears the clip latch, which
    is the only way the latch goes out.
*/
class MasterMeter : public juce::Component,
                    public juce::SettableTooltipClient,
                    private juce::Timer
{
public:
    explicit MasterMeter (GrooveboxProcessor& processorToUse);
    ~MasterMeter() override;

    /** Bottom and top of the bar's scale, in dBFS. */
    static constexpr float floorDb   = -42.0f;
    static constexpr float ceilingDb = Layout::maxMasterGainDb;

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& event) override;

private:
    void timerCallback() override;

    GrooveboxProcessor& processor;
};

/**
    The bar above the grid: play/stop, tempo, swing, pattern tools and the master.

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

    /**
        Called when the probability toggle changes, so the editor can tell the grid.

        The two toggles are mutually exclusive *here*, in the bar that owns them: the
        grid can only edit one number per cell at a time, so a bar that let both stay
        lit would be showing a mode it is not in, and the drag would write the field the
        dimmer button says is off.
    */
    std::function<void (bool)> onChanceModeChanged;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;

    void handlePlayClicked();
    void handleTempoMinus();
    void handleTempoPlus();
    void handleClearPatternClicked();
    void handleVelocityClicked();
    void handleChanceClicked();

    /** Puts every step's *current* overlay value back to full. */
    void handleResetValuesClicked();

    void handleRerollClicked();

    void paintCaption (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area) const;

    static constexpr int swingRadioGroupId = 1;

    GrooveboxProcessor& processor;

    juce::TextButton playButton { "Play" };
    juce::Label hostCaption;
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

    /** The same, for the probability. */
    juce::TextButton chanceButton { "Chance" };

    /** Resets whichever of the two numbers above the grid is currently being edited. */
    juce::TextButton resetValuesButton { "100%" };

    /** How much of the variation the machine is allowed to use, and which take it is. */
    juce::Slider mutateSlider;
    juce::Label mutateValueLabel;
    juce::TextButton rerollButton { "Reroll" };
    juce::Label seedLabel;

    /** The master trim and its meter, at the right-hand end of the bar. */
    juce::Slider masterSlider;
    juce::Label masterValueLabel;
    MasterMeter masterMeter;

    // Caption strips filled in by resized() and drawn by paint().
    juce::Rectangle<int> tempoCaptionArea;
    juce::Rectangle<int> swingCaptionArea;
    juce::Rectangle<int> variationCaptionArea;
    juce::Rectangle<int> masterCaptionArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportBar)
};

} // namespace gb
