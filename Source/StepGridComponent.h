#pragma once

#include "Common.h"
#include "PluginProcessor.h"

namespace gb
{

/**
    The drum grid: twelve channel rows of sixteen steps, mirroring the drum page
    of the web app.

    Rows are drawn bottom-up, so channel 11 is the top row and channel 0 is the
    bottom row — the web app reverses its lane list the same way.

    Interaction:
      - clicking a cell toggles it, dragging paints the toggled value across a run;
      - dropping an audio file on a row loads it as that channel's sound;
      - the strip under each lane's name is that lane's pattern bank: which of its four
        slots the cells are editing, a copy of it into the next empty one, and how many
        bars the machine holds a slot for before walking to the next;
      - a timer follows the playhead, each channel's activity and each channel's playing
        slot, so the grid stays in sync with the engine without repainting while idle.
*/
class StepGridComponent : public juce::Component,
                          public juce::FileDragAndDropTarget,
                          private juce::Timer
{
public:
    explicit StepGridComponent (GrooveboxProcessor& processorToUse);
    ~StepGridComponent() override;

    //==========================================================================
    // Component
    //==========================================================================
    void paint (juce::Graphics& g) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

    //==========================================================================
    // FileDragAndDropTarget
    //==========================================================================
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragMove (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    //==========================================================================
    // Grid
    //==========================================================================

    /** Repaints the grid; call this after the pattern changed elsewhere. */
    void refresh();

    /**
        Switches between toggling steps and editing their velocity.

        In velocity mode a drag inside a cell sets how hard that step hits, drawn
        as a bar rising from the bottom of the cell. The step's on/off state is
        left alone, so a level can be set before a step is switched on, and
        dragging down to nothing never switches the step off.
    */
    void setVelocityMode (bool shouldEditVelocity);
    bool isVelocityMode() const noexcept { return overlay == Overlay::velocity; }

    /**
        The same gesture on the other per-step number: how often the step fires.

        One drag machinery, two fields, because the two are the same interaction on
        the same cell — and because a third set of hover and drag state is how a grid
        ends up with a bar that can be dragging two values at once.
    */
    void setChanceMode (bool shouldEditChance);
    bool isChanceMode() const noexcept { return overlay == Overlay::chance; }

    /** Channel whose row contains `y`, or -1 when y is outside every row. */
    int channelAtY (int y) const;

    /** The rectangle of one cell, in this component's own coordinates.

        Public because a test that aims at a cell has to aim at the real one. The
        layout test used to redo the arithmetic — the border inset, the row gap, the
        reverse row order — and a 17 px change to the strip height put its point in
        the gap under the last row, where a drag does nothing at all.
    */
    juce::Rectangle<int> getCellBounds (int channel, int step) const;

    /** The 16 cells of one channel, and the name column beside them.

        Public for the same reason `getCellBounds` is, and for one more: the bank strip's
        fit is a claim about *these two* numbers — a row tall enough to hold a name line
        and a strip, a column wide enough to hold three controls — so a test that wants to
        say when the strip is allowed to exist has to read the row and the column, not
        infer them from the strip's own rectangle. Inferring it is the gate that asks the
        predicate what it expects.
    */
    juce::Rectangle<int> getRowBounds (int channel) const;
    juce::Rectangle<int> getLabelBounds (int channel) const;

    /**
        Whether the take can drop this step, which is what the corner mark reports.

        Public because the mark is a claim about two numbers and a knob, and a pixel
        check can only prove that *something* was drawn. This is the half that says it
        was drawn for the right reason: a step that fires every time is not mutable, a
        step that is switched off cannot drop, and with the variation shut the grid is
        law, so nothing on it is mutable either — marking a step the machine is not
        allowed to touch would be a warning about a thing that cannot happen.

        Only while no overlay is open, because that is the case the mark exists for.
        With a value bar on the grid the number is already the cell's whole subject,
        and a notch cut into the corner would be eating the bar it shares the cell with.
    */
    bool isStepMutable (int channel, int step) const;

    /**
        Where the number under the pointer is drawn, in this component's own coordinates.

        Public for the same reason `getCellBounds` is: the claim being tested is that the
        readout is *on screen*, and a private rectangle that the painter keeps to itself
        can only be tested by taking a screenshot. The readout sits 12 px above its cell,
        which for the top row is above the component's own top edge at the narrow end of
        the window -- and JUCE clips drawing at the bounds, so the number would lose its
        head rather than complain. The rectangle that comes back here is the one the
        painter uses, already pulled inside, so the test and the pixels cannot drift.
    */
    juce::Rectangle<int> valueReadoutBoundsFor (int channel, int step) const;

    //==========================================================================
    // Pattern bank
    //==========================================================================

    /** Which of the lane's bank controls a pointer is over. */
    enum class BankControl
    {
        none,
        slot,     // one of the four: points the lane's controls at that slot
        copy,     // duplicates this slot into the next empty one
        evolve,   // cycles how many bars the lane holds a slot for
    };

    /** A bank hit: which control, which lane, and for `slot` which one of the four. */
    struct BankTarget
    {
        BankControl control = BankControl::none;
        int channel = -1;
        int index = -1;
    };

    /**
        The whole strip the bank is painted in, for one lane.

        Empty when the row is too short or too narrow for a name line and a strip, which
        is also when none of the three controls below exist: the fit is decided once, here,
        so the painter, the hit test and the test file cannot disagree about whether there
        is a control to click in this row.
    */
    juce::Rectangle<int> getBankStripBounds (int channel) const;

    /** The four slot marks, the copy mark and the rotate chip, left to right. */
    juce::Rectangle<int> getSlotDotBounds (int channel, int slot) const;
    juce::Rectangle<int> getSlotCopyBounds (int channel) const;
    juce::Rectangle<int> getEvolveChipBounds (int channel) const;

    /** What a point in the label column hits, or `none`. Public for the same reason the
        bounds are: a test that aims at a control has to aim at the one the code answers. */
    BankTarget bankTargetAt (juce::Point<int> position) const;

    //==========================================================================
    // Bank strip geometry
    //
    // Public because the layout test has to state the fit boundary and the control
    // sizes in these same numbers. Restated in the test file they become a second copy
    // of one table — and a table written twice is the one that gets missed on the
    // second change, which is the rule behind `Params::globalIds()` carrying its length
    // in its type and the bar sweep using `TransportBar::oneRowWidth` rather than 1774.
    //==========================================================================

    /** The strip's height, and the shortest name line it is allowed to take from a row. */
    static constexpr int bankStripHeight = 13;

    /**
        The shortest name line the grid will give up row height to.

        Fourteen, because the name is drawn at a 14 px font and a line shorter than the
        font ellipsises a name into "Ki…" at the smallest window this editor allows. The
        floor of the window leaves a 27 px row, and 27 - 13 = 14: the strip exists at
        every size the resize limits permit, and this is the number that says so.
    */
    static constexpr int bankNameLineHeight = 14;

    static constexpr int bankEdgePad = 1;
    static constexpr int bankSlotDotSize = 9;
    static constexpr int bankSlotDotGap = 2;
    static constexpr int bankGroupGap = 4;
    static constexpr int bankCopyWidth = 13;
    static constexpr int bankChipMinWidth = 24;   // "8 bars" at a 9 px font, plus its padding

    /** The four marks together. */
    static constexpr int bankDotsWidth = Layout::numPatternSlots * bankSlotDotSize
                                       + (Layout::numPatternSlots - 1) * bankSlotDotGap;

    /** Everything to the left of the chip, including the pad on the far left. */
    static constexpr int bankContentLeftPad = bankEdgePad + bankDotsWidth + bankGroupGap
                                            + bankCopyWidth + bankGroupGap;

    /** The narrowest label column that can hold all three controls at their sizes. */
    static constexpr int bankMinimumLabelWidth = bankContentLeftPad + bankChipMinWidth + bankEdgePad;

private:
    /** A cell address; a channel of -1 means "no cell" (e.g. in the label column). */
    struct Cell
    {
        int channel = -1;
        int step = -1;
    };

    //==========================================================================
    // Timer
    //==========================================================================
    void timerCallback() override;

    Cell cellAt (juce::Point<int> position) const;

    /** Reverse row order: channel 11 is row 0 (top), channel 0 is the last row. */
    static int rowForChannel (int channel) noexcept;
    static int channelForRow (int row) noexcept;

    int stepX (int step) const noexcept;
    int rowY (int row) const noexcept;

    juce::Rectangle<int> getFullRowBounds (int channel) const;  // label + cells, for the drop highlight

    /** Sets the highlighted drop row, repainting only when it actually changed. */
    void setDragHoverChannel (int channel);

    //==========================================================================
    // Per-step value editing
    //
    // One overlay, two fields. Which one is being edited is the only difference, so
    // the drag state, the hover readout and the painting are shared.
    //==========================================================================
    enum class Overlay
    {
        none,
        velocity,   // how hard the step hits, drawn rising from the bottom
        chance,     // how often it fires, drawn falling from the top
    };

    /** Draws the value bars and the number under the pointer, for the active field. */
    void paintValueOverlay (juce::Graphics& g);

    /** Opens one overlay and closes whichever other one was open. */
    void openOverlay (Overlay wanted);

    /** Reads the active field of one cell. */
    float valueOf (int channel, int step) const;

    /** Writes the active field of one cell. */
    void setValue (int channel, int step, float value);

    /** Sets the active field from where the pointer is inside the row. */
    void setValueFromPointer (int channel, int step, juce::Point<int> position);

    Overlay overlay = Overlay::none;

    /** Cell the pointer is over while an overlay is open, or {-1, -1}. */
    Cell valueHover;

    /** Cell the drag started in, so the drag stays on one step. */
    Cell valueDragCell;

    bool isDraggingValue = false;

    GrooveboxProcessor& processor;

    //==========================================================================
    // Pattern bank strip
    //
    // The three controls live in the label column, in a band cut off the bottom of the
    // row, because the column is the only real estate the grid owns outright: the cells
    // are taken by two overlays and a mutable-mark, the bar is laid out by its own width
    // rule, and the strips by their own heights. Nothing here changes a size any other
    // component computes — which is why `TransportBar::heightForWidth`, the editor's
    // floor and the 116-row parameter table all came through this untouched.
    //
    // Every number below is the width of the part it names, and the two thresholds are
    // sums of them, so the fit test, the painter and the hit test cannot be handed three
    // different answers about whether a control exists in this row.
    //==========================================================================

    /** Draws one lane's strip: four slot marks, the copy mark, the rotate chip. */
    void paintBankStrip (juce::Graphics& g, int channel);

    /**
        Is the strip allowed to exist in a row this shape?

        The single answer behind `getBankStripBounds`: a row shorter than a name line plus
        a strip, or a label column narrower than the three controls, gets no strip — and
        therefore nothing to click either. Asked once, here, rather than written separately
        by the painter and the hit test.
    */
    bool hasBankStrip (int channel) const noexcept;

    //==========================================================================
    // Cached layout, computed in resized() and shared by paint() and hit testing.
    //==========================================================================
    int labelX = 0;
    int labelWidth = 0;
    int gridX = 0;
    int gridY = 0;
    int gridWidth = 0;
    int gridHeight = 0;
    int cellWidth = 0;
    int rowHeight = 0;

    //==========================================================================
    // Drag painting
    //==========================================================================
    bool isDragPainting = false;
    bool dragPaintValue = false;

    /** Cells already touched by the current drag, so a cell is painted once. */
    std::array<std::array<bool, (size_t) Layout::numSteps>, (size_t) Layout::numChannels> dragVisited {};

    /** Row currently under a compatible file drag, or -1 when none. */
    int dragHoverChannel = -1;

    //==========================================================================
    // Change detection for the refresh timer
    //==========================================================================
    int lastStep = -1;
    float lastMutate = -1.0f;
    std::array<bool, (size_t) Layout::numChannels> lastSounding {};

    /**
        The slot each lane was sounding at the last tick.

        Watched because the rotate chip and the outline are claims about a number the
        audio thread publishes and nothing tells the panel about: a lane changing slot
        mid-bar moves a mark in the label column, and no parameter, no undo and no
        `onStateChanged` fires for it. Same reason the playhead is watched.
    */
    std::array<int, (size_t) Layout::numChannels> lastPlaying {};

    static constexpr int cellGap = 3;
    static constexpr int labelTargetWidth = 108;
    static constexpr int cornerRadius = 4;
    static constexpr int refreshIntervalMs = 30;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StepGridComponent)
};

} // namespace gb
