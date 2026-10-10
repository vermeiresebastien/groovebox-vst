# Groovebox

A VST3 / standalone drum machine built from the drum page of **Earworm Studio**
(`earworm-studio/index.html`): a 16-step sequencer where each drum channel is a
sample you drag onto its row.

## What it does

| Feature | Detail |
|---|---|
| Step grid | 16 steps per bar, 12 drum channels, painted in the channel's own colour |
| Sample loading | Drag a `.wav` / `.aiff` / `.mp3` / `.flac` / `.ogg` onto any channel **row**, or click the channel's sound button to browse |
| Per-channel pitch | −36 to +12 semitones, with `-12` / `+12` octave buttons |
| Swing | 0–100 % on either a 1/8 or 1/16 grid |
| Tempo | Follows the host project tempo; in the standalone build it has its own transport and tempo |
| Mix | Per-channel level and mute, plus a "clear row" button |
| Row editing | Click a cell to toggle it; drag across cells to paint a run |
| Persistence | Pattern, pitch, levels, mutes, names, colours and sample paths are saved in the session |

## Layout

- **Top bar** — transport, tempo, swing and grid, and "Clear pattern".
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
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release --parallel 2
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
| `Groovebox_VST3` | `build/Groovebox_artefacts/Release/VST3/Groovebox.vst3` |
| `Groovebox_Standalone` | `build/Groovebox_artefacts/Release/Standalone/Groovebox.exe` |
| `GrooveboxSmokeTest` | `build/GrooveboxSmokeTest_artefacts/Release/GrooveboxSmokeTest.exe` |

## Installing

Build the plugin first (above), then copy the whole `Groovebox.vst3` **folder**
(not just the inner binary) into your VST3 folder and rescan in your DAW.

Which folder depends on your DAW:

| DAW | VST3 folder |
|---|---|
| Ableton Live | `C:\Program Files\Common Files\VST3\` (its default), or any extra folder you add under **Settings → Plug-Ins → VST3 Plug-In Custom Folder** |
| Most others | `C:\Program Files\Common Files\VST3\` |

```powershell
# e.g. a user-writable folder, which needs no administrator rights
$dest = "$env:USERPROFILE\Documents\VST3"
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item -Recurse -Force build\Groovebox_artefacts\Release\VST3\Groovebox.vst3 $dest
```

`C:\Program Files\Common Files\VST3\` needs administrator rights. Note that
Ableton Live scans only that folder by default, so if you install somewhere else
you must point Live at it (the table above).

The **standalone** app needs no installation at all — just run
`build\Groovebox_artefacts\Release\Standalone\Groovebox.exe`.

## Verifying the engine

`GrooveboxSmokeTest` drives the real processor with no UI and no host and checks
the parts that are easy to get subtly wrong:

```powershell
build\GrooveboxSmokeTest_artefacts\Release\GrooveboxSmokeTest.exe
```

It asserts, among other things, that:

- an empty engine is silent, and a hit fires exactly once;
- every one of the 16 steps lands within **2 samples** of its grid position at
  120 bpm, regardless of the host block size;
- swing places every step on the onset the web app's formula specifies, on both
  the 1/8 and 1/16 grids;
- an octave of pitch shortens a sample, mute silences a channel, and a lower
  level is quieter;
- a mono sample stays centred;
- pattern, pitch, names, swing and grid survive a save/load round trip.

## How it is put together

```
Source/
  Common.h              layout constants (16 steps, 12 channels) and the palette
  SampleStore.{h,cpp}   sample decoding, and the channel -> sample assignment
  SequencerEngine.{h,cpp}  the scheduler and the voices (the audio thread)
  PluginProcessor.{h,cpp}  the AudioProcessor, transport and state persistence
  PluginEditor.{h,cpp}     window layout
  StepGridComponent.{h,cpp}  the grid, and the file drag-and-drop target
  ChannelStrip.{h,cpp}      one channel's controls
  TransportBar.{h,cpp}      transport, tempo, swing
  LookAndFeel.{h,cpp}       the flat dark theme
Tests/
  SmokeTest.cpp         the headless engine test
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
  `position` every block, so a rounding error cannot accumulate.
- **Block splitting.** A block is split at a step onset so a hit starts on its
  exact sample rather than on the host's buffer boundary. With a host, the
  position comes from `AudioPlayHead`; without one it advances by the block
  length, which keeps playback identical in the standalone build.
- **No locks on the audio thread.** The editor writes atomics that the engine
  reads. Samples are reference-counted, so replacing a sound while it is still
  sounding cannot free the buffer underneath the voice playing it.
- **Sample rate conversion.** A sample whose rate differs from the session's is
  resampled by the voice's playback increment, so no per-sample work is added.

## Known limitations

- Samples are referenced by file path. A session moved to another machine, or a
  sample that has since been deleted or moved, will come back silent.
- Audio dropped from outside the app is decoded into memory but has no path, so
  it is not restored with the session.
- One 16-step bar; there is no pattern chaining or song mode yet.
- No MIDI note input, and no per-channel pan.
