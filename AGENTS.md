# Groovebox -- agent instructions

JUCE 8.0.4 C++17 sample step sequencer (16 steps x 12 channels, per-channel pitch,
ADSR, velocity, probability and choke group, swing, late-only timing humanize, master
trim + peak meter). VST3 + Standalone.
Plugin manufacturer code `Eruw`, product code `Grvb`, version from
`project(... VERSION ...)` in [CMakeLists.txt](CMakeLists.txt).

## Deliverables: `launch/` is the one place to build, run and ship from

```powershell
cmake --build build-new --config Release --parallel 2 --target launch   # exe + bundle -> launch/
.\installer\build-installer.ps1                                         # refreshes launch/, then packages the setup
```

`launch/` holds `Groovebox.exe`, `Groovebox.vst3/` and `Groovebox Setup <version>.exe`.
It is fully gitignored and nothing in it is authored in place.

- Verify an export by **SHA-256 against the artefact tree**, never by timestamp or size:
  `copy_if_different` leaves an old mtime on a file that exported correctly, and rebuilds
  often coincide on size. Export shipped 2026-11-16 from `b603107`: `Groovebox.exe
  A3948E47...`, bundle binary `CF3F8318...` -- both equal in `launch/` and in Program Files.
- **A rebuild is not the install, and the difference is four bytes on identical sources.** Measured
  at the cheapest target in the tree (the std-only planner suite, ~1 s): `touch` one test file --
  content unchanged -- relink, and the two exes differ in exactly 4 of 198,144 bytes, at `0x108`
  and `0x2a394`, which are the COFF File Header `TimeDateStamp` and the `IMAGE_DEBUG_DIRECTORY`
  entry's own stamp. Zero those two fields in both and the hashes match (`e2ec30d0fdf48320`). So
  `/RELEASE` is not hashing the debug directory -- it is two clocks -- and the useful rule is
  neither "hashes must match across builds" nor "hash comparison across builds is meaningless":
  a mismatch confined to those four bytes is the link time, and **anything bigger means the sources
  genuinely differ, which is the answer you were looking for.** Compare per file within one build
  (the rule above, the gate); across builds, strip the stamps first, then believe the diff.
- The bundle is **removed before being rewritten** ([CMakeLists.txt:152-163](CMakeLists.txt))
  so a file that no longer belongs to the build cannot survive in it.
- `launch` is a **target, not a POST_BUILD step**, on purpose: a host with the plugin
  loaded holds the DLL open, and failing a compile over a copy only the release step
  needs would hide the real build result. `tasklist` only finds a candidate (`Ableton
  Live 12 Suite.exe`, not `Live.exe`); `(Get-Process -Id <pid>).Modules` says whether it
  holds Groovebox -- prove that, then let the user close it, never kill it: no autosave.
- `--parallel 2` and no `/MP`: MSVC 14.51 emits intermittent D8040 internal compiler
  errors on JUCE's larger modules when both layers parallelise
  ([CMakeLists.txt:9-17](CMakeLists.txt)).

## Judging sound: measured, level-matched, never louder-is-better

- A render that is **louder is not a render that is better**. Compare A/B at matched
  integrated or peak level and say which one was matched; if the comparison cannot be
  level-matched, it does not support the claim being made.
- No gain as improvement, no clipping, no over-distortion. There is deliberately **no
  limiter** on the master -- the trim and the faders are what the user has
  ([Source/MasterBus.h](Source/MasterBus.h)); the clip latch is a report, not a guard.
- **Never compensate a parameter silently.** Removing the hidden x0.8 master sum meant
  adding the visible -24..+6 dB trim (default -2 dB, chosen so the default output stays
  level-matched with the old build) and migrating saved values -- not changing what a
  fader means behind the user's back.
- **A colour law is transparent at the top of its travel.** Velocity is two laws now,
  gain and tone, and the tone one-pole is bypassed outright at full velocity
  (`SequencerEngine::brightnessMixFor`), so a grid hit leaves the engine at the bits the
  sample was recorded at. The check named "a full-velocity hit is the sample at the trim"
  is what stops a future corner value from quietly retuning the whole kit.
- **A curve fix does not get to move a level.** The exponential decay/release and the
  velocity lowpass landed with every gain constant where it already was; the one
  existing check whose number moved was re-banded with the measured value and the reason
  in the comment, not widened to hide a difference. The tone law does cost level on a
  bright source (1.9 dB of peak at quarter velocity, on flat noise) and that cost is
  stated in the README instead of being bought back invisibly.
- **A gate on an unmeasured defect has to be two-sided.** The kernel edge was written up
  as an attenuation of the transient; measured on a flat source it was a **7.4 % overshoot**
  of the first sample followed by a 2.8 % dip. A `head <= body * 1.02` check passed on the
  broken build. Ask for the head to equal the body in both directions, and let the run say
  which way it was wrong. The same run is how locality is proven: diff the disabled-fix
  output against the fixed one -- `[24]` and the count line were the only eleven lines that
  moved, so nothing else in the engine got retuned.
- **A tuning control is not allowed to be a tone control.** The pitch drop kept the
  anti-alias band it picked at note-on -- the band for the top of the sweep -- and that
  cost **-4.03 dB** of the 2..7 kHz band at a drop of 12, of which -3.29 dB was the band
  and -0.73 dB the kernel's own price (measured against a +1-semitone row, because a
  broadband tilt ratio cannot tell those apart on its own). Following the rate takes it to
  -0.51 dB, 0.995 of the kernel baseline. The band is still never looser than the rate
  being read, and a band switch is not a step: the kernels are symmetric about the same
  centre tap, so below the tighter cutoff they are the same signal.
- **A byte-identical A/B falsifies your explanation, not the test.** The claim that a
  stale `samplesToSourceEnd` "cuts the hit in half" survived writing into the plan and the
  engine comment until the disabled-fix run came back with identical bytes: the countdown
  is re-derived from the current rate at the top of every `renderVoice` call
  ([Source/SequencerEngine.cpp](Source/SequencerEngine.cpp)), so a stale one can only
  survive one block. The real defect is block-size dependence -- 570 of 1576 samples gone
  at a 1024 block, nothing at 256, which is why every fixture in the suite had missed it.
  Re-measure where the mechanism can actually show, and let the gate say that instead.
- **A red test is a question about the signal before it is a question about the limit.**
  The true-peak meter came in reading 0.9042 for a 0.8 sine at 441 Hz, and the write-up
  that was already in the test called it the honest over-read of a short kernel meeting
  its own image near Nyquist. It was the fixture: a tone whose first sample is at full
  amplitude is a **step**, and a band-limited reconstruction of a step rings above the
  crest for as long as the kernel is wide -- the peak landed on sample 12, 13 or 15, the
  first intervals a 24-tap window can span. A Python model of the same kernel with zero
  history matched the C++ to five digits; with a 64-sample raised-cosine on the start,
  both the model and the bus read the crest to 0.00011 and the Fs/3 gap to 1.1549 against
  the analytic 1.1547. Widening that gate from 0.02 to 0.07 would have written a
  fixture's start edge into the DSP's spec, the README and a check designed to pass;
  fixing the tone let the limit tighten and turned a claimed weakness into a measurement.
- **A report has to be proven a report.** "The tick gates nothing" is not shown by a
  signal that stays under 0 dBFS anyway. The square wave in `[26]` puts the reconstruction
  at 1.1378 with not one sample past 0.9000, and asserts the clip latch stays **clear**
  ([Source/MasterBus.h:150](Source/MasterBus.h) latches on the sample, not the estimate).
  Same rule for the meter's own accuracy: the sweep checks the reported *gap* against
  `1/cos(pi/N)` rather than only checking that the tick is high, because a filter with
  gain would pass the second and fail the first.
- Anything that reads like a spec in [README.md](README.md) or
  [installer/README.txt](installer/README.txt) must be code-verified before shipping.
  Three false claims were caught this way: that the sound button's menu switches between
  loaded samples (it only opens a `FileChooser`; `SampleStore::getSampleIds()` has no UI
  caller), that there is no MIDI note input (there is, C1-B1 = notes 36-47,
  `Layout::firstMidiNote` in [Source/Common.h:58](Source/Common.h)), and that Live scans
  `Documents\VST3` (it does not -- see Host folders below).
- **A fit claim states both axes and names the code that decides each.** 4.3b shipped that the
  wrap "lets it go down to **1218 px** — a 1366 px laptop" (README.md:440) and that the
  "editor floor **1218×844**, which does fit 1366×768" (NEXT-STEPS.md:370) -- in the same
  paragraph that then admitted the height half was not paid for. The width is true (1218 <=
  1366); the height is not, because [Source/PluginEditor.cpp:101](Source/PluginEditor.cpp)
  passes 844 straight to `setResizeLimits` as the editor's *minimum* while a 768 px screen has
  roughly 730 px of it -- the bottom strip sits off-screen and no resize brings it back.
  `Tests/LayoutTest.cpp:803` prints that floor already, so this was one subtraction away rather
  than a measurement away: a claim that survives checking only one axis is the dangerous shape.
  Both sentences were corrected by the shipping lane in `71c04fd`.
- **A verified number is not a verified claim.** The Touch A/B quoted `peak 1.438488 -> 1.485904`
  straight out of the run log, so every digit was true -- and both numbers are past 0 dBFS, because
  the fixture is four lanes x all sixteen steps at chance 1.0 on the whole velocity ladder
  (`Tests/SmokeTest.cpp:7861-7867`), a load the genre table never asks for (densest row 10 of 16,
  average 3.4). So the copy reported a rise in *clipping* as if it were the cost of a control, while
  the headroom section two pages earlier says the latch fires at 0 dBFS and there is no limiter.
  When shipped copy quotes a measured level it states the fixture that produced it, whether that
  fixture is a playing state, and which trim was in place -- and the same test arm's relative ceiling
  (`<= shut * 1.25`, `Tests/SmokeTest.cpp:8012`) is an anti-renormalisation gate, not a headroom gate,
  which is worth saying in its comment because nothing else in the suite bounds the summed peak.
  Corrected by the shipping lane in `fbf973c`.

## Tests: three console apps plus a source guard, gated by CTest

```powershell
build-new\GrooveboxSmokeTest_artefacts\Release\GrooveboxSmokeTest.exe   # timing, swing, pitch, state round-trip, automatability
build-new\GrooveboxLayoutTest_artefacts\Release\GrooveboxLayoutTest.exe # real editor, bounds of every control
build-new\Release\GrooveboxPatternPlanTest.exe                          # genre table + planner, std-only, no JUCE
ctest --test-dir build-new -C Release -V                                # the tracked gate over all four
```

The first two compile the plugin sources headlessly ([CMakeLists.txt:166-231](CMakeLists.txt));
the third reads only `Source/GenrePatterns.h` and `Source/PatternPlanner.h`, links no JUCE and gets
no generated header ([CMakeLists.txt:232-335](CMakeLists.txt)), which is what keeps it at about a
second. All four entries are `add_test` cases carrying a `PASS_REGULAR_EXPRESSION` that requires a
`, 0 ` problem count, so a run that prints nothing and a run that prints a problem both fail the
tracked path even at process exit 0, and `-V` is the documented form because the quiet one prints no
counts. Last green run: 422 smoke checks, 441 layout checks, 85 planner checks, 0 problems, guard OK.

**CTest does not build.** Measured by the lane that wired it (`ecfe76c`): with `GenrePatterns.h:101`
broken and no rebuild, `ctest` reported 4/4 passed while the sources no longer matched the binaries.
So `ctest` is the tracked *replay*, and `out/run-gate.ps1` / `out/gate-plan.ps1` -- which build,
refuse to run an exe older than their own inputs, then run the suites and the guard -- stay the local
gate. The two are not interchangeable and neither alone is the whole check; run the build first.

- **A level gate can be red because of the fixture's own ramp, not the code under test.**
  Section [30] asserts that a humanised hit is the *same samples* as the grid hit, only
  later, and it came back red by 0.000415564 on a head sample 482 output samples apart --
  with the two peaks also differing, 0.602598 against 0.602140. The engine was not
  retuning anything: `MasterBus` moves its trim through a 20 ms one-pole
  (`smoothingSeconds`), which needs about 13 time constants to reach a target, and the
  harness set the trim *after* `prepareToPlay`, so both renders started from unity and
  climbed at slightly different phases. `currentGain` is set to the target only in
  `setSampleRate`, so `prepareToPlay` is the moment the smoother is told where to sit.
  Set the globals before the prepare and the two renders are bit-identical. The rule: when
  an A/B that should differ only in *when* something happens reports a *level* difference,
  ask what in the fixture is still moving, and fix that instead of widening the limit. And
  make the passing run say so: the level check now prints both peaks on success
  (`level: peak 0.601509392 on the grid, 0.601509392 late` in `out/smoke-43.txt`), because
  a gate whose evidence only appears when it fails cannot be re-checked later.
- **A proportionality gate is asked in the unit the law works in, and of one voice at a
  time.** [31] checks that half the Touch fader is half the departure. Asked between the
  struck floats it came back red on 101 keys by up to 13 steps, and neither half of that
  was the engine. The rounding bound is exactly one step, so it lands on its own limit and
  the last bit of the `/ 127` in the seam's return crosses it: compare `round (d)` with
  `round (d / 2)` in steps. And the 13 were the clamp, not the law -- a rung near 127 has
  its positive departures cut off, so both arms land on 127 and are *equal* rather than
  halved; the exemption is counted, held under a quarter of the take, and paired with the
  weaker claim that survives it (opening the knob never moves a hit back towards its rung).
  The same asymmetry is why the level cost has no exact ceiling on a summed take: three of
  four voices at one step pulling up beat a shut step whose loudest rung is only one, which
  is how the four-lane peak moved 1.438488 -> 1.485904 while the single rung-127 hit -- the
  arm where monotone gain and monotone tone make `<=` arithmetic -- came down 0.601509 ->
  0.577455. Ask the exact question where one voice can answer it, and print the rest.
- **A correlation gate cannot see a law reading the wrong bits.** [31] asserts that how hard
  a hit is struck is independent of when it lands and of whether it fires at all -- and both
  assertions stayed green when the level law was mutated to read the roll's slice, and again
  when it was made to read the timing's. The reason is the shape of the key rather than the
  size of the sample: `unit` is a 16-bit window of a 64-bit hash, so conditioned on any other
  slice's outcome it is still exactly uniform (each value appears four times in [0, 2^23)),
  and a point-biserial r against a binary roll outcome is 0.0000 by arithmetic, not by luck.
  Read off the bottom 24 instead and `unit_t = (unit_l + H) / 256`, which measures r = 0.0039
  against a 3-sigma limit of 0.0674. What does see it is a gate that **names the bits**:
  re-derive the key inside the test, predict the struck rung, the lateness and the
  fire-or-not for all 4096 cells from the three slices as documented, and require zero wrong
  in each. A correlation says two quantities are independent; only an exact prediction says
  they came from the place you claim they came from.
- **A scan for a seed that shows a defect has to switch the defect on first.** Two [30]
  arms looked for a seed whose draw lands in a usable window, and both did it with the
  humanize knob shut, where the function returns 0.0 for every key. The search finds
  nothing, the dependent checks are skipped or vacuously satisfied, and the run prints a
  pass. Any knob-gated search needs the knob set *before* the loop and a printed
  precondition next to it that says the thing being searched for exists at all.
- **A limit that is derived from a measured quantity is checked against that quantity.**
  `Layout::maxHumanizeLateSteps` is 0.25 steps because the closest two onsets the swing
  grid can produce are 0.5 steps apart -- measured at the shuffle on both grids (0.5000,
  0.5000, and 1.0000 straight), not read off a formula in the test. Same rule as the click
  ruler: derive from the signal, and let the check ask the function that decides the
  behaviour rather than recomputing it in the test file.
- **APVTS state children are keyed by a lowercase `"id"` attribute, and a missing child is
  a default, not a bug.** `Params::humanizeId` is matched with `getStringAttribute ("id")`;
  asking for `"ID"` finds nothing and the arm reports one fewer element than it removed.
  Deleting the row from a saved state and loading it leaves the parameter at the
  `RangedAudioParameter` default via `valueTreeRedirected` ->
  `updateParameterConnectionsToChildTrees` -- which is what makes "a session that predates
  the parameter opens shut" a property of the framework worth a gate rather than a hope.
- **A JUCE double-click is not four mouse-downs.** The click count a `Slider` uses for
  `setDoubleClickReturnValue` comes from the OS through `MouseInputSource`, so synthetic
  down/up pairs in a test leave the fader where the drag put it and the check silently
  measures a different gesture. Dispatch `Slider::mouseDoubleClick (event)` directly with a
  constructed `MouseEvent`; the handler that runs is the one a real double-click runs.
- **A walk that runs zero times looks like a wrong answer, and partly like a right one.**
  Section [29] built its 4096-cell decision matrix with three mirrored loop headers --
  `for (step = numSteps - (backwards ? 1 : 0); backwards ? step >= 0 : step < numSteps;` --
  and in the forward direction that starts the counter *one past* its own end condition, so
  the body never ran and every cell kept its allocated zero. Seven checks went red against
  an engine that had been correct all along, which was the lucky half. The other half: three
  checks went **green** on the empty answer, because "no cell comes back as the knob opens"
  and "a cell that says never is never kept" are both satisfied by nothing at all. So a
  matrix walk gets one index and a mirror, not two spellings of the same loop; and a check
  whose pass condition is `== 0` or `> someFloor` needs the precondition that the thing being
  counted is non-degenerate, next to it in the same section. [29] prints `fired` counts for
  both knob settings for exactly that reason -- a zero in the wrong column is the bug report.
- **A `MemoryOutputStream` reports the allocation, not the payload, until it is flushed.**
  Decoding a base64 step record into an externally-owned `MemoryBlock` left `getSize()` at
  832 for a 576-byte record, because the stream rounds the block up as it grows and only
  shrinks it to `bytesWritten` in `flush()`, which the destructor has not yet run. An
  equality on the decoded width therefore measures the allocator's growth policy. [19]'s
  probe survived for months because it only ever asked for a *lower* bound. Flush, then
  read the size, and take the data pointer *after* the flush -- shrinking can move it, and
  the bytes would otherwise come from freed memory.
- **A tolerance on a hash is not a coin's standard deviation.** The same section first
  gated the fired fraction at four sigma of a fair coin over 4096 cells (0.0271) and the
  actual take sat inside it by 0.0061: one legitimate change of the key would have gone red
  on correct DSP. A splitmix64 draw is a fixed permutation, so its spread over neighbouring
  seeds is a property of it that can simply be measured -- the shipped gate takes three
  times the worst departure of 31 other seeds from their mean (0.0495), with the tested seed
  deliberately *out* of that sample, because a limit drawn around the point it then gates is
  a tautology. The comparison target is also the engine's own formula re-evaluated
  (`1 - 0.5 * (1 - 128/255)` = 0.75098), not a rounded saying of it.

- **A baseline you cannot re-check is an opinion.** Section [28] fingerprints one fixed
  kit -- the same arming, the same 161280 samples, FNV-1a over the length and both
  channels' bits -- against a recorded literal, `4BC3AA231178C071`, whose provenance is
  written next to it. It is what makes 4.2's "bit-identical at probability 1.0" a
  machine answer instead of a listening session. Two sides, both in the same section:
  the hash must repeat on a re-render, and it must *move* on a perturbation (one lane
  softer, a shorter render), or it is a constant wearing a gate. The fixture's velocities
  are all `k/127` because the payload stores a 7-bit level, so a non-lattice value makes
  the save/load half fail for the wrong reason.
- The smoke harness needs `/STACK:16000000` ([CMakeLists.txt:194-202](CMakeLists.txt)):
  a `GrooveboxProcessor` is 23 KB and `main` holds dozens of them, so the default 1 MB
  reserve dies with `STATUS_STACK_OVERFLOW` **printing nothing** -- stdout is buffered
  and lost with the process. A section added at the end of the file moving *where an
  earlier one crashes* is the signature; it is the harness's frame, not the plugin's.
- A test that wants `setStateInformation` to actually load something has to hand it
  `copyXmlToBinary` output: `getXmlFromBinary` only recognises that helper's own 8-byte
  magic+length header and answers bare XML with `nullptr`, which the processor reads as
  "no session" and acts on not at all -- a silent green that was really a no-op.
- **Trust the link, not the exit code.** `Copy-Item` preserves the source's mtime, so a file restored by copy
  can be *older* than the object MSVC built from the version it replaced, and the incremental build then
  reports 0 while running stale code -- one A/B half here linked the old codec against the new header, which is
  also a class-layout mismatch. `out/run-gate.ps1` and `out/ab-41.ps1` refuse to run when a source is newer
  than the exe; after copying into `Source/`, set `LastWriteTime = Get-Date`. **The glob is per target**: each
  console app compiles `Source/` plus its own `Tests/*Test.cpp`, and `cmake` correctly leaves the *other*
  exe alone, so a guard over all of `Tests/` refuses permanently the moment only the other test was edited --
  a guard that can never pass is not a guard, and the first thing it trains is the habit of editing the script.
- No modal loops: `MessageManager::runDispatchLoopUntil` needs
  `JUCE_MODAL_LOOPS_PERMITTED` and JUCE 8 has no public single-step message pump, so the
  layout test drives `ChannelStrip::refresh()` -- the same `syncAutomatableControls()`
  path the 30 ms timer uses -- instead of pumping messages.
- **A JUCE radio group cannot express "neither", so the two exclusive grid toggles are
  made exclusive by hand.** `Button::internalClickCallback` computes
  `shouldBeOn = (radioGroupId != 0 || ! lastToggleState)`: a button in a group lights and
  never goes out when you click it again. Both overlays *can* be off -- that is the
  resting state -- so `TransportBar` uses `clickingTogglesState` with no group and closes
  the other button in the handler (`closeOverlay`, TransportBar.cpp). Enforcement belongs
  there rather than in the grid: `StepGridComponent::openOverlay` already keeps one
  overlay, but a panel with two lit buttons for one overlay is a panel that lies about
  which field a drag is going to write, and only the bar can see both controls.
- **Drive a headless toggle with `setToggleState (v, sendNotificationSync)`, not
  `triggerClick()`.** `triggerClick` posts a message and this harness has no pump; the
  former sets the state and *then* runs the click callback synchronously, so `onClick`
  reads the new state, which is what a real press does. `sendNotificationAsync` is
  `jassert`ed against on that path, so it is not an option either.
- **A control identified by its range is identified by a number it shares.** The layout
  test used to tell the bar's faders apart with `getMaximum() <= 1.01`, which was
  two-way arithmetic: the third 0..1 fader (Mutate) would have been driven and then read
  back against swing's parameter, and gone red on correct code. The bar now *names* its
  controls -- `Swing`, `Master`, `Mutate`, `Seed` -- and the test asks by name. The same
  trap as the parameter table, one layer up: a property two components happen to agree on
  is not an identity, and the second copy of a classification is where the third control
  gets misfiled.
- **An A/B arm that starts from the previous arm's file measures the first mutation
  twice.** `out/ab-42b.ps1` ran three breakages of the step-probability corner mark and
  all three came back with the *same* four BAD lines: each arm edited the working copy, so
  once arm one had deleted the `paint()` call that draws the mark, arms two and three were
  removing pixels from a picture that had none. Restore the recorded good file at the top
  of every arm. The tell afterwards is that the arms disagree -- five of them now kill
  4 / 1 / 2 / 2 / 1 checks -- and every check is killed by at least one.
- **Choose the control where the defect can actually show.** The overlay half of that
  mark's test compared two renders at velocity 1.0, and a velocity bar at 1.0 fills the
  whole cell, so a mark that leaked into the overlay was painted over and the check passed
  on a build with the overlay rule deleted -- a negative control that cannot go red is
  not a control. At velocity 0.1 the bar is 3 px tall, the notch is 10, and the same
  mutation fails it. State in the comment what geometry makes the check able to bite.
- **A claim about pixels needs pixels.** `StepGridComponent::paint` is public, so the
  layout test renders the grid into a `juce::Image` and compares two renders that differ
  in one number: where the pixels moved, that they moved only inside that one cell, and
  that the mark is a two-tone cut (some pixels down *and* some up) rather than a recolour.
  Which arms cannot kill a check matters as much as which can -- the off-cell assertion
  passes on a build whose predicate ignores `isStepOn`, because `paint()` only draws marks
  inside the lit branch, so the predicate's own term is discharged by the rule check
  instead. Say which is which rather than letting eight green checks imply eight controls.
- **A bounds check has to ask for the size, not just the position.** The hover readout is a
  label 12 px above its cell, and the first check asked only whether `getY () >= 0` and the
  bottom was inside the grid -- so it passed on a rectangle **1 px tall**. The helper had been
  written with `Rectangle::setTop`/`setBottom`, which in JUCE 8.0.4 move an edge and resize
  (`setTopLeft`/`setBottomRight` are the ones that move); the rewrite that fixed that was wrong
  twice more -- a corner-by-corner clamp squashes a rect wider than its parent, and a
  `jmin (leftOverflow, rightOverflow)` shift only ever pulls positive, so the right-hand column
  stayed clipped. Slide with `jlimit` on the far corner, keep the size, and gate both:
  `getHeight () == 11`, `getWidth () >= 22`, all four edges inside, for all 192 cells.
- **A gate that asks the predicate under test what it expects is not a gate.** The bar's wrap
  check compared its height with `TransportBar::heightForWidth (its width)` -- the same function
  `resized ()` had just used, so it proved the editor and the bar read one rule and said nothing
  about the rule. Mutating `isWrappedForWidth` to `return false` left that, the line-membership
  and the fader-width checks all green; the run only went red because a 1774 px row does not in
  fact fit in 1182 px and a seed label collapsed to 0 px. Two of the arms are now stated in the
  rule's own numbers instead -- `oneRowHeight` at `>= oneRowWidth`, `twoRowHeight` below -- and
  those are what catch `<= oneRowWidth` (1 problem) and never-wrap (4) as *different* failures.
  The sizes are widths of the component, not of the window: the test measures the editor's inset
  off the running editor rather than typing the 18 px margin a second time, for the reason in
  the bullet above about a list written twice.
- **A control's colour is not its word.** Two of the bank's pixel checks -- "a lane that is
  rotating looks different" and "a full bank reports the copy mark has nowhere to go" -- were
  carried end to end by the *text* drawn beside the controls: arm p2 (the chip's lit branch
  deleted) and arm p3 (the copy glyph never dims) both came back green, and the chip's own
  rectangle measured 0.000 from the lane colour in the lit render and 0.659 in the dim one all
  the same. A diff between two renders cannot attribute anything when the state that flips the
  control's answer flips something else in the same window -- the bank's slot marks move with
  the fullness that takes the copy away. So the gate is an **absolute read of one control's own
  rectangle**: how near its brightest pixel gets to the colour it claims, and how near the
  other render's gets to the colour it claims to have dimmed to.
- **A colour margin is derived from the palette, like a slew limit is derived from the
  fixture.** The threshold is `0.5 x colourDistance (litColour, shutColour)` -- 0.396 for the
  chip, 0.753 for the glyph -- not a typed `0.15`. It fails safe: a theme that draws lit and
  shut nearly equal asks for nearly nothing, and the check that goes red on the next edit of
  `Colours.h` is the one that says so. In JUCE 8.0.4 `Colour::getRed()` returns **uint8**; the
  float accessors are `getFloatRed/Green/Blue` and the float factory is
  `Colour::fromFloatRGBA` (there is no float RGBA constructor), so a distance built from the
  integer accessors is a 0..255 quantity compared against a threshold stated in 0..1 units.
- **Two lanes have to hold different numbers before a render pair can see a write that
  leaks.** "The lane nobody touched keeps every pixel it had" passed arm q1 -- a
  `setChannelSlot` that writes all twelve lanes -- because both lanes already sat on slot 2
  and a second write to the number a lane holds paints the same picture. The check now puts
  them on 1 and 2 first. Same shape as the parity trap below: a pair only discriminates
  across the difference the arm can produce.
- **A guard defended by another guard is not a control.** A dead pixel in the bank's row is
  refused in **three** places, in series: `channelAtY` says the bottom sliver of a row span is
  not a row, `bankTargetAt`'s strip guard says the point is not in the strip, and each
  control's rectangle says the point is not in the control. Arm o (strip guard gone), arm s and
  s3 (dots given the row band), arm s2 (both of those) and arm z1 (strip widened to the band)
  each came back **green** on the gap check, and arm o is still green because the rectangles
  refuse: the guard it deletes is genuinely discharged, and the docs say so rather than
  counting it as a control. What gave each refusal an arm that reaches it was taking the
  click at four x positions -- over a mark, over the copy, over the chip, over the first cell
  -- because the cell column has no strip guard in front of it, only `channelAtY`. Rounds one
  to ten, 47 arm runs, and all 49 checks in the section have been seen to go red.
- **"Nothing changed" has to name everything that could have changed.** The gap and cells
  checks used to assert that the *selection* held still, which a stray rotate satisfies
  perfectly -- the counter moves and the selection does not. They now compare one signature
  string for the lane: slot, period, which slots hold something, and the sixteen step bits.
  The detail prints it, so a red run says which field moved rather than what did not.
- **An alternating mutation is invisible to a per-render check.** The first arm for the
  determinism precondition was `bankEdgePad + (++jitter % 2)` in `getSlotDotBounds` and it
  killed three checks but **not** that one: a render calls the helper 48 times (12 lanes x 4
  slots), so the parity at the start of every render is the same and two renders agree
  exactly. `++jitter` with no modulus moved 23 checks and reddened the precondition. When the
  falsifier is "this must differ between two calls", count the calls between them.
- **`|` is not a safe sed delimiter in this codebase.** `s|...||...|` on a line containing
  `||` ends the command early and the arm reports `SED FAILED` -- round two's arm o, silently
  absent from the table until the log was read. Use `/`, or pick a delimiter the line cannot
  contain, and have the script print `SED CHANGED NOTHING` when the mutation is a no-op so a
  green arm is never an unapplied one.
- **The fit rule is one predicate, not a width repeated.** `StepGridComponent::hasBankStrip`
  answers "does this row have room for the strip" for the painter, the hit test and the layout
  test alike; the test asks it as two numbers (`bankStripHeight + bankNameLineHeight` against
  `rowHeight`, `bankMinimumLabelWidth` against `labelWidth`) rather than calling the predicate,
  which is the rule in the bullet above about asking a predicate what it expects. The row
  window and the column window are read separately -- `channelAtY` for the row,
  `getSlotDotBounds (channel, slot)` for the mark -- because a point that is in the right
  column and the wrong row belongs to nobody.
- Automatability is testable single-threaded because
  `AudioProcessorParameter::setValueNotifyingHost` notifies listeners synchronously.
- JUCE 8 removed `AudioProcessorValueTreeState::setParameterNotifyingHost`; the one
  remaining way in is `apvts.getParameter (id)->setValueNotifyingHost
  (apvts.getParameterRange (id).convertTo0to1 (v))`, wrapped as
  `GrooveboxProcessor::writeParameter` ([Source/PluginProcessor.cpp](Source/PluginProcessor.cpp)).
  116 parameters (8 global + 12 x 9); the pattern is deliberately not one. The listener
  list in `allParameterIds` ([Source/PluginProcessor.cpp:24](Source/PluginProcessor.cpp))
  is built from `Params::globalIds()` and `Params::laneTargets()` for the same reason:
  when it was a hand-written copy of the lane ids, the choke parameter was created,
  offered to the host and read on restore with nothing listening for it, and only a test
  that writes all of them could see that the engine never moved. Mutate did it again
  three controls later -- the hand-list had a *fourth* hand-written half, so the trap
  was not the lane list, it was any list written twice. `Params::globalIds()` carries
  its length in its type, so forgetting a row is a compile error, and test [21] writes
  every id in both tables and then compares the count against what the host is offered.
- **The click ruler is derived, never typed.** Every fade, corner sweep and choke gate
  is 1.5 x `maxDelta` of the 997 Hz tone fixture -- the wave's own sample-to-sample slew,
  0.1419, so the limit reads 0.2129 and a real one-sample cut measures 0.7943. It follows
  the signal being measured, which is why a different test source cannot make the gate
  lenient by accident. A new discontinuity check multiplies `sourceSlew`, it does not
  copy a constant.

## Doc citations: a line number is a claim about one tree

`README.md`, `AGENTS.md`, `NEXT-STEPS.md` and `installer/README.txt` point at code as
`SequencerEngine.cpp:151-159`, and every phase that adds lines moves those numbers. A doc whose
pointers land in an unrelated comment is worse than one with no pointers, because it is trusted.
Two scripts in `out/` keep them honest, and the rules below are what each one cost to learn. Say it once,
up front: those scripts and their pin list live in gitignored `out/`, so this gate belongs to a working
tree and not to a clone -- plan item **6.7** is the decision to move them, and until then nothing here is
reproducible from `git checkout`.

- **A sweep shift preserves what the base's number pointed at -- it cannot repair a pointer that was
  already wrong at the base.** `out/cite-sweep.py` renumbered 43 citations from `bcd6188` onto the 4.4
  tree and the second pass found nothing, which is arithmetic and therefore trustworthy; the same
  pass moved a number that was stale where it was written into a number that is stale everywhere.
  Section A3's read of `soundingChannels` sat at `SequencerEngine.h:309` in `460b3bf`, the tree its own
  section names, and left the sweep at line 373 of that file -- correct at no baseline. So the pair is one
  check in a fixed order: `cite-sweep.py -apply`, re-run and require `to correct: 0, unresolvable: 0`,
  then `cite-audit.py` -- which asks whether the cited lines sit in the symbol the *sentence* names --
  and require `UNPINNED miss ... : 0`. The `JUDGE` lines it prints alongside that -- citations whose
  sentence describes an effect instead of naming an identifier -- are a stated limit, not a pass, and
  the run counts them so this file does not have to retype a number that moves every time a paragraph
  is written: a script that invented a match there would be exactly the unfalsifiable check this file
  warns about.
- **A rewrite has to be keyed, not incremental, or it shifts twice.** The target number is computed
  from the base's own citation and applied to the line matched by its text with every digit masked out.
  The first version added the delta it found instead, got run twice in one sitting, and moved 34
  citations a whole phase too far; the only reason that showed up is that a second dry run still
  wanted to change them. Idempotence is the evidence, so keep it as the gate, not the property.
- **A doc section that names its own baseline keeps that baseline's numbers, and a pin must
  *restore* the base's text rather than merely skip.** Skipping left A3 already-shifted: an exception
  that only declines to act cannot undo the action it declines. `out/cite-pin.txt` carries the range
  and the reason, and the two reasons are different claims -- `pinned` (the section declares its
  tree, so moving the number falsifies the record) and `unverified` (the audit names a symbol and
  cannot find it at those lines). 15 entries were the second kind; measured against `460b3bf`
  afterwards, **13 of them were right where they were written** and only went stale, which is the
  honest reason a map cannot fix them. The findings, not the tool's opinion, are tracked item 6.6.
- **A pin cannot hold prose written this phase, so its sentence has to name its tree.** Pin ranges
  are base coordinates matched by masked text, and a line that does not exist at the base cannot be
  masked into one -- so the audit takes a third kind of exception from the prose itself: a commit id
  in backticks within 80 characters of the citation exempts it, as `baseline-qualified`. It is
  checked *after* the symbol test, so it cannot cost the audit a citation that already passes, and
  every exemption is printed with the number and the sentence's commit beside it -- the count of
  them is one line, and the list is what a reader can actually police. Both sides were measured:
  the A3 sentence at the head of this section reports `BASE`, and with the `` `460b3bf` `` taken out
  of it the same citation comes back `MISS`.
- **A pin is written in a named base commit's coordinates, never HEAD's, and matched on the
  working-copy side by masked text.** Two reasons, both measured. Indices: the docs grow, so a range
  read as a working-copy index silently pins a different paragraph after any edit above it --
  silently, and plausibly, because documents of this shape repeat themselves. Bases: both scripts
  used to take their "before" tree from `git show HEAD:`, so the moment this lane committed its own
  docs the same two commands over the same tree went from `43 already right, 0 to correct,
  unresolvable: 0` and `UNPINNED miss: 0` to `122 already right, 2 to correct` and `miss: 13` --
  thirteen of the fifteen `unverified` pins had fallen out of the slice they were written against,
  with no error anywhere, because HEAD is a perfectly valid name for a commit that just moved.
  `out/cite-pin.txt` now carries `# base: bcd6188`, both scripts print the base they used, and every label
  they print interpolates it (`edited-since-bcd6188 lines left alone`), because a run that says "HEAD" while
  reading another tree is the same defect in the report that the pin exists to prevent in the number.
  Setting the base back to `HEAD` reproduces the 13 misses: the line is what holds the gate, not luck.
  Advance a base deliberately, in the same pass that re-checks every range under it. One reader
  answers "is this line excepted" (`out/cite_pins.py`, including the base); two answers is how an
  exception quietly becomes a shift -- the sweep still had its own copy of the pin grammar lying
  about unused, which is the same rule as any list written twice in this repo.
- **A number quoted as a mistake loses the colon.** Write "line 373", never `` `:373` ``, when the
  prose is narrating a wrong pointer. The colon form is the grammar both scripts read as a
  citation, so a figure that exists only to be wrong gets judged, shifted and -- worst -- re-verified
  as if it were a pointer. The same reasoning makes an indented table row a listing rather than a
  claim, and the audit counts those separately instead of scoring them.
- **New prose is hand-verified, because the sweep cannot see it.** It leaves every freshly written line
  alone as `edited since <base>` -- all of the 6.6 table and every bullet in this section -- since a line
  with no base context has no key to shift by, and the run prints how many it declined to touch.
  Each current location in that table was therefore checked against the tree by hand,
  and two of them changed the sentence beside them: `getUndoManager` has a caller now
  (`PluginEditor.cpp:166`), so "has no callers" is false at any line number, and since 4.3b velocity
  is gain *and* tone, so "gain only" is prose that needs rewriting rather than renumbering.
- **These files are CRLF on disk, and `grep -c $'\r'` cannot tell you.** `git ls-files --eol` says
  `i/lf w/crlf` for all four docs; the count returns 0 with exit 1 on a genuinely CRLF file, so it
  is not a check of anything. Any script that rewrites them opens with `newline=""` on both sides --
  a translated read-back rewrites every line of the file, not the lines it was asked to touch, and
  the diff then says 1549/1549 instead of what moved. And since `out/` is gitignored, a finding that
  lives only in a script's output does not exist: it goes into `NEXT-STEPS.md` as a numbered item.

## Installer conventions (Inno Setup 6 -- house template, with two deliberate divergences)

The siblings still install per-user and still let Setup close your DAW. This one does not,
on purpose: see the two bolded rules below.

- Payload is read straight out of `launch/` ([installer/Groovebox.iss:35](installer/Groovebox.iss)),
  so a setup can never carry binaries older than the last build; the only exception is
  `README.txt`, a tracked source in `installer/`.
- **`CloseApplications=no`** ([installer/Groovebox.iss:80](installer/Groovebox.iss)).
  With `yes` -- and the house default filter `*.exe,*.vst3` -- a silent install that
  finds the bundle locked force-quits the host holding it, which on this machine means
  Ableton Live with an unsaved set. Failing to write a locked file, with the README
  telling the user to quit the host, is the safe outcome. Sibling installers still carry
  the hazardous pair: [skulpt/installer/SKULPT.iss:63-64](../skulpt/installer/SKULPT.iss),
  [pulsekit/installer/PulseKit.iss:66-67](../pulsekit/installer/PulseKit.iss);
  `autostem`, `midi-vault` and `dj-library` use `yes` with the default filter, and
  `sample-vault` names only its own two exes.
- A fixed `AppId` GUID identifies the product across versions; never reuse a sibling's.
- Run ISCC and any setup exe **from PowerShell, not Git Bash**: MSYS rewrites `/Qs`,
  `/DIR=...` and friends into paths, and ISCC answers "You may not specify more than one
  script filename".
- **The install itself is run through a UAC prompt the agent shell cannot answer.** A plain
  elevated-free attempt from here dies the way sandboxed Setup always does -- compilation,
  command-line parsing and its own temp extraction all clear, then Windows denies the first
  directory outside the project: `Error 5: Access is denied`, exit code 4 after a rollback,
  no install and no damage.
  `%TEMP%` itself is refused even earlier because `GetTempPath` hands back the 8.3 form
  `C:\Users\Sebastien\AppData\Local\Temp` as `C:\Users\SEBAST~1\...`; setting `TEMP` to a
  path inside the repo gets past that. With everything retargeted inside the repo the
  process hangs before writing any log and registry writes come back `Error 32: Sharing
  violation` -- the full observation log is `out\test\NOTE.txt`.
  What does work, and is how the 2026-11-15 install/uninstall/reinstall was verified: hand
  the elevation to Windows and to the user, from PowerShell rather than Setup directly --
  `Start-Process -Verb RunAs -Wait -PassThru` with `/VERYSILENT /NORESTART
  /SUPPRESSMSGBOXES /LOG=<path in the repo>`. Setup then installs machine-wide for real
  (`Administrative install mode: Yes`, `Install mode root key: HKEY_LOCAL_MACHINE` in its
  own log) and exits 0, with the user clicking the prompt. Two traps: do **not** wrap that
  call in `timeout N` -- it kills PowerShell mid-UAC-wait, and the script deletes its log
  first, so the reinstall fails silently with no evidence. And pass `-ArgumentList` as one
  quoted string, not an array. Still: do not report an installer as tested on the strength
  of it compiling.
- **One VST3 destination, unconditionally: `C:\Program Files\Common Files\VST3`.** No
  per-user copy, no DAW-folder copy, no task, no `Check:`, no `IsAdminInstallMode` branch.
  A second copy is not convenience -- it is a duplicate browser entry plus two bundles that
  drift apart on the first rebuild, and it happened here: an orphan in `Documents\VST3` was
  found a whole build behind `launch/` (module `7483d04a` vs installed `b4e09344`), and has
  since been relocated -- see Housekeeping. So:
  * exactly **one** `[Files]` line for the bundle, `DestDir: {commonpf64}\Common Files\VST3\`,
    with **neither** `Check=` **nor** `Tasks=` -- a condition on that line is how a second
    destination comes back;
  * `PrivilegesRequired=admin`, and **no** `PrivilegesRequiredOverridesAllowed` line. The
    rule outranks installing without a UAC prompt, and an override is precisely what let the
    copy downgrade to a per-user folder;
  * a DAW-folder `[Tasks]` entry gets **deleted**, not left unchecked -- an option that
    exists gets ticked, and its description becomes documentation for a second copy;
  * retired destinations are **not** listed in `[UninstallDelete]`, even though the obvious
    instinct is to clean up after the old scheme. An elevated uninstaller resolves
    `{localappdata}` and `{userdocs}` to whoever is clicking, not to the account that owns
    them, and Inno says so: it warns "PrivilegesRequired is set to admin but per-user areas
    (localappdata,userdocs) are used". Keeping the lines and silencing the warning with
    `UsedUserAreasWarning=no` is the wrong trade -- reaching into those areas at all is the
    defect. Each old version's own uninstaller removes the copies it made, and a hand-placed
    stray gets deleted by hand after a hash compare. (`[InstallDelete]` would be worse still:
    it runs during an install too, before the new files land.)
  * and because the process is elevated it must not touch per-user areas -- registry hints go
    to `HKLM`, and there is no `{localappdata}` or `{userdocs}` token anywhere in the script.
    Fix the causes; never set `UsedUserAreasWarning=no`.
- **Grep smell for any `.iss` in this family** -- a hit means re-audit the destinations:
  `grep -n 'userdocs\|localappdata\|PrivilegesRequired\|IsAdminInstallMode\|Tasks:' installer/*.iss`.
  More than one `[Files]` line naming `Vst3Bundle` is the defect, whatever the conditions say.
  Siblings still have it: [skulpt/installer/SKULPT.iss](../skulpt/installer/SKULPT.iss) and
  [pulsekit/installer/PulseKit.iss](../pulsekit/installer/PulseKit.iss) both ship the
  `lowest` + per-user pattern.
- Groovebox has **no runtime asset search path at all** -- `getSpecialFolder`,
  `FileSearchPath`, `getAppDataDirectory` and `ProgramData` return zero hits in `Source/`.
  The installer writes no data asset (payload is the exe, the bundle and `README.txt`), and
  samples reach the plugin as absolute paths the user dragged in, stored in the set file. So
  moving the install folder cannot strand a side-by-side lookup, and nothing here needs a
  discovery order. If a bundled library is ever added, that path must include
  `C:\ProgramData\Skeer Audio\...` -- the payload can no longer live beside a per-user
  install -- and the ordered list must be one testable function with a check asserting the
  machine folder is in it.
- Host folders, **measured** 2026-11-15 from the scan set Live 12.3.5 actually keeps at
  `%APPDATA%\Ableton\Live 12.3.5\Preferences\PluginScanDb.txt`. It is grep-readable, but
  only with `grep -F`: paths are quoted with single backslashes, so a shell regex with
  `\\\\` matches nothing and reads as "no entries". Counts of distinct bundle paths:
  `C:\Program Files\Common Files\VST3` **742**, `%LOCALAPPDATA%\Programs\Common\VST3` **4**,
  `Documents` **0**, `Groovebox` **0** (never scanned here). So the machine folder Live
  scans for sure is `C:\Program Files\Common Files\VST3` -- the one destination is measured,
  not assumed -- and `Documents\VST3` is genuinely not scanned, which is what makes an
  orphan there harmless to a Live rescan. Do not cite a `Live 12 Suite\...\PluginManager\
  PluginScanDb.ldb` path; that file does not exist.

## Commit log: a subject is an identity, and a count is a snapshot

- **A reused subject is a fork in the log, and the hazard is retrieval, not taste.** Two commits in
  this tree carry `Cut the corner of every step the take might drop`: `4e6a7ef` really is the 4.2b
  probability mark (the notch, `StepGridComponent::isStepMutable`, the timer that had to start
  watching `getMutate()`), and `4724922` is **two lines of `AGENTS.md`** -- an export-hash refresh
  that borrowed the subject when it shipped the same build. So `git log --grep corner` returns a
  feature and a doc edit that share a name, and `git show` is the only way to tell them apart, which
  is precisely the mistake that produced a wrong "which commit is the release base" claim this
  session. Proposed replacement for `4724922`, pending the user's approval -- rewriting published
  history another lane may already have fetched is not any lane's to do unilaterally, and this
  subject was disclosed and left alone on purpose: *"Move the export-hash bullet to the build it
  now describes."* A new subject states what the diff changes, not what release it travelled with.
- **Prove ancestry with `merge-base`; use `rev-list --count` only with its direction named.**
  `git rev-list --count A..HEAD` counts A's *descendants*, so release commits appear in the set and
  a number read as "how far back the base is" inverts the claim. Both `b603107` and `4e6a7ef` are
  ancestors of this tip; the counts are 6 and 10. And a count is a snapshot: it grows the moment
  another lane commits, which is why the handover said 7 and the same command now says 10. Record
  the pair -- `b603107`, 6 back from `bcd6188` -- never the bare number.
- **A retired artefact's hash stays true, so it has to be labelled outgoing in the tracked carrier
  too.** `out/launch-pre-75fafb3/FROM.txt` records bundle `428E5DDA...`; `sha256sum` on the archived
  file is exactly that, so the record is correct and the misreading was scope -- a gitignored folder
  whose manifest's first word is "Outgoing", quoted later as a current census. Housekeeping already
  says to archive rather than delete; the other half is that a superseded number needs its
  supersession written where the next reader will find it, in the same sentence.

## Housekeeping

- Retiring code means **copying it to `D:\Projects\tools\<name>` and verifying the
  copy**, not deleting it. Done for `dist/` on 2026-11-14: the stale 2026-10-11 build is
  at `D:\Projects\tools\groovebox-dist-2026-10-11` with a `FROM.txt`, three files
  SHA-256-checked before `dist\` was removed. A second deliverable folder is how the
  wrong build gets judged -- `launch/` is the only one.
  Same rule caught the case where the instinct is wrong: a stray `Documents\VST3\Groovebox.vst3`
  is not automatically a duplicate, and duplicates are the only orphans you may delete. When
  its module hash does not equal the installed one it is an older build, and the only copy of
  it -- deleting "cleans up" nothing and destroys something. Done 2026-11-15: archived at
  `D:\Projects\tools\groovebox-documents-vst3-2026-11-14` with a `FROM.txt`, both files
  SHA-256-matched and `cmp -s` exit 0 before the original was removed. Check what else shares
  the folder first -- `SKULPT.vst3` lives in `Documents\VST3` too, and is not ours to touch.
- Scratch that must not be tracked goes in `out/` (gitignored). `out\test\` holds the
  staged installer experiment -- `Groovebox-test.iss` retargets every `DestDir` under
  `out\test\stage\` so the payload could be proven without touching the system -- and
  `out\test\NOTE.txt` records exactly how far it got and where the shell refused it. Read
  that note before trying the retarget-inside-the-repo approach again; it does not work. The
  real install does, through `Start-Process -Verb RunAs` -- see the installer bullet above.
- Per-repo conventions live in this file, not in anyone's memory.
