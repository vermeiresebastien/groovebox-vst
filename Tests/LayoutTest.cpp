/*
    Editor layout and control-wiring check.

    Two classes of bug live here, and neither is visible to an audio-only test:

      1. A control laid out with no size, or outside its parent, cannot be clicked.
      2. A control that updates the processor only on mouse *release* appears to do
         nothing while you drag it, because the strip's refresh timer repopulates
         the control from the processor and pulls the thumb back.

    So this builds the real editor, reports the bounds of every control, and then
    drives each horizontal fader with synthetic mouse events to confirm the change
    actually reaches the processor.

    Build:  cmake --build build --config Release --target GrooveboxLayoutTest
    Run:    build\GrooveboxLayoutTest_artefacts\Release\GrooveboxLayoutTest.exe
*/

#include "PluginEditor.h"
#include "ChannelStrip.h"
#include "TransportBar.h"
#include "StepGridComponent.h"

#include <cstdio>
#include <vector>

using namespace gb;

namespace
{

int problems = 0;
int checked = 0;

void fail (const juce::String& what)
{
    ++problems;
    std::printf ("  BAD  %s\n", what.toRawUTF8());
}

void pass (const juce::String& what)
{
    std::printf ("  ok   %s\n", what.toRawUTF8());
}

/** Fails when a control cannot be seen or clicked. */
void checkBounds (juce::Component& parent, juce::Component& child, const juce::String& name)
{
    ++checked;

    const auto b = child.getBounds();
    juce::String issue;

    if (b.getWidth() <= 0 || b.getHeight() <= 0)
        issue = "ZERO SIZE";
    else if (b.getY() < 0 || b.getBottom() > parent.getHeight())
        issue = "VERTICALLY OUTSIDE PARENT";
    else if (! parent.getLocalBounds().contains (b))
        issue = "OUTSIDE PARENT";

    std::printf ("  %s %-22s x=%4d y=%4d w=%4d h=%4d%s\n",
                 issue.isEmpty() ? "ok  " : "BAD ",
                 name.toRawUTF8(),
                 b.getX(), b.getY(), b.getWidth(), b.getHeight(),
                 issue.isEmpty() ? "" : ("   <-- " + issue).toRawUTF8());

    if (issue.isNotEmpty())
        ++problems;
}

/**
    A claim with its number next to it, for the checks that are not about one rectangle.

    `checked` is what the pass line counts, so a sweep that ran and printed nothing would
    still be a sweep nobody could tell from one that was skipped -- the same reason the
    audio gates print their precondition.
*/
void expect (bool ok, const juce::String& what, const juce::String& detail)
{
    ++checked;

    if (ok)
        pass (what + " [" + detail + "]");
    else
        fail (what + " [" + detail + "]");
}

/** As `expect`, for a loop where twelve passing lines would bury the section it is in. */
void expectQuiet (bool ok, const juce::String& what, const juce::String& detail)
{
    ++checked;

    if (! ok)
        fail (what + " [" + detail + "]");
}

/** Whether a child lies wholly inside its parent and still has a size of its own. */
bool fitsInside (juce::Component& parent, juce::Component& child)
{
    const auto b = child.getBounds();

    return b.getWidth() > 0 && b.getHeight() > 0
           && b.getY() >= 0 && b.getBottom() <= parent.getHeight()
           && parent.getLocalBounds().contains (b);
}

/** Builds a mouse event in a component's own coordinates. */
juce::MouseEvent makeEvent (juce::Component& target, juce::Point<float> position)
{
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(),
                             position,
                             juce::ModifierKeys::leftButtonModifier,
                             1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                             &target, &target, juce::Time::getCurrentTime(),
                             position, juce::Time::getCurrentTime(), 1, false);
}

/**
    Drives one horizontal fader and reports whether the parameter it is bound to
    actually moved.

    `read` is called before and after, so the caller checks the processor rather
    than the slider: a slider whose thumb moves but whose callback never fires is
    precisely the bug this is looking for.

    The fader is first parked at the middle of its range. Without that, a fader
    already sitting at an end produces no change when pushed further that way, and
    the check would report a false failure.
*/
void checkFader (juce::Slider& slider,
                 const juce::String& name,
                 std::function<double()> read)
{
    ++checked;

    if (! slider.isVisible() || ! slider.isEnabled())
    {
        fail (name + ": not visible or not enabled");
        return;
    }

    if (slider.getSliderStyle() != juce::Slider::LinearHorizontal)
    {
        fail (name + ": expected a horizontal fader");
        return;
    }

    if (slider.getWidth() < 4)
    {
        fail (name + ": too narrow to drag (" + juce::String (slider.getWidth()) + " px)");
        return;
    }

    // Park it in the middle. This goes through the same callback a user would
    // trigger, so it also proves the callback exists before the mouse is involved.
    const auto middle = (slider.getMinimum() + slider.getMaximum()) * 0.5;
    slider.setValue (middle, juce::sendNotificationSync);

    if (std::abs (read() - middle) > 0.01)
    {
        fail (name + ": setting the fader to " + juce::String (middle, 3)
                + " left the parameter at " + juce::String (read(), 3));
        return;
    }

    const auto y = (float) slider.getLocalBounds().getCentreY();
    const std::pair<float, float> target{ 2.0f, (float) (slider.getWidth() - 2) };

    const auto startValue = read();

    // Drag to the left end with the button held, the way a user moves a fader.
    {
        const auto p = juce::Point<float> (target.first, y);
        slider.mouseDown (makeEvent (slider, p));
        slider.mouseDrag (makeEvent (slider, p));
        slider.mouseUp (makeEvent (slider, p));
    }

    const auto lowValue = read();

    // ...and then to the right end.
    {
        const auto p = juce::Point<float> (target.second, y);
        slider.mouseDown (makeEvent (slider, p));
        slider.mouseDrag (makeEvent (slider, p));
        slider.mouseUp (makeEvent (slider, p));
    }

    const auto highValue = read();

    const auto movedToLow = std::abs (lowValue - startValue) > 1.0e-3;
    const auto movedToHigh = std::abs (highValue - lowValue) > 1.0e-3;

    if (movedToLow && movedToHigh && highValue > lowValue)
    {
        pass (name + ": " + juce::String (startValue, 3) + " -> " + juce::String (lowValue, 3)
                + " -> " + juce::String (highValue, 3));
    }
    else
    {
        fail (name + ": parameter did not follow the fader"
                + " (start " + juce::String (startValue, 3)
                + ", to left " + juce::String (lowValue, 3)
                + ", to right " + juce::String (highValue, 3) + ")"
                + " [slider now at " + juce::String (slider.getValue(), 3) + "]");
    }
}

/** Runs checkFader over every visible horizontal slider the component owns. */
void checkFadersIn (juce::Component& parent,
                    const juce::String& label,
                    std::function<double (juce::Slider&)> read)
{
    for (int i = 0; i < parent.getNumChildComponents(); ++i)
    {
        auto* child = parent.getChildComponent (i);
        auto* slider = dynamic_cast<juce::Slider*> (child);

        if (slider == nullptr || ! slider->isVisible())
            continue;

        checkFader (*slider, label + " slider " + juce::String (i), [slider, &read] { return read (*slider); });
    }
}

/**
    One of the transport bar's faders, found by the name the bar gives it.

    These used to be told apart by range, which was fine while there were two of them:
    swing runs 0..1, the trim runs in dB. The variation knob is a third 0..1 fader, and
    a control identified by a number it happens to share with someone else gets checked
    against the wrong parameter -- which is the same failure the parameter table had,
    one list written twice.
*/
juce::Slider* findNamedSlider (juce::Component& parent, const juce::String& name)
{
    for (int i = 0; i < parent.getNumChildComponents(); ++i)
    {
        auto* slider = dynamic_cast<juce::Slider*> (parent.getChildComponent (i));

        if (slider != nullptr && slider->getName() == name)
            return slider;
    }

    return nullptr;
}

/** Writes a short click to a scratch wav, for the audio half of the swing check.

    Written beside the executable rather than to the system temp folder, which is
    not guaranteed writable from every launching context.
*/
juce::File makeClickWav (const juce::String& name, int lengthSamples, double sampleRate)
{
    auto dir = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                   .getParentDirectory()
                   .getChildFile ("groovebox-test-scratch");

    if (! dir.isDirectory())
        dir.createDirectory();

    auto file = dir.getChildFile ("click-" + name + ".wav");

    file.deleteFile();

    juce::AudioBuffer<float> buffer (1, lengthSamples);
    auto* data = buffer.getWritePointer (0);

    for (int i = 0; i < lengthSamples; ++i)
    {
        if (i < 3)
        {
            data[i] = 0.9f;
            continue;
        }

        const auto envelope = std::pow (0.5, (double) (i - 3) / 12.0);
        data[i] = (float) (0.9 * envelope
                             * std::cos (2.0 * juce::MathConstants<double>::pi * 2.0 * (i - 3) / 48.0));
    }

    juce::WavAudioFormat format;
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());

    if (stream == nullptr)
        return {};

    std::unique_ptr<juce::AudioFormatWriter> writer (
        format.createWriterFor (stream.get(), sampleRate, 1, 24, {}, 0));

    if (writer == nullptr)
        return {};

    stream.release();
    writer->writeFromAudioSampleBuffer (buffer, 0, lengthSamples);
    writer.reset();

    return file;
}

/** Sample index of the first level above the threshold, or -1. */
int firstOnset (const std::vector<float>& data, float threshold = 0.005f)
{
    for (size_t i = 0; i < data.size(); ++i)
        if (std::abs (data[i]) >= threshold)
            return (int) i;

    return -1;
}

/** Renders a few bars and returns the left channel. */
std::vector<float> renderChannel (GrooveboxProcessor& processor, int totalSamples, int blockSize)
{
    std::vector<float> out;
    out.reserve ((size_t) totalSamples);

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;

    for (int rendered = 0; rendered < totalSamples; rendered += blockSize)
    {
        const auto thisBlock = juce::jmin (blockSize, totalSamples - rendered);

        buffer.setSize (2, thisBlock, false, true, true);
        buffer.clear();

        processor.processBlock (buffer, midi);

        for (int i = 0; i < thisBlock; ++i)
            out.push_back (buffer.getSample (0, i));
    }

    return out;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf ("Groovebox editor layout and wiring check\n");

    GrooveboxProcessor processor;
    processor.prepareToPlay (44100.0, 512);

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());

    if (editor == nullptr)
    {
        std::printf ("FAIL: no editor was created\n");
        return 1;
    }

    // Lay out at the size the plugin itself uses. Hard-coding it here once meant
    // the test was laying out a size the editor never actually takes — and the floor
    // of 1240 that used to sit on this call became dead weight the moment the bar grew
    // past it, since a design width written twice is a design width that gets missed
    // on the second change.
    const auto preferredWidth = editor->getWidth();
    const auto preferredHeight = editor->getHeight();

    editor->setSize (preferredWidth, juce::jmax (preferredHeight, 790));
    std::printf ("\neditor size: %d x %d\n", editor->getWidth(), editor->getHeight());

    TransportBar* transportBar = nullptr;
    ChannelStrip* anyStrip = nullptr;
    StepGridComponent* stepGrid = nullptr;

    //======================================================================
    std::printf ("\n-- editor children --\n");

    for (int i = 0; i < editor->getNumChildComponents(); ++i)
    {
        auto* child = editor->getChildComponent (i);

        if (child == nullptr)
            continue;

        if (dynamic_cast<TransportBar*> (child) != nullptr)
            transportBar = dynamic_cast<TransportBar*> (child);
        else if (dynamic_cast<StepGridComponent*> (child) != nullptr)
            stepGrid = dynamic_cast<StepGridComponent*> (child);
        else if (dynamic_cast<ChannelStrip*> (child) != nullptr && anyStrip == nullptr)
            anyStrip = dynamic_cast<ChannelStrip*> (child);

        if (dynamic_cast<TransportBar*> (child) != nullptr
            || dynamic_cast<StepGridComponent*> (child) != nullptr
            || dynamic_cast<ChannelStrip*> (child) != nullptr)
        {
            checkBounds (*editor, *child, "component " + juce::String (i));
        }
    }

    //======================================================================
    if (transportBar != nullptr)
    {
        std::printf ("\n-- transport bar children --\n");

        for (int i = 0; i < transportBar->getNumChildComponents(); ++i)
        {
            auto* inner = transportBar->getChildComponent (i);

            if (inner != nullptr && inner->isVisible())
                checkBounds (*transportBar, *inner, "child " + juce::String (i));
        }
    }

    //======================================================================
    if (anyStrip != nullptr)
    {
        std::printf ("\n-- one channel strip (height %d) --\n", anyStrip->getHeight());

        for (int i = 0; i < anyStrip->getNumChildComponents(); ++i)
        {
            auto* inner = anyStrip->getChildComponent (i);

            if (inner == nullptr || ! inner->isVisible())
                continue;

            const auto kind = dynamic_cast<juce::TextButton*> (inner) != nullptr ? juce::String ("TextButton:'")
                                + dynamic_cast<juce::TextButton*> (inner)->getButtonText() + "'"
                              : dynamic_cast<juce::ComboBox*> (inner) != nullptr ? juce::String ("ComboBox:'")
                                + dynamic_cast<juce::ComboBox*> (inner)->getText() + "'"
                              : dynamic_cast<juce::Slider*> (inner) != nullptr ? juce::String ("Slider")
                              : dynamic_cast<juce::Label*> (inner) != nullptr ? juce::String ("Label:'")
                                + dynamic_cast<juce::Label*> (inner)->getText() + "'"
                              : juce::String ("component");

            checkBounds (*anyStrip, *inner, juce::String (i) + " " + kind);
        }

        // A negative spare means the strip ran out of room and something below it
        // was laid out with nothing, which is how the mute button went missing.
        ++checked;

        if (anyStrip->getSpareHeight() >= 0)
            pass ("strip has " + juce::String (anyStrip->getSpareHeight()) + " px to spare");
        else
            fail ("strip is " + juce::String (-anyStrip->getSpareHeight()) + " px too short");
    }

    //======================================================================
    //  The bar's wrap rule, at every width the window can take.
    //
    //  This is the half of 5.6 that 4.3b had to pay for: a third fader is 154 px of
    //  row, and the alternative to a rule about where those 154 px go was a floor that
    //  moved again. Two things are being claimed -- that the row wraps instead of
    //  squeezing, and that the squeeze never happened at either size -- and both are
    //  only visible by laying the real editor out at each side of the boundary.
    std::printf ("\n-- the transport bar at the sizes the window can take --\n");
    {
        std::vector<ChannelStrip*> strips;

        for (int i = 0; i < editor->getNumChildComponents(); ++i)
        {
            if (auto* strip = dynamic_cast<ChannelStrip*> (editor->getChildComponent (i)))
                strips.push_back (strip);
        }

        expect (static_cast<int> (strips.size()) == Layout::numChannels,
                "every strip is in reach, not just one",
                juce::String (static_cast<int> (strips.size())) + " of "
                    + juce::String (Layout::numChannels));

        const auto barSlider = [transportBar] (const juce::String& name)
        {
            return findNamedSlider (*transportBar, name);
        };

        struct Size { int width, height; juce::String what; };

        // The bar's rule is about *bar* widths, so the arms are written in bar widths and
        // the editor's margins are measured off the running editor rather than typed in
        // a second time -- a margin written twice is a margin that gets missed.
        const auto barInset = preferredWidth - transportBar->getWidth();

        const Size sizes[] = {
            { preferredWidth,                             preferredHeight, "the default" },
            { TransportBar::oneRowWidth + barInset,       preferredHeight, "exactly what one line asks for" },
            { TransportBar::oneRowWidth - 1 + barInset,   preferredHeight, "one px narrower, so two lines" },
            { TransportBar::wrappedRowWidth + barInset,   preferredHeight, "the editor's floor" },
            { 1500,                                       700,             "in between" },
            { 2200,                                       1400,            "the largest the editor allows" },
        };

        std::printf ("  the bar sits %d px inside the window on each side\n", barInset / 2);

        for (const auto& size : sizes)
        {
            editor->setSize (size.width, size.height);

            const auto wrapped = TransportBar::isWrappedForWidth (transportBar->getWidth());

            std::printf ("\n  %d x %d (bar %d) -- %s (%s)\n", size.width, size.height,
                         transportBar->getWidth(), size.what.toRawUTF8(),
                         wrapped ? "two lines" : "one line");

            // The editor and the bar have to agree about how much of the window the bar
            // has taken, or the grid is laid out inside a band the bar is painting in.
            expect (transportBar->getHeight() == TransportBar::heightForWidth (transportBar->getWidth()),
                    "the bar took the height its own rule says",
                    juce::String (transportBar->getHeight()) + " px for a "
                        + juce::String (transportBar->getWidth()) + " px bar, rule says "
                        + juce::String (TransportBar::heightForWidth (transportBar->getWidth())));

            // The boundary itself, stated without borrowing the predicate being tested.
            //
            // The check above only proves the editor and the bar read one rule, so on its
            // own it stays green for any rule at all -- change `isWrappedForWidth` to
            // `return false` and everything above agrees with itself, and the only reason
            // the run still goes red is that a 1774 px row happens not to fit in 1182 px.
            // This is the gate that names the threshold and the two heights, so a flipped
            // comparison or a swapped pair of numbers is caught at the px, not downstream.
            const auto wantHeight = transportBar->getWidth() >= TransportBar::oneRowWidth
                                      ? TransportBar::oneRowHeight : TransportBar::twoRowHeight;

            expect (transportBar->getHeight() == wantHeight,
                    "one line at oneRowWidth and two below it, by the numbers",
                    juce::String (transportBar->getHeight()) + " px for a "
                        + juce::String (transportBar->getWidth()) + " px bar, want "
                        + juce::String (wantHeight));

            auto* swing = barSlider ("Swing");
            auto* touch = barSlider ("Touch");
            auto* mutate = barSlider ("Mutate");
            auto* humanize = barSlider ("Humanize");
            auto* master = barSlider ("Master");

            if (swing == nullptr || touch == nullptr || mutate == nullptr
                || humanize == nullptr || master == nullptr)
            {
                fail ("a bar fader is missing, so the line cannot be placed");
                continue;
            }

            // Which line the variation band is on, said as overlap rather than as an
            // equality of y: two bands laid out by different code can share a line
            // without agreeing on a pixel, and a check that only passes when they do is
            // a check that fails on a harmless refactor.
            const auto sameLine = touch->getBounds().getY() <= swing->getBounds().getBottom()
                                  && swing->getBounds().getY() <= touch->getBounds().getBottom();

            expect (sameLine != wrapped,
                    wrapped ? "the variation band is on a line of its own"
                            : "the variation band is still in the row",
                    "Touch y=" + juce::String (touch->getBounds().getY())
                        + ", Swing y=" + juce::String (swing->getBounds().getY())
                        + ".." + juce::String (swing->getBounds().getBottom()));

            // The promise the wrap exists to keep: nothing on either line gives up width
            // to fit the window, so every fader is still the fader the design chose.
            const auto widths = juce::String (mutate->getWidth()) + "/" + juce::String (humanize->getWidth())
                                + "/" + juce::String (touch->getWidth()) + "/"
                                + juce::String (swing->getWidth()) + "/" + juce::String (master->getWidth());

            expect (mutate->getWidth() == 96 && humanize->getWidth() == 96 && touch->getWidth() == 96
                        && swing->getWidth() == 140 && master->getWidth() == 110,
                    "no fader was squeezed to pay for the row", widths + " (want 96/96/96/140/110)");

            for (int i = 0; i < transportBar->getNumChildComponents(); ++i)
            {
                auto* inner = transportBar->getChildComponent (i);

                if (inner != nullptr && inner->isVisible())
                    checkBounds (*transportBar, *inner, "bar child " + juce::String (i));
            }

            // All twelve strips, at every size. The `tight` branch in
            // ChannelStrip::resized is a height rule, so this is the only way the walk
            // ever runs the code that decides whether the bottom of a strip is on screen.
            auto shortOnes = 0;

            for (int lane = 0; lane < static_cast<int> (strips.size()); ++lane)
            {
                auto* strip = strips[(size_t) lane];
                auto bad = 0, seen = 0;

                for (int i = 0; i < strip->getNumChildComponents(); ++i)
                {
                    auto* inner = strip->getChildComponent (i);

                    if (inner == nullptr || ! inner->isVisible())
                        continue;

                    ++seen;

                    if (! fitsInside (*strip, *inner))
                        ++bad;
                }

                if (strip->getHeight() < 219)
                    ++shortOnes;

                expectQuiet (bad == 0 && seen > 0 && strip->getSpareHeight() >= 0,
                             "strip " + juce::String (lane) + " is whole",
                             juce::String (bad) + " clipped of " + juce::String (seen)
                                 + ", " + juce::String (strip->getHeight()) + " px tall, spare "
                                 + juce::String (strip->getSpareHeight()));
            }

            // A strip shorter than its wanted 219 px takes `ChannelStrip::resized`'s
            // tight branch, and two of its seventeen visible controls land outside it.
            // Measured rather than guessed: a probe arm at 1832 x 500 put the strips at
            // 236 px with spare 0 and twelve BAD lines, and was then removed again -- the
            // run is in out/layout-tight-probe.log, the fix is 5.6's remaining half. No
            // window the user can reach is that short (the floor is 1218 x 844), so the
            // claim here is only that the broken code stays unreachable, and it is a claim
            // that goes red the day someone lowers the floor again.
            expect (shortOnes == 0,
                    "no strip is short enough to need the tight rule at this size",
                    juce::String (strips.front() != nullptr ? strips.front()->getHeight() : 0)
                        + " px of strip, " + juce::String (shortOnes) + " of "
                        + juce::String (static_cast<int> (strips.size())) + " under 219");

            // The number under the pointer, asked of every corner it has to survive.
            //
            // All 192 of them, because the two that break are opposite ones: the top-left
            // cell hangs the label above the grid's top edge, the top-right one off the
            // right, and a check that only looks at the first cannot see the second. The
            // size is part of the claim too -- a helper that pulled the rectangle's bottom
            // edge up to the component's top edge would stay inside it, be 1 px tall, and
            // print as a pass.
            auto readoutOut = 0, readoutSquashed = 0, readouts = 0;

            if (stepGrid != nullptr)
            {
                for (int lane = 0; lane < Layout::numChannels; ++lane)
                {
                    for (int step = 0; step < Layout::numSteps; ++step)
                    {
                        const auto cell = stepGrid->getCellBounds (lane, step);
                        const auto readout = stepGrid->valueReadoutBoundsFor (lane, step);

                        if (cell.isEmpty())
                            continue;

                        ++readouts;

                        if (readout.getX() < 0 || readout.getY() < 0
                            || readout.getRight() > stepGrid->getWidth()
                            || readout.getBottom() > stepGrid->getHeight())
                            ++readoutOut;

                        if (readout.getWidth() < juce::jmax (cell.getWidth(), 22)
                            || readout.getHeight() != 11)
                            ++readoutSquashed;
                    }
                }
            }

            expect (stepGrid != nullptr && readouts == Layout::numChannels * Layout::numSteps
                        && readoutOut == 0 && readoutSquashed == 0,
                    "every hover readout is on screen at full size",
                    juce::String (readouts) + " cells, " + juce::String (readoutOut)
                        + " outside, " + juce::String (readoutSquashed) + " wrong size in a "
                        + juce::String (stepGrid != nullptr ? stepGrid->getWidth() : 0) + " x "
                        + juce::String (stepGrid != nullptr ? stepGrid->getHeight() : 0) + " grid");
        }

        std::printf ("\n  %d sizes laid out, %d problems so far\n",
                     static_cast<int> (sizeof (sizes) / sizeof (sizes[0])), problems);

        editor->setSize (preferredWidth, juce::jmax (preferredHeight, 790));
    }

    //======================================================================
    std::printf ("\n-- fader wiring: does a drag reach the processor? --\n");

    if (anyStrip != nullptr)
    {
        checkFadersIn (*anyStrip, "channel 0", [&processor] (juce::Slider& s)
        {
            // Two faders live in a strip now, and they are told apart by range:
            // the level runs 0..1.5, the pitch drop counts semitones up to 12.
            // Reading both against the same parameter would have the drop drag
            // "fail" by checking itself against the fader it is not.
            return s.getMaximum() > 1.51 ? (double) processor.getChannelPitchDrop (0)
                                         : (double) processor.getChannelLevel (0);
        });
    }

    if (transportBar != nullptr)
    {
        // Sanity-check the setter on its own, so a failure below can be
        // attributed to the wiring rather than to the parameter.
        ++checked;
        processor.setSwing (0.5f);
        const auto roundTrip = processor.getSwing();
        processor.setSwing (0.0f);

        if (std::abs (roundTrip - 0.5f) < 1.0e-4f)
            pass ("setSwing/getSwing round trip (" + juce::String (roundTrip, 3) + ")");
        else
            fail ("setSwing did not stick: getSwing returned " + juce::String (roundTrip, 3));

        // Confirm the slider's onValueChange is actually connected: change the
        // slider's own value and see whether the callback runs.
        {
            ++checked;
            auto* swingSlider = findNamedSlider (*transportBar, "Swing");

            if (swingSlider == nullptr)
            {
                fail ("the transport bar has no fader named Swing");
            }
            else
            {
                const auto before = processor.getSwing();
                swingSlider->setValue (0.75, juce::sendNotificationSync);
                const auto after = processor.getSwing();

                if (std::abs (after - 0.75) < 1.0e-3)
                    pass ("swing slider onValueChange reaches the processor (" + juce::String (after, 3) + ")");
                else
                    fail ("swing slider onValueChange did not reach the processor ("
                            + juce::String (before, 3) + " -> " + juce::String (after, 3)
                            + ", slider at " + juce::String (swingSlider->getValue(), 3) + ")");
            }
        }

        // Each of the bar's faders has to be checked against its own parameter,
        // so each is asked for by name.
        const auto checkBarFader = [transportBar] (const juce::String& name,
                                                   std::function<double()> read)
        {
            if (auto* slider = findNamedSlider (*transportBar, name))
                checkFader (*slider, "transport " + name, read);
            else
                fail ("the transport bar has no fader named " + name);
        };

        checkBarFader ("Swing",    [&processor] { return (double) processor.getSwing(); });
        checkBarFader ("Master",   [&processor] { return (double) processor.getMasterGainDb(); });
        checkBarFader ("Mutate",   [&processor] { return (double) processor.getMutate(); });
        checkBarFader ("Humanize", [&processor] { return (double) processor.getHumanize(); });
        checkBarFader ("Touch",    [&processor] { return (double) processor.getTouch(); });

        // Those drags leave all five parameters at the top of their range, and the
        // audio checks below measure levels and count hits, so put them back first.
        // The variation one matters most: a knob left open starts dropping steps, and
        // every level check underneath would then be measuring a different pattern.
        // The timing one is the same trap at one remove: an open Humanize does not
        // change which steps fire, but it moves when they fire, and the onset checks
        // below measure the grid. The level one is the trap the third time: an open
        // Touch changes the level of every hit the checks underneath compare, so a
        // threshold that is right with the knob shut is wrong by a quarter of itself
        // with it open.
        processor.setSwing (0.0f);
        processor.setMasterGainDb (Layout::defaultMasterGainDb);
        processor.setMutate (0.0f);
        processor.setHumanize (0.0f);
        processor.setTouch (0.0f);
    }

    //======================================================================
    // The reverse of the drag test above: a value a host writes straight into the
    // parameter, with the panel never touched, has to be reflected back onto the
    // controls. A drag cannot prove this — a drag moves the control first and the
    // processor second, whereas automation moves the processor and leaves the panel
    // to catch up. The strip does that catch-up in syncAutomatableControls(), which
    // its timer runs every tick; a headless build cannot pump the OS timer, so the
    // test invokes the same catch-up through refresh(), which calls exactly that
    // method. It covers the four control kinds separately because a slider
    // attachment — the obvious tool — cannot bind the mute toggle, the envelope's
    // custom drag rows, or the pitch label; those are the ones this fix is for.
    std::printf ("\n-- the panel catches up to a host-written parameter --\n");

    if (anyStrip != nullptr)
    {
        auto& apvts = processor.getValueTreeState();

        juce::Slider* levelSlider = nullptr;
        juce::Slider* dropSlider = nullptr;
        juce::Label* dropLabel = nullptr;
        juce::Button* muteButton = nullptr;
        juce::Label* pitchLabel = nullptr;
        juce::ComboBox* chokeBox = nullptr;
        EnvelopeControls* envelope = nullptr;

        for (int i = 0; i < anyStrip->getNumChildComponents(); ++i)
        {
            auto* child = anyStrip->getChildComponent (i);

            if (auto* s = dynamic_cast<juce::Slider*> (child);
                s != nullptr && s->getSliderStyle() == juce::Slider::LinearHorizontal)
            {
                // Two horizontal faders live in a strip now, so they are told apart
                // by range rather than by the order they were added in: the level
                // tops out at 1.5, the drop counts semitones up to 12.
                if (s->getMaximum() > 1.51)
                    dropSlider = s;
                else
                    levelSlider = s;
            }
            else if (auto* l = dynamic_cast<juce::Label*> (child);
                     l != nullptr && l->getTooltip().startsWith ("Pitch drop"))
                dropLabel = l;
            else if (auto* b = dynamic_cast<juce::Button*> (child);
                     b != nullptr && b->getButtonText() == "Mute")
                muteButton = b;
            else if (auto* pl = dynamic_cast<juce::Label*> (child);
                     pl != nullptr && pl->getTooltip().startsWith ("Pitch offset"))
                pitchLabel = pl;
            else if (auto* c = dynamic_cast<juce::ComboBox*> (child))
                chokeBox = c;
            else if (auto* e = dynamic_cast<EnvelopeControls*> (child))
                envelope = e;
        }

        ++checked;

        if (levelSlider == nullptr || muteButton == nullptr || pitchLabel == nullptr
             || chokeBox == nullptr || envelope == nullptr
             || dropSlider == nullptr || dropLabel == nullptr)
        {
            fail ("the strip is missing a control the automation check needs");
        }
        else
        {
            // Each case: write the parameter the way a host does — normalised, into
            // the parameter, past every setter — catch the panel up, and confirm the
            // control now shows what the host asked for.
            const auto automate = [&] (juce::StringRef id, float value)
            {
                if (auto* p = apvts.getParameter (id))
                    p->setValueNotifyingHost (p->convertTo0to1 (value));
            };

            const struct { juce::String id; float value; const char* what; } cases[]
            {
                { Params::levelId (0),     0.37f, "level" },
                { Params::pitchId (0),     3.0f,  "pitch" },
                { Params::pitchDropId (0), 5.0f,  "drop" },
                { Params::attackId (0),    0.62f, "attack" },
                { Params::muteId (0),      1.0f,  "mute" },
                { Params::chokeId (0),     2.0f,  "choke" },
            };

            for (const auto& c : cases)
            {
                automate (c.id, c.value);
                anyStrip->refresh();

                ++checked;
                const juce::String kind = c.what;
                bool ok = false;
                juce::String detail;

                if (kind == "level")
                {
                    ok = std::abs (levelSlider->getValue() - (double) c.value) < 0.01;
                    detail = juce::String (levelSlider->getValue(), 3);
                }
                else if (kind == "pitch")
                {
                    ok = pitchLabel->getText() == "+3";
                    detail = pitchLabel->getText();
                }
                else if (kind == "drop")
                {
                    // Both halves: the fader has to be where the host put it, and so
                    // does the number the user actually reads.
                    ok = std::abs (dropSlider->getValue() - (double) c.value) < 0.01
                           && dropLabel->getText() == juce::String ("\xe2\x86\x93" "5");
                    detail = "\"" + dropLabel->getText() + "\" at "
                                + juce::String (dropSlider->getValue(), 1);
                }
                else if (kind == "attack")
                {
                    const auto a = envelope->getValue (EnvelopeStage::attack);
                    ok = std::abs (a - c.value) < 0.01f;
                    detail = juce::String (a, 3);
                }
                else if (kind == "choke")
                {
                    // The item id is one above the group number, because JUCE keeps
                    // id 0 for "nothing selected" — so a box that shows the right
                    // group but has the offset wrong anywhere in the pair fails here.
                    ok = chokeBox->getSelectedId() == static_cast<int> (c.value) + 1;
                    detail = "id " + juce::String (chokeBox->getSelectedId())
                                + " (\"" + chokeBox->getText() + "\")";
                }
                else // mute
                {
                    ok = muteButton->getToggleState();
                    detail = ok ? "on" : "off";
                }

                if (ok)
                    pass (juce::String (c.what) + " control follows a host-written value (" + detail + ")");
                else
                    fail (juce::String (c.what) + " control did not follow host automation (showing " + detail + ")");
            }

            // Undo the automation so the audio checks below start from sane values.
            processor.setChannelLevel (0, 1.0f);
            processor.setChannelPitch (0, 0);
            processor.setChannelPitchDrop (0, 0);
            processor.setChannelMuted (0, false);
            processor.setChannelChokeGroup (0, 0);
            processor.resetChannelEnvelope (0);
            anyStrip->refresh();

            //==================================================================
            // The other direction: a pick in the box has to reach the engine, and
            // reach the group it names. The mute toggle, the fader and the envelope
            // rows are checked this way by dragging; a combo box is checked by
            // selecting, which runs the same notification a click does.
            ++checked;

            const auto pickReachesEngine = [&] (int group)
            {
                chokeBox->setSelectedId (group + 1, juce::sendNotificationSync);

                const auto landed = processor.getChannelChokeGroup (0);

                if (landed != group)
                {
                    fail ("picking \"" + chokeBox->getText() + "\" set the lane's group to "
                            + juce::String (landed) + ", not " + juce::String (group));
                    return false;
                }

                return true;
            };

            auto picked = pickReachesEngine (0);
            picked = pickReachesEngine (Layout::numChokeGroups) && picked;

            if (picked)
                pass ("a pick in the choke box reaches the lane, both ends of its range");

            processor.setChannelChokeGroup (0, 0);
            anyStrip->refresh();
        }
    }

    //======================================================================
    // The swing amount has to reach the audio, not just the parameter: step 1 is
    // the off-beat of the first swing pair, so full swing pushes it a whole step
    // later.
    std::printf ("\n-- swing reaches the audio --\n");

    {
        ++checked;

        auto click = makeClickWav ("swing-audio", 2000, 44100.0);
        juce::String loadError;

        if (! click.existsAsFile() || ! processor.loadSampleIntoChannel (0, click, loadError))
        {
            fail ("could not prepare a sample for the swing audio check: " + loadError);
        }
        else
        {
            constexpr int blockSize = 256;
            constexpr int stepSamples = 5512;

            for (int step = 0; step < Layout::numSteps; ++step)
                processor.setStep (0, step, false);

            processor.setStep (0, 1, true); // the off-beat
            processor.setSwingGrid (4);
            processor.setManualTempo (120.0);

            processor.setSwing (0.0f);
            processor.resetTransport();
            processor.setPlaying (true);
            const auto straight = firstOnset (renderChannel (processor, stepSamples * 4, blockSize));

            processor.setSwing (1.0f);
            processor.resetTransport();
            processor.setPlaying (true);
            const auto swung = firstOnset (renderChannel (processor, stepSamples * 4, blockSize));

            processor.setPlaying (false);

            // Swing runs on a 1/8 grid, so a swing pair spans two 16th-note steps
            // and step 1 is the off-beat. At swing 1 the off-beat sits 0.75 of the
            // way through the pair instead of 0.5, which moves it half a pair -
            // that is, one step of 2756 samples at 120 bpm.
            const auto expectedShift = stepSamples / 2;
            const auto shift = swung - straight;

            if (straight >= 0 && swung >= 0 && std::abs (shift - expectedShift) <= 4)
                pass ("full swing moves step 1 by half a pair (" + juce::String (shift) + " samples)");
            else
                fail ("swing did not move the audio as expected (straight "
                        + juce::String (straight) + ", swung " + juce::String (swung)
                        + ", expected a shift of " + juce::String (expectedShift) + ")");

            click.deleteFile();
        }
    }

    //======================================================================
    // The swing grid parameter is an int from 2 to 4, so a host can put it on 3 —
    // a pair that does not divide a 16-step bar, and so not a grid at all. The
    // engine answers a 3 with the wider pair; the panel has to light the button that
    // says which pair that is. Before this, a 3 left both dark, which reads as a
    // control that has stopped working rather than as a value that cannot mean
    // anything.
    std::printf ("\n-- the swing grid buttons follow what the engine plays --\n");

    if (transportBar != nullptr)
    {
        juce::Button* eighth = nullptr;
        juce::Button* sixteenth = nullptr;

        for (int i = 0; i < transportBar->getNumChildComponents(); ++i)
        {
            auto* button = dynamic_cast<juce::TextButton*> (transportBar->getChildComponent (i));

            if (button == nullptr)
                continue;

            const auto text = button->getButtonText();

            if (text == "1/8")
                eighth = button;
            else if (text == "1/16")
                sixteenth = button;
        }

        auto* gridParam = processor.getValueTreeState().getParameter (Params::swingGridId);

        ++checked;

        if (eighth == nullptr || sixteenth == nullptr || gridParam == nullptr)
        {
            fail ("the swing grid controls are not all reachable");
        }
        else
        {
            pass ("both grid buttons and the host parameter are there");

            const auto writeGrid = [gridParam] (int value)
            {
                gridParam->setValueNotifyingHost (gridParam->convertTo0to1 ((float) value));
            };

            struct Case { int grid; bool eighthOn; };

            const Case cases[] = { { 2, false }, { 3, true }, { 4, true } };

            for (const auto& testCase : cases)
            {
                writeGrid (testCase.grid);
                transportBar->refresh();

                ++checked;

                // Exactly one lit, and the one the engine is following.
                if (eighth->getToggleState() == testCase.eighthOn
                     && sixteenth->getToggleState() != testCase.eighthOn)
                    pass ("grid " + juce::String (testCase.grid) + " lights the "
                            + juce::String (testCase.eighthOn ? "1/8" : "1/16") + " button");
                else
                    fail ("grid " + juce::String (testCase.grid) + " lit 1/8="
                            + juce::String (eighth->getToggleState() ? 1 : 0)
                            + " 1/16=" + juce::String (sixteenth->getToggleState() ? 1 : 0));
            }

            writeGrid (4);
            transportBar->refresh();
        }
    }

    //======================================================================
    // The knobs that are shut by default, through the panel rather than through the
    // processor.
    //
    // `checkBarFader` above already proved a drag reaches the engine. What a drag
    // cannot prove is the other two directions: that a value the host writes with the
    // panel untouched gets drawn back onto the fader, and that the double-click — the
    // only way to shut a knob from the bar — actually shuts it. Both matter most on
    // these two because their shut end is the state a session is supposed to come back
    // to, and because it is the state every audio threshold in this suite is tuned to.
    std::printf ("\n-- the knobs that default to shut answer the host, and the mouse can shut them --\n");

    if (transportBar != nullptr)
    {
        // Written as a rule with two arguments rather than as one block copied twice:
        // the two directions a drag cannot prove are the same two directions for both
        // knobs, and a second copy is a block that gets fixed once.
        const auto checkShutKnob = [&] (const juce::String& name,
                                        const juce::String& label,
                                        const juce::String& paramId,
                                        const std::function<float()>& read,
                                        const std::function<void (float)>& write)
        {
            auto* fader = findNamedSlider (*transportBar, name);
            auto* param = processor.getValueTreeState().getParameter (paramId);

            ++checked;

            if (fader == nullptr || param == nullptr)
            {
                fail ("the " + label + " fader or its parameter is not reachable");
                return;
            }

            pass ("the " + label + " fader and the host parameter are both there");

            // A host write, with the notification path a drag does not use: the
            // parameter moves, the processor's atomics follow through
            // `parameterChanged`, and only `refresh()` tells the panel.
            param->setValueNotifyingHost (param->convertTo0to1 (0.4f));
            transportBar->refresh();

            ++checked;

            if (std::abs (fader->getValue() - 0.4) < 0.01 && std::abs (read() - 0.4f) < 1.0e-3)
                pass ("a host-written 0.4 is drawn on the " + label
                        + " fader and heard by the engine (" + juce::String (fader->getValue(), 3) + ")");
            else
                fail ("a host-written 0.4 left the " + label + " fader at "
                        + juce::String (fader->getValue(), 3) + " and the engine at "
                        + juce::String ((double) read(), 3));

            // Double-click is the documented way back to shut. It is dispatched as
            // `mouseDoubleClick`, which is the handler the platform calls after it has
            // decided two clicks were close enough to be one — a pair of synthetic
            // mouseDowns never reaches that decision, because the click count comes
            // from the OS, not from the test.
            fader->setValue (0.8, juce::sendNotificationSync);

            fader->mouseDoubleClick (makeEvent (*fader, fader->getLocalBounds().getCentre().toFloat()));

            ++checked;

            if (fader->getValue() == 0.0 && read() == 0.0f)
                pass ("a double-click shuts the " + label + " knob, in the panel and in the engine");
            else
                fail ("a double-click left the " + label + " fader at "
                        + juce::String (fader->getValue(), 3) + " and the engine at "
                        + juce::String ((double) read(), 3));

            write (0.0f);
            transportBar->refresh();
        };

        checkShutKnob ("Humanize", "humanize", Params::humanizeId,
                       [&processor] { return processor.getHumanize(); },
                       [&processor] (float v) { processor.setHumanize (v); });

        checkShutKnob ("Touch", "touch", Params::touchId,
                       [&processor] { return processor.getTouch(); },
                       [&processor] (float v) { processor.setTouch (v); });
    }

    //======================================================================
    // Velocity editing has to work through the real UI: the button switches the
    // grid over, a drag in a cell sets the step's velocity, and the audio that
    // comes out is quieter. That whole chain is invisible to an audio-only test.
    std::printf ("\n-- velocity editing --\n");

    if (transportBar != nullptr && stepGrid != nullptr)
    {
        // Find the velocity toggle in the transport bar.
        juce::Button* velocityButton = nullptr;

        for (int i = 0; i < transportBar->getNumChildComponents(); ++i)
        {
            auto* button = dynamic_cast<juce::TextButton*> (transportBar->getChildComponent (i));

            if (button != nullptr && button->getButtonText() == "Velocity")
                velocityButton = button;
        }

        ++checked;

        if (velocityButton == nullptr)
        {
            fail ("no Velocity button in the transport bar");
        }
        else
        {
            // Clicking it should switch the grid over, through the editor's wiring.
            velocityButton->setToggleState (true, juce::sendNotificationSync);

            ++checked;

            if (stepGrid->isVelocityMode())
                pass ("the Velocity button switches the grid into velocity mode");
            else
                fail ("the Velocity button did not reach the grid");

            // Now drag inside a cell and check the processor recorded a velocity.
            processor.setStep (0, 0, true);
            processor.setStepVelocity (0, 0, 1.0f);

            // The grid's own geometry, asked rather than re-derived. The point has to
            // land inside a real cell, and the border inset, the gap between rows and
            // the reversed row order are the grid's business; every one of them was
            // duplicated here until a 17 px change to the strip height put this drag
            // in the gap under the last row, where it silently did nothing.
            const auto cell = stepGrid->getCellBounds (0, 0);

            ++checked;

            if (cell.getWidth() < 6 || cell.getHeight() < 6)
            {
                fail ("channel 0 step 0 is too small to drag (" + juce::String (cell.getWidth())
                        + " x " + juce::String (cell.getHeight()) + ")");
            }
            else
            {
                // A quarter of the way up the cell, which is how far the bar has risen.
                const auto point = juce::Point<int> (cell.getX() + 3,
                                                     cell.getBottom() - juce::jmax (2, cell.getHeight() / 4));

                stepGrid->mouseDown (makeEvent (*stepGrid, point.toFloat()));
                stepGrid->mouseUp (makeEvent (*stepGrid, point.toFloat()));

                const auto recorded = processor.getStepVelocity (0, 0);

                ++checked;

                if (recorded > 0.05f && recorded < 0.6f)
                    pass ("a drag in a step sets its velocity (" + juce::String (recorded, 2) + ")");
                else
                    fail ("a drag in a step did not set a sensible velocity (got "
                            + juce::String (recorded, 2) + ")");
            }

            // And the velocity has to reach the audio: compare a full-velocity hit
            // against the one just set.
            auto click = makeClickWav ("velocity", 400, 44100.0);
            juce::String loadError;

            if (click.existsAsFile() && processor.loadSampleIntoChannel (0, click, loadError))
            {
                // A fresh processor per render, so the previous hit's tail cannot
                // leak into the next measurement.
                const auto renderHit = [&click] (float velocity)
                {
                    GrooveboxProcessor p;
                    p.prepareToPlay (44100.0, 256);

                    juce::String err;
                    p.loadSampleIntoChannel (0, click, err);

                    for (int s = 0; s < Layout::numSteps; ++s)
                        p.setStep (0, s, false);

                    p.setStep (0, 0, true);
                    p.setStepVelocity (0, 0, velocity);
                    p.setSwing (0.0f);
                    p.setSwingGrid (4);
                    p.setManualTempo (120.0);
                    p.setPlaying (true);

                    juce::AudioBuffer<float> buffer (2, 256);
                    juce::MidiBuffer midi;
                    float peak = 0.0f;

                    for (int block = 0; block < 8; ++block)
                    {
                        buffer.clear();
                        p.processBlock (buffer, midi);
                        peak = juce::jmax (peak, buffer.getMagnitude (0, buffer.getNumSamples()));
                    }

                    p.setPlaying (false);
                    return peak;
                };

                const auto loud = renderHit (1.0f);
                const auto quiet = renderHit (0.25f);

                ++checked;

                if (loud > 0.0f && quiet > 0.0f && quiet < loud * 0.5f)
                    pass ("a step's velocity scales its audio ("
                            + juce::String (loud, 3) + " full vs " + juce::String (quiet, 3) + " at 25%)");
                else
                    fail ("velocity did not scale the audio (full " + juce::String (loud, 3)
                            + ", 25% " + juce::String (quiet, 3) + ")");

                click.deleteFile();
            }

            // Leaving velocity mode must switch the grid back.
            velocityButton->setToggleState (false, juce::sendNotificationSync);

            ++checked;

            if (! stepGrid->isVelocityMode())
                pass ("clearing the Velocity button returns the grid to step editing");
            else
                fail ("the grid stayed in velocity mode");

            processor.setPlaying (false);
        }
    }

    //======================================================================
    // The variation half of the panel. An audio test cannot tell a drag that wrote a
    // velocity from one that meant to write a probability — the samples come out the
    // same either way if the level happens to match — so the wiring between these
    // buttons and the field they edit is only visible from this side.
    std::printf ("\n-- the variation controls --\n");

    if (transportBar != nullptr && stepGrid != nullptr)
    {
        juce::TextButton* velocityButton = nullptr;
        juce::TextButton* chanceButton = nullptr;
        juce::TextButton* resetButton = nullptr;
        juce::TextButton* rerollButton = nullptr;
        juce::Label* seedLabel = nullptr;

        for (int i = 0; i < transportBar->getNumChildComponents(); ++i)
        {
            auto* child = transportBar->getChildComponent (i);

            if (auto* button = dynamic_cast<juce::TextButton*> (child))
            {
                const auto text = button->getButtonText();

                if (text == "Velocity")       velocityButton = button;
                else if (text == "Chance")    chanceButton = button;
                else if (text == "100%")      resetButton = button;
                else if (text == "Reroll")    rerollButton = button;
            }
            else if (auto* label = dynamic_cast<juce::Label*> (child))
            {
                if (label->getName() == "Seed")
                    seedLabel = label;
            }
        }

        // A plain click, sent the way a press sends it: setToggleState fires the click
        // callback synchronously, which is what post-click flipping a toggle is for.
        const auto clickToggle = [] (juce::TextButton& button, bool on)
        {
            button.setToggleState (on, juce::sendNotificationSync);
        };

        const auto clickButton = [] (juce::Button& button)
        {
            if (button.onClick != nullptr)
                button.onClick();
        };

        // Every cell's probability, so "reset" can be proved to have reached them all
        // rather than only the one the drag touched.
        const auto chancesAtFull = [&processor]
        {
            for (int channel = 0; channel < Layout::numChannels; ++channel)
                for (int step = 0; step < Layout::numSteps; ++step)
                    if (std::abs (processor.getStepChance (channel, step) - 1.0f) > 0.002f)
                        return false;

            return true;
        };

        const auto velocitiesAtFull = [&processor]
        {
            for (int channel = 0; channel < Layout::numChannels; ++channel)
                for (int step = 0; step < Layout::numSteps; ++step)
                    if (std::abs (processor.getStepVelocity (channel, step) - 1.0f) > 0.002f)
                        return false;

            return true;
        };

        const auto countLit = [&processor]
        {
            int lit = 0;

            for (int channel = 0; channel < Layout::numChannels; ++channel)
                for (int step = 0; step < Layout::numSteps; ++step)
                    if (processor.isStepOn (channel, step))
                        ++lit;

            return lit;
        };

        ++checked;

        if (velocityButton == nullptr || chanceButton == nullptr || resetButton == nullptr
            || rerollButton == nullptr || seedLabel == nullptr)
        {
            fail ("the variation controls are not all on the transport bar");
        }
        else
        {
            pass ("Chance, 100%, Reroll and the seed readout are all there");

            //---- one number on the grid at a time --------------------------------
            clickToggle (*velocityButton, true);
            clickToggle (*chanceButton, true);

            ++checked;

            if (chanceButton->getToggleState() && stepGrid->isChanceMode()
                && ! stepGrid->isVelocityMode() && ! velocityButton->getToggleState())
                pass ("opening Chance closes Velocity, on the bar and on the grid");
            else
                fail ("the two overlays are open at once (chance lit="
                        + juce::String (chanceButton->getToggleState() ? 1 : 0)
                        + " velocity lit=" + juce::String (velocityButton->getToggleState() ? 1 : 0)
                        + " grid chance=" + juce::String (stepGrid->isChanceMode() ? 1 : 0)
                        + " grid velocity=" + juce::String (stepGrid->isVelocityMode() ? 1 : 0) + ")");

            //---- a drag writes the field that is on screen -----------------------
            processor.setStep (1, 3, true);
            processor.setStepVelocity (1, 3, 1.0f);
            processor.setStepChance (1, 3, 1.0f);

            const auto cell = stepGrid->getCellBounds (1, 3);

            ++checked;

            if (cell.getWidth() < 6 || cell.getHeight() < 6)
            {
                fail ("channel 1 step 3 is too small to drag (" + juce::String (cell.getWidth())
                        + " x " + juce::String (cell.getHeight()) + ")");
            }
            else
            {
                const auto point = juce::Point<int> (cell.getX() + 3,
                                                     cell.getBottom() - juce::jmax (2, cell.getHeight() / 4));

                stepGrid->mouseDown (makeEvent (*stepGrid, point.toFloat()));
                stepGrid->mouseUp (makeEvent (*stepGrid, point.toFloat()));

                const auto drawn = processor.getStepChance (1, 3);

                ++checked;

                if (drawn > 0.05f && drawn < 0.6f && processor.isStepOn (1, 3)
                    && std::abs (processor.getStepVelocity (1, 3) - 1.0f) < 0.001f)
                    pass ("a drag with Chance open moves the probability, not the level"
                            " (chance " + juce::String (drawn, 2)
                            + ", velocity " + juce::String (processor.getStepVelocity (1, 3), 2) + ")");
                else
                    fail ("the Chance drag did not write the probability alone (chance "
                            + juce::String (drawn, 2) + ", velocity "
                            + juce::String (processor.getStepVelocity (1, 3), 2) + ", step on "
                            + juce::String (processor.isStepOn (1, 3) ? 1 : 0) + ")");

                // A second cell, so the reset below has more than one thing to do.
                processor.setStep (2, 7, true);
                processor.setStepChance (2, 7, 0.5f);
                processor.setStepVelocity (1, 3, 0.4f);
                processor.setStepVelocity (2, 7, 0.6f);

                //---- the shared reset follows the open overlay -------------------
                clickButton (*resetButton);

                ++checked;

                if (chancesAtFull() && std::abs (processor.getStepVelocity (1, 3) - 0.4f) < 0.002f
                    && std::abs (processor.getStepVelocity (2, 7) - 0.6f) < 0.002f)
                    pass ("with Chance open, 100% puts every probability back and leaves the levels alone");
                else
                    fail ("the reset did not do the probabilities (all full "
                            + juce::String (chancesAtFull() ? 1 : 0) + ", velocity at 1,3 "
                            + juce::String (processor.getStepVelocity (1, 3), 2) + ")");

                // One gesture, so one undo takes the whole grid back. Without the
                // transaction in the handler this would need a hundred and ninety-two.
                ++checked;

                if (! processor.getUndoManager().undo())
                {
                    fail ("undo after the probability reset did nothing at all");
                }
                else
                {
                    const auto restored = processor.getStepChance (1, 3);

                    ++checked;

                    if (std::abs (restored - drawn) < 0.002f
                        && std::abs (processor.getStepChance (2, 7) - 0.5f) < 0.002f)
                        pass ("and it was one undo for the whole grid (step 1,3 back to "
                                + juce::String (restored, 2) + ", step 2,7 back to "
                                + juce::String (processor.getStepChance (2, 7), 2) + ")");
                    else
                        fail ("undo did not put the drawn probabilities back (step 1,3 is "
                                + juce::String (restored, 2) + ", step 2,7 is "
                                + juce::String (processor.getStepChance (2, 7), 2)
                                + ", the drag had drawn " + juce::String (drawn, 2) + ")");
                }

                // The other way round, because a handler that always did velocities
                // would have passed the check above by accident.
                clickToggle (*velocityButton, true);

                ++checked;

                if (stepGrid->isVelocityMode() && ! stepGrid->isChanceMode()
                    && ! chanceButton->getToggleState())
                    pass ("opening Velocity closes Chance the same way");
                else
                    fail ("the grid did not go back to velocity editing (chance lit="
                            + juce::String (chanceButton->getToggleState() ? 1 : 0)
                            + " grid velocity=" + juce::String (stepGrid->isVelocityMode() ? 1 : 0) + ")");

                const auto chanceStillDrawn = processor.getStepChance (1, 3);

                clickButton (*resetButton);

                ++checked;

                if (velocitiesAtFull()
                    && std::abs (processor.getStepChance (1, 3) - chanceStillDrawn) < 0.002f
                    && std::abs (processor.getStepChance (2, 7) - 0.5f) < 0.002f)
                    pass ("with Velocity open, the same button puts the levels back and leaves the chances");
                else
                    fail ("the reset did not do the velocities (all full "
                            + juce::String (velocitiesAtFull() ? 1 : 0) + ", chance at 2,7 "
                            + juce::String (processor.getStepChance (2, 7), 2) + ")");

                //---- Reroll: a new take, the same grid ---------------------------
                // Emptied first, because "a reroll is not an edit" can only be shown
                // against a history that had nothing else in it.
                processor.getUndoManager().clearUndoHistory();

                const auto litBefore = countLit();
                const auto seedBefore = processor.getSeed();

                clickButton (*rerollButton);

                const auto seedAfter = processor.getSeed();

                ++checked;

                if (seedLabel->getText() != juce::String (seedAfter))
                    fail ("the seed readout did not follow the take (label says "
                            + seedLabel->getText() + ", the engine is " + juce::String (seedAfter) + ")");
                else if (seedAfter == seedBefore)
                    pass ("the readout matches the engine (the reroll landed on the same number)");
                else
                    pass ("Reroll moved the seed and the readout moved with it ("
                            + juce::String (seedBefore) + " -> " + juce::String (seedAfter) + ")");

                ++checked;

                if (processor.getUndoManager().undo())
                    fail ("Reroll put something in the undo history; the seed is a parameter, not an edit");
                else
                    pass ("a reroll is not an edit, so there is nothing to take back");

                // The grid the user painted is where it was, which is the whole point:
                // a reroll that re-drew the pattern would be a randomise button, and
                // a take you cannot get back to is not one you can write down.
                ++checked;

                if (litBefore > 0 && litBefore == countLit()
                    && std::abs (processor.getStepChance (1, 3) - chanceStillDrawn) < 0.002f
                    && std::abs (processor.getStepVelocity (1, 3) - 1.0f) < 0.002f)
                    pass ("and the grid did not move: " + juce::String (litBefore)
                            + " steps lit, before and after");
                else
                    fail ("Reroll changed the pattern as well as the take ("
                            + juce::String (litBefore) + " steps lit before, "
                            + juce::String (countLit()) + " after)");

                clickToggle (*velocityButton, false);
                clickToggle (*chanceButton, false);

                ++checked;

                if (! stepGrid->isVelocityMode() && ! stepGrid->isChanceMode())
                    pass ("clearing both toggles leaves the grid painting steps");
                else
                    fail ("a toggle was cleared but the grid kept editing");

                processor.setPlaying (false);
            }
        }
    }

    //======================================================================
    // Row shifting, through the buttons on the strip.
    std::printf ("\n-- shifting a row from the strip --\n");

    if (anyStrip != nullptr)
    {
        juce::Button* shiftLeft = nullptr;
        juce::Button* shiftRight = nullptr;

        for (int i = 0; i < anyStrip->getNumChildComponents(); ++i)
        {
            auto* button = dynamic_cast<juce::TextButton*> (anyStrip->getChildComponent (i));

            if (button == nullptr)
                continue;

            const auto text = button->getButtonText();

            // The strip is channel 0, so the button text identifies the direction.
            if (text == juce::CharPointer_UTF8 ("\xe2\x86\x90"))
                shiftLeft = button;
            else if (text == juce::CharPointer_UTF8 ("\xe2\x86\x92"))
                shiftRight = button;
        }

        ++checked;

        if (shiftLeft == nullptr || shiftRight == nullptr)
        {
            fail ("the strip has no shift arrows");
        }
        else
        {
            pass ("the strip has both shift arrows");

            // Put a step on 0 with a velocity that identifies it.
            for (int s = 0; s < Layout::numSteps; ++s)
                processor.setStep (0, s, false);

            processor.setStep (0, 0, true);
            processor.setStepVelocity (0, 0, 0.3f);

            // Button::triggerClick posts its message rather than calling through,
            // and this test has no message loop to dispatch it, so the click
            // callback is invoked directly — the same thing a real click runs.
            const auto clickButton = [] (juce::Button& button)
            {
                if (button.onClick != nullptr)
                    button.onClick();
            };

            clickButton (*shiftRight);

            ++checked;

            if (! processor.isStepOn (0, 0) && processor.isStepOn (0, 1)
                && std::abs (processor.getStepVelocity (0, 1) - 0.3f) < 0.001f)
                pass ("the right arrow moves the row one step later, velocity included");
            else
                fail ("the right arrow did not shift the row"
                        + juce::String (processor.isStepOn (0, 1) ? "" : " (step 1 not set)")
                        + " velocity at 0 " + juce::String (processor.getStepVelocity (0, 0), 3)
                        + ", at 1 " + juce::String (processor.getStepVelocity (0, 1), 3));

            clickButton (*shiftLeft);

            ++checked;

            if (processor.isStepOn (0, 0) && std::abs (processor.getStepVelocity (0, 0) - 0.3f) < 0.001f)
                pass ("the left arrow moves it back");
            else
                fail ("the left arrow did not move the row back (velocity at 0 "
                        + juce::String (processor.getStepVelocity (0, 0), 3) + ")");
        }
    }

    //======================================================================
    // The grid paints through the undo manager, so the keyboard has to reach it:
    // without a handler, an edit that took a drag to make can only be taken back by
    // making another edit — and the panel gives no sign that a history exists at
    // all. This drives the editor's own key path, which is the half a host's focus
    // chain hands to us.
    std::printf ("\n-- undo and redo from the keyboard --\n");
    {
        for (int s = 0; s < Layout::numSteps; ++s)
            processor.setStep (0, s, false);

        processor.getUndoManager().clearUndoHistory();

        const auto cmd = juce::ModifierKeys::commandModifier;

        // Nothing in the history yet, so the key must not be claimed: a plugin that
        // answers every shortcut it recognises has eaten the host's.
        ++checked;

        if (! editor->keyPressed (juce::KeyPress ('z', cmd, 0)))
            pass ("with nothing to take back the key is left for the host");
        else
            fail ("Ctrl/Cmd+Z was claimed even though the history was empty");

        processor.beginTransaction ("paint step 5");
        processor.setStep (0, 4, true);

        ++checked;

        if (processor.isStepOn (0, 4))
            pass ("there is a gesture to take back");
        else
            fail ("the step was not set before the key was pressed");

        ++checked;

        if (editor->keyPressed (juce::KeyPress ('z', cmd, 0)))
            pass ("Ctrl/Cmd+Z is taken once there is something to undo");
        else
            fail ("Ctrl/Cmd+Z was not taken");

        ++checked;

        if (! processor.isStepOn (0, 4))
            pass ("and it takes the engine's step back, not only the stored one");
        else
            fail ("Ctrl/Cmd+Z left the step firing in the engine");

        ++checked;

        if (editor->keyPressed (juce::KeyPress ('Z', cmd | juce::ModifierKeys::shiftModifier, 0))
             && processor.isStepOn (0, 4))
            pass ("Ctrl/Cmd+Shift+Z puts it back");
        else
            fail ("Ctrl/Cmd+Shift+Z did not redo the edit");

        // Back once more, so the other spelling of redo has something to act on.
        editor->keyPressed (juce::KeyPress ('z', cmd, 0));

        ++checked;

        if (editor->keyPressed (juce::KeyPress ('y', cmd, 0))
             && processor.isStepOn (0, 4))
            pass ("Ctrl+Y is the same redo");
        else
            fail ("Ctrl+Y is not wired to the undo manager");

        // A plain letter typed with nothing held belongs to whoever has focus.
        ++checked;

        if (! editor->keyPressed (juce::KeyPress ('z', juce::ModifierKeys::noModifiers, 0)))
            pass ("a bare letter is not swallowed");
        else
            fail ("a bare Z was swallowed by the shortcut handler");
    }

    //======================================================================
    // The other number every step carries has to be visible without opening the
    // overlay that edits it, and this is the one part of the panel that can only be
    // checked by painting. The mark is not a control: it has no bounds, nothing to
    // click and no parameter behind it, so every component-inspecting check above
    // would be inspecting a rule that only the paint path ever applies. Two renders
    // that differ in exactly one number, compared pixel by pixel, is what proves both
    // halves of the claim — that the mark was drawn, and that nothing else moved.
    std::printf ("\n-- the mark on a step the take can drop --\n");
    {
        const int probeChannel = 0;
        const int probeStep = 7;   // not 0: earlier sections park the playhead there

        // One row, two neighbours, identical but for the chance: same channel colour,
        // same level, both switched on. Anything that differs between the two renders
        // therefore differs because of the probability and nothing else.
        for (int s = 0; s < Layout::numSteps; ++s)
        {
            processor.setStep (probeChannel, s, false);
            processor.setStepVelocity (probeChannel, s, 1.0f);
            processor.setStepChance (probeChannel, s, 1.0f);
        }

        processor.setStep (probeChannel, probeStep - 1, true);
        processor.setStep (probeChannel, probeStep, true);
        processor.setMutate (1.0f);

        stepGrid->setVelocityMode (false);
        stepGrid->setChanceMode (false);

        auto render = [stepGrid]
        {
            juce::Image image (juce::Image::ARGB,
                               juce::jmax (1, stepGrid->getWidth()),
                               juce::jmax (1, stepGrid->getHeight()), true);

            juce::Graphics g (image);
            stepGrid->paint (g);
            return image;
        };

        struct Diff
        {
            int pixels = 0;     // pixels that moved at all
            int outside = 0;    // ... and moved outside the window they were allowed to
            int darker = 0;     // moved down: the cut
            int brighter = 0;   // moved up: the lit edge of it
            long sumX = 0;
            long sumY = 0;
        };

        auto compare = [] (const juce::Image& a, const juce::Image& b,
                           const juce::Rectangle<int>& window)
        {
            Diff d;
            const int w = juce::jmin (a.getWidth(), b.getWidth());
            const int h = juce::jmin (a.getHeight(), b.getHeight());

            for (int y = 0; y < h; ++y)
            {
                for (int x = 0; x < w; ++x)
                {
                    const auto before = a.getPixelAt (x, y);
                    const auto after = b.getPixelAt (x, y);

                    if (before == after)
                        continue;

                    ++d.pixels;

                    if (! window.contains (x, y))
                        ++d.outside;

                    const auto luma = before.getRed() + before.getGreen() + before.getBlue()
                                    - after.getRed() - after.getGreen() - after.getBlue();

                    if (luma > 0)      ++d.darker;
                    else if (luma < 0) ++d.brighter;

                    d.sumX += x;
                    d.sumY += y;
                }
            }

            return d;
        };

        const auto cell = stepGrid->getCellBounds (probeChannel, probeStep);

        auto asText = [] (const juce::Rectangle<int>& r)
        {
            return juce::String (r.getX()) + " " + juce::String (r.getY()) + " "
                 + juce::String (r.getWidth()) + "x" + juce::String (r.getHeight());
        };

        // The cell, plus one pixel of slack: the mark's edge is drawn on the cell's own
        // boundary, and an antialiased line can bleed a pixel into the gap, which is
        // still this cell's business rather than its neighbour's.
        const auto window = cell.expanded (1);

        std::printf ("   probe cell (channel %d, step %d) at %s, grid %d x %d\n",
                     probeChannel, probeStep, asText (cell).toRawUTF8(),
                     stepGrid->getWidth(), stepGrid->getHeight());

        processor.setStepChance (probeChannel, probeStep, 1.0f);
        const auto certain = render();

        processor.setStepChance (probeChannel, probeStep, 0.5f);
        const auto mutableMarked = render();

        ++checked;
        {
            const auto d = compare (certain, mutableMarked, window);

            if (d.pixels > 0)
                pass ("a step that can be dropped is drawn differently from one that cannot");
            else
                fail ("a step at half chance painted exactly like a certain one — the grid still"
                        " hides the probability until its overlay is opened");

            ++checked;

            // Two-sided on purpose. "Nothing moved outside the cell" is satisfied by a
            // grid where nothing moved at all, which is the answer a missing mark gives;
            // locality only means something once there is a difference to be local to.
            if (d.pixels > 0 && d.outside == 0)
                pass ("and only that cell moved: the mark is local, not a repaint");
            else
                fail ("the mark moved " + juce::String (d.outside) + " of "
                        + juce::String (d.pixels) + " pixels outside its own cell");

            ++checked;

            if (d.darker > 0 && d.brighter > 0)
                pass ("it is a cut with a lit edge, not a recolour: "
                        + juce::String (d.darker) + " pixels down, "
                        + juce::String (d.brighter) + " up");
            else
                fail ("the mark drew " + juce::String (d.darker) + " darker and "
                        + juce::String (d.brighter)
                        + " brighter pixels, so one of its two colours is missing");

            ++checked;

            const auto centreX = (double) d.sumX / (double) juce::jmax (1, d.pixels);
            const auto centreY = (double) d.sumY / (double) juce::jmax (1, d.pixels);

            if (centreX > (double) cell.getX() + cell.getWidth() * 0.5
                 && centreY > (double) cell.getY() + cell.getHeight() * 0.5)
                pass ("and in the bottom-right corner, clear of the level the colour carries");
            else
                fail ("the mark centred at (" + juce::String (centreX, 1) + ", "
                        + juce::String (centreY, 1) + ") inside a cell at " + asText (cell));
        }

        // Three negative controls, each a pair of renders that must come back identical.
        auto mustMatch = [&compare] (const juce::Image& a, const juce::Image& b,
                                     const juce::Rectangle<int>& window)
        {
            return compare (a, b, window).pixels == 0;
        };

        // The mark reports the take, not the wish. With the variation shut nothing can
        // be dropped, and a warning about a thing that cannot happen is a lie — the same
        // rule the README states as "shut, the grid is law".
        ++checked;
        {
            processor.setMutate (0.0f);
            processor.setStepChance (probeChannel, probeStep, 1.0f);
            const auto shutCertain = render();

            processor.setStepChance (probeChannel, probeStep, 0.5f);
            const auto shutMutable = render();

            processor.setMutate (1.0f);

            if (mustMatch (shutCertain, shutMutable, window))
                pass ("with the variation shut it paints nothing: an uncertain step is law too");
            else
                fail ("a step the machine is not allowed to drop was still marked");
        }

        // An off step is not waiting for the take; it is not in it. Its probability is
        // stored and inert, so marking it would say "this may not happen" about a cell
        // that is already empty.
        ++checked;
        {
            processor.setStep (probeChannel, probeStep, false);
            processor.setStepChance (probeChannel, probeStep, 1.0f);
            const auto offCertain = render();

            processor.setStepChance (probeChannel, probeStep, 0.5f);
            const auto offMutable = render();

            processor.setStep (probeChannel, probeStep, true);
            processor.setStepChance (probeChannel, probeStep, 1.0f);

            if (mustMatch (offCertain, offMutable, window))
                pass ("and a step switched off is not marked, whatever it stores");
            else
                fail ("an empty cell was marked as if it had a hit to lose");
        }

        // Inside an overlay the bar owns the cell, and the number is on screen already.
        // A notch cut here would be eating the thing the overlay was opened to show —
        // so a chance that changes must move nothing while the velocity bars are up.
        //
        // The level is deliberately low. A velocity of 1.0 draws a bar over the whole
        // cell, and a bar that covers the corner hides a mark that leaked into the
        // overlay: the check would pass on a broken build. At a tenth the bar is three
        // pixels tall and the notch is ten, so the seven pixels above it are exactly
        // where a leaked mark has to show.
        ++checked;
        {
            stepGrid->setVelocityMode (true);

            for (int s = 0; s < Layout::numSteps; ++s)
                processor.setStepVelocity (probeChannel, s, 0.1f);

            processor.setStepChance (probeChannel, probeStep, 1.0f);
            const auto overlayCertain = render();

            processor.setStepChance (probeChannel, probeStep, 0.5f);
            const auto overlayMutable = render();

            processor.setStepChance (probeChannel, probeStep, 1.0f);

            for (int s = 0; s < Layout::numSteps; ++s)
                processor.setStepVelocity (probeChannel, s, 1.0f);

            stepGrid->setVelocityMode (false);

            if (mustMatch (overlayCertain, overlayMutable, window))
                pass ("inside an overlay the mark stands down and leaves the bar alone");
            else
                fail ("the mark drew itself over the velocity bar");
        }

        // The rule the pixels were obtained under, stated once, so a future change to
        // one of the four arms has to disagree with a render above to get through.
        ++checked;
        {
            processor.setStepChance (probeChannel, probeStep, 0.5f);

            const auto atRest = stepGrid->isStepMutable (probeChannel, probeStep);

            stepGrid->setChanceMode (true);
            const auto inOverlay = stepGrid->isStepMutable (probeChannel, probeStep);
            stepGrid->setChanceMode (false);

            processor.setMutate (0.0f);
            const auto knobShut = stepGrid->isStepMutable (probeChannel, probeStep);
            processor.setMutate (1.0f);

            processor.setStep (probeChannel, probeStep, false);
            const auto whileOff = stepGrid->isStepMutable (probeChannel, probeStep);
            processor.setStep (probeChannel, probeStep, true);

            if (atRest && ! inOverlay && ! knobShut && ! whileOff)
                pass ("the rule says: at rest mark it, in an overlay no, knob shut no, step off no");
            else
                fail ("the mark's rule came back at rest " + juce::String ((int) atRest)
                        + ", in an overlay " + juce::String ((int) inOverlay)
                        + ", knob shut " + juce::String ((int) knobShut)
                        + ", step off " + juce::String ((int) whileOff));
        }

        // Back as found: the knob is what a fresh session opens with.
        processor.setMutate (0.0f);

        for (int s = 0; s < Layout::numSteps; ++s)
            processor.setStep (probeChannel, s, false);
    }

    std::printf ("\n%s  %d checks, %d problems\n",
                 problems == 0 ? "PASSED" : "FAILED", checked, problems);

    return problems == 0 ? 0 : 1;
}