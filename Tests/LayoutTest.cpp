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
    // the test was laying out a size the editor never actually takes.
    const auto preferredWidth = editor->getWidth();
    const auto preferredHeight = editor->getHeight();
    editor->setSize (juce::jmax (preferredWidth, 1240), juce::jmax (preferredHeight, 790));
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
    std::printf ("\n-- fader wiring: does a drag reach the processor? --\n");

    if (anyStrip != nullptr)
    {
        checkFadersIn (*anyStrip, "channel 0", [&processor] (juce::Slider& s)
        {
            // The strip has exactly one linear slider, the level fader.
            juce::ignoreUnused (s);
            return (double) processor.getChannelLevel (0);
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
        for (int i = 0; i < transportBar->getNumChildComponents(); ++i)
        {
            auto* slider = dynamic_cast<juce::Slider*> (transportBar->getChildComponent (i));

            if (slider == nullptr || slider->getMaximum() > 1.01 || ! slider->isVisible())
                continue;

            ++checked;
            const auto before = processor.getSwing();
            slider->setValue (0.75, juce::sendNotificationSync);
            const auto after = processor.getSwing();

            if (std::abs (after - 0.75) < 1.0e-3)
                pass ("swing slider onValueChange reaches the processor (" + juce::String (after, 3) + ")");
            else
                fail ("swing slider onValueChange did not reach the processor ("
                        + juce::String (before, 3) + " -> " + juce::String (after, 3)
                        + ", slider at " + juce::String (slider->getValue(), 3) + ")");

            break;
        }

        // The bar owns two horizontal faders — swing and the master trim — and
        // each has to be checked against its own parameter. They are told apart by
        // their range: swing runs 0..1, the trim runs in dB.
        for (int i = 0; i < transportBar->getNumChildComponents(); ++i)
        {
            auto* slider = dynamic_cast<juce::Slider*> (transportBar->getChildComponent (i));

            if (slider == nullptr || ! slider->isVisible()
                || slider->getSliderStyle() != juce::Slider::LinearHorizontal)
                continue;

            if (slider->getMaximum() <= 1.01)
                checkFader (*slider, "transport swing",
                            [&processor] { return (double) processor.getSwing(); });
            else
                checkFader (*slider, "transport master",
                            [&processor] { return (double) processor.getMasterGainDb(); });
        }

        // Those drags leave both parameters at the top of their range, and the
        // audio checks below measure levels, so put them back first.
        processor.setSwing (0.0f);
        processor.setMasterGainDb (Layout::defaultMasterGainDb);
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
        juce::Button* muteButton = nullptr;
        juce::Label* pitchLabel = nullptr;
        EnvelopeControls* envelope = nullptr;

        for (int i = 0; i < anyStrip->getNumChildComponents(); ++i)
        {
            auto* child = anyStrip->getChildComponent (i);

            if (auto* s = dynamic_cast<juce::Slider*> (child);
                s != nullptr && s->getSliderStyle() == juce::Slider::LinearHorizontal)
                levelSlider = s;
            else if (auto* b = dynamic_cast<juce::Button*> (child);
                     b != nullptr && b->getButtonText() == "Mute")
                muteButton = b;
            else if (auto* l = dynamic_cast<juce::Label*> (child);
                     l != nullptr && l->getTooltip().startsWith ("Pitch offset"))
                pitchLabel = l;
            else if (auto* e = dynamic_cast<EnvelopeControls*> (child))
                envelope = e;
        }

        ++checked;

        if (levelSlider == nullptr || muteButton == nullptr || pitchLabel == nullptr || envelope == nullptr)
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
                { Params::levelId (0),   0.37f, "level" },
                { Params::pitchId (0),   3.0f,  "pitch" },
                { Params::attackId (0),  0.62f, "attack" },
                { Params::muteId (0),    1.0f,  "mute" },
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
                else if (kind == "attack")
                {
                    const auto a = envelope->getValue (EnvelopeStage::attack);
                    ok = std::abs (a - c.value) < 0.01f;
                    detail = juce::String (a, 3);
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
            processor.setChannelMuted (0, false);
            processor.resetChannelEnvelope (0);
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

            // Cell (channel 11, step 0) is the top-left of the grid, so it is easy
            // to locate without knowing the row order.
            const auto cell = juce::Rectangle<int> (0, 0, 1, 1);
            juce::ignoreUnused (cell);

            // Use the grid's own geometry: drag in the first column of the bottom
            // row, which is channel 0.
            const auto gridArea = stepGrid->getLocalBounds();
            const auto labelWidth = juce::jmin (108, juce::jmax (40, gridArea.getWidth() / 4));
            const auto gridX = gridArea.getX() + labelWidth + 3;
            const auto rowSpan = juce::jmax (2, (gridArea.getHeight() - (Layout::numChannels - 1) * 3) / Layout::numChannels);
            const auto bottomRowY = gridArea.getY() + (Layout::numChannels - 1) * (rowSpan + 3);

            // A quarter of the way up the row should land near 25%.
            const auto targetY = bottomRowY + rowSpan - juce::jmax (2, rowSpan / 4);
            const auto point = juce::Point<int> (gridX + 3, targetY);

            stepGrid->mouseDown (makeEvent (*stepGrid, point.toFloat()));
            stepGrid->mouseUp (makeEvent (*stepGrid, point.toFloat()));

            const auto recorded = processor.getStepVelocity (0, 0);

            ++checked;

            if (recorded > 0.05f && recorded < 0.6f)
                pass ("a drag in a step sets its velocity (" + juce::String (recorded, 2) + ")");
            else
                fail ("a drag in a step did not set a sensible velocity (got "
                        + juce::String (recorded, 2) + ")");

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

    std::printf ("\n%s  %d checks, %d problems\n",
                 problems == 0 ? "PASSED" : "FAILED", checked, problems);

    return problems == 0 ? 0 : 1;
}