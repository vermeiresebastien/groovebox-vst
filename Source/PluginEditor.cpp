#include "PluginEditor.h"
#include "LookAndFeel.h"

namespace gb
{

namespace
{
    constexpr int editorWidth = 1240;
    constexpr int editorHeight = 844;

    constexpr int margin = 18;
    constexpr int titleHeight = 34;
    constexpr int transportHeight = 60;
    constexpr int gridHeight = 400;
    // Tall enough for every control in a channel strip: the sound and clear
    // buttons, the pitch row and the drop line under it, the level fader, the four
    // ADSR rows, and the choke box sharing a row with mute. A shorter strip still
    // lays out, but the bottom control gets clipped. The extra height comes out of
    // the grid's slack, not out of the window: at the 844 px minimum the grid still
    // has 405 px, more than the 400 it was designed around.
    constexpr int stripHeight = 285;
    constexpr int gap = 10;
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

    // The velocity toggle lives in the transport bar but changes how the grid
    // behaves, so the editor joins the two.
    transportBar.onVelocityModeChanged = [this] (bool editVelocity)
    {
        stepGrid.setVelocityMode (editVelocity);
    };

    setSize (editorWidth, editorHeight);
    setResizable (true, true);
    setResizeLimits (editorWidth, editorHeight, 2200, 1400);
}

GrooveboxEditor::~GrooveboxEditor()
{
    processor.onStateChanged = nullptr;
    transportBar.onVelocityModeChanged = nullptr;
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

    // The rule under the title, matching the web app's header divider.
    g.setColour (Colours::rule);
    g.fillRect (margin, titleHeight + 8, getWidth() - margin * 2, 1);

    g.setColour (Colours::muted);
    g.setFont (juce::Font (juce::FontOptions (12.0f)));

    // The two things a new session needs to say: how to get a sound in, and what
    // plays it. The note range used to sit in the transport bar, which ran out of
    // room once the master section moved in.
    const auto firstNote = GrooveboxProcessor::getMidiNoteNameForChannel (0);
    const auto lastNote = GrooveboxProcessor::getMidiNoteNameForChannel (Layout::numChannels - 1);

    g.drawText ("Drop a WAV or AIFF on any channel row to load it   |   MIDI notes "
                  + firstNote + "-" + lastNote + " play the channels",
                margin, titleHeight - 2, getWidth() - margin * 2, 16,
                juce::Justification::centredRight);
}

void GrooveboxEditor::resized()
{
    auto area = getLocalBounds().reduced (margin);

    auto titleRow = area.removeFromTop (titleHeight);
    titleLabel.setBounds (titleRow);
    area.removeFromTop (4);

    transportBar.setBounds (area.removeFromTop (transportHeight));
    area.removeFromTop (gap);

    // The channel strips take their full height: the clamp that used to be here
    // (a third of the remaining area) silently squeezed the bottom control out of
    // the strip whenever the window was a little short.
    auto stripRow = area.removeFromBottom (juce::jmin (stripHeight, juce::jmax (0, area.getHeight() - 120)));
    const auto stripWidth = (stripRow.getWidth() - gap * (Layout::numChannels - 1)) / Layout::numChannels;

    for (int channel = 0; channel < channelStrips.size(); ++channel)
    {
        auto bounds = stripRow.removeFromLeft (channel == channelStrips.size() - 1 ? stripRow.getWidth()
                                                                                  : stripWidth);
        channelStrips[channel]->setBounds (bounds);

        if (channel < channelStrips.size() - 1)
            stripRow.removeFromLeft (gap);
    }

    area.removeFromBottom (gap);
    stepGrid.setBounds (area);
}

} // namespace gb
