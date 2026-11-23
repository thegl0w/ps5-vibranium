#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Clone detector: does any shipped source file contain code copied from another project?

Usage: tools/clonecheck.py <export tree> <report.md> <corpus name=path> [...]
Every source file of the export tree is compared with every source file of each corpus. Comments, blank lines,
whitespace and case are removed; lines shorter than 12 characters or made only of punctuation and common keywords
("return 0;", "} else {", "#endif", ...) are dropped. Each run of 4 consecutive remaining lines is a shingle. A shipped
file that shares a shingle with a corpus file is listed with the number of shared shingles, the longest shared run of
lines, and the corpus file. Licence texts and Markdown are not compared (they are not code)."""
import hashlib, re, sys
from collections import defaultdict
from pathlib import Path

EXT = {".c", ".cc", ".cpp", ".h", ".hpp", ".S", ".s", ".asm", ".nasm", ".inc", ".py", ".sh", ".ps1", ".cmd", ".bat",
       ".inf", ".dsl", ".dsc", ".dec", ".mm", ".m", ".swift", ".pl"}
SKIP_DIRS = {"build", "Build", ".git", "__pycache__", "node_modules", "out", "obj", "DerivedData"}
K = 4
TRIVIAL = re.compile(r"^[\s{}()\[\];,:.*&|!=<>+\-/\\#'\"0-9a-fx]*$|^(return|break|continue|else|default|endif|end|fi|done|"
                     r"then|do|esac|pass|exit|goto|case|#endif|#else|set -e.*|setlocal|@echo off|endlocal)\b.{0,12}$")


def lines_of(path):
    try:
        t = path.read_text(errors="replace")
    except OSError:
        return []
    t = re.sub(r"/\*.*?\*/", " ", t, flags=re.S)                    # C block comments
    out = []
    for raw in t.splitlines():
        s = re.sub(r"(//|^\s*#(?!\s*(include|define|if|endif|else|ifdef|ifndef|pragma))|^\s*rem\b|^\s*;|^\s*::).*", "",
                   raw, flags=re.I)
        s = re.sub(r"\s+", "", s).lower()
        if len(s) < 12 or TRIVIAL.match(s):
            continue
        out.append(s)
    return out


def shingles(ls):
    return {hashlib.blake2b("\n".join(ls[i:i + K]).encode(), digest_size=8).digest(): i for i in range(len(ls) - K + 1)}


def files(root):
    for p in sorted(Path(root).rglob("*")):
        if p.is_file() and not p.is_symlink() and p.suffix in EXT and not (set(p.relative_to(root).parts[:-1]) & SKIP_DIRS):
            yield p


def main():
    export, report, corpora = Path(sys.argv[1]), Path(sys.argv[2]), [a.split("=", 1) for a in sys.argv[3:]]
    index = defaultdict(set)            # shingle -> {(corpus, file)}
    counts = {}
    for name, root in corpora:
        n = 0
        for p in files(root):
            for h in shingles(lines_of(p)):
                index[h].add((name, str(p.relative_to(root))))
            n += 1
        counts[name] = n
    rows, checked = [], 0
    for p in files(export):
        rel = str(p.relative_to(export))
        if rel.startswith("release/licenses/"):
            continue
        ls = lines_of(p)
        sh = shingles(ls)
        checked += 1
        hits = defaultdict(list)
        for h, i in sh.items():
            for src in index.get(h, ()):
                hits[src].append(i)
        for src, idx in hits.items():
            idx.sort()
            run, best = 1, 1
            for a, b in zip(idx, idx[1:]):
                run = run + 1 if b == a + 1 else 1
                best = max(best, run)
            rows.append((rel, src[0], src[1], len(idx), best + K - 1, len(sh)))
    rows.sort(key=lambda r: (-r[3], r[0]))
    out = ["# Clone-detector report", "",
           f"Shipped source files compared: {checked} (export tree `{export.name}`; licence texts and Markdown excluded).",
           "Corpora: " + ", ".join(f"{n} ({counts[n]} files)" for n, _ in corpora) + ".",
           f"Method: comments, whitespace, case and trivial lines removed; shared runs of {K} consecutive lines "
           "(`tools/clonecheck.py`).", ""]
    if not rows:
        out.append("**No shared code found.**")
    else:
        out += ["| Shipped file | Corpus | Corpus file | Shared 4-line runs | Longest shared run (lines) | Shingles in file |",
                "|---|---|---|---|---|---|"]
        out += [f"| `{r[0]}` | {r[1]} | `{r[2]}` | {r[3]} | {r[4]} | {r[5]} |" for r in rows]
    report.write_text("\n".join(out) + "\n")
    print(f"{checked} files checked against {sum(counts.values())} corpus files; {len(rows)} file pairs share code")
    for r in rows[:40]:
        print(f"  {r[0]}  <->  {r[1]}:{r[2]}  runs {r[3]}, longest {r[4]} lines (of {r[5]} shingles)")


if __name__ == "__main__":
    main()
