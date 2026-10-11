#include "StepGridComponent.h"

namespace gb
{

namespace
{
    /** Extensions the grid accepts when a file is dragged onto a row. */
    bool hasSupportedAudioExtension (const juce::String& path)
    {
        static const juce::StringArray supportedExtensions {
            "wav", "wave", "aif", "aiff", "aifc", "mp3",
            "ogg", "oga", "opus", "m4a", "aac", "flac"
        };

        const auto extension = path.fromLastOccurrenceOf (".", false, false).trim().toLowerCase();
        return supportedExtensions.contains (extension);
    }
} // namespace

//==============================================================================
// Construction

StepGridComponent::StepGridComponent (GrooveboxProcessor& processorToUse)
    : processor (processorToUse)
{
    // paint() fills every pixel, so the component can be opaque.
    setOpaque (true);
    startTimer (refreshIntervalMs);
}

StepGridComponent::~StepGridComponent()
{
    stopTimer();
}

//==============================================================================
// Layout

void StepGridComponent::resized()
{
    const auto bounds = getLocalBounds().reduced (2);

    // The label column shrinks on very narrow windows so the cells keep a chance.
    labelWidth = juce::jmin (labelTargetWidth, juce::jmax (40, bounds.getWidth() / 4));

    labelX    = bounds.getX();
    gridX     = labelX + labelWidth + cellGap;
    gridY     = bounds.getY();
    gridWidth = juce::jmax (0, bounds.getRight() - gridX);
    gridHeight = bounds.getHeight();

    // Equal-width cells and equal-height rows, with a small gap between each.
    cellWidth = juce::jmax (2, (gridWidth - (Layout::numSteps - 1) * cellGap) / Layout::numSteps);
    rowHeight = juce::jmax (2, (gridHeight - (Layout::numChannels - 1) * cellGap) / Layout::numChannels);
}

int StepGridComponent::rowForChannel (int channel) noexcept
{
    return (Layout::numChannels - 1) - channel;
}

int StepGridComponent::channelForRow (int row) noexcept
{
    return (Layout::numChannels - 1) - row;
}

int StepGridComponent::stepX (int step) const noexcept
{
    return gridX + step * (cellWidth + cellGap);
}

int StepGridComponent::rowY (int row) const noexcept
{
    return gridY + row * (rowHeight + cellGap);
}

juce::Rectangle<int> StepGridComponent::getRowBounds (int channel) const
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return {};

    return { gridX, rowY (rowForChannel (channel)), gridWidth, rowHeight };
}

juce::Rectangle<int> StepGridComponent::getLabelBounds (int channel) const
{
    const auto row = getRowBounds (channel);

    if (row.isEmpty())
        return {};

    return { labelX, row.getY(), labelWidth, row.getHeight() };
}

juce::Rectangle<int> StepGridComponent::getFullRowBounds (int channel) const
{
    const auto row = getRowBounds (channel);

    if (row.isEmpty())
        return {};

    // Label column plus every cell, right up to the panel edge: the whole row is
    // the drop target, which matches channelAtY()'s row-based hit testing.
    return { labelX, row.getY(), juce::jmax (0, getWidth() - labelX), row.getHeight() };
}

juce::Rectangle<int> StepGridComponent::getCellBounds (int channel, int step) const
{
    const auto row = getRowBounds (channel);

    if (row.isEmpty() || ! juce::isPositiveAndBelow (step, Layout::numSteps))
        return {};

    return { stepX (step), row.getY(), cellWidth, rowHeight };
}

//==============================================================================
// Hit testing

int StepGridComponent::channelAtY (int y) const
{
    const int rowSpan = rowHeight + cellGap;

    if (rowSpan <= 0)
        return -1;

    const int local = y - gridY;

    if (local < 0)
        return -1;

    const int row = local / rowSpan;

    if (row >= Layout::numChannels)
        return -1;

    // The bottom sliver of a row span is the gap between rows, not a row.
    if (local - row * rowSpan >= rowHeight)
        return -1;

    return channelForRow (row);
}

StepGridComponent::Cell StepGridComponent::cellAt (juce::Point<int> position) const
{
    const int channel = channelAtY (position.y);

    if (channel < 0 || cellWidth <= 0)
        return {};

    const int local = position.x - gridX;

    if (local < 0)                       // label column
        return {};

    const int stepSpan = cellWidth + cellGap;
    const int step = local / stepSpan;

    if (step >= Layout::numSteps)        // right of the last cell
        return {};

    if (local - step * stepSpan >= cellWidth)   // in the gap between two cells
        return {};

    return { channel, step };
}

void StepGridComponent::refresh()
{
    repaint();
}

//==============================================================================
// Painting

void StepGridComponent::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);

    const auto nameFont = juce::Font (juce::FontOptions().withHeight (14.0f));
    const auto dropFont = juce::Font (juce::FontOptions().withHeight (15.0f));

    // getCurrentStep() returns -1 while the transport is stopped.
    const int playStep = processor.getCurrentStep();
    const int gridBottom = gridY + gridHeight;
    const float corner = (float) cornerRadius;

    for (int channel = 0; channel < Layout::numChannels; ++channel)
    {
        const auto channelColour = processor.getChannelColour (channel);
        const auto row = getRowBounds (channel);

        if (row.isEmpty())
            continue;

        // --- Label column: name, plus a glow while the channel is firing. ---
        const auto label = getLabelBounds (channel);

        if (processor.isChannelSounding (channel))
        {
            g.setColour (channelColour.withAlpha (0.18f));
            g.fillRoundedRectangle (label.toFloat(), corner);

            g.setColour (channelColour);
            const float dotSize = 7.0f;
            g.fillEllipse ((float) label.getRight() - 4.0f - dotSize,
                           (float) label.getCentreY() - dotSize * 0.5f,
                           dotSize, dotSize);
        }

        g.setColour (Colours::text);
        g.setFont (nameFont);
        g.drawText (processor.getChannelName (channel),
                    label.withTrimmedRight (16),      // leave room for the activity dot
                    juce::Justification::centredLeft,
                    true);                            // ellipsise long names

        // --- Steps. ---
        for (int step = 0; step < Layout::numSteps; ++step)
        {
            const auto cell = getCellBounds (channel, step).toFloat().reduced (0.5f);

            if (processor.isStepOn (channel, step))
            {
                // Dimmed by velocity, so the dynamics are visible without opening
                // the velocity overlay.
                const auto velocity = juce::jlimit (0.0f, 1.0f, processor.getStepVelocity (channel, step));

                g.setColour (channelColour.withMultipliedAlpha (0.35f + 0.65f * velocity));
                g.fillRoundedRectangle (cell, corner);
            }
            else
            {
                g.setColour (Colours::cellOff);
                g.fillRoundedRectangle (cell, corner);

                g.setColour (Colours::chipBorder);
                g.drawRoundedRectangle (cell, corner, 1.0f);
            }
        }
    }

    // Beat grouping: a stronger edge in the gap to the left of every 4th step.
    // Step 0 is skipped because its edge would fall in the label column.
    for (int step = 4; step < Layout::numSteps; step += 4)
    {
        const int x = stepX (step) - 2;
        g.setColour (Colours::rule.withAlpha (0.6f));
        g.fillRect (x, gridY, 2, gridBottom - gridY);
    }

    // Playhead: a faint wash down the whole column plus a bright outline on its cells.
    // (cellWidth/rowHeight are only valid once resized() has run.)
    if (cellWidth > 0 && rowHeight > 0 && juce::isPositiveAndBelow (playStep, Layout::numSteps))
    {
        g.setColour (Colours::text.withAlpha (0.10f));
        g.fillRect (stepX (playStep), gridY, cellWidth, gridBottom - gridY);

        g.setColour (Colours::text.withAlpha (0.85f));

        // Keep every drawn rectangle positively sized, even for very small cells.
        const float inset = (cellWidth > 4 && rowHeight > 4) ? 1.0f : 0.0f;

        for (int channel = 0; channel < Layout::numChannels; ++channel)
            g.drawRoundedRectangle (getCellBounds (channel, playStep).toFloat().reduced (inset), corner, 2.0f);
    }

    // Drop target: outline and wash the hovered row, then label it.
    if (juce::isPositiveAndBelow (dragHoverChannel, Layout::numChannels))
    {
        const auto row = getFullRowBounds (dragHoverChannel);

        if (! row.isEmpty())
        {
            const auto channelColour = processor.getChannelColour (dragHoverChannel);

            g.setColour (channelColour.withAlpha (0.12f));
            g.fillRoundedRectangle (row.toFloat(), corner);

            g.setColour (channelColour);
            g.drawRoundedRectangle (row.toFloat().reduced (row.getHeight() > 5 ? 1.0f : 0.0f), corner, 2.0f);

            g.setColour (Colours::text);
            g.setFont (dropFont);
            g.drawText ("Drop to load", row, juce::Justification::centred, true);
        }
    }

    // Velocity editing replaces the plain on/off cells with a bar per step.
    if (velocityMode)
        paintVelocityOverlay (g);
}

void StepGridComponent::setVelocityMode (bool shouldEditVelocity)
{
    if (velocityMode == shouldEditVelocity)
        return;

    velocityMode = shouldEditVelocity;

    if (! velocityMode)
    {
        velocityHover = {};
        isDraggingVelocity = false;
    }

    repaint();
}

void StepGridComponent::setVelocityFromPointer (int channel, int step, juce::Point<int> position)
{
    const auto row = getRowBounds (channel);

    if (row.isEmpty())
        return;

    // The value comes from how high the pointer sits in the row, so the bar
    // follows the pointer. Rounded to twentieths so it is easy to land on a value
    // and reads as a tidy number.
    const auto fromBottom = (float) (row.getBottom() - position.y) / (float) juce::jmax (1, row.getHeight());
    const auto rounded = juce::jlimit (0.0f, 1.0f, std::round (fromBottom * 20.0f) / 20.0f);

    if (std::abs (rounded - processor.getStepVelocity (channel, step)) < 0.001f)
        return;

    processor.setStepVelocity (channel, step, rounded);
    repaint();
}

void StepGridComponent::paintVelocityOverlay (juce::Graphics& g)
{
    const auto valueFont = juce::Font (juce::FontOptions (9.0f));

    for (int channel = 0; channel < Layout::numChannels; ++channel)
    {
        for (int step = 0; step < Layout::numSteps; ++step)
        {
            const auto cell = getCellBounds (channel, step);

            if (cell.isEmpty())
                continue;

            const auto velocity = juce::jlimit (0.0f, 1.0f, processor.getStepVelocity (channel, step));
            const auto on = processor.isStepOn (channel, step);
            const auto colour = processor.getChannelColour (channel);

            // A bar rising from the bottom of the cell: its height is the velocity,
            // and it is dimmed while the step is off so an on step still stands out.
            const auto body = cell.toFloat().reduced (1.0f);
            const auto barHeight = juce::jmax (2.0f, body.getHeight() * velocity);
            const auto bar = body.withTop (body.getBottom() - barHeight);

            g.setColour (on ? colour : colour.withAlpha (0.35f));
            g.fillRoundedRectangle (bar, 2.0f);

            g.setColour (on ? Colours::text.withAlpha (0.45f) : Colours::chipBorder);
            g.drawRoundedRectangle (cell.toFloat().reduced (0.5f), (float) cornerRadius * 0.5f, 1.0f);
        }
    }

    // The value under the pointer, so a drag is precise rather than guesswork.
    if (juce::isPositiveAndBelow (velocityHover.channel, Layout::numChannels)
        && juce::isPositiveAndBelow (velocityHover.step, Layout::numSteps))
    {
        const auto cell = getCellBounds (velocityHover.channel, velocityHover.step);

        if (! cell.isEmpty())
        {
            g.setColour (Colours::text);
            g.drawRoundedRectangle (cell.toFloat().reduced (0.5f), (float) cornerRadius * 0.5f, 2.0f);

            const auto value = juce::roundToInt (juce::jlimit (0.0f, 1.0f,
                                 processor.getStepVelocity (velocityHover.channel, velocityHover.step)) * 100.0f);

            const auto label = juce::Rectangle<int> (cell.getX() - 4, cell.getY() - 12,
                                                     juce::jmax (cell.getWidth(), 22), 11);

            g.setColour (Colours::background);
            g.fillRoundedRectangle (label.toFloat(), 2.0f);
            g.setColour (Colours::text);
            g.setFont (valueFont);
            g.drawText (juce::String (value), label, juce::Justification::centred, false);
        }
    }
}

//==============================================================================
// Mouse

void StepGridComponent::mouseDown (const juce::MouseEvent& e)
{
    const auto cell = cellAt (e.getPosition());

    if (cell.channel < 0)               // label column or outside the grid
        return;

    if (velocityMode)
    {
        // A drag edits the one step it started on, so it stays put even if the
        // pointer wanders into a neighbouring cell part way through.
        velocityDragCell = cell;
        isDraggingVelocity = true;
        velocityHover = cell;

        setVelocityFromPointer (cell.channel, cell.step, e.getPosition());
        return;
    }

    // The clicked cell is the anchor: toggling it decides what the rest of the
    // drag paints.
    processor.toggleStep (cell.channel, cell.step);
    dragPaintValue = processor.isStepOn (cell.channel, cell.step);

    for (auto& row : dragVisited)
        row.fill (false);

    dragVisited[(size_t) cell.channel][(size_t) cell.step] = true;   // anchor, painted once
    isDragPainting = true;

    repaint();
}

void StepGridComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (velocityMode)
    {
        if (isDraggingVelocity)
        {
            setVelocityFromPointer (velocityDragCell.channel, velocityDragCell.step, e.getPosition());
            return;
        }

        // Not dragging: just keep the readout under the pointer.
        const auto cell = cellAt (e.getPosition());

        if (cell.channel != velocityHover.channel || cell.step != velocityHover.step)
        {
            velocityHover = cell;
            repaint();
        }

        return;
    }

    if (! isDragPainting)
        return;

    const auto cell = cellAt (e.getPosition());

    if (cell.channel < 0)
        return;

    if (dragVisited[(size_t) cell.channel][(size_t) cell.step])
        return;                          // each cell is painted at most once per drag

    dragVisited[(size_t) cell.channel][(size_t) cell.step] = true;
    processor.setStep (cell.channel, cell.step, dragPaintValue);

    repaint();
}

void StepGridComponent::mouseUp (const juce::MouseEvent&)
{
    isDragPainting = false;
    isDraggingVelocity = false;
}

void StepGridComponent::mouseMove (const juce::MouseEvent& e)
{
    if (! velocityMode)
        return;

    const auto cell = cellAt (e.getPosition());

    if (cell.channel == velocityHover.channel && cell.step == velocityHover.step)
        return;

    velocityHover = cell;
    repaint();
}

void StepGridComponent::mouseExit (const juce::MouseEvent&)
{
    if (! velocityMode)
        return;

    if (velocityHover.channel >= 0)
    {
        velocityHover = {};
        repaint();
    }
}

//==============================================================================
// File drag and drop

bool StepGridComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& path : files)
        if (hasSupportedAudioExtension (path))
            return true;

    return false;
}

void StepGridComponent::fileDragEnter (const juce::StringArray& files, int /*x*/, int y)
{
    if (isInterestedInFileDrag (files))
        setDragHoverChannel (channelAtY (y));
    else
        setDragHoverChannel (-1);       // never keep a stale highlight from an earlier drag
}

void StepGridComponent::fileDragMove (const juce::StringArray& files, int /*x*/, int y)
{
    if (isInterestedInFileDrag (files))
        setDragHoverChannel (channelAtY (y));
}

void StepGridComponent::fileDragExit (const juce::StringArray&)
{
    setDragHoverChannel (-1);
}

void StepGridComponent::filesDropped (const juce::StringArray& files, int /*x*/, int y)
{
    const int targetChannel = channelAtY (y);

    setDragHoverChannel (-1);

    if (targetChannel < 0 || files.isEmpty())
        return;

    juce::String error;
    const bool loaded = processor.loadSampleIntoChannel (targetChannel, juce::File (files[0]), error);

    if (error.isNotEmpty())
    {
        juce::NativeMessageBox::showAsync (juce::MessageBoxOptions()
                                               .withIconType (juce::MessageBoxIconType::WarningIcon)
                                               .withTitle ("Could not load sample")
                                               .withMessage (error),
                                           nullptr);
    }
    else if (loaded && processor.onStateChanged != nullptr)
    {
        processor.onStateChanged();
    }

    repaint();
}

void StepGridComponent::setDragHoverChannel (int channel)
{
    if (channel == dragHoverChannel)
        return;

    dragHoverChannel = channel;
    repaint();
}

//==============================================================================
// Playhead following

void StepGridComponent::timerCallback()
{
    bool needsRepaint = false;

    const int step = processor.getCurrentStep();

    if (step != lastStep)
    {
        lastStep = step;
        needsRepaint = true;
    }

    for (int channel = 0; channel < Layout::numChannels; ++channel)
    {
        const bool sounding = processor.isChannelSounding (channel);

        if (sounding != lastSounding[(size_t) channel])
        {
            lastSounding[(size_t) channel] = sounding;
            needsRepaint = true;
        }
    }

    if (needsRepaint)
        repaint();
}

} // namespace gb
