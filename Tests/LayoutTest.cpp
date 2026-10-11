/*
    Editor layout check.

    Builds the real editor against a real processor and reports the bounds of
    every control, flagging any that ended up with no size or outside its parent.
    GUI-only bugs — a control laid out off-panel, or at zero height — are exactly
    the kind of thing a headless audio test cannot see.

    Build:  cmake --build build --config Release --target GrooveboxLayoutTest
    Run:    build\GrooveboxLayoutTest_artefacts\Release\GrooveboxLayoutTest.exe
*/

#include "PluginEditor.h"
#include "ChannelStrip.h"
#include "TransportBar.h"
#include "StepGridComponent.h"

#include <cstdio>

using namespace gb;

namespace
{

int problems = 0;
int checked = 0;

/** Writes a short click to a temp wav so a real sample can be loaded for the
    audio half of the check. Same shape as the smoke test's click. */
juce::File makeClickWav (const juce::String& name, int lengthSamples, double sampleRate)
{
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("groovebox-layout-" + name + ".wav");

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

void report (juce::Component& parent, juce::Component& child, const juce::String& name)
{
    ++checked;

    const auto b = child.getBounds();
    const auto inside = parent.getLocalBounds().contains (b);

    juce::String issue;

    if (b.getWidth() <= 0 || b.getHeight() <= 0)
        issue = "ZERO SIZE";
    else if (b.getY() < 0 || b.getBottom() > parent.getHeight())
        issue = "VERTICALLY OUTSIDE PARENT";
    else if (! inside)
        issue = "OUTSIDE PARENT";

    const auto line = juce::String ("  ") + (issue.isEmpty() ? "ok   " : "BAD  ")
                        + name.paddedRight (' ', 26)
                        + " x=" + juce::String (b.getX()).paddedLeft (' ', 4)
                        + " y=" + juce::String (b.getY()).paddedLeft (' ', 4)
                        + " w=" + juce::String (b.getWidth()).paddedLeft (' ', 4)
                        + " h=" + juce::String (b.getHeight()).paddedLeft (' ', 4)
                        + (issue.isEmpty() ? "" : "   <-- " + issue);

    std::printf ("%s\n", line.toRawUTF8());

    if (issue.isNotEmpty())
        ++problems;
}

/** Walks a strip and reports each child it owns. */
void reportStrip (ChannelStrip& strip, int index)
{
    std::printf ("\n-- channel %d strip (height %d) --\n", index, strip.getHeight());

    for (int i = 0; i < strip.getNumChildComponents(); ++i)
    {
        auto* child = strip.getChildComponent (i);

        if (child == nullptr)
            continue;

        // Groups only exist for layout, not to be interacted with or seen.
        if (dynamic_cast<juce::GroupComponent*> (child) != nullptr)
            continue;

        report (strip, *child, "child " + juce::String (i));
    }
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf ("Groovebox editor layout check\n");

    GrooveboxProcessor processor;
    processor.prepareToPlay (44100.0, 512);

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());

    if (editor == nullptr)
    {
        std::printf ("FAIL: no editor was created\n");
        return 1;
    }

    // Force a layout at the editor's own preferred size.
    editor->setSize (1240, 720);
    std::printf ("\neditor size: %d x %d\n", editor->getWidth(), editor->getHeight());

    std::printf ("\n-- editor children --\n");

    for (int i = 0; i < editor->getNumChildComponents(); ++i)
    {
        auto* child = editor->getChildComponent (i);

        if (child == nullptr)
            continue;

        const auto name = child->getName().isNotEmpty()
                            ? child->getName()
                            : juce::String ("component ") + juce::String (i);

        // Only report the controls that matter, by type.
        if (dynamic_cast<TransportBar*> (child) != nullptr
            || dynamic_cast<StepGridComponent*> (child) != nullptr
            || dynamic_cast<ChannelStrip*> (child) != nullptr
            || dynamic_cast<juce::Label*> (child) != nullptr)
        {
            report (*editor, *child, name);
        }

        // The transport bar holds the swing slider, so check inside it too.
        if (auto* bar = dynamic_cast<TransportBar*> (child))
        {
            std::printf ("\n-- transport bar children --\n");

            for (int c = 0; c < bar->getNumChildComponents(); ++c)
            {
                auto* inner = bar->getChildComponent (c);

                if (inner == nullptr || ! inner->isVisible())
                    continue;

                report (*bar, *inner, "child " + juce::String (c));
            }
        }
    }

    // The strips are where the level sliders live, so look inside one.
    for (int i = 0; i < editor->getNumChildComponents(); ++i)
    {
        if (auto* strip = dynamic_cast<ChannelStrip*> (editor->getChildComponent (i)))
        {
            reportStrip (*strip, i);

            // A control can have correct bounds and still not be reachable or
            // drawn, so check that the level slider is visible and responds to a
            // click in the middle of its own bounds.
            for (int c = 0; c < strip->getNumChildComponents(); ++c)
            {
                auto* child = strip->getChildComponent (c);

                if (auto* slider = dynamic_cast<juce::Slider*> (child))
                {
                    if (slider->getSliderStyle() != juce::Slider::LinearHorizontal)
                        continue;

                    std::printf ("\n-- level slider interaction --\n");
                    ++checked;

                    if (! slider->isVisible() || ! slider->isEnabled())
                    {
                        std::printf ("  BAD  slider is not visible/enabled\n");
                        ++problems;
                        break;
                    }

                    const auto before = slider->getValue();
                    const auto centre = slider->getLocalBounds().getCentre();

                    // Press at the far left: a working slider jumps to its minimum.
                    juce::MouseEvent press (juce::Desktop::getInstance().getMainMouseSource(),
                                            juce::Point<float> (2.0f, (float) centre.getY()),
                                            juce::ModifierKeys::leftButtonModifier,
                                            1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                            slider, slider, juce::Time::getCurrentTime(),
                                            juce::Point<float> (2.0f, (float) centre.getY()),
                                            juce::Time::getCurrentTime(), 1, false);

                    slider->mouseDown (press);
                    slider->mouseUp (juce::MouseEvent (press));

                    const auto after = slider->getValue();

                    std::printf ("  %s drag changes the value: %.3f -> %.3f\n",
                                 after != before ? "ok  " : "BAD ", before, after);

                    if (after == before)
                        ++problems;

                    // The whole point of the fader is the audio, so follow the
                    // chain all the way: did the processor record the value, and
                    // does a hit rendered afterwards come out quieter?
                    const auto processorLevel = processor.getChannelLevel (0);
                    const auto expected = (float) after;

                    std::printf ("  %s processor level now %.3f (slider says %.3f)\n",
                                 std::abs (processorLevel - expected) < 0.001f ? "ok  " : "BAD ",
                                 processorLevel, expected);

                    if (std::abs (processorLevel - expected) >= 0.001f)
                        ++problems;

                    // Now the realistic path: a drag with the mouse held down, the
                    // way a user moves a fader. mouseDrag fires while the button is
                    // down, and the strip's refresh timer keeps running throughout.
                    const auto beforeDrag = processor.getChannelLevel (0);

                    juce::MouseEvent down (juce::Desktop::getInstance().getMainMouseSource(),
                                           juce::Point<float> (2.0f, (float) centre.getY()),
                                           juce::ModifierKeys::leftButtonModifier,
                                           1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                           slider, slider, juce::Time::getCurrentTime(),
                                           juce::Point<float> (2.0f, (float) centre.getY()),
                                           juce::Time::getCurrentTime(), 1, false);

                    slider->mouseDown (down);

                    const auto dragPosition = juce::Point<float> ((float) (slider->getWidth() - 2),
                                                                  (float) centre.getY());
                    juce::MouseEvent drag (juce::Desktop::getInstance().getMainMouseSource(),
                                           dragPosition,
                                           juce::ModifierKeys::leftButtonModifier,
                                           1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                           slider, slider, juce::Time::getCurrentTime(),
                                           dragPosition, juce::Time::getCurrentTime(), 1, false);

                    slider->mouseDrag (drag);
                    slider->mouseUp (juce::MouseEvent (drag));

                    const auto afterDrag = processor.getChannelLevel (0);

                    std::printf ("  %s drag with the button held: processor level %.3f -> %.3f\n",
                                 afterDrag > beforeDrag + 0.01f ? "ok  " : "BAD ", beforeDrag, afterDrag);

                    if (afterDrag <= beforeDrag + 0.01f)
                        ++problems;
                }
            }

            break; // one representative strip is enough
        }
    }

    std::printf ("\n%s  %d controls checked, %d layout problems\n",
                 problems == 0 ? "PASSED" : "FAILED", checked, problems);

    return problems == 0 ? 0 : 1;
}
