#!/usr/bin/env python3
"""Does each citation point at the thing the sentence names?

cite-sweep.py proves a number *moved the way the code moved*: text identity, which is
arithmetic and needs no reading. That is the weaker question. A citation can be perfectly
text-identical and still point at the wrong function, because it was written from memory in
the first place -- and the docs in this repo are read as a spec.

This script asks the stronger one: every citation is judged against the working tree, and the
base commit is consulted only for the pin list. For every `file:NN[-MM]` in
the tracked docs it takes the symbol the sentence names -- a backticked word, or a
`Namespace::member`, or a camelCase identifier -- and asks whether that symbol actually
governs the cited lines: inside the range, or the function the range sits in, or within a
few lines of it. When it does not, the script searches the file for where the symbol really
is and prints that, so the fix is a decision with the evidence next to it rather than a
second guess.

No names on the line, or names that are English words, come back as JUDGE -- a script that
invented a match here would be exactly the unfalsifiable check the repo's own rules warn
about.

Four kinds of citation are kept out of the MISS count, and each says so: `pinned` (a range in
cite-pin.txt beside this script, matched by masked text against the base), a table row (an
indented `|` line is a
list *about* citations), `baseline-qualified` (the sentence itself names a commit whose tree the
number belongs to, in backticks, within 80 characters of it) and JUDGE above. The gate is
`UNPINNED miss: 0` read together with the printed lists -- the exemption counts are small and
every one of them is named, because a number the reader cannot see is not an exception with a
reason but a gap in the check. The known hole in the last one: a current-tree number that is
wrong, written in a sentence that also happens to name a commit, is exempted rather than
reported. It is 80 characters rather than a whole paragraph, and it is only reached after the
symbol check has already failed, so it cannot cost the audit a passing citation.

A fifth pin reason, `repaired:`, is *not* kept out. It is exempted from the shift because its
base number was wrong where it was written -- the map preserves what the base pointed at, so it
would carry the error forward -- and the working copy's number was chosen by reading the tree.
That makes it a claim about this tree, which is exactly what this script tests, so it is judged
like any other pointer and printed under `held` as well: the repair needs the gate, and the next
phase that moves the file needs to be able to say so.
"""
import io
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cite_pins

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
DOCS = sys.argv[2:] if len(sys.argv) > 2 else ["README.md", "AGENTS.md", "NEXT-STEPS.md", "installer/README.txt"]

def head_lines(doc):
    ls = cite_pins.show(ROOT, BASE, doc)
    return ls.split("\n") if ls is not None else []

PINS = cite_pins.load()
BASE = cite_pins.base()
# doc -> {masked base text: (base line, reason)}; a hit here is an exception with a stated
# reason, so it is reported as PIN and kept out of the MISS count that a reader has to act on
PINNED_KEYS = {doc: cite_pins.masked_pinned(head_lines(doc), ranges)
               for doc, ranges in PINS.items()}

TOK = cite_pins.TOK   # one grammar for all four readers; see the comment in cite_pins.py
# The symbol grammar and the name extraction live in cite_pins.py too, because the head-check now
# asks the same question ("does this number hold what the sentence claims?") to tell a drifted
# pointer from one the sweep already moved. Two spellings of that rule is how one of the two
# scripts starts calling a correct citation wrong.
BACK, SYMBOL, CAND = cite_pins.BACK, cite_pins.SYMBOL, cite_pins.CAND
# A citation is baseline-qualified when the sentence itself names the commit whose tree the number
# belongs to -- `460b3bf`, in backticks, which is how this repo writes every commit id. It is the
# same claim a `pinned` entry in cite-pin.txt makes, and it is the only form available to prose
# written this phase: pin ranges are base coordinates matched by masked text, so a line that does
# not exist at the base cannot be pinned by that file at all. Unlike a pin, this one is printed for
# every citation it exempts -- an exemption that scores silently is a hole, not a rule.
BASELINE = re.compile(r"`[0-9a-f]{7,10}`")
SEARCH = ("", "Source", "Tests", "installer")

def rel_key(rel):
    for pre in SEARCH:
        cand = os.path.normpath(os.path.join(ROOT, pre, rel))
        key = os.path.relpath(cand, ROOT).replace(os.sep, "/")
        if key.split("/")[0] in ("out", ".."):
            continue
        if os.path.isfile(cand):
            return key
    return None

def body(key):
    return open(os.path.join(ROOT, key.replace("/", os.sep)),
                encoding="utf-8", errors="replace").read().replace("\r\n", "\n").split("\n")

cache = {}
def lines_of(key):
    if key not in cache:
        cache[key] = body(key)
    return cache[key]

enclosing, names_in = cite_pins.enclosing, cite_pins.names_in

ok = misses = judged = pinned = listed = held = 0
heldlist = []
baselined = []
report = []
misslines = set()
for doc in DOCS:
    text = open(os.path.join(ROOT, doc), encoding="utf-8", errors="replace").read()
    text = text.replace("\r\n", "\n")
    ls = text.split("\n")
    keys = PINNED_KEYS.get(doc, {})
    for li, line in enumerate(ls, 1):
        wide = False
        if not TOK.search(line):
            continue
        masked = cite_pins.mask(line)
        if masked in keys:
            if not cite_pins.is_repair(keys[masked]):
                pinned += 1
                continue
            # `repaired:` is the one pin reason that is a claim about *this* tree: the base's
            # number was wrong where it was written, and the working copy's was put there by
            # reading the code. So it holds the line against the sweep's shift and is still judged
            # here, because an exception that also stopped the symbol test would protect the repair
            # from the only check that can say the repair is right -- a held pointer is a pointer,
            # and after the next phase moves the file it is exactly as stale as the ones below.
            held += 1
            # ...and the clause that names its symbol is read from the whole paragraph, not the
            # 220 characters above the token on this line. Both repaired sentences open with their
            # citation -- the pointer is the subject of the line -- so the tight clause finds
            # nothing and the repair lands in JUDGE, which is the exemption the pin was written to
            # remove, reached from the other direction. Widening is scoped to held lines because
            # that is where the tool has already been told to stop trusting the base; the other
            # ninety-odd citations keep the clause the reader can see on one line.
            wide = True
            heldlist.append(f"HELD   {doc}:{li}  {keys[masked][1][:20]}  {line.strip()[:64]}")
        if re.match(r"^\s+\|", line):
            # an indented table row is a list *about* citations -- NEXT-STEPS 6.6 -- and the
            # old number in it is the subject of the sentence, not a pointer into the code
            listed += 1
            continue
        owner = None
        for m in TOK.finditer(line):
            rel = m.group("file") or owner
            if owner is None and not m.group("file"):
                continue
            if m.group("file"):
                owner = m.group("file")
            key = rel_key(rel)
            if key is None:
                continue
            a = int(m.group("a"))
            b = int(m.group("b")) if m.group("b") else a
            if b < a:
                b = a
            clause = line[max(0, m.start() - 220):m.start()]
            if wide:
                back = []
                k = li - 1
                while k > 0 and ls[k - 1].strip() and sum(len(x) + 1 for x in back) < 400:
                    back.append(ls[k - 1])
                    k -= 1
                clause = "\n".join(reversed(back)) + "\n" + line[:m.start()]
            ns = names_in(clause)
            span = f"{rel}:{a}" + (f"-{b}" if b != a else "")
            if not ns:
                judged += 1
                report.append(f"JUDGE  {doc}:{li}  {span}   no symbol named in the clause")
                continue
            fls = lines_of(key)
            if cite_pins.window_hit(fls, a, b, ns):
                ok += 1
                continue
            # Only a citation the audit is about to call wrong can be baseline-qualified, and only
            # one whose own sentence names the tree it was written against. Checked *after* the
            # symbol hit above, so the exemption never steals a passing check: an exemption that
            # fires on a citation that already points at its symbol would silently shrink the gate.
            near = line[max(0, m.start() - 80):m.end() + 80]
            if BASELINE.search(near):
                baselined.append(f"BASE   {doc}:{li}  {span}   names {ns}   "
                                 "sentence declares " + " ".join(sorted(set(BASELINE.findall(near)))))
                continue
            where = []
            for n in ns:
                pat = re.compile(r"\b" + re.escape(n) + r"\b")
                cand = [i + 1 for i, l in enumerate(fls) if pat.search(l)]
                defs = [i + 1 for i, l in enumerate(fls)
                        if SYMBOL.match(l) and n in SYMBOL.match(l).group(1)]
                if cand:
                    where.append(f"{n} -> " + (f"defined {defs[:3]}" if defs
                                               else f"lines {cand[:5]} ({len(cand)} hits)"))
            misses += 1
            report.append(f"MISS   {doc}:{li}  {span}   names {ns}   " + "; ".join(where))
            misslines.add((doc, li))

print(f"baseline the pins are measured from  : {BASE}"
      + ("" if BASE == "HEAD" else "   (the # base: line of the pin file, named below; a pin range is a line range THERE, not here)"))
print(cite_pins.provenance())
print(f"citation points at the symbol the sentence names : {ok}")
print(f"no symbol named, so a human must judge           : {judged}")
print(f"a table row listing citations, not pointing      : {listed}")
print(f"pinned, with the reason in cite-pin.txt          : {pinned}")
print(f"repaired, held from the shift but judged here    : {held}")
print(f"baseline-qualified by its own sentence           : {len(baselined)}")
print(f"UNPINNED miss -- symbol named, not where it says : {misses}")
for r in report:
    if r.startswith(("MISS", "JUDGE")):
        print(r)
# Every exemption is printed, because the count is cheap to trust and the list is not.
for r in baselined:
    print(r)
# and so is every held pointer, with the sentence that holds it: two is small enough to read,
# and the claim each one makes is about this tree, so it is the reader's job to disagree.
for r in heldlist:
    print(r)
