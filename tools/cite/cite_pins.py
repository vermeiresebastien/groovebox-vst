#!/usr/bin/env python3
"""The one reader of tools/cite/cite-pin.txt, shared by the sweep and the audit.

Both scripts have to answer the same question -- "is this doc line pinned on purpose?" -- and
they look at it from opposite ends: the sweep works in base coordinates (that is what a pin
range means), the audit works in working-copy coordinates (that is what it prints). Reading
the file twice, in two places, is how the two disagree about which lines are excepted -- and
a disagreement here silently converts a pinned line into a shifted one, which is the exact
failure the pin exists to prevent.

A pin range is *base* coordinates -- the commit named on the `# base:` line, which is HEAD only
when the file was written. Working it out from HEAD is not safe: the lane that finishes a phase
commits its own docs, and that commit is the new HEAD, so every pin range in the file silently
re-scopes to a different set of lines the moment it is run again. Measured 2026-11-16: the same
command over the same tree, before and after one commit, reported `43 already right, 0 to
correct, 0 unresolvable / UNPINNED miss: 0` and then `122 already right, 2 to correct /
UNPINNED miss: 13` -- thirteen of those were the fifteen `unverified` pins falling out of the
slice they were written against. The working-copy side is matched by masked text instead:
these docs grow, and an index taken from a base range after an edit above it points at a
different paragraph -- a plausible-looking one, since documents of this shape repeat.

Three things changed when this folder stopped being `out/`, and each one is a defect that was
measured before it was fixed:

  * The pin file is found next to *this module*, never inside the tree being audited. Reading
    `ROOT/out/cite-pin.txt` is what made a correct tree report 22 misses: the shipping lane ran
    `python out/wt-44/out/cite-audit.py <main-tree>`, the script came from one lane and the
    exemptions from the other, so the 31 pinned citations fell out of the set and were
    re-reported as findings. Scripts and their exemption list now travel together, and the tree
    argument selects only what gets *judged*. `CITE_PIN=<path>` overrides the location, which is
    how the two pin sets were compared against each other over one doc set.
  * Absent, base-less or empty is a **refusal**, exit 3 -- not an empty exemption list. The
    hazard is asymmetric: a missing pin file with the old behaviour still prints a number, and
    the number it prints is `22` findings on a tree that has none, or `pinned: 0` beside a
    nonzero miss count. Either way the reader acts on it. A run that reports a census it could
    not have computed is worse than a run that refuses, so nothing is printed on those paths.
  * The census lines now name the pin file's path, its `# base:`, its entry count and its own
    sha256, because `miss: 0` is only evidence when you can see which exemption set produced it.
"""
import hashlib
import os
import re
import subprocess
import sys

PINNED = re.compile(r"^(\S+)\s+(\d+)-(\d+)\s+#\s*(.*)$")
BASE = re.compile(r"^#\s*base\s*:\s*(\S+)")
FILE = r"[A-Za-z0-9_.\-/]*[A-Za-z0-9_\-]\.(?:cpp|h|hpp|cc|iss|txt|md)"

# The citation grammar, in one place. Four scripts read citations -- the sweep, the audit, the
# head-check and the judge -- and each used to spell this pattern out again, which is the
# "list written twice" rule this repo already has, one layer down: the sweep and the audit then
# disagree about what *is* a citation before they ever disagree about whether it is right.
#
# The digits are capped at five and a sixth refuses the match (`(?!\d)`) rather than truncating.
# The cap used to be four, and `Tests/SmokeTest.cpp` passed 9,999 lines this phase: `:10863`
# matched as `:1086` with the `3` left outside the token. In the audit that is a MISS reported
# with confident evidence -- it searched for the symbol near line 1086, not 10863 -- and in the
# sweep it is worse than a false alarm, because `--apply` rewrites the four digits it captured
# and leaves the fifth where it was, so a correct pointer becomes `:11073` for a symbol at
# 10863. A truncating grammar is a pointer-corrupting grammar; a lookahead that refuses is a
# pointer the sweep declines to touch, which is the failure direction the whole tool is built to
# have. Widen the cap and re-check every 5-digit citation when a source file passes 99,999.
CITE = re.compile(r"(?:(?P<file>" + FILE + r")(?P<qsep>[: ])|(?P<bsep>:))"
                  r"(?P<a>\d{1,5})(?P<mid>[,-](?P<b>\d{1,5}))?(?!\d)")

# `cite-sweep.py` and `cite-audit.py` read the groups by name; `masked_pinned` above only asks
# whether a line carries a citation at all. One pattern, two names, so the digit rule cannot
# drift the way the four copies of it just did.
TOK = CITE


def mask(line):
    """Digits out, so a citation shift and its own text compare equal."""
    return re.sub(r"\d+", "#", line)


def show(root, ref, rel):
    """`git show <ref>:<path>` with CRLF normalised, or None if the ref has no such file."""
    r = subprocess.run(["git", "-C", root, "show", f"{ref}:{rel}"],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    return r.stdout.replace("\r\n", "\n") if r.returncode == 0 else None


PIN_FILENAME = "cite-pin.txt"
PIN_FOLDER = os.path.dirname(os.path.abspath(__file__))


def pins_path():
    """Where the exemption list is read from: `CITE_PIN` if set, else beside this module."""
    return os.path.abspath(os.environ.get("CITE_PIN") or os.path.join(PIN_FOLDER, PIN_FILENAME))


def _refuse(why, path):
    """Exit 3 with no census. A report the reader acts on is only ever printed after the
    exemptions have actually loaded, so this is the one output path that must not share it."""
    sys.stderr.write(f"REFUSED -- the citation gate cannot run without its exemption set.\n"
                     f"  pin file: {path}\n  {why}\n")
    raise SystemExit(3)


_CACHE = {}


def read_pins():
    """Parse the pin file once, refusing on the three states that are not a usable exemption set."""
    if "all" in _CACHE:
        return _CACHE["all"]
    path = pins_path()
    if not os.path.isfile(path):
        _refuse("not found. Either restore it from git (`git checkout -- tools/cite/`) or set "
                "CITE_PIN to a pin list; an empty exemption list would re-report every pinned "
                "citation as a miss.", path)
    raw = open(path, "rb").read()
    base_ref, pins = None, {}
    for line in raw.decode("utf-8", errors="replace").split("\n"):
        m = BASE.match(line.strip())
        if m:
            base_ref = m.group(1)
        m = PINNED.match(line.strip())
        if m:
            pins.setdefault(m.group(1), []).append((int(m.group(2)), int(m.group(3)), m.group(4)))
    if base_ref is None:
        _refuse("has no `# base:` line. A pin range is a line range in that commit, so without it "
                "the ranges are read in whatever HEAD happens to be -- measured to turn `miss: 0` "
                "into `miss: 13` with no error anywhere.", path)
    if not pins:
        _refuse(f"holds no `file first-last # reason` entry, so nothing is exempted while the "
                f"prose that expects exemptions is still in the docs ({path}).", path)
    _CACHE["all"] = {"path": path, "base": base_ref, "pins": pins,
                     "entries": sum(len(v) for v in pins.values()),
                     "sha": hashlib.sha256(raw).hexdigest()[:16]}
    return _CACHE["all"]


def base(root=None):
    """The commit every coordinate in cite-pin.txt is measured in.

    `root` is accepted and ignored: it used to select the pin file, which is how one lane's
    scripts audited another tree with the wrong exemption set. See the module docstring.
    """
    return read_pins()["base"]


def load(root=None):
    """doc -> [(first, last, reason)] in base coordinates."""
    return read_pins()["pins"]


def provenance():
    """The one-line saying of which pin file produced this run's classification."""
    p = read_pins()
    return (f"pin set: {p['path']}  entries {p['entries']}  # base: {p['base']}  "
            f"sha256 {p['sha']}...")


def masked_pinned(head_lines, ranges):
    """{masked base text: (base line, reason)} for every base line inside a pin range.

    Membership is the working-copy answer to "is this line excepted from the shift", and it is
    the same key the sweep matches a rewrite on, so the exception and the edit cannot drift
    apart. The reason string is returned with it because the reasons are not all the same job,
    and this is the one place that says which is which -- `startswith ("repaired")` in two
    callers would be a second grammar of the same file, three lines from the first.

      `pinned:` / `unverified:` -- the sentence belongs to the baseline's tree, so the base's
        text is restored over any shift. Skipping is not enough: an exception that only declines
        to act cannot undo the action it declines, and that is how A3 went already-shifted.
      `repaired:` -- the opposite claim. The base's number was wrong the day it was written (the
        sweep preserves what it pointed at, so it renews the error at every phase), and the
        working copy's number is the one that holds the symbol. Hold, never restore: writing the
        base back would undo a pointer that was checked by hand. The audit still judges these,
        which is what makes the repair a gate rather than an opinion.
    """
    keys = {}
    for first, last, why in ranges:
        for hl in head_lines[max(0, first - 1):min(last, len(head_lines))]:
            if CITE.search(hl):
                keys.setdefault(mask(hl), (hl, why))
    return keys


def is_repair(masked_pinned_entry):
    """Is this pin reason the hold-dont-restore kind? One test, both callers.

    Takes the `(line, why)` value that `masked_pinned` returns rather than re-reading cite-pin.txt.
    """
    return masked_pinned_entry[1].startswith("repaired")


# --------------------------------------------------------------------------- the symbol test
# Where a citation's number actually puts you, and whether the sentence's own symbol is there.
# The audit asks this to decide `UNPINNED miss`; the head-check asks it to tell a pointer that has
# drifted from one the sweep *already* corrected this session -- because that second case is a
# number its HEAD -> working map wants to move a third time, and nothing on the printed line says
# which of the two you are holding. Both used to spell the rule out separately.

SYMBOL = re.compile(r"^[A-Za-z_][\w:<>&*, ]*\s+([\w~][\w:~]*::[\w~]+|[\w~]+)\s*\(")
BACK = re.compile(r"`([^`\n]{2,60})`")
CAND = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(?:[A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)?"
                  r"(?:\(\)?)?|\([a-z]?[a-z, )=&]*\))$")


def names_in(clause):
    """Symbols the sentence names, most specific first."""
    out = []
    for m in BACK.finditer(clause):
        t = m.group(1).strip()
        if t.endswith((".cpp", ".h", ".txt", ".md", ".iss")) or not re.search(r"[A-Za-z]", t):
            continue
        # a commit id is not an identifier: `ecfe76c` passes CAND, and a search for it in a
        # source file answers "not found" with the confidence of a real symbol
        if re.fullmatch(r"[0-9a-f]{7,40}", t):
            continue
        t = t.split(":")[0].strip()
        if CAND.match(t) and not re.search(r"[ .]", t):
            out.append(t)
    for m in re.finditer(r"\b([A-Za-z_][A-Za-z0-9_]*::[A-Za-z_][A-Za-z0-9_]*"
                         r"|[a-z][A-Za-z0-9]*[A-Z][A-Za-z0-9]*)\b", clause):
        if m.group(1) not in out:
            out.append(m.group(1))
    return out[:4]


def enclosing(lines, n, back=80):
    """The signature a line sits under, searched upwards."""
    for i in range(min(n, len(lines)) - 1, max(0, min(n, len(lines)) - 1 - back), -1):
        m = SYMBOL.match(lines[i])
        if m:
            return m.group(1)
    return ""


def window_hit(lines, a, b, names):
    """Do the cited lines -- or the functions they sit in -- hold one of these names?

    A qualified name is tried whole and by its last component: `Layout::firstMidiNote` is written
    `inline constexpr int firstMidiNote` inside `namespace Layout`, and a checker that matched
    only the whole string would report a correct citation as a miss. Returns the name it saw.
    """
    if not names or not lines:
        return ""
    lo, hi = max(1, a - 4), min(len(lines), b + 4)
    window = "\n".join(lines[lo - 1:hi])
    enc = {enclosing(lines, x) for x in range(a, b + 1)}
    tries = []
    for n in names:
        tries.append(n)
        if "::" in n:
            tries.append(n.split("::")[-1])
    for n in tries:
        if n in window or any(n == e or e.startswith(n) or n in e for e in enc if e):
            return n
    return ""

