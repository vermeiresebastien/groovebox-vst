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

        The toggles are mutually exclusive *here*, in the bar that owns them: the
        grid can only edit one number per cell at a time, so a bar that let two stay
        lit would be showing a mode it is not in, and the drag would write the field the
        dimmer button says is off.
    */
    std::function<void (bool)> onChanceModeChanged;

    /**
        Called when the ratchet toggle changes, so the editor can tell the grid.

        The third of three, and the reason the exclusion below is written as a rule rather
        than as one call per handler: the grid edits one number per cell at a time, so with
        two of these lit the panel is showing a mode it is not in, and with three the same
        lie is available in three directions.
    */
    std::function<void (bool)> onRatchetModeChanged;

    void paint (juce::Graphics& g) override;
    void resized() override;

    /**
        The bar's own layout rule, in the open because the editor has to pay for the
        second line in height, and because "which control gave up width" is exactly the
        question this bar answers by refusing to ask it.

        One line needs `oneRowWidth` and is `oneRowHeight` tall. Below that width the
        Variation group -- the widest band on the bar, and the one nobody is dragging
        while watching the grid -- moves to a second line instead of anything getting
        narrower, and the bar becomes `twoRowHeight` tall. That is what lets the editor's
        floor come down to `wrappedRowWidth`, which is the first line with the variation
        band lifted out of it: nothing on either line is squeezed, and a 1366 px laptop
        can now hold the whole machine.

        The three numbers are deliberately typed once, here, and read by the layout test
        at each side of the boundary -- a width the row was measured at is worth nothing
        if nothing checks that the row still fits at it.

        They are widths of *this component*, not of the window: the editor has a margin
        of its own, and mixing the two is how a bar ends up laying out two lines inside a
        band the editor paid for as one. The editor asks `heightForWidth` with the width
        it is about to hand over, and adds its own margin to `wrappedRowWidth` to get its
        floor.
    */
    static constexpr int variationBandWidth = 584 + 8;  // the group, plus the gap after it
    /**
        Every band, on one line, measured as the sum of what `resized()` takes.

        It moved from 1774 to 1854 when the ratchet button joined the two overlays, and
        nothing else on the row gave way: the rule the comment above describes is that a
        band that runs out of room goes to the second line rather than anything getting
        narrower, and a fourth control in a row with no slack in it is exactly the case
        that rule was written for. The wrapped first line therefore grew by the same 80 px
        and took the editor's floor with it -- see `PluginEditor.cpp`, which derives both
        numbers from these rather than remembering either.
    */
    static constexpr int oneRowWidth = 1854;
    static constexpr int wrappedRowWidth = oneRowWidth - variationBandWidth;
    static constexpr int oneRowHeight = 60;
    static constexpr int twoRowHeight = 96;             // two 38 px lines, their air and the rule

    static bool isWrappedForWidth (int width) noexcept
    {
        return width < oneRowWidth;
    }

    static int heightForWidth (int width) noexcept
    {
        return isWrappedForWidth (width) ? twoRowHeight : oneRowHeight;
    }

private:
    void timerCallback() override;

    /** The three faders, Reroll and the seed, laid out into whatever band they were given. */
    void layoutVariation (juce::Rectangle<int> band);

    void handlePlayClicked();
    void handleTempoMinus();
    void handleTempoPlus();
    void handleClearPatternClicked();
    void handleVelocityClicked();
    void handleChanceClicked();
    void handleRatchetClicked();

    /** Puts every step's *current* overlay value back to its neutral one. */
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

    /**
        The third number each step carries: how many strikes one hit is spread into.

        "Ratchet" on the button rather than "Roll", which is the word the plan used: this
        bar already carries a **Reroll** button that changes a seed, and the engine already
        has a function that rolls the probability dice, so a third meaning for "roll" on
        the same panel is a name two controls share -- the same trap the layout test fell
        into when it told two faders apart by their range.
    */
    juce::TextButton ratchetButton { "Ratchet" };

    /** Resets whichever of the three numbers above the grid is currently being edited. */
    juce::TextButton resetValuesButton { "100%" };

    /** How much of the variation the machine is allowed to use, and which take it is. */
    juce::Slider mutateSlider;
    juce::Label mutateValueLabel;
    juce::Slider humanizeSlider;
    juce::Label humanizeValueLabel;
    juce::Slider touchSlider;
    juce::Label touchValueLabel;
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
