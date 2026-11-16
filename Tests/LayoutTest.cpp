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

//==============================================================================
// Pixel evidence.
//
// Two renders that differ in exactly one number, compared pixel by pixel, is the only
// way to check a claim about what the grid *draws* rather than what it holds: a mark, a
// ring, a word. Everything here is file-scope because there are now two sections that
// need it and the comparison is the part that must not be written twice — the counting
// of darker against brighter, and the window a difference is allowed to fall in, are
// the whole content of the claim.

struct Diff
{
    int pixels = 0;     // pixels that moved at all
    int outside = 0;    // ... and moved outside the window they were allowed to
    int darker = 0;     // moved down
    int brighter = 0;   // moved up
    long sumX = 0;
    long sumY = 0;
};

Diff compareImages (const juce::Image& a, const juce::Image& b,
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
}

/** A pair of renders that must come back identical. */
bool mustMatch (const juce::Image& a, const juce::Image& b)
{
    return compareImages (a, b, juce::Rectangle<int> {}).pixels == 0;
}

/**
    How far two colours are, as the sum of their three channel gaps.
    The one metric the two gates below and `nearestPixel` all read, so a limit stated in
    it means the same thing in all three places.
*/
float colourDistance (const juce::Colour& a, const juce::Colour& b)
{
    auto gap = [] (float x, float y) { return x > y ? x - y : y - x; };

    return gap (a.getFloatRed(),   b.getFloatRed())
         + gap (a.getFloatGreen(), b.getFloatGreen())
         + gap (a.getFloatBlue(),  b.getFloatBlue());
}

/**
    How close the pixel in `area` that most resembles `wanted` actually gets to it.
    An absolute read of one render, not a difference between two.

    Needed because a difference can only ever say *these pictures are not the same*. It
    cannot say which control made the difference when the state that flips that control's
    answer flips something else with it — which is exactly the case for the copy glyph's
    dimming (whether a copy is possible is a fact about the other slots' fullness, and the
    marks report that same fullness) and for the rotate chip's light (`rotating` drives the
    colour *and* the word, so the word alone satisfies any pair). Measured against those, a
    mutation that leaves the word changing and kills the colour is invisible to a diff and
    plain here: the glyph's rectangle keeps its distance from the bright colour whatever
    the marks are doing, and so does the chip's from its lane's own colour.

    The nearest-pixel form rather than a count is deliberate: a 1 px stroke drawn on a
    rounded rectangle straddles pixel boundaries, so some of its pixels land on the colour
    exactly and some are half-covered blends, and a count with a tight threshold would be a
    statement about JUCE's antialiasing rather than about the plugin. Distance is the sum of
    the three channel gaps — the palette's alternatives are a whole channel apart, and
    `wanted` must be handed in already composited over the panel, because `Colour` keeps an
    alpha'd colour's channels at full strength and the paint does not.

    `getFloatRed` and not `getRed`: in this JUCE the plain one is a `uint8`, so a difference
    of them is integer arithmetic on 0..255 and would silently compare a different quantity
    from the one the 0.15 threshold is stated in.
*/
float nearestPixel (const juce::Image& image, const juce::Rectangle<int>& area,
                    const juce::Colour& wanted)
{
    float best = 10.0f;

    for (int y = juce::jmax (0, area.getY()); y < juce::jmin (image.getHeight(), area.getBottom()); ++y)
        for (int x = juce::jmax (0, area.getX()); x < juce::jmin (image.getWidth(), area.getRight()); ++x)
            best = juce::jmin (best, colourDistance (image.getPixelAt (x, y), wanted));

    return best;
}

/** What a colour would look like once the painter's own alpha lands on the panel. */
juce::Colour composited (const juce::Colour& over)
{
    const auto a = over.getFloatAlpha();

    return juce::Colour::fromFloatRGBA (a * over.getFloatRed()   + (1 - a) * gb::Colours::background.getFloatRed(),
                                        a * over.getFloatGreen() + (1 - a) * gb::Colours::background.getFloatGreen(),
                                        a * over.getFloatBlue()  + (1 - a) * gb::Colours::background.getFloatBlue(),
                                        1.0f);
}

juce::String asText (const juce::Rectangle<int>& r)
{
    return juce::String (r.getX()) + " " + juce::String (r.getY()) + " "
         + juce::String (r.getWidth()) + "x" + juce::String (r.getHeight());
}

/**
    Whether one rectangle is drawn in the lit colour in one render and in the shut colour
    in the other.

    Both colours are read, not just the lit one, because "the lit colour is missing from
    the shut render" is only a statement about this control if the *shut* colour is there
    instead — a rectangle that holds neither is a rectangle this gate has not looked at.
    What is not asserted is the reverse, that the lit render holds no shut colour: the
    antialiased edge of a word drawn in full-strength white passes through every grey on
    its way to the panel, including the one the dim border is, so that condition would be
    a claim about JUCE's text antialiasing rather than about the plugin.

    The margin is half the palette's own distance between the two colours rather than a
    typed number, which is how the gate fails safe: a theme in which lit and shut are
    nearly the same colour asks for nearly nothing, and says so by going red on the
    painting, instead of trusting a constant somebody had to believe.
*/
struct LitShut
{
    bool ok = false;
    juce::String detail;
};

LitShut readsAs (const juce::Image& lit, const juce::Image& shut,
                 const juce::Rectangle<int>& area,
                 const juce::Colour& litColour, const juce::Colour& shutColour,
                 juce::String what)
{
    const auto separation = colourDistance (litColour, shutColour);
    const auto margin = 0.5f * separation;

    const auto litHasLit   = nearestPixel (lit, area, litColour);
    const auto shutHasLit  = nearestPixel (shut, area, litColour);
    const auto shutHasShut = nearestPixel (shut, area, shutColour);

    LitShut r;

    r.ok = litHasLit < shutHasLit - margin && shutHasShut <= margin;

    r.detail = what + ": nearest pixel to the lit colour is " + juce::String (litHasLit, 3)
             + " away when lit and " + juce::String (shutHasLit, 3) + " when shut; the shut colour"
             " itself sits " + juce::String (shutHasShut, 3) + " from the shut render. The palette"
             " puts those two colours " + juce::String (separation, 3)
             + " apart, so the gate asks for half of it [" + asText (area) + "]";

    return r;
}

/**
    The grid painted into an image of its own size.

    `StepGridComponent::paint` is public, so this runs the painter headless — no window,
    no screenshot, and nothing clipped away by what the desktop happens to be covering.
*/
juce::Image renderGrid (StepGridComponent& grid)
{
    juce::Image image (juce::Image::ARGB,
                       juce::jmax (1, grid.getWidth()),
                       juce::jmax (1, grid.getHeight()), true);

    juce::Graphics g (image);
    grid.paint (g);
    return image;
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
        // It used to be the leftover of the layout rectangle, and `Rectangle::removeFromTop`
        // clamps, so that number was 0 on a strip 44 px short and this line could not
        // go red. It is the plan's own deficit now.
        ++checked;

        if (anyStrip->getSpareHeight() >= 0)
            pass ("strip has " + juce::String (anyStrip->getSpareHeight()) + " px to spare");
        else
            fail ("strip is " + juce::String (-anyStrip->getSpareHeight()) + " px too short");
    }

    //======================================================================
    //  The strip's height rule, asked in its own numbers.
    //
    //  The walk below lays out real components, which is how it catches a fader being
    //  squeezed; it cannot catch this rule being wrong at a size no arm happens to visit.
    //  And the rule is 5.6's remaining half on its own: what shipped was two sets of row
    //  sizes costing 268 px and 228 px of room, and a threshold at 219 — below the cost of
    //  both, so the compact branch had never fitted at any height, and the roomy one
    //  stopped fitting 49 px earlier than anyone thought. Only a gate over the function
    //  can say *for every room*, so that is what this is: every claim is arithmetic on
    //  `Layout::stripPlan`, and none of it borrows `ChannelStrip::resized`.
    std::printf ("\n-- the strip height rule, at every room --\n");
    {
        // What one layout pass actually consumes at a given room, gaps and captions
        // included, using the sizes the plan returned rather than the ones it wanted. This
        // is the quantity that has to stay under the room: above it, `removeFromTop`
        // clamps and a control gets a zero-height rectangle.
        const auto used = [] (const Layout::StripPlan& p)
        {
            return 5 * p.sectionGap + p.soundGap + 4 * p.caption
                   + p.header + p.sound + p.clearRow + p.pitchRow
                   + p.dropRow + p.level + p.envelope + p.muteRow;
        };

        // 1. The four budgets are the four costs, and each is the *smallest* room its stage
        //    fits. Nothing here types a size: `wanted` is the plan's own sum, so a row that
        //    changes without its budget moving shows up as a disagreement between the two.
        const int budgets[] = { Layout::stripFullRoom, Layout::stripTightRoom,
                                Layout::stripFloorRoom, Layout::stripBareRoom };

        for (int stage = 0; stage < 4; ++stage)
        {
            const auto at = Layout::stripPlan (budgets[stage]);

            expect (at.stage == stage && at.wanted == budgets[stage] && at.spare == 0,
                    "budget " + juce::String (budgets[stage]) + " is exactly what stage "
                        + juce::String (stage) + " costs",
                    juce::String (at.wanted) + " px wanted at stage " + juce::String (at.stage)
                        + ", spare " + juce::String (at.spare));

            // The boundary, from the other side: one px less and the strip has to give
            // something up. Stage 3 is the exception and is said next, because it has no
            // cheaper stage to fall to.
            if (stage < 3)
            {
                const auto under = Layout::stripPlan (budgets[stage] - 1);

                expect (under.stage == stage + 1,
                        "one px under it the plan takes the next stage down",
                        juce::String (budgets[stage] - 1) + " px gives stage "
                            + juce::String (under.stage) + ", want " + juce::String (stage + 1));
            }
            else
            {
                const auto under = Layout::stripPlan (budgets[stage] - 1);

                // This is the claim that makes the whole exercise worth doing: below the
                // cheapest stage the plan does not quietly over-allocate, it reports the
                // shortfall, and the ADSR row is what pays for it.
                expect (under.stage == 3 && under.spare == -1 && under.envelope == 43
                          && used (under) == budgets[3] - 1,
                        "and below the last budget it says so instead of lying",
                        juce::String (budgets[3] - 1) + " px gives stage "
                            + juce::String (under.stage) + ", spare "
                            + juce::String (under.spare) + ", envelope "
                            + juce::String (under.envelope));
            }
        }

        // 2. Totality, over every room a strip can be given, including the negative side of
        //    the boundary. `used <= room` is the whole repair: it means no row was clamped,
        //    which is the same thing as no control being laid out with nothing.
        auto overflowed = 0, firstOver = -1, honest = 0;

        for (auto room = Layout::stripHardRoom; room <= 400; ++room)
        {
            const auto p = Layout::stripPlan (room);

            if (used (p) > room)
            {
                if (firstOver < 0)
                    firstOver = room;

                ++overflowed;
            }

            // The plan spends what it is given when it can, and no more: a rule that
            // under-used the room would pass the check above and still leave the strip
            // shorter than it had to be.
            if (used (p) == juce::jmin (p.wanted, room))
                ++honest;
        }

        expect (overflowed == 0 && honest == 400 - Layout::stripHardRoom + 1,
                "from the hard floor up, nothing is clamped and nothing is wasted",
                juce::String (overflowed) + " of "
                    + juce::String (400 - Layout::stripHardRoom + 1) + " rooms overflow"
                    + (firstOver < 0 ? juce::String() : " (first at " + juce::String (firstOver) + ")")
                    + ", " + juce::String (honest) + " spend what they are given");

        // The other side of that bound, because a floor nobody derived is a wide limit. At
        // one px below `stripHardRoom` every row is already at its own minimum, so the
        // consumption cannot come down any further and sits on the floor itself: the
        // number is derived, and 123 is 112 px of rows + 10 of gaps + the 1 px the envelope
        // will squeeze to.
        expect (used (Layout::stripPlan (Layout::stripHardRoom - 1)) == Layout::stripHardRoom
                  && Layout::stripPlan (Layout::stripHardRoom).envelope == 1
                  && Layout::stripPlan (Layout::stripHardRoom + 1).envelope == 2,
                "the hard floor is the px where the envelope runs out, not a rounded guess",
                "room " + juce::String (Layout::stripHardRoom - 1) + " needs "
                    + juce::String (used (Layout::stripPlan (Layout::stripHardRoom - 1)))
                    + "; envelope is " + juce::String (Layout::stripPlan (Layout::stripHardRoom).envelope)
                    + " px there and "
                    + juce::String (Layout::stripPlan (Layout::stripHardRoom + 1).envelope)
                    + " one px up");

        // 3. Monotone, and no control row is ever given up entirely. A taller window is not
        //    allowed to buy a shorter button: that is the property the old two-set rule held
        //    between stages and lost inside them, and it is what makes the walk below able to
        //    claim the compact path is *served* rather than merely *chosen*.
        auto backwards = 0, zeroRow = 0, halfCaption = 0;
        Layout::StripPlan previous = Layout::stripPlan (0);

        for (auto room = 1; room <= 400; ++room)
        {
            const auto p = Layout::stripPlan (room);

            const bool shrunk = p.header < previous.header || p.sound < previous.sound
                                || p.clearRow < previous.clearRow || p.pitchRow < previous.pitchRow
                                || p.dropRow < previous.dropRow || p.level < previous.level
                                || p.envelope < previous.envelope || p.muteRow < previous.muteRow
                                || p.caption < previous.caption || p.sectionGap < previous.sectionGap
                                || p.soundGap < previous.soundGap;

            backwards += shrunk;
            halfCaption += p.caption != 0 && p.caption != 11;

            const auto smallest = juce::jmin (juce::jmin (p.header, p.sound),
                                              juce::jmin (juce::jmin (p.clearRow, p.pitchRow),
                                                          juce::jmin (juce::jmin (p.dropRow, p.level),
                                                                      juce::jmin (p.envelope, p.muteRow))));

            zeroRow += smallest < 1;
            previous = p;
        }

        expect (backwards == 0 && zeroRow == 0 && halfCaption == 0,
                "taller never means shorter, no control row is ever 0, captions are all or nothing",
                juce::String (backwards) + " backwards, " + juce::String (zeroRow)
                    + " with a row squeezed away, " + juce::String (halfCaption)
                    + " with a caption at a size paintCaption would not draw");

        // 4. The room the probe measured, and the two numbers a laptop has to live with.
        //    236 px of strip is what a 500 px window gave, and it is where the old rule lost
        //    two of seventeen controls; 280 px is the smallest strip that still gets the
        //    layout the design asks for, which is what 5.6's height half has to buy.
        expect (Layout::stripRoomFor (236) == 224
                  && Layout::stripPlan (224).spare == 14 && used (Layout::stripPlan (224)) == 210
                  && Layout::stripRoomFor (Layout::stripFullRoom + 2 * Layout::stripFrame)
                       == Layout::stripFullRoom,
                "the probe's room is served now, and the roomy layout starts at 280 px of strip",
                "236 px strip -> room " + juce::String (Layout::stripRoomFor (236))
                    + ", stage " + juce::String (Layout::stripPlan (224).stage) + ", spare "
                    + juce::String (Layout::stripPlan (224).spare) + ", full design from "
                    + juce::String (Layout::stripFullRoom + 2 * Layout::stripFrame) + " px");
    }

    //======================================================================
    //  The editor's own height rule, at every stage it can be at.
    //
    //  5.6's other half. The width half was paid for by 4.3b; what was left was a typed
    //  844 that `setResizeLimits` passed straight through as the *minimum* -- 149 px above
    //  what the layout needs at its cheapest stage, and enough to sit the bottom strip of
    //  the window off a 768 px screen. The repair is a plan of four stages, each giving up
    //  one thing in a fixed order: the GROOVEBOX row, then the strip's slack and row
    //  heights, then its captions. The grid is never asked to pay, because the grid's floor
    //  is the row height the bank strip needs to carry a name without ellipsising it.
    //
    //  The floor is priced against the *wrapped* bar on purpose. The minimum width the same
    //  rule publishes is below `TransportBar::oneRowWidth`, so the one corner where both
    //  limits are met at once has a two-line bar, and a height floor derived against the
    //  one-line bar would be 36 px short there. That coupling is asserted below rather than
    //  assumed, because it is the claim that decides which of the two is the worst case.
    std::printf ("\n-- the editor's height rule, at every stage --\n");
    {
        std::vector<ChannelStrip*> walks;

        for (int i = 0; i < editor->getNumChildComponents(); ++i)
        {
            if (auto* strip = dynamic_cast<ChannelStrip*> (editor->getChildComponent (i)))
                walks.push_back (strip);
        }

        const auto gridFloor = StepGridComponent::heightForRowAtLeast (StepGridComponent::minimumRowHeight);
        const auto tightBar  = TransportBar::twoRowHeight;
        const auto chrome    = Layout::editorChromeHeight (tightBar, gridFloor);

        // Every term here belongs to the component that owns it -- the bar's second line,
        // the strip's own budget for the stage it is priced at, the grid's row rule read
        // backwards -- except the editor's chrome, which is this rule's own vocabulary.
        // `editorCostForStage` is deliberately not called: AGENTS.md records what happens to
        // a gate that asks the predicate under test what it expects, and this is the rule
        // whose cost a mistake here would hide.
        const int costs[] = {
            Layout::wordmarkHeight + Layout::stripDesignHeight + chrome,
            Layout::stripDesignHeight + chrome,
            Layout::stripFloorRoom + 2 * Layout::stripFrame + chrome,
            Layout::stripBareRoom  + 2 * Layout::stripFrame + chrome };

        // 1. Each stage begins exactly where its own terms cost that much, and one px under
        //    a boundary the plan takes the stage below it. `spare == 0` at the cost is what
        //    makes these boundaries rather than three points on a slope; below the last one
        //    there is nothing left to give up, so the plan has to say so instead.
        auto stageWrong = 0, budgetWrong = 0, boundaryWrong = 0;

        for (int stage = 0; stage < 4; ++stage)
        {
            const auto at = Layout::editorPlan (costs[stage], tightBar, gridFloor);

            stageWrong  += at.stage != stage;
            budgetWrong += at.spare != 0 || at.wanted != costs[stage];

            const auto under = Layout::editorPlan (costs[stage] - 1, tightBar, gridFloor);

            if (stage < 3)
                boundaryWrong += under.stage != stage + 1;
            else
                boundaryWrong += under.stage != 3 || under.spare != -1;
        }

        expect (stageWrong == 0 && budgetWrong == 0,
                "each stage begins exactly where its own terms cost that much",
                juce::String (stageWrong) + " stages wrong, " + juce::String (budgetWrong)
                    + " with a budget that is not its cost (costs " + juce::String (costs[0])
                    + "/" + juce::String (costs[1]) + "/" + juce::String (costs[2]) + "/"
                    + juce::String (costs[3]) + " px below the margins, under a "
                    + juce::String (tightBar) + " px bar and a " + juce::String (gridFloor)
                    + " px grid)");

        expect (boundaryWrong == 0,
                "one px under a boundary the plan takes the stage below, and the last stage reports the shortfall",
                juce::String (boundaryWrong) + " of 4 boundaries disagree");

        // 2. The height each stage hands a strip has to be a height that strip reads back as
        //    the same stage, or the two rules have drifted apart: the editor would be paying
        //    for rows the strip then squeezes, or paying for a squeeze the strip never asked
        //    for. Stage 1 is the one deliberate disagreement -- it gives up the wordmark and
        //    leaves the strip at its design height -- so it is expected to read back as 0.
        auto stripOff = 0, slackOff = 0;

        for (int stage = 0; stage < 4; ++stage)
        {
            const auto handed = Layout::stripHeightForStage (stage);
            const auto read   = Layout::stripPlan (Layout::stripRoomFor (handed));

            stripOff += read.stage != (stage == 1 ? 0 : stage);
            slackOff += read.spare != (stage <= 1 ? Layout::stripRoomFor (Layout::stripDesignHeight)
                                                           - Layout::stripFullRoom
                                                                  : 0);
        }

        expect (stripOff == 0 && slackOff == 0,
                "the height each stage hands a strip is the height that strip reads as that stage",
                juce::String (stripOff) + " of 4 disagree, " + juce::String (slackOff)
                    + " with unexpected slack (the design height is room "
                    + juce::String (Layout::stripRoomFor (Layout::stripDesignHeight))
                    + " against a " + juce::String (Layout::stripFullRoom)
                    + " px budget, so stage 0 and 1 keep "
                    + juce::String (Layout::stripRoomFor (Layout::stripDesignHeight) - Layout::stripFullRoom)
                    + " px of it; 222 and 178 px are exact)");

        // 3. The floor the editor publishes is stage 3 at the worst bar, to the pixel -- and
        //    the worst bar really is the one the width floor forces.
        const auto* limits = editor->getConstrainer();
        const auto minWidth  = limits != nullptr ? limits->getMinimumWidth()  : 0;
        const auto minHeight = limits != nullptr ? limits->getMinimumHeight() : 0;

        expect (minHeight == costs[3] + 2 * Layout::editorMargin,
                "the published minimum is stage 3 with the wrapped bar, to the pixel",
                juce::String (minHeight) + " px against "
                    + juce::String (costs[3] + 2 * Layout::editorMargin) + " px wanted ("
                    + juce::String (costs[3]) + " below the margins, "
                    + juce::String (2 * Layout::editorMargin) + " px of margin)");

        const auto barWidthAtMin = minWidth - 2 * Layout::editorMargin;
        const auto barAtMinWidth = TransportBar::heightForWidth (barWidthAtMin);

        expect (limits != nullptr && TransportBar::isWrappedForWidth (barWidthAtMin)
                  && barAtMinWidth == tightBar,
                "and the width floor does force the bar to the height floor's worst case",
                "the bar at the minimum width is " + juce::String (barWidthAtMin)
                    + " px wide and " + juce::String (barAtMinWidth) + " px tall, against the "
                    + juce::String (tightBar) + " px the floor was priced at, because "
                    + juce::String (barWidthAtMin) + " is under "
                    + juce::String (TransportBar::oneRowWidth));

        // 4. Real components, at exactly the published floor. This is what turns the
        //    arithmetic into a layout claim: the row is read off the grid, the controls off
        //    each strip, the lines off the bar, the wordmark off the label that draws it.
        editor->setSize (minWidth, minHeight);

        auto walked = 0, clipped = 0, stripShort = 0, stageOffReal = 0, wordmark = 0, barOff = 0;
        auto rowBelow = 0, bankOff = 0;
        int rowAtFloor = 0, gridAtFloor = 0;
        juce::String clipDetail {};

        if (stepGrid != nullptr && transportBar != nullptr && ! walks.empty())
        {
            rowAtFloor  = stepGrid->getRowBounds (0).getHeight();
            gridAtFloor = stepGrid->getHeight();
            barOff      = transportBar->getHeight() != tightBar;
            rowBelow    = rowAtFloor < StepGridComponent::minimumRowHeight;

            for (int i = 0; i < editor->getNumChildComponents(); ++i)
            {
                if (auto* label = dynamic_cast<juce::Label*> (editor->getChildComponent (i)))
                {
                    wordmark += label->isVisible();
                    break;
                }
            }

            const auto bank = stepGrid->getBankStripBounds (0);

            bankOff += bank.getHeight() != StepGridComponent::bankStripHeight
                       || bank.getWidth() <= 0
                       || rowAtFloor < bank.getHeight() + StepGridComponent::bankNameLineHeight;

            for (auto* strip : walks)
            {
                ++walked;
                stageOffReal += strip->getLayoutStage() != 3;
                stripShort       += strip->getSpareHeight() < 0;

                auto bad = 0;

                for (int i = 0; i < strip->getNumChildComponents(); ++i)
                {
                    auto* inner = strip->getChildComponent (i);

                    if (inner == nullptr || ! inner->isVisible())
                        continue;

                    if (! strip->getLocalBounds().contains (inner->getBounds())
                        || inner->getWidth() < 1 || inner->getHeight() < 1)
                        ++bad;
                }

                if (bad != 0)
                {
                    ++clipped;

                    if (clipDetail.isEmpty())
                        clipDetail = juce::String (bad) + " of "
                                   + juce::String (strip->getNumChildComponents())
                                   + " in a " + juce::String (strip->getHeight())
                                   + " px strip, spare " + juce::String (strip->getSpareHeight());
                }
            }
        }

        expect (walked == Layout::numChannels && clipped == 0 && stripShort == 0 && stageOffReal == 0
                  && wordmark == 0 && barOff == 0 && rowBelow == 0,
                "at the floor every strip keeps its controls, the grid keeps its row and the bar keeps its lines",
                juce::String (walked) + " strips walked, " + juce::String (clipped)
                    + " with a clipped child (" + clipDetail + "), " + juce::String (stripShort)
                    + " short of what they asked for, " + juce::String (stageOffReal)
                    + " not at stage 3, " + juce::String (wordmark)
                    + " wordmark labels still visible, " + juce::String (barOff)
                    + " bars off " + juce::String (tightBar) + " px, row "
                    + juce::String (rowAtFloor) + " px against "
                    + juce::String (StepGridComponent::minimumRowHeight) + " needed");

        expect (bankOff == 0,
                "and the bank strip still stands in that row, with its name line over it",
                "row " + juce::String (rowAtFloor) + " px, grid " + juce::String (gridAtFloor)
                    + " px, strip " + (stepGrid != nullptr
                        ? asText (stepGrid->getBankStripBounds (0)) : juce::String ("no grid"))
                    + " against " + juce::String (StepGridComponent::bankStripHeight)
                    + " px over a " + juce::String (StepGridComponent::bankNameLineHeight)
                    + " px line");
        // 4b. Pixels, for the half of the claim that lives in `paint()`. The GROOVEBOX row is
        //     not only a band the layout reserves: the editor's own paint draws the divider
        //     and the one-line hint into it, and a paint that drew them whatever the stage
        //     would put a full-width line through the bar's top line at the floor -- a fact
        //     about pixels, not about bounds or child components, so nothing above can see it.
        //     Render the editor alone, not its children, and measure the longest horizontal
        //     run of the rule colour. The divider is `width - 2 * margin` px of that colour
        //     in one row, less whatever the right-aligned hint overprints it -- measured 1344 px
        //     of an unbroken 1796 px line at the default, so the claim is half the panel, which is
        //     more than any caption, button edge or text run reaches and 0 px cannot be confused with.
        const auto ruleRun = [] (const juce::Image& img)
        {
            auto best = 0;

            for (auto y = 0; y < img.getHeight(); ++y)
            {
                auto run = 0;

                for (auto x = 0; x < img.getWidth(); ++x)
                {
                    run = img.getPixelAt (x, y) == gb::Colours::rule ? run + 1 : 0;
                    best = juce::jmax (best, run);
                }
            }

            return best;
        };

        const auto floorHeightAtDefaultWidth = costs[3] + 2 * Layout::editorMargin;
        auto* shell = dynamic_cast<GrooveboxEditor*> (editor.get());
        auto drawnRule = 0, goneRule = 0, paintStage = -1;

        if (shell != nullptr)
        {
            // The Graphics object has to be destroyed before the pixels are read: an Image
            // being painted into keeps a cached context, and reading it while that context is
            // still alive answers with the untouched bitmap. `renderGrid` gets this right by
            // scoping its Graphics inside a function.
            const auto renderAt = [&] (int height)
            {
                editor->setSize (preferredWidth, height);

                juce::Image image (juce::Image::ARGB, preferredWidth, height, true);

                {
                    juce::Graphics g (image);
                    shell->paint (g);
                }

                return image;
            };

            const auto atDefault = renderAt (preferredHeight);
            const auto atFloor   = renderAt (floorHeightAtDefaultWidth);

            drawnRule = ruleRun (atDefault);
            goneRule  = ruleRun (atFloor);
            editor->setSize (preferredWidth, preferredHeight);

            paintStage = Layout::editorPlan (floorHeightAtDefaultWidth - 2 * Layout::editorMargin,
                                              TransportBar::oneRowHeight, gridFloor).stage;
        }

        expect (shell != nullptr && paintStage == 3
                  && drawnRule >= (preferredWidth - 2 * Layout::editorMargin) / 2
                  && goneRule < 40,
                "and the wordmark's divider is painted exactly when the layout paid for it",
                "stage " + juce::String (paintStage) + " at " + juce::String (preferredWidth)
                    + " x " + juce::String (floorHeightAtDefaultWidth) + " px, longest run of the "
                    "rule colour " + juce::String (drawnRule) + " px at the default against "
                    + juce::String (goneRule) + " px at that height, where an unbroken divider would be "
                    + juce::String (preferredWidth - 2 * Layout::editorMargin) + " px minus what the hint overprints");

        // 5. One px below the floor something has to give, and what gives is the grid's row:
        //    the very thing the floor exists to protect. A floor typed one px too small goes
        //    red here, and one typed too large goes red in the equality above -- together those
        //    two pin the number, which is more than "the layout still works at the minimum"
        //    would have said on its own.
        editor->setSize (minWidth, minHeight - 1);

        const auto rowUnder = stepGrid != nullptr ? stepGrid->getRowBounds (0).getHeight() : 0;

        expect (stepGrid != nullptr && rowUnder == rowAtFloor - 1
                  && rowUnder < StepGridComponent::minimumRowHeight,
                "the last px of the floor is the grid's, not a control's",
                juce::String (rowUnder) + " px row one px under the floor against "
                    + juce::String (rowAtFloor) + " px at it; the name line needs "
                    + juce::String (StepGridComponent::minimumRowHeight));

        // 6. And the window a session opens at is untouched: the wordmark still drawn, the
        //    strip at its design height, the bar on one line, the grid at the 405 px
        //    `out/layout-35-final.txt` recorded. Had any of those moved, the repair would have
        //    retuned everybody's normal layout to buy a laptop size nobody was using it for.
        editor->setSize (preferredWidth, preferredHeight);

        const auto grid0   = stepGrid != nullptr ? stepGrid->getHeight() : 0;
        const auto row0    = stepGrid != nullptr ? stepGrid->getRowBounds (0).getHeight() : 0;
        const auto strip0  = walks.empty() ? 0 : walks.front()->getHeight();
        const auto bar0    = transportBar != nullptr ? transportBar->getHeight() : 0;
        auto wordmarkSeen = 0;

        for (int k = 0; k < editor->getNumChildComponents(); ++k)
        {
            if (auto* label = dynamic_cast<juce::Label*> (editor->getChildComponent (k)))
            {
                wordmarkSeen += label->isVisible();
                break;
            }
        }

        // Read off the components rather than off the plan: the plan is the thing under
        // test here, and a claim that asks it what it decided would stay green on a rule that
        // decided a stage and then laid the window out as if it had not.
        expect (wordmarkSeen == 1 && strip0 == Layout::stripDesignHeight
                  && bar0 == TransportBar::oneRowHeight,
                "the default window still shows the wordmark, a strip at its design height and one bar line",
                juce::String (wordmarkSeen) + " wordmark labels visible, strip "
                    + juce::String (strip0) + " px against "
                    + juce::String (Layout::stripDesignHeight) + " designed, bar "
                    + juce::String (bar0) + " px against "
                    + juce::String (TransportBar::oneRowHeight) + " for one line");

        // 397 is what a 30 px row costs and 409 what a 31 px row would, asked of the grid's
        // own inverse: the 405 sits between them, so the row is 30 px and the 8 px left over
        // is the slack the division truncates away rather than a budget nobody planned for.
        expect (grid0 == 405
                  && grid0 >= StepGridComponent::heightForRowAtLeast (30)
                  && grid0 < StepGridComponent::heightForRowAtLeast (31)
                  && row0 == 30,
                "and the grid still gets the 405 px it was measured at, at 30 px rows",
                juce::String (grid0) + " px grid, row " + juce::String (row0)
                    + " px, " + juce::String (StepGridComponent::heightForRowAtLeast (30))
                    + " px being the least a 30 px row costs and "
                    + juce::String (StepGridComponent::heightForRowAtLeast (31)) + " px a 31 px one");

        // 7. Totality, at the worst bar, over every interior the editor can be handed. No
        //    stage may be priced at or below the one after it -- the order the stages give
        //    things up is the rule, and a swap would make a taller window show less -- and
        //    walking up from 1 px the stage may only ever come down.
        auto mispriced = 0, backwards = 0, previous = -1;

        for (int stage = 0; stage < 3; ++stage)
            mispriced += Layout::editorCostForStage (stage, tightBar, gridFloor)
                       <= Layout::editorCostForStage (stage + 1, tightBar, gridFloor);

        for (auto interior = 1; interior <= 1200; ++interior)
        {
            const auto p = Layout::editorPlan (interior, tightBar, gridFloor);

            // Growing the window may only ever buy a *lower* stage number and a taller
            // strip -- the two move in opposite directions, which is why "nothing went up" is
            // not the same claim as "nothing got cheaper to serve".
            if (previous >= 0 && (p.stage > previous
                || Layout::stripHeightForStage (p.stage) < Layout::stripHeightForStage (previous)))
                ++backwards;

            previous = p.stage;
        }

        expect (mispriced == 0 && backwards == 0,
                "no stage is cheaper than the one above it and a taller window never buys less",
                juce::String (mispriced) + " stages priced at or below the next, "
                    + juce::String (backwards) + " backwards steps over 1200 interiors");

        editor->setSize (preferredWidth, preferredHeight);
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

        // How many controls a strip owns, counted at the size the design asks for rather
        // than typed. The four section captions are painted, not child components, so the
        // number is the same at every stage of the height rule, and a walk that only asked
        // for `seen > 0` would be satisfied by a rule that made the mute button vanish.
        auto stripControls = 0;

        if (! strips.empty())
        {
            for (int i = 0; i < strips.front()->getNumChildComponents(); ++i)
            {
                auto* inner = strips.front()->getChildComponent (i);

                if (inner != nullptr && inner->isVisible())
                    ++stripControls;
            }

            expect (stripControls == 17, "a strip owns seventeen visible controls",
                    juce::String (stripControls) + " at "
                        + juce::String (strips.front()->getHeight()) + " px");
        }

        const auto barSlider = [transportBar] (const juce::String& name)
        {
            return findNamedSlider (*transportBar, name);
        };

        // `allowed` is whether the window may actually be this size, and it comes off the
        // editor's own constrainer rather than from a remembered 844: 5.6's height half has
        // now changed that number to 695, and an arm that is quietly outside the limits
        // should stop being excused the moment the limits reach it.
        const auto* limits = editor->getConstrainer();
        const auto floorHeight = limits != nullptr ? limits->getMinimumHeight() : preferredHeight;

        struct Size
        {
            int width, height;
            juce::String what;
            bool allowed = true;
        };

        // The bar's rule is about *bar* widths, so the arms are written in bar widths and
        // the editor's margins are measured off the running editor rather than typed in
        // a second time -- a margin written twice is a margin that gets missed.
        const auto barInset = preferredWidth - transportBar->getWidth();

        const Size sizes[] = {
            { preferredWidth,                             preferredHeight, "the default" },
            { TransportBar::oneRowWidth + barInset,       preferredHeight, "exactly what one line asks for" },
            { TransportBar::oneRowWidth - 1 + barInset,   preferredHeight, "one px narrower, so two lines" },
            { TransportBar::wrappedRowWidth + barInset,   preferredHeight, "the width floor, at the height a session opens at" },
            // Both limits met at once -- the corner the height floor is priced against, and
            // the reason it is 96 px of bar rather than the 60 the default width gets.
            { TransportBar::wrappedRowWidth + barInset,   floorHeight,     "the floor, on both axes" },
            { 1500,                                       700,             "in between" },
            { 2200,                                       1400,            "the largest the editor allows" },
            // Two arms below the floor, on purpose. The first is the height that used to
            // lose two controls per strip (236 px of strip, `out/layout-tight-probe.log`);
            // since 5.6's height half a 500 px window hands the strip 178 px instead, which
            // is the cheapest stage's exact budget with nothing clamped, so what this arm
            // regression-tests is now the stage the plan picks rather than the squeeze. The
            // second is 1832 x 399, whose 169 px strip is room 157 -- below the 166 px the
            // bare stage costs, so the ADSR row gives up 9 px and every control survives.
            { preferredWidth,                             500, "PROBE: strips at 178 px, the bare stage (below the floor)", false },
            { preferredWidth,                             399, "PROBE: strips at 169 px, inside the rule's own floor", false },
        };

        std::printf ("  the bar sits %d px inside the window on each side,"
                     " the window may not go under %d px\n", barInset / 2, floorHeight);

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

            // All twelve strips, at every size the walk reaches. The height rule in
            // `ChannelStrip::resized` is the code that decides whether the bottom of a strip
            // is on screen, and the two short arms below are in this list precisely because
            // they reach the part of it that the reachable sizes never touch: they are
            // smaller than the window can be, and the claim is that the rule is total anyway.
            auto clipped = 0, outOfRule = 0, shortWhereAllowed = 0, wrongCount = 0;
            const auto armStripHeight = strips.empty() ? 0 : strips.front()->getHeight();

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

                wrongCount += seen != stripControls || bad != 0;
                clipped += bad;

                // The strip ran the shared rule, not a private copy of it. This is the
                // editor-and-the-bar claim, which on its own proves only that two callers
                // agree — the numbers that make it worth anything are gated in the section
                // above, and what this one catches is `resized()` going back to its own
                // arithmetic while the test kept checking the plan.
                const auto plan = Layout::stripPlan (Layout::stripRoomFor (strip->getHeight()));

                outOfRule += strip->getSpareHeight() != plan.spare
                          || strip->getLayoutStage() != plan.stage;

                // Only a size the window can actually take owes anyone a usable control.
                if (size.allowed && plan.spare < 0)
                    ++shortWhereAllowed;

                expectQuiet (bad == 0 && seen == stripControls,
                             "strip " + juce::String (lane) + " is whole",
                             juce::String (bad) + " clipped of " + juce::String (seen)
                                 + ", " + juce::String (strip->getHeight()) + " px tall, spare "
                                 + juce::String (strip->getSpareHeight()) + " at stage "
                                 + juce::String (strip->getLayoutStage()));
            }

            expect (wrongCount == 0,
                    "every strip keeps all of its controls inside, at every arm",
                    juce::String (clipped) + " clipped in total, " + juce::String (wrongCount)
                        + " of " + juce::String (static_cast<int> (strips.size()))
                        + " strips wrong at " + juce::String (armStripHeight) + " px");

            expect (outOfRule == 0,
                    "and each one laid out by the shared rule rather than a copy of it",
                    juce::String (outOfRule) + " strips disagree with Layout::stripPlan");

            // What this arm used to assert was that nothing reaches the compact branch,
            // because the compact branch was broken: a probe at 1832 x 500 put 2 of every
            // strip's 17 controls outside it, at 236 px of strip with `getSpareHeight () == 0`
            // (`out/layout-tight-probe.log`), and the arm was then deleted so the suite could
            // go green. That claim is worth nothing now that the rule is a budget — and it was
            // worth nothing before, since it was satisfied by the floor being too high rather
            // than by the layout being right. The claim here is the one that survives 5.6's
            // height half: at any size the window is allowed to be, no strip is given less
            // than the sizes it asked for. The floor comes off the editor's own constrainer,
            // so lowering it pulls these arms in rather than letting this line stay green.
            expect (shortWhereAllowed == 0,
                    "no strip the editor can show is short of what its plan asked for",
                    juce::String (shortWhereAllowed) + " of "
                        + juce::String (static_cast<int> (strips.size()))
                        + " under, at " + juce::String (armStripHeight) + " px of strip, "
                        + (size.allowed ? juce::String ("inside the ")
                                        : juce::String ("below the "))
                        + juce::String (floorHeight) + " px floor"
                        + (size.allowed ? juce::String() : " on purpose"));

            // Which side starves when the window is smaller than the floor.
            //
            // `Layout::gridGuardHeight` is the promise the resize path makes to the grid,
            // and an arm that halves it is invisible to every check above: the strip is
            // still whole, its controls are still inside it, and the plan still agrees with
            // itself. So the promise is gated at the component it is made to -- the grid --
            // and stated with the one term the resize path spends before handing the band
            // over, the gap above it. The two sub-floor arms answer in different orders and
            // the message says which: a 500 px window leaves the strip its full 178 and
            // shortens the grid to 202, a 399 px one cannot take any more from the grid, so
            // the strip gives up 9 px and lands on 169.
            const auto gridBand = stepGrid != nullptr ? stepGrid->getHeight() : 0;

            expect (gridBand >= Layout::gridGuardHeight - Layout::editorGap,
                    "the grid never falls under the guard the resize path promises it",
                    juce::String (gridBand) + " px of grid against a "
                        + juce::String (Layout::gridGuardHeight) + " px guard less a "
                        + juce::String (Layout::editorGap) + " px gap, with "
                        + juce::String (armStripHeight) + " px of strip against "
                        + juce::String (Layout::stripHeightForStage (3))
                        + " for stage 3 ("
                        + (size.allowed ? juce::String ("inside the floor")
                                        : juce::String ("below the floor")) + ")");

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
            return renderGrid (*stepGrid);
        };

        const auto cell = stepGrid->getCellBounds (probeChannel, probeStep);

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
            const auto d = compareImages (certain, mutableMarked, window);

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

            if (mustMatch (shutCertain, shutMutable))
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

            if (mustMatch (offCertain, offMutable))
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

            if (mustMatch (overlayCertain, overlayMutable))
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

    //======================================================================
    // The bank strip is the only place the pattern bank can be seen or reached: there is
    // no file browser, no menu and no parameter for a slot. So five claims have to hold,
    // and each needs its own kind of evidence:
    //
    //   the strip exists at every size the window can take, and nowhere it cannot;
    //   its three controls have sizes that do not overlap, so they can be told apart;
    //   a point on one of them is answered as that one, and a point on nothing is
    //   answered as nothing;
    //   a click moves the number the engine reads, and only for that lane;
    //   and the marks report the numbers rather than the last one the user touched.
    //
    // The geometry names come out of the class's own table, restated nowhere in this
    // file: a threshold typed here is a threshold that gets missed when the strip grows.
    std::printf ("\n-- the bank strip in the label column --\n");
    {
        using Control = StepGridComponent::BankControl;

        constexpr auto stripH   = StepGridComponent::bankStripHeight;
        constexpr auto nameLine = StepGridComponent::bankNameLineHeight;
        constexpr auto fitRow   = stripH + nameLine;
        constexpr auto fitCol   = StepGridComponent::bankMinimumLabelWidth;
        constexpr auto dotSize  = StepGridComponent::bankSlotDotSize;
        constexpr auto dotGap   = StepGridComponent::bankSlotDotGap;
        constexpr auto edgePad  = StepGridComponent::bankEdgePad;
        constexpr auto groupGap = StepGridComponent::bankGroupGap;
        constexpr auto copyW    = StepGridComponent::bankCopyWidth;
        constexpr auto chipMinW = StepGridComponent::bankChipMinWidth;
        constexpr auto dotsW    = StepGridComponent::bankDotsWidth;

        const int numSlots = Layout::numPatternSlots;
        const int gridW = stepGrid->getWidth();
        const int gridH = stepGrid->getHeight();

        auto stripAt = [stepGrid] (int channel)
        {
            return stepGrid->getBankStripBounds (channel);
        };

        // The two numbers the fit rule is about, read out of the geometry the painter
        // uses: a row's height is any cell in it, and the label column's width is the
        // name area's own width. Redoing `resized()`'s integer division here would be
        // writing that maths twice, and the boundary this arm hunts is made of it.
        auto rowHeight = [stepGrid] { return stepGrid->getCellBounds (0, 0).getHeight(); };
        auto colWidth  = [stepGrid] { return stepGrid->getLabelBounds (0).getWidth(); };

        auto fitNumbers = [&]
        {
            return "row " + juce::String (rowHeight()) + " px (needs " + juce::String (fitRow)
                 + "), column " + juce::String (colWidth()) + " px (needs "
                 + juce::String (fitCol) + ")";
        };

        //------------------------------------------------------------------
        // 1. Where the strip is allowed to exist, found by scanning rather than by
        // asking the predicate at one size.
        struct Scan
        {
            int sizes = 0;
            int withStrip = 0;
            int disagreements = 0;
            int reversals = 0;
            int firstPresent = -1;
            int lastMeasured = -1;
        };

        auto scan = [&] (bool varyWidth, int from, int to)
        {
            Scan s;

            for (int v = from; v <= to; ++v)
            {
                stepGrid->setSize (varyWidth ? v : gridW,
                                   varyWidth ? gridH : v);

                ++s.sizes;

                const auto row = rowHeight();
                const auto col = colWidth();
                const auto measured = varyWidth ? col : row;

                if (measured < s.lastMeasured)
                    ++s.reversals;

                s.lastMeasured = measured;

                const auto present = ! stripAt (0).isEmpty();

                if (present != (row >= fitRow && col >= fitCol))
                    ++s.disagreements;

                if (present)
                {
                    ++s.withStrip;

                    if (s.firstPresent < 0)
                        s.firstPresent = v;
                }
            }

            return s;
        };

        auto reportScan = [&] (const Scan& s, const juce::String& what,
                               const juce::String& grows)
        {
            // The precondition goes first, because a scan in which the strip never
            // appeared — or never failed to appear — satisfies "no disagreements" by
            // testing one arm, exactly the way a matrix walk that runs zero times does.
            expect (s.withStrip > 0 && s.withStrip < s.sizes,
                    "both arms of the fit rule occur across the " + what + " scan",
                    juce::String (s.withStrip) + " of " + juce::String (s.sizes)
                        + " sizes have a strip, first at " + juce::String (s.firstPresent));

            // Monotonic, so that checking the smallest permitted size is the same as
            // checking all of them: if the row ever went back down as the window grew,
            // the floor would stop carrying the claim.
            expect (s.reversals == 0,
                    "the " + grows + " never shrinks as the " + what + " grows",
                    juce::String (s.sizes) + " sizes walked, " + juce::String (s.reversals)
                        + " reversals");

            expect (s.disagreements == 0,
                    "the strip exists exactly where a name line and three controls fit",
                    juce::String (s.disagreements) + " of " + juce::String (s.sizes)
                        + " sizes disagree with " + juce::String (fitRow) + " px row and "
                        + juce::String (fitCol) + " px column");
        };

        const auto heights = scan (false, 200, 500);
        const auto widths  = scan (true,  200, 500);

        reportScan (heights, "height", "row");
        reportScan (widths, "width", "label column");

        // The boundary itself, taken from each scan rather than typed, and gated on both
        // sides of it. The height arm also asks the question the strip was given up for:
        // whether the name still has the line the 14 px font needs.
        stepGrid->setSize (gridW, heights.firstPresent);
        {
            const auto strip = stripAt (0);
            const auto line = strip.getY() - stepGrid->getRowBounds (0).getY();

            expect (rowHeight() >= fitRow && ! strip.isEmpty() && line >= nameLine,
                    "at the first height that takes the strip the name still has its line",
                    "grid " + juce::String (heights.firstPresent) + " px: " + fitNumbers()
                        + ", name line " + juce::String (line) + ", strip " + asText (strip));

            stepGrid->setSize (gridW, heights.firstPresent - 1);

            expect (stripAt (0).isEmpty(),
                    "one px of row shorter and there is no strip to click at all",
                    "grid " + juce::String (heights.firstPresent - 1) + " px: " + fitNumbers());
        }

        stepGrid->setSize (widths.firstPresent, gridH);
        {
            const auto chip = stepGrid->getEvolveChipBounds (0);

            expect (colWidth() >= fitCol && ! stripAt (0).isEmpty()
                      && chip.getWidth() >= chipMinW,
                    "at the first width that takes the strip every control fits",
                    "grid " + juce::String (widths.firstPresent) + " px: " + fitNumbers()
                        + ", chip " + asText (chip));

            stepGrid->setSize (widths.firstPresent - 1, gridH);

            expect (stripAt (0).isEmpty(),
                    "one px of column narrower and the three controls are given up",
                    "grid " + juce::String (widths.firstPresent - 1) + " px: " + fitNumbers());
        }

        //------------------------------------------------------------------
        // 2. Every size the window is allowed to take. The extremes come out of the
        // editor's own constrainer — the object `setResizeLimits` filled — because
        // typing 1218 x 695 here would be a second copy of the floor, and the floor is
        // what this arm is about: it is the one size at which the row is exactly
        // strip + name line, with nothing to spare.
        const auto* limits = editor->getConstrainer();

        expect (limits != nullptr && limits->getMinimumWidth() > 0
                  && limits->getMinimumHeight() > 0,
                "the editor says its own limits, so the floor is not typed here",
                limits == nullptr
                    ? "no constrainer"
                    : (juce::String (limits->getMinimumWidth()) + " x "
                       + juce::String (limits->getMinimumHeight()) + " to "
                       + juce::String (limits->getMaximumWidth()) + " x "
                       + juce::String (limits->getMaximumHeight())));

        struct Size { int width, height; juce::String what; };

        const Size sizes[] = {
            { limits != nullptr ? limits->getMinimumWidth() : preferredWidth,
              limits != nullptr ? limits->getMinimumHeight() : preferredHeight,
              "the smallest the window can go" },
            { limits != nullptr ? limits->getMaximumWidth() : preferredWidth,
              limits != nullptr ? limits->getMaximumHeight() : preferredHeight,
              "the largest" },
            { preferredWidth, juce::jmax (preferredHeight, 790), "the default" },
        };

        for (const auto& size : sizes)
        {
            editor->setSize (size.width, size.height);

            int missing = 0, misshapen = 0, noRoom = 0, intoCells = 0;

            for (int channel = 0; channel < Layout::numChannels; ++channel)
            {
                const auto strip = stripAt (channel);

                if (strip.isEmpty())
                {
                    ++missing;
                    continue;
                }

                const auto label = stepGrid->getLabelBounds (channel);
                const auto row   = stepGrid->getRowBounds (channel);

                noRoom    += strip.getY() - row.getY() < nameLine;
                intoCells += strip.getRight() > row.getX();
                misshapen += strip.getHeight() != stripH
                          || strip.getX() != label.getX()
                          || strip.getWidth() != label.getWidth()
                          || strip.getBottom() != label.getBottom();
            }

            std::printf ("\n  %d x %d -- %s: %s\n", size.width, size.height,
                         size.what.toRawUTF8(), fitNumbers().toRawUTF8());

            expect (missing == 0, "twelve strips, one per lane",
                    juce::String (missing) + " of " + juce::String (Layout::numChannels)
                        + " lanes have none");

            expect (misshapen == 0, "each is the bottom of its label column at the strip height",
                    juce::String (misshapen) + " lanes disagree with "
                        + juce::String (stripH) + " px, e.g. "
                        + asText (stripAt (0)));

            expect (noRoom == 0, "and none of them eats the name line it was cut from",
                    juce::String (noRoom) + " lanes leave the name under "
                        + juce::String (nameLine) + " px");

            expect (intoCells == 0, "and none of them reaches into the cells",
                    juce::String (intoCells) + " lanes overlap their own row's cells, strip right "
                        + juce::String (stripAt (0).getRight()) + " vs cell left "
                        + juce::String (stepGrid->getCellBounds (0, 0).getX()));
        }

        // Back to the size the rest of the file works at, and let the editor give the
        // grid its own bounds again rather than restoring them by hand.
        editor->setSize (preferredWidth, juce::jmax (preferredHeight, 790));
        stepGrid->setSize (gridW, gridH);

        //------------------------------------------------------------------
        // 3. The shape of the three controls, in the table's own relations: the pad, the
        // gaps between marks, the two group gaps, the centring, and the widths. Stated as
        // relations rather than as one typed formula per control, so a change to the order
        // of the row is caught as a broken relation and not as twelve wrong literals.
        int wrongSize = 0, wrongPad = 0, wrongDotGap = 0, offCentre = 0, wrongGroup = 0;
        int wrongCopy = 0, wrongChip = 0, overlapping = 0, leaked = 0;

        for (int channel = 0; channel < Layout::numChannels; ++channel)
        {
            const auto strip = stripAt (channel);
            const auto label = stepGrid->getLabelBounds (channel);

            juce::Rectangle<int> dots [Layout::numPatternSlots];

            for (int slot = 0; slot < numSlots; ++slot)
                dots[slot] = stepGrid->getSlotDotBounds (channel, slot);

            const auto copy = stepGrid->getSlotCopyBounds (channel);
            const auto chip = stepGrid->getEvolveChipBounds (channel);

            for (int slot = 0; slot < numSlots; ++slot)
            {
                wrongSize  += dots[slot].getWidth() != dotSize
                           || dots[slot].getHeight() != dotSize;
                offCentre  += dots[slot].getY() - strip.getY()
                           != strip.getBottom() - dots[slot].getBottom();
                leaked     += ! strip.contains (dots[slot]) || ! label.contains (dots[slot]);
            }

            wrongPad    += dots[0].getX() != strip.getX() + edgePad;

            for (int slot = 1; slot < numSlots; ++slot)
                wrongDotGap += dots[slot].getX() - dots[slot - 1].getRight() != dotGap;

            wrongGroup += dots[numSlots - 1].getRight() - dots[0].getX() != dotsW
                       || copy.getX() != dots[numSlots - 1].getRight() + groupGap
                       || chip.getX() != copy.getRight() + groupGap;

            wrongCopy  += copy.getWidth() != copyW || copy.getHeight() != stripH
                       || copy.getY() != strip.getY();

            wrongChip  += chip.getWidth() < chipMinW
                       || chip.getRight() != strip.getRight() - edgePad
                       || chip.getHeight() != stripH - 2;

            leaked += ! strip.contains (copy) || ! strip.contains (chip)
                   || ! label.contains (copy) || ! label.contains (chip)
                   || ! stepGrid->getLocalBounds().contains (strip);

            for (int a = 0; a < numSlots; ++a)
            {
                for (int b = a + 1; b < numSlots; ++b)
                    overlapping += dots[a].intersects (dots[b]);

                overlapping += dots[a].intersects (copy) || dots[a].intersects (chip);
            }

            overlapping += copy.intersects (chip);
        }

        expect (wrongSize == 0, "the four marks are all the same size the table gives them",
                juce::String (wrongSize) + " marks are not " + juce::String (dotSize)
                    + " px square");

        expect (wrongPad == 0, "the group starts at the strip's own padding",
                juce::String (wrongPad) + " rows start anywhere else");

        expect (wrongDotGap == 0, "the marks are one gap apart, so each has its own target",
                juce::String (wrongDotGap) + " gaps are not " + juce::String (dotGap) + " px");

        expect (offCentre == 0, "and the marks sit centred in the strip, not on its floor",
                juce::String (offCentre) + " marks are off-centre");

        expect (wrongGroup == 0, "dots, copy and chip are the group widths the table says",
                juce::String (wrongGroup) + " rows disagree with " + juce::String (dotsW)
                    + " px of marks and " + juce::String (groupGap) + " px between groups");

        expect (wrongCopy == 0, "the copy mark keeps its width and the strip's height",
                juce::String (wrongCopy) + " rows wrong, e.g. "
                    + asText (stepGrid->getSlotCopyBounds (0)));

        expect (wrongChip == 0, "the chip takes the rest of the column and nothing more",
                juce::String (wrongChip) + " rows wrong, e.g. "
                    + asText (stepGrid->getEvolveChipBounds (0)));

        expect (overlapping == 0, "no two controls of a lane overlap, so each is clickable alone",
                juce::String (overlapping) + " pairs share pixels");

        expect (leaked == 0, "and every control is inside its strip, its column and the panel",
                juce::String (leaked) + " controls reach outside");

        //------------------------------------------------------------------
        // 4. The hit test: the centre of each control's rectangle is answered as that
        // control, and the dead pixels the geometry leaves between them are answered as
        // nothing. Not circular: the rectangles above were gated against the table, so
        // this arm only decides which control owns the pixels it is given.
        auto controlAt = [stepGrid] (juce::Point<int> p)
        {
            return stepGrid->bankTargetAt (p);
        };

        int wrongDotHit = 0, wrongCopyHit = 0, wrongChipHit = 0, wrongChannelHit = 0;
        int deadPoints = 0, deadAnswered = 0;

        for (int channel = 0; channel < Layout::numChannels; ++channel)
        {
            for (int slot = 0; slot < numSlots; ++slot)
            {
                const auto hit = controlAt (stepGrid->getSlotDotBounds (channel, slot).getCentre());

                wrongDotHit += hit.control != Control::slot || hit.index != slot;
                wrongChannelHit += hit.channel != channel;
            }

            // The `index` field is a readout for the tooltip rather than the click's
            // argument, and this sweep runs before any lane has been pointed anywhere:
            // every lane opens on slot 0 with a period of off, so that is what the two
            // readouts have to say. Compared against the getter the hit test itself calls
            // it would be a gate that asks the predicate what it expects.
            const auto copyHit = controlAt (stepGrid->getSlotCopyBounds (channel).getCentre());

            wrongCopyHit += copyHit.control != Control::copy || copyHit.index != 0;

            const auto chipHit = controlAt (stepGrid->getEvolveChipBounds (channel).getCentre());

            wrongChipHit += chipHit.control != Control::evolve || chipHit.index != 0;

            wrongChannelHit += copyHit.channel != channel || chipHit.channel != channel;

            // The dead pixels this row leaves: the pad before the marks, the gap inside
            // the group, the gap between the groups, the far pad, the name line above,
            // the gap between two rows, and the first cell of the row.
            const auto strip = stripAt (channel);
            const auto row   = stepGrid->getRowBounds (channel);
            const auto dot0  = stepGrid->getSlotDotBounds (channel, 0);
            const auto dot1  = stepGrid->getSlotDotBounds (channel, 1);
            const auto copy  = stepGrid->getSlotCopyBounds (channel);
            const auto last  = stepGrid->getSlotDotBounds (channel, numSlots - 1);

            const juce::Point<int> dead[] = {
                { strip.getX(), strip.getCentre().getY() },
                { dot0.getRight() + dotGap / 2, dot0.getCentre().getY() },
                { last.getRight() + groupGap / 2, last.getCentre().getY() },
                { copy.getRight() + groupGap / 2, copy.getCentre().getY() },
                { strip.getRight() - edgePad / 2, strip.getCentre().getY() },
                { strip.getCentre().getX(), row.getY() + 1 },
                { strip.getCentre().getX(), row.getBottom() + 1 },
                { stepGrid->getCellBounds (channel, 0).getCentre() },
            };

            for (const auto& p : dead)
            {
                ++deadPoints;

                if (controlAt (p).control != Control::none)
                    ++deadAnswered;
            }
        }

        expect (wrongDotHit == 0, "a mark's centre is answered as that mark's slot",
                juce::String (wrongDotHit) + " of "
                    + juce::String (Layout::numChannels * numSlots) + " wrong");

        expect (wrongCopyHit == 0, "the copy centre is the copy, and reports the slot it would copy",
                juce::String (wrongCopyHit) + " wrong");

        expect (wrongChipHit == 0, "the chip centre is the rotate, and reports the period it would cycle",
                juce::String (wrongChipHit) + " wrong");

        expect (wrongChannelHit == 0, "and every answer names the lane the point is in",
                juce::String (wrongChannelHit) + " wrong channels");

        expect (deadAnswered == 0,
                "the padding and gaps between the controls answer nothing",
                juce::String (deadAnswered) + " of " + juce::String (deadPoints)
                    + " dead points hit a control");

        //------------------------------------------------------------------
        // 5. A click moves the number the engine reads. This is the arm that decides
        // whether the feature works at all: the geometry above could be painted on a
        // control that does nothing.
        const int probe = 5;
        const int other = 6;

        auto clearSlot = [&] (int channel, int slot)
        {
            processor.setChannelSlot (channel, slot);

            for (int s = 0; s < Layout::numSteps; ++s)
            {
                processor.setStep (channel, s, false);
                processor.setStepVelocity (channel, s, 1.0f);
                processor.setStepChance (channel, s, 1.0f);
            }
        };

        auto emptyBank = [&] (int channel)
        {
            for (int slot = 0; slot < numSlots; ++slot)
                clearSlot (channel, slot);

            processor.setChannelSlot (channel, 0);
            processor.setChannelEvolveBars (channel, 0);
        };

        emptyBank (probe);
        emptyBank (other);
        processor.getUndoManager().clearUndoHistory();

        auto clickAt = [stepGrid] (juce::Point<int> p)
        {
            const auto e = makeEvent (*stepGrid, p.toFloat());
            stepGrid->mouseDown (e);
            stepGrid->mouseUp (e);
        };

        /**
            The two numbers the bank controls move, and what the lane holds.

            A click that is supposed to belong to *nobody* has to be shown not to have
            gone to somebody, and "the selection did not move" is not that proof: the
            rotate chip and the dead pixels share a row, and a click handed to the chip
            leaves the selection exactly where it was. So the arms below that expect
            nothing to happen ask for both numbers, and for the copy's own signature —
            which slots the lane holds — because a stray copy moves the pointer and the
            mask together.
        */
        auto pointerState = [&] (int channel)
        {
            return juce::String (processor.getChannelSlot (channel)) + "/"
                 + juce::String (processor.getChannelEvolveBars (channel));
        };

        auto filledMask = [&] (int channel)
        {
            juce::String mask;

            for (int slot = 0; slot < numSlots; ++slot)
                mask += processor.isChannelSlotFilled (channel, slot) ? "x" : "-";

            return mask;
        };

        /**
            Everything one click could have changed in one lane, as one string.

            The dead-pixel arms write different things -- a stray pointer, a filled slot,
            a counter that cycled, a step that toggled -- and a check that watches only the
            one its own comment names is satisfied by an arm that wrote something else. This
            is the whole lane, so any write at all is a difference.
        */
        auto bankSignature = [&] (int channel)
        {
            juce::String bits;

            for (int s = 0; s < Layout::numSteps; ++s)
                bits += processor.isStepOn (channel, s) ? "1" : "0";

            return pointerState (channel) + " " + filledMask (channel) + " " + bits;
        };

        // In an order that is not the slots' order, so a handler that ignored the index
        // it was given, or always used the next one, cannot pass by luck.
        const int clicks[] = { 3, 1, 2, 0 };

        for (const auto slot : clicks)
        {
            clickAt (stepGrid->getSlotDotBounds (probe, slot).getCentre());

            expect (processor.getChannelSlot (probe) == slot,
                    "clicking mark " + juce::String (slot) + " points the lane at slot "
                        + juce::String (slot),
                    "selection is " + juce::String (processor.getChannelSlot (probe))
                        + ", lane " + juce::String (other) + " still "
                        + juce::String (processor.getChannelSlot (other)));
        }

        // The cells follow the selection: pointing the lane at a slot is only worth
        // clicking if what it shows is that slot's grid.
        processor.setChannelSlot (probe, 0);
        processor.setStep (probe, 4, true);
        processor.setChannelSlot (probe, 1);
        processor.setStep (probe, 9, true);

        clickAt (stepGrid->getSlotDotBounds (probe, 0).getCentre());

        expect (processor.isStepOn (probe, 4) && ! processor.isStepOn (probe, 9),
                "and the grid that comes up is the one that slot holds",
                "step 4 " + juce::String ((int) processor.isStepOn (probe, 4))
                    + ", step 9 " + juce::String ((int) processor.isStepOn (probe, 9)));

        clickAt (stepGrid->getSlotDotBounds (probe, 1).getCentre());

        expect (! processor.isStepOn (probe, 4) && processor.isStepOn (probe, 9),
                "and pointing it back shows the other pattern instead",
                "step 4 " + juce::String ((int) processor.isStepOn (probe, 4))
                    + ", step 9 " + juce::String ((int) processor.isStepOn (probe, 9)));

        // The bank answers with an overlay open: choosing which pattern to edit is not an
        // edit of the cell under the cursor, so the velocity bars are no reason to ignore
        // the click — and the label column is where a cell drag already gives up.
        stepGrid->setVelocityMode (true);
        clickAt (stepGrid->getSlotDotBounds (probe, 2).getCentre());
        stepGrid->setVelocityMode (false);

        expect (processor.getChannelSlot (probe) == 2,
                "a mark still answers with an overlay open",
                "selection " + juce::String (processor.getChannelSlot (probe)));

        // A click in a cell is a cell edit and nothing else: the step under the cursor has
        // to move, and the bank's two numbers must not. The second half is the one that
        // matters, and it needs both numbers: a chip handed the whole row band and the
        // cells with it answers with the rotate, which moves the counter and leaves the
        // selection exactly where it was. That is arm v of out/mutate-bank6.log, and a
        // check that watched only the selection stayed green through it.
        const auto beforeCell = pointerState (probe);
        const auto stepWasOn = processor.isStepOn (probe, 0);

        clickAt (stepGrid->getCellBounds (probe, 0).getCentre());

        expect (processor.isStepOn (probe, 0) != stepWasOn && pointerState (probe) == beforeCell,
                "and a click in the cells edits the cell, not the bank",
                "step 0 was " + juce::String ((int) stepWasOn) + ", now "
                    + juce::String ((int) processor.isStepOn (probe, 0)) + "; bank "
                    + beforeCell + " then " + pointerState (probe));

        // The row gap is not a row: a click there belongs to neither lane, so nothing in
        // that lane changes. A stray copy would move the pointer and fill a slot, a stray
        // rotate would move the counter and a stray cell edit would toggle a step, so the
        // check asks for the lane's whole signature rather than any one of the three -- all
        // of which the old selection-only assertion could miss.
        const auto beforeGap = pointerState (probe);
        const auto maskBeforeGap = filledMask (probe);

        // A gap point is refused in three places, in series: channelAtY says the bottom
        // sliver of a row span is not a row at all, the strip guard in bankTargetAt says the
        // point is not in the strip, and each control's own rectangle says it is not in the
        // control. Rounds four to eight each took one or two of the three out and came back
        // green (arm o, arm s, arm s2, arm s3, arm z1), which is what says a refusal
        // defended by another refusal is not a control. Taking the click at four x
        // positions -- over a mark, over the copy, over the chip and over the first cell --
        // and asking the whole signature is what gave each of the three an arm that reaches
        // it: the cell shot is answered by channelAtY's sliver rule on its own (arm x), the
        // chip shot needs that rule and the strip guard both gone (arm z), and the mark and
        // copy shots say that no arm can satisfy this check by answering for a control it
        // was not pointed at.
        const int gapY = stepGrid->getRowBounds (probe).getBottom() + 1;

        const int gapX[] = { stepGrid->getSlotDotBounds (probe, 0).getCentre().getX(),
                             stepGrid->getSlotCopyBounds (probe).getCentre().getX(),
                             stepGrid->getEvolveChipBounds (probe).getCentre().getX(),
                             stepGrid->getCellBounds (probe, 0).getCentre().getX() };

        int gapMoved = 0, gapFirst = -1;
        juce::String gapWas, gapNow;

        for (int shot = 0; shot < 4; ++shot)
        {
            const auto stateWas = bankSignature (probe);

            clickAt (juce::Point<int> { gapX[shot], gapY });

            if (bankSignature (probe) != stateWas)
            {
                ++gapMoved;

                if (gapFirst < 0)
                {
                    gapFirst = shot;
                    gapWas = stateWas;
                    gapNow = bankSignature (probe);
                }
            }
        }

        expect (gapMoved == 0, "a click in the gap between two rows belongs to neither lane",
                juce::String (gapMoved) + " of 4 gap clicks wrote something"
                    + (gapFirst < 0 ? juce::String()
                                    : ", first over x " + juce::String (gapX[gapFirst])
                                          + ": " + gapWas + " became " + gapNow)
                    + ", bank " + beforeGap + " and " + maskBeforeGap + ", then "
                    + pointerState (probe) + " and " + filledMask (probe));

        // Duplicate: the copy goes to the next empty slot and the lane moves onto it.
        processor.setChannelSlot (probe, 3);
        emptyBank (probe);
        processor.setChannelSlot (probe, 0);
        processor.setStep (probe, 3, true);
        processor.setStepVelocity (probe, 3, 0.5f);

        clickAt (stepGrid->getSlotCopyBounds (probe).getCentre());

        expect (processor.getChannelSlot (probe) == 1,
                "the copy mark duplicates into the next empty slot and lands there",
                "selection " + juce::String (processor.getChannelSlot (probe))
                    + ", slot 1 filled " + juce::String ((int) processor.isChannelSlotFilled (probe, 1)));

        expect (processor.isStepOn (probe, 3) && processor.getStepVelocity (probe, 3) == 0.5f,
                "with the level the original row carried, not the default",
                "step 3 on " + juce::String ((int) processor.isStepOn (probe, 3))
                    + ", velocity " + juce::String (processor.getStepVelocity (probe, 3), 3));

        // One undo step for what is two writes. Left ungrouped, undo takes the copy back
        // and leaves the lane pointing at a slot the user never selected.
        const auto undone = processor.getUndoManager().undo();
        const auto afterUndo = processor.getChannelSlot (probe);

        expect (undone && afterUndo == 0 && ! processor.isChannelSlotFilled (probe, 1),
                "and one undo takes the copy and the selection together",
                "undo " + juce::String ((int) undone) + ", selection "
                    + juce::String (afterUndo) + ", slot 1 filled "
                    + juce::String ((int) processor.isChannelSlotFilled (probe, 1)));

        processor.getUndoManager().clearUndoHistory();

        // Nowhere to go: a mark that duplicates over a pattern would destroy one.
        for (int slot = 0; slot < numSlots; ++slot)
        {
            processor.setChannelSlot (probe, slot);
            processor.setStep (probe, 6, true);
        }

        processor.setChannelSlot (probe, 2);
        const auto fullSelection = processor.getChannelSlot (probe);

        clickAt (stepGrid->getSlotCopyBounds (probe).getCentre());

        expect (processor.getChannelSlot (probe) == fullSelection,
                "with all four slots full the copy mark finds nowhere to go and changes nothing",
                "selection " + juce::String (processor.getChannelSlot (probe))
                    + ", filled " + juce::String ((int) processor.isChannelSlotFilled (probe, 0))
                    + juce::String ((int) processor.isChannelSlotFilled (probe, 1))
                    + juce::String ((int) processor.isChannelSlotFilled (probe, 2))
                    + juce::String ((int) processor.isChannelSlotFilled (probe, 3)));

        // The rotate chip walks its ladder and wraps.
        emptyBank (probe);

        const int ladder[] = { 1, 2, 4, 8, 0 };

        for (const auto want : ladder)
        {
            clickAt (stepGrid->getEvolveChipBounds (probe).getCentre());

            expect (processor.getChannelEvolveBars (probe) == want,
                    "the chip cycles the period to " + juce::String (want),
                    "period " + juce::String (processor.getChannelEvolveBars (probe))
                        + ", lane " + juce::String (other) + " still "
                        + juce::String (processor.getChannelEvolveBars (other)));
        }

        // A period that is not on this build's ladder — one a newer session brought in —
        // must still answer, and must land above where it started rather than on itself.
        processor.setChannelEvolveBars (probe, 3);
        clickAt (stepGrid->getEvolveChipBounds (probe).getCentre());

        expect (processor.getChannelEvolveBars (probe) == 4,
                "an off-ladder period walks to the next rung rather than staying put",
                "3 bars became " + juce::String (processor.getChannelEvolveBars (probe)));

        expect (processor.getChannelEvolveBars (other) == 0,
                "and a lane's period is its own",
                "lane " + juce::String (other) + " is "
                    + juce::String (processor.getChannelEvolveBars (other)));

        //------------------------------------------------------------------
        // 6. Pixels: the marks have to report the numbers, not the last thing clicked.
        //
        // Two windows per pair, because pointing a lane at a slot legitimately moves two
        // things — the ring in the strip, and the pattern the cells show. "Only the strip
        // moved" is not the claim: it is false, and the first version of this arm came
        // back with 6100 of 6196 pixels outside the strip on a build that was correct.
        // What is true, and what is gated, is that something moved *inside the strip* —
        // which no amount of cell repainting can substitute for — and that nothing moved
        // outside this lane's own row, which is what a painter that rings all twelve rows
        // or repaints the whole grid would fail. Both halves are needed: the first stops
        // the locality claim passing vacuously, the second stops a repaint-everything.
        auto rowBand = [stepGrid] (int channel)
        {
            const auto row = stepGrid->getRowBounds (channel);
            const auto lastCell = stepGrid->getCellBounds (channel, Layout::numSteps - 1);
            const auto strip = stepGrid->getBankStripBounds (channel);

            return juce::Rectangle<int> (strip.getX(), row.getY(),
                                         lastCell.getRight() - strip.getX(),
                                         row.getHeight()).expanded (1);
        };

        struct Moved
        {
            bool ok = false;
            juce::String detail;
        };

        auto movedBetween = [&stripAt, &rowBand] (const juce::Image& a, const juce::Image& b,
                                                  int channel)
        {
            const auto inStrip = compareImages (a, b, stripAt (channel).expanded (1));
            const auto inRow   = compareImages (a, b, rowBand (channel));

            Moved m;

            m.ok = inStrip.pixels - inStrip.outside > 0 && inRow.outside == 0;

            m.detail = juce::String (inStrip.pixels - inStrip.outside)
                     + " px of the strip moved, " + juce::String (inRow.pixels)
                     + " px of the row, " + juce::String (inRow.outside) + " outside it";

            return m;
        };

        emptyBank (probe);
        emptyBank (other);

        processor.setChannelSlot (probe, 0);
        processor.setStep (probe, 6, true);
        processor.setChannelSlot (probe, 1);
        processor.setStep (probe, 7, true);
        processor.setChannelSlot (probe, 0);

        const auto selected0 = renderGrid (*stepGrid);
        const auto selected0Again = renderGrid (*stepGrid);

        processor.setChannelSlot (probe, 1);
        const auto selected1 = renderGrid (*stepGrid);
        processor.setChannelSlot (probe, 0);

        {
            const auto m = movedBetween (selected0, selected1, probe);

            expect (m.ok, "pointing the lane at another slot is drawn in its strip",
                    m.detail + ", selection now "
                        + juce::String (processor.getChannelSlot (probe)));
        }

        expect (mustMatch (selected0, selected0Again),
                "the same state painted twice is the same picture",
                "two renders at selection " + juce::String (processor.getChannelSlot (probe)));

        processor.setChannelEvolveBars (probe, 4);
        const auto rotating = renderGrid (*stepGrid);
        processor.setChannelEvolveBars (probe, 0);

        const auto chipArea = stepGrid->getEvolveChipBounds (probe);

        {
            const auto m = movedBetween (selected0, rotating, probe);
            const auto d = compareImages (selected0, rotating, stripAt (probe).expanded (1));

            expect (m.ok, "a lane that is rotating looks different from one that is off",
                    m.detail + ", " + juce::String (d.darker) + " px down and "
                        + juce::String (d.brighter) + " px up");

            // ... and that the *light* is part of it, not only the word. The pair above
            // cannot tell: `rotating` drives the chip's colour and its text together, so a
            // build whose light is dead still shows "4 bars" against "off" and the
            // difference survives — measured, and arm p2 of out/mutate-bank2.log came back
            // green on exactly that. This asks one rectangle for the lane's own colour and
            // needs it present while rotating and absent while off, where "present" means
            // the nearest pixel in the chip gets close rather than the whole picture
            // changing.
            const auto chip = readsAs (rotating, selected0, chipArea,
                                       composited (processor.getChannelColour (probe)),
                                       composited (gb::Colours::chipBorder),
                                       "the lane's own colour in the chip");

            expect (chip.ok, "and the chip is lit with the lane's own colour, not merely worded",
                    chip.detail);
        }

        // The number itself, which is the only place the period is readable: the light says
        // "rotating", the word says how long. Two periods the ladder offers both light the
        // chip the same way, so the chip's own rectangle is the only thing that can differ
        // between them — and arm p2b of out/mutate-bank3.log, a chip hard-worded "off",
        // is what asked for this instead of leaving the wording to the pair above.
        processor.setChannelEvolveBars (probe, 1);
        const auto oneBar = renderGrid (*stepGrid);

        processor.setChannelEvolveBars (probe, 2);
        const auto twoBars = renderGrid (*stepGrid);

        processor.setChannelEvolveBars (probe, 0);

        {
            const auto d = compareImages (oneBar, twoBars, chipArea.expanded (1));

            expect (d.pixels - d.outside > 0 && d.outside == 0,
                    "and the chip words the period, not merely that it is rotating",
                    juce::String (d.pixels - d.outside) + " px moved inside the chip and "
                        + juce::String (d.outside) + " outside it, " + asText (chipArea));
        }

        // A mark that holds a pattern and a mark that holds nothing are drawn as different
        // shapes, which is the only way to see what a slot carries without pointing at it.
        // This pair differs in exactly one filled slot, and the selection is already on it,
        // so the ring cannot be what moved in the strip.
        processor.setChannelSlot (probe, 2);
        const auto emptyMark = renderGrid (*stepGrid);

        processor.setStep (probe, 6, true);
        const auto filledMark = renderGrid (*stepGrid);

        {
            const auto m = movedBetween (emptyMark, filledMark, probe);

            expect (m.ok, "a slot with something in it is drawn differently from an empty one",
                    m.detail + ", strip " + asText (stripAt (probe)));
        }

        // The copy mark dims when a click there would find nowhere to go. Whether a copy
        // is possible is a fact about the *other* slots' fullness, and the marks report
        // that same fullness, so this pair cannot be isolated to the copy glyph: the whole
        // strip changes. Stated rather than hidden, because a green here does not mean the
        // glyph alone was tested — the click arm above is what gates the behaviour the
        // glyph claims.
        const auto somewhereToGo = renderGrid (*stepGrid);

        for (int slot = 3; slot < numSlots; ++slot)
        {
            processor.setChannelSlot (probe, slot);
            processor.setStep (probe, 6, true);
        }

        // Back onto the slot the first render was taken with, so the two renders differ by
        // one mark's fullness and the glyph's answer, and by nothing else.
        processor.setChannelSlot (probe, 2);

        const auto nowhereToGo = renderGrid (*stepGrid);

        {
            const auto m = movedBetween (somewhereToGo, nowhereToGo, probe);

            expect (m.ok, "and a full bank reports that the copy mark has nowhere to go",
                    m.detail + " - marks and glyph together, not the glyph alone");

            // The glyph on its own, which the pair above cannot reach for the reason in
            // its own comment: filling the last slot is the thing that takes the copy away,
            // so it changes a mark at the same time. This asks the copy rectangle alone how
            // near its brightest drawing gets to the panel's text colour, and needs the
            // answer to be materially nearer while a click there would land. Arm p3 of
            // out/mutate-bank2.log — a glyph that never dims — is what asked for it.
            const auto copyArea = stepGrid->getSlotCopyBounds (probe);

            const auto glyph = readsAs (somewhereToGo, nowhereToGo, copyArea,
                                        composited (gb::Colours::text),
                                        composited (gb::Colours::chipBorder),
                                        "the panel's text colour in the copy glyph");

            expect (glyph.ok, "and the copy glyph itself is the bright one when a click would land",
                    glyph.detail);
        }

        // The other lane's number is the other lane's business. The two lanes have to be
        // answering different numbers first: arm q1 of out/mutate-bank2.log, a
        // setChannelSlot that writes every lane instead of the one it was named, survived
        // the mirror check below for as long as both lanes sat on slot 2, because a second
        // write to the number a lane already holds paints the same picture. Probe is moved
        // off that slot so a write that leaks has somewhere visibly to leak to.
        processor.setChannelSlot (probe, 1);

        const auto beforeNeighbour = renderGrid (*stepGrid);

        processor.setChannelSlot (other, 2);
        const auto afterNeighbour = renderGrid (*stepGrid);

        {
            const auto m = movedBetween (beforeNeighbour, afterNeighbour, other);

            expect (m.ok, "and a neighbour's selection moves only its own row",
                    m.detail + ", lane " + juce::String (other) + " strip "
                        + asText (stripAt (other)));

            // The mirror of the same pair, which the row window alone cannot say: the
            // strip of the lane that was not clicked keeps every pixel it had. What makes
            // this able to bite is the two lanes holding different slot numbers across the
            // pair -- see above -- so a selection write that leaks to every lane moves
            // probe's ring from dot 1 to dot 2 and lands inside this window.
            const auto untouched = compareImages (beforeNeighbour, afterNeighbour,
                                                 stripAt (probe).expanded (1));

            expect (untouched.pixels - untouched.outside == 0,
                    "and the lane that was not clicked keeps its own marks",
                    juce::String (untouched.pixels - untouched.outside)
                        + " px moved inside the strip of the lane nobody touched, which now "
                        + pointerState (probe) + " against " + pointerState (other));
        }

        // Back as found: two empty lanes at the bank's resting numbers.
        emptyBank (probe);
        emptyBank (other);
    }

    std::printf ("\n%s  %d checks, %d problems\n",
                 problems == 0 ? "PASSED" : "FAILED", checked, problems);

    return problems == 0 ? 0 : 1;
}