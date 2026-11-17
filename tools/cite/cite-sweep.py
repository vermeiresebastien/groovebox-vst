#!/usr/bin/env python3
"""Line-citation sweep: report, and with -apply, force every citation to its right number.

Why this exists: the docs cite code as `SequencerEngine.cpp:151-159`, and every phase that
adds lines moves those numbers. A doc whose pointers land in an unrelated comment is worse
than one that has no pointers, because it is trusted.

How it works. `git show <base>:<file>` is the tree the docs were written against, the base
being the commit named on the `# base:` line of tools/cite/cite-pin.txt (see tools/cite/
cite_pins.py); the working
copy is the tree they have to describe now. For each cited file the diff gives an
old-line -> new-line map built from the equal blocks, so a citation that was right at the base
is answered by arithmetic on the diff rather than by reading.

The rewrite is keyed, not incremental: the target token is computed from the base's own
citation, and the line it belongs to is matched by its text with every digit masked out.
That makes the command idempotent -- run it twice and the second run finds nothing -- which
a "shift each number by the delta you find" version is not. The first version of this script
did the incremental thing, was run twice in one sitting, and moved 34 citations one phase
too far; the damage only showed because a second dry run still wanted to change them.

Two cases are escalated rather than guessed:
  * the named line was rewritten rather than shifted -- matched by searching the base's line
    text in the new file, applied only when exactly one line carries it;
  * the doc line itself has been edited since the base, so its keyed context is gone -- printed
    for a human, because inventing a match is how a citation gets pointed at a lie.

`cite-pin.txt`, the tracked file beside this script, lists doc line ranges whose citations
are pinned to a recorded baseline
on purpose (NEXT-STEPS section A declares "Status at 460b3bf"); those are skipped, and the
skip is printed, so the exception reads as a decision with a reason and not an oversight.

A pin is two different jobs and the reason string in that file tells them apart. `pinned:` and
`unverified:` say the sentence belongs to the baseline's tree, so the base's text is written back
over any shift -- skipping is not enough, because an exception that only declines to act cannot
undo the action it declined. `repaired:` says the opposite: the base's number was wrong the day it
was written, a hand check put the working copy's right, and shifting from the base renews the
error (the map preserves what the base's number *pointed at*, not what the sentence meant). Those
are held, not restored, and printed as held -- a fix that disappears in a pass reporting
`to correct: 0` is the worst kind of loss, because the run looks clean.
"""
import io
import os
import re
import subprocess
import sys
from difflib import SequenceMatcher

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cite_pins

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")

ARGV = [a for a in sys.argv[1:] if not a.startswith("-")]
APPLY = "-apply" in sys.argv
ROOT = os.path.abspath(ARGV[0] if ARGV else ".")
DOCS = ["README.md", "AGENTS.md", "NEXT-STEPS.md", "installer/README.txt"]

TOK = cite_pins.TOK   # one grammar for all four readers; see the comment in cite_pins.py
BASE = cite_pins.base()
SEARCH = ("", "Source", "Tests", "installer")

def rel_key(rel):
    """Repo-relative path for a cited file name, or None if it is not in this repo."""
    for pre in SEARCH:
        cand = os.path.normpath(os.path.join(ROOT, pre, rel))
        key = os.path.relpath(cand, ROOT).replace(os.sep, "/")
        if key.split("/")[0] in ("out", ".."):
            continue
        if os.path.isfile(cand):
            return key
    return None

def git_show(key):
    return cite_pins.show(ROOT, BASE, key)

def read(key):
    # newline="": these docs are CRLF on disk under core.autocrlf=true, and a script that
    # translated them would rewrite every line of the file, not the lines it was asked to.
    return open(os.path.join(ROOT, key.replace("/", os.sep)),
                encoding="utf-8", errors="replace", newline="").read()

def body(key):
    return read(key).replace("\r\n", "\n").split("\n")

def write(key, text):
    # newline="" again, so the "\r" characters that came out of read() go back unchanged.
    open(os.path.join(ROOT, key.replace("/", os.sep)), "w",
         encoding="utf-8", errors="replace", newline="").write(text)

# The masked-text key is shared with the pin reader: the sweep matches a rewrite on it and
# `is_pinned` matches an exception on it, so a private copy would let the two drift -- which is
# the same "either the shift or the exception is wrong, and nothing says so" as TOK above.
mask = cite_pins.mask

def toks(line):
    """Citation tokens on a line, each attributed to a file.

    A bare `:449-452` belongs to the last file named earlier on the same line -- the way a
    reader resolves it. One named further up the paragraph is *not* assumed: that guess is
    how a number gets re-pointed at a different file's code, so it is left alone and said.
    """
    out, owner = [], None
    for m in TOK.finditer(line):
        if m.group("file"):
            owner = m.group("file")
            f, sep = owner, m.group("qsep")
        elif owner is None or m.start() == 0:
            continue
        else:
            f, sep = owner, ":"
        out.append({"start": m.start(), "end": m.end(), "text": m.group(0), "file": f,
                    "sep": sep, "a": int(m.group("a")),
                    "b": int(m.group("b")) if m.group("b") else None,
                    "mid": m.group("mid") or "", "bare": not m.group("file")})
    return out

# A pin is a base line range, because that is the coordinate the baseline is written in. But the
# sweep also has to *find* those lines in the working copy, and the working copy grows: the 4.4
# docs edit pushed section E down by forty-odd lines, so a `cur`-side index taken from a base
# range silently pins the wrong paragraph -- and the wrong paragraph is a plausible one, since
# docs of this shape repeat themselves. So the current side is matched by masked text instead:
# a line is pinned when its digits are the only thing separating it from a base line inside the
# range. That is the same key the rewrite already uses, so the pin and the shift cannot drift
# apart, and it survives any amount of editing above or below the pinned lines. The reading of
# the list lives in cite_pins.py, which the symbol audit imports too: two answers to "is
# this line excepted" is how an exception quietly becomes a shift, and nothing would say so.
pins = cite_pins.load()
pin_masks = {}
for doc, ranges in pins.items():
    pin_masks[doc] = cite_pins.masked_pinned((git_show(doc) or "").split("\n"), ranges)

def is_pinned(doc, head_index, cur_text):
    """Base coordinate or working-copy text -- either one pins the line."""
    for x, y, _ in pins.get(doc, []):
        if x <= head_index <= y:
            return True
    return bool(cur_text) and cite_pins.mask(cur_text) in pin_masks.get(doc, {})


maps, lines_of, blocky = {}, {}, {}
def amp_of(key):
    """Base line number -> working-tree line number for one file, or None (not in repo).

    Only equal runs of three lines or more are trusted. A one- or two-line run is usually
    a `}` or a blank landing on another `}`, and difflib will happily pair it with the wrong
    one -- which is how a citation gets renumbered into a different function and comes back
    looking tidier than it is.
    """
    if key in maps:
        return maps[key]
    old, new = git_show(key), body(key)
    if old is None:
        maps[key] = None
        return None
    ol = old.split("\n")
    lines_of[key] = (ol, new)
    if ol == new:
        maps[key] = {}
        return maps[key]
    amp, blocks = {}, []
    for tag, i1, i2, j1, j2 in SequenceMatcher(None, ol, new, autojunk=False).get_opcodes():
        if tag == "equal" and (i2 - i1) >= 3:
            blocks.append((i1 + 1, i2, j1 - i1))
            for k in range(i2 - i1):
                amp[i1 + k + 1] = j1 + k + 1
    maps[key], blocky[key] = amp, blocks
    return amp

SIG = re.compile(r"^[A-Za-z_][\w:<>&*, ]*\s+([\w~][\w:~]*::[\w~]+|[\w~]+)\s*\(")

def func_of(lines, n):
    """The function a line sits in: the last signature at or above it."""
    name, at = "?", 0
    for i in range(min(n, len(lines)) - 1, -1, -1):
        m = SIG.match(lines[i])
        if m:
            return m.group(1), i + 1
    return name, at

def shift(key, a, b, mid=""):
    """(new_a, new_b, note) for base lines a[-b] of key, or None when it cannot be answered.

    A comma is two separate lines, not a range: `SequencerEngine.cpp:290,138` names one
    thing at 290 and another at 138, so each is resolved on its own anchors. Extrapolating
    138 by the delta found at 290 -- which an earlier version of this function did -- moves
    a second, unrelated pointer by the first one's arithmetic.
    """
    amp = amp_of(key)
    if amp is None:
        return None
    if not amp:
        return a, b, None                       # file byte-identical: the number cannot be stale
    if b and b < a:
        b = a                                   # comma pairs are handled by the caller per number
    lo, hi = a, (b if b else a)
    deltas = {}
    for probe in range(lo, hi + 1):
        if probe in amp:
            d = amp[probe] - probe
            deltas[d] = deltas.get(d, 0) + 1
    if deltas:
        d = max(deltas, key=lambda k: (deltas[k], -abs(k)))
        if mid == ",":
            r = shift(key, b, None, "")
            if r is None:
                return None
            return a + d, r[0], f"delta {d:+d} on the first, {r[2]} on the second"
        return a + d, (b + d if b else None), (f"delta {d:+d} from {deltas[d]} anchor(s)"
                                               if deltas[d] > 1 else "single anchor")
    ol, nl = lines_of[key]
    if a > len(ol):
        return None
    probe = ol[a - 1].strip()
    hits = [i + 1 for i, l in enumerate(nl) if probe and l.strip() == probe]
    if len(hits) == 1:
        return hits[0], (hits[0] + (b - a) if b else None), "line rewritten, matched by text"
    return None


already = 0
wanted = {}                    # doc -> {line index: [(start, end, old, new, note)]}
quiet, questions, skipped = [], [], set()
symbols, allsyms = [], []
restore, restored = {}, []
held = []

for doc in DOCS:
    head = (git_show(doc) or "").split("\n")
    cur = read(doc).replace("\r\n", "\n").split("\n")
    pinned_head = [(i + 1, hl) for i, hl in enumerate(head) if is_pinned(doc, i + 1, hl)]
    pkeys = {}
    for _, hl in pinned_head:
        if TOK.search(hl):
            pkeys.setdefault(mask(hl), []).append(hl)
    for x, y, _ in pins.get(doc, []):
        if any(x <= i <= y for i, hl in pinned_head):
            skipped.add(f"{doc}:{x}-{y}")
    by_key, used = {}, {}
    for i, hl in enumerate(head):
        if is_pinned(doc, i + 1, hl):
            continue
        hs = toks(hl)
        if hs:
            by_key.setdefault((mask(hl), len(hs)), []).append(hs)
    for j, cl in enumerate(cur, 1):
        cs = toks(cl)
        if not cs:
            continue
        if mask(cl) in pkeys:
            # pinned by its text, wherever it moved to: if the digits are the only difference
            # from the base's line, the shift already happened and the base's version is the answer
            for x, y, why in pins.get(doc, []):
                if any(x <= i <= y for i, hl in pinned_head if mask(hl) == mask(cl)):
                    skipped.add(f"{doc}:{x}-{y}")
            if cite_pins.is_repair(pin_masks[doc][mask(cl)]):
                # ...unless the reason is `repaired:`, which makes the base's number the wrong one.
                # Restoring here would overwrite a hand-verified pointer with the error it was
                # written to replace, and it would do it in the same pass that reports `to
                # correct: 0` -- the cleanest possible way to lose a fix. Hold, and say so.
                held.append(f"  {doc}:{j}  {cl.strip()[:64]}")
                continue
            same = [hl for hl in pkeys[mask(cl)] if hl != cl]
            if len(same) == 1:
                restore.setdefault(doc, {})[j] = same[0]
                restored.append(f"  {doc}:{j}  pinned: {cl.strip()[:64]}  ->  {same[0].strip()[:64]}")
            continue
        keyt = (mask(cl), len(cs))
        bucket = by_key.get(keyt)
        idx = used.get(keyt, 0)
        used[keyt] = idx + 1
        if not bucket or idx >= len(bucket):
            for c in cs:
                k = rel_key(c["file"])
                if k and amp_of(k):
                    quiet.append(f"  {doc}:{j}  {c['text']}  (line edited since {BASE} -- not touched)")
            continue
        hs = bucket[idx]
        for hc, cc in zip(hs, cs):
            if hc["file"] != cc["file"]:
                continue
            k = rel_key(cc["file"])
            if k is None:
                continue
            r = shift(k, hc["a"], hc["b"], hc["mid"])
            if r is None:
                questions.append(f"  {doc}:{j}  {cc['text']}   ?? {BASE}'s {hc['text']} "
                                 f"cannot be located in {cc['file']}")
                continue
            na, nb, note = r
            correct = ((":" if cc["bare"] else cc["file"] + cc["sep"]) + str(na)
                       + ((hc["mid"].replace(str(hc["b"]), str(nb)) if nb else "") if hc["b"] else ""))
            # the enclosing symbol on both sides: a citation that lands in a different
            # function after renumbering was moved by arithmetic, not by meaning
            hol, hnl = lines_of[k]
            hf, _ = func_of(hol, hc["a"])
            nf, _ = func_of(hnl, na)
            tag = "" if hf == nf else f"   !! {hf}() at {BASE} is now inside {nf}()"
            if tag:
                symbols.append(f"  {doc}:{j}  {cc['text']} -> {correct}{tag}")
            allsyms.append(f"{doc}:{j}\t{cc['text']}\tsits in {nf}()")
            if cc["text"] == correct:
                already += 1
                continue
            wanted.setdefault(doc, {}).setdefault(j, []).append(
                (cc["start"], cc["end"], cc["text"], correct,
                 (" [bare, in " + cc["file"] + "]" if cc["bare"] else "")
                 + (f" [{note}]" if note else "")))

print(f"baseline the docs are measured from: {BASE}"
      + ("" if BASE == "HEAD" else "   (the # base: line of the pin file, named below)"))
print(cite_pins.provenance())
print(f"citations already right        : {already}")
print(f"citations to correct           : {sum(len(v) for d in wanted.values() for v in d.values())}")
for doc in DOCS:
    for j, outs in sorted(wanted.get(doc, {}).items()):
        for s, e, old, new, note in outs:
            print(f"  {doc}:{j}  {old}{note}  ->  {new}")
print(f"unresolvable                   : {len(questions)}")
for q in questions:
    print(q)
print(f"citations whose enclosing function differs {BASE} vs now: {len(symbols)}")
for s in symbols:
    print(s)
sym_path = os.path.join(ROOT, "out", "cite-symbols.txt")
os.makedirs(os.path.dirname(sym_path), exist_ok=True)   # a fresh clone has no out/ yet
with open(sym_path, "w", encoding="utf-8", newline="") as f:
    f.write("Every citation, with the function it lands in in the working tree.\n"
            "The prose next to it is the other half of the check, and this script cannot\n"
            "read prose -- the point of this file is that the symbol is stated, so a\n"
            "sentence about `renderVoices` pointing at `fireStep` is visible in one column.\n\n")
    f.write("doc:line\tcitation\tlands in\n")
    for s in sorted(set(allsyms)):
        f.write(s + "\n")
print("symbol table written to " + os.path.relpath(sym_path, ROOT).replace(os.sep, "/"))
print(f"pinned ranges skipped          : {sorted(skipped)}")
print(f"pinned lines put back as {BASE} wrote them: {len(restored)}")
for r in restored:
    print(r)
print(f"repaired pointers held, NOT restored     : {len(held)}")
for r in held:
    print(r)
print(f"edited-since-{BASE} lines left alone: {len(quiet)}")
for q in quiet[:20]:
    print(q)

if APPLY:
    n = 0
    for doc in DOCS:
        if doc not in wanted and doc not in restore:
            continue
        lines = read(doc).split("\n")
        for j, outs in wanted.get(doc, {}).items():
            line = lines[j - 1]
            for s, e, old, new, note in sorted(outs, key=lambda o: -o[0]):
                line = line[:s] + new + line[e:]
                n += 1
            lines[j - 1] = line
        for j, hl in restore.get(doc, {}).items():
            # the base's text came out of git with the CRLF normalised away, and these files
            # are CRLF on disk: put the line back with the ending the file actually uses.
            cr = "\r" if lines[j - 1].endswith("\r") else ""
            lines[j - 1] = hl + cr
            n += 1
        write(doc, "\n".join(lines))
    print(f"\nAPPLIED {n} rewrites")
else:
    print("\ndry run -- pass -apply to write")
