# Groovebox -- agent instructions

JUCE 8.0.4 C++17 sample step sequencer (16 steps x 12 channels, per-channel pitch,
ADSR, velocity, probability, strike count 1-8 and choke group, swing, late-only timing
humanize, four pattern slots per lane, master trim + peak meter). VST3 + Standalone.
Plugin manufacturer code `Eruw`, product code `Grvb`, version from
`project(... VERSION ...)` in [CMakeLists.txt](CMakeLists.txt).
It answers to three names on purpose -- `Groovebox` (product), `Earworm` (the `COMPANY_NAME` JUCE writes
into the bundle), and `Skeer Audio` (the installer publisher and the `HKLM\Software\Skeer` key) -- and that
is documented in README rather than fixed: `COMPANY_NAME` and the manufacturer code are what a host stores
against existing tracks, so changing either orphans every set that already holds this plugin. Do not
"tidy" the naming in the binary.

## Deliverables: `launch/` is the one place to build, run and ship from

```powershell
cmake --build build-new --config Release --parallel 2 --target launch   # exe + bundle -> launch/
.\installer\build-installer.ps1                                         # refreshes launch/, then packages the setup
```

`launch/` holds `Groovebox.exe`, `Groovebox.vst3/` and `Groovebox Setup <version>.exe`.
It is fully gitignored and nothing in it is authored in place.

- Verify an export by **SHA-256 against the artefact tree**, never by timestamp or size:
  `copy_if_different` leaves an old mtime on a file that exported correctly, and rebuilds
  often coincide on size. **Current export: shipped 2026-11-19 from `bef0699` at `0.2.1`,
  installed that afternoon and re-installed that evening once the payload carried a licence** --
  `Groovebox.exe` `f5abfabb...` (7,169,536 B), bundle binary
  `49dab016...`, `moduleinfo.json` `40f81ec9...` reading `"Version": "0.2.1"`, and the payload
  documents `README.txt` `54295c90...` plus `LICENSE.txt` `0d96a4ff...` -- the first three equal
  in `launch/`, in the artefact tree and in
  Program Files, the last two equal to `installer/README.txt` and to the repo `LICENSE`, which is
  in turn byte-equal to the FSF's own text, so the licence is one document in four places and not
  four paraphrases. That pair is the document check described
  below and not a build check, and no binary moved for it: the evening repackaging had
  `installer/` and a root text file as its only inputs, which is why the exe and bundle hashes
  above are the same ones the afternoon install verified. `5fae5418...` is the `README.txt` of
  that first package -- true of what went in at 15:10, superseded by the Licence section written
  into the payload document afterwards. Setup's own log closes `Installation process succeeded` with
  `Administrative install mode: Yes` and `Install mode root key: HKEY_LOCAL_MACHINE`, both retired
  per-user destinations are still empty of the bundle, and `0.2.1` reads in all four version
  places: the bundle metadata, the setup's own `FileVersion` and `ProductVersion` resources, the
  HKLM `Version` value and the uninstall entry's `DisplayVersion`. The setup is
  `Groovebox Setup 0.2.1.exe` (5,110,054 B, `2e0dce61...`); the package it replaced -- same
  binaries, no licence, 5,087,885 B and `c22b7f9c...` -- is archived in `out/setup-0.2.1-precite/`
  with a `FROM.txt`, its hash re-read from the copy rather than retyped from memory. Note what
  *cannot* prove this: grepping the setup exe for `LICENSE.txt` returns **zero hits in both the
  new and the archived package**, because `[Files]` names live in the compressed block -- so the
  positive evidence is ISCC's own `Compressing: ...installer\..\LICENSE` line and the installed
  file's hash, and a negative there is the reader's test failing, not the payload missing. The outgoing export it replaced --
  still true as a record of what shipped, no longer what is installed:
  export shipped 2026-11-18 from `be65cfd` and installed the same day:
  `Groovebox.exe` `4B8DCE27...`, bundle binary `4B4B38F8...` -- equal in `launch/`, in the
  artefact tree and in Program Files, as are `moduleinfo.json` (`5EAECB37...`, the first one to
  advertise `0.2.0`) and the payload `README.txt` (`5FAE5418...`, hash-matched against
  `installer/README.txt` rather than against a copy of itself, which is what makes that last pair
  a check on the *document* and not on the build). It replaces the `3c3f059` export
  (`Groovebox.exe A3948E47...` was `b603107`; `3c3f059` was `0309ABCD...` and bundle `98643A14...`),
  which was the installed build for two days and stays true as a record of it -- **but its loose
  bytes are gone**: `launch/` was rebuilt over them and Program Files was written over them, both by
  design, and a sweep of `out/`, `D:\Projects\tools` and both `Program Files` trees finds no file
  hashing to that bundle. It is recoverable rather than lost -- the payload sits inside the archived
  installer at `D:\Projects\tools\groovebox-setup-0.1.0-build-2026-11-16`, whose `7797B73F...` was
  re-checked against its own `FROM.txt` while writing this -- but recovery needs an extractor that is
  not installed, so the rule below is the cheap fix, not the archive.
  **Snapshot before you rebuild:** the next ship copies `launch/` to `out/launch-pre-<ship>` *before*
  running the `launch` target whenever the outgoing export has no such folder, because `launch/` and
  Program Files are the only two loose copies of a build and both are overwritten by design. The
  `launch-pre-*` archives show the pattern working -- five of them at the time of writing, the newest
  `out/launch-pre-bef0699`, which is the `be65cfd` export kept with a `FROM.txt` and its four
  files SHA-256-matched before the `launch` target ran over them; it lapsed at `3c3f059`, whose
  bytes survived only inside its setup exe.
  **It is no longer a habit.** Stage 5 of `tools/verify-build.ps1` archives `launch/` to
  `out/launch-pre-<shortsha>` with a `FROM.txt` before the `launch` target can reach it, and stage 7 writes
  `out/launch-manifest-<shortsha>` as a replayable record -- LF, no BOM, one separator, `<hash>  <path>` per
  line, so `sha256sum -c` from the repo root answers "is the deliverable the build?" in four OK lines instead
  of a person remembering to look. The first snapshot that way is `out/launch-pre-03a70e8`, whose four digests
  are the 0.2.1 export above; the folder with the `.v1fmt` suffix beside it is the same archive as the
  writer's first cut, kept to show what the separator and BOM fixes changed.
  A manifest you can only read is a screenshot of a gate -- and `Out-File -Encoding utf8` on PowerShell 5.1 is
  what writes the BOM, so these two files are written with `[IO.File]::WriteAllText` and an explicit encoder.
  `moduleinfo.json` is **not** build identity: one 1,404-byte file, `E7C65365...`, came out of four
  different builds (04:06, 12:40, 16:55 and the then-installed `3c3f059`), because it carries the version
  metadata rather than the code -- so it proves the payload is complete, never which build shipped.
  The version-bump check that used that fact has been spent twice and passed twice: the rebuilt
  bundle's metadata read `0.2.0` at `be65cfd` and reads `0.2.1` at `bef0699`, so a stale value there
  would have meant a bundle older than the `project` line it was configured from. Both files are
  1,404 bytes and their hashes differ (`5eaecb37...` against `40f81ec9...`, re-checked against the
  archived copy in `out/launch-pre-bef0699`), which is the same lesson one level down: the length is
  the schema, the bytes are the version.
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
- **Four bytes is a relink; two *directories* are thousands, because MSVC salts a `.cpp`-defined
  closure's type key with the path of the file it is compiling.** Every figure below names the pair it
  belongs to, because one phrase -- "two builds of the same sources" -- covers two different phenomena.
  *One tree, relink*: `out/wt-990e4c2` (HEAD `d8bbeb8`, dirty 0), the smoke exe the 5.3 receipt hashed
  (`7ef0e28a3fb36c80...`) against that exe deleted and rebuilt with no source touched -- the build log
  prints no compile line (`3aa8c869f169eb8c...`): **4** of 8,693,248 bytes, at `0x128`/`0x129` and
  `0x6fe4b4`/`0x6fe4b5`, stamp-zeroed EQUAL. *Two cold trees, sources differing only in
  `NEXT-STEPS.md`*: `out/wt-8e49944` (HEAD `8e49944`) against `out/wt-a942cd0` (HEAD `a942cd0`), each
  side identified in `out/planbuild/ab-4222-labelled.txt` with its worktree HEAD, exe sha and COFF stamp
  beside that tree's own newest compiled input -- both fresh, both 8,739,840 bytes -- gives smoke
  (`363ca899fec4180c` vs `ae18b104fa748c1a`) **4,222** and layout **4,784**, and the **std-only planner
  of those same two trees its documented 4** (`out/planbuild/relink-probe.txt`). Those three
  worktree directories were retired on 2026-11-18, and the exes that carry these numbers -- six for
  the cold pair, one for the relink side -- are the only copies left, at
  `D:\Projects\tools\groovebox-worktree-evidence-2026-11-18`, where `cmp -l` over the archived pair
  re-measures 4,222, 4,784 and 4 without a rebuild. Stamp-zeroing the smoke
  pair leaves it different (`f1481f572d667059` vs `37807f8026b4ac69`), so this is not the clock
  difference grown larger -- and the planner's 4 across the very same pair of directories is the arm
  that proves it: a docs-only pair *does* reproduce the relink number when nothing but the clocks is
  in play.
  **Where the 4,222 are:** .data 2,551, .text 1,660, .rdata 8, header 3 (layout: 3,103 / 1,670 / 8 / 3).
  The only section-table move is `.data` VirtualSize `0x27b34` -> `0x27b24`, and `.data` is *not* a
  shifted copy -- its 107,149-byte middle is the same length on both sides and no +-8/16/32 shift
  reconstructs it (`out/planbuild/shift-test.txt`). **2,500 of the 4,222 -- 59.2% -- sit inside
  `<lambda_32hex>` type keys**: 475 digest sites at *identical offsets* in both exes, 84 sites (42 of
  236 distinct keys) with a different value; layout 3,052 of 4,784, 102 of 493 sites.
  **Why, measured on 3 KB objects rather than argued** (`out/planbuild/lambda-path-probe.txt`): compile
  one TU twice at its own path and its four keys do not move -- 10 bytes of the object differ, none of
  them inside a digest, and they are its own timestamps; rename the file (same bytes, same length) and
  all 86 digest sites move, 2,681 of 2,989 differing bytes inside one; move it to a deeper directory and
  the keys move again and the object grows by the 4 characters the path gained. A second probe splits
  *which* lambdas are keyed: move only the included header -> 0 of 4 keys change, move only the includer
  -> 2 of 4. A closure declared in a `.cpp` therefore carries that file's path in its type key and one
  declared in a header carries nobody's -- which is the partition the exe shows: the 42 keys that moved
  are the closures declared in `.cpp` files (this suite's own plus JUCE's; an opener scan over the tree
  finds 226 candidate sites in `.cpp` against 11 in `Source/*.h`, and the exe cannot attribute a key to a
  file), and the 194 that held are header-declared, which is what lets one header lambda keep one type
  across twenty translation units. So the sentence this bullet used to carry -- "it is *not* the embedded build path, which
  appears once per object and zero times in either exe" -- was true of the *string* and wrong about the
  cause: the path is hashed into the name and then dropped, and the exe holds the difference without
  holding what produced it. There is no lever: `/d1trimfile:<root>` on the same probe gives a *third*
  pair of keys -- trimmed, not canonicalised. The residual 1,722 bytes are about 1,560 mostly isolated
  single-byte edits, 91% of them in .text, spread over the whole 6 MB rather than clustered in one TU's
  code, and three quarters of them are a value *decrease* of exactly 8 or exactly 16 (1,033 at -16 and
  285 at -8; 65 more sit at 0xF0, which is a -16 that borrowed a byte) -- the sign and magnitude address
  operands would show if their target moved 16 bytes down, which is what the `.data` VirtualSize delta
  reports for B; and two 12-byte .text runs are the same instruction
  with its two `mov` operands emitted in the opposite order, so at least some of the residual is codegen
  and not reference. Characterised, not fully attributed.
  So the sentence "anything bigger means the sources genuinely differ" is valid **within one build tree**
  -- `launch/` against its artefact tree, which is what the gate hashes -- and invalid **across two
  directories**, where it would testify that a docs-only commit changed the DSP. Across trees compare
  verdicts (check counts, the `[28]` fingerprint); within one, compare hashes -- and put the pair in the
  sentence, because 4, 4,222 and 4,784 are three claims about three different pairs, not one scale.
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
  Four false claims were caught this way, two of them about the same button: that the sound
  button's menu switches between loaded samples (it only opens a `FileChooser`;
  `SampleStore::getSampleIds()` has no UI caller), that the same button has a *preview* that is
  "a diagnostic" for the humanize law (it has never played a sound -- the audition is the lane's
  **name**, since 5.3, and `previewChannel` has exactly one UI caller), that there is no MIDI note
  input (there is, C1-B1 = notes 36-47, `Layout::firstMidiNote` in [Source/Common.h:58](Source/Common.h)),
  and that Live scans `Documents\VST3` (it does not -- see Host folders below).
- **A fit claim states both axes and names the code that decides each.** 4.3b shipped that the
  wrap "lets it go down to **1218 px** — a 1366 px laptop" and that the "editor floor
  **1218×844**, which does fit 1366×768" -- in the same paragraph that then admitted the height
  half was not paid for. The width was true (1218 <= 1366); the height was not, because
  [Source/PluginEditor.cpp](Source/PluginEditor.cpp) passed 844 straight to `setResizeLimits` as
  the editor's *minimum* while a 768 px screen has roughly 730 px of it: the bottom strip sat
  off-screen and no resize brought it back. `Tests/LayoutTest.cpp` already printed that floor as
  `floorHeight` off the editor's own constrainer, so this was one subtraction away rather than a
  measurement away -- a claim that survives checking only one axis is the dangerous shape. Both
  sentences were corrected by the shipping lane in `71c04fd`, and the defect was closed by 5.6's
  height half, which is why the copy now reads **1298 × 695** (4.5's fourth control took the
  derived width from 1218 to 1298; 695 is 5.6's and has not moved) with the two deciding
  expressions beside their own axes: `TransportBar::wrappedRowWidth + 2 * Layout::editorMargin`
  for the width and `Layout::editorFloorHeight (TransportBar::twoRowHeight, gridFloor)` for the
  height. Three
  things that rule has to keep saying out loud: the *height* floor is priced against the
  **wrapped** bar, because the published width floor is below `oneRowWidth` and the corner where
  both limits are met at once is the worst case; the word "fits" states what it fits as -- a
  window, on the roughly 730 px a 768 px screen leaves -- and what it does not fit, Live
  maximised; and no bare "fits a 1366×768 laptop" sentence is allowed anywhere, because a
  one-axis fit claim is exactly what shipped once.
- **A verified number is not a verified claim.** The Touch A/B quoted `peak 1.438488 -> 1.485904`
  straight out of the run log, so every digit was true -- and both numbers are past 0 dBFS, because
  the fixture is four lanes x all sixteen steps at chance 1.0 on the whole velocity ladder
  (`Tests/SmokeTest.cpp:8293-8299`), a load the genre table never asks for (densest row 10 of 16,
  average 3.4). So the copy reported a rise in *clipping* as if it were the cost of a control, while
  the headroom section two pages earlier says the latch fires at 0 dBFS and there is no limiter.
  When shipped copy quotes a measured level it states the fixture that produced it, whether that
  fixture is a playing state, and which trim was in place -- and the same test arm's relative ceiling
  (`<= shut * 1.25`, `Tests/SmokeTest.cpp:8444`) is an anti-renormalisation gate, not a headroom gate,
  which its comment now says, because nothing else in that arm bounds the summed take's level.
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
counts. Last green run: 640 smoke checks, 730 layout checks, 86 planner checks, 0 problems, guard OK.

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
- **A JUCE radio group cannot express "neither", so the three exclusive grid toggles are
  made exclusive by hand.** `Button::internalClickCallback` computes
  `shouldBeOn = (radioGroupId != 0 || ! lastToggleState)`: a button in a group lights and
  never goes out when you click it again. All three overlays *can* be off -- that is the
  resting state -- so `TransportBar` uses `clickingTogglesState` with no group and closes
  the other button in the handler (`closeOverlay`, TransportBar.cpp). Enforcement belongs
  there rather than in the grid: `StepGridComponent::openOverlay` already keeps one
  overlay, but a panel with two lit buttons for one overlay is a panel that lies about
  which field a drag is going to write, and only the bar can see both controls.
- **An exclusivity gate has to start from the state the rule exists to leave behind.** The
  4.5 helper that proved the three overlays cannot coexist shut *all three* buttons, then
  clicked the wanted one and counted what was lit -- and a build whose ratchet handler had
  deleted the line closing Velocity came back green on all three arms, because with nothing
  open there is nothing for the missing close to do. Open one rival first, then the wanted
  overlay on top of it, and cover every directed pair: three arms became six, and the deleted
  line now reports "bar lit 2, grid modes 1" -- the bar showing two modes for a grid editing
  one, which is the lie the rule was written against. The same arm had a second hole, in its
  *report*: the failure message recomputed the counts after the loop had run the next pair and
  cleaned up, so it printed "bar lit 1, grid modes 1" beside its own failure. Evidence a check
  quotes has to come from the iteration that broke, not from wherever the loop happened to end.
- **A claim about what is visible is a claim about contrast, and only a render can answer it.**
  The roll overlay drew its ticks in the channel's colour on a cell filled with the channel's
  colour at the velocity a fresh session uses, so the overlay whose entire job is to show a
  count showed *nothing at all* on every cell of every new session -- and nothing said so,
  because the only question any pixel check thought to ask was whether pixels moved *somewhere*.
  The fix is a recess behind the ticks; the gate is four renders that differ in exactly one
  number (same velocity, same cell, same everything), a zero-contrast band that must report
  *zero* runs rather than noise, and an arm that deletes the recess and counts the ticks that
  disappear. Read the other way: a negative control that cannot go red is not a control, and a
  colour chosen because "it is the channel's own colour" is a colour nobody measured.
- **A comment that names a control is not a control.** `TransportBar`'s comment on the shared
  reset says it puts back "the *current* overlay value", and the first cut of the third overlay
  left the button reading "100%" over a number of hits -- a reset of strikes described as a
  reset to full. Nothing caught it: the label was one of two states nobody had ever rendered,
  and the sentence that promised it predates the field it now gets wrong about. Found by reading
  the handler against the comment during review, before any gate existed for it. The rule: when
  a comment states that the UI shows something, that sentence is a gate waiting to be written --
  and a new state for an existing control is exactly when the old prose stops being true and
  keeps passing.
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
- **A name can be supplied twice, and the arm that deletes the restatement is green.**
  `juce::Button (const String& name) : Component (name), text (name)` -- so a
  `TextButton { "Generate" }` already answers to `"Generate"` and the explicit
  `generateButton.setName ("Generate")` is a statement of identity, not the mechanism that
  provides it. Arm b1 of `out/ab-14.py` comments that line out and all 698 layout checks
  pass. Two consequences, both now written where they will be read: `setName` is
  load-bearing exactly where the label is not the identity (`resetValuesButton { "100%" }`
  named `ResetValues`), and the way a control actually disappears from the panel is by not
  being *shown* -- which is why the lookup folds `isVisible ()` into the match and why arm
  b2, not b1, is what reddens the membership line (1 problem). Same rule as the range
  bullet above, one level down: before trusting an identity check, ask how many places put
  that value there, and arm the one the constructor does not fill.
- **A layout constant that moves under a fixed default changes what every session looks
  like, and only a check on the default's *state* says so.** Item 14 put a 76 px button in
  the variation band, `oneRowWidth` went 1854 -> 1936, and `editorWidth` stayed 1890 -- so
  every session opened with the band on a second line. Nothing in the tree was wrong: the
  two-line layout is legal, the floor is derived, and each control kept its own width. What
  caught it was the layout test's default-window arm asserting the bar's *line count* and
  the grid's measured 405 px, and the reason those were there at all is the 4.3b fit-claim
  audit in "Judging sound" above -- a bounds-only check would have called 1890 x 844 fine. Buying the line back
  was a choice of axis: +82 px of width (to 1972) is past a 1920 px display, so the second
  line was paid in 36 px of height instead, and the repaired default keeps every measured
  design number -- 405 px grid, 30 px rows, 285 px strip, wordmark drawn. The gate for the
  axis not taken is a counterfactual, not a literal: the same window at
  `preferredHeight - (twoRowHeight - oneRowHeight)` is asked for its row, which comes out at
  exactly `StepGridComponent::minimumRowHeight` (27 px). Note what that number is *not*: the
  bank strip does not vanish at 844 -- 27 is the smallest row that still carries it -- so the
  honest claim is "the default sat one px above the point where `hasBankStrip` refuses the
  row and the door to the pattern bank disappears", not "the door was gone". A budget with
  zero slack is a defect with a delay attached, and the check has to say which it found.
- **A filtered falsifier run that matches no arm still prints its own green.** The first
  cut of `out/ab-14.py`'s selection filter compared `label[0]` -- one character -- against
  tokens like `B5`, so `python ab-14.py B5 C1` ran zero arms, then walked the restore,
  hash-checked all three files and printed `PASSED 620 / 698 / 86`. That is the
  missing-arm-from-the-log failure in its worst shape, because the tail of the report is
  true and the body is empty. A driver that can select must print how many arms ran and
  exit nonzero when that number is zero.
- **An A/B arm that starts from the previous arm's file measures the first mutation
  twice.** `out/ab-42b.ps1` ran three breakages of the step-probability corner mark and
  all three came back with the *same* four BAD lines: each arm edited the working copy, so
  once arm one had deleted the `paint()` call that draws the mark, arms two and three were
  removing pixels from a picture that had none. Restore the recorded good file at the top
  of every arm. The tell afterwards is that the arms disagree -- five of them now kill
  4 / 1 / 2 / 2 / 1 checks -- and every check is killed by at least one.
  The same trap in its quiet form is an arm whose anchor does not match: nothing is
  mutated, the suite runs against the good build, and it prints a pass. `out/ab-45.py`
  therefore prints one of three things per arm and they are not the same claim -- `SKIP`
  (anchor not found, nothing was measured), a problem *count* (the mutation applied and
  was seen), and `BUILD FAILED` (it applied and did not compile, which is also not a
  pass). One anchor was written for the wrong indentation level and printed SKIP for a
  lap; an arm that is missing from the log is not an arm that passed.
- **`isShowing ()` is false for every control in this harness, so a name lookup folds in
  `isVisible ()`.** The layout test never puts the editor on the desktop. A first cut of the caption
  gate matched `button->isShowing ()` and reported all six narrow strip controls missing:
  `RUN EXIT=1`, `FAILED 718 checks, 6 problems`, on a tree whose `ChannelStrip.obj` was demonstrably
  newer than its `.cpp`. This is the blind-not-lenient failure, and it is expensive because the
  first hypothesis it invites is "the build is stale" -- which is exactly the thing this file says
  to check, and it checked clean. The idiom the other two lookups use is `isVisible ()`.
- **A driver that makes several edits to one file accumulates them in memory and writes once.**
  `out/ab-c2.py` applied each arm's edits as `text[rel].replace (...)` and re-stored the dict, so
  every write restarted from the pristine bytes and only the *last* anchor in an arm landed:
  `c2a-old-captions`, meant to put both octave captions back, built as `+12`-only and reported 1
  problem where the pair needs 4, and `c2e` silently lost `-12`. The tell is one member of a pair
  staying silent inside its own arm. Build a blob per file, mutate the blob, write the blobs.
- **A driver that dies mid-arm leaves the tree mutated and the newest build is the mutant, because
  the restore is downstream of the crash.** The same script raised `UnicodeEncodeError` printing a
  BAD line that quoted `«` into a cp1252 stdout -- after the wide-inset arm had been applied, with
  the good bytes still only in `out/ab-c2-good/`. Three rules came out of it: a falsifier's
  *reporting* is load-bearing code, so `sys.stdout.reconfigure (encoding="utf-8", errors="replace")`
  goes at the top; a pipeline's exit status is not the driver's, because `tee` returned 0 over the
  crash, so the verdict is read out of the log's `arms that ran: N of N` tail; and recovery is
  byte-restore plus `git diff --numstat` back to the intended shape, not a rebuild of whatever the
  working copy happens to hold.
- **A collision helper that returns its first pair is not a census, and a box of no width makes
  every pair collide.** `collidesIn` reports one clash per height, so `c2e` names `PitchDown12`
  against `PitchUp` and is *silent* on the octave twins -- which must not be read as "they differ at
  6 px". And when the name lookup lost a control, the row width read from that control went to zero
  and the check announced that two captions "painted the same 0 px at 0 x 20 px": renders of an empty
  box are trivially equal, so a missing control got credited to the caption gate as an
  indistinguishability defect. The helper refuses `width <= 0` and the discovery line owns that
  failure -- which is the arm `c2c` now reports as exactly 1 problem.
- **A sentence about what a text path *does* is a claim only a render can answer.** The caption
  gate's preamble said `drawFittedText` "elides", from the API's reputation -- and checking that
  reputation was worth two corrections at once, neither of them ever committed. The vendored header
  (`external/JUCE/modules/juce_graphics/contexts/juce_GraphicsContext.h`) names the fifth argument
  `minimumHorizontalScale`, not the missing-character width the reasoning had assumed; and the same
  header's doc paragraph *does* say that past the squash floor the text is crammed in with "an
  ellipsis at the end", so the word the check names use turns out to be defensible after all.
  What neither the recollection nor the doc can settle is which pixels survive, and that is the claim
  the gate makes: at 14 x 20 px each of `-12` and `+12` puts ink on the button (the blank-box arm is
  green for both) and the two pictures are the same 280 px. The renders also said more than the
  sentence did: at the old fixed inset the octave caption was indistinguishable from the
  *neighbouring* semitone stepper, not only from its own twin. So a platform doc is a reason to
  expect a lossy fallback and never evidence about a picture -- and the sentence that cites it is
  worth reading in the header before anyone repeats it a third time.
- **Choose the control where the defect can actually show.** The overlay half of that
  mark's test compared two renders at velocity 1.0, and a velocity bar at 1.0 fills the
  whole cell, so a mark that leaked into the overlay was painted over and the check passed
  on a build with the overlay rule deleted -- a negative control that cannot go red is
  not a control. At velocity 0.1 the bar is 3 px tall, the notch is 10, and the same
  mutation fails it. State in the comment what geometry makes the check able to bite.
- **A travel gate only travels a field that was already on the cell when the move
  happened.** `shiftChannelRow` carries four loads and four stores, and the block that has
  caught a field left behind since 4.2 writes the roll *after* the shift -- so a build that
  dropped the fourth store kept every check in the file. Setting the strike count on the
  source step before `shiftChannelRow (2, 3)`, and asking that the destination read 5 while
  the place it left reads 1, is what makes that store load-bearing: arm **J** of
  `out/ab-45b.py` deletes it and exactly the two new gates move, 2 failures out of 465.
  **K** is the same trap one layer up: the grid paints the selected slot and nothing else, so
  a shift that turns a different take of the same lane is invisible on screen *and* invisible
  to a check that only reads the slot it edited -- the gate has to read the slot the shift is
  not supposed to touch, and K's "wrong slot" mutation is what proves that line bites (12
  failures, the named carrier among them).
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
- **A leftover rectangle is not a shortfall.** `Component::removeFromTop` clamps, so reading
  `area.getHeight ()` after a row of them returns 0 whether the layout had 5 px to spare or ran
  44 px short. That is what `ChannelStrip::getSpareHeight ()` was until 5.6, and the walk's
  `shortOnes == 0` gate was built on it: it had no red state to reach. The 12 BAD lines the
  4.3b probe arm printed came from the *bounds* check, which is the one that could still see the
  clipping. The strip now reports `Layout::stripPlan (room).spare` -- the plan's own deficit,
  negative when the strip cannot be served -- and that is what kills the roomy-envelope arm on
  **12 of 12** strips at 285 px (`out/56-T.txt`). Any "how much is left" number has to be asked
  of the code that *spends* it, not of the rectangle it spends from.
- **Price every branch against the predicate that selects it.** The old strip rule was two size
  sets and a threshold: roomy rows when `room >= 219`, compact rows below. Adding each set up
  showed roomy costs **268** px and compact **228**, so roomy was chosen in [219, 268) where it
  does not fit and compact only below 219 where its own 228 does not fit either -- the compact
  arm had never worked at any strip height. Note what that does *not* look like: the branch was
  not unreachable, it was reached and then overflowed, so a check for "is the tight branch ever
  exercised" passes on it. `Layout::stripPlan` (`Source/Common.h:511`) is one budget that picks a
  stage at exactly that stage's cost -- 268 / 246 / 210 / 166, hard floor 123 -- and the gate
  asserts cost <= room for every room the editor can reach, one px under each boundary, and that
  the floor is where the absorber runs out rather than a rounded guess (`LayoutTest.cpp:653`,
  twelve checks; six mutation arms kill 28 / 10 / 7 / 2 / 1 / 4 of the suite). 5.6's height half is
  twelve more (`LayoutTest.cpp:825`) plus one per size in the walk, and its ten arms kill
  6 / 5 / 4 / 5 / 1 / 4 / 1 / 4 / 1 / 1.
- **An arm's "this size is impossible, so skip the claim" flag is read from the code that makes
  it impossible.** The layout test walks nine sizes: seven the window can take -- the default, one
  row exactly, one px narrower, the width floor at the height a session opens at, both limits met
  at once, one in between, the largest -- plus the 178 px and 169 px strip probes, kept
  permanently, and it marks the probes unreachable by comparing against
  `editor->getConstrainer ()->getMinimumHeight ()` rather than a typed 844. Typing the number
  would have made the two probe arms a permanent excuse the moment 5.6's remaining half lowered
  the floor -- the gate would keep passing while the sizes it forgave stopped being forgivable.
  It did lower it, and the arms were pulled in: they now get the full height their plan asks at
  stage 3, so what they test is the stage rather than the squeeze.
- **A clamp that never binds at a reachable size has no gate until something asks it.** The last
  of 5.6's ten height arms -- `Layout::gridGuardHeight` halved -- passed all 610 checks, because
  at every size the window can take the strip is clamped by its own plan, not by the guard; the
  guard only decides anything below the floor, and below the floor the deficit lands on the grid
  band first (a 500 px window still hands the strip its 178 px and shortens the grid to 202).
  Where it does bind -- 1832 x 399 -- the grid keeps guard minus the gap above it and the strip
  starves to 169, so the check that catches the arm asks the grid component's own height against
  that promise and prints 101 px against 110. The lesson is the same one as the row-wrap gate:
  an arm that comes back green is a question about which component the rule can actually be read
  off, and the answer is the component that ends up with the leftover.
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
- **A helper that exists twice is a gate on which one the caller used.** `SequencerEngine` clamped
  its tempo with `jlimit (20.0, 400.0, tempo)` while `Params::tempoMin/tempoMax` said the same two
  numbers one file away, so a rail edit moved the parameter and left the engine -- which is the
  *audio* -- where it was. The rails now live in `Layout` (`Common.h`), which is where `PluginEditor.cpp`
  and `GenrePatterns.h` already keep theirs, because the dependency runs `Parameters.h -> Common.h`
  and the engine deliberately includes `Common.h` and not `Parameters.h`: rails in `Params` would be
  unreachable from the engine, and a third copy would be the same defect written twice. The tempo half of
  the gate (`[37]`, twelve checks over three subjects) is four checks, because the engine's own
  `samplesPerStep` has no accessor and the claim
  is about the *audio*: the live `getParameterRange (Params::tempoId)` is read off the APVTS instead
  of being restated from `Params`; `Layout::clampedTempoBpm` is probed at 0, 19.9, 20, 137.5, 400,
  400.1 and 5000 -- both rails and one value on each side of each, which is the pair that separates
  a `jlimit` from a clamp written at either end; the processor's `getCurrentTempo()` is asked after
  a panel write of 0 and of 5000, which means a block has to run, because `displayedTempo` is only
  written inside `processBlock`; and a steps-0-and-8 pattern is *rendered*, with the second onset
  measured against the 400-rail step length, armed through a `FakePlayHead` that reports a tempo and
  no ppq -- the engine takes host tempo on one branch and host *position* on another, so this is the
  one state where the engine's clamp decides the sample grid with no help from the parameter.
  The arms, and which line each one reaches (`out/mut-D-run1.txt`, `out/mut-D-run3-hostpath.txt`):
  `Params::tempoMin = 30.0f` reddens the APVTS line and the panel-write line; a ceiling-less
  `clampedTempoBpm` reddens the probe line; removing the clamp from `beginBar` reddens the rendered
  line, alone. Removing it from `setManualTempo` reddens nothing and never can: the third fence is
  JUCE's, not the repo's, because `NormalisableRange::convertTo0to1` applies `clampTo0To1`
  unconditionally in 8.0.4 (there is no clip flag to clear), so the parameter pins every write to its
  own rails before either caller is consulted. The store-side call therefore stays what it is --
  defence-in-depth that keeps two write doors storing one representation -- with an arm that says so
  rather than a green line that implies coverage. And no interior tempo appears in the panel-write
  check: 137.5 does not survive the 0..1 round-trip bit-exact, so a mismatch there would be about the
  quantiser rather than the rail, which is why the interior value is probed on the pure function.
- **A check that names its own mechanism in its failure text is a claim about that mechanism's
  source.** The panel-write line above used to read "`writeParameter` maps a value through the range
  *without binding it*, so nothing below this catches a removed clamp". In JUCE 8.0.4 the mapping does
  bind, unconditionally, and the sentence that said otherwise is why the audio check was armed through
  `setManualTempo` -- the panel path -- where it duplicates the write-through line instead of reaching
  the engine. Seven falsifier arms ran before that was visible, and the rendered-onset line was green
  in every one of them: an unfalsified check does not announce itself, it just agrees with everything.
  When a check's text explains *why* it should be able to fail, read the code it names before
  believing the check exists, and demand of every new gate the one arm that makes it red.
  The same audit deleted a bullet from this file that described `Tests/kHumanizeFixture.h`, an
  `add_custom_command` and a pinned onset tuple: none of the three names occurs anywhere in the tree.
- **Count the sites before replacing them, and count what kind each is.** 5.1-D2 asked for "the five
  restated `/127.0f` sites" to fold into `Layout::levelFromByte`. The sweep over `Source/` found
  **three** codec sites: two decoders (`SequencerEngine.cpp`'s roll unpack and the codec's per-step
  level store in `PluginProcessor.cpp`) and the *encoder* itself. An inverse cannot absorb an encoder.
  The rest of the `/127` arithmetic in the tree is the velocity rung law (`written * 127.0`,
  `rung * (1.0 / 127.0)`) and comments, which are a different quantity wearing the same denominator.
  The gate is three lines. One direction is a bijection: `levelByte (levelFromByte (b)) == b` for all
  128 bytes, which is what makes the codec's 832-character pattern a lossless encoding rather than a
  rounding. The other direction is *not* -- `levelFromByte (levelByte (v)) == v` holds only for `v`
  already on the rung lattice, and the first cut of this check typed the readout's hundredths as if
  they were, so it went red on `1/127` against `0.01` while the engine was doing nothing wrong. The
  panel snaps a drag to twentieths (`setValueFromPointer`) and prints a whole percent, so the second
  line asks the product question instead: set one cell to each of the 21 twentieths, read the percent,
  save, load, read again, require agreement -- a differential through the real codec, with agreement
  as the standard rather than a rung this file typed. The third line is the corners, and `64/127` is
  the *engine's own* struck float, not a typed constant.
  Both directions of the codec have an arm now, and they are not the same arm. Truncating `levelByte`
  instead of rounding it reddens the rung-for-rung take, the corners and the twentieths differential
  (arm d5, 3 failures) but leaves the bijection line green -- which is why the first cut of this
  section said the lattice was gated when only one way through it was. Biasing the encoder by one
  rung (`roundToInt (level * 127.0f) + 1`) is the mutation that reaches the bijection, and it reddens
  that line first and 15 others with it (arm d9, `out/mut-D-run4-bytebias.txt`): the saved record, the
  four-byte step encoding, the level seam and the `[28]` baseline canary, `4BC3AA231178C071` against
  `F5D68A3706536EF1`. The difference is what each mutation does to a byte that started as a byte: a
  truncating encoder maps every decoded value back to the same byte, so the round trip cannot see it
  and only the value->byte->value lines can; a one-rung bias cannot hide inside the round trip because
  it moves the answer. Two checks over one law, and the plan that asked for "the bijection" got one
  direction of it.
  The rule for a plan that quotes a count: re-derive it from the tree at the moment of the edit.
- **A guard you cannot reach is still worth a tripwire, but only a tripwire.** The claim that
  `renderVoice` derefs `voice.sample->buffer` one statement behind the call-site `if (voice.active)`
  is true, and so is the opposite: `sample` is never null while `active` is set. All five
  `sample = nullptr` stores sit under an `active = false` in the same block (three of them the
  voice's own end-of-life), so a delegated audit's "this crashes" was wrong. D5 adds a `jassert`
  and a census, and the census' shape is what matters: rather than picking the instants to look at,
  `[37]` drives 40 blocks and reads `debugVoice (i)` after every one, so the pairing is gated at a
  natural voice end, a choke, a `clearChannelRow` dropped mid-run and the stop whether or not
  whoever edits this thought to name them. Two preconditions hold it up -- the pool is empty before
  the transport runs, and the peak active count inside the run is non-zero -- because a census of
  nothing makes "no active voice lacked a source" true of an engine that never started. The test is
  `sampleLength > 0` rather than `!= nullptr`: that is the observable `VoiceDebug` exposes, and it is
  the strictly stronger one -- it also refuses a voice wired to a loaded-but-empty sample, which is
  the state `renderVoice` retires on the next statement instead of reading. What it does NOT reach:
  the `tails` array. `getVoiceDebug` only
  indexes `voices`, and adding a second accessor to the engine so a hygiene check could read the
  stolen hits would widen the surface for the check rather than for the product, so tails stay a
  named blind spot and `renderVoice`'s `jassert` is the only thing watching them.
  Both halves of that blind spot are arms rather than arguments. `stopAllVoices` keeping the source
  on a voice it has already marked inactive (arm d6) leaves the whole suite green -- the census reads
  active voices, so it cannot see a source held by an inactive one, and that is the `tails` gap shown
  from the store side (`out/mut-D-run1.txt`). The mirror mutation, dropping the `active = false` while
  still clearing `sample`, does not print a failure line either: the process dies with `0xC0000005`
  and no summary (exit 3221225477, `[NO SUMMARY LINE]`), because the engine dereferences the null
  source the census exists to catch. A driver that reports that as "0 FAIL lines" is reporting a
  crash as a pass, so the runner distinguishes a timeout, an absent summary and a problem count.
- Automatability is testable single-threaded because
  `AudioProcessorParameter::setValueNotifyingHost` notifies listeners synchronously.
- JUCE 8 removed `AudioProcessorValueTreeState::setParameterNotifyingHost`; the one
  remaining way in is `apvts.getParameter (id)->setValueNotifyingHost
  (apvts.getParameterRange (id).convertTo0to1 (v))`, wrapped as
  `GrooveboxProcessor::writeParameter` ([Source/PluginProcessor.cpp](Source/PluginProcessor.cpp)).
  116 parameters (8 global + 12 x 9); the pattern is deliberately not one. The listener
  list in `allParameterIds` ([Source/PluginProcessor.cpp:25](Source/PluginProcessor.cpp))
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
- **Verify a SHA another lane names in a detached worktree, and assert the ref, not just the SHA.**
  `tools/verify-sha.ps1 -Sha <sha> -Tag <tag> -Since <prev>` checks the tree out detached,
  builds each suite **per target**, refuses any exe older than its own inputs, then runs the suites
  and `ctest` -- in that order, because CTest does not build. It is **suites-only** -- see the next
  bullet for the grep that proves it and for why the one I used here in `ccfdfdd` went false the same
  afternoon. The
  `launch/`-against-artefact-tree re-hash is the *tree-side* gate's step, `out/planbuild/gate-merged.ps1`
  -- the sentence that used to claim otherwise here was written in `aa22b44` and is the reason for the
  bullet below. It prints `NAMED BY:` deliberately: a worktree made with `--detach` leaves the commit
  named by no branch, one `git worktree prune` from unverifiable, which is how a first commit landed on
  a detached HEAD while the branch sat where it was. The script used to live in gitignored `out/` and
  is now a tracked `tools/` file (item **6.7**), with every path resolved from `$PSScriptRoot` and an
  `out/` log folder created on demand, because a typed absolute path made it a machine-local harness.
- **A grep discriminator on a word the prose also uses is not a gate -- and mine proved it the same
  afternoon.** The bullet above said "`grep -n launch` returns zero hits in its 131 lines"; I then
  added a `NOT CONFIGURED` refusal and a run banner, and the script went to 168 lines with **one**
  `launch` hit -- line 67, the banner printing "no plugin target built, launch/ untouched". The claim
  stayed true and the evidence for it went false, which is the worse combination: the next reader who
  checks the grep concludes the *sentence* is the lie. Discriminate on what only a build step can
  contain, measured at `9c872f7`+ and re-checked at the promoted path: `--target launch` **0**,
  `Groovebox.exe` **0**, `vst3` **0** hits in `tools/verify-sha.ps1`, which is now 178 lines with the
  same single `launch` hit at line 77 -- a banner cannot produce any of those three.
  Same rule as the "must not print X" gate that an exception report satisfies by quoting X: state a
  negative about a file in terms of the syntax only an action would write, never the vocabulary of the
  disclaimer.
- **A guard covers only what it measures, and a verification worktree is reused, not fresh.** The
  detached gate freshness-checks the three *test* exes it builds; the plugin artefacts sitting in the
  same `build/` tree belong to whatever commit was checked out when they were last linked, and the
  directory is reused for the next SHA -- its name is not an identity (`out/wt-990e4c2` was holding
  `d8bbeb8` on 2026-11-16). An A/B that reads them prints a disagreement that is about the reader: a
  04:06-stamped bundle compared against a 16:55 build came back "sizes differ", which looked like a
  peer's quoted hash being wrong and was in fact the superseded `b603107` export -- its two prefixes
  are recorded above, which is the only way it got identified. So a script that compares two binaries
  names each side before it diffs anything -- worktree HEAD, dirty count, exe mtime beside that tree's
  newest compiled input -- and refuses when a side is older than its own sources.
  `out/planbuild/ab-hash-53.py` does exactly that and exits 3 on a stale side; the cross-build
  four-byte rule then has to be tested on a pair that actually exists, which costs one plugin build.
- **A quantisation gate is asked on both sides of a tie, and the reference number comes from
  the keyboard rather than from this file.** [34] claims an audition *is* the note it sends, which
  makes it two renders to compare rather than a level to band. The first version asked only 0.3:
  `0.3 x 127` is 38.1, so a quantiser that truncated, copied or rounded all agreed on 38 and the
  check could not fail for the reason it was written. Half velocity is 63.5, the one ordinary value
  where floor and round disagree, and the answer it must land on is 64 because
  `MidiMessage::noteOn` rounds the same product -- so the claim is two checks, and arms e3 (floor)
  and e4 (ceil) of `out/mut53.py` kill one each and nothing else in 484.
- **A queue is gated against the door it shares, not against its own numbers.** Where [34] typed a
  number about the seam it was wrong twice. "Sixteen asks on one lane leave one voice" -- they leave
  sixteen, there is no per-lane de-dupe, and what the allocator does with sixteen hits on a lane is
  the allocator's behaviour, already gated by the keyed arm, so the rewrite asserts the two paths
  *agree* and names no count. And a splice loop written `i <= insertAt` compared the inserted onset
  against the grid list it had just displaced: a check that cannot pass when the feature works, which
  the red run reported as "5 onsets in an unclicked bar" beside a bar that had four and one insertion.
- **Two controls in one rectangle means one defect can light up a different feature's suite.**
  Hoisting the audition above the bank branch (arm l5) was expected to cost the audition section two
  checks. It cost fifteen: twelve of them the 4.4 bank click gates, because the bank's marks live
  *inside* the name column and every bank click in that section also sits in the audition region. The
  wide kill set is the evidence that the ordering rule is load-bearing rather than decorative -- and
  `StepGridComponent::auditionLaneAt` still answers for those pixels by design, which `LayoutTest` now
  states as a check of its own instead of leaving to a comment.
- **A negative control that does not compile is not a control.** Three of `out/mut53.py`'s seventeen arms
  first came back `BUILD FAILED`: a marker written as a trailing `//` inside a statement ate the `);`
  that followed it, so e2, e3 and e4 mutated the file without ever building. Use block comments for
  anything mid-expression. Then read each arm's *failure count* rather than its exit status: the driver
  asked for `^FAIL` in a suite that prints `  FAIL`, which matched nothing but the summary line
  `FAILED  484 checks, 12 failures` and reported the largest kill in the file as a one-problem arm.
- **Ask a refusal gate while the door is open.** [34]'s out-of-range check sat directly under the
  ceiling-fill loop, which by then has filled the ring, so `previewChannel (-1)` answered `false` for
  a reason that had nothing to do with the range guard -- and arm e6, range check deleted, came back
  **green**. Moved above the loop, where the only way to be refused is to be refused, the same arm
  reddens two lines: the refusal, and the ceiling itself, because with the guard gone the junk
  requests eat slots and 24 offers yield 14. That second line is the tell that a precondition and a
  queue sharing one boolean are one mechanism. The general form: every `check (! foo ())` needs the
  state in which `foo` has exactly one reason left to say no, and a green refusal arm is a question
  about where the check sits in the file before it is a question about the guard.
- **A gate whose pass condition is silence needs an arm that makes noise elsewhere.** Nothing draining
  the audition ring (e1) reddens 12 checks, but it cannot touch the ones that assert a lane the user
  did not click stays quiet or that only one voice starts -- a dead queue is silent, which is what those
  say. What discharges them is a *misroute*: e5 sends the request to the neighbour and takes 9, e7 bypasses
  the shared mute gate and takes 6. Same reason the check reads `debugVoice (i).channel` rather than a
  count: a wrong channel number is a failure line, an ambiguous silence is not. A shutdown arm proves
  the feature is load-bearing; only a misroute arm proves the gate can tell *which* thing went wrong.
- **A hit area is gated from both edges, never one.** An oversized label rect still contains every point
  the correct one does, so the inside probes stay green on `.expanded (1)` (l3, 4 kills, the border arms
  only); a shrunken one still excludes every point it should, so the outside probes stay green on
  `.reduced (1)` (l9, 2 kills, the mirror). The two arms together are what bound the rectangle, and a
  border probe written on one side only measures the side it is not asking about. At the floor the label
  column is 108 px wide, so a 1 px leak is under one per cent of the control and no "reasonable" rounding
  test would have caught it by accident.
- **A refusal gate is a pair, and a pair needs a difference.** Arm w1 of `out/mut14.py` moves the
  pre-scan in `writeSlotRows` (`Source/PluginProcessor.cpp:736`) to after the first row is stored, and came
  back GREEN: the refused ask was a default `SlotRowWrite`, every cell off, on a grid that already held every
  cell off at that step -- so the leak wrote the value that was already there and the check read its own
  fixture. The refusal rows now ask for step 0 ON, at a quarter level and two strikes, and `[35]` prints that
  disagreement as its own precondition (`Tests/SmokeTest.cpp:10863`). Same shape as the
  two-lanes-hold-different-numbers rule above, aimed at the quietest kind of control there is: one that passes
  by comparing a thing with itself. An arm that cannot bite indicts the fixture before it indicts the code.
- **A clamp that is discharged at every reachable read has no gate at the store, and saying so is the
  finding.** Arm w8 deletes the strike clamp from that same loop and reddens nothing, because the value is
  clamped again by `getStepRatchet`, again by `cellSaysAnything` and the codec, and again at
  `SequencerEngine.cpp:874`, which is the one place the number becomes sound -- and 4.5's
  `setStepRatchet (3, 7, 999)` check gates *that* one. So the seam's clamp is defence-in-depth that keeps both
  write doors storing one representation, the check's comment now says the arm exists and what it does not
  gate, and the arm stays in the set as the one that names which of four copies of a rule the harness can
  actually see. Reporting a green arm is cheaper than believing it.
- **A concurrency gate has to prove the two threads overlapped.** `[11]` (`Tests/SmokeTest.cpp:1874`) hammers
  two hundred sample loads while a render thread spins, and asserts the thread rendered during them. It can
  assert that and be false: the loads finish on one core before the scheduler starts the thread, three checks
  print `ok`, and the section measured no race at all. It was found because an unrelated arm (w4) reddened
  "the audio thread ran throughout [0 blocks]" and the only honest reading was that the section had always
  been able to say that. Now `[11]` waits, bounded, for the first block before the loads begin. Any gate that
  says "while" owes a check that both halves were awake at the same time.
- **An undo count is not a write count, and neither is a notification count.** The first cut of this seam's
  own comment said the cell-route cost "192 tree writes and 192 undo commands". Measured against a listener
  and an `UndoManager` (`[35]` part d, `Tests/SmokeTest.cpp:11169-11285`): 768 encodings of the row, **240** tree
  notifications, and **two** undo actions. `ValueTree::setProperty` does not notify when the value is
  unchanged, and `UndoManager::perform` coalesces successive `SetPropertyAction`s on one property
  (`juce_UndoManager.cpp:141`) within the transaction the gesture already opened. So the saving this seam
  buys is the codec and the host, not the history -- a materially smaller claim than the one the sentence was
  making, and the kind that only survives review until someone measures it. Print the number in the test, not
  in the comment.

- **A gate's red line can be about the gate, and its evidence field can be empty on a green run.**
  The from-nothing check that item **6.7** implies -- `git clone --no-hardlinks file://…`, `checkout
  --detach <sha>`, configure (it fetches JUCE), the three suites built per target, `ctest` after them --
  is now written, as `tools/verify-clone.ps1` -- promoted out of gitignored `out/` on 2026-11-19,
  which is the fix for the complaint this bullet's own item makes: the copy that ran first is gone
  and the doc named only that path, so for three days the from-nothing gate was a sentence in a
  published repository that no clone could execute. Its first run printed `FAILED: clone` on a clone that had
  succeeded: the `Run` helper both *printed* its status and *returned* the code, so the caller held an
  array whose first element was a sentence, and `if ($rc -ne 0)` compared that array with 0 and got the
  sentence back -- truthy. Nothing after stage 1 ran, so the verdict described the verifier. Two more of
  the same shape came out of reading the log instead of the summary: `PASSED (\d+)` matched nothing
  because both JUCE suites print `PASSED` then TWO spaces, and a field labelled "problem-count lines"
  looked for a heading no suite prints, so a green run reported an empty evidence line. The rules: a
  helper returns a value and reports on a different stream (`Write-Host`), and an evidence field is
  copied out of a real log line, never described from memory -- this one now prints the three verdict
  lines it gated on. Scope, stated the way the detached gate states it: suites and `ctest` only, no
  plugin target, `launch/` untouched. And the clone carries no `out/`, so a run that proved the sources
  could not check one of the 147 numbered pointers it inherits: **6.7** stands, and the runner's own
  location is the item's complaint demonstrated by the run written to answer it.
- **A grep-smell count in a doc is a snapshot of a script that other lanes edit.** The detached gate's
  bullet says `grep -n launch` returns zero hits in its 131 lines, and today it returns one hit in 168:
  `Write-Output "SCOPE: three test suites + ctest only -- no plugin target built, launch/ untouched."`,
  which is the *disclaimer*, so the claim it exists to prove is still true and the count that proves it
  is not. Ask the SCOPE line what it scopes -- a stated invariant checked by a line count dies the first
  time someone names the thing it disclaims in order to disclaim it.
- **A line a tool *prints* is source code that has never been compiled until the tool runs.** `tools/verify-build.ps1`
  warned "do NOT wrap it in `timeout N`" -- inside a double-quoted PowerShell string, where `` `t `` is a tab
  escape, so the shipped banner read `wrap it in <TAB>imeout N`: the backtick and the `t` went into the tab and
  the sentence about the trap lost its own subject. Same family, same file: `@("a"+$x, "b"+$y)` is ONE element, because the comma binds tighter
  than `+`, which silently collapsed two `FROM.txt` header lines into one (`Count` was 1 where it should have
  been 2). Both were invisible in the source and obvious in the log, so the rule for a harness is: read its
  own output after every edit that touches a printed string, and put the escaping in a single-quoted literal.
- **A verifier that reaches the end of its file has not therefore verified anything.** The first usable
  version of `tools/verify-build.ps1` exited **0** having run zero stages: `$RepoRoot = (Split-Path -Parent $PSScriptRoot)`
  as a *param default* raised "cannot bind ... empty string" (`$PSScriptRoot` is not populated that early), the
  error went to stderr, and the driver only looked at `$LASTEXITCODE`. Fixes, both now in the file: resolve
  paths in the **body** and refuse on a missing `CMakeLists.txt` with a loud exit 3, and end the script with an
  explicit `exit 0` whose comment says what that 0 is a claim about. An implicit fall-through exit code is the
  same defect as a suite that prints nothing and passes -- the absence of a failure is not a result.
- **A count a doc quotes about a tool's own config is re-derived from that tool, not from the prose.**
  `tools/README.md` said the citation gate sweeps "the five tracked docs" while both scripts hold a four-entry
  `DOCS` list -- and the fifth markdown file in the tree (`tools/README.md` itself) is outside the gate, which
  is worth saying rather than implying. When a sentence counts what code enumerates, print the code's list.

## Doc citations: a line number is a claim about one tree

`README.md`, `AGENTS.md`, `NEXT-STEPS.md` and `installer/README.txt` point at code as
`SequencerEngine.cpp:151-159`, and every phase that adds lines moves those numbers. A doc whose
pointers land in an unrelated comment is worse than one with no pointers, because it is trusted.
`tools/cite/` keeps them honest -- the sweep, the audit, their one shared reader and the pin list are
tracked files now (item **6.7**, shape (b): tracked, and deliberately *not* a fifth `add_test`, because
these need Python and the plugin build needs neither), and `tools/README.md` is what a fresh clone
reads first. The rules below are what each one cost to learn, and the move added two of its own: an
absent, base-less or empty `cite-pin.txt` is a **refusal** -- exit 3, with no census printed at all --
and every census line names the pin file's path, its entry count, its `# base:` and its own sha256,
because an exemption list needs a provenance exactly like a hash does.

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
- **The merge decides which tree a sweep belongs to, and sweeping the losing side is a defect.** The
  pair above is `0 to correct` *in the tree the numbers will live in*. Measured on a two-lane split with
  docs on both sides: `master` reported `to correct: 9`, and applying it would have been wrong twice. It
  is wrong because `git diff --name-only <merge-base> master -- Source\Tests\CMakeLists.txt` was empty
  while the merge-tree returned `Source`, `Tests` and `CMakeLists.txt` byte-equal to the *peer's*
  (`f903ac7d`, `5a2fc680`, `34a1a1df`) -- so the merged tree's compiled inputs are the peer's, the
  peer's already-swept numbers are the true ones there, none of master's nine stale strings survive into
  the merged doc, and a shift on this side writes numbers that are correct today and false the moment the
  merge lands. And it is wrong expensively: those nine lines merge cleanly, so editing them turns one
  conflict region into ten. Ask before sweeping -- and before quoting `to correct: 0` as owed -- which
  side's code the merged tree takes; if it takes the peer's, this side's drift is deferred, and the
  deferral is a stated state of the tree, not a green gate.
  Once the merge lands the deferral is called in, and what is owed is one-sided by construction: each
  side's prose was hand-verified against *its own* code, so the only citations that can go stale are the
  ones pointing **into a file the merge took from the other side**. Enumerate exactly those -- citations
  on lines authored by the side whose code lost, intersected with `git diff --name-only <merge-base> HEAD
  -- Source Tests installer` -- and ask of each whether the cited window still holds the text it held at
  the base. This is the question the sweep cannot answer, because for those doc lines its whole verdict is
  "edited since base, left alone". Measured at `7ab94d9`: 3 candidates, 1 real defect -- and it was two
  numbers swapped between two trees, the shape `UNPINNED miss: 0` is blind to because the citation names no
  symbol, so the audit judged it and passed it.
- **An exemption list that is merely *older* can silence a real finding, not only invent one.** The
  hazard this section records is the loud direction: absent pins re-report exempt citations as
  `UNPINNED miss` (measured 22 on a correct tree). The quiet direction is worse because the gate
  still prints 0. Measured with the same docs and the two `cite-pin.txt` files that existed *then* --
  main `out/` carried 17 range entries, the working set in `out/wt-44/out/` carried 19 (that
  worktree was retired on 2026-11-18 and the 19-entry list itself is archived beside the prefix
  `a2a9e2c2`, so the contrast stays re-runnable) -- run over
  **this** tree's docs: stale pins give `checked 20, judged 60, pinned 31, miss 0`, current pins give
  `checked 21, judged 58, pinned 31, miss 1`, and the one extra check is a genuine defect --
  NEXT-STEPS.md line 114 cites PluginProcessor.h lines 333-338 for the `parameterChanged` sentence
  while the declaration sits at `PluginProcessor.h:519` in `9c872f7`, so that pointer is wrong by 186
  lines. What the pin file changes is its *classification*: under the older set the same citation
  prints `JUDGE ... no symbol named in the clause` and contributes nothing to the gate, under the
  current one it prints `MISS` -- the windows the audit slices a paragraph by come from the pin list,
  so an old list edits the reader's question, not just its exemptions. Proven by 2x2 over a detached
  `9c872f7` worktree: both doc states x both pin sets, and only the pin axis moves the verdict.
  The same citation in lane-44's
  tree is held at PluginProcessor.h lines 602-612, correct at `lane-44`: `8e49944` repaired it, and
  master still carries the number the shared base `5d071b0` carried, so this is my side's stale
  pointer, not a merge artifact. Two consequences: the merged tree has to be audited with the
  **current** six-file set (item **22**), and `miss: 0` is only evidence when the pin file's own
  provenance is printed beside it -- an exemption list needs a ref, exactly like a hash. Re-run on the
  merged tree with the promoted tooling and `CITE_PIN` pointed at the superseded 17-entry list (kept as
  `out/cite-pin-17entry-main-3518a1e1.txt`): the audit says `miss: 0` under *both* sets, while the sweep
  goes `to correct: 0` -> `to correct: 2`, and the two it wants to rewrite are the `repaired` pointers --
  pin ranges at base lines 100 and 860, which carry by masked text onto `NEXT-STEPS.md` lines 119 and
  1122 -- back to numbers that were wrong the day they were written. So the quiet direction is not only a
  silenced finding; it is an undone fix, printed as a clean pass.
- **A rewrite has to be keyed, not incremental, or it shifts twice.** The target number is computed
  from the base's own citation and applied to the line matched by its text with every digit masked out.
  The first version added the delta it found instead, got run twice in one sitting, and moved 34
  citations a whole phase too far; the only reason that showed up is that a second dry run still
  wanted to change them. Idempotence is the evidence, so keep it as the gate, not the property.
- **A citation grammar that truncates digits corrupts pointers; one that refuses them only declines.**
  The pattern is capped at five digits with a `(?!\d)` lookahead, and until this phase it was capped at
  four -- which read a `Tests/SmokeTest.cpp` line number above 9,999 as its own first four digits and
  left the fifth outside the token. In the audit that is a confident `MISS` about a line nobody cited.
  In the sweep it is worse, because `--apply` rewrites the four digits it captured and leaves the fifth
  where it was: a correct pointer becomes a wrong one, in the same pass that prints `to correct: 0`.
  Four scripts read citations and all four had spelled the pattern out again, so the cap now lives once,
  in `cite_pins.py`, beside the reason -- and widening it again when a file passes 99,999 lines is a
  re-check of every five-digit pointer, not a one-character edit.
- **A space is a citation separator, so a file name written beside a hash becomes a pointer.** `CITE`
  accepts `[: ]` between the file token and the number, which is what lets prose read a file name, a space
  and a number as a citation at all. It read the table header's name beside a hex baseline ending 97d as a
  pointer to line 12, and the bar source file's name beside a hex ending c3, as one to line 730 -- measured,
  because 14b's
  falsification paragraph put three baseline hashes next to three file names and the audit's census grew two
  `JUDGE` lines it could never resolve. Nothing was rewritten, and only because those lines are newer than
  the pin base so the sweep declines them: a hash beside its file name on an *old* line is a digit string
  `-apply` will shift, silently lengthening a record by a line delta. So never put a hash in the same breath
  as a `Name.ext` -- write the number and then name the file ("012b31bba4c9b97d for the table"), or keep the
  backtick closed around the name so a quote mark sits where the separator would be.
- **A doc section that names its own baseline keeps that baseline's numbers, and a pin must
  *restore* the base's text rather than merely skip.** Skipping left A3 already-shifted: an exception
  that only declines to act cannot undo the action it declines. `cite-pin.txt` carries the range
  and the reason, and three reasons appear there, which are three different claims -- `pinned` (the
  section declares its tree, so moving the number falsifies the record), `unverified` (the audit names
  a symbol and cannot find it at those lines) and `repaired` (the base's number was wrong *where it was
  written*, so restoring it would put the error back -- the sweep holds those lines and prints
  `repaired pointers held, NOT restored`). 15 entries were the second kind; measured against `460b3bf`
  afterwards, **13 of them were right where they were written** and only went stale, which is the
  honest reason a map cannot fix them. The findings, not the tool's opinion, are tracked item 6.6.
- **An exception that also stops the symbol test hides the pointer from the only check that can say it
  is right.** `repaired:` is the one reason that makes a pin a claim about *this* tree rather than about
  the base, so holding it from the shift is not enough -- an unshifted and unjudged number is a number
  nobody read. The audit therefore judges held lines as well as reporting them, and to do that it needed
  a wider clause than anywhere else uses: a repaired pointer sits in prose that names its symbol one or
  two sentences *above* the citation, so held lines get a paragraph-scoped clause and print as `HELD`
  with the reason beside them. Both repairs moved from `JUDGE -- no symbol named in the clause` to a
  located symbol -- `parameterChanged` and the codec's `v2` header, each printed `HELD` with its repaired
  window beside it rather than restated here -- when the clause widened, and the run's own counts went
  `ok 28 -> 30`, `JUDGE 75 -> 73`. Narrower and the two
  hand-verified pointers in `NEXT-STEPS.md` are the two the gate never looks at. The hold is probed, not
  trusted: `out/probe-cite.py` plants the base's wrong number back into a repaired pointer and requires the
  audit to call it a `MISS` *and* the sweep to report that line as held rather than restored -- three arms,
  every one able to go red, and every doc byte-identical on the way out (`out/probe-cite-14a.txt`).
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
  `cite-pin.txt` now carries `# base: bcd6188`, both scripts print the base they used, and every label
  they print interpolates it (`edited-since-bcd6188 lines left alone`), because a run that says "HEAD" while
  reading another tree is the same defect in the report that the pin exists to prevent in the number.
  Setting the base back to `HEAD` reproduces the 13 misses: the line is what holds the gate, not luck.
  Advance a base deliberately, in the same pass that re-checks every range under it. One reader
  answers "is this line excepted" (`cite_pins.py`, including the base); two answers is how an
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
  (`PluginEditor.cpp:182`), so "has no callers" is false at any line number, and since 4.3b velocity
  is gain *and* tone, so "gain only" is prose that needs rewriting rather than renumbering.
- **A red pointer has three causes, and only one of them is a shift.** `out/repoint14a.py` refuses to
  write a number until it has said which: *drift* (the base's pointer was right and the phase moved the
  file -- the sweep's job, arithmetic, and the only class a map can fix); *already current* (a HEAD-keyed
  head-check proposes the same delta the sweep just applied, so its "answer" is the shift twice -- the
  two `Tests/SmokeTest.cpp` rows and the `PluginProcessor.cpp` one in this lane's 14a handover were
  refused for exactly that); and *repaired-and-pinned* (the number is wrong at every baseline and the pin
  holds it against the sweep, which is why its needle in that script is the base's 333-338 rather than a
  shifted form: the hold runs *before* the shift, so the swept text never existed). The second class looks
  like a bug in the sweep and the third looks like a bug in the pin list. Both are the tool working; the
  thing that fails is a fix applied without asking which class the red is in.
- **These files are CRLF on disk, and `grep -c $'\r'` cannot tell you.** `git ls-files --eol` says
  `i/lf w/crlf` for all four docs; the count returns 0 with exit 1 on a genuinely CRLF file, so it
  is not a check of anything. Any script that rewrites them opens with `newline=""` on both sides --
  a translated read-back rewrites every line of the file, not the lines it was asked to touch, and
  the diff then says 1549/1549 instead of what moved. And since `out/` is gitignored, a finding that
  lives only in a script's output does not exist: it goes into `NEXT-STEPS.md` as a numbered item.
- **A scripted edit takes its bytes before it opens the file for writing, and a planted-error probe runs
  against a copy.** This phase a probe of the citation gates restored its mutations with
  `open (p, "wb").write (open (p, "rb").read().replace(...))` -- and `"wb"` truncates before the read
  inside it runs, so both sides of the assignment were empty: `AGENTS.md` and `NEXT-STEPS.md` went to zero
  bytes, and nothing complained until the next `git status`. The recovery was `HEAD` plus the four `out/`
  scripts that had written the prose in the first place, and it is proven rather than asserted -- replayed
  through the same scripts, `git diff --numstat` came back to the same 36/4 and 144/26 the loss destroyed
  (that is the replay's own diff, not this file's: the bullets below were added after it), and
  `out/cite-audit.py` printed a byte-identical log to the pre-loss one (`out/audit-14a-e.txt` against
  `out/audit-14a-f.txt`).
  Lucky, not safe: the same content in a tree whose docs are newer than their last commit has no second
  copy at all. Read into a variable, then write; keep the pre-run bytes outside the file the script opens;
  and have the probe mutate a scratch copy, because its whole job is to be able to break something.
- **A defect list is a snapshot, so every row and its heading name the same baseline.** `NEXT-STEPS.md` headed
  its section A table "Status at `460b3bf`" and then said, in the sentence above it, that six items were
  already fixed at **HEAD** -- two baselines in one paragraph, which is how **B4** survived: four confident
  sentences about undo ("no `ValueTree::Listener` exists anywhere in `Source/`", "`getUndoManager()` has no
  callers", "undo cannot be invoked at all", the clears "fire with no confirmation") that `990e4c2` had
  discharged in full, gate included, at `Tests/SmokeTest.cpp:2956-2959` -- which asserts the *engine* takes the
  pattern back, not merely the stored string. A section that advertises finished work as open costs the reader
  a re-derivation session and then discounts the findings that are real. So: one baseline per section, stated
  in the prose as well as the header, and a superseded row keeps its original claim with the discharging commit
  named beside it -- annotate the supersession, never rewrite the finding. The same defect in its quiet form:
  row 4's "no choke concept anywhere" was true at `460b3bf` (re-checked against that blob, zero hits) and is
  false of every tree since, while the row said only "STILL OPEN".
- **A runnable instruction must resolve in a fresh clone; a receipt keeps the path it ran from.** These sit one
  grep apart and the fixes are opposite. `NEXT-STEPS.md` carries eleven mentions of `out/cite-pin.txt` and the
  retired worktree's script copies, and the plan for this round said repoint them all -- which would have been
  wrong twice. Two of them are inside past-tense sentences recording what a run printed on a specific tree,
  and the number reported there came from *that* tree's exemption file, which is the paragraph's whole point;
  rewriting the path to the promoted `tools/cite/` copy turns a receipt into a claim that cannot be reproduced
  and reads like a correction. What was owed instead is a key at the top naming the current path for each
  promoted tool, plus the archive folder for receipts whose worktree was retired. Tell the cases apart by the
  verb: "run this" is a pointer you own and must resolve, "ran this and got" is a fact about a tree that no
  longer exists.

## Installer conventions (Inno Setup 6 -- house template, with two deliberate divergences)

The siblings still install per-user and still let Setup close your DAW. This one does not,
on purpose: see the two bolded rules below.

- Payload is read straight out of `launch/` ([installer/Groovebox.iss:35](installer/Groovebox.iss)),
  so a setup can never carry binaries older than the last build; the only exception is
  `README.txt`, a tracked source in `installer/`.
- **One version, in [CMakeLists.txt](CMakeLists.txt), and the installer cannot fall back to it.**
  `build-installer.ps1` reads `project(... VERSION ...)` and passes `/DMyAppVersion`; the `.iss`
  deliberately has **no `#ifndef MyAppVersion` default** -- measured on the 0.2.0 bump: a bare
  `ISCC installer\Groovebox.iss` exits **2** at `Groovebox.iss:42` (`AppVersion={#MyAppVersion}`) and
  writes no exe, so a hand-run compile either carries the declared number or fails loudly. The
  bundle's own `moduleinfo.json` has a `"Version"` field generated from the same `project` line, so
  after a bump a rebuilt bundle must read the new value there -- it read `0.1.0` in the superseded
  `3c3f059` export, which is what makes it a version check and not an identity one, and the check has
  since been run twice more: against the `be65cfd` build it read `0.2.0`, and against the `bef0699`
  build on 2026-11-19 it reads `0.2.1` -- in the bundle's metadata, in the setup's own
  version resource and in the `Version` value under `HKLM\SOFTWARE\Skeer\Groovebox`. Three places,
  one number -- and the uninstall registry entry is the fourth, also `0.2.1` at the same reading,
  taken from `DisplayVersion` under the key the `AppId` above builds.
- **`CloseApplications=no`** ([installer/Groovebox.iss:84](installer/Groovebox.iss)).
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
  **Installing does not touch that file, and no number in it may be quoted as proof of an install.**
  Re-measured after the 2026-11-18 ship: the Groovebox line count is still zero and the file's own mtime
  is still 2026-11-15, because Live has not rescanned since the product existed on this machine. So an
  install is proven by Setup's log plus a per-file hash against `launch/` -- and "a host has loaded it"
  is a *different* claim, still un-run, that only a rescan in the owner's own session can answer. A ship
  step that expected the scan file to list the bundle was asking a cache for a fact about a program that
  has not been opened.

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
  **Measured before recommending it.** `origin/master` is `ecfe76c`, so nothing from `b603107` up is
  published -- the rewrite is local, and the exposure is two lanes' worktrees rather than GitHub. Its
  cost, though, is this file's own evidence: `4724922` has **5** descendants on `master` (`71c04fd`,
  `10fabec`, `bbd8cb1`, `fbf973c`, `bcd6188`) plus `lane-44`'s two on top, and every one of those SHAs
  changes. `bcd6188` is the base that `cite-pin.txt` is written in and `README.md`/`AGENTS.md`/
  `NEXT-STEPS.md` quote all six as the provenance of a corrected claim -- so an amend means re-anchoring
  the pin base, re-sweeping the ids the prose cites as evidence, and re-naming the SHA another lane
  verified in a detached worktree, to fix a subject that this bullet has already disambiguated.
  **Recommendation: leave the log alone, and make retrieval safe instead** -- `git log --grep` finds
  both, the bodies differ on the first line, and the durable fix for "which commit is the release base"
  was the ancestry rule below, not a new subject line. The owner's call, and the amend stays theirs to
  approve either way.
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
- **A claim about external state names the call that answers it, because another call on the same
  service can answer `null` in the same minute.** Measured at the `v0.2.1` publish:
  `gh api repos/<owner>/<repo>` returns `"license": "AGPL-3.0"` and `"visibility": "PUBLIC"`, while
  `.../community/profile` returns `license: null` **and** `readme: null` for a repo that has a README,
  and `gh repo view --json licenseInfo` returns null as well. Those are lazily-populated caches, not
  disagreements about the product -- but a future reader who checks the lazy one concludes the *sentence*
  is the lie, which is the failure the `grep -n launch` bullet above is about, one system out. So a
  published-state claim says which endpoint was asked, and quotes a second field from the same call that
  is self-evidently true (`visibility`) as the control that distinguishes a stale cache from a real no.
- **A docs commit after a verified SHA is judged by what its diff can still invalidate.** Run
  `git diff --name-only <verified-sha>..HEAD` and split the paths into **compiled inputs** (`Source/`,
  `Tests/`, `CMakeLists.txt`, `external/`) and **payload inputs** (`installer/`). An empty compiled set
  means the green run still stands and nothing is owed a rebuild; a non-empty payload set means the
  setup and the installed tree are older than the copy they carry, which costs a repackage and one UAC
  click -- not a recompile. Both halves were measured on the `3c3f059` ship: the pre-merge probe
  `git diff --name-only master 3c3f059 -- CMakeLists.txt installer/Groovebox.iss` came back empty, which
  is why that merge went straight to export, and the post-ship `AGENTS.md` commit is docs on both sides,
  so the installed build stays the verified one.

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
- **A worktree under `out/` is only scratch if nothing cites into it -- census before removing.**
  Done 2026-11-18: seven of the nine `out/wt-*` trees retired (about 6 GB, read off `df` either
  side), and `out/wt-verify` kept because `tools/README.md` names it as `tools/verify-sha.ps1`'s
  reused default. The census that made removal safe: extract every `out/...` token out of the five
  tracked docs -- 93 of them -- and test each against the main `out/`. Thirty-six did not resolve
  there, and **32 of those existed only inside `out/wt-44/out/`**, among them the `56-T.txt` that
  this file cites as the proof of the strip-plan gate and the four pre-promotion citation-tooling
  files whose sha256 prefixes `NEXT-STEPS.md` quotes for the byte-identical promotion. Deleting on
  the assumption that anything gitignored is disposable would have been discovered by the next
  reader, not by me. Bytes went to `D:\Projects\tools\groovebox-worktree-evidence-2026-11-18`
  with a `FROM.txt` naming, for each group, which side of which comparison it is, and 692 copied
  files were SHA-256-compared against their source before any `git worktree remove` ran (0
  mismatches). Two rules fell out. (1) **Archive the unreproducible and say why the rest is not
  archived**: the 580 MB `clone-cad30ae/` was left behind, because no doc cites it and the
  documented clone command rebuilds every byte of it, while the cold-tree test exes were copied,
  because with the two directories gone the 4,222 / 4,784 / 4 partition needs two fresh
  configures to re-measure -- and `cmp -l` over the archived copies returns exactly those three
  numbers, which is the archive proving itself from one folder. (2) **A hash quoted in prose is a
  promise that some byte still exists to match it**: the relink bullet's `3aa8c869f169eb8c...` is
  one side of a pair whose other side was already destroyed by the experiment that produced the
  claim, so what survives is half a gate, and `FROM.txt` says that rather than implying more.
  The `lane-44` branch outlived its worktree, as it should -- `git rev-list --count
  master..lane-44` is 0, so the removal took no commit with it.
- Scratch that must not be tracked goes in `out/` (gitignored). `out\test\` holds the
  staged installer experiment -- `Groovebox-test.iss` retargets every `DestDir` under
  `out\test\stage\` so the payload could be proven without touching the system -- and
  `out\test\NOTE.txt` records exactly how far it got and where the shell refused it. Read
  that note before trying the retarget-inside-the-repo approach again; it does not work. The
  real install does, through `Start-Process -Verb RunAs` -- see the installer bullet above.
- **One bare CR turns a doc edit into a whole-file rewrite.** This repo's blobs are LF and
  the working copies are CRLF (`core.autocrlf=true`), and git normally normalises back on add,
  so `git diff --numstat` reads as 20/8 for a README paragraph. A single CR *not* followed by
  LF makes git treat the file as `w/-text` in `git ls-files --eol` and switch the conversion
  off entirely -- the next commit then carries CRLF on every line: 1408 changed lines for a
  40-line edit, found here after ten arms' worth of python-driven docs work, and the tell is
  exactly that numstat magnitude. The cause is a replacement string that ends in `\n` being
  converted to `\r\n` by hand while the literal already carries a trailing CR. So a scripted
  edit of these docs checks `git ls-files --eol` and `git diff --numstat` before staging, and
  `re.sub(rb"\r(?!\n)", b"", …)` is the repair -- three such bytes in this repo's two big
  markdown files, all of them written this session.
- **A consolidation that already happened still has to be counted, not remembered.** 5.1-D3 asked
  to collapse "the four spellings" of the strike clamp onto `Layout::maxRatchetHits`. They were
  already one: `grep -rn -- "-ratchetByte (-.*127\|min (127" Source/` returns empty, and the live
  clamp at `SequencerEngine.cpp` is the `setStepRatchet (3, 7, 999)` check AGENTS.md already names
  as the one that becomes sound. The item closes as *already done* with that command as its receipt
  -- which is also why the census is re-run before the edit rather than quoted from the plan.
- Per-repo conventions live in this file, not in anyone's memory.
