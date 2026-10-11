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
    bool isVelocityMode() const noexcept { return velocityMode; }

    /** Channel whose row contains `y`, or -1 when y is outside every row. */
    int channelAtY (int y) const;

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
    juce::Rectangle<int> getCellBounds (int channel, int step) const;

    /** Sets the highlighted drop row, repainting only when it actually changed. */
    void setDragHoverChannel (int channel);

    //==========================================================================
    // Velocity editing
    //==========================================================================

    /** Draws the velocity bars and the value under the pointer. */
    void paintVelocityOverlay (juce::Graphics& g);

    /** Sets one cell's velocity from where the pointer is inside that cell. */
    void setVelocityFromPointer (int channel, int step, juce::Point<int> position);

    bool velocityMode = false;

    /** Cell the pointer is over in velocity mode, or {-1, -1}. */
    Cell velocityHover;

    /** Cell the velocity drag started in, so the drag stays on one step. */
    Cell velocityDragCell;

    int velocityDragStartY = 0;
    float velocityDragStartValue = 1.0f;
    bool isDraggingVelocity = false;

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
    std::array<bool, (size_t) Layout::numChannels> lastSounding {};

    static constexpr int cellGap = 3;
    static constexpr int labelTargetWidth = 108;
    static constexpr int cornerRadius = 4;
    static constexpr int refreshIntervalMs = 30;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StepGridComponent)
};

} // namespace gb
