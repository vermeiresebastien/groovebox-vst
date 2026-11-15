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

    /**
        The mark on a cell the take can drop: a corner cut off, with its edge lit.

        Both colours belong to the mark rather than to the cell, because the cell is
        already spoken for — its colour is the channel's and its alpha is the velocity.
        A notch alone would have to be dark or light, and the grid's own colour law
        guarantees it is neither at one end of the velocity range: a mutable step can
        sit on a pale lane at full level, where a light dot disappears, or at a quarter
        of it, where a dark one does. The lit hypotenuse against the cut gives the mark
        its own contrast, so it reads whatever is underneath.

        A third of the shorter side, floored at three pixels, because a mark that
        cannot be drawn at all on a tight layout is worse than one that is small.
    */
    void paintMutableMark (juce::Graphics& g, juce::Rectangle<float> cell)
    {
        const auto leg = juce::jmax (3.0f, juce::jmin (cell.getWidth(), cell.getHeight()) / 3.0f);

        juce::Path notch;
        notch.startNewSubPath (cell.getRight(),       cell.getBottom());
        notch.lineTo          (cell.getRight() - leg, cell.getBottom());
        notch.lineTo          (cell.getRight(),       cell.getBottom() - leg);
        notch.closeSubPath();

        g.setColour (Colours::background);
        g.fillPath (notch);

        g.setColour (Colours::text);
        g.drawLine (cell.getRight() - leg, cell.getBottom(),
                    cell.getRight(),       cell.getBottom() - leg, 1.0f);
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

                // The other number this cell carries, reported without opening the
                // overlay that edits it. See isStepMutable for what the mark claims.
                if (isStepMutable (channel, step))
                    paintMutableMark (g, cell);
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

    // Editing a per-step number replaces the plain on/off cells with a bar per step.
    if (overlay != Overlay::none)
        paintValueOverlay (g);
}

bool StepGridComponent::isStepMutable (int channel, int step) const
{
    // The mark is for the resting grid only; see the header.
    if (overlay != Overlay::none)
        return false;

    // An off step is not waiting for the take — it is not in it. Nothing fires, so
    // there is nothing that could be dropped, and the probability it still stores is
    // inert until the step is switched back on.
    if (! processor.isStepOn (channel, step))
        return false;

    // Two ends of one rule. A step at certain is never dropped whatever the knob says;
    // with the knob shut the grid is law, so not even an uncertain step can move.
    return processor.getMutate() > 0.0f
        && processor.getStepChance (channel, step) < 1.0f;
}

void StepGridComponent::setVelocityMode (bool shouldEditVelocity)
{
    openOverlay (shouldEditVelocity ? Overlay::velocity : Overlay::none);
}

void StepGridComponent::setChanceMode (bool shouldEditChance)
{
    openOverlay (shouldEditChance ? Overlay::chance : Overlay::none);
}

void StepGridComponent::openOverlay (Overlay wanted)
{
    if (overlay == wanted)
        return;

    // One overlay at a time, and closing means closing whichever was open: the two
    // toggles that ask for one can disagree about it, and a grid that is editing two
    // numbers at once is a grid that writes a cell nobody meant to write.
    overlay = wanted;

    if (overlay == Overlay::none)
    {
        valueHover = {};
        isDraggingValue = false;
    }

    repaint();
}

float StepGridComponent::valueOf (int channel, int step) const
{
    return overlay == Overlay::chance ? processor.getStepChance (channel, step)
                                      : processor.getStepVelocity (channel, step);
}

void StepGridComponent::setValue (int channel, int step, float value)
{
    if (overlay == Overlay::chance)
        processor.setStepChance (channel, step, value);
    else
        processor.setStepVelocity (channel, step, value);
}

void StepGridComponent::setValueFromPointer (int channel, int step, juce::Point<int> position)
{
    const auto row = getRowBounds (channel);

    if (row.isEmpty())
        return;

    // The value comes from how high the pointer sits in the row, so the bar follows
    // the pointer. Rounded to twentieths so it is easy to land on a value and reads
    // as a tidy number.
    const auto fromBottom = (float) (row.getBottom() - position.y) / (float) juce::jmax (1, row.getHeight());
    const auto rounded = juce::jlimit (0.0f, 1.0f, std::round (fromBottom * 20.0f) / 20.0f);

    /**
        Half a drag step, and not a hair.

        The number that gets stored is the one the processor keeps: a probability is
        snapped again, harder, onto the rungs the saved record can carry, so what comes
        back out of `valueOf` is not the twentieth the pointer is on. A guard tighter
        than that snap would re-push the whole grid on every pixel of a drag that had
        not moved at all — and each of those pushes is a change the undo manager sees.
    */
    if (std::abs (rounded - valueOf (channel, step)) < 0.025f)
        return;

    setValue (channel, step, rounded);
    repaint();
}

void StepGridComponent::paintValueOverlay (juce::Graphics& g)
{
    const auto valueFont = juce::Font (juce::FontOptions (9.0f));
    const auto editingChance = overlay == Overlay::chance;

    for (int channel = 0; channel < Layout::numChannels; ++channel)
    {
        for (int step = 0; step < Layout::numSteps; ++step)
        {
            const auto cell = getCellBounds (channel, step);

            if (cell.isEmpty())
                continue;

            const auto value = juce::jlimit (0.0f, 1.0f, valueOf (channel, step));
            const auto on = processor.isStepOn (channel, step);
            const auto colour = processor.getChannelColour (channel);

            const auto body = cell.toFloat().reduced (1.0f);
            const auto barHeight = juce::jmax (2.0f, body.getHeight() * value);

            /**
                Two directions, because the two numbers say opposite things about the
                same rectangle.

                A velocity is how much of the hit is there, so its bar grows up from
                nothing. A probability is how much of the time the step is left in, so
                its bar hangs from the top and the gap under it is the part that gets
                dropped — which is the one reading that survives looking away from the
                buttons and back.
            */
            const auto bar = editingChance ? body.withBottom (body.getY() + barHeight)
                                           : body.withTop (body.getBottom() - barHeight);

            g.setColour (on ? colour : colour.withAlpha (0.35f));
            g.fillRoundedRectangle (bar, 2.0f);

            g.setColour (on ? Colours::text.withAlpha (0.45f) : Colours::chipBorder);
            g.drawRoundedRectangle (cell.toFloat().reduced (0.5f), (float) cornerRadius * 0.5f, 1.0f);
        }
    }

    // The value under the pointer, so a drag is precise rather than guesswork.
    if (juce::isPositiveAndBelow (valueHover.channel, Layout::numChannels)
        && juce::isPositiveAndBelow (valueHover.step, Layout::numSteps))
    {
        const auto cell = getCellBounds (valueHover.channel, valueHover.step);

        if (! cell.isEmpty())
        {
            g.setColour (Colours::text);
            g.drawRoundedRectangle (cell.toFloat().reduced (0.5f), (float) cornerRadius * 0.5f, 2.0f);

            const auto value = juce::roundToInt (juce::jlimit (0.0f, 1.0f,
                                 valueOf (valueHover.channel, valueHover.step)) * 100.0f);

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

    if (overlay != Overlay::none)
    {
        // A drag edits the one step it started on, so it stays put even if the
        // pointer wanders into a neighbouring cell part way through.
        valueDragCell = cell;
        isDraggingValue = true;
        valueHover = cell;

        processor.beginTransaction (overlay == Overlay::chance ? "Edit step probability"
                                                               : "Edit step velocity");
        setValueFromPointer (cell.channel, cell.step, e.getPosition());
        return;
    }

    // One transaction for the whole gesture, opened here rather than per cell: a
    // drag across a row is a single edit and should undo as one.
    processor.beginTransaction ("Paint steps");

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
    if (overlay != Overlay::none)
    {
        if (isDraggingValue)
        {
            setValueFromPointer (valueDragCell.channel, valueDragCell.step, e.getPosition());
            return;
        }

        // Not dragging: just keep the readout under the pointer.
        const auto cell = cellAt (e.getPosition());

        if (cell.channel != valueHover.channel || cell.step != valueHover.step)
        {
            valueHover = cell;
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
    isDraggingValue = false;
}

void StepGridComponent::mouseMove (const juce::MouseEvent& e)
{
    if (overlay == Overlay::none)
        return;

    const auto cell = cellAt (e.getPosition());

    if (cell.channel == valueHover.channel && cell.step == valueHover.step)
        return;

    valueHover = cell;
    repaint();
}

void StepGridComponent::mouseExit (const juce::MouseEvent&)
{
    if (overlay == Overlay::none)
        return;

    if (valueHover.channel >= 0)
    {
        valueHover = {};
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

    // The variation knob changes what every lit cell is claiming — a mark appears on
    // the steps the machine is now allowed to drop — and nothing else in the panel
    // repaints the grid for it. A drag on the knob does not go through the undo path,
    // and a parameter write does not raise onStateChanged, so at rest this is the only
    // way the grid finds out. Every other number a cell paints from is either written
    // through a path that already calls refresh(), or is the playhead, watched above.
    const auto mutate = processor.getMutate();

    if (mutate != lastMutate)
    {
        lastMutate = mutate;
        needsRepaint = true;
    }

    if (needsRepaint)
        repaint();
}

} // namespace gb
