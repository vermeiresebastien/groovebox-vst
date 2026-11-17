[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Sha,
    [Parameter(Mandatory = $true)] [string] $Tag,
    # Neutral on purpose. This directory is REUSED -- checking out a new SHA in it keeps the
    # object files, and a fresh worktree costs a full rebuild -- so it holds whatever SHA was
    # verified last, which is not written in its name. It used to be `wt-990e4c2`, and a reader
    # (me, 2026-11-16) took that for an identity, read the plugin binaries left in its build/
    # tree as that run's output, and reported a disagreement that was my own stale file.
    # Empty means <this repo>\out\wt-verify. It used to be a typed absolute path, which is a
    # second reason the harness never travelled: a clone elsewhere looked for a worktree in
    # another machine's folder, and the reuse warning below never fired where it mattered.
    [string] $Worktree = '',
    [string] $Since = ''
)

# Every path is resolved from this script's own location, never from the caller's working
# directory: promoting it out of gitignored `out/` into tracked `tools/` is only worth
# something if a fresh clone can run it from anywhere.
$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot
if ($Worktree -eq '') { $Worktree = Join-Path $RepoRoot 'out\wt-verify' }
# Stop is wrong here: git reports "Previous HEAD position was ..." on stderr during a
# routine detached checkout, and a NativeCommandError aborts the run before any build.
$ErrorActionPreference = 'Continue'
$logDir = Join-Path $RepoRoot 'out'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log = Join-Path $logDir "verify-$Tag"

# The cheap answer before the expensive one. A green block at SHA A judges SHA B when the diff
# between them touches nothing the compiler or the suites read -- and that is a two-second
# question, so asking it beats spending a worktree build on a docs commit. `installer/` is NOT
# one of those paths: it is not compiled, but it is installed, so a diff that reaches it means no
# rebuild and still a repackage.
if ($Since -ne '') {
    # Against THIS repo, not the worktree: a rev walk needs objects, not files, and the worktree
    # may not exist yet on a first run. Keying it to $Worktree made the cheap answer depend on
    # the expensive one, and reported a phantom one-file diff when the directory was absent.
    $moved = @(& git diff --name-only "$Since..$Sha" 2>&1)
    $compiled = @($moved | Where-Object { $_ -match '^(Source/|Tests/|CMakeLists\.txt|external/)' })
    $payload  = @($moved | Where-Object { $_ -match '^installer/' })
    Write-Output ("DIFF SINCE " + $Since + ": " + $moved.Count + " files, compiled inputs " `
        + $compiled.Count + ", payload inputs " + $payload.Count)
    $moved | ForEach-Object { Write-Output ("  " + $_) }
    if ($compiled.Count -eq 0) {
        if ($payload.Count -gt 0) {
            Write-Output ("VERDICT: no rebuild needed, but " + $payload.Count +
                " payload file(s) changed, so a repackage and an elevated install ARE owed.")
        } else {
            Write-Output "VERDICT: no rebuild and no repackage -- the earlier block still judges this tip."
        }
        Write-Output "Record it as verified-at <since> plus unchanged-at <tip>, never as a re-run."
        exit 0
    }
}

# A detached worktree, checked out from the commit itself: the working tree is the worker's
# and its mtimes say nothing about which commit produced an exe.
if (-not (Test-Path $Worktree)) {
    & git worktree add --detach $Worktree $Sha 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "worktree add failed for $Sha at $Worktree" }
}
& git -C $Worktree checkout --detach $Sha 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "worktree checkout failed for $Sha" }
Write-Output ("VERIFIED TREE: " + (& git -C $Worktree log --format='%h %s' -1))

# A reused worktree can carry an uncommitted edit through a checkout, and then the run judges a
# tree no commit describes. Count it and say so; refusing would hide the case that verifying a
# dirty tip is exactly the question -- but that question has to be answered in the log.
$dirty = @(& git -C $Worktree status --porcelain 2>&1 | Where-Object { $_ -match '\S' })
Write-Output ("WORKTREE: " + (Split-Path $Worktree -Leaf) + "  DIRTY FILES: " + $dirty.Count)
$dirty | Select-Object -First 8 | ForEach-Object { Write-Output ("  " + $_) }

# What this script does not gate, printed where a reader of the log will see it: the suites and
# nothing else. The plugin binaries that may sit in this tree's build/ belong to whichever
# commit was last built by hand, and no check here says otherwise.
Write-Output "SCOPE: three test suites + ctest only -- no plugin target built, launch/ untouched."

# Assert the ref, not only the SHA. A feature commit that no branch or tag names is reachable
# from nowhere but the command line, which is one `git worktree prune` away from an unverifiable
# verdict -- measured on 2026-11-16, when a lane's own work commit sat on a detached HEAD. Warn
# loudly rather than refusing: verifying a loose SHA is sometimes exactly the question, and a
# guard that cannot answer the question gets edited instead of heeded.
$named = @(& git -C $Worktree branch -a --contains $Sha --format='%(refname:short)' 2>&1) +
         @(& git -C $Worktree tag --contains $Sha 2>&1)
# Drop git's own noise: this verifier always leaves the worktree detached, and `branch
# --contains` reports that pseudo-ref alongside the real ones. A detached HEAD is not a name.
$named = @($named | Where-Object { $_ -notmatch 'fatal|error' -and $_ -notmatch 'detached' -and
                                   $_ -notmatch '^\(' })
if ($named.Count -eq 0) {
    Write-Output ("REF WARNING: " + $Sha + " is named by NO branch or tag -- verified bytes that " `
        + "no ref can re-check out. Ask the authoring lane to point a ref at it.")
} else {
    Write-Output ("NAMED BY: " + ($named -join ', '))
}

# The per-target compile inputs, stated per target because that is what cmake builds:
# the two console suites are Source/ plus their OWN Tests/*Test.cpp, and the planner test
# links no JUCE and reads only the two headers it is about. One shared Source/ glob would
# refuse the planner permanently the moment any other source moved -- and a guard that can
# never pass trains the habit of editing the script.
$targets = @(
    @{ name = 'GrooveboxSmokeTest';       exe = "$Worktree\build\GrooveboxSmokeTest_artefacts\Release\GrooveboxSmokeTest.exe";
       inputs = @(@{ dir = "$Worktree\Source"; recurse = $true; include = '*.cpp','*.h' },
                  @{ file = "$Worktree\Tests\SmokeTest.cpp" }) },
    @{ name = 'GrooveboxLayoutTest';      exe = "$Worktree\build\GrooveboxLayoutTest_artefacts\Release\GrooveboxLayoutTest.exe";
       inputs = @(@{ dir = "$Worktree\Source"; recurse = $true; include = '*.cpp','*.h' },
                  @{ file = "$Worktree\Tests\LayoutTest.cpp" }) },
    @{ name = 'GrooveboxPatternPlanTest'; exe = "$Worktree\build\Release\GrooveboxPatternPlanTest.exe";
       inputs = @(@{ file = "$Worktree\Source\GenrePatterns.h" },
                  @{ file = "$Worktree\Source\PatternPlanner.h" },
                  @{ file = "$Worktree\Tests\PatternPlanTest.cpp" }) }
)

# This gate does not configure, and says so before the first build rather than after a failed
# one. Measured the hard way: pointed at a worktree that had never been configured, every
# `cmake --build` answered "build is not a directory" -- which arrived here as exit 1 on the
# first target, and as a silent yes from the shell when the run was piped through `tee`,
# because the pipe reports tee's status, not the script's.
if (-not (Test-Path "$Worktree\build\CMakeCache.txt")) {
    Write-Output ("NOT CONFIGURED: " + $Worktree + "\build has no CMakeCache.txt. This gate will" `
        + " not configure a tree for you -- do it once with:")
    Write-Output ('  cmake -S "' + $Worktree + '" -B "' + $Worktree + '\build" ' `
        + '-G "Visual Studio 18 2026" -A x64')
    exit 3
}

foreach ($t in $targets) {
    & cmake --build "$Worktree\build" --config Release --parallel 2 --target $t.name 2>&1 |
        Out-File -Encoding utf8 "$log-$($t.name).build.log"
    Write-Output ("BUILD " + $t.name + " EXIT: " + $LASTEXITCODE)
    if ($LASTEXITCODE -ne 0) { exit 1 }
}

# Trust the link, not the exit code: an exe older than any of ITS OWN inputs is a stale
# verdict.
foreach ($t in $targets) {
    $stamp = (Get-Item $t.exe).LastWriteTime
    $inputs = foreach ($src in $t.inputs) {
        if ($src.dir) { Get-ChildItem $src.dir -Recurse -Include $src.include }
        else { Get-Item $src.file }
    }
    $inputs = @($inputs)
    $stale = @($inputs | Where-Object { $_.LastWriteTime -gt $stamp })
    Write-Output ((Split-Path $t.exe -Leaf) + " stamp=" + $stamp.ToString("HH:mm:ss") +
        " inputs=" + $inputs.Count + " stale=" + $stale.Count +
        " sha=" + (Get-FileHash $t.exe -Algorithm SHA256).Hash.Substring(0, 12))
    $stale | ForEach-Object { Write-Output ("  STALE " + $_.Name + " " + $_.LastWriteTime.ToString("HH:mm:ss")) }
    if ($stale.Count -gt 0) { exit 2 }
}

foreach ($t in $targets) {
    & $t.exe 2>&1 | Out-File -Encoding utf8 "$log-$($t.name).txt"
    Write-Output ($t.name + " EXIT: " + $LASTEXITCODE)
}

Write-Output "--- failures per suite ---"
foreach ($t in $targets) {
    # Case-sensitive: both suites print their problems as an uppercase "FAIL" and the smoke
    # summary ends "..., 0 failures" in lower case, so an insensitive scan for FAIL reports
    # one phantom failure on a run that had none -- and a verifier that cries wolf gets its
    # own output ignored.
    $bad = @(Select-String -CaseSensitive -Path "$log-$($t.name).txt" -Pattern 'FAIL' |
        ForEach-Object { $_.Line.Trim() })
    Write-Output ($t.name + ": " + $bad.Count + " failure lines")
    $bad | Select-Object -First 12 | ForEach-Object { Write-Output ("  " + $_) }
}
Write-Output "--- summary lines ---"
foreach ($t in $targets) {
    Write-Output ($t.name + " last: " + (Get-Content "$log-$($t.name).txt" | Select-Object -Last 1))
}

# ctest is the tracked replay of all four entries, guard included. It does NOT build, which
# is why the three builds above come first and why a green ctest alone proves nothing.
& ctest --test-dir "$Worktree\build" -C Release -V 2>&1 | Out-File -Encoding utf8 "$log-ctest.txt"
Write-Output ("CTEST EXIT: " + $LASTEXITCODE)
Write-Output "--- ctest tail ---"
Get-Content "$log-ctest.txt" | Select-Object -Last 8
