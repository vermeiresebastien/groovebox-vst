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
| **ADSR per channel** | **Attack, Decay, Sustain and Release for every channel, so a hit can be tightened or turned into a sustained sound** |
| Swing | 0–100 % on either a 1/8 or 1/16 grid |
| Tempo | Follows the host project tempo; in the standalone build it has its own transport and tempo |
| Mix | Per-channel level and mute, plus a "clear row" button |
| **Master trim & meter** | **A −24 to +6 dB master trim you can see and reach, with a peak meter and a clip latch on the output** |
| **Automation** | **Levels, pitches, envelopes, mutes, swing, tempo and the master trim are host parameters — automate them, and the panel follows what the host drew** |
| Row editing | Click a cell to toggle it; drag across cells to paint a run |
| **Shift a row** | **The ← and → buttons in a channel strip turn that whole row one step earlier or later, and every step's velocity goes with it** |
| Persistence | Pattern, pitch, levels, mutes, names, colours and sample paths are saved in the session; a session saved before the parameter migration is read in and its values carried over |

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
| Decay | 0 – 3 s | Falls from full to the sustain level |
| Sustain | 0 – 100 % | 0 = one-shot; above 0 the note holds until released |
| Release | 0 – 4 s | How long the tail takes to fade after release |

The times are on a squared curve, so the low end of a row's travel is usable —
most drum shaping happens in the first few milliseconds, and a linear control
would put all of that in its first percent. Each row shows its value in
milliseconds, seconds or percent.

## Playing it from MIDI

The channels answer **C1 upwards** — C1 plays channel 0, C#1 channel 1, and so on
to B1 for channel 11. That is the range a General MIDI drum map uses for its first
twelve pieces, and what most pad controllers send out of the box, so a kick pad
plays the kick without any setup. The note range is shown in the top bar.

- **Velocity scales the hit**, so a pad or keyboard plays dynamically. The step
  grid always plays at full velocity.
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
with the clip indicator marking 0 dBFS. It reads a peak held on the audio side —
the editor polls a few times a second, far too slowly for a drum transient, so
the bus keeps the highest value it saw and lets it fall away between blocks.
Anything reaching 0 dBFS latches the clip indicator, and **the latch is only
cleared by clicking the meter**. There is no limiter: if a kit is too hot, the fix
is the trim or the channel levels, not a processor quietly reshaping what you
mixed.

## Automation

Every continuous control is a proper host parameter, so it can be drawn as
automation instead of only moved by hand: per-channel level, pitch, A/D/S/R and
mute (7 parameters × 12 channels), plus master trim, swing, swing grid and
tempo — 88 in total, grouped per lane so a host lists them sensibly.

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
host moves the fader, the mute button, the pitch readout and the envelope rows in
front of you. Every control writes straight through when you move it, so during a
drag the poll reads back the value already under the pointer and leaves it alone.

## Layout

- **Top bar** — transport, tempo, swing and grid, master trim and meter, and
  "Clear pattern".
- **Middle** — the 16 × 12 step grid. Channel 11 is the top row and channel 0 the
  bottom, matching the web app, which reverses its lane list.
- **Bottom** — one strip per channel: name (double-click to rename), colour
  swatch (click to cycle), sound, pitch, level, mute.

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

That gives you `launch\Groovebox.exe` and `launch\Groovebox.vst3` — the one
folder to run from and the one folder to install from, whatever the build tree
happens to look like. It is a target rather than an automatic post-build copy
because Ableton Live holds the plugin DLL open: a locked bundle should fail the
release step loudly, not break the build you were trying to test. If the copy
fails, quit Live and run it again.

## Installing

Build the plugin first (above), then copy the whole `Groovebox.vst3` **folder**
(not just the inner binary) from `launch/` into your VST3 folder and rescan in
your DAW.

Which folder depends on your DAW:

| DAW | VST3 folder |
|---|---|
| Ableton Live | `C:\Program Files\Common Files\VST3\` (its default), or any extra folder you add under **Settings → Plug-Ins → VST3 Plug-In Custom Folder** |
| Most others | `C:\Program Files\Common Files\VST3\` |

```powershell
# e.g. a user-writable folder, which needs no administrator rights
$dest = "$env:USERPROFILE\Documents\VST3"
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item -Recurse -Force launch\Groovebox.vst3 $dest
```

`C:\Program Files\Common Files\VST3\` needs administrator rights. Note that
Ableton Live scans only that folder by default, so if you install somewhere else
you must point Live at it (the table above).

The **standalone** app needs no installation at all — just run
`launch\Groovebox.exe`.

### Ableton Live specifics

Live does **not** scan `Documents\VST3` by default. Its scan database showed only
`C:\Program Files\Common Files\VST3`, so a plugin placed anywhere else is simply
invisible until you tell Live where to look:

1. **Settings → Plug-Ins**
2. Tick **Use VST3 Plug-In System Folder** (keeps your existing plugins working)
3. Tick **VST3 Plug-In Custom Folder** and point it at the folder you used
4. Click **Rescan** — hold `Alt` while clicking to force a full rescan

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
- a mono sample stays centred;
- pattern, pitch, names, swing and grid survive a save/load round trip;
- an incoming note plays its channel with the transport stopped, a note outside
  the mapped range stays silent, velocity scales the hit, a muted channel ignores
  its note, and a note off leaves the one-shot ringing;
- the envelopes behave: sustain 0 makes a one-shot while sustain 1 holds, a note
  off releases a held note, and a longer attack, decay or release measures longer
  in the audio;
- the envelope values survive a save/load round trip, and a channel that was never
  touched keeps the defaults;
- step velocities survive a save and load, including a very quiet step, and an off
  step never comes back as on;
- shifting a row carries each step's velocity with it, wraps the last step to the
  first, is a no-op over a whole bar, and leaves every other row alone;
- a value written into a **parameter** the way a host writes it — normalised,
  straight through `setValueNotifyingHost`, without touching any of the
  processor's own setters — reaches the engine and changes the audio, so all 88
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
  MasterBus.h           the master trim, peak meter and clip latch
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
- **Sample rate and pitch conversion.** A voice reads its sample through a
  precomputed windowed-sinc kernel rather than by linear interpolation. Linear
  interpolation is not a filter: pitch a sample up and everything above the
  output's Nyquist folds back into the audible band, and at a ratio of exactly
  two it does not even interpolate — it decimates. The table is built when the
  engine is constructed, off the audio thread, and a voice picks a cutoff band at
  note-on so the kernel never changes within a hit.
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
