# Groovebox

A VST3 / standalone drum machine built from the drum page of **Earworm Studio**
(`earworm-studio/index.html`): a 16-step sequencer where each drum channel is a
sample you drag onto its row, and which also plays from MIDI notes.

## What it does

| Feature | Detail |
|---|---|
| Step grid | 16 steps per bar, 12 drum channels, painted in the channel's own colour |
| **MIDI input** | **Play the channels from a keyboard or pads: the channels answer C1 upwards, with velocity** |
| **Step velocity** | **Every step has its own velocity. Hit *Velocity* in the top bar and drag up or down inside a step to set how hard it hits** |
| Sample loading | Drag a `.wav` / `.aiff` / `.mp3` / `.flac` / `.ogg` onto any channel **row**, or click the channel's sound button to browse |
| Per-channel pitch | −36 to +12 semitones, with `-12` / `+12` octave buttons |
| **Pitch drop** | **0–12 semitones per channel: the hit starts that far above the note and glides down onto it over 30 ms — kick bloom you cannot get from Live's sample page. It is a parameter like the rest, never something the plugin decides for you** |
| **ADSR per channel** | **Attack, Decay, Sustain and Release for every channel, so a hit can be tightened or turned into a sustained sound** |
| **Choke groups** | **Three per lane. A hit in a group cuts every other hit in that group short, ramped over 20 ms — put the closed hat in the same group as the open one and it stops it instead of stacking on it** |
| **Step probability** | **Every step says how often it is allowed to fire, 0–100 %. What actually happens is that wish discounted by the *Mutate* knob, and the same grid with the same seed drops the same hits every time** |
| Swing | 0–100 % on either a 1/8 or 1/16 grid |
| Tempo | Follows the host project tempo; in the standalone build it has its own transport and tempo |
| Mix | Per-channel level and mute, plus a "clear row" button |
| **Master trim & meter** | **A −24 to +6 dB master trim you can see and reach, a bar for the sample peak with a tick for the true peak hiding between the samples, and a clip latch on the output** |
| **Automation** | **Levels, pitches, pitch drops, envelopes, mutes, choke groups, swing, swing grid, tempo, the master trim, the variation amount and the seed are host parameters — automate them, and the panel follows what the host drew** |
| Row editing | Click a cell to toggle it; drag across cells to paint a run |
| **Shift a row** | **The ← and → buttons in a channel strip turn that whole row one step earlier or later, and every step's velocity goes with it** |
| Persistence | Pattern, per-step velocity and probability, pitch, levels, mutes, names, colours, seed and sample paths are saved in the session; a session saved before the parameter migration is read in and its values carried over |

## Step velocity

Every step carries its own velocity, so a pattern can have accents instead of
every hit landing at the same level.

Press **Velocity** in the top bar to switch the grid into velocity editing. A bar
rises from the bottom of each step showing how hard it hits, with the value under
the pointer; drag up or down inside a step to set it. The value is taken from how
high the pointer is in the row, so the bar follows the pointer.

- **The step's on/off state is left alone.** You can set a level on a step that is
  switched off, and dragging to nothing never switches a step off. An on step is
  never stored as "off", so a quiet hit stays a hit.
- **Press 100%** next to the toggle to put every step back to full velocity.
- **Shifting a row moves its velocities too.** The ← and → buttons in a channel
  strip turn that row one step earlier or later, and each step carries its velocity
  with it, so an accented pattern stays accented after it moves. The last step
  wraps round to the first rather than falling off the end.
- Outside velocity mode the steps are dimmed by their velocity, so you can see the
  dynamics without opening the overlay.
- The grid's velocity and a played note's velocity are separate: the grid uses the
  value on the step, while a pad or keyboard uses how hard you hit it.

**Velocity is two laws, not one.** It scales the level, and it also opens the top
end: a hit is played through a lowpass whose corner rises with how hard it was
struck, from 900 Hz at the very bottom of the travel to **no filter at all at full
velocity**. That last part is the point — the loudest thing the machine plays is the
recording exactly as it is, bit for bit, so the law can only ever change the hits you
asked it to.

| Velocity | Corner | What it means |
|---|---|---|
| 100 % | none | the sample as recorded |
| 50 % | about 5.8 kHz | played, not turned down |
| 25 % | about 2.9 kHz | soft: the sizzle is off the top |
| 6 % | about 1.4 kHz | a muted strike |

A dimmed hit also loses whatever level lived above its corner, so a soft hit comes
out a little *under* what the gain alone would give — measured on a flat noise burst
at 1.9 dB of peak at 25 % velocity and 4.0 dB at 6 %, which is the worst case, since
a sample with a body keeps most of its energy below those corners. The fader, by
contrast, moves level only: the same quarter of a gain by the fader leaves the tone
exactly where it was, and both of those are checked by name in the test suite.

## The envelope

Every channel has its own ADSR, shown as four rows (**A D S R**) under the level
fader in that channel's strip. Drag a row up or down to change it — horizontally
works too — and double-click a row to reset just that one.

The defaults are a drum hit: instant attack, a slow decay, **no sustain** and a
short release. Sustain 0 is what makes a channel a one-shot: the envelope lets go
as soon as it reaches the sustain stage, so the decay and release are what shape
the hit.

**Raise sustain and the channel holds.** It then behaves like a sustained
instrument: the note rings until you let go, and the note off starts the release.
That is what makes a pad or a keyboard play a sustained sound rather than a blip.

| Control | Range | Notes |
|---|---|---|
| Attack | 0 – 2 s | Linear rise; at 0 the hit starts instantly |
| Decay | 0 – 3 s | Exponential fall to the sustain level, arriving exactly at it |
| Sustain | 0 – 100 % | 0 = one-shot; above 0 the note holds until released |
| Release | 0 – 4 s | The same curve down to silence, after the note off |

Decay and release are one curve, not a straight line. Every fifth of the duration
you set multiplies what is left of the distance by 0.368 — about 8.6 dB — so a 0.5 s
decay is halfway down inside its first 70 ms, has a tenth of its level left after
230 ms, and spends the rest of its time thinning away. A linear ramp cannot do that;
it closes like a gate. The curve is nevertheless bounded by the number on the row:
the last sample of the fall is the target itself, so a decay ends at the sustain
level and a release ends at silence rather than trailing off for ever below hearing.

The settings themselves are on a squared curve, so the low end of a row's travel is
usable —
most drum shaping happens in the first few milliseconds, and a linear control
would put all of that in its first percent. Each row shows its value in
milliseconds, seconds or percent.

## Playing it from MIDI

The channels answer **C1 upwards** — C1 plays channel 0, C#1 channel 1, and so on
to B1 for channel 11. That is the range a General MIDI drum map uses for its first
twelve pieces, and what most pad controllers send out of the box, so a kick pad
plays the kick without any setup. The note range is shown in the top bar.

- **A played note takes its own velocity**, both in level and in tone, so a pad or
  keyboard plays dynamically. The step grid takes the velocity written on the step —
  which is 100 % unless you have edited it, so a sequence can stay flat while you
  play over it. See *Step velocity* above.
- **Notes play whether or not the transport is running**, so you can play the kit
  with the sequencer stopped. Grid hits are faded out when the transport stops;
  played notes are not, because they did not come from the transport.
- **A note off releases the note.** With the default sustain of 0 that makes no
  audible difference, because the one-shot has already let go of itself; raise
  sustain and the note off is what ends the sound.
- Notes outside the mapped range are ignored, and a muted channel ignores its note.

## Mix, headroom and the meter

The voices sum at unity and the master stage trims the result. There is no hidden
compensation anywhere in the signal path: an earlier build multiplied every voice
by a fixed 0.8 that no control could reach, so the panel's loudest setting was
never actually the loudest the engine could produce, and no meter explained the
missing 2 dB.

**Master Trim** is the slider in the top bar, −24 to +6 dB, defaulting to −2 dB.
It is where the old hidden 0.8 now lives, made visible: the default leaves
roughly the same headroom a full kit used to get by itself, but you can move it.
Gain is smoothed over 20 ms so dragging it cannot zipper, and double-clicking the
slider returns it to −2 dB.

The meter beside it is one bar per output channel over a scale of −42 to +6 dBFS,
a thin tick at the **true peak**, and the clip indicator marking 0 dBFS. It reads a
peak held on the audio side — the editor polls a few times a second, far too slowly
for a drum transient, so the bus keeps the highest value it saw and lets it fall
away between blocks.

The tick exists because a sample peak is not the level the converter puts out.
Between two samples the band-limited waveform can be higher than either of them, and
on a summed kit that is regularly a decibel or two — one hit through this master
measures 1.05 dB of it. The worst case is easy to state exactly: a tone with four
samples to the period has its crest halfway between two of them, so the bar reads
0.5657 of an 0.8 wave while the wave itself crests at 0.8000, and the tick says so.
The bus reconstructs its own output at four points per sample — the same windowed-sinc
kernel the voices already use, at three fixed sub-sample offsets — and reports the
highest point it finds. The tick turns red at 0 dBFS, where a bar can still be short
of it.

**That number is a report and nothing else.** It gates no level, changes no latch
and reshapes no audio: the latch still means 0 dBFS *at the sample*, which is what
the plugin wrote. Anything reaching 0 dBFS latches the clip indicator, and **the
latch is only cleared by clicking the meter**. There is no limiter: if a kit is too
hot, the fix is the trim or the channel levels, not a processor quietly reshaping
what you mixed.

## Automation

Every continuous control is a proper host parameter, so it can be drawn as
automation instead of only moved by hand: per-channel level, pitch, pitch drop,
A/D/S/R, mute and choke group (9 parameters × 12 channels), plus master trim,
swing, swing grid and tempo — 112 in total, grouped per lane so a host lists them
sensibly.

The pattern is deliberately **not** in the parameter list. 192 cells as 192
parameters would swamp a host's automation list, and it is not what anyone
automates; it stays in the session state. Tempo is in the list for the standalone
build and for recalling a session, but a hosted plugin follows the host's tempo —
drawing a ramp on it there does nothing the host has not already decided.

Ranges match the controls that already existed, so the migration changed what a
host can reach rather than what the panel does. Values saved before the
migration are read in and carried onto the new parameters on load, so an old
session comes back at the levels it was left at.

Host-written values reach the panel, not just the audio: the channel strips and
the top bar re-read the parameters on their timers, so a ramp you draw in the
host moves the fader, the mute button, the pitch readout, the envelope rows and
the choke box in front of you. Every control writes straight through when you
move it, so during a drag the poll reads back the value already under the pointer
and leaves it alone.

## Layout

- **Top bar** — transport, tempo, swing and grid, master trim and meter, and
  "Clear pattern".
- **Middle** — the 16 × 12 step grid. Channel 11 is the top row and channel 0 the
  bottom, matching the web app, which reverses its lane list.
- **Bottom** — one strip per channel: name (double-click to rename), colour
  swatch (click to cycle), sound, pitch, pitch drop, level, mute and choke group.

While a channel is firing its row lights up, and a white column marks the
playhead.

## Building

Requirements: **CMake 3.22+**, **Visual Studio 2022 or newer** with the C++
workload (or another C++17 compiler), and **git**.

```powershell
cmake -S . -B build-new -G "Visual Studio 18 2026" -A x64
cmake --build build-new --config Release --parallel 2
```

`--parallel 2` is not a typo. On this machine MSVC 14.51 fails intermittently
with "internal compiler error" / `D8040` when too many `cl.exe` processes run at
once, so the build deliberately does not enable `/MP`; capping MSBuild's own
parallelism keeps it reliable. If a build does fail that way, just run it again —
it is not a code error.

The first configure clones JUCE 8.0.4 into `external/JUCE` (shallow, ~1 minute).
On this machine git's default schannel backend cannot acquire credentials, so the
configure step passes `-c http.sslBackend=openssl`; if the clone still fails,
clone it by hand:

```powershell
git -c http.sslBackend=openssl clone --depth 1 --branch 8.0.4 https://github.com/juce-framework/JUCE.git external/JUCE
```

Adjust the generator for your Visual Studio version if `Visual Studio 18 2026`
does not match (`cmake --help` lists the available ones).

### Targets

| Target | Output |
|---|---|
| `Groovebox_VST3` | `build-new/Groovebox_artefacts/Release/VST3/Groovebox.vst3` |
| `Groovebox_Standalone` | `build-new/Groovebox_artefacts/Release/Standalone/Groovebox.exe` |
| `GrooveboxSmokeTest` | `build-new/GrooveboxSmokeTest_artefacts/Release/GrooveboxSmokeTest.exe` |

The two shippable artefacts are assembled into **`launch/`** by a dedicated
target, the same way the other skeer-audio projects do it:

```powershell
cmake --build build-new --config Release --parallel 2 --target launch
```

That gives you `launch\Groovebox.exe`, `launch\Groovebox.vst3` and — once
`installer\build-installer.ps1` has run — `launch\Groovebox Setup <version>.exe`:
the one folder to run from, to install from and to ship, whatever the build tree
happens to look like. It is a target rather than an automatic post-build copy
because Ableton Live holds the plugin DLL open: a locked bundle should fail the
release step loudly, not break the build you were trying to test. If the copy
fails, quit Live and run it again.

## Installing

There are two ways in: the **installer** for a machine you are shipping to, and a
**manual copy** for this one.

```powershell
.\installer\build-installer.ps1              # build, refresh launch/, then compile the setup
.\installer\build-installer.ps1 -SkipBuild   # package whatever launch/ already holds
```

That produces **`launch\Groovebox Setup <version>.exe`** — Inno Setup, the same
installer the other skeer-audio products ship, reading its payload straight out of
`launch/` so a setup file can never contain binaries older than the last build. The
version comes from `project(... VERSION ...)` in `CMakeLists.txt` unless `-Version`
overrides it. It writes the VST3 to **exactly one place** —
`C:\Program Files\Common Files\VST3\` — asks for elevation to do it, and has no option,
task or conditional that puts a second copy anywhere. See `installer\Groovebox.iss`.

One destination is the rule, not a simplification. A plugin installed twice appears
twice in the browser, and the two bundles start drifting the moment one of them is
rebuilt — so a "convenience" copy into a per-user or DAW folder is a defect with a
friendlier name.

**Quit your DAW before installing.** The installer deliberately does not close running
applications (`CloseApplications=no`): if the plugin DLL is held open by Live or
another host, the copy fails with a clear message instead of killing the host and the
unsaved set inside it. Quit the host, run the setup again.

**The setup prompts for UAC.** `Program Files` is protected, and the alternative —
installing unprivileged into a folder that happens to be scanned — is what produced the
duplicate. Elevation is the cheaper cost.

To install by hand instead: build the plugin first (above), then copy the whole
`Groovebox.vst3` **folder** (not just the inner binary) into your VST3 folder and rescan
in your DAW. Keep it to one folder; two copies is the bug this section describes.

```powershell
# one copy, in the folder every host scans -- run this shell elevated
$dest = "$env:ProgramFiles\Common Files\VST3"
Copy-Item -Recurse -Force launch\Groovebox.vst3 $dest
```

The **standalone** app needs no installation of its own beyond the `Groovebox.exe` the
setup puts next to the plugin — for a quick look, run `launch\Groovebox.exe` straight
from the build folder.

### Ableton Live specifics

Live's global VST3 folder is `C:\Program Files\Common Files\VST3`, which is where the
installer writes, so the only setting you need is the system-folder tick:

1. **Settings → Plug-Ins**
2. Tick **Use VST3 Plug-In System Folder**
3. Click **Rescan** — hold `Alt` while clicking to force a full rescan

If **VST3 Plug-In Custom Folder** is ticked, check what it points at before you rescan,
because a custom folder is a second place Live will happily find a stale Groovebox. Live
12.3.5 on this machine records 742 bundles under `C:\Program Files\Common Files\VST3` and 4
under `%LOCALAPPDATA%\Programs\Common\VST3` — that custom folder is scanned, and it is where
this installer used to write, so a leftover there rescan as a duplicate entry. `Documents\VST3`
(also a former destination) appears **0** times in that database: Live does not read it unless
you point the custom folder at it yourself. The uninstaller removes the copy it made; a folder
you filled by hand you clean by hand, after checking which binary is current:

```powershell
Get-FileHash "$env:ProgramFiles\Common Files\VST3\Groovebox.vst3\Contents\x86_64-win\Groovebox.vst3",
             "$env:LOCALAPPDATA\Programs\Common\VST3\Groovebox.vst3\Contents\x86_64-win\Groovebox.vst3",
             "$env:USERPROFILE\Documents\VST3\Groovebox.vst3\Contents\x86_64-win\Groovebox.vst3",
             "launch\Groovebox.vst3\Contents\x86_64-win\Groovebox.vst3" -ErrorAction SilentlyContinue
```

The plugin appears under **Plug-Ins → VST3 → Earworm → Groovebox** and loads on a
**MIDI** track (it is an instrument, so it is not listed under Audio Effects).

If Live reports *"this VST plugin could not be opened"*, its log says why. The
scanner writes to:

```
%APPDATA%\Ableton\Live <version>\Preferences\PluginScanner.txt
%APPDATA%\Ableton\Live <version>\Preferences\Log.txt
```

Search those for the plugin name; a bug in this plugin was diagnosed that way
(see the MIDI input bus note below).

Two things can stop a rebuild from reaching Live: Live holds the plugin DLL open
while it is running, so quit it before replacing the bundle, and Windows Explorer
will not overwrite a bundle that is in use.

## Verifying

Two test programs run with no GUI window and no host.

`GrooveboxSmokeTest` drives the real processor and checks the parts that are easy
to get subtly wrong:

```powershell
build-new\GrooveboxSmokeTest_artefacts\Release\GrooveboxSmokeTest.exe
```

It asserts, among other things, that:

- an empty engine is silent, and a hit fires exactly once;
- every one of the 16 steps lands within **2 samples** of its grid position at
  120 bpm, regardless of the host block size;
- the pattern still lands on the grid after **32 bars** at 120 bpm, after 16 bars
  at 300 bpm with several onsets inside one block, and after 24 bars at 127 bpm on
  an awkward 333-sample block — one bar could not see a clock that drifts;
- swing places every step on the onset the web app's formula specifies, on both
  the 1/8 and 1/16 grids;
- an octave of pitch shortens a sample, mute silences a channel, and a lower
  level is quieter;
- pitching a sample **up** does not fold its top end back into the audible band:
  measured on synthetic tones, a component that would alias is left at **−83.1 dB**
  while one inside the passband, and a tone at unity, both stay within **0.00 dB**
  of where they started;
- a pitched hit is at **its own level from its first sample**: on a source held flat,
  the eight samples at the top of a hit an octave up read the same number the body of
  that hit reads, with no step between one sample and the next. The taps that hang off
  the end of the source used to be skipped, which left that hit starting **7.4 % hot**
  and arriving at its level over the next four samples — on the part of a drum hit that
  carries the transient;
- a **pitch drop** is a glide and not a jump, and it lands on the note it was asked
  for: measured from zero crossings, twelve semitones of drop starts the hit at 0.761 of
  the note's own period, the seven cycles across the sweep lengthen monotonically from
  131.8 to 198.7 samples, and the settled part is at the 200.455-sample period the Pitch
  control says to within half a sample — at the same level as the same hit with no drop.
  What the sweep reads extra is what the hit gives up: 263 samples, and the same 263 on
  a 454 ms and on a 36 ms sample;
- **the same strike is the same hit whatever block the host asked for**: a dropped hit
  rendered at 256, 512, 1024 and 2048-sample blocks ends on the same sample at the same
  integrated level, and at a drop of twelve the tail of it measures 0.995 of the
  brightness of a lane that only had to interpolate, 0.984 at five. A band pinned at
  the top of the sweep used to cost that tail 4 dB, which is a tone control wearing a
  tuning control;
- a level or mute change made **while the sequencer is running** takes effect on
  the hits that follow;
- samples can be loaded from another thread while the audio thread is rendering,
  and the render keeps making sound throughout;
- the master trim is the only place the output is scaled — a unity hit comes out at
  the sample's own peak, the 0.8 the old build hid is reachable from the trim, and
  the shipped default is **level-matched** with it (so "louder" is never mistaken
  for "better" here);
- the meter holds what the output reached, the clip indicator latches at 0 dBFS,
  the latch survives silence until you click it, and the peak falls away;
- the **true-peak tick reads the level between the samples**: over seven tones whose
  samples straddle the crest, from 441 Hz to a quarter of the sample rate, the bar lands
  on the `cos(π/N)` the sampling says it must (worst error 0.00000) and the tick lands on
  the wave's own crest (worst 0.00011), with the gap it reports matching `1/cos(π/N)` to
  four decimals at every row; at a third of the sample rate it reads 1.1549 where √3/2
  implies 1.1547; a 1 kHz tone, where a sample is already on the crest, is given nothing
  (1.0000); a lone 0.9 sample reads 0.9000 and not the reconstruction filter's own ring;
  a hard ±0.9 edge is shown as the 1.2643 overshoot it is while its **latch stays clear**,
  because no sample reached 0 dBFS; one real hit through the master measures 1.05 dB of
  it; the reading is identical at a 256 and a 2048-sample block, and it leaves the audio
  bit for bit where it found it;
- a mono sample stays centred;
- pattern, pitch, names, swing and grid survive a save/load round trip;
- an incoming note plays its channel with the transport stopped, a note outside
  the mapped range stays silent, velocity scales the hit, a muted channel ignores
  its note, and a note off leaves the one-shot ringing;
- the envelopes behave: sustain 0 makes a one-shot while sustain 1 holds, a note
  off releases a held note, and a longer attack, decay or release measures longer
  in the audio;
- a decay and a release **fall on a curve, not a ramp**: measured in four windows
  across each fall, every fifth of the duration removes the same share of what is
  left — 0.368 of the level, to three decimals on all three steps of both falls —
  where the straight line this replaced measures 0.778, 0.715 and 0.603, and the
  fall is over when the parameter says it is;
- no combination of the four envelope controls makes a click: the engine is
  rendered at **all 81 corner settings** of attack, decay, sustain and release —
  each at its bottom, middle and top, with the transport stopped part way through
  to force a release out of a hold — and the sample-to-sample slew of every one
  stays under the ruler's limit;
- a harder hit is **brighter**, not only louder: the tone of a hit falls as its
  velocity does while a fader change leaves the tone exactly where it was, and a
  hit at full velocity comes out at the sample's own peak and its own balance,
  bit for bit;
- the envelope values survive a save/load round trip, and a channel that was never
  touched keeps the defaults;
- step velocities survive a save and load, including a very quiet step, and an off
  step never comes back as on;
- shifting a row carries each step's velocity with it, wraps the last step to the
  first, is a no-op over a whole bar, and leaves every other row alone;
- a value written into a **parameter** the way a host writes it — normalised,
  straight through `setValueNotifyingHost`, without touching any of the
  processor's own setters — reaches the engine and changes the audio, so all 112
  parameters are genuinely automatable rather than merely listed.

`GrooveboxLayoutTest` builds the real editor and inspects it. A control laid out
at zero size or outside its parent cannot be clicked, and no audio test can see
that:

```powershell
build-new\GrooveboxLayoutTest_artefacts\Release\GrooveboxLayoutTest.exe
```

It reports the bounds of every control, flags any with no size or outside their
parent, and drives the level fader with synthetic mouse events to confirm the
change actually reaches the processor. It also goes the other way: it writes a
parameter as a host would and checks that the strip's fader, mute button, pitch
readout and envelope rows all catch up to it.

Run both after any change to the editor or the engine. Both have already caught
real bugs: swing that was computed but never applied, a step that fired early
after a bar wrap, a pitch control laid out at 0 px wide, faders whose moves never
reached the processor, and a panel that kept showing the old value after the host
automated it.

## How it is put together

```
Source/
  Common.h              layout constants (16 steps, 12 channels) and the palette
  Parameters.h          the automatable parameter set: ids, ranges, grouping
  SampleStore.{h,cpp}   sample decoding, and the channel -> sample assignment
  Resampler.h           the windowed-sinc coefficient table the voices read from
  SequencerEngine.{h,cpp}  the scheduler and the voices (the audio thread)
  MasterBus.h           the master trim, sample-peak bar, true-peak tick and clip latch
  PluginProcessor.{h,cpp}  the AudioProcessor, transport and state persistence
  PluginEditor.{h,cpp}     window layout
  StepGridComponent.{h,cpp}  the grid, and the file drag-and-drop target
  ChannelStrip.{h,cpp}      one channel's controls
  EnvelopeControls.{h,cpp}  the A/D/S/R rows inside a strip
  TransportBar.{h,cpp}      transport, tempo, swing, master trim and meter
  LookAndFeel.{h,cpp}       the flat dark theme
Tests/
  SmokeTest.cpp         the headless engine test
  LayoutTest.cpp        builds the real editor and inspects it
```

Notes on the design:

- **A VST3 instrument must expose an event input bus.** The plugin reports
  `NEEDS_MIDI_INPUT TRUE` and `acceptsMidi() == true` even though the sequencer
  ignores incoming notes. Without that bus, a host rejects the plugin at
  instantiation: Ableton Live logs `plugin has instrument category, but no valid
  event input bus` and shows "this VST plugin could not be opened". The plugin
  clears its MIDI buffer each block, so the bus carries no cost.
- **One clock.** The engine tracks `position`, measured in steps from the start
  of the song. Step onsets are expressed in the same unit, so there is no second
  cursor that can drift out of step with the first. Onsets are recomputed from
  `position` every block, so a rounding error cannot accumulate. The head is
  advanced only by whole rendered sample counts, and an onset is quantised to the
  sample it falls on with the sub-sample remainder left behind: carrying that
  remainder forward is what used to leak up to a sample per onset and slide the
  standalone clock a third of a step over thirty-two bars.
- **Block splitting.** A block is split at a step onset so a hit starts on its
  exact sample rather than on the host's buffer boundary. With a host, the
  position comes from `AudioPlayHead`; without one it advances by the block
  length, which keeps playback identical in the standalone build.
- **No locks on the audio thread.** The editor writes atomics that the engine
  reads. The sample registry is append-only and published through atomic pointers
  — a sample is never removed, moved or reused once it is live — so the audio
  thread resolves a channel id to a reference-counted sample without taking any
  lock, and replacing a sound while it is still sounding cannot free the buffer
  underneath the voice playing it. (It used to take a spinlock here, which on an
  audio thread is a priority inversion waiting to happen.) The price is a
  ceiling of `SampleStore::maxSamples` loaded samples per session, which is
  reported as an error rather than silently exceeded.
- **Parameters, not a private state tree.** Continuous controls live in an
  `AudioProcessorValueTreeState`, which is what makes them reachable from a host.
  Every setter on the processor funnels through one private helper,
  `writeParameter()`, so a drag on the panel and a ramp drawn in the host arrive
  at the engine by the same path and both notify the host back. The engine reads
  the parameter atomics, never the tree, so the audio thread stays off the message
  thread.
- **The grid is one property, not 192 parameters.** A host's automation list would
  be buried by a parameter per cell, and nobody draws a step as a lane. So the
  pattern travels as a string on the root of the parameter state: `v3:` followed by
  base64 of three bytes a step — the switched-on flag, the level as a 7-bit MIDI
  number, then the probability as a number of 255ths — which is 771 characters for
  the whole grid. The level and the probability travel whether or not the step is
  lit, which is what lets a step keep the values you set while it was still switched
  off; 127 divisions is every velocity a controller can send, and 255 is as fine a
  slice of "how often" as one byte can name. The root also carries `stateVersion`,
  and the payload names its own codec as well, so a string says what it is instead of
  being guessed at from how long its rows happen to be. Sessions from the older
  two-bytes-a-step, two-digits-a-step and one-digit-a-step formats still load — the
  width is read from the tag, not assumed — and saving one writes it back in the
  current format, so a step from a two-byte record comes in meaning *fires every
  time*, which is what it did before there was a choice.
- **A take, not a shimmer.** Whether a lit step sounds is decided by one function,
  `SequencerEngine::wouldThisHitFire`, keyed on the seed and on the bar, step and
  lane of the hit — splitmix64 over those four, and the top 24 bits of the answer
  compared against a threshold. There is no counter of rolls and no clock in it, so
  the same place in the same song rolls the same way on every pass: a bounce is an
  A/B of the take rather than a different take each time you print it. The threshold
  is the step's own probability discounted by the *Mutate* amount — `1 - mutate *
  (1 - chance)` — so opening the knob can only ever take hits away, never add one the
  grid does not claim, and a step at 100 % is never dropped. Two gates mean the
  common case never reaches a generator at all: variation shut, or a step that fires
  every time, and that is what keeps a session with the knob closed bit-identical to
  the build that had no knob.
- **A record this build cannot read is kept, not repaired.** A payload whose codec tag
  names a newer generation — from a session saved by a later build, or a preset
  someone edited by hand — is not decoded, and goes back out byte for byte so that
  opening it cannot cost you the file. The grid shows nothing, because an empty grid is
  the honest picture of what this build can hear, and the first edit of a step takes
  the pattern over in this build's own format.
- **Sample rate and pitch conversion.** A voice reads its sample through a
  precomputed windowed-sinc kernel rather than by linear interpolation. Linear
  interpolation is not a filter: pitch a sample up and everything above the
  output's Nyquist folds back into the audible band, and at a ratio of exactly
  two it does not even interpolate — it decimates. The table is built when the
  engine is constructed, off the audio thread. A voice picks its cutoff band at
  note-on from the fastest rate the hit is ever going to read at — the top of a
  pitch drop — and the render loop opens it again as that glide settles: holding
  it shut for the whole tail cost 3.3 dB of top end at a drop of 12, which makes a
  tuning control a tone control. Switching bands is not a step in the sound either
  way, because these kernels are symmetric about the same centre tap, so below the
  tighter of the two cutoffs they are the same signal. Normalising each band to
  unity is what keeps a resampled hit at the level it was
  recorded at, and that promise only holds while every tap has something to read:
  at either end of the source the engine sums the weights it actually used and
  divides back by them, so the truncated window keeps the same DC gain instead of
  starting a pitched hit a little hot.
- **Peak detection lives on the audio side.** The editor polls a few times a
  second, too slow for a drum hit, so the master bus holds the peak it has seen
  and lets it decay between blocks instead of asking the GUI to catch it.

## Known limitations

- Samples are referenced by file path. A session moved to another machine, or a
  sample that has since been deleted or moved, will come back silent.
- Audio dropped from outside the app is decoded into memory but has no path, so
  it is not restored with the session.
- One 16-step bar; there is no pattern chaining or song mode yet.
- No per-channel pan, and no group or submix routing.
- The loaded-sample registry is capped at `SampleStore::maxSamples` per session;
  past that, a load reports an error instead of growing the list.
