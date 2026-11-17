# Groovebox — next steps

Produced 2026-11-14 from a seven-lane parallel audit of HEAD `460b3bf` (DSP/sound, sequencing/timing,
UI/DAW workflow, architecture/real-time safety, test & measurement harness, market/portfolio, frontier
capabilities). Every claim below was re-read in the source at HEAD; three audit findings were rejected
because they did not survive the check (see A3).

**How to use this:** pick steps by number (e.g. "do 1.1–1.4 and 2.1"). Each step is written to be
buildable on its own, with a gate that is supposed to fail before the fix. Phases 1 and 2 are done —
see **G. Run record** at the end for what each step moved and what is still red.

---

## A. Verified state — the 2026-11-14 review backlog is mostly stale

The previous critical review ranked 8 items. Six are already fixed at HEAD. Re-deriving them would have
wasted a session, so they are closed here with evidence.

| Backlog item | Status at `460b3bf` | Evidence |
|---|---|---|
| 1. `SpinLock` on the audio thread in `SampleStore` | **FIXED**, better than the proposed RCU | lock-free `std::array<std::atomic<Sample*>, 512>` with release/acquire pairs — `SampleStore.h:141`, `SampleStore.cpp:140,151`; two-thread stress test `Tests/SmokeTest.cpp:1409-1472` |
| 2. No master section | **FIXED** | `Source/MasterBus.h:59-134`, trim −24..+6 dB default −2 (`Common.h:59-61`), peak/clip latch; tests at `SmokeTest.cpp:1240-1363` |
| 3. Linear two-tap resampling | **FIXED** — redrum's kernel is already in | 24-tap / 1024-phase / 16-band windowed sinc, `Resampler.h:27-138`; aliasing test `SmokeTest.cpp:1663-1686` proves ≤ −30 dB octave-up fold-back |
| 4. Voice stealing hard-cuts, no choke groups | **STILL OPEN** | oldest voice overwritten in place with no fade, `SequencerEngine.cpp:151-159`; same class of click at sample end `:390-395`; no choke concept anywhere |
| 5. Undo contradicts its comments | **OPEN, and wronger than reported** | transactions now exist (`StepGridComponent.cpp:406,413`, `ChannelStrip.cpp:344-445`, `TransportBar.cpp:345,362`) and JUCE pops a whole drag as one step — but nothing reads the tree back; see B4 |
| 6. Standalone clock drifts early | **NOT PRESENT** — the review analysed `67cdf4a`, not HEAD | head advances only by rendered whole samples (`SequencerEngine.cpp:598,613`), the floor residual stays in the head and is re-measured each block; onset error is bounded in (−1, 0] samples, non-monotonic. The "511.5 samples early" figure is the *old* code |
| 6b. "Multi-bar test must be written first" | **ALREADY EXISTS and passes** | `SmokeTest.cpp:1519-1593`: 32 bars @120/256, 16 @300/4096, 24 @127/333, worst onset error ≤ 2 samples |
| 7. `getTailLengthSeconds()` 2.0 vs 4 s release | **FIXED** | `PluginProcessor.h:58` returns `Layout::maxReleaseSeconds` (4.0, `Common.h:39`) |
| 7b. note-off releases grid voices | **FIXED** | `releaseChannel` filters `voice.fromMidi` — `SequencerEngine.cpp:199` |
| 7c. duplicated increment maths | **FIXED** | `fireStep:337` and `triggerChannel:251` both call `trigger()`, ratio computed once at `:130-131` |
| 7d. `getWritePointer` inside the per-sample loop | **FIXED** | hoisted, `SequencerEngine.cpp:350-351` |
| 7e. `soundingChannels` cleared per chunk | **FIXED** | cleared once per block at `:493-495`, with the reason commented at `:355-359` |
| 7f. fresh `AudioFormatManager` per load | **PARTLY** | manager registered once (`SampleStore.cpp:23`), but `ChannelStrip.cpp:104,385` still builds one per strip ×12 for a wildcard string; whole-file decode still inline on the message thread (`SampleStore.cpp:70,113`) |
| 8. APVTS migration | **DONE** | 88 params (4 global + 12×7), `Parameters.h:63-117`; `writeParameter` at `PluginProcessor.cpp:133` |

Two things about this table, both learned while re-checking it for 4.4 (2026-11-16):

- **Row 4 was discharged after this table was written**, by 3.1 and 3.2, and the row above is kept as the
  `460b3bf` verdict it is. A stolen voice is now handed to a tail before being overwritten —
  `SequencerEngine::handOffToTail` with `stealFadeSamples` — and a choke group exists as a concept
  (`chokeGroup` in `SequencerEngine.h`), so the oldest voice is faded rather than cut and a lane in a group
  shuts the others. What is *not* true: 4.4 did not re-gate this. Nothing in the bank section `[32]` renders
  across a slot change with a long release, so a rotation that lands a new bar's hit on top of the previous
  bar's tail is protected by the engine's general behaviour, not by a bank check of its own.
- **Every line number in sections A, B and C is a `460b3bf` number** — the table says so in its own header, and
  the sections below inherit it. They were right where they were written; the tree has moved under them. 6.6
  lists the ones that need re-pointing, with the verified current location of each symbol, so the sweep is a
  one-pass job instead of a re-audit.
- **One of those numbers points at nothing, and not because of drift.** Row 7c's `triggerChannel:251` and B7's
  `PluginProcessor.cpp:884` name `SequencerEngine::triggerChannel`, which 5.3 deleted rather than repaired: it
  was the message-thread way into `trigger()`, and the audition now goes through a ring the render drains. A
  stale line number still lands on the right file; a stale symbol name lands on a search that returns nothing,
  which is why it gets its own line rather than riding on the bullet above.

### A3. Audit claims I checked and rejected — do not act on these

- *"The host branch re-renders the block twice"* — **true** (see B1), keep.
- *"`soundingChannels` is stored atomically but read with plain `operator[]`"* — **false**. The read is a
  proper `.load()` at `SequencerEngine.h:309`. No defect.
- *"`PresetManager.h/.cpp` exists and lane 2 mis-cited it"* — there is no `PresetManager` in the tree at
  all; `getNumPrograms()` returns 1 (`PluginProcessor.h:60-63`). The finding (no presets) is real, the file
  was invented.
- *"Add a `stateVersion` tag as a standalone task"* — rejected as standalone. Do it inside the first new
  per-step property (step 4.1), where it actually pays.
- *"Groovebox clock drift"* — rejected; see row 6 above.

---

## B. The five defects that are real, cheap, and hit you weekly

These are not polish. Each one is something a Live session already trips over, and none of them are
visible to the current test suite because the suite never installs a play head and never feeds MIDI into
a running sequencer.

**B1 — Any note played over a running pattern doubles that block.**
`processBlock` renders the whole block through the engine (`PluginProcessor.cpp:183`), then calls
`engine.processMidi` (`:187`), which **re-renders `[0, numSamples)` again** so the new note lands in the
right place (`SequencerEngine.cpp:296-320`). `renderVoices` accumulates (`outLeft[i] += …`, `:449-452`)
and advances `voice.position` (`:551`). Net effect for every voice already sounding in that block: its
samples are summed a **second time** (a level jump of up to +6 dB for one block) and its read position
advances at **2× block length** (a skip/glitch in the middle of the hit). Note-off events trigger the same
path. It is invisible to the tests because `processMidi` ignores non-note events (`:368`) and checks
`[8]`/`[9]` render with the transport stopped and the grid cleared.
*Audible as: a kick that jumps in level and stutters the moment you play a pad note over the pattern.*

**B2 — A mid-bar stop/resume or a punch-in puts the groove permanently out of phase with the bar.**
When the host is stopped, the engine sets `position` from PPQ and disarms (`SequencerEngine.cpp:525-532`),
but on resume `onsetStep` is never re-derived from the position. `distanceToNextOnsetInSteps` then returns
`barStart + onset(0) − head` **clamped at zero** (`:86-96`), so step 0 fires instantly wherever the playhead
sits — mid-bar — and the whole pattern runs permanently `head % 16` steps early relative to the grid. A
seek while playing hits the `candidate < position → +16` branch (`:98-107`) and can drop up to a bar of
onsets. The comment at `:616-619` promises "pick the position back up when it resumes"; the code does not.
*Audible as: after punching in or stopping/starting, the drums never line up with the rest of the set.*
Zero coverage today: `setPlayHead` / `PositionInfo` / `AudioPlayHead` appear **nowhere** in `Tests/`.

**B3 — The kit goes silent after enough recalls, and RAM creeps until it does.**
`applyMetadata` calls `samples.addFile()` for all 12 lanes on **every** `setStateInformation`
(`PluginProcessor.cpp:1102-1117`) with no path dedupe, and the registry is append-only — an id is
"never removed, never moved, never reused" (`SampleStore.h:49`) up to `maxSamples = 512`
(`SampleStore.h:62`), after which `addSample` returns 0 forever (`SampleStore.cpp:123-131`). Live calls
state-set on set load, preset recall, track duplicate and its own device undo: ~12 ids a pop, so after
roughly **43** of those, every sample load in that instance fails and 12 decoded buffers of RAM were
wasted each time. No test catches it — all four round-trip tests build a fresh processor
(`SmokeTest.cpp:633,675,1146,1380`).

**B4 — Undo is cosmetic and self-erasing.**
Edits push the encoded 395-char pattern string into the tree with the undo manager
(`PluginProcessor.cpp:972`), and transactions exist, so Ctrl+Z would restore the *string* — but
`GrooveboxProcessor` is only an APVTS listener, never a `ValueTree::Listener` (no such listener exists
anywhere in `Source/`), so the engine atomics are untouched; the next edit or save re-encodes the live
atomics over the undone string. Also `getUndoManager()` (`PluginProcessor.h:244`) has **no callers**: there
is no `KeyPressMapping` / `ApplicationCommandTarget` in the repo, so undo cannot be invoked at all. A
mis-drag that paints a run across a row (`StepGridComponent.cpp:599-612`) is unrecoverable, and "Clear
pattern" (`TransportBar::handleClearPatternClicked`) / "Clear row" (`ChannelStrip::handleClearRowClicked`) fire with no confirmation.

**B5 — A host-automated parameter allocates on the audio thread, and the header says it cannot.**
`parameterChanged` parses the id with two `juce::String::substring` calls (`PluginProcessor.cpp:92,97`).
Its own doc comment states *"It can therefore arrive on the audio thread, so it only ever stores atomics"*
(`PluginProcessor.h:602-612`) — JUCE notifies listeners synchronously from the parameter write, so a
Live automation ramp runs exactly that code inside `processBlock`, allocating twice per parameter per
block across up to 88 lanes. Low severity, but it is a stated invariant that is false.

Two more, small but real: **B6** `swingGrid` is an int 2–4 that the engine honours
(`SequencerEngine.cpp:60`) while the UI only ever writes 2 or 4 and, at grid 3, lights **neither** button
(`TransportBar.cpp:235-237`); **B7** was two halves and 5.3 took one of them —
`SampleStore::getSampleIds()` (`SampleStore.h:108`) still has **no UI caller**, so pointing a lane at a
sample already in the session remains unreachable and that half is 5.2's, while `previewChannel` is now
both called and safe: the grid's label column asks for it (`StepGridComponent.cpp:912`) and the request
is parked in a ring for the render to drain (`SequencerEngine.cpp:637` producing, `:666` consuming)
instead of starting a voice from whichever thread the UI happened to be on.

---

## C. Avenue map — everything found, scored

Effort in dev-hours at your pace. "Risk" means risk of breaking something that works today, chiefly the
sample-exact clock invariant that test `[12]` protects.

| # | Avenue | Impact on the thing you can hear / ship | Effort | Risk | Unblocks |
|---|---|---|---|---|---|
| **P0** | Host-playhead + MIDI-in test rig (fake `AudioPlayHead`, sparse patterns, tempo ramp) | makes B1/B2/B3 provable instead of argued; every later timing step depends on it | 6-8 h | low | B1, B2 |
| **P0** | Fix double-render (single split loop owning all block regions) | kills a +6 dB one-block jump and a mid-hit stutter | 6-10 h | med — touches the render order | — |
| **P0** | Re-derive `onsetStep` from host position on takeover/seek | the groove stays on the grid through punch-ins and seeks | 4-6 h | med — clock invariant | pattern chaining |
| **P0** | Dedupe sample loads by path+size+mtime | ends the ~43-recall silence and the RAM creep | 2-3 h | low | kits (C-UX3) |
| **P0** | Make undo real (`ValueTree::Listener` + keys + confirm on destructive) | removes "one wrong drag cost me 20 min" | 4-6 h | low-med (re-entrancy) | every future pattern feature becomes undoable |
| **P0** | Index-table parameter dispatch (no `substring`) | false invariant made true; no RT allocation | 1-2 h | low | RT tripwire test |
| **C1** | Click-safe terminations: steal fade, end-of-sample fade | no tick on fast 16ths; audible on every hat/ride | 6-10 h | low | choke groups |
| **C2** | Choke groups (3), reusing `releaseChannel` (`:192-205`) | hats and perc cut each other like hardware; market says this is table stakes and you lack it | 3-4 h on top of C1 | low | — |
| **C3** | Envelope truth (exponential decay/release, the header already claims it, `SequencerEngine.h:31-33`) + velocity→brightness law | soft hits sound soft instead of merely quiet; tails stop sounding synthetic | 3-4 h | low | — |
| **C4** | Per-hit pitch drop (varispeed ramp on `increment`, set once at `:130`) | 808-style kick bloom that Live's sample page cannot do; nothing else in the family does it | 4 h | low | — |
| **C5** | Kernel edge renormalisation + hoist `getWeights` (constant per voice) | recovers a little level over the first ~12 source samples — exactly where a kick transient lives; less CPU | 4-6 h | low | — |
| **F1** | Per-step payload **v2**: 1 byte per step, v1 still readable | the enabler: probability/ratchet/choke/humanize have **no bits** today (velocity is 1/100-quantised inside the 395-char string, `PluginProcessor.cpp:673-693`) | 6-8 h | med — must teach `shiftChannelRow` (`:334-361`, currently copies pattern+velocity only) | F2, F3, F4 |
| **F2** | Seeded per-step probability + one "mutate" dial | loops stop feeling programmed; deterministic per bar so A/B stays reproducible | 4 h | low | — |
| **F3** | Humanize (microtiming + velocity jitter) | the difference between a beatbox and a drummer | 3 h on F1 | med — an offset can put an onset behind the head (`:105`) | — |
| **F4** | Sub-step onset keystone → ratchet/rolls | fills without leaving the plugin | 6-8 h | med-high — rewrites the split loop | F3, chaining |
| **F5** | 4-slot pattern bank + auto-evolve over N bars | the sequencer becomes a song sketch | 6 h | low | — |
| **U1** | First sound on open: 12 **synthesised** default lanes (no shipped files) | right now the plugin opens silent with an empty grid | 8-12 h | med — level honesty, installer payload | — |
| **U2** | Kit workflow: multi-file drop fills lanes (`StepGridComponent.cpp`'s `filesDropped` uses `files[0]` and ignores the rest; its silence was fixed earlier, it raises the same error box the browse path does), lane menu from `getSampleIds()`, remembered folder, audition on click (B7 — the audition landed as **5.3** on 2026-11-16; the other three are still this row's) | 12 file dialogs → one gesture | 12-20 h | low | U3 |
| **U3** | Live programs/kits (`getNumPrograms()==1`) = a free preset browser in Live's browser | makes the thing shareable and recallable | 16-24 h, partly blocked by path-only refs | med | — |
| **U4** | Pattern → MIDI clip export by drag (`juce::MidiFile` already linked; `skulpt/Source/SequenceView.cpp:507` is the precedent) | gets your groove into the arranger | 4-8 h | low | — |
| **U5** | Editor floor for a 1366×768 laptop, all-12-strip layout test — **done, all three halves**: the row half (2026-11-16, pulled into 4.3b — the bar wraps to two lines below 1774 px, `TransportBar::oneRowWidth`/`wrappedRowWidth`), the strip-height half (2026-11-16, 5.6 — `Layout::stripPlan` prices four stages) and the editor-height half (2026-11-16, 5.6 — `Layout::editorPlan`, and the 5.6 entry below has the numbers and the gates). `LayoutTest.cpp` walked one size and the first strip; it now walks nine sizes × all twelve strips, two of them deliberately below the editor's own minimum, and each one asserts `0 clipped` **and** that it ran the shared rule. The published floor is **1298 × 695 px** — the width is `TransportBar::wrappedRowWidth + 2 * Layout::editorMargin`, the height is `Layout::editorFloorHeight (TransportBar::twoRowHeight, gridFloor)`, and neither is typed. 695 px fits the roughly 730 px a 768 px screen leaves for a window; it still does not fit Live maximised on that screen. The default is **1890×844** — 1832 until 4.5 put a fourth control on the bar, which took 80 px off the row rule and nothing off the grid | done | low | — |
| **V1** | `verify.ps1`: build → both test exes with `$LASTEXITCODE` checked → SHA-256 manifest of `launch/` vs artefacts | today `installer/build-installer.ps1:92-105` only `Test-Path`s 5 files; nothing hashes anything | 3-4 h | low | — |
| **V2** | RT tripwire (counting allocator armed inside `processBlock`) + click metric (max \|x[n]−x[n−1]\|) + level-matched A/B render rig | makes "sounds better" falsifiable, which is your rule | 12-16 h | med — JUCE 8.0.4 has **no** `OfflineRenderer`, BS.1770 LUFS would be hand-rolled | everything audio |
| **M1** | Positioning: free BYO-sample in-plugin sequencer is genuinely unoccupied (Triaz Player can't load custom samples; TerminalVST is one sound per instance; Hydrogen isn't a plugin; Battery/Maschine sequence nothing in-plugin) | decide what the next 20 hours are *for* | 2 h decision | n/a | — |
| **M2** | Portfolio collision: the duplicate risk is **`drumgen`/SliceForge (16 pads × 32 steps, 8 banks)**, not `redrum` (a 128-pad player with no sequencer) | avoid building the same product twice | 1 h decision | n/a | — |
| **M3** | Distribution: signing no longer buys SmartScreen trust (reputation is earned, ~$99/yr for an OV cert); a KVR page + a 90-second beat is the actual channel | shipping, not coding | S | n/a | — |

---

## D. The decision: four avenues worth your next sessions

Everything else in the map is either already done, a sound-design lane you said is not where effort
belongs yet, or a toy. Ranked by audible value per hour and by what stops being possible if skipped.

**1. Make it honest under a real host (P0 lane: rig → B1 → B2 → B3 → B4 → B5).**
This is the whole product's promise — a grid-locked, automation-friendly drum machine inside Live — and
today it double-sums the block whenever you play a note, falls out of phase after a punch-in, and can go
silent after ~43 recalls. All six fixes together are roughly a day and a half and none of them change a
single sound. It is also the only tier where the current tests are *blind*, so it is where a new test
buys the most protection.

**2. Make hits stop clicking and start choking (C1 + C2 + C5).**
The oldest defect on the list (voice stealing with no fade, `SequencerEngine.cpp:197-205`) is still open,
sample-end cuts click the same way, and choke groups are the one feature the paid competition assumes and
you do not have. Redrum already solved all three shapes — `DrumEngine.cpp:304-339`, `DrumVoice.cpp:465-492`
— so this is a port with a known answer, not an invention. Judge it with the level-matched protocol in E3.

**3. Make the loop breathe (F1 → F2, then F3/C4).**
Per-step probability and a mutate dial are 4 hours on top of a versioned byte-per-step payload, they
change what comes out of the box on every listen, and because the RNG is seeded by bar index the A/B is
reproducible and level-matched — it survives your measuring rule instead of dodging it. This is the first
*feature* tier, and it must be preceded by F1, since the current pattern encoding physically has no bits
for it.

**4. Make the first two minutes work (U1 → U2 → U3, with U4 cheap).**
The plugin opens silent with an empty grid, a sample is one file dialog per lane, `files[0]` throws away
the rest of a dropped kit, drop failures are silent, and there are no programs. This is the lane that
decides whether anyone else can use it — but it sits on the state paths that tier 1 repairs, so it goes
fourth, not first. Synthesise the default kit rather than shipping files: it keeps the installer rule
(payload comes from `launch/`, nothing authored) and avoids the licensing treadmill you cannot fund.

**Deliberately not now:** per-channel pan/filter and an FX rack (a sound-design lane with +24 params and a
77 px strip to redesign, and the clock/click work is more audible); song mode and per-channel step lengths
(`Layout::numSteps` is `constexpr` and feeds the atomics and the UI, and one `onsetStep` serves all 12
lanes — 40-60 h and it puts the invariant test `[12]` at risk); MIDI learn (88 params are already host-
automatable and Live maps better than an in-plugin list ever will); Ableton Link (the host *is* the clock —
it would be a second one); bundled sample library; ML/resynthesis (`skulpt` and `pulsekit` own that lane);
macOS/Linux builds.

---

## E. The plan

### E1 — Phase 1: build the tests that can see the bugs first

Do not fix anything in Phase 2 without its failing test written and observed red in Phase 1. This is your
own rule: a test that passes on old code proves nothing.

- **1.1** Add a fake `juce::AudioPlayHead` to `Tests/SmokeTest.cpp`: a settable struct (`isPlaying`,
  `tempo`, `ppqPosition`, `barPosition`, `signature`), installed with `setPlayHead()`, stepped by the test
  between `processBlock` calls. No message pump, no modal loop — the AGENTS.md constraint holds.
  *Done when:* it drives the host branch at all — assert the engine sees the position (there is no accessor
  today, so step 1.1 will surface how that gets reported; a `getCurrentStep()`-style read-only getter on the
  engine is the honest way).
- **1.2** **T-DOUBLE**: one lane armed on all 16 steps, sustain open, transport playing via the fake head;
  a second lane receives a note-on at sample offset 0 in a block. Assert that lane's rendered audio for the
  block is bit-identical to the same block with an empty MIDI buffer. **Expected red today** (position
  advances 2× and sums twice). ~2 h.
- **1.3** **T-PHASE**: with the fake head, stop mid-bar at `head % 16 == 7`, resume playing, and assert the
  next fired step is the one whose *swung onset* is at or after the head — not step 0. Second case: seek
  backward while playing and assert no step in the following bar is skipped. **Expected red today.** ~3 h.
- **1.4** **T-RECALL**: on one instance, `setStateInformation` twice with the same block, assert
  `getSampleIds().size()` is unchanged and the 12 lane sample ids are identical. **Expected red today.** ~1 h.
- **1.5** **T-UNDO**: `beginTransaction`; paint a run of steps; `undo()`; assert `isStepOn()` is back to
  baseline. **Expected red today.** ~1 h.
- **1.6** **T-RT**: install a counting global allocator, render 20 warm-up blocks, then assert zero
  allocations during `processBlock` while a parameter is written from another thread (this is what a host
  automation ramp does). **Expected red today** (two `substring`s). ~3 h.
- **1.7** **T-CLICK**: metric `max |x[n] − x[n−1]|` at three places — a voice stolen mid-flight, a one-shot
  reaching `sourceLength-1` at max envelope value, and attack-0 / release-max envelope corners. Baseline the
  numbers so Phase 3 has something to compare against. ~2 h.
- **1.8** One `out/` note recording baseline run counts (currently 123 smoke / 63 layout checks, 0 problems)
  and the new failing check names, so the red state is on the record before any code moves. ~0.5 h.

*Phase 1 total ≈ 13-14 h. Nothing in it changes product behaviour.*

### E2 — Phase 2: correctness (each step gated by its own test from Phase 1)

- **2.1** Make T-RECALL pass: dedupe sample loads by canonical path (+ size/mtime as a cheap freshness check)
  in `SampleStore`/`applyMetadata`, reusing the existing id. Do **not** add sample removal — that would put
  `free()` on the audio thread via the refcount at `SequencerEngine.cpp:368,457`. ~2-3 h.
- **2.2** Make T-RT pass: replace `parameterChanged`'s string parsing with a prebuilt
  index → {channel, field} table, resolved once when parameters are created (`Parameters.h:80-112`). ~1-2 h.
- **2.3** Make T-DOUBLE pass: give the block **one** renderer. Collect note events before rendering and add
  their offsets to the split points in `SequencerEngine::processBlock` (`:596-616`), so a voice is rendered
  once per region and the MIDI trigger happens at its own split. `processMidi` stops rendering entirely.
  This is the delicate one: the render order is what keeps the clock sample-exact, so re-run 1.2 *and*
  existing test `[12]` afterwards. ~6-10 h.
- **2.4** Make T-PHASE pass: on host takeover and whenever `position` jumps more than one onset, re-arm
  `onsetStep` by scanning the bar the head is in for the first onset ≥ head, instead of clamping a negative
  distance to zero (`SequencerEngine.cpp:86-107`). ~4-6 h.
- **2.5** Make T-UNDO real: `GrooveboxProcessor` becomes a `ValueTree::Listener`, decodes `"pattern"` back
  into the atomics on change, with a re-entrancy flag so `pushPatternToState` cannot loop. This also makes
  every later pattern feature undoable for free. ~4-6 h.
- **2.6** Wire undo/redo keys (`KeyPressMapping` / `ApplicationCommandTarget` — none exists in `Source/`),
  refresh the panel through the existing `onStateChanged` path, and add a confirm on "Clear pattern"
  (`TransportBar::handleClearPatternClicked`) and "Clear row" (`ChannelStrip::handleClearRowClicked`). ~3-4 h.
- **2.7** Fix B6: light the correct swing-grid button for grid 3, or make the UI write only 2/4 and stop the
  engine honouring 3 — pick one and make the comment and the code agree. ~1 h.
- **2.8** Re-check the two claims we rejected (`SequencerEngine.h:397` atomic read; no `PresetManager`) —
  nothing to do, but confirm in the commit message that they were examined. ~0.2 h.
- **2.9** Green sweep: both test exes, `--parallel 2` build, `launch/` SHA-256 against the artefact tree.

*Phase 2 total ≈ 25-32 h. No sound changes; every step has a named failing-then-passing test.*

### E3 — Phase 3: the sound of a hit (first audio-changing phase, so it needs a protocol)

**Protocol for every step here.** Render the same MIDI/state through the old and the new binary,
level-matched to within 0.1 dB at the same −2 dB trim, and report which was matched. The gates are: max
first-difference |Δx| at the steal and at sample end (a click is a broadband step, so the fix must collapse
it); integrated/peak level, unchanged; and a gated 18 kHz tone across velocity 127 vs 40 for anything that
touches the velocity law. No step here is allowed to be reported as better because it is louder.

- **3.1** Click-safe terminations: steal into a small tail pool with a 2 ms ramp, fade the last ~6 ms before
  `sourceLength-1`, keep `voice.position` sane across the hand-off. Shapes to copy, not invent:
  `redrum/.../DrumEngine.cpp:304-339` (`stealFor`), `redrum/.../DrumVoice.cpp:465-492` (`startRelease`,
  `copyTailFrom`). Gate: T-CLICK at the steal and end-of-sample. ~6-10 h.
- **3.2** Choke groups: 3 per lane, `Voice.chokeGroup`, generalise `releaseChannel` (`:192-205`) from
  channel → group; UI is a small popup on the strip. Gate: a rendered 16th hat pattern with a choke on step
  4 shows the earlier voice ramped to zero within the fade window, at matched level. ~3-4 h after 3.1.
- **3.3** Envelope truth: replace the straight-line decay/release (`SequencerEngine.h:91-92,109-110`) with
  the curve the header already claims (`:31-33`), and give velocity a brightness law as well as a gain law
  (today it is gain only, `SequencerEngine.cpp:290,138`). Gate: T-CLICK at the corners + the velocity tone
  test. ~3-4 h. **Do not touch any gain constant to make this sound better — that is the silent-compensation
  line.**
- **3.4** Kernel edge renormalisation: taps that fall off either end of the source are skipped, costing level
  over the first/last ~12 samples of a hit — exactly where a transient lives (`SequencerEngine.cpp:567-587`,
  the comment admits it). Renormalise by the weights actually used. Hoist `kernelTable.getWeights` out of the
  per-sample loop where the increment is constant (`:511-512`). Gate: level of the first 12 samples of a
  pitched-up kick, old vs new, level-matched overall. ~4-6 h.
- **3.5** Per-hit pitch drop: multiply `increment` (set once at note-on, `:131-132`) by a ramp for the hit —
  808-style kick bloom, not reachable from Live's sample page. Visible per-lane amount parameter only, never
  automatic. ~4 h.
- **3.6** Optional, only if 3.1-3.5 land cleanly: true-peak metering on the master path
  (`MasterBus.h:112-115` is sample-peak only, so summed kicks can pass the latch and clip in Live). Keep it a
  report, not a guard: the house rule is no limiter. ~3-4 h.

*Phase 3 total ≈ 20-32 h. Ship nothing from here without the A/B numbers in the commit message.*

### E4 — Phase 4: variation (the first new features, on a payload that can hold them)

- **4.1** Per-step payload **v2**: one byte per step (or a compact parallel property) instead of the 395-char
  string that currently encodes on/off and 1/100-quantised velocity (`PluginProcessor.cpp:952-972`), with v1
  still readable, **a `stateVersion` on the root tree** (there is no version today — compatibility rests on
  `hasProperty("swing")` at `:1040` and `row.length() == 16` at `:958`, and rows are silently truncated at
  `:952`), and `shiftChannelRow` (`:334-374`) taught to carry every per-step field it now moves — today it
  copies only pattern + velocity and would silently drop the new ones. Gate: old-state → v2 migration
  round-trip, plus a round-trip test that asserts each field survives a shift. ~6-8 h.
  **Done 2026-11-15, in `4628335`** (its payload half; the probability byte rode in the same commit). See the
  4.1 record in section G. The line citations in this entry are `460b3bf` numbers and are now stale — see 6.6.
- **4.2** Seeded per-step probability + a single "mutate" amount. RNG keyed on (bar, step) so offline render
  equals live playback and A/B stays reproducible. Gate: at probability 1.0 the output is bit-identical to
  today's; at 0.5 across 64 bars it is deterministic. ~4 h.
  **Done 2026-11-16, in two steps (`4628335` then `75fafb3`, which put it on the grid).** Both gate halves are
  measured, not asserted: the bit-identity at probability 1.0 is `[28]`'s fingerprint of one fixed kit, and the
  determinism at half the knob is `[29]`'s seed 7 / seed 8 fingerprints. See the 4.2 record in section G.
- **4.3** Humanize: microtiming offsets applied inside `stepOnsetInSteps` (`:57-75`) so the ≤1-sample
  invariant is preserved, plus velocity jitter. Clamp offsets so no onset can land behind the head (that is
  the `:105` whole-bar skip path) and re-run T-PHASE. ~3-4 h after 4.1.
  **Done as two steps (2026-11-16):** 4.3a shipped the timing half (`6439f0b`); the velocity half became
  **4.3b** — its own fader, its own law, its own A/B — because one number on the panel cannot be judged by
  one gate when it moves two things. See the 4.3b record at the end of section G.
- **4.4** 4-slot pattern bank per channel (four pattern strings on the tree, no file browser), with a
  bar-counter evolve option. ~6 h.
  **Done 2026-11-16.** Four slots per lane as `pattern` plus sparse `pattern1..pattern3`, one selected-slot
  pointer and one rotate period per lane, both tree state and not parameters, and a mark row in each lane's
  label column. See the 4.4 record in section G.
- **4.5** Sub-step onset keystone → ratchet/rolls. This rewrites the three onset functions into
  (step, subslot) and is the highest-risk item in the plan because it touches the split loop repaired in 2.3
  — do it only after Phase 2 is a week old and stable. ~6-8 h. Alternative that avoids the keystone:
  `Voice.startDelaySamples`, which leaves the clock untouched.
  **Done 2026-11-16.** Route decided for it on 2026-11-16: the alternative, and the keystone stays
  deferred. The entry's own
  precondition is unmet — Phase 2 landed this week — so none of the three onset functions is touched. What
  ships is the feature the keystone was only a route to: every grid cell carries a hit count of 1..8, and an
  onset fires that many voices through the `Voice.startDelay` countdown 4.3a built for humanize. The spacing
  is `gapToNextOnsetInSteps (step) / hits`, in steps, converted to samples at the one moment the engine knows
  both halves — so it follows tempo and it follows swing, and the last strike of a roll starts strictly
  before the next onset. A roll can fill the space it was given and cannot reach into the next step, which
  is the same shape of guarantee `maxHumanizeLateSteps` gives the timing knob. A cell at 1 hit makes the old
  call with the old arguments, so the section [28] fingerprint is the gate on that claim rather than an
  argument for it. See the 4.5 record in section G.

*Phase 4 total ≈ 25-30 h.*

### E5 — Phase 5: the first two minutes (workflow)

- **5.1** Default kit from **synthesis**, not shipped files: 12 named lanes with a plausible kick/snare/hat/…
  so the plugin makes sound the instant it opens (today the constructor loads nothing,
  `PluginProcessor.cpp:47-56`). Keep levels at the current trim so nothing is louder by default. Gate:
  assert a fresh processor produces a non-zero onset per lane with zero file loads. ~8-12 h.
- **5.2** Kit path: multi-file drop fills consecutive lanes (today `filesDropped` in
  `StepGridComponent.cpp` loads `files[0]` and ignores the rest; the silent-failure half of
  this item was discharged earlier -- the same path already raises the browse path's error
  box through `loadSampleIntoChannel`'s `error` out-param), remembered last folder, and a
  lane menu built from `getSampleIds()` so a lane can point at a sample already in the
  session. ~10-14 h.
- **5.3** Audition: wire `previewChannel` behind the audio-thread command queue introduced by 2.3, or delete
  it. Leaving a message-thread `triggerChannel` in the tree is a loaded gun. ~2-4 h.
  **Done 2026-11-16, and neither of the two routes the sentence offered.** The queue it named does not
  exist: 2.3 repaired the *render split loop*, not a command queue, so there was nothing to wire behind —
  and "delete it" describes a seam that was never reached: `triggerChannel`'s one caller was
  `previewChannel`, and `previewChannel` had no caller anywhere in `Source/` or `Tests/` (measured at
  `3c3f059`; the same dead path `AGENTS.md` records as README's false "sound button's preview" claim). So
  the gun was not loaded — it was in a drawer, which is a different reason not to leave it in the tree.
  What shipped is a third thing: a bounded ring of packed requests that the UI thread fills and the render
  empties at the head of its own note-event pass, so an audition enters through the same door a keyboard
  note does, `triggerChannel` is deleted with the seam in place of it, and the path finally has a caller.
  See **G**, 5.3.
- **5.4** Live programs/kits: `getNumPrograms() > 1` with names, i.e. a preset browser in Live's own
  browser. Partly blocked by path-only sample refs (`:995-1008`). ~16-24 h.
- **5.5** Pattern → MIDI clip export by drag. `juce::MidiFile` is already linked; the drag idiom exists in
  `skulpt/Source/SequenceView.cpp:507` and `redrum/Source/PluginProcessor.cpp:3502-3545`. ~4-8 h.
- **5.6** Fit a laptop. The floor moved under this step twice: `PluginEditor.cpp`'s `editorWidth`
  and `editorHeight` were **1678×844** — both the default and the minimum, since the transport
  bar is one row, 4.2's panel half filled it and 4.3's timing fader needed room in it — which does
  not fit 1366×768 with
  Live's chrome at all. Trimming heights cannot win that; the row has to wrap to two lines
  below some width, and then the bar needs a layout rule for which group moves down. That,
  plus extending `LayoutTest` past its current single size: it lays out at the editor's own
  preferred width/height only, checks one strip out of twelve (so the `tight` branch in
  `ChannelStrip::resized` is never executed), and should assert that the
  hover readout `StepGridComponent::paintValueOverlay` draws at `cell.getY() − 12` is not
  clipped on the top row. ~4-6 h.
  **Split as of 4.3b (2026-11-16), and discharged except for the height as of 5.6 the same
  day:** the third fader had made the floor worse (**1832×844**), so the *wrap rule* and the
  multi-size layout test were pulled forward into 4.3b and shipped there — one row at
  **1774 px** and wider, two lines below it, editor floor **1218×844**, which fits 1366 px of
  *width* and not 768 px of height. *(That clause was true of the floor as 4.3b shipped it; the
  height half below is what paid for it, and the published floor is **1298×695** now — 695 from
  5.6, and the width 80 px wider again since 4.5, whose fourth control moved the wrap rule from
  1774 to 1854.)*
  **The strip-height half, and the defect was worse than this entry claimed.** The 4.3b
  write-up said the `tight` branch "is broken": a probe arm at a 500 px window put **2 of
  every strip's 17 visible controls outside it**, at 236 px of strip with
  `getSpareHeight () == 0` — 12 BAD lines, `out/layout-tight-probe.log`. Measuring the rule
  properly showed `219` was not a band that went unused: it was a threshold **no strip height
  satisfies in either direction**. Roomy rows cost **268 px** of area and compact rows cost
  **228**, so roomy was picked in [219, 268) where it does not fit and compact was picked
  only *below* 219, where its own 228 still does not fit — the compact arm has never worked
  at any height, and `spare` could not say so because it was `area.getHeight()` after a
  `removeFromTop`, which clamps: a strip 44 px short leaves a rectangle of exactly 0, so the
  check built on it had no red state to reach. Fixed as one budget-chosen rule.
  `Layout::stripPlan (room)` (`Source/Common.h:511`) returns four stages priced at exactly
  their own cost — **268** full / **246** gaps in / **210** rows at their floors / **166**
  captions gone — over a hard floor of **123 px** of room, and `ChannelStrip::resized`
  (`Source/ChannelStrip.cpp:241`) asks it for every row height instead of holding a copy of
  the arithmetic; `spare` is now the plan's own deficit and may be negative, and the envelope
  row is the only absorber because it is the only row with no child components. Gated twice
  over: `LayoutTest.cpp:653` puts **twelve checks** on the rule itself — each stage's budget
  against what the plan actually consumes, one px under each boundary, the sub-166 claim
  stated as `spare -1 / envelope 43`, totality over all 278 rooms the editor can reach, the
  hard floor derived rather than typed, monotonicity, and the 236 px probe resolved — and the
  walk is now **eight** sizes rather than six: the six the window can take plus the 236 px
  and 135 px strip arms kept permanently, with reachability read from
  `editor->getConstrainer()->getMinimumHeight()` rather than a literal 844, so lowering the
  floor in the next step pulls those arms into the usability gate instead of excusing them.
  *(It did: the walk is **nine** sizes now, one of them both limits met at once, and the two
  probes hand the strip 178 px and 169 px rather than 236 px and 135.)*
  Green: **554 checks, 0 problems**. Six single-expression arms kill **28 / 10 / 7 / 2 / 1 /
  4** of them — revert to `8a27d27`, stage-0 envelope asking 66 while the budget counts 56
  (this is the one that reddens the walk: *12 of 12 short at 285 px*), stage-2 header at 22,
  absorber clamped at 4 instead of 1, top boundary `>=` made `>`, and stage 2 wired to
  `stripBareRoom` — so every gate in the new section is seen to bite by at least one arm
  (`out/56-<arm>.txt`, `out/mut56.py` restores from `out/*.good` at the top of every run).
  **The editor-height half, paid for the same day.** The strip held its 285 px down to a 585 px
  window, so the debt was never the strip: it was that `editorHeight 844` did double duty as the
  default *and* the minimum, and nobody had ever asked what the layout costs at its cheapest.
  `Layout::editorPlan (interior, barHeight, gridFloor)` (`Source/Common.h:630`) answers that in
  four stages, each giving up exactly one thing, in a fixed order: the GROOVEBOX row — 34 px that
  holds the mark, the one-line load/MIDI hint *and* the rule, all three drawn inside it — then the
  strip's slack and its rows down to the floors `stripPlan` already prices, then the strip's four
  captions. Costs **800 / 766 / 703 / 659 px** below the margins, so the published floor is stage 3
  plus 36 px of margin = **695 px**. The bar in that price is `TransportBar::twoRowHeight`, not the
  60 the default width gets, because the minimum *width* the same rule publishes — 1298, since
  4.5; 1218 when this was written — is under
  `oneRowWidth`: there is one corner where both limits are met at once, and pricing the height
  against the one-line bar would leave it 36 px short exactly there, which is gated rather than
  assumed. `GrooveboxEditor::paint` and `::resized` read one plan through `currentPlan()`, so the
  wordmark cannot be drawn by one half and left unpaid by the other. The grid is never asked to
  pay: its floor is `StepGridComponent::heightForRowAtLeast (minimumRowHeight)` = **361 px**, the
  row rule read backwards from the 27 px the bank strip needs to carry a name line without
  ellipsising it, so the marks and the name survive to the last px — and `Layout::gridGuardHeight`
  (120) is what the strip may never take from the grid on the way *below* the floor. The default is
  untouched: **1832×844 is stage 0**, strip 285, grid 405, rows 30, wordmark drawn. *(4.5 made the
  default 1890×844, and 1890 is still over `oneRowWidth`, so stage 0 is still what you get when the
  editor opens — the height plan did not move.)*
  Gated by **13 new check sites**: twelve in `LayoutTest.cpp:825` (each stage boundary built out of
  the terms it is made of — the bar's `twoRowHeight`, the strip's own budget for the stage, the
  grid's row rule inverted — rather than by calling `editorCostForStage`; one px under every
  boundary; the strip's height read back off a real strip at each stage; the published minimum
  equal to stage 3 with the wrapped bar to the pixel; the width floor forcing that worst-case bar;
  twelve real strips at the floor with 0 clipped / 0 short / 0 not stage 3 / 0 visible wordmarks /
  bar 96 / row 27 against 27 needed; the bank strip still standing in that row; the divider asked
  of pixels — longest run of the rule colour **1344 px** at the default against **0 px** at the
  floor; a 26 px row one px under the floor; the default window unchanged; monotonicity over 1200
  interiors) and one per size in the walk, that the grid never falls under the guard.
  Green: **619 checks, 0 problems** (smoke 422, planner 85). Ten single-expression arms kill
  **6 / 5 / 4 / 5 / 1 / 4 / 1 / 4 / 1 / 1**: the strip never gives anything up, the wordmark always
  paid for, a gap dropped from the chrome, the grid's own border left out of the inverse, `>=` made
  `>` at the top boundary, `resized` reserving the 34 px whatever the plan said, `paint` ignoring
  the plan, the floor priced against the one-line bar, the guard halved, and the label left visible
  with nothing to draw into it (`out/mut56h.py`, restoring from `out/*.good` at the top of every
  run; logs `out/mut56h-h<arm>.txt`, with the SHA-256 of every file that arm went red against in
  `out/mut56h-h<arm>.sha`).
  **One arm needed a second gate.** Nine of the ten reddened the suite as it stood; `gridGuardHeight
  / 2` passed all 610, because the clamp it weakens never binds at a size the window can take — the
  deficit only lands there *below* the floor, where a 500 px window still leaves the strip its full
  178 px and shortens the grid band to 202. Halving the guard is only visible where the grid has
  given all it is allowed to: at 1832×399 the strip starves to 169 px and the grid keeps guard minus
  the 10 px gap above it, so the new check asks the grid component's own height and h9 prints
  **101 px against 110**. An arm that survives the suite is the suite's defect, not the arm's.
- **5.7** Tell people the two gestures exist. Undo and redo are wired, tested and named in
  `LayoutTest` — `Ctrl/Cmd+Z`, `Ctrl+Y`, one press per *gesture* — and appear in **neither**
  README, while drag-a-sample-to-load does get a line in the header. A host that keeps its
  own Ctrl+Z is the reason the claim has to be worded carefully rather than big: check what
  Live actually forwards to a focused plugin window before printing a shortcut table, and
  say what was measured. ~1 h, mostly wording.

*Phase 5 total ≈ 45-70 h. Longest phase, least urgent; it is for when you want other people to use it.*

### E6 — Phase 6: shipping and housekeeping

- **6.1** `verify.ps1`: build → run both test exes checking `$LASTEXITCODE` directly (no pipes, no trailing
  `echo`) → SHA-256 manifest of `launch/` vs the artefact tree → file-tree equality → then the installer.
  Run from PowerShell, never Git Bash. Today `installer/build-installer.ps1:92-105` only `Test-Path`s five
  files and nothing hashes anything. ~3-4 h.
- **6.2** Correct `README.md` against HEAD: the "rounding error cannot accumulate" line is now true and worth
  keeping, but the sample-menu and MIDI-input claims need the same re-audit that caught three false claims
  last time. ~1-2 h.
- **6.3** Update `AGENTS.md` with what this audit established, so the next session does not re-derive it:
  the closed backlog, the "one renderer per block" invariant, the host-phase re-arm rule, the append-only
  registry + 512 ceiling and why dedupe-not-free is the fix, and that `Tests/` never installed a play head
  before Phase 1. ~1 h.
- **6.4** Two decisions to make in writing, not in code: (a) portfolio lane vs `drumgen`/SliceForge, which is
  where the actual duplication is (16 pads × 32 steps, 8 banks, 29 kits) — `redrum` is a 128-pad player with
  no sequencer and does not collide; (b) distribution — signing no longer buys SmartScreen trust and costs
  ~$99/yr, while a KVR product page plus a 90-second beat is the channel that has moved free plugins in 2026.
  ~1-2 h of thinking, no build.
- **6.5** Only if 5.x landed: the positioning sentence to put in the README — *"the free VST3 drum sequencer
  that stays out of your arrangement: drop your own sample on any of 12 rows, paint the pattern, export it as
  MIDI, automate everything from the host."* **Two of its four clauses are not true of this build, and the
  sentence must not ship as written:** there is no MIDI export and no drag-out at all — `MidiFile`,
  `startWholeFileDrag` and `DragAndDropContainer` return zero hits in `Source/`, which is what makes the
  README's *"nothing that carries a pattern out of the plugin"* an accurate limitation rather than a hedge —
  and *"automate everything"* overstates the 116 parameters by exactly the thing 4.4 added: the pattern grid
  and the slot a lane plays are tree state, deliberately not parameters, so a host cannot draw a slot change
  in. Either build the export, or drop both clauses; a positioning sentence is a spec like any other line in
  the README.
- **6.6** Re-point the `460b3bf`-era citations in this file, from the table below. The 4.4 docs sweep
  renumbered every citation its mechanical map could resolve — 43 of them, checked idempotent — and refused
  these, because the map had no equal block to anchor them on. Measured against the tree each entry names,
  **13 were right where they were written and only went stale later**; **5 were wrong the day they were
  typed**, and a sixth was half right (of `:290,138` one number landed on the gain line and the other landed
  on a MIDI argument) — and one of the five points past the end of a file that did not reach that far, which
  says it was written from a dirty working copy rather than from any commit. Shifting a number that was never
  right is arithmetic on a mistake, so they stay at the base's text, listed in `out/cite-pin.txt` with the reason,
  and the finding lives here — `out/` is gitignored and a tool's opinion is not a record. Re-check the
  sentence beside every one of them, not just the number: a faithful new figure buys confidence in prose that
  may itself have moved (row B5 — the *symbol* shifted, and the *"has no callers"* claim moved the other way
  and is now simply false; row 3.x — the pointer moved *and* 4.3b made "gain only" wrong).

  | where | sentence cites | right at `460b3bf` | in the tree today |
  |---|---|---|---|
  | B1 | `PluginProcessor.cpp:183` (engine renders the block) | yes | `processBlock` at `PluginProcessor.cpp:272`, the call at `:289` |
  | B1 | `SequencerEngine.cpp:296-320` (the host branch) | yes | `renderVoices` at `:1021`, the boundary walk at `:1408-1458` |
  | B1 | `SequencerEngine.cpp:449-452` (`outLeft[i] += …`) | **no — the accumulate was at 445, four lines above** | `SequencerEngine.cpp:1333` |
  | B2 | `SequencerEngine.cpp:525-532` (position from PPQ, disarm) | yes | `:1508-1528`, with `position = hostHead` at `:1555` |
  | B4 | `SampleStore.h:62` (the 512 ceiling) | yes | unchanged, `SampleStore.h:62` |
  | B4 | `SampleStore.cpp:123-131` (`addSample` returns 0) | yes | `SampleStore.cpp:151` |
  | B5 | `PluginProcessor.h:244` (`getUndoManager`, "no callers") | yes | `PluginProcessor.h:590`; **it has a caller now** — `PluginEditor.cpp:182` |
  | B6 | `PluginProcessor.cpp:92,97` (two `substring` parses) | yes | `parameterChanged` at `:113`, and the parse is **gone** — ids are looked up in `Params::laneTargets()` |
  | B7 | `TransportBar.cpp:235-237` (audition) | **no — that is the swing grid** | no audition call exists in `TransportBar.cpp` at all; since 5.3 there is exactly one, in `StepGridComponent.cpp:912` |
  | B7 | `PluginProcessor.cpp:884` (`previewChannel`) | yes | `PluginProcessor.cpp:1808` — **it has a UI caller now**, the lane-name click, and it queues instead of playing (5.3) |
  | B7 | `SampleStore.h:88` (`getSampleIds`) | yes | `SampleStore.h:108`, `SampleStore.cpp:225` |
  | F1 | `PluginProcessor.cpp:673-693` (1/100-quantised velocity) | yes, and discharged by 4.1/4.2 | the codec is `slotPropertyName`/`pushPatternToState`/`applySlotFromState` at `:1245-1526` |
  | F1 | `:334-361` (`shiftChannelRow`) | yes | `PluginProcessor.cpp:560` |
  | U5 | `ChannelStrip.cpp:197-209` (the `tight` branch) | yes | **discharged by 5.6** — that branch is gone: the row heights come from `Layout::stripPlan` (`Common.h:511`), which `ChannelStrip.cpp:241-332` asks once per strip |
  | E2 | `SequencerEngine.cpp:368,457` (`free()` via a refcount) | **no — no refcount, no `free` in that file, at any baseline** | the registry is append-only with dedupe-not-free; the sentence is describing a design that was rejected |
  | 3.x | `SequencerEngine.cpp:290,138` (velocity is gain only) | **half — 138 was the gain line, 290 a MIDI argument** | the gain is `SequencerEngine.cpp:385`, inside `trigger` at `:343` — and the sentence is out of date too, since 4.3b velocity is gain *and* tone |
  | 2.8 | `SequencerEngine.h:317` (the only read of `soundingChannels`) | **no — 317 was `double position = 0.0;`, the read was 309** | `SequencerEngine.h:682` |
  | P2 | `PluginProcessor.cpp:10` (the hand-written id list) | yes | `:24`, derived from `Params::globalIds()` since 4.2 |
  | 4.x | `SequencerEngine.cpp:858` (`dropRemaining` per sample) | **no — the file was 618 lines then, so it was written from a dirty copy** | `SequencerEngine.cpp:1340-1347`, inside `renderVoice` at `:1064` |

  ~1-2 h, no build. Two things this list does *not* cover, and the re-pointing has to:

  - The table is the subset a script can judge. The audit names a symbol in 5 of these citations and
    cannot tell whether the rest point anywhere useful, because the sentence describes an effect
    ("renders the whole block twice", "silently truncated") rather than an identifier — and that blind
    spot is a number the run itself prints -- `no symbol named, so a human must judge` -- which lists every
    one and counts them, so this entry never has to re-type a figure a paragraph moves. So sections B, C and
    the E-plan carry the same `460b3bf` numbers as these 19 rows whether or not they appear here.
  - A sweep shift preserves what the base's number pointed at — it does not repair a pointer that was
    already wrong at the base. That is why section A's baseline sentence now covers A3 as well: the 4.4 sweep had moved
    `SequencerEngine.h:309` — right at `460b3bf`, the read of `soundingChannels` — to line 373 of that file,
    which is right at neither baseline, and the pin put it back. The rule for a doc section that names its
    tree is that its numbers belong to that tree, and a drift sweep has no business touching them. A number
    quoted as a mistake is written as "line 373", never in the `file:373` form: the colon form is the grammar
    both scripts read as a pointer, so a wrong figure narrated in prose would be judged, shifted and — worst
    — re-verified as if it were one.

  Afterwards the pair of scripts is the check, in this order: `python out/cite-sweep.py . -apply`, then re-run
  it and require `to correct: 0, unresolvable: 0` on the second pass; then `python out/cite-audit.py .` and
  require `UNPINNED miss ... : 0`, which says every citation the audit can judge either points at its symbol
  or is excepted with a reason it prints — one of three: a range in `out/cite-pin.txt`, an indented table row
  that lists citations rather than pointing, or a sentence that names the commit its own number belongs to.
  The last one exists because pins are base coordinates and prose written this phase has no base line to pin;
  it is checked *after* the symbol test so it cannot cost a passing citation, it prints every exemption, and
  both sides were measured — the sentence above about `SequencerEngine.h:309` reports `BASE`, and a copy of
  that file with the commit id struck out of the same sentence reports it as `MISS`. Both gates were proven
  able to go red on this lane's own work: a hand-broken `Source/MasterBus.h:150` came back as
  `AGENTS.md:95 ... -> :150`, and the 15 lines the audit reported as MISS before the pin list existed are
  what the pin list is for.
- **6.7** Promote the citation tooling out of `out/`, or say plainly that the check does not ship. The four
  files that *are* this gate — `cite-sweep.py`, `cite-audit.py`, `cite_pins.py` and the pin list
  `cite-pin.txt` — all live in a gitignored folder, so a fresh clone inherits the rules in `AGENTS.md` with
  no way to run them, and the `# base:` line that holds the pin list is one `git clean -xdf` from gone.
  The owner already decided this question once, the other way: the planner gate went into
  `CMakeLists.txt` rather than an `out/` runner script precisely because a gitignored runner "gives nothing
  to a fresh clone" (see the CTest bullet in `AGENTS.md`). Two shapes, both about an hour and neither
  touching `Source/`: `tools/cite/` plus a fifth `add_test` whose pass expression requires `to correct: 0`
  and `UNPINNED miss: 0`, or the same four files tracked with no CTest entry — they need Python, not the
  plugin build, and a `PASS_REGULAR_EXPRESSION` on a script that is absent must fail loudly rather than
  skip. Leaving them local is defensible; drifting is not — but then the tracked prose has to say that
  the citation gate is a local harness, the way the staleness guard already does.
  **The gitignored folder does not only withhold the gate from a clone — it changes the answer the gate
  prints for a correct tree.** On 2026-11-16 the shipping lane ran the sequencer lane's own tool the way
  it is designed to be run, `python out/wt-44/out/cite-audit.py <main-tree>`, against the merged `3c3f059`
  tip and got `UNPINNED miss -- symbol named, not where it says : 22`, every one of them inside
  `NEXT-STEPS.md`'s own history. The tree was fine. `cite_pins.load (ROOT)` and `base (ROOT)` read
  `ROOT/out/cite-pin.txt`, which exists only in the lane that wrote it, so the 31 pinned citations fell
  out of the exemption set and were re-reported as misses; copying the pin file into the main `out/`
  returned the census to `20 symbol-checked / 60 judged / 31 pinned / UNPINNED miss: 0` with nothing else
  touched. That is the worse failure mode, because the number arrives looking like a finding — so any
  promotion has to make an absent `cite-pin.txt` a **refusal**, not an empty exemption list. The same
  hazard now covers a second harness: the new `AGENTS.md` bullet points at `out/planbuild/verify-sha.ps1`
  as the way to verify a peer's SHA, and a fresh clone does not have it either.
  **A third consequence, measured the hard way the same day.** A scripted probe of these gates truncated
  `AGENTS.md` and `NEXT-STEPS.md` to zero bytes, and the only copies of this phase's prose were `HEAD`
  plus four files in this folder — the section text and the three scripts that write it — and a session
  transcript. The replay is checked rather than claimed: the same scripts, `git diff --numstat` back to
  the same two pairs, and the audit's log byte-identical to the pre-loss one. A tracked `tools/cite/`
  would have had the identical failure and a one-command recovery; reconstruction recipes that live in
  `out/` only survive until someone runs `git clean -xdf`, which is the same `# base:` line's risk in
  AGENTS.md's own words, now with a cost attached to it.
- **6.7a** The from-nothing run now exists, and it changes nothing about the item. `out/verify-clone.ps1`
  clones over `file://` with `--no-hardlinks` into a folder that has never been seen, detaches the SHA,
  configures (fetching JUCE 8.0.4 into `external/JUCE`, 58 s), builds the three suites per target,
  freshness-checks each exe against its OWN inputs and then runs `ctest`. At `cad30ae`: 37 tracked files,
  top level `installer Source Tests .gitignore AGENTS.md CMakeLists.txt NEXT-STEPS.md README.md`, no
  `out/`, `external/`, `build/` or `launch/`; the only tracked scripts are `Tests/RtPathGuard.ps1` and
  `installer/build-installer.ps1`; `stale=0` on 25 / 25 / 3 inputs (`3562F62E3846`, `A790C27D3245`,
  `167A6273ED1A`); 546 smoke / 675 layout / 85 planner with 0 failures and 0 problems, `GUARD OK`, `[28]`
  back at `4BC3AA231178C071`, `100% tests passed out of 4`, exit 0. So "the tracked file set alone is
  enough to build and pass the suites" is a measurement, not an inference from a linked worktree that
  shares this repo's object DB. What the run could not do is the item: a clone inherits 147 numbered
  pointers -- 28 in `AGENTS.md`, 119 in `NEXT-STEPS.md`, none in `README.md`, which links rather than
  numbers -- and zero of the four files that check them. `launch/` was not built either, and the installed
  payload is still the `3c3f059` export: `git log --oneline 3c3f059..master -- Source Tests CMakeLists.txt`
  is empty, so Program Files matches `master`'s compiled sources, and it is behind `lane-44` by three --
  `ba7c953`, `8e49944`, `cad30ae` -- the first of which also rewords `installer/README.txt`, a payload
  input. That costs the owner one repackage and one UAC click, not a recompile of what is installed today.
  Promotion itself is still the open decision: `tools/cite/` plus a fifth `add_test`, or the four files
  tracked with no CTest entry, or the prose saying plainly that the citation gate is a local harness.

---

## F. What I would do in the next single sitting

Steps **1.1 → 1.6**, then **2.1 → 2.2**, then stop and look. That is ~20 hours, it ends with a test suite
that can see the host, six new failing-then-passing tests, two real bugs dead, and the double-render fix —
the one everyone will hear first — with its gate already red and waiting. Phase 3 is the first place a sound
changes, and it should not start until Phase 2's tests are green.

---

## G. Run record

### Phase 1 — the tests that can see the bugs (done 2026-11-15, red on the old code)

Written first, per the rule at the head of E2: nothing in Phase 2 was allowed to move without its own
failing check. Baseline was **123 smoke checks / 0 problems**; after Phase 1 it was **154 / 12**, with
`[1]`–`[15]` untouched at 142 ok, so every new failure was a new window, not a regression. Layout stayed
**63 / 0** because no Phase 1 step touched the UI. `Tests/RtPathGuard.ps1` reported **2 hits** (the two
`substring`s in `parameterChanged`).

1.6 (T-RT) could not be gated the way the plan assumed. JUCE's allocation hooks are off in this build
(`JUCE_ENABLE_ALLOCATION_HOOKS` defaults to 0, `juce_core.h:196`), and even with them on they only cover
`operator new`/`delete` — `juce::String`'s storage comes from `HeapBlock` → `mallocWrapper`/`std::free`
(`juce_HeapBlock.h:124,135,145`), so a counting `new` would have counted nothing. It became a static guard
over the function body plus a dispatch-equivalence check on all 88 ids, and the reasoning is written into
both files. Full numbers, per defect, are in `out/phase1-baseline.md` (gitignored scratch, not a deliverable).

### Phase 2 — correctness (done 2026-11-15)

**At the close of Phase 2: 157 smoke checks / 2 failures, 74 layout checks / 0 problems,
`RtPathGuard.ps1` exit 0, `launch/` hash sweep clean.** The two remaining failures were both `[20]`
T-CLICK, Phase 3's gate, and stayed red on purpose until 3.1 landed.

| Step | What moved | Evidence in the run |
|---|---|---|
| 2.1 | `SampleStore::findSampleForFile` + `applyMetadata` asks the registry before decoding | `[18]` green: 45 recalls leave the registry at 12 entries and a *new* sample still loads — the failure it replaced was "refused: This session has already loaded 512 samples" |
| 2.2 | `Params::laneTargets()` table; `parameterChanged` looks the id up instead of cutting it | guard exit 0 (was 2 hits); `[21]` all 88 ids land on the lane and field they name |
| 2.3 | one renderer: `renderBlock` splits the block at onsets **and** note offsets; `processMidi` and `currentBlock` gone | `[16]` green — was 256/256 samples differing, peak 0.7943 → 1.2478 (+3.9 dB), voice at sample 512 after one 256-sample block. `[12]` still green, so the sample-exact clock survived |
| 2.4 | `predictedHead`/`havePrediction`, `firstStepAtOrAfter`, slip tolerance of 4 samples | `[17]` green — measured: resume fires step 15 at 0.60 steps, scrub-back step 3 at 0.05, jump-forward step 10 at 0.05. Was: step 0 on the spot, and step 1 fourteen steps late |
| 2.5 | processor listens to the state tree; `applyPatternFromState` is the one decoder, guarded against its own push | `[19]` green — undo now reverts the engine, not only the stored string |
| 2.6 | Ctrl/Cmd+Z, Ctrl/Cmd+Shift+Z, Ctrl+Y on the editor; confirm before `Clear pattern` and `Clear row` | 7 new layout checks, including "with nothing to take back the key is left for the host" and "a bare letter is not swallowed" |
| 2.7 | `Layout::usableSwingGrid` — the engine and the panel answer a host-written 3 with the 1/8 pair | smoke `[4]` "a grid the bar cannot divide is answered as the pair that does"; layout lights 1/16 at 2, 1/8 at 3 and 4 |
| 2.8 | both rejections re-verified against the post-Phase-2 tree | the only read of `soundingChannels` is `.load()` at `SequencerEngine.h:317`; no `PresetManager` exists anywhere in the tree and `getNumPrograms()` still returns 1 (`PluginProcessor.h:61`) |
| 2.9 | `--parallel 2` build of `launch`, then SHA-256 of every file against the artefact tree | `Groovebox.exe A665F0AD…`, bundle binary `B4E09344…` (at `Contents\x86_64-win\Groovebox.vst3`, not the `.dll` path AGENTS.md still quotes — fixed in 6.3) |

Two things deliberately left undone here, both recorded so they are not mistaken for oversights:

- **The note-off release ramp had no check of its own.** It only existed as MIDI, and MIDI is what
  triggered the double render, so the honest gate had to wait for 2.3 — and a released voice still
  nothing asserted. It is asserted now, in `[20]`, with the rest of the terminations.
- **No ns-per-call timing proxy.** A gate that flaps on a noisy machine erodes trust in every other gate.

One procedural note, because it is how the `out/phase2-inprogress/` folder exists: 2.3/2.4 were found
half-applied in the working tree by another hand (engine header already on the merged API, `.cpp` still on
`processMidi`), so the tree did not compile and the state was snapshotted before finishing the migration.
Read `out/phase2-inprogress/FROM.txt` before trusting any file in there.

### Phase 3 — the sound of a hit (done 2026-11-15, 3.1 → 3.6)

The bar `[20]` sets, and the number it is measured against: the fixture's own slew is **0.1419**, so the
limit is **0.2129**, and the reference tone is printed on every run so a limit that drifts out of scale with
the signal is visible rather than silently passing.

**3.1 — click-safe terminations (done 2026-11-15).** A voice now ends by ramping, whichever way it ends.
Three ramps, because there were three cuts:

| Where it ended in a step | What does it now | Slew before | after |
|---|---|---|---|
| the read ran off the end of the source | the last `endFadeSeconds` (6 ms) taper to zero, in `renderVoice` | **7 of 8** source lengths breached; worst **0.7921** at sample 5033 | **0.1127** at sample 1615 on all 8 — the fixture's own slew at the trim |
| the pool was full and the oldest voice was overwritten | `handOffToTail`: the stolen hit keeps its position, increment and level in a 16-slot tail pool and fades over `stealFadeSeconds` (2 ms) | **0.4302** at sample 27562, 64/64 voices busy | **0.1127**, still 64/64 busy, so the steal really was forced |
| the transport stopped, or release was wound to nothing | `Envelope::setParameters` floors a *falling* stage at `Layout::minimumRampSeconds` | **0.7943** at sample 11026 — the whole amplitude in one sample | **0.1127**; a one-shot with decay at nothing reads **0.1125** |

The first two rows came from the same build with the two ramp lengths zeroed in `prepare` — which reproduces
the old behaviour exactly, since a zero-length window is the cut — and the third from the build that had the
tail pool but not yet the envelope floor, so all three columns are the same ruler, the same fixture and the
same −2 dB trim. `out/smoke-31-before.txt` is the first two, `out/smoke-31c.txt` the third, and
`out/smoke-31d.txt` the run with everything in.

The level side, because a fade fixes a click by taking amplitude away and a quieter machine wearing the same
pattern would satisfy a slew gate: **the peak is 0.7943 in every case, before and after** — the source at
the default trim, untouched. `[20]` now asserts that (`the end of a hit is what fades, not the hit`,
`a steal takes the slot, not the level`) and prints RMS beside it. RMS on the end-of-source renders falls by
the taper's share of the hit and no more: 0.5013 → 0.4899 on a 4000-sample source (6 ms out of 90 ms) and
0.5272 → 0.5212 on a 7619-sample one, the deficit shrinking as the hit gets longer. The steal's RMS went
*up*, 0.3140 → 0.3142, because the two milliseconds the old code threw away are now played.

What this deliberately did not touch: the **attack**. A hit starting from silence cuts nothing the listener
could hear a sample earlier, and putting a 2 ms ramp on every onset would soften the transient — a change of
sound nobody asked for. The reason is written at `Layout::minimumRampSeconds`, next to the floor it refuses.

Run now: **162 smoke checks / 0 failures**, **74 layout / 0 problems**, `RtPathGuard.ps1` exit 0. The tail
pool is a fixed `std::array` and the countdown is one divide per region rather than per sample, so nothing
here allocated on the audio thread; a stolen voice dropping its `Sample::Ptr` is still not the last
reference, because the registry owns loaded samples.

**3.2 — choke groups (done 2026-11-15).** A lane is either in no group or in one of three
(`chNChoke`, 0..3, default 0). When a hit triggers in a group, every *other* voice already sounding in that
group goes through the same `handOffToTail` the steal uses, over `Layout::chokeFadeSeconds` = 20 ms — so the
closed hat stops the open one without either of them clicking, and a lane can cut another lane's hit while
keeping its own.

The gate `[22]` went red before it went green, and it is what found the bug underneath the feature:
`allParameterIds()` in `Source/PluginProcessor.cpp:10` was a hand-written list of the seven lane ids. The
choke parameter was therefore built, offered to the host, read back on a state restore and displayed — and
nothing was listening for it. A write moved the parameter and left the atomics the audio thread reads exactly
where they were. The run says so directly: *"asked for 3, engine says 0, parameter holds 3"*, nine lanes of
them, and every one of `[22]`'s audio claims red because the engine had never been told. The list is now
built from `Params::laneTargets()`, the same table `parameterChanged` dispatches through, so a lane control
cannot be added to one without the other; `params.add` and the listener registration are now derived from the
same two lines and there is nothing left to forget.

| What `[22]` measures | Without the choke wired | With it |
|---|---|---|
| Held hit's peak after the fade window (step 4 onset + 882 samples + a block) | **0.7943** — still at full level | **0.00000** |
| Last sample above 1% of the hit, counted from the cut | **66149** (it never stopped) | **873**, against a fade of 882 |
| Tail of the run where the cutting hit is audible too | **1.5885** — two voices summing | **0.7943** — one: the old one gone, the new one kept |
| Worst sample-to-sample slew across the choke | 0.1127 | 0.1127 — the same ruler as `[20]`, so the fade is not a cut |

Two of the four claims are guards rather than gates: with nothing being choked, nothing jumps and nothing is
cut, so the slew and the "unchanged before the cut" checks pass on the broken build too. What makes the pair
honest is the level evidence. `a choke changes nothing before the hit that causes it` compares the choked and
unchoked renders **sample for sample, exactly**, for all 22050 samples up to the cut, and
`and it was still sounding until the cut` fixes the level the choked render then falls from. The scope claim
— that a group only reaches its own lanes — is the same kind of measurement: the render with the choker in
group 2 is bit-identical to the ungrouped render across the whole bar.

The cutting hit's own source is digital silence except in the run that needs to hear it, and there it is a
*different pitch* (1489 Hz against the held hit's 997 Hz): two hits of the same sine, one starting 22050
samples after the other, are exactly half a cycle apart and cancel — which reads as "the choked voice is gone"
when nothing was choked at all. That was measured, not assumed; the first version of the check produced a
0.1127 tail out of a sum of two full-level voices.

The strip needed a row for the box, and there was no spare height to take it from — the layout reported 0 px
to spare at 254 px — so `stripHeight` went 254 → **271** (`"Choke" caption 11 + gap 6 = 17`), which comes out
of the grid's slack, not the window: at the 830 px minimum the grid is still 405 px against the 400 it was
designed around, and the strip now reports 5 px to spare. *(Dated record, and the word "spare"
changed meaning under 5.6: it used to be the leftover of the layout rectangle and is now the plan's
own deficit. They are the same number wherever the old roomy set fitted — stage 0 is priced at that
set — and differed only where it did not, which is where the old reading was the lie. The strip is 285
px today, `Layout::stripDesignHeight` at `Source/Common.h:561`.)* The box is a 34 px `ComboBox` sharing the last row
with Mute (40 px), items `0 1 2 3` at ids `1 2 3 4` because JUCE keeps 0 for "no selection"; a combo box
rather than a popup menu because it shows the value without a click and `setSelectedId` fires synchronously,
which is what makes both directions testable headless.

Run now: **170 smoke checks / 0 failures**, **78 layout / 0 problems**, `RtPathGuard.ps1` exit 0. The layout
suite also had a real bug fixed: the velocity drag re-derived the grid's geometry (inset, row gap, reversed
row order) instead of asking for it, and at the new height its point landed in the gap under the last row,
where a drag does nothing. It now uses `StepGridComponent::getCellBounds`, made public for exactly that
reason.

**3.3 — envelope truth (done 2026-11-15).** Two things the code contradicted about itself, and neither was
a sound preference: the header promised a fall "the way an analogue envelope does" while both falling stages
travelled in a straight line, and velocity was documented as how hard a hit was *struck* while the engine used
it as a gain fader and nothing else.

*The falls.* `Envelope::getNextSample` now runs decay and release as one recursion — `value = target +
(value - target) * k`, `k = exp(-5 / fallSamples)` — in a single shared case, and the last sample of the fall is
snapped onto the target instead of chasing it. Five time constants across the labelled duration leaves e⁻⁵ =
0.67% (−43 dB) at the seam, which is the residual bought for keeping the curve bounded by the number the row
shows. `[9]` measures the shape through four windows a fifth of the fall apart, and both columns of the table
below are **rendered numbers**: `out/smoke-33-linear.txt` is a build with only the two fall cases put back the
way they were, everything else from 3.3 still in.

| What `[9]` reads, on a 0.75 s decay to no sustain | Straight line | Curve |
|---|---|---|
| Level at a tenth, three, five and seven tenths of the travel | 0.45585, 0.35478, 0.25378, 0.15293 | 0.31642, 0.11640, 0.04282, 0.01575 |
| …so the share of itself each fifth gives up | 0.778, 0.715, 0.603 | **0.368, 0.368, 0.368** |
| Widest ratio over narrowest | 1.292 | 1.000 (limit 1.1) |
| Seven tenths in, as a share of one tenth in | 0.336 | 0.050 (limit 0.15) |
| Checks red | **6 of 6** — three per fall, decay and release | 0 |

The release reads the same three ratios from its own windows (0.31247, 0.11495, 0.04229, 0.01556), which is the
point of the shared case: it is one curve with two targets, not a decay that happens to look exponential and a
release that does not. The band's top edge of 0.47 was put between the two shapes it has to tell apart — 0.368
and 0.603 — rather than around the one being tested, and the equality limit of 1.1 sits between 1.000 and 1.29
for the same reason. Writing the record caught two mistakes in the check itself: it printed what a straight
line "would" measure as 0.778, 0.571, 0.417 (measured: 0.778, 0.715, 0.603) and its comment said the ratios
*grow* along a straight line (they shrink — the level being divided by keeps falling). Both are the measured
values now; a gate that misdescribes the thing it distinguishes is not a gate.

*The velocity law.* `trigger` derives a per-voice one-pole from the hit's velocity
(`brightnessMixFor`, `Layout::softestHitCutoffHz` = 900 Hz) and `renderVoice` runs it on the way to the summing
bus, ahead of the level — so the top end opens as the hit gets harder, from 900 Hz at the bottom of the travel
to **no filter at all** at the top. The filter is carried into the tail pool with the position and the increment,
because a lowpass that reopened from silence on a steal is a step in the output. At velocity ≥ 1 the branch is
not taken at all: the grid, which defaults every step to full velocity, renders the sample at the bits it was
recorded at, and every pre-existing peak and slew number in the suite is unchanged for exactly that reason.

| `[23]` render (22050-sample noise burst, so the tone is measurable) | peak | rms | tilt HF/LF | the same render with the law off |
|---|---|---|---|---|
| level 1.0, velocity **1.0** | 0.71488 | 0.41327 | 30.51 | 0.71488, 30.51 — identical |
| level **0.25**, velocity 1.0 — the fader | 0.17872 | 0.10332 | **30.51** | 0.17872, 30.51 |
| level 1.0, velocity 0.5 | 0.33342 | 0.12893 | 12.22 | 0.35744 (−0.60 dB), 30.51 |
| level 1.0, velocity 0.25 | 0.14338 | 0.04685 | 6.64 | 0.17872 (−1.91 dB), 30.51 |
| level 1.0, velocity 0.0625 | 0.02813 | 0.00805 | 3.29 | 0.04468 (−4.06 dB), 30.51 |

The right-hand column is `out/smoke-33-noilaw.txt`: `brightnessMixFor` returning 1.0f for everything, the other
186 checks untouched, and three going red — the three that ask whether a soft hit is *darker*. Tilt is a power
ratio, so it cannot be satisfied by a gain change, and row two is the control: the same quarter of level taken
by the fader leaves the tilt at 30.51, exactly where the source's own balance sits. What the colour law costs
in level is stated rather than buried — 1.9 dB of peak at a quarter, 4.0 dB at a sixty-sixth, on flat noise,
which is the worst case a sample can be, since anything with a body keeps most of its energy below those
corners. No gain constant in the engine moved to make either half of this sound better; that is the
silent-compensation line and `[20]` proves it, reading the same **worst slew 0.1127 against a limit of 0.2129**
and the same **loudest hold at 0.7943, the source at the trim** across all 81 corner renders on both envelope
shapes.

One existing check moved, and only one: `[10] a step's velocity is applied once, not squared` read 0.5000 and
now reads **0.4807**, because a half-velocity hit loses the top end its transient is made of. The band went
0.49..0.51 → 0.44..0.52 with the measured number and the reason in the comment — the gain law is still pinned at
exactly 0.5 where the tone law cannot reach, and `[23]`'s full-velocity rows are what keep that honest.
`out/smoke-33-before.txt` is the run that caught it: the law landed before the band moved, 170 checks and 1 red.

The README had gone stale in the same two places: the envelope table described the fall as "falls from full to
the sustain level", and the MIDI section still claimed *the step grid always plays at full velocity*, which
stopped being true when per-step velocity landed. Both now say what the code does, and the test list names the
curve, the 81-render corner sweep and the brightness law.

Run now: **189 smoke checks / 0 failures**, **78 layout / 0 problems**, `RtPathGuard.ps1` exit 0. The one-pole
is four floats per voice and its coefficient is two multiplies from the velocity — the `exp` that makes
`softestHitMix` happens once in `prepare`, not per trigger and not per sample — so nothing here allocated or
looped on the audio thread.

**3.4 — kernel edge renormalisation (done 2026-11-15).** The table is normalised so that a constant in comes out
as the same constant, and that promise holds only while all 24 taps have something to read. Off the start and
off the end of a source the engine used to skip those taps and leave the result as it stood, so the first and
last handful of samples of every *pitched* hit were not at the hit's own level — the first handful being the
transient.

*How it was measured, and what the model got wrong.* A flat DC source is the only fixture that answers this
without a statistical argument: whatever the phase and whatever the band, a read with all its taps inside must
return the constant, and a read that does not has an exact debt printed rather than inferred. On a source flat
at 0.6355 (0.8 through the −2 dB trim) the first four sounding samples of an octave-up hit came out
**0.6824, 0.6176, 0.6409, 0.6345**. The guess going in — written into the plan and into the first version of
the engine comment — was that dropping taps subtracts level. It does the opposite on the first sample: the taps
that hang off the start are mostly **negative sidelobe**, so losing them puts the read 7.4% *above* the level
the hit sits at everywhere else, and the next three samples are the kernel walking back down through its own
normalisation (2.8% cold, then converging). On a signal that cannot move, that walk is a **0.0648 step** in one
sample. `[24]` prints all of it; `out/smoke-34-no-renorm.txt` is the run with the renormalisation branch turned
off and everything else from the step intact — 5 red, and both of the first two went red only after the check
was made two-sided. As first written it asked `head < body * 1.02`, which the overshoot passed. A one-sided
gate on a defect whose sign was guessed is not a gate: the direction was the thing being measured.

*The fix.* In `renderVoice`, the branch that has to test its indices adds up the weights it actually read and
divides the result back by that sum, which restores unity DC gain for a truncated window. The floor is
`used > 0.25`: below it the division would amplify whatever remainder was left, and it is unreachable in
practice because the voice stops the sample the centre tap leaves the source — `samplesToSourceEnd > 0` means
`floor (position) <= sourceLength - 2`, and the head cannot go negative, so the centre tap is inside on every
read the engine makes and the partial sum is bounded well above the floor. The interior keeps its own branch
with **no bounds test at all**, so the fix costs nothing over the body of a hit; and because the read advances
by a constant, a rate that is a whole number of source samples has a constant phase, so its kernel is looked up
once for the region instead of 24 lerps per output sample.

| `[24]` on a source flat at 0.6355 | old | renormalised |
|---|---|---|
| first eight samples, rate 2.0 (hoisted path) | 0.6824 0.6176 0.6409 0.6345, then 0.6355 | **0.6355 ×8** |
| first eight samples, rate 1.50 (per-sample path) | 0.6783 0.6131 0.6468 0.6305 0.6372 0.6351 … | **0.6355 ×8** |
| worst sample-to-sample step, first 40 | 0.064827 (+12), 0.065185 (+7) | **0.000000, 0.000000** |
| decaying kick at +12: rise after the head | +0.0227 on a hit that only comes down | monotone: 0.7117 0.7075 0.7044 0.7007 … |
| …the same at +7 | +0.0353 | monotone: 0.7124 0.7093 0.7071 0.7042 … |
| body rms, every row | 0.6355 | 0.6355 |

The rate-2.0 row is the hoisted kernel and the rate-1.5 row is the per-sample lookup, and both print the same
0.6355, which is what makes the hoist proven rather than assumed. The two precondition checks — unity body at
0.6355 and pitched body at 0.6355 — are in `[24]` so that the ratios can only be read as the kernel: if the
engine had drifted the level anywhere else, the gate would fail on itself instead of quietly measuring the
test. A second fixture, a decaying kick, is there because a constant cannot show a *wobble*: the old head rose
again inside the hit, which is the shape a transient must not have, and after the fix the row only ever comes
down. The tail edge is the same code path, and the end-fade from 3.1 already owns the level there.

What it changed everywhere else: **nothing that prints.** The two runs differ in eleven lines — section `[24]`
and the final count — and the other 189 checks are byte-identical across the pair, including `[20]`'s 81 corner
renders (worst slew still 0.1127 against 0.2129, loudest hold still 0.7943) and the alias check in `[13]` and the
pitch-and-level numbers in `[5]`. That is the level-matched half of this step: an edge fix that moved the body
would be a new gain law wearing a bug fix.

Run now: **196 smoke checks / 0 failures**, **78 layout / 0 problems**, `RtPathGuard.ps1` exit 0. No allocation
and no loop the compiler cannot see through was added to the render path: the hoisted weights are a 24-float
stack array filled before the loop, `scratchWeights` is filled on the stack inside it, and the guard over
`parameterChanged` is unchanged.

**3.5 — per-hit pitch drop (done 2026-11-15).** A lane can start the hit above the note it is asked to play and
fall onto it. `Params::pitchDropId` (`chNPitchDrop`, `AudioParameterInt` 0..12, default 0) is the only way in:
nothing in the engine picks a drop by itself, so the plan's "visible per-lane amount parameter only, never
automatic" is the whole feature surface rather than a preference about it. The parameter count goes 100 to
**112 (4 global + 12 x 9)** — 114 since 4.2's two variation parameters, 115 since 4.3's timing one,
116 since 4.3b's Touch, and the number the check prints is the table's own size rather than a
constant typed into it —
and `[21]`'s "all 112 parameters are present" is exactly the check that would have
caught the classic failure of this kind of step — parameter created, offered to the host, restored on load, and
nobody listening for it.

Shape: an exponential over a fixed 30 ms reaching five time constants, so the last sweep sample is snapped to the
nominal rate exactly (`Layout::pitchDropSweepSeconds`, `pitchDropTimeConstants`, `maxPitchDropSemitones`,
`Common.h:223-225`; `coefficient = exp(-5/1323) = 0.996228` at 44.1 kHz). `voice.increment` keeps its old
meaning — the settled rate, the note — and the fall lives in two new mutable voice fields, `dropExcess` and
`dropRemaining`, recursed once per output sample at `SequencerEngine.cpp:858`. The window cost is one more row
per lane: `ChannelStrip.cpp:299` (was `:249` before 5.6 rewrote `resized`) 205 to 219 px and the editor 830 to
**844 px** at 1240 wide (the width is
**1520** since 4.2's panel half, **1678** since 4.3's timing fader; 844 is still the default, and since 5.6 it is
nothing else). The grid keeps its
405 (`out/layout-35-final.txt`: component 1 at y=126, h=405), so the window pays the 14 at the bottom. That was
the debt Phase 5 U5 had to pay back, and it is paid. *(5.6 covered both halves: that 14 is now
`Layout::stripPlan`'s `dropRow`, 14 px in the two roomy stages and 12 in the two at floors, and the strip is
285 px at the default; and the 844 quoted above is the **default** only — the *minimum* is 695 px now, which
`Layout::editorPlan` prices rather than types. The 5.6 entry has the numbers and the gates. *4.5 is the third
width in that chain: a fourth control on the bar took the default to **1890** and the minimum to **1298**, and
left 844 and 695 where they were.)*

*What `[25]` measures.* Periods are counted from zero crossings, so the claim is about pitch and not about a
waveform shape; the fixture tone is a known 220 Hz, hence a known 200.455-sample period.

| `[25]` row | measured | the glide model predicts |
|---|---|---|
| cycle 1..4 period, drop 0 | 200.455 | 200.455 — the note |
| ...drop 5 | 186.256 early, then 200.455 | 186.3 (+5 = 1.414) |
| ...drop 12 | 152.521 early, then 200.455 | 153.0 (octave up; 0.761 of the note) |
| successive cycle lengths, drop 12 | 7 cycles, 131.8 .. 198.7 | 132 154 164 173 186 192 197 |
| widest step back up | 0.000 | monotone: it only ever goes down |
| settled rms, none vs twelve | 0.56127 vs 0.56162 | 1.0006 — no level law |

The first version of this section failed two checks on numbers I had *predicted* rather than measured — a 0.75
ratio, and "more than 8 cycles across the sweep", which cannot happen inside a 1523-sample window. Both were my
model of the measurement, not of the engine: `meanPeriod (data, onset, 3)` spans crossings 1 to 4, so the window
starts one cycle into the fall. Re-derived analytically and then from the printed lengths, the bars are now 0.85
and "at least 6", with a second, separate check that the longest cycle is more than 1.3x the shortest — because
a gate that only asks "is the early part shorter" is satisfied by a hit that holds one pitch and calls it a
glide, and two of the first three gates did pass on the broken build (see the A/B table).

*Two claims of mine died over this step, and both changed what ships.*

**The countdown.** `renderVoice` counts down to the end of the source from `((sourceLength - 1) - position) /
increment`. During a fall the increment moves, so the recomputation at `:868` looks optional; the plan and the
first engine comment both asserted that skipping it "cuts the hit in half". The A/B came back **byte-identical**
— and a real defect does not do that. The reason is at `:704`: the countdown is re-derived from the current rate
at the top of *every* call, so a stale one can only ever survive the block it was computed in. What it really
costs is therefore bounded by the host's block size, which is a smaller claim and a much more useful one.
Measured on a fixture short enough for the sweep to matter — a 1600-sample (36 ms) sample struck with a drop of
12:

| block | stale countdown (`out/smoke-ab1-naive-countdown.txt`) | as shipped |
|---|---|---|
| 256 | ends 1313, rms 0.29850 | ends 1313, rms 0.29873 |
| 512 | ends 1311, rms 0.29814 (0.9988) | ends 1313, rms 0.29873 |
| 1024 | ends **743**, rms 0.21928 (0.7346) | ends 1313, rms 0.29873 |
| 2048 | ends **743**, rms 0.21928 (0.7346) | ends 1313, rms 0.29873 |

So the divide is not there to save the hit from being chopped in half. It is there because **the same strike has
to be the same hit whatever block size the host asked for**: at 1024 the stale version throws away 570 of the
1576 samples that hit has, and the fixed column is identical to four significant digits down the whole table,
which identity is the form the gate asserts now. At 256 the stale version is indistinguishable on the end —
which is why eleven earlier sections of this suite, all run at small blocks, never saw it.

**The band.** The anti-alias cutoff is picked from the fastest rate the read ever gets to, so at a drop of 12 the
note-on band is the one for ratio 2 (band 5, 0.453 of source Nyquist) rather than the one for ratio 1 (band 14,
0.944). `SequencerEngine.h` said this band was chosen once per hit "so the filter cannot change while a hit is
sounding", and holding to that promise costs the whole tail an octave of top end it no longer needs: `[25]`'s
broadband tilt — a level-independent high/low energy ratio over 2000..7000, so no level matching is involved —
measured **-4.03 dB at a drop of 12**, and the +1 row separates what an interpolation always costs (-0.73 dB, 24
taps) from what the pinned band costs (-3.29 dB). A tuning control should not be a tone control. `renderVoice`
now follows the rate (`band = SincKernelTable::bandForRatio (increment)`, `:878`) and the sweep keeps the band it
settled on (`voice.sincBand = band`, `:911`). The first tracked run printed the pinned numbers *again*: without
the write-back the next call walks back in holding the peak-rate cutoff, which is the pinning, so the tracking
lasts exactly as long as the sweep and not one sample longer. Both halves ship, and the tail is now -0.51 dB at
+12 and -0.55 at +5 — **0.995 and 0.984 of the +1 baseline**, that is down to the kernel's own price with -0.02
dB left over attributable to the band. The switch is not a step: these kernels are symmetric about the same
centre tap and normalised to unity DC, so below the tighter of the two cutoffs they are the same signal — which
is why the same section still measures one uninterrupted glide straight through the change. (The baseline itself
moved, 26.06 to 27.59: a +1 hit used the band for ratio 1.059 at note-on and settles now on the band for 1.0.)

*What the feature honestly costs.* A fall reads extra source, so the hit ends sooner by exactly the extra it
consumed: 263 samples, and the same 263 whether the sample is 454 ms (19980 to 19716) or 36 ms (1576 to 1313).
The whole-hit rms of a dropped short hit is 0.9023 of the undropped one *because it is shorter*, and the check
that says so prints both numbers, over the settled part, where the ratio is 1.0006. No gain law went in. Per
sample the sweep pays one multiply, one divide and one `bandForRatio`, for 30 ms; after it settles the voice
takes the same path it always did. The only pre-existing behaviour that changed at all is that a lane with drop 0
is now measured to be at its pitch from its first sample — 200.455, unchanged.

| A/B | red checks |
|---|---|
| `out/smoke-ab8.txt` — drop accepted but ignored | 3: starts above, amount is the amount, falls a bit |
| `out/smoke-ab1-naive-countdown.txt` — no per-sample re-derive | 3: block 512 / 1024 / 2048 |
| `out/smoke-35-colour2.txt` — band pinned at note-on | 1: -4.03 dB of top end at +12 |
| `out/smoke-35-bandtrack.txt` — tracked in the loop, no write-back | byte-identical to the pinned row |
| `out/smoke-35-bandtrack2.txt` — both halves, first gates | 2, including "monotone in the parameter", which the tracked tail is not (27.15 at +5, 27.43 at +12) — restated from measurement, not relaxed |
| `out/smoke-35-final.txt` — as shipped | 0 |

Run now: **212 smoke checks / 0 failures**, **82 layout / 0 problems**, `RtPathGuard.ps1` exit 0. Three floats
per voice and no allocation: `dropExcess`, `dropRemaining`, and the band the sweep ends on.

#### 3.6 — true-peak metering on the master (done 2026-11-15). Phase 3 is closed.

`MasterBus` already knew the highest sample it wrote. That is the number a clip latch wants and not the number a
converter puts out: between two samples the band-limited waveform can be higher than either of them, so a bar
that stops short of 0 dBFS can still hand a host something that clips. The bus now reports a second number — the
peak of its own output reconstructed at four points per sample — and nothing else uses it. It gates no level,
changes no latch, reshapes no audio, because the house rule is that the trim and the faders are what the user
has, and a limiter was deliberately not part of this.

**The reconstruction.** `truePeakOversampling = 4` (`MasterBus.h:55`): three kernels at fractions 0.25, 0.5 and
0.75, built once in `prepare` from `SincKernelTable::interpolatingWeights` (`Resampler.h:103`). That function is
new and so is `fillKernel` (`Resampler.h:64`), which is the kernel maths that used to be inline in the table's
constructor — lifted out rather than duplicated, so the meter and the resampler cannot drift apart, and so the
meter does not allocate the table's 380 kB to read 72 numbers. Per output sample per channel the window shifts
left one `memmove`, and three 24-tap dot products run against it: 72 multiply-adds, no allocation, no trig, no
table lookup on the path. Tap `j` stands for sample `i-23+j`, so the read position is `(i-12)+f` and the interval
being measured is the one twelve samples back — 0.27 ms at 44.1 kHz. That delay is what a kernel this short can
afford, and it is invisible against a meter that already holds peaks for 120 ms.

**What `[26]` measures.** Seven tones at 0.8 whose samples straddle the crest — phase π/2 + π/N, N even — so
both numbers are known before anything runs: the sample peak is `A·cos(π/N)` and the level of the wave is `A`,
and the gap has to be `1/cos(π/N)`.

| f (Hz) | bar | bar should be | tick | crest error | gap | gap should be |
|---|---|---|---|---|---|---|
| 441 | 0.7996 | 0.7996 | 0.8000 | -0.0000 | 1.0005 | 1.0005 |
| 882 | 0.7984 | 0.7984 | 0.8000 | -0.0000 | 1.0019 | 1.0020 |
| 2205 | 0.7902 | 0.7902 | 0.7999 | -0.0001 | 1.0124 | 1.0125 |
| 4410 | 0.7608 | 0.7608 | 0.8000 | -0.0000 | 1.0515 | 1.0515 |
| 5512 | 0.7391 | 0.7391 | 0.7999 | -0.0001 | 1.0823 | 1.0824 |
| 7350 | 0.6928 | 0.6928 | 0.8000 | +0.0000 | 1.1547 | 1.1547 |
| 11025 | 0.5657 | 0.5657 | 0.8001 | +0.0001 | 1.4144 | 1.4142 |

Worst bar error 0.00000, worst crest error 0.00011, over a 25:1 frequency range. The gap column is the one that
matters for a meter: it is the inter-sample headroom the editor draws, and it reproduces `1/cos(π/N)` to four
decimals at every row. Alongside it: a third of the sample rate, where the samples land at 0 and at `A·√3/2` —
bar 0.6928, tick 0.8002, gap 1.1549 against the 1.1547 that √3/2 implies; 1 kHz, where a sample already sits on
the crest, and the tick invents nothing (tick/bar 1.0000); a lone 0.9 sample, where the reconstruction *is* the
kernel and the highest point of the kernel is the sample, so the tick reads 0.9000 and not the filter's own ring;
±0.9 square, which is the opposite case — a hard edge with real inter-sample level, bar 0.9000 against tick
1.1378, 1.2643, where an infinitely banded square overshoots to 1.179 and a 24-tap Hamming kernel puts its
passband ripple on top of that; the same signal read identically at a 256 and a 2048-sample block; and bit-exact
audio (`worstTouched == 0.0f`) on every row, which is the claim that the meter is a report.

In situ, one real hit through the master: bar 0.4292, tick 0.4846, **+1.05 dB** of headroom the samples alone do
not show, nothing latched.

**Two claims of mine were wrong, both about the fixtures.**

*The kernel does not over-read near the top of the band.* The first `[26]` run put the 441 Hz tick at 0.9042 — 13
% over a 0.8 wave — and the Fs/3 gap at 1.2220 against the analytic 1.1547, and I wrote those up as the honest
limit of a 24-tap filter whose transition band sits just above Nyquist, answering to the tone *and* its image. A
Python model of the same kernel, the same normalisation and the same window, run with zero history, reproduced
0.90416 / 0.89952 / 0.87244 / 0.80934 / 0.82087 / 0.80003 / 0.87029 — the C++ numbers to five digits — and put
the peak at i = 12, 13 or 15: the first intervals the 24-tap window can span. The tones started at full
amplitude, which is a step, and the band-limited reconstruction of a step rings above the crest for exactly as
long as the kernel is wide. Nothing to do with the meter. With a 64-sample raised-cosine on the start of every
tone — `sine`'s job now, and the comment in the test says why — the sweep reads the crest to 0.00011 and the
Fs/3 gap to 1.1549, and the image story is gone. The square wave keeps its hard edge on purpose; that is where
the ring is the finding rather than the artifact.

*The reflex was the bug.* Facing a red sweep row, the cheap move was to widen the gate from 0.02 to 0.07 and
document 0.9042 as the kernel's limit. That would have written a fixture's start step into the DSP's
specification, in the README, and in a check designed to pass. Fixing the signal instead let the gate tighten to
0.005 and turned a claimed weakness into a measured strength. When a check is red, the first question is what
the signal actually *is*, not what number the limit should become.

One more check went in because the README's "report, not guard" sentence was only asserted by a case where the
tick stayed under 0 dBFS anyway: the square now proves the interesting case, tick 1.1378 past 0 dBFS with the
bar at 0.9000 and the latch **clear** — a reconstruction that exceeds full scale does not get to decide what the
user hears, and does not light the indicator either, which still means a sample reached 0 dBFS.

| A/B | red checks |
|---|---|
| `out/smoke-36-ab1.txt` — `truePeakOversampling = 1`, estimator off, first test file | 4: sweep crest, Fs/3 crest, Fs/3 gap, square overshoot |
| `out/smoke-36-ab2.txt` — same, final test file | 5: those four plus "a true peak over 0 dBFS with no sample there latches nothing" |
| `out/smoke-36-final.txt` — as shipped | 0 |

What stayed green with the estimator off is what should have: the 1 kHz ratio, the impulse, block-size equality,
bit-exact audio and the latch semantics. A reconstruction that is switched off still cannot invent level on a
tone whose samples are already on the crest. If those had gone red too, they would have been measuring the
existence of the filter rather than the accuracy of the meter — and the five that did go red are the ones that
would have to be re-explained if the tick were ever wired into a gain stage.

Run now: **228 smoke checks / 0 failures**, **82 layout / 0 problems**, `RtPathGuard.ps1` exit 0. The cost is
72 multiply-adds per sample per channel, three `std::array<float, 24>` per bus and one `float[3][24]` of state,
and a reading that is 12 samples late. `launch/` has not been re-assembled for 3.5 and 3.6 yet: Ableton Live and
the standalone are both running on this machine, and the target removes `launch/Groovebox.vst3` before rewriting
it, so it waits for the apps to be closed rather than fighting them.

### Phase 4 — variation

**4.1 — the step payload says what a step is (done 2026-11-15).** The grid now travels as `v2:` + base64 of two
bytes a step — the switched-on flag, then the level as a 7-bit MIDI number — with `stateVersion: 2` on the root
of the tree beside it. `[27]` is the new section, `[19]`'s record probe was rewritten to read the bytes, and
`Source/PluginProcessor.cpp:1245-1526` is the codec.

| | v1 rows, on this build | v2 |
|---|---|---|
| payload for 192 steps | **395 characters** | **515 characters** |
| how a reader knows the format | row length: 16 chars a step means one digit, 32 means two | `v2:` on the payload, `stateVersion` on the root |
| level resolution | hundredths of the range (0..99) | 127 steps, and `k/127` for all 128 values of *k* comes back **exact** |
| full velocity | `jlimit (1, 99, …)` **clamped it to 0.99** — every hit at the top of the travel came back 1.3 MIDI steps quiet | 127/127 |
| a step that is off | keeps no level at all: `setStepVelocity` then a save/load moved it to 1.0, **63.0 MIDI steps** away from where it was left | keeps the byte it was given |
| a step on at zero | cannot be written: 0 meant off, so the code raised it to 1 | flag and level are separate bytes; on-at-zero is on-at-zero |
| a payload from a newer build | decoded as digits — base64 is full of them — and **lit steps the session never set**, then re-wrote the property in its own format on the next save | kept byte for byte, grid empty, until an edit takes the row over |

That last row is the one that decided the design. Feeding a `v99:` payload to the old decoder produced a grid
with steps on that no one painted (`and a build that cannot read it plays nothing it did not read`, red on the
old code with *"the grid still holds a step from before the load"*), and the following `getStateInformation`
handed back `0009…`. An unreadable record is therefore not decoded, not repaired and not re-encoded:
`holdingUnreadablePattern` makes `getStateInformation` skip its flush, and `pushPatternToState` — which is the
only place the flag clears — is reached by an edit, not by a save. The root's `stateVersion` is left at what
came in too, because stamping 2 on top of a v99 pattern would describe a record this build did not write.

Backwards is measured, not assumed: v1 digits still load at 0.75 / 0.05 / 0.99 (the legacy path keeps its own
hundredths, so nothing moves onto the new lattice until the row is written again), v0's ones and zeros still
come in at full velocity, a row shorter than the grid still invents nothing and the next row still reads from
where the short one stopped, and a row longer than the grid is still truncated at the edge rather than spliced
into the neighbouring channel. Those six checks are green on **both** sides of the A/B — they are regression
gates for the old behaviour, not evidence for the new.

One consequence is worth stating plainly, because it is a change of a stored number: opening a v1 session and
saving it writes v2, so its levels move from hundredths onto the 1/127 lattice, by at most half a MIDI step.
The alternative — leaving a readable record alone until it is edited — keeps the old bytes but leaves the
session half-migrated and the `stateVersion` property lying about it. It is in the README under *How it is put
together*.

The plan's other half — *"`shiftChannelRow` taught to carry every per-step field it now moves"* — is gated
rather than refactored, and the difference matters for 4.2. The shift already moves both fields it knows about;
what it did not have was a test that could see the second field go, because the payload could not hold one.
`[27]`'s turn-and-save now shifts a row by three and asserts all three levels, **including the one on a step
left switched off**, against where they were written: 0.3, 94.0 and 1.3 of a MIDI step on the old code, 0 on
the new. When 4.2 adds a probability byte and 4.3 a microtiming byte, `stepRecordBytes` in
`Source/PluginProcessor.cpp` is the one constant that changes, the length check follows the grid rather than a
literal, and this check is where a field that does not travel gets caught. What a new field *will* still need is
a line in the two loops of `shiftChannelRow` — the enumeration is not yet impossible to forget, only
impossible to forget silently.

*Superseded by 4.2 on the width:* `stepRecordBytes` is gone, and one constant would not have been enough. The
reader's question is "how wide is a step in **this** record", and the answer differs between the generations a
build can read — a v2 session has no probability to carry — so it is now `strideForCodec (int)` in the same
file, taken from the payload's own tag. A narrower known width is a format, not a defect; only `stride == 0`,
a codec this build cannot name, still means "keep it, do not repair it".

Two harness faults cost more time than the feature and are written up in `AGENTS.md`: the smoke target needed
`/STACK:16000000` because a 23 KB processor × dozens of locals in one `main` dies with
`STATUS_STACK_OVERFLOW` **printing nothing**, and `loadSession` was handing `setStateInformation` bare XML,
which `getXmlFromBinary` answers with `nullptr` — so eight checks had been measuring a load that never
happened. A third, found while A/B-ing: `Copy-Item` preserves the source's mtime, so restoring a file by copy
left MSVC free to skip the TU, and the "green" run was a stale link. Both scripts now refuse to run a binary
older than any source file, and the A/B below was re-run after that check existed.

A/B, same test file both sides (`out/ab-41.ps1`, which swaps only the three codec sources):

| Build | result |
|---|---|
| `out/smoke-41-oldcodec.txt` — HEAD's `pushPatternToState`/`applyPatternFromState` | **252 checks, 12 failures**, exit 1 |
| `out/smoke-41-gate.txt` — as shipped | **252 checks, 0 failures**, exit 0 |

Ten of the twelve are the format and the fields. The two in `[19]` are the record probe following the format it
probes: the behaviour they gate — an undo moving the string *and* the engine together — was green before and is
green after.

Run at the close of 4.1: **252 smoke checks / 0 failures**, **82 layout / 0 problems**, `RtPathGuard.ps1` exit
0, and a full `cmake --build build-new --config Release` (VST3 + Standalone + both console apps) clean.
`launch/` was not refreshed then — the Live wait behind that was a `tasklist` inference, not a lock proof (corrected at the end of this file).

Found on the way, **not fixed**: a shift is two stores per cell, so shifting a row *while playing* can let the
audio thread read a half-rotated bar for one block — one hit duplicated or missing, then self-correcting. It is
a pre-existing property of the atomics rather than something 4.1 introduced, it cannot corrupt a session, and
the fix (a shadow grid behind an atomic index) is a different change than a payload format.


**4.2 — a seeded probability per step, and one knob for how much of it happens (done
2026-11-16, engine and panel).** The plan asked for *"seeded per-step probability + a single 'mutate' amount. RNG keyed
on (bar, step) so offline render equals live playback and A/B stays reproducible. Gate: at probability 1.0 the
output is bit-identical to today's; at 0.5 across 64 bars it is deterministic"*, and both gate halves are now
machine answers.

What landed is one field, one function and two parameters. A step's third payload byte is its chance as a number
of 255ths — `Layout::chanceLevels`, so the byte fills its field and 255 means *fires every time*, which is also
what a step from a v2 record means and what every cell of a new session starts as; `stateVersion` is 3 and the
tag says `v3:`, 771 characters for the grid against v2's 515. `setStepChance` snaps to the rung *before* the
engine sees it, so the percentage on the panel is the number in the file rather than the nearest thing to it that
survives a save. `shiftChannelRow` now carries three stores per cell where it used to carry two, and `[29]`'s
turn-and-save is the check that catches a field that stays behind.

The decision lives in exactly one place, `SequencerEngine::wouldThisHitFire (bar, step, channel)`, which
`fireStep` calls and which the tests call too — the same function, not a model of it. A seam the audio thread
never reached would have answered every determinism question below perfectly while playing the grid as if the
knob did not exist, which is why `[29]` also renders: seed 7 at full variation fingerprints `AE5D8A8C366F3F9C`
twice over, seed 8 is `DC37925D3C3F0D1C`, and a grid whose every cell is certain is `507E5F3650B7D274` — the
fingerprint of the same machine with the knob shut, to the bit.

The arithmetic is `1 - mutate * (1 - chance)`: the drop-out can only grow from zero and never past what the cell
itself asked for, so opening the knob is a promise the grid can be read back from rather than a second pattern
drawn somewhere else. Two gates short-circuit before any generator is consulted — variation shut, or a cell that
fires every time — and that is why the closed knob is bit-inert instead of merely quiet: nothing that decides a
sample is even looked at. The key is splitmix64 over the seed and the bar, step and lane of the hit, and the top
24 bits of the answer are the draw, so the question is a bijection of *where the hit is*: no counter of rolls, no
clock, nothing to reset on a seek. The bar comes from `onsetPositionInSteps (step)`, the same arithmetic that
decided to schedule the onset, so a hit that arrives a sample late after a host seek is still keyed to the bar it
is in.

`Mutate` (0–1, default 0) and `Seed` (0–999999) are ordinary parameters — 114 then, 6 global +
12 × 9, and 115 with 4.3's `Humanize` — so a host
can ramp variation over time and a take can be named by a number you write down. Getting them to the engine
re-caught the oldest trap in `AGENTS.md`: `allParameterIds` had a fourth hand-written half, so both new
parameters were created, offered to the host and read on restore with **nothing listening for them**, and only a
test that writes every id could see the engine never moved. It is derived from `Params::globalIds()` and
`Params::laneTargets()` now, with the length baked into the array type so forgetting a row is a compile error.

Measurements from `[29]` — 64 bars × 16 steps × 4 lanes = 4096 cells, every step painted, every chance at
128/255:

| question | answer |
|---|---|
| knob shut | **4096 of 4096 fire** — the roll is not consulted at all |
| knob open | 2127 fire (0.5193), against the 0.50196 the cell asked for |
| the ladder at 0 / .25 / .5 / .75 / 1 | 4096 → 3636 → 3158 → 2646 → 2127, **0 cells reappear** |
| half the knob, asked of the formula | 0.7710 fired against **0.75098** asked; band **0.0495** |
| seed 7 against seed 8 | 2062 of 4096 cells move (0.5034) |
| order | forward and reverse walks agree cell for cell, on two machines prepared at 512 and 256 |
| asking twice | the same hash — nothing advances |
| a reroll | a new seed inside the range, all 64 lit cells and every chance byte where they were, about half the answers moved (2048, 2055, 2068 on three runs) |

The band in the fourth row is the interesting one, because the first version was wrong in a way that only shows
up in the margin. It was four standard deviations of a *fair coin* over 4096 cells — 0.0271 — and the take came
out 0.0200 from the expectation: inside, by six thousandths. One legitimate change of the key would have gone red
on correct DSP. A hash is not a coin, and the spread of a hash over neighbouring seeds is a property of it that
can simply be measured, so the shipped limit takes three times the worst departure from their mean over **31
other seeds** (mean 0.7499, worst 0.0165) with seed 7 deliberately out of that sample — a limit drawn around the
point it then gates is a tautology. The comparison target is the engine's own formula re-evaluated, not a rounded
saying of it. Both lessons are in `AGENTS.md` with the two other harness faults this step found: a triple loop
whose forward direction started one past its own end condition and ran **zero times** — seven reds against a
correct engine, and three *greens* on an empty answer, because `added == 0` and `== 0 fire` are both satisfied by
nothing — and `MemoryOutputStream` reporting the allocation rather than the payload until it is flushed, which
made a 576-byte record read as 832 bytes and made `[19]`'s lower-bound probe look like it had never noticed
anything.

A/B, one line of the engine (`out/smoke-42-ab-seedblind.txt`, key built without the seed): **291 checks, 4
failures**, exit 1 — `a different seed is a different take`, `neighbouring seeds disagree about about half of it`,
`the take moves with it` and, in the audio, `a new seed is a new two bars`. What stayed green is what the change
does not touch: the knob-shut precondition, the subset law, the bit-inert gate and the same-seed render again. The
seed is load-bearing and nothing else moved.

**The panel half — done, in the same step.** `TransportBar` now carries the four controls
that make the feature usable without a host:

| control | what it writes | note |
|---|---|---|
| **Chance** toggle | `StepGridComponent::setChanceMode`, through the editor's `onChanceModeChanged` | beside **Velocity**; the two are exclusive by hand |
| **100%** | `resetChances()` or `resetVelocities()` | whichever overlay is open; the tooltip says which before the click |
| **Mutate** slider (0–100 %, double-click shut) | `GrooveboxProcessor::setMutate` | writes straight through, the same rule swing obeys |
| **Reroll** + the **Seed** readout | `rerollSeed()` | the number is on the panel because a take you cannot name is a take you cannot get back |

The exclusivity is `closeOverlay` in `TransportBar.cpp`, not a `RadioGroup`: JUCE computes
`shouldBeOn = (radioGroupId != 0 || ! lastToggleState)`, so a grouped button lights and can
never be clicked out again — and both overlays being off *is* the resting state. Enforcing
it in the bar rather than leaving it to the grid is the point: `openOverlay` already keeps
one overlay, so a bar that let both stay lit would show a mode the grid is not in, and the
drag would write the field the dimmer button says is off.

One row, and the row is now full: `editorWidth` went **1240 → 1520**, and that value is
both the default and the minimum, because the alternative to widening the window was a
fader too narrow to drag or a button clipped to zero width — which is precisely what the
layout test reads as an unclickable control. This makes **5.6's laptop floor worse**, not
better: at 1366 px the bar cannot lay out, so that step now has to give the row somewhere
else to go rather than only trim heights. *(Closed 2026-11-16 by 4.3b, which is the step that
gave it somewhere else to go: the row wraps below 1774 px and the floor came down to
**1218×844** (the height half of that is 5.6's, and is now 695). The widening sentence above stayed true for the default size and stopped being
true of the minimum the moment `editorWidth` became a default rather than a floor — which is
what `PluginEditor.cpp`'s comment now says. Both of those numbers are 4.3b's own; 4.5's fourth
control moved the wrap to 1854 px and the floor to **1298×695**.)* It is no longer reachable from `1240×830`, and
it cites `PluginEditor`'s `editorWidth`/`editorHeight` rather than a line number, because
the comment block above them just moved both.

`LayoutTest` is **82 → 101 checks**. Two of the additions are dispatch, not decoration:
the bar's three faders are now found by `getName()` — `Swing`, `Master`, `Mutate`, and
`Humanize` joined them in 4.3 — because
the old `getMaximum() <= 1.01` test was two-way arithmetic and a third 0..1 fader would have
been driven and read back against swing's parameter, i.e. red on correct code. And the
eleven new checks are each known to bite: with the exclusivity, the reset's dispatch and the
seed readout deliberately broken in one build, the run came back **exit 1, 4 problems** —

```
BAD  the two overlays are open at once (chance lit=1 velocity lit=1 grid chance=1 grid velocity=0)
BAD  the reset did not do the probabilities (all full 0, velocity at 1,3 1.00)
BAD  the grid did not go back to velocity editing (chance lit=1 grid velocity=0)
BAD  the seed readout did not follow the take (label says , the engine is 545198)
```

(`out/layout-42-ab-panel.txt`; the shipped green run is `out/layout-42-all.txt`.) The
dispatch is checked in both directions, because a handler that always did velocities would
have passed the probability half by accident, and the undo half asserts that one click is
one gesture. Reroll is checked as *not* an edit: the history is cleared first, so "nothing
to take back" is a claim about the reroll rather than about whatever the earlier sections
left in the manager.

`README.md` gained the section the panel needed (**Step probability, and the take**), the
100% bullet now says which number it resets, the automation row names the parameters the
host actually lists (`Mutate`, `Seed`), and the sweep caught two counts the core commit had
left at **112** where the code says 114.

**4.2b — a probability is visible outside its own overlay (done 2026-11-16).**
`StepGridComponent::paint` dimmed a cell by velocity only, so a step that fires one time in
four looked exactly like one that fires every time until **Chance** was pressed. The mark is
the corner of the cell cut off, with the edge of the cut lit: a notch alone would have to be
dark or light, and the grid's own colour law guarantees the cell is neither at one end of the
velocity range — a mutable step sits on a pale lane at full level, where a light pip vanishes,
or at a tenth of it, where a dark one does. Two colours from the mark itself, not from the
cell, so it reads whatever is underneath, and nothing about what the colour *means* moves.

The rule is one function, `StepGridComponent::isStepMutable`: marked when the cell is lit, the
overlay is closed, the knob is open and the step's own chance is under certain. Each term is a
claim about a different thing — an off step is not waiting for the take, it is not in it; with
the knob shut the grid is law, so the mark would be warning about a thing that cannot happen;
inside an overlay the bar owns the rectangle and a notch would be eating the number the overlay
was opened to show. `launch/`-side consequence: moving the knob is a parameter write, which
does not go through `onStateChanged`, so the grid's 30 ms timer now watches `getMutate()`
alongside the playhead and the activity lights — at rest, that was the only way the grid could
find out.

Tested by painting. The mark has no bounds, so every component-inspecting check the suite has
would be inspecting a rule only `paint()` applies; the layout test renders the grid into an
image and compares two renders that differ in exactly one cell's chance. **109 layout checks,
from 101**: the mark appears, it appears only inside that cell, it is a cut *and* a lit edge
(some pixels down, some up), its centre is in the bottom-right quadrant, and three negative
controls — knob shut, step off, overlay open — each demand two renders come back identical.
Five-arm A/B (`out/ab-42b.ps1`, logs `out/layout-42b-*.txt`) kills 4 / 1 / 2 / 2 / 1 checks per
arm, and two harness faults fell out of it rather than being stated in advance. The arms were
editing the working copy instead of the snapshot, so after the first deleted the `paint()` call
the other two reported the first one's answer — three identical BAD blocks that looked decisive
and measured one mutation; each arm now restores the recorded good file first. And the overlay
control was built at velocity 1.0, where the bar fills the cell and hides a mark that leaked
into it, so arm d passed on a build with the overlay term deleted; at velocity 0.1 the bar is
3 px, the notch is 10, and it bites. Both are in AGENTS.md with the numbers.

One asymmetry worth keeping, because it is the honest shape of the result: the off-cell control
cannot go red on a predicate that ignores `isStepOn` — `paint()` only draws marks inside the lit
branch, so the outcome is already correct before the rule is consulted. That term is discharged
by the rule check instead. Eight new checks, seven of them killable; the eighth is a claim about
pixels that a different piece of code is holding up.

Docs: the README's **"Where it shows"** paragraph was a present-tense statement of the defect
this step closes — "only while Chance is open … looks like a certain one until you press the
button" — and is now the mark's own description, including what takes it away; the feature table
row gained the cut corner; the layout-test paragraph says the mark is checked by comparing
renders. `installer/README.txt` gained the same in plain text. Run at the close of 4.2b: **291
smoke checks / 0 failures**, **109 layout checks / 0 problems**, staleness guard 0 on both
(guard now scoped per target — see AGENTS.md), `Tests/RtPathGuard.ps1` exit 0, `Groovebox`
target exit 0. No audio changed.

Still not done here: the mark reports the *possibility*, not its size. Three cells at 10 %,
50 % and 90 % carry the same corner, and deepening that is what the overlay is for.

Run at the close of 4.2, panel included: **291 smoke checks / 0 failures**, **101 layout
checks / 0 problems**, staleness guard 0 on both, `Tests/RtPathGuard.ps1` exit 0
("the parameter path is allocation-free"), and the `Groovebox` target (VST3 + Standalone)
exit 0. `[28]`'s golden fingerprint is still `4BC3AA231178C071` after the codec bump and two
new parameters, which is the "bit-identical at probability 1.0" half of the gate answered on
the whole 161280-sample take rather than on two bars. `launch/` was not refreshed then either, on
the same unproven Live wait — corrected at the end of this file.

Still open from 4.1 and unchanged by this: a shift is two stores per cell, so shifting a row *while playing* can
let the audio thread read a half-rotated bar for one block. The probability now makes it three.

**4.3 — late on purpose (done 2026-11-16, with two deviations).** One global parameter,
`humanize` (0–100 %, default **shut**), and every grid hit is allowed to start a little behind the
onset it was given. The offset is drawn per hit from the same key the roll uses — seed, bar, step,
lane — and it is a *fraction of a step*, so it follows the tempo the way swing does instead of turning
into a flam at 240.

Two claims in the plan line are not how it went, and both were decided before the code rather than
discovered after it.

- **The offset is not applied inside `stepOnsetInSteps`.** The plan asked for it there to keep the
  ≤1-sample invariant; applying it there moves the step's *own* position, which is exactly what the
  sample-accuracy gates in [2], [3], [4] and the many-bars runs in [12] measure, and an early offset
  can put an onset behind the playhead — the one path in the clock that answers a late request by
  pushing the step a whole bar forward (`firstStepAtOrAfter`). So the clock is untouched and the wait
  lives on the voice: `Voice::startDelay` is a countdown of output samples spent at the top of
  `renderVoice`, before the voice reads a sample, advances an envelope or lights its lane. The
  invariant survives because the invariant was never asked about this knob; what replaced it is a
  direct measurement that the hit arrives *drawn* samples late.
- **Velocity jitter is deferred to 4.3b.** A timing change and a level change in one step would put two
  sound-moving edits in the same commit, and the level half needs its own A/B — the tone law maps
  velocity to brightness, so jittering velocity is jittering timbre, and "it is the same samples, only
  later" cannot be the gate for both. *(Discharged by 4.3b below: its own fader, its own A/B, and the
  tone cost measured and stated rather than bought back.)*

**The bound is arithmetic, not taste.** `Layout::maxHumanizeLateSteps` is 0.25 steps because the
closest two onsets this machine can put a bar at are **0.5000** steps apart — asked of
`getStepOnsetInSteps` at swing 1.0 on the 1/8 grid and on the 1/16, and **1.0000** on the straight grid
(`out/smoke-43.txt`), so the pressure is at the shuffle, not on the straight row. Half the gap is the
largest offset that cannot overtake the step in front: a late hit can join the next one, it can never
pass it, and the pattern the user drew stays the pattern that comes out. Two checks hold that against
the measured gaps rather than against the constant.

**The law is squared, and the number in the comment was wrong before the measurement agreed.**
`unit * unit` on the draw puts the average hit at a third of the travel (measured **0.08202** of
**0.25000** steps over 4096 keys) and **2065 of 4096** — half — inside the bottom quarter of it. A
uniform draw measures 0.12367 for the same bound, which is the "whole groove reads as late" failure the
skew is there to avoid. Two sentences written from intuition were caught by writing the gate: the code
comment claimed "three quarters of the hits inside the quarter of the travel" (it is half) and described
0.25 steps as "a quarter of that gap" (the gap is 0.5 steps, so the bound is *half* the gap). Both are
now the measured numbers.

**Which hits fire does not move when the timing knob moves, and that is a property of the bits.**
The roll reads bits 40..63 of the key, the timing draw reads bits 0..23, so the two are provably
disjoint. The gate is a point-biserial correlation over 4096 cells: **r = −0.0012** with the split, and
arm d below (timing made to read the roll's bits) comes back **r = −0.8662**. The same section asserts
the roll is non-degenerate first — 2085 of 4096 cells fire — because a correlation computed on a walk
that skipped every cell is 0 and reads as a pass.

**What the suite now knows.** Smoke is **291 → 321 checks**, and all thirty additions are in section
[30] "a late hit is late on purpose": shut draws nothing off the onset; the bound, the mean and the
half-within-a-quarter bands; the amount *scales* the draw rather than replacing it; the same seed twice
and the order-independence; a new seed re-draws 4096 of 4096 keys and the new take obeys the same law;
the timing/roll bit-disjointness; the bound against both swing grids; a seed searched for whose first
hit is drawn 160–600 samples late (found: seed 2 → 482 samples, onset 483 against 1); the hit arriving
exactly the drawn number of samples after the grid hit; **the late hit being the same samples at the
same level — worst sample difference 0.000000000, no output sample differing**; the wait surviving a
block boundary at 64, 256 and 1024 alike, and spanning at least three blocks at the test block so that
check can actually go red; the lane lighting in the block the sound starts in rather than the block the
hit was fired in (block 7 of 64-sample blocks), with the no-wait case still lighting on its own block;
and the state half — a fresh processor starts shut, the value round-trips, the restored session draws
the same timing, and a session written *before the parameter existed* opens shut while its grid, swing
and seed come through. Layout is **109 → 115**: two from the bounds sweep picking up the new fader and
its % label, one drag-to-parameter through `checkBarFader`, and three in the new section — reachability,
a host-written 0.4 drawn back onto the fader *and* heard by the engine, and a double-click shutting
both. The default is 0.0 and short-circuits before any generator call, so `[28]`'s fingerprint is still
`4BC3AA231178C071`.

**Two harness faults, both found by going red rather than by being predicted.** [30]d's "same samples,
only later" came back red by 0.000415564 on a head sample 482 output samples apart, with the two peaks
also differing (0.602598 / 0.602140) — and the engine was innocent. `MasterBus` moves its trim through
a 20 ms one-pole that needs ~13 time constants to settle, and `arm()` set the trim *after*
`prepareToPlay`, which had already parked the smoother at unity; the two renders were comparing the
level at two different points on the same ramp. `currentGain` is set to the target only in
`setSampleRate`, so the globals now go in before the prepare, and the same gate reads
bit-identical: `worst sample difference 0.000000000 (no sample differs)`, and the level check
now prints its two peaks on the pass as well as the fail — `level: peak 0.601509392 on the grid,
0.601509392 late` (`out/smoke-43.txt`), because evidence that only appears when a gate is red
cannot be re-checked by anyone later. The limit was not widened. Second: [30]e and, latently, [30]d searched 1000 seeds
for a usable draw **with the knob shut**, where the function returns 0.0 for every key — the search
finds nothing, the dependent checks are skipped, and the run prints a green that measured nothing. The
knob is now opened before the scan, with the printed precondition (`darkDrawn >= 3 * watchBlock`)
next to it. Both are in AGENTS.md with the numbers. A third, smaller one: the gate script's
staleness guard scanned all of `Tests/`, so editing `SmokeTest.cpp` reported the *layout* exe as
stale — true of nothing, since neither suite compiles the other's main (and at the time
`Tests/PatternPlanTest.cpp` was compiled by no CMake target at all — **superseded 2026-11-16 by
`ecfe76c`**, which builds it and registers all four checks with CTest, so `ctest --test-dir
build-new -C Release -V` is now the tracked way to go red from a clone). The guard now takes each
target's own main plus `Source/**`; a guard that cries wolf on a correct build is a guard that
gets skipped.

**Nine arms, and each one bites** (`out/ab-43.ps1`, per-file backups, restore re-stamps
`LastWriteTime`): delete the draw → 9 failures; make the bound the whole step → 4; un-square the draw →
3; read the roll's bits for timing → 1 (r −0.8662); never spend the wait → 3 (`measured 0, drawn 482`);
light the lane at scheduling → 1; default the parameter to 0.5 → 13, which is the honest cost of a
non-shut default: the fingerprint, every sample-accuracy check and both grid checks go red because the
feature was not meant to be on; fader not refreshed on a host write → 1 BAD; drag not committing → 2 BAD
(`out/ab-43-report.txt`). Arm e needed a second run: its multi-line anchor was passed as unparenthesised
PowerShell concatenation, so the arguments came apart and the build died on a `{` instead of the mutation
running — the arms now parenthesise, and a mutation that does not match its anchor still refuses to run.

**One line no gate can see, recorded rather than faked.** `handOffToTail` clears `slot->startDelay`
instead of carrying it. A voice that is still waiting has not stepped its envelope, so what a steal or a
choke captures from it is silence at level 0, and a tail that then swallowed 482 samples would be
fading nothing — the fault is inaudible at every block size and every amount, so a check for it would be
a check that cannot go red. The comment says what the line is for (a reused slot inheriting the previous
tail's wait would drop samples it was told to fade); the claim is discharged by reading, not by a gate.

Docs: README gained **"How late a hit is"** (the travel table, why a quarter of a step, why squared, the
separate-bits argument, notes-you-play being excluded, and what it costs at 0 %), a feature-table row,
the timing fader in the top-bar list, a bullet in **How it is put together** on the two halves of one
hash key, three new Verifying bullets, and a limitation stating it is late-only and timing-only. The
parameter count moved 114 → **115** in every place that quotes it, including the sentence in the
installer README whose list had never mentioned Mutate or Seed at all. `installer/README.txt` got the
plain-text version, including the old-sessions-open-shut behaviour.

Run at the close of 4.3: **321 smoke checks / 0 failures**, **115 layout checks / 0 problems**, staleness
guard 0 on both, `Tests/RtPathGuard.ps1` exit 0, `Groovebox` target exit 0. No audio changed at the
default, and `launch/` was not refreshed — the "Live (PID 43876) holds the bundle" behind that was inferred from `tasklist` and was wrong (corrected at the end of this file).

Still open inside 4.3: velocity jitter (**4.3b**, now closed — see its record below), and the amount is
deliberately one number for the
whole machine — per-lane timing depth is not on the parameter list, and the two ends of the fader are
the whole design until someone needs the middle per lane.


### 4.3b — Touch: a hit struck a little off what was written (done 2026-11-16)

**What shipped.** A fourth global fader named **Touch**, in the Variation group beside Mutate
and Humanize, 0..1, default **shut** (`Params::touchDefault = 0.0f`), double-click to shut,
`Params::touchId` in `Parameters.h`, and a row in `Params::globalIds()` whose length is in its
type so a forgotten row is a compile error — not hand-copied into `allParameterIds`, which is the
trap that bit three times before and is now test [21]’s job: **116 parameters**, all written, all
reaching the field they name, and the table compared against what the host is offered. The engine
law is `SequencerEngine::struckVelocityFor`, and it is a **deviation**, not a redraw: the rung the
step stores is the centre, the draw is `((key >> 24) & 0xffff) / 65536`, squared about zero
(`away * |away|`), scaled by `Layout::maxTouchRatio` (0.25) and by the knob, and the result is
clamped **into the 7-bit rung lattice the payload stores** — 127 in, 127 out, never 0.63. That
last part is what keeps [28]’s baseline honest: `bit-identical at probability 1.0` is a machine
answer only while the law cannot invent a value the record cannot carry, and the gate that says
so runs on the cell deliberately drawn at 0.5 (63.5 of 127), the one rung where a law applied to
the float rather than the rung answers differently on the two sides of a write.

**Three readers, three slices.** `takeKey` is one splitmix64 per cell; the roll reads bits 40..63,
timing 0..23 and level 24..39, and the timing permutation is untouched — 4.3a’s
`maxHumanizeLateSteps` gate is the same code and the same limit, and [31] now proves a level knob
cannot retune it (`the timing bound is the same with the level knob open as with it shut`, and the
converse). Two checks were already there, one had to be invented:

- the correlations were **not enough**. Mutating the level law to read the roll’s slice left both
  green: `unit` is a 16-bit window of a 64-bit hash, so conditioned on a binary roll outcome it is
  exactly uniform (each value four times in [0, 2^23)) and the point-biserial r is 0.0000 by
  arithmetic. Reading the bottom 24 instead gives `unit_t = (unit_l + H) / 256`, measured
  r = 0.0039 against a 3-sigma limit of 0.0674. A correlation gate is blind to a wrong slice; all
  it can ever say is that two things are independent.
- so [31] part c now **names the bits**: re-derive the key in the test, predict the struck rung,
  the lateness in steps and the fire-or-not for all 4096 cells from the three slices as
  documented, and require zero wrong in each. On the two mutated builds it reports 3519 and 3547
  of 4096 disagreeing. `out/ab-43b.ps1`, 14 arms, second run: **14/14 red, restored green** — the
  first run is what found this (`out/ab-43b-summary.txt` shows c and d green;
  `out/ab-43b-summary-2.txt` does not).
- gates follow the shipped philosophy, not 4-sigma-of-a-coin. The cross-seed stability limit is
  the worst departure among 47 *other* seeds with the tested seed deliberately outside that
  sample, compared against the engine’s own formula re-evaluated: `seed 1234 sits 0.00256 from
  centred; the worst of 47 neighbours sits 0.00705`. Nothing rounded, nothing typed.
- the one seed-hunting arm has the knob set **before** the loop and prints its precondition:
  `precondition: Touch open for the search; seed 0 strikes rung 80 -17 steps off it`.

**What it costs, measured and left in.** Same seed, same −3.5 dB trim in both arms, four bars of
the whole 64-rung ladder: rms 0.0416536 → 0.0416878 (**+0.0071 dB**), 2..7 kHz 0.000412934 →
0.000411705 (**−0.0259 dB**), peak 1.438488 → 1.485904 (**+0.28 dB**). The peak rise is not a gain
and is stated as what it is: four voices at one step add together, and three of them pulling up
beats a shut step whose loudest rung is only one. Asked of **one** voice — the rung-127 hit, where
monotone gain and monotone tone make `<=` arithmetic rather than statistics — the direction is only
ever down: peak 0.601509 → 0.577455, 6 kHz 0.00000835 → 0.00000798. No limiter, no hidden trim,
nothing bought back. The level-matched claim is the bit-exact one: a jittered hit is the same
samples as the hit *drawn* at the level the draw said, at 64, 256 and 1024 blocks.

**The clamp is the asymmetric part and it is counted, not widened away.** Half the knob should be
half the departure, and 101 of 4096 keys disagreed — by one step (the rounding bound is exactly
one, so the gate now compares `round (d)` with `round (d / 2)` in steps) and by the ceiling (a rung
near 127 has its positive departures cut off, so both arms land on 127 and are *equal* rather than
halved). The exemption is printed and held under a quarter of the take, paired with the weaker
claim that survives it: opening the knob never moves a hit back towards its rung.
`37 held, 27 struck lighter` at the full rung, and `the full rung shows the clamp biting and not
only existing`.

**Migration.** A session written before the row existed opens with Touch **shut**, not at whatever
the last take used, and keeps everything else it did store — the same `copyXmlToBinary`-shaped
fixture as 4.3’s, with the new row deleted out of a saved state and what is left loaded.

**5.6 pulled forward: the row wraps.** The third fader is 154 px of row, and the alternative to a
rule was a floor that moved again (1678 → 1832 was already twice). So: `TransportBar` now owns
`variationBandWidth / oneRowWidth (1774) / wrappedRowWidth (1182) / oneRowHeight (60) /
twoRowHeight (96)` and `isWrappedForWidth / heightForWidth`; `layoutVariation()` was extracted so
the same code lays the group out on either line; the editor asks `heightForWidth` with the width it
is about to hand over and takes its floor from `wrappedRowWidth + margin * 2` = **1218×844**; the
grid pays 36 px at the narrow end (405 → 369). The default is still 1832×844 and is now documented
as a default rather than a floor. *(Superseded the same day on the height axis: 844 was still the
default pasted in as a minimum. `Layout::editorPlan` prices the minimum instead, and the floor is
**1218×695** — the width half of this row is untouched, the 36 px the grid paid is now its own
361 px floor, and the 5.6 entry above has the gates.)* Widening past 1832 was rejected: the cost lands on the grid, which
is the thing being looked at while the faders are not. *(Rejected, and then spent — 4.5 put a fourth control
in the row and there was nowhere to take 80 px from that was not a control: `oneRowWidth` is **1854**, the
default **1890×844**, and the editor floor **1298×695**. Two of the three numbers in that sentence moved, and
the grid paid none of it — the wrap rule is what grew, so the cost is 80 px of window width and the floor moves
by exactly the same 80 because it is `wrappedRowWidth + margin * 2`, derived. The 4.5 entry has what the control
is and why nothing else gave way.)*

**The layout gates that were false greens, and what each one asks now.** Six sizes × six claims.
(i) The first version compared `height` with `heightForWidth (getWidth)` — the same function
`resized ()` had just used, so it proved only that the editor and the bar read one rule, and stayed
green on `return false`; the arm that bites states the numbers (`oneRowHeight` at `>= oneRowWidth`,
`twoRowHeight` below), which is also the only thing that catches `<= oneRowWidth` at all. (ii) The
rule is about **bar** widths, not window widths: the first sweep laid out at `1774` and wrapped
inside a 60 px band, printing 14 problems. The test measures `barInset` off the running editor and
the editor asks with `area.getWidth ()`. (iii) The hover readout check asked only about the top edge
and passed on a **1 px tall** rectangle — `Rectangle::setTop`/`setBottom` move an edge and *resize*
in 8.0.4 — so the helper now slides with `jlimit` on the far corner and the gate wants all 192 cells
inside, 11 px tall and at least 22 wide. (iv) The strip walk is all twelve strips at every size,
quiet, with one printed exception, and it says `no strip is short enough to need the tight rule`
because that rule is broken and unreachable: probe at 1832×500 → 236 px strips, 2 of 17 controls
outside, spare 0, 12 BAD lines (`out/layout-tight-probe.log`). That is what is left of 5.6.
*(Superseded the same day by 5.6, above — and the probe arm was read correctly here, it just
understated the defect: `219` was not a band nobody reached, it was a threshold **no** strip
height satisfies, because the compact set cost 228 and was only ever chosen below 219. The rule
is now `Layout::stripPlan`; the walk was eight sizes then and is **nine** now, both probe arms
re-based on 178 px and 169 px when the height half moved the floor to 695;
and `spare` is the plan's own deficit rather than the leftover of a clamping rectangle, which is
the specific reason this sentence's `spare 0` could never have gone red.)*

**Run.** `ctest --test-dir build-new -C Release -V` → **4/4**: 365 smoke checks / 0 failures, 374
layout checks / 0 problems, 85 planner checks / 0 problems, `GUARD OK`. Both harnesses exited 0 on
their own, `Tests/RtPathGuard.ps1` exit 0, the staleness guard in `out/run-gate.ps1` clean, no
C4456 left in `LayoutTest.cpp`. Docs: README gained *How hard a hit is actually struck*, the
feature-table row, the 116 count in both places that quote it, the three-slice bullet, the wrap
sentence in **Layout**, the Touch bullet in **Verifying**, the planner suite in the test inventory
and a limitation saying Touch is level-only and a deviation; `installer/README.txt` the plain-text
version; AGENTS.md the three bullets (a wrong slice, bounds needing a size, a gate that may not
borrow its own predicate) and the CTest correction the `ecfe76c` handover asked for. **Not done
here, on purpose:** a feature commit ships no export — `launch/` and the export hashes AGENTS.md
quotes belong to the shipping lane, and whoever builds it refreshes those hashes against that
build’s SHA; the genre presets are still undocumented in `installer/README.txt`, which is 4.4’s to
fix; and 4.4 itself stands on its own.

### Correction — the "`launch/` waits for Live" claim was an inference, and it was wrong (2026-11-16)

Three run records above (4.1, 4.2, 4.3) say `launch/` still waits for Live to be closed, and 4.3's
names PID 43876. This lane wrote that from `tasklist` output alone: AGENTS.md says a host with the
plugin loaded holds the DLL open, so *seeing* Live was reported as *a lock*. It was never measured as
one, and the shipping lane disproved it two ways — `(Get-Process -Id 43876).Modules` enumerated 2
`.vst3` modules with no Groovebox among them, and under `CloseApplications=no` a Setup that ends
`Installation process succeeded` can only have found the bundle unlocked. The state, re-hashed here
rather than recalled: `tasklist` returns no Live process; `out/planbuild/install-b603107.out` records
`HOST PROCESSES: 0`, `SETUP EXIT: 0` and success at 04:15:50; and `launch/` and Program Files hold the
same two files, standalone `a3948e47b34a…` and bundle binary `cf3f8318c1bc…` — i.e. the machine is
already running the `b603107` payload, which `4724922` documents. Two consequences: a release base of
`4e6a7ef` would ship a build six commits back, without Humanize (`6439f0b`) or Touch and the row wrap
(`b603107`) — the likelier cause being `4724922` reusing `4e6a7ef`'s exact subject line; and since
`b603107..71c04fd` is docs-only (`AGENTS.md`, `NEXT-STEPS.md`, `README.md`), so the artefacts in
`launch/` are byte-for-byte what a build of `71c04fd` produces. The rule that was missing from
AGENTS.md and is now in it: `tasklist` says which process *might* hold the bundle, module enumeration
says whether it does.

### 4.4 — four patterns per lane and a bar counter (done 2026-11-16)

The plan asked for *"4-slot pattern bank per channel (four pattern strings on the tree, no file browser),
with a bar-counter evolve option. ~6 h."*, and that is what shipped: no browser, no song mode, no slot
change the host can automate. The genre presets that 4.3b left undocumented in `installer/README.txt`
were said here to be folded in, since the bank is what they finally have somewhere to go — **that clause
did not happen, and was measured against this tree rather than against the sentence.** `0c066ad` adds 47
lines to `installer/README.txt` and none of them name a genre; `grep -c -i genre` there is 0 and has been
since, `README.md` describes the genre *table* and its test but lists no preset, and `PatternPlanner` still
has zero callers in `Source/` at `3c3f059`. The presets have neither a document nor a destination until
item 14 lands, which is the honest state and is what `README.md`'s limitations row now says.

**The law, in one place.** `Layout::numPatternSlots` is 4 (`Source/Common.h:34`) and slot 0 is the grid that
already existed: the property is still `pattern`, with the same bytes, so a session written before the bank
opens as one lane, one pattern, pointed at slot 0, counter shut. The other three are **sparse** —
`slotPropertyName` (`Source/PluginProcessor.cpp:1245`) names them and `slotCarriesAnything` (the next function,
at `:1252`) decides whether they are on the tree at all, so an empty slot costs a session nothing and the
census a test can read is *which names are present*, not a count. Which slot a bar sounds is
`SequencerEngine::slotPlaying` (`Source/SequencerEngine.cpp:277`): it starts at the lane's selected slot and
walks **forward over the filled ones only**, so two filled slots of four alternate those two rather than going
quiet for the other two, and one filled slot never rotates at all. `isSlotFilled` (`:208`) is the one answer
to "does this slot hold something" and is shared by the dot, the rotation and the copy rule — the engine's own
fill test used to be a width mask over the first cell, and the comment at `:247` records why that is a second
answer to the same question rather than a faster one. The rotate ladder lives in `Layout::evolvePeriods`
(`Source/Common.h:48`, `{ 0, 1, 2, 4, 8 }`) and **belongs to the control, not the law**: `slotPlaying` divides
by whatever the state holds, so a period the panel never offers still rotates evenly — which is a check, not a
hope (`[32]`).

Three helpers went into `Source/Common.h` for the same reason: `levelByte` (`:340`), `chanceByte` (`:345`) and
`cellSaysAnything` (`:359`). The velocity mark, the probability copy and the save path all need "what does
this cell store, and is it worth storing"; that sentence written three times is how a corner value quietly
retunes a kit, and it already bit once this year with `allParameterIds`.

**The panel.** Each lane's label column carries a strip of marks (`getSlotDotBounds` at
`Source/StepGridComponent.cpp:270`, hit-tested by `bankTargetAt` at `:309`, dispatched at `:873`): four slot
marks — solid when filled, outline when not, a white ring on the selected one and a dot inside a mark when the
sounding slot is not the selected one — then a copy mark, then the rotate chip. Copy duplicates the row into
the next *unfilled* slot starting after the selected one and moves the pointer there
(`GrooveboxProcessor::duplicateChannelRowToNextSlot`, `Source/PluginProcessor.cpp:683`, which ends by storing
the new pointer at `:722`); it is wrapped in `beginTransaction ("Duplicate row")` at
`Source/StepGridComponent.cpp:871` so one undo takes the copy and the pointer back together, and a half-copied
bank cannot be a thing. `cycleChannelEvolve` (`:615`) walks the ladder and wraps back to off,
because off has to be reachable by the same gesture that turns it on.

**An edit writes the ring, never the sounding slot** — with a period above one bar those are different
patterns, and a lane the user is editing must not change under them because a bar boundary arrived.

**It is state, not a parameter.** `slots` and `evolve` are tree properties on the atomics (`pushBankToState`
at `Source/PluginProcessor.cpp:1528`, `applyBankFromState` at `:1560`, gated by `isGridProperty` at `:1583`)
and they are deliberately **not** in the 116-parameter table, so the host cannot ramp a slot change and Live's
arrangement cannot draw one. The README says so next to the limitation about no song mode, and the reason it
stays that way is that a slot is a *pattern*, and the other patterns — the grid itself — are not automatable
either. Automating one and not the other is the inconsistency someone finds mid-take.

**Run.** **422 smoke checks / 0 failures** (`out/smoke-44-final.txt`), **441 layout checks / 0 problems**
(`out/layout-44k.txt`), **85 planner checks / 0 problems** (`out/plan-44-final.txt`). The bank's own half is
57 checks inside `[32]` and 67 inside the layout suite's *bank strip in the label column* block.
`[28]`'s fixed-kit fingerprint is `4BC3AA231178C071`, unchanged: **the bank costs nothing in level**, which is
the only audio claim this step makes and is a machine answer, not a listening one. Rotation is asked as a
function of the bar number alone, and the same bank prepared at a 1024-sample block and at a 256-sample block
sounds the same slot on the same bar — block-size dependence is the class of defect this suite was blind to
for four phases, so it is gated here rather than assumed.

**Negative controls.** Ten rounds, 47 arm runs, **49 of the bank's 49 checks killed, 0 survived every arm**
(`out/killed.txt`: *49 checks in the bank section, 61 distinct BAD lines across 47 arm logs*). Four findings
decided how the arms had to be built, and they are in `AGENTS.md`:

- a dead pixel in that strip is refused **three times in series** — `channelAtY`'s sliver rule,
  `bankTargetAt`'s strip guard, then each control's own rectangle — so any one guard hides an arm, and the
  panel has to be broken at each layer to show that layer bites;
- a colour claim is read as **absolute colour**, not as a render diff, because an arm that moves the palette
  moves both sides of the comparison;
- a rotation mirror means nothing until **both lanes hold different slot numbers**; with two lanes on the same
  slot, every wrong answer is correct;
- a parity arm (`% 2` instead of the ladder) is **invisible to a determinism check**, because one render calls
  `getSlotDotBounds` 48 times and the averages hide it — the gate has to name the period.

**Docs.** README gained the feature row, the persistence row, the *Four patterns per lane* section and the
plain *How the pattern bank works*; `installer/README.txt` the plain-text mirror; and the limitation that read
*"One 16-step bar; there is no pattern chaining or song mode yet."* is replaced by the honest one — four grids
and a fixed period, still no arrangement, and nothing that carries a pattern out of the plugin. Both were
re-read against the code, not against each other.

**The citation sweep, and what it could not do.** `out/cite-sweep.py` renumbered 43 doc citations from
`bcd6188` onto this tree, and a second pass finds nothing — `to correct: 0`, `unresolvable: 0`.
`out/cite-audit.py` then asked the stronger question, does the cited line sit in the symbol the sentence
names, and returned 15 MISSes, every one of them a citation that was **already wrong at the base**
(`bcd6188`, which was HEAD when this was written), in the sections written from the `460b3bf` audit.
Shifting those would have made a bad pointer look renumbered, so they are pinned — restored to the base's
text, with the reason in the pin list — and their verified current
locations are recorded as **6.6**. The audit's gate is now `UNPINNED miss: 0`. One case is the argument for
running both: section A3's `.load()` pointer had drifted from `SequencerEngine.h:309` — right at the baseline
its own section names — to line 373 of that file, through this very sweep, which is correct arithmetic on a
number that was stale where it started. It is back at 309, and 6.6 says where the read actually is.

**And the sweep's own baseline moved under it, on the first commit.** Re-run after `0c066ad` landed, over a
tree nothing had been edited in, the same two commands answered `122 already right, 2 to correct` and
`UNPINNED miss: 13` instead of `43 / 0 / 0` and `miss: 0`: every pin range in `out/cite-pin.txt` was written in
HEAD's line numbers, and this lane committing its own docs had made HEAD a different tree. No error, no
warning — the ranges still resolved, at the wrong paragraphs. `out/cite-pin.txt` now names its baseline
(`# base: bcd6188`), `out/cite_pins.py` is the one reader of that file for both scripts, and every label the
runs print interpolates it (`edited-since-bcd6188 lines left alone`) so a report cannot say HEAD while
reading another tree. Both directions were measured: with the base put back to `HEAD` the 13 misses return,
with it left at `bcd6188` the pair is green. The rule and its reason are in `AGENTS.md`'s citation section;
what belongs here is that the check passed for months on a name that was only ever accidentally stable.

**Not done here, on purpose.** `launch/`, the installer and post-install verification stay with the shipping
lane, and the export hashes `AGENTS.md` quotes belong to that lane's build rather than to a feature commit.
Item #14's three planner/panel contracts are the owner's to decide and were not invented here. And the bank
has no check that renders *across* a slot change with a long release: what protects a rotation that lands a
new bar's hit on the previous bar's tail is 3.1's steal hand-off, which is general and was not written for
this — the gap is stated under section A's table instead of being glossed over.

### 4.5 — one hit, several strikes (done 2026-11-16)

The plan offered two routes and made one of them conditional on its own precondition: *"This
rewrites the three onset functions into (step, subslot) and is the highest-risk item in the plan
because it touches the split loop repaired in 2.3 — do it only after Phase 2 is a week old and
stable."* Phase 2 landed this week, so the keystone stays deferred and what shipped is the
alternative: `Voice.startDelay`, the countdown 4.3a built for humanize, spread one hit over the
space its own step owns. No onset function was rewritten, no split loop was touched, and the
clock, the playhead and the swing grid are exactly where 4.4 left them.

**The law, in two functions and one conversion.** `SequencerEngine::gapToNextOnsetInSteps`
(`Source/SequencerEngine.cpp:140`) answers how far the *next onset this grid will actually play*
is from this step, in steps — so it already knows about the swing grid and about which division
the row is on. `strikeDelayInSteps` (`:166`) is `gap * strike / strikes`, and strike 0 is always
zero, which is what makes a roll additive on top of the humanize offset rather than a second
opinion about timing. Both halves go into one rounding at the one call site (`:830`,
`std::lround ((late + strikeDelayInSteps (...)) * samplesPerStep)`) because a rule that rounded
the wait and the lateness separately could land a strike on either side of the sample the grid
means, and the seam the tests ask about is the sum. The wait itself is spent in
`renderVoice` (`:1093`) before a sample is read or the envelope is touched, so it survives any
block size for the reason 4.3a recorded.

**It follows swing because it follows the room, and both are measured, not derived in the test.**
On the straight 1/16 grid step 4's room is 5513 samples (1.0001 steps — the ruler arm, so the
eight-strike arms are not measured against a broken ruler). At the shuffle the head of a pair
holds **8269** and its off-beat **2756**, ratio 3.000, and the same eight strikes space
themselves **1033.6** samples apart in the first and **344.6** in the second. An eighth of the
straight room is **689** samples, 15.6 ms. That is the argument for a fraction of a step instead
of milliseconds, stated as numbers: a fixed delay would play the off-beat's roll three times
slower than its own grid. And the ceiling holds where it is tightest — eight on the *smaller*
room still clears the next onset by more than a rounding, so eight cannot become nine by leaning
on the step after it.

**One hit for every other law the engine runs.** The probability draw is keyed once per
(bar, step, lane), so a rejected step sounds nothing at eight strikes (measured arm: a bar that
kept 10 steps and dropped 6 at half chance, with the dropped cell rolled to eight and
returning zero onsets); the humanize draw is keyed once too, so the whole roll carries the one
lateness (the take said step 4 is 0.0435 steps late, the unrolled hit starts at 22050 and the
rolled one at 22290 — 240 samples, both, and the seven intervals stay within two samples of even
with the knob wide open). Only **strike 0 fires the choke group** (`StrikeChoke::apply` /
`.skip`, `:834`): a rolled lane keeps all eight of its voices inside a group, because a hit
cannot cut itself up, and a *different* hit in that group is still cut (the held lane under it
goes 0.41241 → 0.00000). A MIDI note is not rolled at all — `trigger` is called from the note
paths with its defaults (`:642`, `:695`) and only the step loop asks for a count.

**The level cost is arithmetic, and it is stated rather than bought back.** There is no
per-strike gain scaling: eight strikes are eight voices playing the sample. On one lane, one
step, full velocity and chance, channel level 0.12, on a noise bed with decay shut and sustain
at 1 so the voices genuinely overlap, at the default trim, the tail window reads 0.04951 for one
hit and 0.13897 for eight — **×2.81, +8.97 dB**, against √8 = 2.83 for eight incoherent voices.
Same roll inside a choke group: 0.13897 again, so the group costs no level. That fixture peaks at
0.5385 and the render stays under −1 dB, which is why the check can call the growth summation and
not the bus folding. There is still no limiter, and the trim is the user's. A whole kit rolled at
once — 12 lanes × 8 at every step — is 96 hits a bar against a pool of **64** voices: the pool
fills to exactly 64, never past it, nothing goes non-finite, and the densest roll steals rather
than overrunning.

**State, and the fourth byte.** The step record is generation 4: stride 4, 768 bytes a slot,
1027 characters of payload (`[27]` prints the width it wrote). `Layout::maxRatchetHits` is 8
(`Source/Common.h:305`) and `Layout::ratchetByte` (`:410`) is the one clamp, applied on the way
in, on the way out of the codec, and again where the engine reads the atomic — so a byte that
meant 60 costs eight strikes, not sixty, and a hand-edited preset cannot ask for a hundred
voices. A record with no fourth byte — every session written before this — reads as one strike
everywhere, which with off / full / every-time is exactly the state a missing record already
meant, so `cellSaysAnything` gained its fourth term and the sparse encoding still writes nothing
for it. One strike at the engine is one turn of a loop whose delay is zero, which is why the
**[28] fingerprint `4BC3AA231178C071` is a gate on this change rather than a hope about it**: it
did not move.

**The panel: the third overlay, and 80 px of bar.** **Ratchet** joins **Velocity** and **Chance**
as a hand-made exclusive trio (`TransportBar::handleRatchetClicked`,
`Source/TransportBar.cpp:699` — JUCE's radio group cannot express "neither", and all three
overlays *can* be off). The shared reset now does three fields and says which: `x1` over a strike
count, `100%` over the other two, with the tooltip moving with it. A cell carries one tick per
strike on the axis the rest of the grid already reads as time — `paintStrikePips` under
`strikePipsFit` (`Source/StepGridComponent.h:115`), which is the same arithmetic the drawing does
rather than a second copy of it; the resting grid marks a rolled cell with a 2 px cut at its top
edge (`restingStrikeMarks`, `Source/StepGridComponent.cpp:481`) and nothing at all on one strike,
on an off cell, or under an open overlay. Adding the control moved `oneRowWidth` 1774 → **1854**
and with it the derived floor 1218 → **1298** and the default 1832 → **1890**; nothing else gave
way, and the 68 × 27 px cell the floor now measures was verified to hold all eight ticks at the
published minimum.

**Two defects found by reading, not by a red test.** The reset's `x1` label was *promised by a
comment that had no code under it* — the button said "100%" over a number of hits, which is a
button describing something it is not going to do. And the overlay's ticks were invisible at the
velocity a fresh session draws by default: a tick painted in the channel's own colour on a cell
filled by that same colour is not a tick, so every cell of a new session showed *nothing* in the
one overlay whose whole job is to show a count. Both fixed; arm **G** below exists because the
second fix is load-bearing, not decorative.

**Gates, and what each arm killed.** SmokeTest **[33] "one hit can be several strikes"**, six
arms (a ruler, swing, choke, the three other laws, the pool, the clamp and the reset), plus two
travel gates in `[29]`'s field-travel block — that a row shift carries the strike count with the
step it belongs to, and that it turns only the slot the lane points at — for **465 checks,
0 failures**, `[28]` unchanged. LayoutTest: the variation-control block rewritten to look
every control up **by name**, six directed exclusivity pairs, a drag arm, an eleven-position
monotonic walk, a reset-and-undo arm, and a new pixel section that counts ticks out of real
renders — **651 checks, 0 problems**. Nine panel falsification arms (`out/ab-45.py`, log
`out/ab-45.txt`) and two state arms over `Source/PluginProcessor.cpp` run against the smoke suite
(`out/ab-45b.py`, log `out/ab-45b.txt`), each restoring the recorded good file before it mutates,
and the restore hash-checked against `out/*.45.good` and `out/PluginProcessor.cpp.45b.good` with
a green re-run after: A the ratchet handler stops
closing Velocity → 1 problem; B the label never follows the overlay → 2; C the reset stops doing
strike counts → 1; D the control is unnamed → 1 problem **and 629 checks instead of 651**; E the
ticks under-count (`jmin 4`) → 3; F the neutral value gets marked → 2; G the recess goes → 1; H
the mark draws itself under an overlay → 2; I the roll drag writes chance → 4; J the shift drops
the fourth store → **2 failures, and they are exactly the two new travel gates**; K the shift
turns the slot next to the selected one → 12 failures, the named carrier among them.

**Why J and K exist, and what they say about a travel gate.** 4.1's own record said the shape out
loud: *"`shiftChannelRow` taught to carry every per-step field it now moves" is gated rather than
refactored … what a new field will still need is a line in the two loops, and the turn-and-save is
where a field that stays behind gets caught.* It was not caught here, because the block that has
played that role since 4.2 writes the roll **after** the shift — so a missing fourth store left
every check in the file agreeing. A travel gate only travels a field that was already on the cell
when the move happened, and arm J is what proves the line is now load-bearing: drop the store and
the only two checks that move are the two written for it. **K** is the other half of the same
field: the grid paints the selected slot and nothing else, so a shift that turns a different take
of the same lane is invisible on screen *and* invisible to every check that only ever reads the
slot it edits — the gate reads the slot the shift is not supposed to touch.

**Arms A and I did not bite the first time, and the reasons are the useful part.** A removed the
line that closes Velocity when Ratchet opens, and the suite came back green at 651. The helper
that claims to test exclusivity shut *all three* toggles before opening the wanted one — so when
it clicked Ratchet there was nothing lit to fail to close, and all three arms were green on a
build that had the exact bug the rule exists to prevent. Open one rival first, then the wanted
overlay on top of it, and do it for both rivals: six directed pairs where three had been. The
second version reports "bar lit **2**, grid modes 1" — the bar lights two buttons while the grid
edits one field, which is the lie the rule was written against, and which is now visible in the
log instead of inferred. (Its sibling defect was in the *report*: the failure message recomputed
the counts after the loop had already run the next pair and cleaned up, so it printed "bar lit 1,
grid modes 1" beside its own failure — a number that says nothing is wrong under a sentence that
says something is. The counts now come from the pair that broke.) **I** skipped for a different
reason, and the duller one: the anchor was indented for a nesting level the real `switch` does not
have, so the arm mutated nothing and reported nothing. An arm that cannot apply and an arm that
applies and cannot bite both look like a suite that passed; the driver prints SKIP for one and a
count for the other, which is why the log line beside every arm is its *problem count*, not just
its exit code.

**Deliberately not here.** No (step, subslot) onset keystone — deferred on its own precondition,
and this feature did not need it. No per-strike accent or velocity taper: the count is the only
new number, and a taper would be the engine deciding a level the grid already says. No rolls from
MIDI, no rolls as a host parameter, no autoplay of the count from **Mutate** — the count belongs
to the cell, like the level and the chance, and **Reroll** does not re-deal it.

**What the citation sweep did, and the nine numbers it refused to move.** 4.5 put lines into seven
`Source/` files, so every pointer into them that was true at `9a7d942` reads false now — 62
citations flagged, and a doc that cites `Common.h:446` for `stripPlan` while 446 is a sentence
about the editor's floor is worse than one with no pointer, because it is trusted.
`out/cite-sweep.py -apply` (base `bcd6188`, the tracked tool) offered nine corrections and every
one of them was declined after reading the code it landed on: 93 → `setHumanize`/`getTouch`,
176 → `distanceToNextOnsetInSteps` for a sentence about voice stealing, 290 → `chokeVoicesInGroup`
for a sentence about kernel weights, 1066 → `loadSampleDataIntoChannel` for "is the codec". Those
numbers were already wrong *at the base* — the same condition `out/cite-pin.txt` records for its
fifteen `unverified` entries — and the sweep's guarantee is text identity, which preserves a
mistake and stamps it current. The docs came back off `out/pre-cite/` byte for byte.
What went in instead is per-citation: each candidate was opened both at the number the doc gives
and at the number a HEAD→working map proposes, and decided by which one holds the thing the
sentence names (`out/cite-head-check.py` to enumerate, `out/cite-judge.py` to print both sides,
`out/repoint45.py` to write the 23 decisions, each matched once on the line it names). The gate
`out/cite-audit.py` now prints **UNPINNED miss: 0**; it printed 5 before, and all five were
symbols this lane moved.
Some of the refused shifts are the inverse trap and worth naming: `Common.h:305`/`:389`,
`StepGridComponent.h:115`, `SequencerEngine.cpp:140` and `TransportBar.cpp:699` are *already*
current-tree numbers, written by this lane, and a map keyed on HEAD moves them a second time —
which is the failure mode `cite-sweep.py`'s own docstring records from the 4.4 sitting, arrived at
by a different road. A fifth of them, `StepGridComponent.cpp:468` for `restingStrikeMarks`, has
since moved for real: 5.3 put the audition above it and it is `:481` today. That is the honest
asymmetry in a refused shift — refusing a move is a verdict about the tree as it stands, not a
pin, so the next lane has to re-check the number rather than inherit the refusal. `:389` has since moved for real, and it is `Common.h:410` today: 14a's two chance helpers went in above it, which is what re-checking rather than inheriting is for.
And one correction was not a move
at all: 5.6's record says "the strip is 285 px today, `PluginEditor.cpp:47`", and 47 is the
editor's own initialiser list at every recent commit — the number is `Layout::stripDesignHeight`,
`Source/Common.h:561`, and the sentence says that now. Wrong the day it was typed, not drift, and
the difference only shows up if something reads the line rather than shifting it.
`cite-pin.txt`'s `base:` stays at `bcd6188`: the seventeen ranges under it are coordinates in
that commit, and advancing it means re-checking every one in the same pass — a 23-pointer fix
against the working tree does not need it.

### Verification/export handover — the `b603107` records, three corrections, and what a rebuild hash can say (2026-11-16)

**What the export lane sent, and what it is evidence of.** Run records for the shipped commit `b603107`
(Touch + row wrap), built detached at that SHA, nothing dirty, per-target staleness guard `STALE=0` on both
exes: smoke **365 / 0 failures**, layout **374 / 0 problems**, planner **85 / 0 problems**, RT-path guard OK
(84 `parameterChanged` lines from `PluginProcessor.cpp`, exit 0), `ctest` replay 4/4 exit 0. Payload:
`Groovebox.exe` 7,095,296 `A3948E47B34A4ECE`, bundle binary 7,195,136 `CF3F8318C1BCD6D2` — the two values
`AGENTS.md` quotes. Setup 5,060,477 `36EBB607509A57E7`; install exit 0 at 04:15:50 with
`Administrative install mode: Yes` and `Install mode root key: HKEY_LOCAL_MACHINE`, VST3-folder census
1/0/0/0, installed `README.txt` byte-equal to `installer/README.txt`; and re-checked live rather than read
from a log — installed bundle == `launch/` bundle == `CF3F8318C1BCD6D2`, exe == `A3948E47B34A4ECE`.
`6439f0b` (Humanize) was green before it (321/0 smoke, 115/0 layout) and is an ancestor of `b603107`, so its
code is on the machine too.

**Correction 1 — the release base is an ancestor, and the direction I had it in was inverted.** Measured
here: `git merge-base --is-ancestor b603107 master` → yes, and likewise `4e6a7ef`, `6439f0b`, `10fabec`,
`4724922`. `git rev-list --count b603107..master` = **6**, `4e6a7ef..master` = **10** — the export lane reads
7 for the latter because `10fabec`, `bbd8cb1`, `fbf973c` and `bcd6188` landed after that count was taken.
Which is the lesson worth keeping: `rev-list A..HEAD` counts *descendants* of A, which is exactly why the
release commits appear in the set, and a bare count quoted between lanes is a snapshot of one tip that starts
lying the moment a lane commits. Name the pair — `b603107`, 6 back from `bcd6188` — never the number.

**Correction 2 — `428E5DDA…` is not somebody's stale log; it is an accurate manifest of a retired artefact.**
It appears in exactly one place in either tree: `out/launch-pre-75fafb3/FROM.txt`, "bundle binary sha256:
`428E5DDAE08A4C48A113F477D8787420BE9925FB2A8C9CF1E15C1B20A4203FB8`", written 00:52 when `launch/` was
refreshed to the `75fafb3` build. Verified just now: `sha256sum` of
`out/launch-pre-75fafb3/Groovebox.vst3/Contents/x86_64-win/Groovebox.vst3` is `428e5ddae08a4c48…` — the
record matches the bytes it describes, so nobody mis-measured it. What mis-led was scope: a gitignored folder
whose manifest's first word is "Outgoing", read later as a current census. Housekeeping already says a
retired artefact keeps its hash; the missing half is that a superseded record has to say *outgoing* in the
tracked carrier too, or the next reader re-derives a wrong install census from a true old one. Not folded into
the run records above as the shipping lane's error, because it is not one.

**Correction 3 — right conclusion, wrong mechanism, and the real mechanism makes the check stronger.**
The claim was that a relink of identical sources cannot reproduce the installed bytes because `/RELEASE`
folds the debug-directory hash in. Tested on this toolchain at its cheapest target — the std-only planner
suite, one second to build: exe A `8b415ba5f896256b`, COFF stamp `1794803856`; then `touch
Tests/PatternPlanTest.cpp` (content unchanged — `git status` clean afterwards) and the same target rebuilt →
exe B `38fbcdd697badb59`, stamp `1794817406`, 13,550 s later. Identical length; **4 differing bytes**, at
`0x108-0x109` and `0x2a394-0x2a395`, which parse as the COFF File Header `TimeDateStamp` (`e_lfanew` = `0x100`,
+8) and the one `IMAGE_DEBUG_DIRECTORY` entry's own `TimeDateStamp` (entry at `0x2a390`, +4). The PDB Guid and
every byte of code are unchanged — so it is not `/OPT:REF` hashing anything, it is two clocks. Zero those two
fields in both files and the hashes come out equal (`e2ec30d0fdf48320`), on identical sources. The rule that
follows is sharper than the one sent: a rebuild-vs-install mismatch confined to those four bytes is a link
time and proves nothing about the code, while a mismatch *larger* than that means the sources really do
differ — so the comparison is usable across builds, not abandoned. Which is also why the same-build check in
`AGENTS.md` (export hashed against the artefact tree, per file) is the one that passed and stays the gate.

**No UAC click outstanding — true at `bcd6188`, and it stops being true when 4.4 is folded in.**
`git diff --name-only b603107..master` is `AGENTS.md`, `NEXT-STEPS.md`, `README.md`: nothing under `Source/`,
`Tests/`, `installer/` and no `CMakeLists.txt`, so the machine already carries the tip's code. `0c066ad`
(4.4, parent `bcd6188`, fast-forward clean, `lane-44` points at it) changes six `Source/` files, both suites
and `installer/README.txt`, so once it lands, Program Files holds `b603107`-era code while the tree holds 4.4
— and the payload should be re-hashed against that lane's own new artefact tree, with the user answering the
prompt. Numbers for that verification: 422 smoke / 441 layout / 85 planner checks, 0 problems, guard OK, `[28]`
fingerprint `4BC3AA231178C071` unchanged. *(Five commits on from `bcd6188`, and the same sentence is true one
level up: 4.4 landed, 5.6's two halves and the two `AGENTS.md` corrections landed, and 4.5 is what this lane is
handing over now — so Program Files is behind the tree by every `Source/`-changing commit since `b603107`, with
4.4, 5.6 and 4.5 among them, and the payload has to be re-hashed against this lane's artefact tree. Current
numbers for the next verification: **465 smoke / 651 layout / 85 planner checks, 0 problems, guard OK**, and
`[28]`'s fingerprint is the same `4BC3AA231178C071` — 4.5 was required not to move it, and did not.)*
*(Later again — 5.6's height half, 4.5, 5.3 and the 14a seam have all landed in this lane since that
sentence: **546 smoke / 675 layout / 85 planner checks, 0 problems, guard OK**, and the same
`4BC3AA231178C071`. Program Files is behind the tree by two `Source/`-changing commits after the `3c3f059` —
export — `ba7c953` and `8e49944` — and closing that gap is the owner's install step.)*

**Still open, and whose.** Item 14 stays with the owner: `PatternPlanner.h` and `GenrePatterns.h` still have
no UI or state caller, only the planner suite — the three planner/panel contracts are decisions, not work to
invent under commit pressure. 5.6's height half, 4.4 and 4.5 stay in this lane. *(Written before any of the
three landed; all three have now shipped from it, and the lane's open list is 6.5's false spec sentence and
nothing else it owns. 14a has since landed the one seam that item 14 named as its own prerequisite -- `writeSlotRows` -- so the planner has a door and still no caller: what is left of item 14 is the wiring and the three contracts, and both stay with the owner.)* The duplicate subject line
(`4724922` reusing `4e6a7ef`'s text verbatim while containing two lines of `AGENTS.md`) is recorded under
`AGENTS.md` "Commit log" conventions, with the replacement subject the export lane asked for. Measured there
before recommending anything: the rewrite would be *local* — `origin/master` is `ecfe76c` and nothing from
`b603107` up is published — but it moves 5 descendant SHAs on `master` plus this branch's 2, and `bcd6188`
is the commit `out/cite-pin.txt` writes its pin ranges in while six of those ids are quoted in the tracked
docs as the provenance of a corrected claim. So the recommendation is **leave the log, fix retrieval**: the
collision is disambiguated in the carrier a reader actually opens, and an amend costs a re-anchored pin base
and a renamed SHA in another lane's verification worktree to buy nothing. The owner's call either way.
### Receipt -- the 14a/14b handover, checked at the ref it names (2026-11-16, export lane)

`a942cd0` cold in its own fresh SHA-named worktree: 546 smoke / 675 layout / 85 planner,
`stale=0` at 25/25/3 inputs, `GUARD OK`, ctest 4/4, exit 0 -- and the run one commit earlier at
`8e49944` printed the same 546/675/85, which is what `git diff --name-only 8e49944..a942cd0` =
`NEXT-STEPS.md` alone predicts: a docs commit cannot move a verdict. Against this lane's own
`d8bbeb8` receipt (484/675/85) 14a is +62 smoke checks, and the `[28]` baseline is one of them in
kind, not in name: `4BC3AA231178C071` is an asserted constant at Tests/SmokeTest.cpp:5174 in
`8e49944` (5159 in this tree), never
printed by a green run, so the receipt for it is the `0 failures` line and nothing more.
`writeSlotRows` is defined at Source/PluginProcessor.cpp:736 in `a942cd0`, declared at :364 there,
and driven from section [35]; at that ref `PatternPlanner` still has no caller in Source/, so the
handover's future-tense exclusion is
the tree's shape and item 21's blocker stands now that the seam exists.
Two corrections to the handover. Ancestry: `9c872f7` is not an ancestor of `lane-44` -- merge-base
is `5d071b0`, master +2, lane-44 +5 when I measured, so a ship is named by SHA, never by a count
over a branch someone may push. The tip then moved again, to `ccd6308`: `git diff --name-only
a942cd0..ccd6308` is `NEXT-STEPS.md`, 0 compiled inputs, 0 payload inputs, and `out/wt-44` is clean
at that ref -- so 14a/14b reads **verified at `a942cd0`, unchanged at `ccd6308`**, under the
AGENTS.md rule that a doc commit after a verified SHA is judged by what its diff can still
invalidate. Provenance of that rule, since it was attributed to `9c872f7`: `git log -S` on the
sentence puts it in `aa22b44`; `9c872f7` is the commit that says what the two hashes it quotes
*can* and *cannot* prove, which is the half this paragraph re-checks.

I re-hashed the machine rather than accept the pair: the installed standalone exe is
`0309abcd...` and the installed bundle binary is `98643a14aa7115b5...`, both byte-equal to their
`launch/` copies and to the prefixes `AGENTS.md` records for the `3c3f059` export -- so Program Files
holds `3c3f059`, and the gap to `lane-44` is exactly what
`git log --oneline 3c3f059..lane-44 -- Source/` says: `ba7c953` and `8e49944`, two commits, not
everything since `b603107`. `moduleinfo.json` is `e7c65365...` on both sides, at
`Contents\Resources\` rather than beside the binary -- completeness, never identity -- and the
installed payload `README.txt` `c98efc6c...` equals the tracked `installer/README.txt`.

The sweep was run over this tree dry and **not** applied: `to correct: 9, unresolvable: 0`
(25 already right, 86 edited-since-`bcd6188` lines left alone) and all nine sit in the
`NEXT-STEPS.md` head that `5d071b0` shares with the lane. Not sweeping is the measured choice, not
a skipped step: `git diff --name-only 5d071b0 master -- Source/ Tests/ CMakeLists.txt` is empty and
the merge-tree over `4f51a7c ccd6308` returns `Source`, `Tests` and `CMakeLists.txt` byte-equal to
lane-44's (`f903ac7d`, `5a2fc680`, `34a1a1df`) -- the merged tree's compiled inputs *are* the
lane's, so the lane's already-swept numbers are the correct ones there (none of master's nine
stale strings survive into the merged `NEXT-STEPS.md`), while a sweep on this side would write
numbers true today and false at the merge and turn one conflict region into ten. `external/` is
untracked, so it is not a fourth equal tree -- `git ls-tree lane-44 --name-only external` is empty.

The audit, with the exemption list named beside it because the list decides the verdict: this
tree's 17-entry `out/cite-pin.txt` (`3518a1e1089c`) prints points-at-symbol 20, needs-a-human 62,
table rows 19, pinned 31, repaired 0, baseline-qualified 3, `UNPINNED miss ... : 0`. The lane's
19-entry set (`a2a9e2c225aa`) over the *same* docs prints 21, 60, 19, 31, repaired 2 and
`UNPINNED miss ... : 1` -- the one is line 114's `parameterChanged` pointer, which is at 519 in
this tree. So the two 60s and two 0s differ only by which list was loaded, and `miss: 0` is not
evidence until the pin file's hash is printed next to it; that is the AGENTS.md rule this run
re-measures rather than first states.

Hashes, with the pair named in each clause, because these are three claims about three pairs and not
one scale. The two cold trees are `out/wt-8e49944` (HEAD `8e49944`) and `out/wt-a942cd0` (HEAD
`a942cd0`), equal in every compiled input and differing only in this file, each side identified in
`out/planbuild/ab-4222-labelled.txt` by worktree HEAD, exe sha and COFF stamp beside that tree's own
newest compiled input. Across those two directories the std-only planner differs by exactly 4 bytes --
its two link clocks -- while smoke differs by 4,222 and layout by 4,784; and in one tree, objects
untouched, a relink of the smoke exe is also 4 (`out/planbuild/relink-probe.txt`). The mechanism is no
longer open: MSVC salts a `.cpp`-declared lambda's `<lambda_...>` type key with the path of the file it
is compiling, so 42 of the smoke exe's 236 distinct keys move (84 of 475 sites, 2,500 of the 4,222
bytes), header-declared closures keep theirs, and all 69 `.obj` differ because every `.cpp` in the tree
sits at a new path. The probes are recorded in `AGENTS.md`; the rule this closes is unchanged -- across
directories compare verdicts, within one compare hashes.
The citation census reproduced at its ref (31/73/19/29/2/1, `UNPINNED miss ... : 0`) with the
current tooling in `out/wt-44/out/`, and its probe log reddens three planted errors with hashed
restores. But this tree's `out/cite-pin.txt` (17 entries) is not just shorter than the working set
(19): over the same docs it reports `miss: 0` where the current set reports `miss: 1` --
line 114 of this file cites PluginProcessor.h lines 333-338 for a declaration that is at line 519 in
`9c872f7`. `8e49944` repaired that pointer to 602-612 on the lane; master carries the base's number,
so item 22 has a concrete first job: the merged tree is audited with the current six-file set and
this citation is re-derived, not inherited from either side. The sweep is not a way out of it, and
this run caught why: over this tree it offers lines 370-375 for that same citation, which is
`formatEnvelopeValue`'s neighbourhood rather than the declaration now at line 519 -- the
stale-at-the-base case the `AGENTS.md` rule describes in `SequencerEngine.h` history, reproduced live
on a pointer this file still carries.

The merge preview the handover closed with -- "AGENTS.md ... still the only conflicted path" -- was
true when it was written and is not true now, and the second path is mine. Measured on this tree:
`git merge-tree --write-tree 9c872f7 ccd6308` lists AGENTS.md alone; the same command over `4f51a7c`
(which appends 38 lines to `NEXT-STEPS.md` an hour later) lists **AGENTS.md and NEXT-STEPS.md**.
Both are the same shape -- one region per file, both sides appending bullets to the same tail -- and
both resolve by keeping both, which the line arithmetic proves loses nothing: `NEXT-STEPS.md` merges
to 2357 lines = 3 marker lines + 2354 = 1977 (base `5d071b0`) + 38 (master) + 339 (lane-44), and
`AGENTS.md` to 1002 = 3 + 999 = 798 + 69 + 132. So item **22**'s "audit the merged tree with the
current six-file set" now has a sibling duty in the same step: two paths to resolve, not one, and a
ship that merges by SHA has to re-run the preview at the SHA it names rather than reuse this sentence.

One witness was spent to get the hash rule, and it has to be recorded here or the next reader finds
a disagreement in my own scratch: `out/wt-990e4c2`'s smoke exe is now a **relink** made at 19:10 with
the sources untouched (`7ef0e28a3fb36c80` -> `3aa8c869f169eb8c`, same 8,693,248 bytes, 4 of them
different). The `d8bbeb8` receipt at `out/verify-53.console.txt` names `7EF0E28A3FB3`, and that is
still the binary whose verdicts it reports -- the relink differs from it only in the two clocks. Both
binaries are kept at `out/planbuild/relink-{A,B}.exe`; the durable statement is the one in
`AGENTS.md`, not these files, since `out/` is gitignored.

### 5.3 — hear a lane by clicking its name (done 2026-11-16)

**The plan's premise was false, and fixing it was most of the design.** E5/5.3 said *"wire `previewChannel`
behind the audio-thread command queue introduced by 2.3, or delete it"*. There is no command queue: 2.3
repaired the render *split loop*, and nothing since has put a queue between the panel and the engine. And
"delete it" describes a seam nothing ever reached — measured at `3c3f059`, `triggerChannel` had exactly one
caller, `GrooveboxProcessor::previewChannel` (`PluginProcessor.cpp:1748`), and `previewChannel` had zero
callers in `Source/` **or** `Tests/`. That is the same dead path `AGENTS.md` keeps as one of the three false
README claims it caught, so the loaded-gun framing was wrong in the interesting direction: the risk was not
a live wire someone would grab mid-set, it was a feature that had been shipped to nobody for four commits.
What landed is a third route — build the seam, then give it its caller. `triggerChannel` is gone; the
audition goes in through the door a keyboard note already uses, and the message thread now cannot start a
voice at all.

**The seam is a ring, and its shape is all borrowed from the note path.** `SequencerEngine::requestPreview`
(`Source/SequencerEngine.cpp:637`) range-checks first, then refuses when full rather than waiting
(`return false`, never a spin), then packs `(struck << 8) | channel` into one `std::atomic<std::uint32_t>`
slot of a 16-deep array (`SequencerEngine.h:447`). The ordering is the only subtle part and it is the
textbook SPSC argument: each thread advances its own counter so it may read *that* one relaxed, and the
crossing is the acquire — the producer's `previewConsumed` load is what says the slot about to be
overwritten has genuinely been let go of, and the consumer's `previewProduced` load is what says the packed
word it is about to read was really written. No lock, no allocation, nothing on the render's critical path
but a modulo and two loads. `drainPreviewRequests` (`:666`) takes everything outstanding in one pass at the
*head* of `noteEvents`, at `sampleOffset 0` with `isNoteOn` true, bounded by `maxNoteEvents`. That ceiling
is why 16 requests against a pool of 64 voices needs no budget of its own: the drain cannot crowd out the
MIDI events it shares the array with, which is a property of the array's size rather than a rule someone
has to remember.

**Level is quantised to seven bits, in one place, because a note-on cannot carry anything else.** JUCE hands
`getFloatVelocity()` back as the 0..127 byte over 127, so `requestPreview` stores
`roundToInt (jlimit (0,1,v) * 127)` (`:656`) and the drain rebuilds the float from those bits. This is what
makes the identity gate a bit-comparison instead of a tolerance, and the cost is stated rather than hidden:
a level off that lattice is not reachable by a keyboard either. The rounding law got two checks because one
cannot see it — 0.5 is the level where *floor* is wrong and round is right (63.5 → 64), and 0.3 is the level
where *ceil* is wrong and round is right (38.1 → 38). Every arm of that mutation pair now reddens exactly
one of those two lines, which is the gate having two sides rather than a saying about nearest.

**A preview and a key on the same sample resolve the same way on every run, by not being sorted together.**
`collectNoteEvents` sorts `[numPreviews, numEvents)` (`:733`), not the array. The previews lead in arrival
order and are all due on sample 0; `std::sort` is not stable, so sorting them with the MIDI would let the
engine pick an arbitrary winner between a click and a key struck on the same sample, and the tie would move
between runs on the same build. (It stays `std::sort` and not `std::stable_sort` for 4.5's reason: the
latter allocates, and this array lives on the render thread.)

**The hit area is the label rect, and the bank answers first.** `auditionLaneAt`
(`Source/StepGridComponent.cpp:162`) asks `channelAtY` — the same row rule the file drop uses, so the gap
between rows, the sliver under the last one and the border left of the column are not lanes — and then
requires `getLabelBounds (channel).contains (position)`. The click handler (`:873`) runs `bankTargetAt`
*before* `cellAt` and returns from there, so the six controls that share that same column — four slot
marks, a copy mark and the rotate chip — own their pixels outright: a click on one moves the control and
queues nothing. Where a row is too short to carry the
strip, the name still has a rect and still sounds, which is what the README claims and what the gate
measures at the editor's own floor as well as at its default size.

**Gates.** SmokeTest **[34] "a lane can be auditioned from the panel"**, 19 checks in six arms — a-f: an
auditioned lane makes a sound (precondition); a click and a key at the same struck level are *the same
samples* on both channels (no second engine); the two-sided quantisation pair; the hit starts inside the
block the request arrived in, with nothing sounding before the ask; exactly the ceiling taken with nothing
rendered between, an out-of-range lane refused, and room again after one block; twelve requests → twelve
voices and sixteen asks on one lane → the same onsets sixteen notes give (the queue neither dedupes nor
invents); mute obeys and an empty lane stays silent to the last sample; the grid-coexistence arm (four armed
steps give four onsets *as its own precondition*, then one insertion at index 1 and every other onset on its
old sample); and the choke arm — a tone held in block 0, a click cutting it in block 1, the held lane loud
eight blocks in, the choked one under a quarter of that and under 0.005 absolute, so the ratio cannot be
satisfied by "quieter instead of gone". **484 checks, 0 failures**, `[28]` still `4BC3AA231178C071` — that
one is a requirement rather than a hope, because an audition that is not playing must not retune anything
that is. LayoutTest's new section *"auditioning a lane from its name"* is nineteen `expect` sites and
twenty-four executed checks — the geometry group runs at two window sizes — counted here in the groups
rather than left to the total: two fixture prechecks (the wav was written, all twelve lanes hold it), five
geometry assertions at each size (own rectangle, the middle of a name, the whole inside edge, one pixel
outside every edge, and no cell in the row auditioning), the click precondition, a twelve-lane count and a
twelve-lane identity, the cell arm (a plain-mode precondition, the step still toggles, and a cell costs no
voice), the bank arm (its own geometry precondition, the region does cover the mark, the mark moves, and
the audition sharing those pixels stays silent), and the dead points (they answer no lane, and they queue
nothing). **675 checks, 0 problems.**

**Why identity reads a voice rather than a level, and why it is two expects.** The harness loads the *same*
sample into all twelve lanes and reads `debugVoice (i).channel` after rendering one block, so a misrouted
audition is a wrong channel number rather than an ambiguous silence — the failure line says "a voice on
channel 5 for a click on channel 9". That needs no new accessor and no debug counter. It is also why
"sounding" returns a count *and* a channel as one struct: a misroute is only visible when exactly one voice
is up, so the identity arm is deliberately conditional on `count == 1`, and arm **l4** (never queue) kills
the count while leaving identity vacuously green. One expect would have hidden the second defect inside the
first one's exemption.

**Seventeen arms, and the disagreement is the evidence.** `out/mut53.py`, driven by
`out/mutate-53.sh` (logs `out/mutate-53b.log`, `out/mutate-53c.log`, `out/mutate-53d.log`) and, for the
arms written after those rounds, `out/mutate-one.sh` (`out/mutate-53e.log`, `out/mutate-53f.log`), which
runs one arm as green → red → restore-hash → green so the suite is proven alive on the *current* tests
before any count is believed. Every arm restores the recorded good file before it mutates, edits one
expression, and the final restore is hash-compared against `out/*.good` (`StepGridComponent.cpp
dc8a9def…`, `SequencerEngine.cpp 949f7153…`) with a green re-run of both suites after:

| Arm | Mutation | Problems | What it proves the gate can see |
|---|---|---|---|
| l1 | the row answers, not the name | 6 | both sizes' outside-the-name arms, and the dead points |
| l2 | hit area = `getFullRowBounds` | 6 | same set, by the wider-rect road |
| l3 | hit area `.expanded (1)` | 4 | the 1 px border arms only — no cell claim rides on it |
| l4 | click computes the lane, queues nobody | 2 | precondition + count; identity stays green by design |
| l5 | bank hoisted **without** its return | 15 | 12 of them are 4.4's own bank gates, 3 are the audition |
| l6 | bank hoisted, return removed, audition still runs | 3 | precondition, count, and "the mark moves too" |
| l7 | always audition lane 0 | 1 | identity alone — the only arm that can reach it |
| l8 | the handler binds to the row, above the cell branch | 4 | "editing a step never costs a voice", and the dead points from the call side |
| l9 | hit area `.reduced (1)` | 2 | the inside-edge probes at both sizes — the mirror of l3, and nothing else |
| l10 | the hit test answers nothing | 7 | the positive geometry, the click precondition, the count, and the bank-region overlap |
| e1 | nothing drains | 12 | most of [34]: the ring is load-bearing for the whole feature |
| e2 | ceiling + 8 | 1 | the exact-ceiling arm, and nothing else |
| e3 | floor instead of round | 1 | the 0.5 side of the tie |
| e4 | ceil instead of round | 1 | the 0.3 side of the tie |
| e5 | the request routes to the neighbour | 9 | the choke-to-the-floor check, which a dead queue satisfies by silence |
| e6 | the range guard removed | 2 | the refusal gate — **after** it was relocated, see below |
| e7 | the shared mute gate bypassed | 6 | the audition's mute line *and* five note/grid ones, from one expression |

Seventeen arms, no arm green, and every check that a mutation can legitimately reach reached. The pattern
the set was built for is there: the oversized arms (l1/l2/l3) and the stingy ones (l9/l10) reddens
different lines, the two rounding laws split the tie between them (e3/e4), and a queue that is *dead*
(e1) cannot redden a gate whose pass condition is silence — that took e5 and e7.

**e6 went green the first time, and that was a defect in the test.** The out-of-range refusal check sat
directly under the ceiling loop, which by then has filled the ring — so `previewChannel (-1)` answered
`false` because there was no room, not because there was no frame, and deleting the range check changed
nothing. Moved above the fill loop, where the only way to be refused is to be refused, the same arm
reddens **two** lines: the refusal, and the ceiling itself (with the guard gone the two junk requests eat
slots and 24 offers yield 14). The count is the tell that the door and the queue are one mechanism. Both
runs are in the logs, and the second one is the arm's number in the table above.

**The nine checks no arm reddens, and why each is allowed to stay that way.** In [34]: the ruler (four
onsets with nothing clicked) exists to survive every audition mutation — it measures the grid alone, and a
precondition that a feature arm can kill is a precondition that reports the feature instead of the
fixture; "nothing sounds before the ask", which no single expression can break because the producer cannot
increment before the call that made it; and "an empty lane stays silent", discharged by
`retainSampleForChannel` returning null — the same line that keeps a note off an unloaded lane, and
mutating it dereferences a null pointer rather than reddening a gate. In the label section: the two
fixture prechecks (the wav was written, all twelve lanes hold it), "every lane has a name rectangle",
"the panel is in plain mode when this arm runs", "a click on a cell still edits that cell" — which is
4.5's editor path, gated by 4.5's own arms — and "the bank mark is laid out inside the name column at this
size", a statement about the geometry the arm exists to make true. Counted as the list above enumerates them
— three in [34], six in the label section, the latter because "the two fixture prechecks" is two checks — six
of the nine are preconditions whose job is to be green: the ruler, the wav being written, all twelve lanes
holding it, every lane having a name rectangle, the panel being in plain mode, and the bank mark sitting
inside the column. The other three are not preconditions, and they are not claims about a different feature's
suite either; they are claims discharged by a mechanism outside this arm: the producer's ordering, the null
that `retainSampleForChannel` already returns, and 4.5's editor path, which 4.5's own arms gate. That is
written here rather than left to the table because a coverage claim that does not name its residual is the
same shape as the check e6 just caught.

**Two harness bugs that had to die first** are in `AGENTS.md`, and they were not cosmetic: round one
reported **BUILD FAILED** for e2-e4 because a mid-statement `// MUT` comment ate the `);` on the next line,
and it reported 1 failure for e1 where there were 12, because the driver greped `^FAIL` and the smoke
format puts two spaces in front of every per-check line but none in front of its own summary. An arm that
cannot compile and an arm that cannot be counted both look exactly like a suite that does not need the
feature.

**The one lesson worth carrying past this item: two controls in one rectangle means one defect can light up
a different feature's suite.** l5 reddens fifteen checks and twelve of them are 4.4's bank gates, so its
problem count says "4.4 is broken" while its failure *lines* say "the label column now answers twice". A
count attributes nothing; the list of sentences does. This is the same trap `AGENTS.md` records for a control
identified by its range, one layer further down — two components that share pixels share a defect.

**A false claim discharged while writing the docs.** README's humanize section said *"the sound button's
preview is a diagnostic"* — `ChannelStrip::handleSoundClicked` opens a `juce::FileChooser` and plays nothing
(`previewChannel` has exactly one UI caller, `StepGridComponent.cpp:913`), which makes it the second claim
about that button in `AGENTS.md`'s list, after "the menu switches between loaded samples", and the fourth in
the list overall — a claim the file had already contradicted once and which had survived into prose nobody
re-read. It now says the button
browses, and points at the lane name for the sound. `installer/README.txt` gained the matching paragraph, and
`README.md` gained *"Hearing one lane"* between the MIDI and mix sections.

**Deliberately not here.** No slot lookup in the request path: the lane's *sound* — sample, pitch, envelope,
level, choke — is one setting for the lane and not one per slot (4.4's rule), so an audition cannot be
"wrong about the bank" because there is nothing bank-shaped to be wrong about. No velocity from a modifier
or a drag: full level is the answer to "what is this sound", and a kit's rows are not equal-loud, so a
remembered per-lane audition level would be a new parameter with no owner. No held audition and no note-off:
a click has no duration, and the release path belongs to a note. No preview on the sound button, no lane
menu from `getSampleIds()` — that half of B7 is still 5.2's. And nothing about the queue is a parameter,
recorded, saved or undone: an audition is a question about a sound, not an event in the pattern, which is why
one undo step cannot be filled with twelve clicks.

### 14a — the door a generated pattern comes through (done 2026-11-16)

**What was ruled, and what this is not.** Item 14's plan sentence closed on a dependency rather than a
feature — *a generator therefore needs a slot-taking writer that pushes once per slot; that seam is the thing
to agree with the sequencer lane before writing any wiring* — and the owner's ruling was that this lane writes
it. So 14a is the door and nobody walks through it yet: `writeSlotRows` (`Source/PluginProcessor.h:364`,
defined at `Source/PluginProcessor.cpp:736`, row struct `SlotRowWrite` at `Source/PluginProcessor.h:308`) has **no caller in
`Source/`**. `PatternPlanner` and the genre table are still caller-less, and the three planner/panel contracts
item 14 also names are still open. What is settled is the shape a generated take arrives through: into a slot
the user is not pointing at, in one push, without moving a pointer.

**Why the four cell seams cannot serve the shape.** `setStep`, `setStepVelocity`, `setStepChance` and
`setStepRatchet` each resolve the slot for themselves, with `selectedSlotOf (channel)`, so a grid painted
through them can only ever land on the slot the lane is already playing — which is precisely the thing a
generated take must not do to what the user is hearing. And every one of them ends in `pushPatternToState`,
which re-serialises the whole row: four fields by twelve lanes by sixteen steps is **768 encodings of one
768-byte record for one pattern**. `writeSlotRows` takes the slot as an argument, never reads the selection,
and pushes once.

**The economy, measured — and two numbers this lane had written down wrong.** The first cut of the header
comment said the cell seams pay "192 tree writes and 192 undo commands". Neither is what happens. Of the 768
encodings, **240** move the property — 192 level moves and the 48 flags that actually flip — because
`ValueTree::setProperty` does not notify when the value is unchanged, so the codec runs 768 times and the tree
hears 240. And those 240 writes are **two** undo actions, not 240 or 192: a transaction is the editor's per
gesture rather than the cell's, and `UndoManager::perform` coalesces successive `SetPropertyAction`s on one
property (`juce_UndoManager.cpp:141`). So what the one push buys is 768 encodings down to one and 240 host
notifications down to one, with the undo history saving one action out of two — a different and much smaller
claim than the one the sentence was making. Both numbers are now printed by the section that gates them
(`[35]` part d) rather than asserted by a comment, which is the only reason they can be re-checked next phase:
**a count inside a comment is a claim, and a count nobody measured is a guess wearing a number.**

**The contract, in the four consequences the header states.** A refusal is only ever about the *shape* of the
ask — slot outside the bank, null `lanes` or `rows`, count outside `1..12`, a named lane outside the twelve —
and never about what the machine already holds, so the return value is worth reading. Every one of those is
checked before a single cell moves, which is what makes a refusal leave the session byte-identical. A slot no
lane points at is inaudible while it fills, and filling the slot a lane *does* point at is allowed; for that
case the four fields are stored neighbours-then-flag, so a step that comes on arrives with the level,
probability and strike count that go with it instead of a block's worth of the old ones. A lane named twice is
written twice and the later row wins, because a caller assembling a lane list can easily name one twice and
last-write is the rule that matches what the list says. Lanes not named keep every cell they hold, in this
slot and in every other.

**One helper went public on purpose.** `slotPropertyName` (`Source/PluginProcessor.cpp:1245`) was private until
this seam needed a test that reads the record it wrote. The bank is sparse — slot 0 is `pattern`, the others
`pattern1..3`, and only when filled — so a caller that aims at a slot has to be able to *name* it, and a test
or a host that spells the name out is a second grammar for the same property. The section now gates both
halves at once: it asks the function for the name, and it asserts that a bank with only slot 2 filled names
its records exactly `pattern` and `pattern2`.

**Gates: section `[35]`, 61 of the suite's 546 checks.** Six parts. (a) a fresh session holds slot 0's record
under that literal name and nothing else, which is what makes a later emptiness a *removal* rather than a
never-was. (b) seven refusals, each asked while nothing else could say no, each followed by the whole 192-cell
live grid and the tree census being byte-for-byte what they were, a printed precondition that the refused ask
*disagrees* with the grid in the flag, the level and the strike count, and the zero-repaint rule. (c) an
accepted twelve-lane fill: one property change, one repaint, a v4 record on the slot the bank has, all 192
cells equal to the ask, no lane's pointer moved, the user's grid unchanged, and the two renders identical
sample for sample while nothing points at the filled slot. (d) the same grid through the bulk seam (one
change), the same grid through the cell seams (240 changes, printed), the two records agreeing so the economy
is not bought by writing less, the undo actions two against one, and one undo restoring both routes to the
absent record. (e) the neutral fill removing a slot's record, the bank's marks agreeing with the record rather
than with the gesture, slot 0 being exempt exactly as the save path says it is, and the render: a fill of the
slot nobody points at sounds nothing on an armed lane with a sample in it, and pointing that lane at the slot
sounds `0 4 8 12 16` — four hits and the bar's own wrap at 16, through the transport rather than a second
door. (f) the three clamps and the duplicate lane.

**Ten arms in `out/mut14.py`, eight red and two green on purpose.**

    w1  store before the pre-scan            2   the refusal's grid, and the first lane's own cell
    w2  flag before its neighbours           0   declared: no single-threaded harness sees a row mid-write
    w3  a push per cell                      4   the notification counts, in both directions
    w4  the panel is never told              1   the repaint count
    w5  slot read from the selection        16   everything that says "aimed at a slot", plus the render
    w6  level stored unclamped               1   the clamp
    w7  probability stored off the lattice   1   the rung
    w8  strike count stored past the ceiling 0   declared: discharged at every reachable read
    w9  only the trues are written           3   the removal, the dots, the duplicate lane
    w10 an empty lane list accepted          2   the refusal and the zero-repaint rule

**Three things the lap taught.**

- **w1 came back green, and the fixture was the bug, not the arm.** A default `SlotRowWrite` is every cell
  off, and every cell off is exactly what that grid already held at step 0 — so the leak wrote the value that
  was already there and the check read its own fixture. The refused asks are now *shaped* to disagree with the
  grid in all three of flag, level and strikes, and the section prints that difference as its own precondition.
  A control that cannot bite indicts the pair it is comparing first.
- **w8 is green for the opposite reason: the rule it breaks is real and gated, just not at that door.** The
  stored atomic is clamped again on the way out of `getStepRatchet`, again by `cellSaysAnything` and the codec,
  and again at the one place the number becomes sound — `SequencerEngine.cpp:874`, where 4.5's own
  `setStepRatchet (3, 7, 999)` check does gate "sixty cannot become sixty hits". So the seam's clamp keeps the
  two write doors storing one representation and is not the load-bearing one, and no observation this harness
  can reach tells the two builds apart. The check's comment now says so, so the next reader does not count it
  as the control for that.
- **w4 reddened two checks that have nothing to do with it, and they were lying beforehand.** "The audio thread
  ran throughout [0 blocks]" is section `[11]`, the two-hundred-load concurrency hammer: two hundred loads can
  finish on one core before the scheduler starts the render thread, so the section was able to measure no
  overlap at all and still print three greens. `[11]` now waits, bounded, for the first block before the
  loads begin, and says so if it never gets one. Found by an unrelated arm, which is the usual way a fixture
  that has always been flaky under load gets noticed.

**Run.** 546 smoke / 675 layout / 85 planner checks, 0 problems, RT-path guard exit 0, `Groovebox` target 0,
`ctest` 4/4 (`out/gate14-run2.txt`, suite logs in `out/smoke-14-final-utf8.txt` and its twins), and `[28]`'s
fingerprint is still `4BC3AA231178C071` over 161280 samples (peak 0.68136, rms 0.16312) — a door nobody
walks through must not retune a hit. The falsification lap is `out/arms14-run-c.txt`: ten arms, each from a
restored-good file, run against the *final* shape of the test file, with `FINAL BUILD exit: 0` and
`PASSED 546 checks, 0 failures` at the bottom so the restore is proven by the log rather than asserted by
this paragraph.

**Off the tip, cold.** `8e49944` was then checked in a worktree that had never held a build — a new
folder, a fresh `cmake` configure (it cloned JUCE into `external/JUCE`), the three suites built per
target and `ctest` run after them, and no `Groovebox` plugin target: `out/planbuild/verify-sha.ps1
-Sha 8e49944 -Tag 14a -Since d8bbeb8`. It prints `DIRTY FILES: 0`, `NAMED BY: lane-44`, `stale=0`
beside each exe's own input count, then 546 smoke checks with 0 failures, 675 layout and 85 planner
with 0 problems, `GUARD OK: the parameter path is allocation-free` and `100% tests passed out of 4`
— and `[28]` comes back at the same `4BC3AA231178C071` in that tree. The run is
`out/verify-14a.console.txt`, with the per-suite logs beside it. Scope is part of the claim: that
proves the *sources*, not the deliverable. `launch/` was not touched and no setup was built, so
Program Files stays behind the tree by two `Source/`-changing commits after the `3c3f059` export, and closing
that gap is the owner's install step. This paragraph then lands as a docs-only commit on top of the
tree that run was made in, touching no compiled input, so by the rule in `AGENTS.md` it is not owed
a rebuild either.

**Deliberately not here.** No UI and no "generate" button: the seam is the agreement, the wiring is the
decision that still belongs to the owner. No undo-transaction wrapper — the fill is already one action because
it is one property write, not because anything opened a transaction. No pointer move, no slot copy, no
clear-on-write: `writeSlotRows` fills, and the only way a slot goes away is by holding nothing. No validation
of what a caller asks for beyond the shape of the ask, and no gate on the store's strike clamp, for the w8
reason above.
