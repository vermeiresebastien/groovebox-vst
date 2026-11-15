# Groovebox -- agent instructions

JUCE 8.0.4 C++17 sample step sequencer (16 steps x 12 channels, per-channel pitch,
ADSR and velocity, swing, master trim + peak meter). VST3 + Standalone. Plugin
manufacturer code `Eruw`, product code `Grvb`, version from `project(... VERSION ...)`
in [CMakeLists.txt](CMakeLists.txt).

## Deliverables: `launch/` is the one place to build, run and ship from

```powershell
cmake --build build-new --config Release --parallel 2 --target launch   # exe + bundle -> launch/
.\installer\build-installer.ps1                                         # refreshes launch/, then packages the setup
```

`launch/` holds `Groovebox.exe`, `Groovebox.vst3/` and `Groovebox Setup <version>.exe`.
It is fully gitignored and nothing in it is authored in place.

- Verify an export by **SHA-256 against the artefact tree**, never by timestamp or size:
  `copy_if_different` leaves an old mtime on a file that exported correctly, and rebuilds
  often coincide on size. Current export: `Groovebox.exe 6F9E5D48...`,
  bundle binary `7483D04A...`.
- The bundle is **removed before being rewritten** ([CMakeLists.txt:152-163](CMakeLists.txt))
  so a file that no longer belongs to the build cannot survive in it.
- `launch` is a **target, not a POST_BUILD step**, on purpose: a host with the plugin
  loaded holds the DLL open, and failing a compile over a copy only the release step
  needs would hide the real build result. Check `tasklist` -- the process is
  `Ableton Live 12 Suite.exe`, not `Live.exe` -- and let the user close it rather than
  killing it: the standalone does not autosave.
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
- Anything that reads like a spec in [README.md](README.md) or
  [installer/README.txt](installer/README.txt) must be code-verified before shipping.
  Three false claims were caught this way: that the sound button's menu switches between
  loaded samples (it only opens a `FileChooser`; `SampleStore::getSampleIds()` has no UI
  caller), that there is no MIDI note input (there is, C1-B1 = notes 36-47,
  `Layout::firstMidiNote` in [Source/Common.h:27](Source/Common.h)), and that Live scans
  `Documents\VST3` (it does not -- see Host folders below).

## Tests: two console apps, no CMake test target

```powershell
build-new\GrooveboxSmokeTest_artefacts\Release\GrooveboxSmokeTest.exe   # timing, swing, pitch, state round-trip, automatability
build-new\GrooveboxLayoutTest_artefacts\Release\GrooveboxLayoutTest.exe # real editor, bounds of every control
```

Both compile the plugin sources headlessly ([CMakeLists.txt:166-221](CMakeLists.txt));
they print a check count and a problem count, and exit non-zero on failure. Last green
run: 123 smoke checks, 63 layout checks, 0 problems.

- No modal loops: `MessageManager::runDispatchLoopUntil` needs
  `JUCE_MODAL_LOOPS_PERMITTED` and JUCE 8 has no public single-step message pump, so the
  layout test drives `ChannelStrip::refresh()` -- the same `syncAutomatableControls()`
  path the 30 ms timer uses -- instead of pumping messages.
- Automatability is testable single-threaded because
  `AudioProcessorParameter::setValueNotifyingHost` notifies listeners synchronously.
- JUCE 8 removed `AudioProcessorValueTreeState::setParameterNotifyingHost`; the one
  remaining way in is `apvts.getParameter (id)->setValueNotifyingHost
  (apvts.getParameterRange (id).convertTo0to1 (v))`, wrapped as
  `GrooveboxProcessor::writeParameter` ([Source/PluginProcessor.cpp:133](Source/PluginProcessor.cpp)).
  88 parameters (4 global + 12 x 7); the pattern is deliberately not one.

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
