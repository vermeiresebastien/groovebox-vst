# `tools/` -- the harnesses that check the docs' pointers, and the trees they were run in

These six files are the citation gate described in `AGENTS.md` ("Doc citations: a line number is a
claim about one tree") plus the two verifiers that replay a named commit or a fresh clone. They live
here rather than in `out/` because `out/` is gitignored, so a fresh
clone inherited the rules with no way to run them -- and, worse, the exemption list that decides the
verdict was a `git clean -xdf` away. NEXT-STEPS item **6.7**, shape (b): tracked, and **not** wired
into CTest.

Nothing here needs the plugin build. Python 3 and `git` on PATH are the whole dependency.

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

The defaults name the shipped verdicts (628 / 698 / 86 checks, 0 problems, the `[28]` fingerprint
`4BC3AA231178C071`), and the run prints the verdict lines it read rather than a count derived from
them, because a green summary beside an empty evidence field is the failure mode this script was
born fixing. For a public AGPL repository this is also the completeness proof behind the licence:
"corresponding source" means somebody else can reach the same numbers from `git clone`, and this is
the command that shows it. Takes a few minutes, most of it the JUCE clone and three cold builds.
