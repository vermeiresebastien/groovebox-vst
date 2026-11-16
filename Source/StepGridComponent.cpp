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

    /**
        The strike ticks: one for every time this hit actually fires, spread across the
        cell on the axis the grid already uses for time.

        Centred as a group, so the neutral one strike is one tick in the middle rather than
        a tick at one edge with the rest of the cell empty, and eight fill it. The pitch is
        as wide as the cell allows up to the minimum, so the ticks read as a density that
        grows with the count — which is the one thing a roll is.

        Nothing is drawn when `strikePipsFit` says the count cannot be told apart: this is
        called for both views, and a mark that shows four ticks for eight strikes is not a
        shorter version of the truth. The caller decides *whether* to ask at all; this only
        refuses a shape it cannot hold.
    */
    void paintStrikePips (juce::Graphics& g, juce::Rectangle<float> area, int strikes,
                          juce::Colour colour, float height)
    {
        if (strikes <= 0 || height <= 0.0f || ! StepGridComponent::strikePipsFit ((int) area.getWidth(), strikes))
            return;

        const auto inset = (float) StepGridComponent::ratchetPipInset;
        const auto usable = area.getWidth() - inset * 2.0f;
        const auto pitch = usable / (float) strikes;
        const auto halfTick = (float) StepGridComponent::ratchetPipWidth * 0.5f;

        g.setColour (colour);

        for (int strike = 0; strike < strikes; ++strike)
            g.fillRect (juce::Rectangle<float> (area.getX() + inset + pitch * ((float) strike + 0.5f) - halfTick,
                                                area.getY() + inset,
                                                (float) StepGridComponent::ratchetPipWidth,
                                                height));
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
    const auto bounds = getLocalBounds().reduced (gridBorder);

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

//==============================================================================
// Pattern bank: geometry and hit testing

bool StepGridComponent::hasBankStrip (int channel) const noexcept
{
    // Row and column, both, because a control that is drawn but not clickable and one
    // that is clickable but never drawn are the same bug seen from either end.
    return rowHeight >= bankStripHeight + bankNameLineHeight
        && labelWidth >= bankMinimumLabelWidth
        && juce::isPositiveAndBelow (channel, Layout::numChannels);
}

juce::Rectangle<int> StepGridComponent::getBankStripBounds (int channel) const
{
    const auto label = getLabelBounds (channel);

    if (label.isEmpty() || ! hasBankStrip (channel))
        return {};

    return { label.getX(), label.getBottom() - bankStripHeight, label.getWidth(), bankStripHeight };
}

juce::Rectangle<int> StepGridComponent::getSlotDotBounds (int channel, int slot) const
{
    const auto strip = getBankStripBounds (channel);

    if (strip.isEmpty() || ! juce::isPositiveAndBelow (slot, Layout::numPatternSlots))
        return {};

    return { strip.getX() + bankEdgePad + slot * (bankSlotDotSize + bankSlotDotGap),
             strip.getY() + (strip.getHeight() - bankSlotDotSize) / 2,
             bankSlotDotSize, bankSlotDotSize };
}

juce::Rectangle<int> StepGridComponent::getSlotCopyBounds (int channel) const
{
    const auto strip = getBankStripBounds (channel);

    if (strip.isEmpty())
        return {};

    return { strip.getX() + bankEdgePad + bankDotsWidth + bankGroupGap,
             strip.getY(), bankCopyWidth, strip.getHeight() };
}

juce::Rectangle<int> StepGridComponent::getEvolveChipBounds (int channel) const
{
    const auto strip = getBankStripBounds (channel);

    if (strip.isEmpty())
        return {};

    // The chip takes whatever the label column has left, down to the width that made
    // the strip worth drawing at all -- so widening the window widens the word, not the
    // gap before it.
    return { strip.getX() + bankContentLeftPad,
             strip.getY() + 1,
             strip.getWidth() - bankContentLeftPad - bankEdgePad,
             strip.getHeight() - 2 };
}

StepGridComponent::BankTarget StepGridComponent::bankTargetAt (juce::Point<int> position) const
{
    // The row comes from the grid's own row maths, so a click in the gap between two
    // rows belongs to neither lane's strip.
    const auto channel = channelAtY (position.y);

    if (channel < 0 || ! getBankStripBounds (channel).contains (position))
        return {};

    for (int slot = 0; slot < Layout::numPatternSlots; ++slot)
        if (getSlotDotBounds (channel, slot).contains (position))
            return { BankControl::slot, channel, slot };

    if (getSlotCopyBounds (channel).contains (position))
        return { BankControl::copy, channel, processor.getChannelSlot (channel) };

    if (getEvolveChipBounds (channel).contains (position))
        return { BankControl::evolve, channel, processor.getChannelEvolveBars (channel) };

    return {};
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

        // The bank strip is cut off the bottom of the column, so the name and the activity
        // dot share what is left of it. With no strip — a row too short for a name line and
        // a strip both — the name keeps the whole column exactly as it did before the bank
        // existed, which is the case this layout has to stay correct for rather than
        // exercise: nothing in the resize limits produces it, but a component this cheap to
        // ask is not worth a hardcoded 108.
        const auto strip = getBankStripBounds (channel);
        const auto nameLine = strip.isEmpty() ? label : label.withTrimmedBottom (strip.getHeight());

        if (processor.isChannelSounding (channel))
        {
            g.setColour (channelColour.withAlpha (0.18f));
            g.fillRoundedRectangle (label.toFloat(), corner);

            g.setColour (channelColour);
            const float dotSize = 7.0f;
            g.fillEllipse ((float) label.getRight() - 4.0f - dotSize,
                           (float) nameLine.getCentreY() - dotSize * 0.5f,
                           dotSize, dotSize);
        }

        g.setColour (Colours::text);
        g.setFont (nameFont);
        g.drawText (processor.getChannelName (channel),
                    nameLine.withTrimmedRight (16),   // leave room for the activity dot
                    juce::Justification::centredLeft,
                    true);                            // ellipsise long names

        paintBankStrip (g, channel);

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

                // And the fourth: how many times this one hit fires. Cut out of the cell in
                // the panel's own background, the same trick the notch uses to read on a lane
                // whose colour is not known here -- and along the top edge, because the
                // bottom-right belongs to the notch and the time axis belongs to the ticks.
                if (const auto strikes = restingStrikeMarks (channel, step))
                    paintStrikePips (g, cell, strikes, Colours::background,
                                     (float) ratchetPipMarkHeight);
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

int StepGridComponent::restingStrikeMarks (int channel, int step) const
{
    // The mark is for the resting grid only, exactly as the notch is: with an overlay open
    // the count is the whole subject of the cell and is already drawn, per cell, in colour.
    if (overlay != Overlay::none)
        return 0;

    // A step that does not fire has no strikes to show. The engine only ever spreads a hit
    // that happens, so a count on an off cell would be a promise about silence.
    if (! processor.isStepOn (channel, step))
        return 0;

    const auto strikes = processor.getStepRatchet (channel, step);

    // One strike is the neutral value and every cell of a new session carries it. Marking it
    // would mark the grid.
    if (strikes <= 1)
        return 0;

    return strikePipsFit (getCellBounds (channel, step).getWidth(), strikes) ? strikes : 0;
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

void StepGridComponent::setRatchetMode (bool shouldEditRatchet)
{
    openOverlay (shouldEditRatchet ? Overlay::ratchet : Overlay::none);
}

void StepGridComponent::openOverlay (Overlay wanted)
{
    if (overlay == wanted)
        return;

    // One overlay at a time, and closing means closing whichever was open: the three
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
    switch (overlay)
    {
        case Overlay::ratchet: return strikesToAmount (processor.getStepRatchet (channel, step));
        case Overlay::chance:  return processor.getStepChance (channel, step);
        case Overlay::velocity:
        case Overlay::none:    break;   // the level, below
    }

    return processor.getStepVelocity (channel, step);
}

void StepGridComponent::setValue (int channel, int step, float value)
{
    switch (overlay)
    {
        case Overlay::ratchet: processor.setStepRatchet (channel, step, amountToStrikes (value)); return;
        case Overlay::chance:  processor.setStepChance (channel, step, value);                     return;
        case Overlay::velocity:
        case Overlay::none:    processor.setStepVelocity (channel, step, value);                   return;
    }
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
    if (std::abs (rounded - valueOf (channel, step)) < valueDeadBand())
        return;

    setValue (channel, step, rounded);
    repaint();
}

juce::Rectangle<int> StepGridComponent::valueReadoutBoundsFor (int channel, int step) const
{
    const auto cell = getCellBounds (channel, step);

    if (cell.isEmpty())
        return {};

    const auto above = juce::Rectangle<int> (cell.getX() - 4, cell.getY() - 12,
                                             juce::jmax (cell.getWidth(), 22), 11);

    // The whole rule: the readout may leave its cell, but not the component, and it
    // slides rather than being squashed -- a 1 px tall number is as unreadable as a
    // clipped one, and JUCE clips drawing at the component's bounds without complaining.
    //
    // Sliding is `jlimit` on the far corner, not a clamp of each edge. An edge-by-edge
    // clamp is what a rect of unknown size wants, and here the size is the point: the
    // first version of this moved the top-left and let the bottom-right fall where it
    // liked, which produced a rectangle 1 px tall for the top row and still read as a
    // pass because the check below only asked about the top edge.
    const auto bounds = getLocalBounds();

    return above.withPosition (juce::jlimit (bounds.getX(), juce::jmax (bounds.getX(), bounds.getRight() - above.getWidth()),
                                             above.getX ()),
                               juce::jlimit (bounds.getY(), juce::jmax (bounds.getY(), bounds.getBottom() - above.getHeight()),
                                             above.getY()));
}

void StepGridComponent::paintValueOverlay (juce::Graphics& g)
{
    const auto valueFont = juce::Font (juce::FontOptions (9.0f));
    const auto editingChance = overlay == Overlay::chance;
    const auto editingRatchet = overlay == Overlay::ratchet;

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

            if (editingRatchet)
            {
                // A roll has no amount to draw as a bar: it is a number of hits, and the
                // honest picture of that is one tick per hit on the axis the rest of the
                // grid already reads as time. Full height, because with this overlay open
                // nothing else is in the cell.
                //
                // The recess is not decoration. Under the other two overlays the *shape*
                // carries the number — a bar whose height is the amount — so a bar that
                // happens to be the same colour as the cell it lies on still reads, and at
                // full velocity that is exactly what happens, since both are the lane
                // colour. Here the count is the shape, and a tick the colour of the cell it
                // is drawn on is not a tick: without this, every cell at the velocity a
                // fresh session draws by default shows nothing at all in the overlay that
                // exists to show its roll. So the cell is pushed down with the panel's own
                // background first, which is the same cut the resting mark is drawn with,
                // and the ticks then sit above their own cell rather than inside it.
                g.setColour (Colours::background.withAlpha (0.62f));
                g.fillRoundedRectangle (cell.toFloat().reduced (1.5f), (float) cornerRadius * 0.5f);

                paintStrikePips (g, cell.toFloat(), amountToStrikes (value),
                                 on ? colour : colour.withAlpha (0.35f),
                                 (float) juce::jmax (2, cell.getHeight() - 2 * ratchetPipInset));

                g.setColour (on ? Colours::text.withAlpha (0.45f) : Colours::chipBorder);
                g.drawRoundedRectangle (cell.toFloat().reduced (0.5f), (float) cornerRadius * 0.5f, 1.0f);
                continue;
            }

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
        const auto label = valueReadoutBoundsFor (valueHover.channel, valueHover.step);

        if (! cell.isEmpty() && ! label.isEmpty())
        {
            g.setColour (Colours::text);
            g.drawRoundedRectangle (cell.toFloat().reduced (0.5f), (float) cornerRadius * 0.5f, 2.0f);

            // The roll counts things, so the readout prints the count: "3" over a cell that
            // fires three times, not "36" -- a percentage of a whole that does not exist, and
            // the one place this overlay would otherwise lie about what it is editing.
            const auto hovered = valueOf (valueHover.channel, valueHover.step);
            const auto readout = editingRatchet ? juce::String (amountToStrikes (hovered))
                                                : juce::String (juce::roundToInt (juce::jlimit (0.0f, 1.0f,
                                                                                   hovered) * 100.0f));

            g.setColour (Colours::background);
            g.fillRoundedRectangle (label.toFloat(), 2.0f);
            g.setColour (Colours::text);
            g.setFont (valueFont);
            g.drawText (readout, label, juce::Justification::centred, false);
        }
    }
}

//==============================================================================
// Pattern bank painting

void StepGridComponent::paintBankStrip (juce::Graphics& g, int channel)
{
    const auto strip = getBankStripBounds (channel);

    if (strip.isEmpty())
        return;

    const auto colour = processor.getChannelColour (channel);
    const auto selected = processor.getChannelSlot (channel);
    const auto playing = processor.getChannelSlotPlaying (channel);
    const auto period = processor.getChannelEvolveBars (channel);

    /**
        The four marks, left to right.

        Filled and empty are drawn as *different shapes* rather than two alphas of one,
        because the distinction the user is being asked to make is "does this slot hold a
        record" — a thing they cannot see any other way without pointing the lane at it —
        and a dim version of a colour is the reading that survives least well against a
        lane whose own colour is already pale. The mark's size is the same either way, so
        the four of them stay one row of four boxes whatever they hold. It is the engine's
        answer, so the mark, the rotation and the copy target cannot disagree; see
        `SequencerEngine::isSlotFilled` for why a row of off steps with quiet levels counts.
    */
    for (int slot = 0; slot < Layout::numPatternSlots; ++slot)
    {
        const auto dot = getSlotDotBounds (channel, slot).toFloat().reduced (0.5f);

        if (processor.isChannelSlotFilled (channel, slot))
        {
            g.setColour (colour.withMultipliedAlpha (0.75f));
            g.fillRoundedRectangle (dot, 2.0f);
        }
        else
        {
            g.setColour (Colours::chipBorder);
            g.drawRoundedRectangle (dot, 2.0f, 1.0f);
        }

        // The ring goes *outside* the mark it names, at one px into the two px gap, so a
        // selected mark keeps all of its fill. Inside would have eaten the very thing the
        // mark is reporting: whether the slot has anything in it.
        if (slot == selected)
        {
            g.setColour (Colours::text);
            g.drawRoundedRectangle (dot.expanded (1.0f), 3.0f, 1.0f);
        }

        // What is actually sounding, when that is not what the cells are editing. The
        // outline stays on the selection — it is the editing pointer, and repainting it
        // every four bars would read as an edit nobody made — so the rotation is a second,
        // quieter mark inside.
        if (slot == playing && playing != selected)
        {
            g.setColour (Colours::text);
            g.fillEllipse (dot.getCentreX() - 1.5f, dot.getCentreY() - 1.5f, 3.0f, 3.0f);
        }
    }

    /**
        The duplicate mark, and whether it can do anything.

        `canCopySomewhere` walks the slots exactly the way
        `GrooveboxProcessor::duplicateChannelRowToNextSlot` does — cyclically, from the
        selected one, skipping what is already filled — because that is the only way the
        flat version of this glyph is a true statement: a click here would find nowhere to
        go. A control painted identically whether or not it has an effect is a control that
        lies, and "all four are full" is otherwise invisible from the row you are looking at.
    */
    auto canCopySomewhere = false;

    for (int offset = 1; offset < Layout::numPatternSlots; ++offset)
        if (! processor.isChannelSlotFilled (channel, (selected + offset) % Layout::numPatternSlots))
        {
            canCopySomewhere = true;
            break;
        }

    const auto copy = getSlotCopyBounds (channel).toFloat();
    const auto markColour = canCopySomewhere ? Colours::text : Colours::chipBorder;

    const juce::Rectangle<float> behind (copy.getX() + 5.0f, copy.getY() + 1.5f, 6.0f, 6.0f);
    const juce::Rectangle<float> before (copy.getX() + 1.5f, copy.getY() + 5.0f, 6.0f, 6.0f);

    g.setColour (markColour.withAlpha (0.55f));
    g.drawRoundedRectangle (behind, 1.0f, 1.0f);

    // The front square occludes the back one, which is what makes two outlines read as
    // "a copy" rather than "two slots".
    g.setColour (Colours::background);
    g.fillRoundedRectangle (before, 1.0f);

    g.setColour (markColour);
    g.drawRoundedRectangle (before, 1.0f, 1.0f);

    /**
        The rotate chip: how many bars this lane holds a slot for.

        Lit and worded rather than only worded, because "off" and "1 bar" are the same
        behaviour seen two ways and a lane that is rotating is a lane whose grid on screen
        is not the grid you are hearing — a fact worth a colour, not just a number.
    */
    const auto chip = getEvolveChipBounds (channel).toFloat().reduced (0.5f);
    const auto rotating = period > 0;

    if (rotating)
    {
        g.setColour (colour.withMultipliedAlpha (0.28f));
        g.fillRoundedRectangle (chip, 2.0f);
    }

    g.setColour (rotating ? colour : Colours::chipBorder);
    g.drawRoundedRectangle (chip, 2.0f, 1.0f);

    g.setColour (rotating ? Colours::text : Colours::text.withAlpha (0.5f));
    g.setFont (juce::Font (juce::FontOptions().withHeight (9.0f)));

    // Ellipsised, because a period a newer build put in the session is not on this
    // build's ladder and may be any width of number: the chip clips itself rather than
    // drawing over the row it belongs to.
    g.drawText (period <= 0 ? "off" : (period == 1 ? "1 bar" : juce::String (period) + " bars"),
                chip, juce::Justification::centred, true);
}

//==============================================================================
// Mouse

void StepGridComponent::mouseDown (const juce::MouseEvent& e)
{
    // The bank answers first, and answers in every mode: it points a lane at a different
    // pattern or changes how often that pointer moves, and neither of those is an edit of
    // the cell under the cursor — so an open velocity overlay is not a reason to ignore it,
    // and the label column is where `cellAt` already gives up.
    if (const auto bank = bankTargetAt (e.getPosition()); bank.control != BankControl::none)
    {
        switch (bank.control)
        {
            case BankControl::slot:
                processor.setChannelSlot (bank.channel, bank.index);
                break;

            // One undo step for what is two writes: the copied slot's record and the
            // selection that lands on it. Left ungrouped, undo would take the copy back
            // and leave the lane pointing at a slot the user never selected.
            case BankControl::copy:
                processor.beginTransaction ("Duplicate row");
                processor.duplicateChannelRowToNextSlot (bank.channel);
                break;

            case BankControl::evolve:
                processor.cycleChannelEvolve (bank.channel);
                break;

            case BankControl::none:
                break;
        }

        // The processor's own onStateChanged reaches refresh() through the editor, but
        // that hook is optional and a headless caller has none — and a click that moves a
        // mark is not finished until the mark has moved.
        repaint();
        return;
    }

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

        processor.beginTransaction (overlay == Overlay::ratchet ? "Edit strike count"
                                       : overlay == Overlay::chance ? "Edit step probability"
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

        // Which slot a lane is sounding is decided on the audio thread from the bar count,
        // and it is not a parameter — so nothing notifies the panel that the lane's centre
        // dot has moved to another mark. Same reason the playhead is watched rather than
        // trusted. With every lane's chip shut the answer cannot change, and 12 atomic
        // loads at 30 ms is nowhere near the cost of finding that out.
        const auto playing = processor.getChannelSlotPlaying (channel);

        if (playing != lastPlaying[(size_t) channel])
        {
            lastPlaying[(size_t) channel] = playing;
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
