<#
  verify-clone.ps1 -- the from-nothing check that no worktree run can make.

  A linked worktree shares the object DB, ignores nothing the main tree ignored, and sits
  beside `out/` and `external/`, so "green in a fresh worktree" still does not answer the
  question a reader on another machine asks: does `git clone` + configure + build + ctest
  work from the tracked file set ALONE? This clones over `file://`, checks the SHA out
  detached, and runs the same per-target build / freshness / ctest chain as
  `tools/verify-sha.ps1`.

  For a published repo this is also the completeness proof of the source grant: AGPL
  obliges the *source* of a conveyed binary, and "the clone has no `external/`, fetches
  JUCE at configure, and reaches 628 / 730 / 86 checks with 0 problems" is what shows the
  obligation is actually satisfiable from what is on GitHub.

  Scope, stated before it runs: three test suites plus ctest. No plugin target, no
  `launch/`, no installer. It refuses a destination that already exists -- a reused folder
  is not a clone.
#>
param(
    [string]$Sha      = '',
    [string]$RepoRoot = '',
    [string]$Dest     = '',
    [string]$Generator = '',
    [int]   $WantSmoke   = 628,
    [int]   $WantLayout  = 730,
    [int]   $WantPlan    = 86,
    [string]$WantFinger  = '4BC3AA231178C071'
)

$ErrorActionPreference = 'Continue'

# Every path resolves from this file's own location: the first copy of this harness lived in
# gitignored `out/` with absolute paths typed into it, which made it a machine-local script
# and the thing the repo quotes as a gate while nobody else could run it (item 6.7).
if ($RepoRoot -eq '') { $RepoRoot = Split-Path -Parent $PSScriptRoot }
$repo = $RepoRoot
if ($Sha -eq '') { $Sha = ((git -C $repo rev-parse HEAD) -join '') }
$shortForPath = $Sha.Substring(0, 7)
if ($Dest -eq '') { $Dest = Join-Path $repo ("out\clone-" + $shortForPath) }
$log = Join-Path $repo ("out\clone-run-" + $shortForPath)
New-Item -ItemType Directory -Force -Path $log | Out-Null

function Say ([string]$m) { Write-Output $m }

# The status line goes to the host, NOT the output stream: a function that both prints and
# returns leaves the caller holding an array whose first element is a sentence, and `if ($rc
# -ne 0)` then compares that array with 0 and gets the sentence back -- truthy. Measured on
# 2026-11-16: a clone that had succeeded was reported `FAILED: clone` and nothing after stage
# 1 ran, because of this one line and nothing else.
#
# Logs are written with `2>&1 | Out-File -Encoding utf8`, not `*>`: PowerShell 5.1's `*>`
# writes UTF-16LE with a BOM, which every later Select-String of the evidence has to survive.
function Run ([string]$what, [string]$file, [scriptblock]$body) {
    & $body 2>&1 | ForEach-Object { [string]$_ } | Out-File -Encoding utf8 $file
    $rc = $LASTEXITCODE
    Write-Host ("  " + $what + " exit=" + $rc + "  log=" + $file)
    return $rc
}

Say "=== Groovebox clone verification ==========================================="
Say ("SHA        : " + $Sha)
Say ("REPO       : " + $repo)
Say ("DEST       : " + $Dest)
Say ("GENERATOR  : " + $(if ($Generator) { $Generator } else { 'auto -- newest installed (no -G passed)' }))
Say ("SCOPE      : three test suites + ctest. No plugin target. launch/ untouched.")
Say ("START      : " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))

# ---- 0. a reused folder is not a clone ----------------------------------------
if (Test-Path $Dest) {
    Say "REFUSE: the destination already exists. Delete it or pass -Dest; this gate only"
    Say "        means something against a tree that has never been seen before."
    exit 3
}

# ---- 1. clone over the smart protocol, not a copy -----------------------------
Say "`n[1] clone (file:// transport, no hardlinks, no local optimisations)"
$rc = Run "git clone" "$log\01-clone.log" { git clone --no-hardlinks "file://$($repo -replace '\\','/')" $Dest }
if ($rc -ne 0) { Say "FAILED: clone"; exit 1 }

$rc = Run "git checkout" "$log\02-checkout.log" { git -C $Dest checkout --detach $Sha }
if ($rc -ne 0) { Say "FAILED: checkout"; exit 1 }

$head    = (git -C $Dest rev-parse HEAD) -join ''
$short   = $head.Substring(0, 7)
$subject = ((git -C $Dest log -1 --format='%s') -join '')
$dirty   = @(git -C $Dest status --porcelain | Where-Object { $_ -match '\S' })
Say ("  HEAD     : " + $short + "  " + $subject)
Say ("  DIRTY    : " + $dirty.Count)
if ($short -ne $Sha -and -not $head.StartsWith($Sha)) {
    Say "FAILED: the clone is not at the requested SHA."
    exit 1
}

# The ref assertion verify-sha.ps1 makes, restated for a clone: here the name a reader can
# re-check out is a remote-tracking ref, which is the honest form of the claim.
$named = @((git -C $Dest branch -r --contains $Sha 2>&1) | Where-Object { $_ -notmatch 'fatal|HEAD' })
if ($named.Count -eq 0) {
    Say "REF WARNING: no remote-tracking ref in the clone names $Sha -- a fetched object with no"
    Say "             name is not something another machine can check out by ref."
} else {
    Say ("  NAMED BY : " + ($named -join ', '))
}

# ---- 2. what the tracked file set actually is ---------------------------------
Say "`n[2] the clone's own inventory (this is the part a worktree cannot show)"
$tracked = @(git -C $Dest ls-files)
Say ("  tracked files        : " + $tracked.Count)
Say ("  top-level entries    : " + ((Get-ChildItem $Dest -Force | Where-Object { $_.Name -ne '.git' } | ForEach-Object { $_.Name }) -join ' '))
foreach ($p in @('out', 'external', 'build', 'build-new', 'launch')) {
    Say ("  " + $p.PadRight(10) + " present : " + (Test-Path (Join-Path $Dest $p)))
}
$tooling = @($tracked | Where-Object { $_ -like 'tools/*' })
Say ("  tracked tooling      : " + ($tooling -join ', '))
$licence = @($tracked | Where-Object { $_ -match 'LICENSE|NOTICE' })
Say ("  licence files in the clone : " + $(if ($licence.Count) { $licence -join ', ' } else { 'NONE -- the published tree is unlicensed' }))

# ---- 3. configure (this is where JUCE gets fetched from GitHub) ---------------
Say "`n[3] configure -- external/JUCE is gitignored, so the clone must fetch it"
# An empty -Generator means "let CMake choose the newest installed generator", which is what makes
# this gate runnable on a machine that does not have the one this project was first built with. The
# two branches are spelled out rather than splatting an argument array into the Run helper's
# scriptblock, because a scope slip there would show up as a confusing configure error, not as a
# missing flag.
if ($Generator) {
    $rc = Run "cmake configure" "$log\03-configure.log" {
        cmake -S $Dest -B (Join-Path $Dest 'build') -G $Generator -A x64
    }
} else {
    $rc = Run "cmake configure" "$log\03-configure.log" {
        cmake -S $Dest -B (Join-Path $Dest 'build') -A x64
    }
}
if ($rc -ne 0) { Say "FAILED: configure. Tail:"; Get-Content "$log\03-configure.log" -Tail 25; exit 1 }
$juce = Join-Path $Dest 'external/JUCE/CMakeLists.txt'
Say ("  JUCE populated       : " + (Test-Path $juce))
Get-Content "$log\03-configure.log" |
    Select-String -Pattern 'Cloning JUCE|Configuring done|JUCE_VERSION|error' |
    Select-Object -First 6 | ForEach-Object { Say ("    " + $_.Line.Trim()) }

# ---- 4. build each suite per target -------------------------------------------
$targets = @(
    @{ name = 'GrooveboxSmokeTest';       exe = "$Dest\build\GrooveboxSmokeTest_artefacts\Release\GrooveboxSmokeTest.exe";
       inputs = @(@{ dir = "$Dest\Source"; recurse = $true; include = '*.cpp','*.h' },
                  @{ file = "$Dest\Tests\SmokeTest.cpp" }) },
    @{ name = 'GrooveboxLayoutTest';      exe = "$Dest\build\GrooveboxLayoutTest_artefacts\Release\GrooveboxLayoutTest.exe";
       inputs = @(@{ dir = "$Dest\Source"; recurse = $true; include = '*.cpp','*.h' },
                  @{ file = "$Dest\Tests\LayoutTest.cpp" }) },
    @{ name = 'GrooveboxPatternPlanTest'; exe = "$Dest\build\Release\GrooveboxPatternPlanTest.exe";
       inputs = @(@{ file = "$Dest\Source\GenrePatterns.h" },
                  @{ file = "$Dest\Source\PatternPlanner.h" },
                  @{ file = "$Dest\Tests\PatternPlanTest.cpp" }) }
)

Say "`n[4] build (per target, --parallel 2, no /MP)"
foreach ($t in $targets) {
    $rc = Run ("cmake --build " + $t.name) ("$log\04-" + $t.name + ".build.log") {
        cmake --build "$Dest\build" --config Release --parallel 2 --target $t.name
    }
    Say ("  BUILD " + $t.name + " EXIT: " + $rc)
    if ($rc -ne 0) { Say "FAILED: build"; exit 1 }
}

Say "`n[5] freshness -- an exe older than its OWN inputs is a stale verdict"
$staleTotal = 0
foreach ($t in $targets) {
    if (-not (Test-Path $t.exe)) { Say ("  MISSING " + $t.exe); exit 1 }
    $stamp = (Get-Item $t.exe).LastWriteTime
    $inputs = foreach ($src in $t.inputs) {
        if ($src.dir) { Get-ChildItem $src.dir -Recurse -Include $src.include }
        else { Get-Item $src.file }
    }
    $inputs = @($inputs)
    $stale = @($inputs | Where-Object { $_.LastWriteTime -gt $stamp })
    $staleTotal += $stale.Count
    Say ("  " + (Split-Path $t.exe -Leaf).PadRight(34) + " stamp=" + $stamp.ToString('HH:mm:ss') +
         " inputs=" + $inputs.Count + " stale=" + $stale.Count +
         " sha=" + (Get-FileHash $t.exe -Algorithm SHA256).Hash.Substring(0,12))
    $stale | ForEach-Object { Say ("    STALE " + $_.Name) }
}
if ($staleTotal -gt 0) { Say "FAILED: stale exe in a tree that was just built -- impossible, so investigate."; exit 2 }

# ---- 6. ctest replay (CTest does not build, which is why stage 4 ran first) ----
Say "`n[6] ctest --test-dir <clone>\build -C Release -V"
$rc = Run "ctest" "$log\06-ctest.log" { ctest --test-dir "$Dest\build" -C Release -V }
Say ("  ctest EXIT: " + $rc)
$ctest = Get-Content "$log\06-ctest.log" -Raw

# The two JUCE suites print "PASSED  628 checks," (TWO spaces -- a single-space pattern
# matched nothing in any log in this repo, and a gate that silently reports NOT FOUND is a
# gate that reports nothing), and the std-only planner prints "86 checks, 0 problems" with no
# PASSED prefix, so one pattern over the raw log cannot tell layout from planner. Split on
# lines instead.
$smokeM   = [regex]::Match($ctest, 'PASSED[ \t]+(\d+)[ \t]+checks,[ \t]+(\d+)[ \t]+failures')
$layoutM  = [regex]::Match($ctest, 'PASSED[ \t]+(\d+)[ \t]+checks,[ \t]+(\d+)[ \t]+problems')
$linesP   = [regex]::Matches($ctest, '(?m)^.*checks, \d+ problems.*$')
$planLine = ($linesP | ForEach-Object { $_.Value } | Where-Object { $_ -notmatch 'PASSED' } | Select-Object -First 1)
$planM    = [regex]::Match([string]$planLine, '(\d+) checks, (\d+) problems')
$guardOk  = $ctest -match 'GUARD OK'
$fingerHit = $ctest -match $WantFinger
$testsPassed = (([regex]::Matches($ctest, '\d+% tests passed out of \d+') | ForEach-Object { $_.Value }) -join '; ')
# The suites print no line saying "Problem count". They print the verdict itself, with the
# ctest `N:` prefix, in THREE shapes. An evidence field that comes back empty on a green run
# is the worst kind of lie, so this reads the verdict lines and prints them.
$probLines = (([regex]::Matches($ctest, '(?m)^\s*\d+:\s+.*\d+ checks, \d+ (failures|problems).*$') | ForEach-Object { $_.Value.Trim() }) -join ' | ')

Say ("  smoke checks         : " + $(if ($smokeM.Success)  { $smokeM.Groups[1].Value + " (" + $smokeM.Groups[2].Value + " failures)" }  else { 'NOT FOUND' }))
Say ("  layout checks        : " + $(if ($layoutM.Success) { $layoutM.Groups[1].Value + " (" + $layoutM.Groups[2].Value + " problems)" } else { 'NOT FOUND' }))
Say ("  planner checks       : " + $(if ($planM.Success)   { $planM.Groups[1].Value + " (" + $planM.Groups[2].Value + " problems)" } else { 'NOT FOUND -- line was: ' + $planLine }))
Say ("  guard printed OK     : " + $guardOk)
Say ("  [28] fingerprint     : " + $(if ($fingerHit) { $WantFinger } else { 'NOT FOUND' }))
Say ("  ctest summary        : " + $testsPassed)
Say ("  verdict lines read   : " + $probLines)

$fail = 0
if ($probLines -eq '') { Say "MISMATCH: no verdict line was read -- the log or the pattern is wrong"; $fail = 1 }
if (-not $smokeM.Success  -or [int]$smokeM.Groups[1].Value  -ne $WantSmoke)  { Say "MISMATCH: smoke count";  $fail = 1 }
if (-not $layoutM.Success -or [int]$layoutM.Groups[1].Value -ne $WantLayout) { Say "MISMATCH: layout count"; $fail = 1 }
if (-not $planM.Success   -or [int]$planM.Groups[1].Value   -ne $WantPlan)   { Say "MISMATCH: planner count"; $fail = 1 }
if ($smokeM.Success  -and [int]$smokeM.Groups[2].Value  -ne 0) { Say "MISMATCH: smoke failures non-zero"; $fail = 1 }
if ($layoutM.Success -and [int]$layoutM.Groups[2].Value -ne 0) { Say "MISMATCH: layout problems non-zero"; $fail = 1 }
if ($planM.Success   -and [int]$planM.Groups[2].Value   -ne 0) { Say "MISMATCH: planner problems non-zero"; $fail = 1 }
if (-not $guardOk)   { Say "MISMATCH: guard"; $fail = 1 }
if (-not $fingerHit) { Say "MISMATCH: fingerprint"; $fail = 1 }
if ($rc -ne 0)       { Say "MISMATCH: ctest exit"; $fail = 1 }
if ($dirty.Count -ne 0) { Say "MISMATCH: the clone is dirty"; $fail = 1 }

Say "`n=== VERDICT ==="
if ($fail -eq 0) {
    Say ("CLONE GREEN at " + $short + ": " + $WantSmoke + " / " + $WantLayout + " / " + $WantPlan +
         " checks, guard OK, fingerprint " + $WantFinger + ", ctest exit 0.")
} else {
    Say ("CLONE NOT GREEN -- read the mismatches above. Logs in " + $log)
}
Say ("END " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
exit $fail
