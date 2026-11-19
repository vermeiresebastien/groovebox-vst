# Third-party notices

Groovebox's own source is AGPL-3.0-or-later (see [`LICENSE`](LICENSE)). This file names the
other copyrighted work that reaches a user's machine, and what each one's terms ask.

## In the build, not in the repository

**`external/` is gitignored and carries no tracked files**, so a clone holds none of this —
the first `cmake` configure clones JUCE into it. That is deliberate: vendoring a copy would
put someone else's licence text under *this* repository's control, and a fetched copy keeps
provenance where it can be checked.

| Component | Where it comes from | Licence |
|---|---|---|
| **JUCE 8.0.4** | cloned from <https://github.com/juce-framework/JUCE.git> into `external/JUCE` at configure time | dual: **AGPL-3.0** or the commercial **JUCE 8 licence** |
| **VST3 SDK** (the subset JUCE ships, `modules/juce_audio_processors/format_types/VST3_SDK`) | inside JUCE, not added by us | dual: **Proprietary Steinberg VST3 licence** or **GPL-3.0** |
| **AAX SDK**, **AudioUnitSDK**, **LV2 SDK** | inside JUCE | Avid proprietary / Apache-2.0 / ISC. Not reachable here: this build makes `VST3` and `Standalone` targets only |

Groovebox takes the **AGPL** option on JUCE, which is why this repository and every build
conveyed from it are AGPL. Anyone who holds a commercial JUCE 8 licence may build under it
instead; that choice is theirs and does not change the grant this repo makes to anyone else.
The README's [Licence](README.md#licence) section states what the grant obliges.

## Compiled into the executable through JUCE's modules

Codecs and support libraries that are linked into `Groovebox.exe` and `Groovebox.vst3`. All
of them are permissive and none of them is what makes the whole AGPL.

| Library | Licence | Function |
|---|---|---|
| **zlib** | zlib licence | the DEFLATE used by JUCE's zip/GZIP support and by PNG |
| **libpng** | libpng licence | PNG reading and writing (the `--shots` renders are PNG files it wrote) |
| **jpeglib** | Independent JPEG Group licence | JPEG reading |
| **libFLAC** | BSD-style | FLAC sample loading |
| **Ogg** and **libVorbis** | BSD-style | `.ogg` / `.oga` sample loading |

Presence was measured on the shipped bytes, not assumed from the source list — grep the
artefact for its own markers:

```powershell
grep -ao "libpng version [0-9.]*" launch\Groovebox.exe   # -> libpng version 1.6.37
grep -ao "JUCE v[0-9.]*"           launch\Groovebox.exe   # -> JUCE v8.0.4
```

For the full dependency list and each licence's own text, read `LICENSE.md` in the JUCE
checkout under `external/JUCE`, which is the copy this build was actually compiled against.

## Not source, and not ours to license

- **Inno Setup 6** builds `Groovebox Setup <version>.exe`. It is a tool, used under its own
  free-for-use licence (<https://jrsoftware.org/>); the compiler's stub inside the setup is
  unmodified Inno Setup, not Groovebox code.
- **Ableton Live, and every other host**, is only ever the thing that loads the bundle.
  Nothing of theirs is in the repository or the payload.

## What is *not* here

No samples, loops, one-shots or presets ship with Groovebox — not in the repository, not in
the installer. Every sound the plugin plays is a file the user dragged in, remembered by its
absolute path in the session; the payload is the executable, the bundle, `README.txt` and
`LICENSE.txt`. The twelve file extensions in the README are formats the codecs above can
decode, not content licensed to anybody.

Nothing in this project is code-signed, on purpose: a certificate costs money the author has
not spent on it, and Windows saying "unknown publisher" is the honest state of a free build.
