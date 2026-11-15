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
      - a timer follows the playhead and each channel's activity so the grid stays
        in sync with the engine without repainting while idle.
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

    juce::Rectangle<int> getRowBounds (int channel) const;      // the 16 cells of one channel
    juce::Rectangle<int> getLabelBounds (int channel) const;    // name / activity area
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

    static constexpr int cellGap = 3;
    static constexpr int labelTargetWidth = 108;
    static constexpr int cornerRadius = 4;
    static constexpr int refreshIntervalMs = 30;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StepGridComponent)
};

} // namespace gb
