# `tools/` -- the harnesses that check the docs' pointers, and the trees they were run in

The three `cite/` scripts plus their pin list are the citation gate described in `AGENTS.md` ("Doc
citations: a line number is a claim about one tree"); the three `verify-*.ps1` files replay a named commit,
a fresh clone, and the whole ship chain. They live here rather than in `out/` because `out/` is gitignored, so a fresh
clone inherited the rules with no way to run them -- and, worse, the exemption list that decides the
verdict was a `git clean -xdf` away. NEXT-STEPS item **6.7**, shape (b): tracked, and **not** wired
into CTest.

## What each one is allowed to touch

| file | scope | builds the plugin? | writes `launch/`? |
| --- | --- | --- | --- |
| `cite/cite-sweep.py`, `cite/cite-audit.py` | the four docs in their own `DOCS` list: `README.md`, `AGENTS.md`, `NEXT-STEPS.md`, `installer/README.txt` | no | no |
| `verify-sha.ps1` | a named SHA in a detached worktree | three suites only | no |
| `verify-clone.ps1` | a fresh `--no-hardlinks` clone | three suites only | no |
| `verify-build.ps1` | this tree, end to end | three suites, then the `launch` target, then ISCC | **yes** -- and it snapshots the old one first |

The split is the point: two of the three verifiers can never consume `launch/` while something is reading
it, which is why they are safe to run any time, and exactly one is a ship step. `.github/workflows/` holds
the fourth carrier -- suites on every push, no `launch` target, so CI cannot write a deliverable either.

The `cite/` scripts need nothing but Python 3 and `git` on PATH -- no JUCE, no compiler. The verifiers need
the toolchain, and `verify-build.ps1` needs Inno Setup, because packaging is its last stage.

## Line endings

These files are LF in the blob, and a checkout under this repo's `core.autocrlf=true` hands them to you
as CRLF -- that is the normal state of a clone, not a mistake. Both scripts were run over the merged docs
from a CRLF checkout and print a census identical to the LF originals'. What moves is the digest the pin
banner reports, because that number names the bytes it read. So a banner digest is a fact about the file
on disk; if you need a number that survives the checkout, take it from the blob side -- `git show :<path>
| sha256sum` gives the same six values in any clone on any platform, and the six digests NEXT-STEPS.md's
item 6.7 receipt lists for these files are that kind.

## The two commands, in this order

From the repo root:

```
python tools/cite/cite-sweep.py .            # dry run: what moved, what it would rewrite
python tools/cite/cite-sweep.py . -apply     # apply
python tools/cite/cite-sweep.py . -apply     # run it AGAIN: idempotence is the evidence
python tools/cite/cite-audit.py .            # does each pointer land in the symbol its sentence names
```

The second sweep pass must report `citations to correct: 0` and `unresolvable: 0`; the audit must
report `UNPINNED miss -- symbol named, not where it says : 0`. A sweep that still wants to change
something is a sweep that has not finished, and the first version of this script moved 34 citations
one phase too far exactly that way.

`cite-sweep.py` also writes `out/cite-symbols.txt` (created if absent): every citation beside the
function it lands in, so a sentence about one symbol pointing at another is visible in one column.

## Why `cite-pin.txt` exists, and why its absence stops the run

`cite-pin.txt` lists doc line ranges whose citations are deliberately *not* renumbered, each with a
reason, in the coordinates of the commit named on its `# base:` line. Three reasons, three different
jobs -- see the header of the file.

The scripts now refuse (exit 3, no census printed) when the pin file is absent, has no `# base:`
line, or holds no entry. That is not caution, it is arithmetic: without the exemptions a correct tree
reprints its own history as 22 findings, and a `pinned: 0` beside a nonzero miss count is a run that
never loaded what it was about to excuse. Measured on the merged tree with the superseded 17-entry
list in place of the current 19-entry one: the audit still says `miss: 0`, while the sweep goes from
`to correct: 0` to `to correct: 2` -- and the two it wants to rewrite are the repaired pointers, back
to numbers that were wrong the day they were written. The pin file changes the classification, so the
run prints its path, entry count, base and sha256 on every census. `CITE_PIN=<path>` overrides the
location, which is how two pin sets are compared against one doc set. The two it would rewrite are
the `repaired` entries -- pin ranges at base lines 100 and 860, which carry by masked text onto
working-copy lines 119 and 1122 of `NEXT-STEPS.md` -- so a stale list does not merely under-report,
it quietly undoes a pointer that was fixed by reading the tree.

## `verify-sha.ps1` -- checking a commit that is not your tree

`tools/verify-sha.ps1 -Sha <sha> -Tag <tag> [-Since <previously-verified-sha>]` checks the SHA out
detached into `out/wt-verify` (reused on purpose -- see the comment at the top of the file), builds
each suite **per target**, refuses any exe older than its own inputs, then runs the suites and
`ctest`, in that order, because CTest does not build. `-Since` answers the cheap question first: if
the diff between the two SHAs touches no compiled input, the script exits 0 and says whether a
repackage is still owed (`installer/` is a payload input, not a compiled one) -- so a docs commit
never costs a worktree build.

It is suites-only. Its own log line says so, and that sentence is the check: the file must contain
zero hits for `--target launch`, `Groovebox.exe` and `vst3`.

## `verify-clone.ps1` -- the same chain from a tree that has never been seen

`tools/verify-clone.ps1 [-Sha <sha>] [-Generator <cmake generator>]` answers the question a
worktree cannot: does the **tracked file set alone** build and pass? It `git clone --no-hardlinks`
over `file://`, checks the SHA out detached, inventories what the clone actually holds, configures
(which is where `external/JUCE` gets fetched, since `external/` is gitignored), builds the three
suites **per target**, refuses any exe newer-input-than-its-own-stamp, and only then replays
`ctest`. It **refuses** a destination that already exists -- a reused folder is not a clone -- so a
second run needs `-Dest` or the previous clone deleted. Scope, in its own banner: suites plus
ctest, no plugin target, `launch/` untouched.

The defaults name the shipped verdicts (640 / 730 / 86 checks, 0 problems, the `[28]` fingerprint
`4BC3AA231178C071`), and the run prints the verdict lines it read rather than a count derived from
them, because a green summary beside an empty evidence field is the failure mode this script was
born fixing. For a public AGPL repository this is also the completeness proof behind the licence:
"corresponding source" means somebody else can reach the same numbers from `git clone`, and this is
the command that shows it. Takes a few minutes, most of it the JUCE clone and three cold builds.

## `verify-build.ps1` -- the ship chain in one command

`tools/verify-build.ps1 [-SkipInstaller] [-RepoRoot <path>] [-WantSmoke N] [-WantLayout N] [-WantPlan N] [-WantFinger HEX]`
is item **6.1**: the sequence that used to live in a person's memory -- run the gate, refresh `launch/`,
re-hash the payload against the tree it came from, package, don't install. It refuses on the first failure
and prints one banner per stage:

| stage | what it does | how it can refuse |
| --- | --- | --- |
| 1 | freshness guard, **per suite target** | an input newer than its own exe prints `NEWER-THAN-EXE` with full dates -- not a failure, stage 2 rebuilds; an **empty** input set for a target is a refusal |
| 2 | builds the three suites separately (`--parallel 2`, no `/MP`) | build exit code, read straight from `$LASTEXITCODE` on the next line |
| 3 | runs them and judges **both** numbers the suite prints, plus the `[28]` fingerprint | `COUNT 628, wanted 999` / `FINGERPRINT ABSENT` then `FAILED:` with the tail |
| 4 | `ctest -V` as the replay of the tracked gate (four entries, incl. the source guard) | its own exit code, plus the `GUARD OK` line |
| 5 | snapshots the **outgoing** `launch/` to `out\launch-pre-<shortsha>` with a `FROM.txt` | skipped when that folder already exists -- a snapshot is never overwritten |
| 6 | `-SkipInstaller` skips this; otherwise the `launch` target, then `installer/build-installer.ps1` | either exit code |
| 7 | SHA-256 census, **both directions**: `launch/` vs the artefact tree | `ONLY IN launch/`, `ONLY IN artefacts`, `HASH DIFFERS`, and `ZERO files compared` |
| 8 | prints the ready-to-paste `Start-Process -Verb RunAs` line and **stops** | nothing to install is stated, not assumed |

Scope, in its own banner: it does **not** install, push, tag or publish. Elevation stays a human click, and
`launch/` is only ever reached from stage 6 -- which is why stages 1-5 run on a tree with a DAW open.

The write-side of stage 7 is the durable part: the census goes to `out/launch-manifest-<shortsha>.txt`, one
SHA-256 per payload file with `launch/` as the root of every name, LF endings and no BOM -- so it replays.
Measured from the repo root on the run above:

```
$ sha256sum -c out/launch-manifest-03a70e8.txt
launch/Groovebox.exe: OK
launch/Groovebox.vst3/Contents/Resources/moduleinfo.json: OK
launch/Groovebox.vst3/Contents/x86_64-win/Groovebox.vst3: OK
launch/Groovebox Setup 0.2.1.exe: OK
```

That replay is the reason the writer does not use `Out-File`: the first version mixed separators inside one
name (`Groovebox.vst3/Contents\Resources\moduleinfo.json`), wrote a BOM, and put the setup exe's byte count
on the digest line, which is exactly where a checker stops being able to read a record. A manifest you can
only look at is a screenshot of a gate.

What a green pass looks like, copied out of `out/vbuild-recheck.txt` (the tree at `03a70e8`, `-SkipInstaller`):

```
[3] run suites
  GrooveboxSmokeTest         exit=0  ok (628, 0 failures)
  GrooveboxLayoutTest        exit=0  ok (698, 0 problems)
  GrooveboxPatternPlanTest   exit=0  ok (86, 0 problems)
[7] hash census, launch/ vs the artefact tree
  equal         standalone exe  f5abfabb91127a746c80eae2184040c94e645f39fb1f4e89d0de917ed47e16c4
  equal              vst3 bundle  Contents\Resources\moduleinfo.json
  equal              vst3 bundle  Contents\x86_64-win\Groovebox.vst3
  files compared: 3
```

Those three hashes are the 0.2.1 export `AGENTS.md` records, so the census agrees with the hand-kept note
rather than only with itself. The `vst3` artefact directory also holds `Groovebox.exp` and `Groovebox.lib`
-- link leftovers, not payload -- so the tree walk starts inside the bundle, and that is why the count is
three and not five.

The two red arms are on record, because a gate nobody has seen fail is a decoration:

```
  GrooveboxSmokeTest         exit=0  COUNT 628, wanted 999        -> exit 1  (out/vbuild-neg1-wrong-smoke-count.txt)
  GrooveboxSmokeTest         exit=0  FINGERPRINT ABSENT           -> exit 1  (out/vbuild-neg2-wrong-fingerprint.txt)
```

Both were run by passing a deliberately wrong default (`-WantSmoke 999`, `-WantFinger 0123456789ABCDEF`)
against a suite that passed, so they prove stage 3 reads the report rather than the process exit code --
which is the whole reason the check exists: every suite here exits 0 while printing its own problem count.

**When a count moves, change the default in the same commit that moves it.** `-WantSmoke`, `-WantLayout`,
`-WantPlan` and `-WantFinger` are parameters for exactly that reason; a hardcoded literal would have to be
found and edited under time pressure.

One caveat for anyone scripting around this file: `Say` writes to the **output** stream, so a caller that
wants a greppable log must redirect through `Out-File -Encoding utf8`. PowerShell 5.1's `*>` hands you
UTF-16LE, and `grep` then reports "no matches" on a log that plainly contains them -- which is how the two
receipts above looked empty until they were decoded.
