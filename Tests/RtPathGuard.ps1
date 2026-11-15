<#
    Static guard: the parameter callback must not allocate.

    Why this is a source check and not a runtime one
    -----------------------------------------------
    A test that counts allocations would be the obvious way to gate this, and it is
    not available in this build. JUCE's allocation counter is compiled out unless
    JUCE_ENABLE_ALLOCATION_HOOKS is set, and it defaults to 0
    (external/JUCE/modules/juce_core/juce_core.h:196-197). Turning it on would not
    help either: it hooks `operator new` and `operator delete`
    (juce_AllocationHooks.cpp:56-88), while `juce::String`'s storage comes from
    HeapBlock, which calls std::malloc and std::free directly
    (juce_HeapBlock.h:124,135,145). A counter built on the hook would therefore
    report "no allocations" through a callback that allocates twice per call — a
    gate that goes green on the exact fault it exists to catch, which is the one
    outcome a test is not allowed to have.

    So the invariant is checked where it actually lives: in the code. A heap
    allocation on this path is a property of what the function calls, not of any
    particular run, and the call list below is complete enough to be worth
    enforcing on every verification run.

    What is allowed
    ---------------
    Everything `juce::String` does without taking memory: `startsWith`,
    `equalsIgnoreCase`, `isEmpty`, `length`, `operator[]`, `getIntValue` and the
    comparison operators. Reading a character and comparing a prefix are both
    pointer arithmetic over the existing buffer.

    Usage
        powershell -NoProfile -ExecutionPolicy Bypass -File Tests\RtPathGuard.ps1
    Exit code
        0 clean, 1 a forbidden call was found, 2 the guard itself could not run.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$source = Join-Path $PSScriptRoot '..\Source\PluginProcessor.cpp'

if (-not (Test-Path $source)) {
    Write-Host "GUARD ERROR: $source not found" -ForegroundColor Red
    exit 2
}

$lines = Get-Content -LiteralPath $source

# The callback body, from its signature to the first line that closes it.
$start = ($lines | Select-String -SimpleMatch 'void GrooveboxProcessor::parameterChanged' |
          Select-Object -First 1).LineNumber

if (-not $start) {
    Write-Host 'GUARD ERROR: GrooveboxProcessor::parameterChanged not found; the guard ' -ForegroundColor Red -NoNewline
    Write-Host 'has to be updated along with the code, not silently skipped.'
    exit 2
}

$body = @()

for ($i = $start; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^\}') { break }
    $body += $lines[$i]
}

if ($body.Count -eq 0) {
    Write-Host "GUARD ERROR: empty body for parameterChanged (line $start)" -ForegroundColor Red
    exit 2
}

# Each entry is a pattern that takes memory out of the heap.
$forbidden = [ordered]@{
    'substring'           = 'String::substring allocates a new string'
    'trim'                = 'String::trim allocates'
    'upToFirstOf'         = 'String::upToFirstOf allocates'
    'fromFirstOccurrence' = 'String::fromFirstOccurrence allocates'
    'replace'             = 'String::replace allocates'
    'toUpperCase'         = 'String::toUpperCase allocates'
    'String\s*\('         = 'constructing a String allocates'
    '\+=\s*"'             = 'appending a literal to a String reallocates'
    '"\s*\+\s*id'         = 'concatenating with a String allocates'
    'newCharacterArray'   = 'raw character storage'
    'std::vector'         = 'a container on the automation path'
    'Array\s*<'           = 'a container on the automation path'
}

$hits = @()

foreach ($token in $forbidden.Keys) {
    for ($n = 0; $n -lt $body.Count; $n++) {
        $line = $body[$n]

        # A comment is not a call, and the explanation of this very rule is the
        # likeliest thing in the function to mention the words it forbids.
        if ($line -cmatch '^\s*(//|/\*|\*)') { continue }

        # Case-sensitive: without the `c` the `String (` rule matches the tail of
        # `substring (` and reports one call as two faults.
        if ($line -cmatch $token) {
            $hits += [pscustomobject]@{
                Line  = $start + $n + 1
                Text  = $line.Trim()
                Since = $forbidden[$token]
            }
        }
    }
}

Write-Host "parameterChanged is $($body.Count) lines from $source"

if ($hits.Count -gt 0) {
    Write-Host ''
    Write-Host "GUARD FAILED: $($hits.Count) allocating call(s) on the parameter path:" -ForegroundColor Red

    foreach ($hit in $hits) {
        Write-Host ("  {0}: {1}" -f $hit.Line, $hit.Text) -ForegroundColor Red
        Write-Host ("       {0}" -f $hit.Since) -ForegroundColor DarkRed
    }

    Write-Host ''
    Write-Host 'A host writes automation from the audio thread. Two heap allocations per'
    Write-Host 'change is a malloc inside the callback that is supposed to be the cheap one.'
    Write-Host 'Dispatch on the id without building one: see the prebuilt id table in'
    Write-Host 'Source/Parameters.h.'

    exit 1
}

Write-Host 'GUARD OK: the parameter path is allocation-free.' -ForegroundColor Green
exit 0
