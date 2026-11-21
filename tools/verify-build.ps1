<#
.SYNOPSIS
  One command from dirty source tree to a shippable setup, with every step refusing out loud.
.DESCRIPTION
  Item 6.1, promoted. Until now the ship chain was a sequence of habits: build, remember to check the
  exit code, remember to hash, remember to snapshot `launch/` before the copy step overwrites the
  outgoing export. Two of those habits have already failed in the open -- `AGENTS.md` records an export
  whose loose bytes were destroyed by a rebuild with no snapshot (`3c3f059`), and `CTest does not build`
  is the reason the run order below is build-then-ctest and never the reverse.

  What this adds over `installer/build-installer.ps1`, which already refreshes `launch/` and runs ISCC
  but only `Test-Path`s five files and hashes nothing:

    * a freshness guard **per suite target**, because each console app compiles `Source/` plus its own
      `Tests/*Test.cpp` and a guard over all of `Tests/` can never pass once only the other test moved;
    * the pre-build snapshot of `launch/`, which is the one step that cannot be redone afterwards;
    * a SHA-256 census of `launch/` against the artefact tree, in both directions, so a file that exists
      on one side only is a refusal rather than a shrug;
    * a manifest that outlives the run, because a hash quoted from a console buffer is a memory test.

  Scope, stated so a green report is not read as more than it is: this builds the three suites, runs the
  gate, then builds `launch` and packages. It does NOT install -- elevation is a human click -- and it
  does not push, tag or publish.
#>
[CmdletBinding()]
param(
    [string]$RepoRoot   = '',
    [string]$BuildDir   = 'build-new',
    [switch]$SkipInstaller,
    [int]   $WantSmoke  = 628,
    [int]   $WantLayout = 730,
    [int]   $WantPlan   = 86,
    [string]$WantFinger = '4BC3AA231178C071'
)

$ErrorActionPreference = 'Continue'

# Everything resolves from this file, never from a typed absolute path: the first copies of these
# harnesses lived in gitignored `out/` with `D:\Projects\...` baked in, which made them machine-local.
#
# The resolution happens in the BODY, not in the param default, and that is not stylistic. Measured on
# the first run of this file: `$RepoRoot = (Split-Path -Parent $PSScriptRoot)` as a default raised
# "Cannot bind argument to parameter 'Path' because it is an empty string" -- `$PSScriptRoot` is not
# populated while defaults are evaluated -- and the script then printed one error, ran no stage, and
# exited 0. A gate whose worst failure is a silent zero is the failure AGENTS.md already names about
# the clone harness, so the root is resolved here and then PROVED below before anything is read.
if ($RepoRoot -eq '') { $RepoRoot = Split-Path -Parent $PSScriptRoot }
$repo   = $RepoRoot
$cmake  = Join-Path $repo 'CMakeLists.txt'
if (-not (Test-Path $cmake)) {
    Write-Output ("REFUSE: '$repo' is not the repository root (no CMakeLists.txt). " +
                  "Pass -RepoRoot. Nothing ran, and this script exits nonzero for exactly that reason.")
    exit 3
}
$build  = Join-Path $repo $BuildDir
$build  = Join-Path $repo $BuildDir
$launch = Join-Path $repo 'launch'
$arte   = Join-Path $build 'Groovebox_artefacts\Release'
$sha    = ((git -C $repo rev-parse HEAD) -join '').Trim()
$short  = $sha.Substring(0, 7)
$dirty  = @(git -C $repo status --porcelain).Count
$logDir = Join-Path $repo ("out\ship-run-" + $short)
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

function Say ([string]$m) { Write-Output $m }

# Print on one stream, return on another. A helper that does both leaves the caller holding an array
# whose first element is a sentence, and `if ($rc -ne 0)` compares that array with 0 and gets the
# sentence back -- truthy. Measured here on 2026-11-16, on the clone gate.
function Run ([string]$what, [string]$file, [scriptblock]$body) {
    & $body 2>&1 | ForEach-Object { [string]$_ } | Out-File -Encoding utf8 $file
    $rc = $LASTEXITCODE
    Write-Host ("  " + $what + " exit=" + $rc + "  log=" + $file)
    return $rc
}

function Sha256Of ([string]$path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLower() }

function Fwd ([string]$p) { $p.Replace('\', '/') }

# The two hash records are written by hand rather than with Out-File, for three reasons measured on the
# first pass: PowerShell 5.1's `Out-File -Encoding utf8` prepends a BOM, CRLF makes every sha256sum-style
# replay need a dos2unix step, and the tree walk returns NATIVE paths, so a name built by joining a
# forward-slash prefix to a relative path came out `Groovebox.vst3/Contents\Resources\moduleinfo.json`.
# LF, no BOM, one separator, `<hash>  <path>` and nothing else per line -- so the manifest is checkable
# from the repo root and not only readable.
function WriteRecord ([string]$path, $lines) {
    $text = ((@($lines) -join "`n") + "`n")
    [IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding($false)))
}

# The three suites, each with the exe path and the file set THAT target compiles. This table is the
# only place the trio is spelled out: a list written twice is how a fourth control gets misfiled.
#
# The input sets are deliberately NOT one glob. The planner target compiles two headers and no JUCE, so
# a guard that watched all of `Source/` would call it stale on every engine edit and refuse permanently
# -- and a guard that can never pass is not a guard, it trains the habit of editing the script.
# `unit` is the word that suite prints for its problem count, because the three do not agree.
$suites = @(
    @{ name = 'GrooveboxSmokeTest';   exe = "$BuildDir\GrooveboxSmokeTest_artefacts\Release\GrooveboxSmokeTest.exe";
       want = $WantSmoke;  unit = 'failures';
       inputs = @(@{ dir = 'Source'; include = '*.cpp', '*.h' }, @{ file = 'Tests\SmokeTest.cpp' },
                  @{ file = 'CMakeLists.txt' }) },
    @{ name = 'GrooveboxLayoutTest';  exe = "$BuildDir\GrooveboxLayoutTest_artefacts\Release\GrooveboxLayoutTest.exe";
       want = $WantLayout; unit = 'problems';
       inputs = @(@{ dir = 'Source'; include = '*.cpp', '*.h' }, @{ file = 'Tests\LayoutTest.cpp' },
                  @{ file = 'CMakeLists.txt' }) },
    @{ name = 'GrooveboxPatternPlanTest'; exe = "$BuildDir\Release\GrooveboxPatternPlanTest.exe";
       want = $WantPlan;   unit = 'problems';
       inputs = @(@{ file = 'Source\GenrePatterns.h' }, @{ file = 'Source\PatternPlanner.h' },
                  @{ file = 'Tests\PatternPlanTest.cpp' }, @{ file = 'CMakeLists.txt' }) }
)

Say "=== Groovebox ship verification =========================================="
Say ("SHA        : " + $sha)
Say ("DIRTY FILES: " + $dirty + $(if ($dirty) { "  (work in progress -- the report describes this tree, not a commit)" } else { "" }))
Say ("SCOPE      : 3 suites + ctest + launch/ census + installer. No install, no push, no tag.")
Say ("START      : " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))

# ---- 1. freshness guard, per target, BEFORE building --------------------------
# A build that quietly decides nothing is stale prints exit 0 and runs a binary that does not contain
# the change under test. `Copy-Item` preserves the source's mtime, so a file restored by copy can be
# older than the object built from the version it replaced.
#
# A guard whose input list comes back empty is a guard that passes: a typo in one of the table's paths
# would make "nothing is newer than the exe" true by arithmetic, so the count is checked and a zero is
# a refusal, not a clean line.
Say "`n[1] freshness guard (per target)"
foreach ($s in $suites) {
    $exePath = Join-Path $repo $s.exe
    $inputs = foreach ($src in $s.inputs) {
        if ($src.dir) { @(Get-ChildItem -LiteralPath (Join-Path $repo $src.dir) -Recurse -Include $src.include) }
        else { @(Get-Item -LiteralPath (Join-Path $repo $src.file) -ErrorAction SilentlyContinue) }
    }
    $inputs = @($inputs | Where-Object { $_ })
    if ($inputs.Count -eq 0) {
        Say ("  REFUSE  " + $s.name + ": its declared inputs matched no file, so the comparison would be vacuous.")
        Say ("          table entries: " + (($s.inputs | ForEach-Object { if ($_.dir) { $_.dir } else { $_.file } }) -join ', '))
        exit 3
    }
    if (-not (Test-Path $exePath)) { Say ("  ABSENT     " + $s.name + " (" + $inputs.Count + " inputs) -- building it below, nothing to compare yet"); continue }
    $stamp = (Get-Item $exePath).LastWriteTime
    $stale = @($inputs | Where-Object { $_.LastWriteTime -gt $stamp })
    if ($stale.Count -gt 0) {
        Say ("  NEWER-THAN-EXE  " + $s.name + " (" + $stale.Count + " of " + $inputs.Count + " inputs):")
        # Date AND clock: the first version of this line printed HH:mm:ss alone, so a source last touched
        # two days before the export looked like it had been edited minutes ago -- which is the wrong
        # sentence next to a number meant to justify a rebuild.
        $stale | ForEach-Object { Say ("    " + $_.Name + "  " + $_.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')) }
        Say ("      exe stamped " + $stamp.ToString('yyyy-MM-dd HH:mm:ss') + " -- stage 2 rebuilds it.")
        Say "  That is not a failure here. It IS a reason not to trust any earlier run of this exe."
    } else {
        Say ("  current      " + $s.name + "  (exe " + $stamp.ToString('yyyy-MM-dd HH:mm:ss') + ", " + $inputs.Count + " inputs all older)")
    }
}

# ---- 2. build the suites, per target so a failure names its target ------------
Say "`n[2] build suites (--parallel 2, no /MP: MSVC 14.51 emits D8040 when both layers parallelise)"
foreach ($s in $suites) {
    $rc = Run ("build " + $s.name) (Join-Path $logDir ("build-" + $s.name + ".log")) {
        & cmake --build $build --config Release --parallel 2 --target $s.name
    }
    if ($rc -ne 0) { Say ("FAILED: build " + $s.name + ". Tail:"); Get-Content (Join-Path $logDir ("build-" + $s.name + '.log')) -Tail 25; exit 1 }
}

# ---- 3. run each exe, exit code owned directly, verdict line matched ---------
Say "`n[3] run suites"
$report = @()
foreach ($s in $suites) {
    $exePath = Join-Path $repo $s.exe
    $out = Join-Path $logDir ($s.name + '.txt')
    # `$LASTEXITCODE` is read on the next line and nowhere else: a pipe or a `$(...)` steals it.
    & $exePath | ForEach-Object { [string]$_ } | Out-File -Encoding utf8 $out
    $rc = $LASTEXITCODE
    $text = Get-Content $out -Raw
    # The verdict grammar is `<N> checks, <M> <unit>` and the two numbers are matched SEPARATELY on
    # purpose: the first cut of this line hard-coded ", 0 " into the pattern, so a suite that ran and
    # reported three failures printed "NO VERDICT LINE" -- a parse bug wearing the costume of a missing
    # run. Ask for the shape, then judge both numbers, and the report says which one moved.
    $cm = [regex]::Match($text, '(\d+) checks, (\d+) ' + $s.unit)
    $seen = if ($cm.Success) { [int]$cm.Groups[1].Value } else { -1 }
    $probs = if ($cm.Success) { [int]$cm.Groups[2].Value } else { -1 }
    $finger = ''
    if ($s.name -eq 'GrooveboxSmokeTest') {
        # Presence only, and that is the honest scope: [28] inside the suite is what proves the two-sided
        # property (the hash repeats on a re-render AND moves on a perturbation). Here the question is the
        # cheaper one -- did the build under test still reach the recorded fingerprint for the fixed kit.
        $finger = if ($text -match ('(?i)' + $WantFinger)) { 'match' } else { 'ABSENT' }
    }
    $verdict = if ($rc -ne 0) { "EXIT $rc" }
               elseif ($seen -lt 0) { 'NO VERDICT LINE' }
               elseif ($probs -ne 0) { "$probs $($s.unit)" }
               elseif ($seen -ne $s.want) { "COUNT $seen, wanted $($s.want)" }
               elseif ($finger -eq 'ABSENT') { 'FINGERPRINT ABSENT' }
               else { "ok ($seen, 0 $($s.unit))" }
    Say ("  " + $s.name.PadRight(26) + " exit=" + $rc + "  " + $verdict)
    $report += ("{0,-26} exit={1,-3} {2}" -f $s.name, $rc, $verdict)
    if ($rc -ne 0 -or $seen -ne $s.want -or $probs -ne 0 -or $finger -eq 'ABSENT') {
        Say "FAILED: the suite did not report what the gate asks for. Tail:"
        Get-Content $out -Tail 20
        exit 1
    }
}

# ---- 4. ctest, AFTER the build (CTest does not build) -----------------------
Say "`n[4] ctest (replay of the tracked gate, four entries incl. the source guard)"
$rc = Run "ctest" (Join-Path $logDir '04-ctest.log') {
    & ctest --test-dir $build -C Release -V
}
if ($rc -ne 0) { Say "FAILED: ctest. Tail:"; Get-Content (Join-Path $logDir '04-ctest.log') -Tail 30; exit 1 }
Say ("  guard line: " + $(if ((Get-Content (Join-Path $logDir '04-ctest.log') -Raw) -match 'GUARD OK') { 'GUARD OK' } else { 'not printed -- check the log' }))

# ---- 5. snapshot launch/ BEFORE anything overwrites it ----------------------
# The one step with no second chance: `launch/` and Program Files are the only two loose copies of a
# build and the `launch` target writes over the first by design. Archived here rather than entrusted to
# remembering, because it was forgotten once and a build survives only inside its own setup exe.
Say "`n[5] snapshot the outgoing launch/ (before the launch target can touch it)"
$pre = Join-Path $repo ("out\launch-pre-" + $short)
if (-not (Test-Path $launch)) {
    Say "  no launch/ to snapshot -- this tree has never exported; nothing is being destroyed"
} elseif (Test-Path $pre) {
    Say ("  already archived: " + $pre)
} else {
    New-Item -ItemType Directory -Force -Path $pre | Out-Null
    Copy-Item -Path (Join-Path $launch '*') -Destination $pre -Recurse -Force
    # Each element is wrapped in its own parentheses, and that is not decoration: in PowerShell the comma
    # binds TIGHTER than `+`, so `@("a" + $x, "b" + $y)` parses as `"a" + ($x, "b") + $y` and yields ONE
    # space-joined string. Measured here -- the first FROM.txt this wrote had its two header lines merged
    # into one and `Count` was 1 where it should have been 2.
    #
    # `From:` names the CHECKOUT the snapshot was taken from, not the build inside it -- this script cannot
    # know which commit produced the bytes in launch/, and the mtimes below are the only in-folder evidence
    # of that. AGENTS.md's `launch-pre-<ship>` names are ships; this one is a run.
    $lines = @(("# Header: " + $sha + "  (dirty files at snapshot: " + $dirty + ")"),
               ("# Meaning: bytes that were in launch/ while HEAD was " + $short + ", not bytes built from it"),
               ("# Taken: " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + "  by tools/verify-build.ps1, before the launch target ran"))
    Get-ChildItem $pre -Recurse -File | ForEach-Object {
        $lines += ("{0}  {1}  (built {2})" -f (Sha256Of $_.FullName), (Fwd $_.FullName.Substring($pre.Length + 1)),
                   $_.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))
    }
    WriteRecord (Join-Path $pre 'FROM.txt') $lines
    Say ("  archived " + (@(Get-ChildItem $pre -Recurse -File).Count - 1) + " files + FROM.txt -> " + $pre)
}

# ---- 6. launch target + installer (build-installer.ps1 owns both) -----------
if ($SkipInstaller) {
    Say "`n[6] SKIPPED installer pass (-SkipInstaller) -- launch/ not rebuilt, census not owed"
} else {
    Say "`n[6] installer pass (refreshes launch/ from the artefacts, then ISCC)"
    $rc = Run "build-installer" (Join-Path $logDir '06-installer.log') {
        & (Join-Path $repo 'installer\build-installer.ps1')
    }
    if ($rc -ne 0) { Say "FAILED: installer. Tail:"; Get-Content (Join-Path $logDir '06-installer.log') -Tail 30; exit 1 }
}

# ---- 7. census: launch/ against the artefact tree, both directions ----------
# `copy_if_different` leaves an old mtime on a file that exported correctly and rebuilds coincide on
# size, so the only honest check is per-file bytes. A file present on one side only is a refusal: the
# bundle is removed before being rewritten precisely so a stale member cannot survive, and if one did,
# this is where it says so.
#
# Two shapes, because the two deliverables have different roots. The standalone is ONE file compared
# with ONE file. The bundle is a directory tree -- and its artefact parent also holds `Groovebox.exp`
# and `Groovebox.lib`, which are link leftovers and not payload, so the tree walk starts at the `.vst3`
# folder and not at `VST3/`. Getting this wrong is what the first run of this stage did: it took a file
# path as a directory root, every relative name came back empty, `Sha256Of $null` threw, `Continue` ate
# the exception -- and the only reason the run still failed was that the map lookup missed. Errors
# inside a census have to be refusals, not noise, hence the explicit precondition at the bottom.
Say "`n[7] hash census, launch/ vs the artefact tree"
$manifest = @(("# Groovebox ship manifest"), ("# sha " + $sha + "  dirty " + $dirty),
              ("# built " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')))
if (-not (Test-Path $arte)) { Say ("FAILED: no artefact tree at " + $arte + " -- run the launch target first"); exit 1 }

$mismatch = 0
$compared = 0

function RelPath ([string]$root, $file) {
    # Refuses rather than returning "": a silent empty name here becomes a null dictionary key, and a
    # null key makes every lookup below miss in a way that looks like a mismatch instead of a bug.
    $rootFull = (Resolve-Path -LiteralPath $root).Path.TrimEnd('\')
    if (-not $file.FullName.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) {
        Say ("  REFUSE  " + $file.FullName + " is not under " + $rootFull); $script:mismatch++
        return $null
    }
    return $file.FullName.Substring($rootFull.Length + 1)
}

$singleFiles = @(
    @{ label = 'standalone exe'; launch = (Join-Path $launch 'Groovebox.exe'); arte = (Join-Path $arte 'Standalone\Groovebox.exe') }
)
foreach ($p in $singleFiles) {
    if (-not (Test-Path $p.launch) -or -not (Test-Path $p.arte)) {
        Say ("  MISSING SIDE  " + $p.label + "  launch=" + (Test-Path $p.launch) + " artefacts=" + (Test-Path $p.arte))
        $mismatch++; continue
    }
    $a = Sha256Of $p.launch; $b = Sha256Of $p.arte
    $compared++
    if ($a -ne $b) { Say ("  HASH DIFFERS  " + $p.label + "  launch " + $a.Substring(0,12) + " vs artefacts " + $b.Substring(0,12)) }
    else { Say ("  equal         " + $p.label + "  " + $a) }
    $manifest += ("{0}  launch/{1}" -f $a, (Split-Path $p.launch -Leaf))
}

$trees = @(
    @{ label = 'vst3 bundle'; launch = (Join-Path $launch 'Groovebox.vst3'); arte = (Join-Path $arte 'VST3\Groovebox.vst3') }
)
foreach ($p in $trees) {
    if (-not (Test-Path $p.launch) -or -not (Test-Path $p.arte)) {
        Say ("  MISSING SIDE  " + $p.label + "  launch=" + (Test-Path $p.launch) + " artefacts=" + (Test-Path $p.arte))
        $mismatch++; continue
    }
    $dstMap = @{}
    foreach ($f in @(Get-ChildItem -LiteralPath $p.arte -Recurse -File)) {
        $n = RelPath $p.arte $f
        if ($n) { $dstMap[$n.ToLower()] = $f }
    }
    $seen = @{}
    foreach ($f in @(Get-ChildItem -LiteralPath $p.launch -Recurse -File)) {
        $rel = RelPath $p.launch $f
        if (-not $rel) { continue }
        $key = $rel.ToLower(); $seen[$key] = 1; $compared++
        if (-not $dstMap.ContainsKey($key)) { Say ("  ONLY IN launch/      " + $p.label + '  ' + $rel); $mismatch++; continue }
        $a = Sha256Of $f.FullName; $b = Sha256Of $dstMap[$key].FullName
        if ($a -ne $b) { Say ("  HASH DIFFERS       " + $p.label + '  ' + $rel); $mismatch++ }
        else { Say ("  equal              " + $p.label + '  ' + $rel) }
        $manifest += ("{0}  launch/{1}" -f $a, (Fwd ((Split-Path $p.launch -Leaf) + '/' + $rel)))
    }
    foreach ($k in $dstMap.Keys) { if (-not $seen.ContainsKey($k)) { Say ("  ONLY IN artefacts  " + $p.label + '  ' + $k); $mismatch++ } }
}
if ($compared -eq 0) { Say "FAILED: the census compared zero files -- it verified nothing"; exit 1 }
Say ("  files compared: " + $compared)

$setup = @(Get-ChildItem $launch -Filter 'Groovebox Setup *.exe' -File | Sort-Object LastWriteTime -Descending)
if ($setup.Count -eq 0) {
    if ($SkipInstaller) { Say "  no setup exe in launch/ (-SkipInstaller, expected)" }
    else { Say "FAILED: the installer pass reported success and launch/ holds no setup exe"; $mismatch++ }
} else {
    # The size goes on a comment line, not beside the digest: a manifest line has to stay
    # `<hash>  <path>` for `sha256sum -c` to read it, and the setup exe is the one entry a human also
    # wants the byte count of.
    foreach ($s in $setup) {
        $manifest += ("# size launch/" + $s.Name + " = " + $s.Length + " bytes")
        $manifest += ("{0}  launch/{1}" -f (Sha256Of $s.FullName), $s.Name)
    }
    Say ("  setup: " + $setup[0].Name + "  " + $setup[0].Length + " B  " + ((Sha256Of $setup[0].FullName).Substring(0, 12) + '...'))
    if ($setup.Count -gt 1) { Say ("  NOTE " + $setup.Count + " setup files in launch/ -- more than one deliverable folder copy is how the wrong build gets judged") }
}
if ($mismatch -gt 0) { Say ("FAILED: " + $mismatch + " census mismatch(es) above"); exit 1 }

$manifestPath = Join-Path $repo ("out\launch-manifest-" + $short + ".txt")
WriteRecord $manifestPath $manifest
Say ("  manifest: " + $manifestPath + "  (" + $manifest.Count + " lines)")

# ---- 8. the step this script is not allowed to take -------------------------
Say "`n[8] install -- a human click, deliberately not automated"
if ($setup.Count -eq 0) {
    Say "  nothing to install: launch/ holds no setup exe (stage 6 was skipped)"
} else {
    Say "  Run from PowerShell (never Git Bash: MSYS rewrites /VERYSILENT into a path), and do NOT"
    # Single-quoted on purpose: in a double-quoted PowerShell string a backtick is the escape character,
    # so `timeout N` printed as <TAB>imeout N -- the backtick and the t went into the tab and the warning
    # about that command lost its own subject. Measured in out/vbuild-recheck.txt vs the earlier log.
    Say '  wrap it in `timeout N` -- that kills PowerShell mid-UAC-wait and Setup deletes its log first:'
    Say ""
    Say ('  Start-Process -FilePath "' + (Join-Path $launch $setup[0].Name) + '" -Verb RunAs -Wait -PassThru ' +
         '-ArgumentList ''/VERYSILENT /NORESTART /SUPPRESSMSGBOXES /LOG="' + (Join-Path $repo 'out\install.log') + '"""')
    Say ""
    Say "  Then re-hash the installed tree against launch/ and read moduleinfo.json's Version --"
    Say "  that file carries the version, not the build identity, so it proves the payload is complete."
}
Say ("VERDICT  : " + ($report -join ' | ') + "  census clean  manifest written")

# Written, not left to chance: every failure above exits nonzero on its own line, and falling off the
# end of the file is the only path that means "all stages ran and agreed". The first version of this
# script reached the end without running a single stage and reported success.
exit 0
