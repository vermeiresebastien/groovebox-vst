#include "PluginEditor.h"
#include "LookAndFeel.h"

namespace gb
{

namespace
{
    /**
        The minimum the transport bar needs to show every control at its own size.

        The bar is one row at the default width, and the alternative to widening it was
        shrinking a fader until it was too narrow to drag or letting the last button fall
        off the right edge — which the layout test reads as a zero-width child, i.e. a
        control that cannot be clicked. The grid takes the extra width as wider cells.

        1678 was the row with two variation faders in it; the 154 that made it 1832 is
        exactly the 96 px track, the 42 px readout and the gaps between, paid for in full
        rather than taken from the swing or the trim. 4.5's fourth bar button cost the row
        another 80 and this came with it, so the default is now `TransportBar::oneRowWidth`
        plus the editor's own margins with not a pixel to spare -- which is the point: a
        narrower default would open a session already wrapped, and the wrap is the thing a
        small window has to earn rather than the state it starts in.

        It is the *default*, not the floor. The floor is `TransportBar::wrappedRowWidth`,
        and below `TransportBar::oneRowWidth` the variation band moves to a second line
        of its own — the row-wrap rule that 5.6 was going to need anyway, taken off the
        editor's floor here rather than leaving a third fader to push the minimum another
        154 px wider. Nothing gets narrower on either line; the bar gets taller, and since
        5.6's height half the bar's second line is paid for by the height floor itself, which
        is priced under `TransportBar::twoRowHeight` rather than the one-line height.
    */
    constexpr int editorWidth = 1890;

    /**
        The window a new session opens at. Unchanged by 5.6's height half, and no longer
        the same number as the floor: the floor is derived below, from what the cheapest
        stage of `Layout::editorPlan` costs under the bar's worst (wrapped) height.
    */
    constexpr int editorHeight = 844;

    // What a `StepGridComponent` has to be handed to keep the bank strip's name line
    // readable, asked of the grid rather than remembered here: 361 px at the row height
    // `StepGridComponent::minimumRowHeight` derives from its own two numbers.
    constexpr int gridFloor = StepGridComponent::heightForRowAtLeast (StepGridComponent::minimumRowHeight);
}

GrooveboxEditor::GrooveboxEditor (GrooveboxProcessor& p)
    : juce::AudioProcessorEditor (&p),
      processor (p),
      transportBar (p),
      stepGrid (p)
{
    lookAndFeel = std::make_unique<GrooveboxLookAndFeel>();
    setLookAndFeel (lookAndFeel.get());

    titleLabel.setText ("GROOVEBOX", juce::dontSendNotification);
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    titleLabel.setColour (juce::Label::textColourId, Colours::text);
    titleLabel.setFont (juce::Font (juce::FontOptions (22.0f)).boldened());
    addAndMakeVisible (titleLabel);

    addAndMakeVisible (stepGrid);
    addAndMakeVisible (transportBar);

    for (int channel = 0; channel < Layout::numChannels; ++channel)
        addAndMakeVisible (channelStrips.add (new ChannelStrip (p, channel)));

    // The grid owns the pattern, so let it know when something else changed it.
    processor.onStateChanged = [this]
    {
        refreshAll();
    };

    // The editing toggles live in the transport bar but change how the grid behaves,
    // so the editor joins them. Each one only ever opens its own overlay: the bar has
    // already put the others away by the time these fire, and a grid that could be told
    // to edit two numbers at once would need a rule to say which one a drag writes.
    transportBar.onVelocityModeChanged = [this] (bool editVelocity)
    {
        stepGrid.setVelocityMode (editVelocity);
    };

    transportBar.onChanceModeChanged = [this] (bool editChance)
    {
        stepGrid.setChanceMode (editChance);
    };

    transportBar.onRatchetModeChanged = [this] (bool editRatchet)
    {
        stepGrid.setRatchetMode (editRatchet);
    };

    setSize (editorWidth, editorHeight);
    setResizable (true, true);

    // The width floor is the *wrapped* row, not the one-line row: below
    // `TransportBar::oneRowWidth` the variation band moves down a line rather than
    // anything on either line losing width, so the smallest window that can still show
    // every control at its own size is the first line on its own -- 1262 px of bar plus
    // this editor's margins, which is what a 1366 px laptop has to work with. The 80 that
    // joined them is 4.5's ratchet button, and it moves the floor because the wrap lifts a
    // whole band off the *first* line: a control added to the row that stays is a control
    // the minimum has to pay for, whatever the band that leaves is worth.
    //
    // The height floor is that same worst case priced one level up. A window at the width
    // floor has a two-line bar, so the minimum height has to be paid with 96 px of bar,
    // not the 60 the default width gets: `editorFloorHeight` is stage 3 of the plan under
    // `TransportBar::twoRowHeight`, which is 695 px -- the strip at its captions-gone
    // budget, the grid at `gridFloor` and no wordmark row. Every size in between is the
    // plan's to decide, and none of them costs a control: what the four stages give up is
    // the GROOVEBOX row (mark, hint and rule, all three drawn inside it), then the strip's
    // slack, its row heights and its captions, in that order.
    setResizeLimits (TransportBar::wrappedRowWidth + Layout::editorMargin * 2,
                     Layout::editorFloorHeight (TransportBar::twoRowHeight, gridFloor),
                     2200, 1400);
}

GrooveboxEditor::~GrooveboxEditor()
{
    processor.onStateChanged = nullptr;
    transportBar.onVelocityModeChanged = nullptr;
    transportBar.onChanceModeChanged = nullptr;
    transportBar.onRatchetModeChanged = nullptr;
    setLookAndFeel (nullptr);
}

void GrooveboxEditor::refreshAll()
{
    stepGrid.refresh();

    for (auto* strip : channelStrips)
        strip->refresh();

    transportBar.refresh();
    repaint();
}

bool GrooveboxEditor::keyPressed (const juce::KeyPress& key)
{
    /**
        The keys are matched by hand rather than through an
        `ApplicationCommandTarget` and a `CommandManager`, because a plugin does not
        own the host's command manager: registering a second one to answer two
        shortcuts would put this editor in front of the host's own keys, and the
        house rule is that a plugin stays out of the host's business.

        `isCommandDown` is the right test on both platforms — it is Cmd on macOS and
        Ctrl on Windows, where the two flags are the same bit.

        One press takes back one gesture, not one cell, because that is what the
        pointer did: the grid opens a transaction when the button goes down and
        closes it when it comes up, so a drag that painted six steps undoes as six.
    */
    const auto mods = key.getModifiers();

    if (! mods.isCommandDown())
        return false;

    // A held Shift makes the character itself uppercase, so both spellings count.
    switch (key.getKeyCode())
    {
        case 'z':
        case 'Z':
            if (mods.isShiftDown())
                return undoOrRedo (false);

            return undoOrRedo (true);

        case 'y':
        case 'Y':
            // Windows' redo, and the only meaning Shift has on this one.
            return undoOrRedo (false);

        default:
            return false;
    }
}

bool GrooveboxEditor::undoOrRedo (bool undo)
{
    auto& manager = processor.getUndoManager();
    const auto didSomething = undo ? manager.undo() : manager.redo();

    // The pattern refreshes itself through `onStateChanged` when the undo moves the
    // property, but an undo of a parameter change does not: the sliders follow their
    // attachments, while the strip and bar that display the same values in their own
    // way have to be told. One refresh covers both, and it is what a user pressing a
    // key expects to see happen immediately.
    if (didSomething)
        refreshAll();

    return didSomething;
}

void GrooveboxEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);

    // Everything in here is the GROOVEBOX row, and the row is stage 0's to keep: the
    // wordmark is branding, the rule is chrome, and the hint is the one-line answer to
    // "how do I get a sound in". Nothing else is drawn at this level, so a short window
    // gives up all three together rather than leaving a rule over an empty band.
    if (! currentPlan().wordmark)
        return;

    // The rule under the title, matching the web app's header divider.
    g.setColour (Colours::rule);
    g.fillRect (Layout::editorMargin, Layout::wordmarkHeight + 8,
                getWidth() - Layout::editorMargin * 2, 1);

    g.setColour (Colours::muted);
    g.setFont (juce::Font (juce::FontOptions (12.0f)));

    // The two things a new session needs to say: how to get a sound in, and what
    // plays it. The note range used to sit in the transport bar, which ran out of
    // room once the master section moved in.
    const auto firstNote = GrooveboxProcessor::getMidiNoteNameForChannel (0);
    const auto lastNote = GrooveboxProcessor::getMidiNoteNameForChannel (Layout::numChannels - 1);

    g.drawText ("Drop a WAV or AIFF on any channel row to load it   |   MIDI notes "
                  + firstNote + "-" + lastNote + " play the channels",
                Layout::editorMargin, Layout::wordmarkHeight - 2,
                getWidth() - Layout::editorMargin * 2, 16,
                juce::Justification::centredRight);
}

Layout::EditorPlan GrooveboxEditor::currentPlan() const noexcept
{
    const auto interior = getLocalBounds().reduced (Layout::editorMargin);

    // Both the bar's height and the grid's floor are asked of the components that own
    // those rules, so the editor cannot decide a budget the bar or the grid then spends
    // differently -- which is the trap AGENTS.md records for a gate that called the very
    // predicate it was meant to test.
    return Layout::editorPlan (interior.getHeight(),
                               TransportBar::heightForWidth (interior.getWidth()),
                               gridFloor);
}

void GrooveboxEditor::resized()
{
    auto area = getLocalBounds().reduced (Layout::editorMargin);
    const auto plan = currentPlan();

    auto titleRow = area.removeFromTop (plan.wordmark ? Layout::wordmarkHeight : 0);
    titleLabel.setVisible (plan.wordmark);
    titleLabel.setBounds (titleRow);
    area.removeFromTop (Layout::editorTitleAir);

    // One line or two, asked of the bar itself with the width it is about to be given:
    // the bar's own numbers are component widths, and the margin here is not part of
    // them. Getting that backwards is how a bar ends up laying out two lines inside a
    // band the editor paid for as one.
    transportBar.setBounds (area.removeFromTop (TransportBar::heightForWidth (area.getWidth())));
    area.removeFromTop (Layout::editorGap);

    // The strip gets what its own stage of `Layout::stripPlan` costs, and never more than
    // what is left once the grid has been promised `Layout::gridGuardHeight`. The clamp
    // used to be the only thing deciding the strip's height, and the 120 was typed: below
    // it the strip went on shrinking until a control lost its rectangle, with the slack
    // spent on a grid that could not use it.
    auto stripRow = area.removeFromBottom (juce::jmin (plan.strip,
                                                        juce::jmax (0, area.getHeight() - Layout::gridGuardHeight)));
    const auto stripWidth = (stripRow.getWidth() - Layout::editorGap * (Layout::numChannels - 1)) / Layout::numChannels;

    for (int channel = 0; channel < channelStrips.size(); ++channel)
    {
        auto bounds = stripRow.removeFromLeft (channel == channelStrips.size() - 1 ? stripRow.getWidth()
                                                                                  : stripWidth);
        channelStrips[channel]->setBounds (bounds);

        if (channel < channelStrips.size() - 1)
            stripRow.removeFromLeft (Layout::editorGap);
    }

    area.removeFromBottom (Layout::editorGap);
    stepGrid.setBounds (area);
}

} // namespace gb
