#!/usr/bin/env python3
#
# ====- CI gate: no MEM_COMMIT outside nt_pal:: --------------*- python -*-==#
#
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# ==-------------------------------------------------------------------------==#
"""
Enforce the Layer 0 PAL invariant: every `MEM_COMMIT` allocation-type use
in the libc tree lives inside `nt_pal::`.

`NTPOSIX_MEMORY_ARCHITECTURE_DESIGN` §6.0 + §17.1 require that every
private commit pairs `MEM_COMMIT` with `MEM_WRITE_WATCH`, so the
dirty-page bitmap is universally available (fork CoW preservation,
`MADV_DONTNEED` fast paths, slab reclaim telemetry, mremap split-remap
dirty detection). The architectural fix is to route every commit through
`nt_pal::commit_replace` (which has no opt-out for `MEM_WRITE_WATCH`),
`nt_pal::commit_replace_large` (the documented carve-out — the kernel
rejects `MEM_LARGE_PAGES + MEM_WRITE_WATCH`, MMAP_OPTIMIZATION_RESEARCH
§17.9), or one of the small set of explicit no-watch entry points in
`nt_pal/placeholder.h`. Any direct
`NtAllocateVirtualMemoryEx(..., MEM_COMMIT, ...)` outside the PAL
silently violates the invariant and motivates this gate.

The script greps the libc tree for `MEM_COMMIT`, suppresses occurrences
that are clearly comments or state-query reads (`mbi.State == MEM_COMMIT`,
`walk.entry->State != MEM_COMMIT`, `case MEM_COMMIT:`), and reports any
remaining use that isn't under an exempt path.

Exempt paths (rationale documented inline in `EXEMPT_PATHS`):

  * `libc/src/__support/OSUtil/windows/nt_pal/`         — the PAL itself
  * `libc/src/__support/OSUtil/windows/nt/`             — DDK type / constant
                                                          definitions and
                                                          shape doc-comments
  * `libc/src/__support/OSUtil/windows/alloc/page_alloc.h`
                                                        — pre-PAL salvage
                                                          path; scheduled
                                                          for removal once
                                                          consumers migrate
  * `libc/test/`                                         — hermetic tests
                                                          must call raw NT
  * `libc/startup/`                                      — DLL CRT bootstrap
                                                          runs before libc
                                                          init, has no PAL
                                                          to call

Usage:
  python3 libc/utils/depcheck/check_no_mem_commit.py [--root <path>]

Exit codes:
  0  no violations
  1  violations found (printed to stderr)
  2  bad invocation (`--root` is not an llvm-project root)
"""

from argparse import ArgumentParser
from pathlib import Path
import re
import sys


# Subtrees that may legitimately reference NT virtual-memory APIs.
SCAN_ROOTS = (
    "libc/src",
    "libc/startup",
    "libc/test",
)

# File extensions worth scanning. Limited to C / C++ source and headers.
SCAN_EXTS = (".h", ".hpp", ".cpp", ".cc", ".c", ".inl")

# Files / directories whose `MEM_COMMIT` use is a documented exception.
# Paths are relative to the repo root and use POSIX separators. A trailing
# "/" denotes a directory subtree; an entry without a trailing "/" is an
# exact file match.
EXEMPT_PATHS = (
    # Layer 0 PAL — the only intended owner of `MEM_COMMIT`.
    "libc/src/__support/OSUtil/windows/nt_pal/",

    # DDK type / constant definitions, plus shape doc-comments that have
    # to mention the flag by name.
    "libc/src/__support/OSUtil/windows/nt/",

    # Pre-PAL raw-page primitives — `page_commit`, `page_commit_numa`,
    # `page_alloc`. The Phase-1 close-out audit calls these out by name;
    # they continue to ship until their consumers migrate to the PAL.
    "libc/src/__support/OSUtil/windows/alloc/page_alloc.h",

    # Hermetic libc tests reach into raw NT for fixture setup (e.g.
    # reserve-then-commit a guard page in `thread_registry_stress_test`).
    "libc/test/",

    # DLL CRT bootstrap (`__cxa_atexit` / `__cxa_finalize` for libunwind
    # and libc++). Loaded before libc init, so it cannot call into the PAL
    # — has its own embedded `MEM_COMMIT` constant + direct
    # `NtAllocateVirtualMemoryEx`.
    "libc/startup/",
)

# Match `MEM_COMMIT` as a whole token. `MEM_DECOMMIT` is not a substring
# of this regex's match, so the boundary is defence in depth against any
# future flag whose name shares the suffix.
MEM_COMMIT_RE = re.compile(r"\bMEM_COMMIT\b")

# Detect a trailing `case` keyword in the prefix before MEM_COMMIT, so
# `case MEM_COMMIT:` switch labels are treated as state lookups.
CASE_TAIL_RE = re.compile(r"\bcase\s*$")


def is_exempt(rel_path: str) -> bool:
    """True iff `rel_path` is covered by a directory or file in EXEMPT_PATHS."""
    for ex in EXEMPT_PATHS:
        if ex.endswith("/"):
            if rel_path.startswith(ex):
                return True
        elif rel_path == ex:
            return True
    return False


def strip_comments(text: str) -> str:
    """
    Return `text` with all C/C++ comments removed but line breaks
    preserved (so line numbers in the result match the input). Tracks
    block comments across lines, so a `/* ... */` opened on one line and
    closed many lines later strips correctly. String literals are left
    intact — `MEM_COMMIT` inside a string would still be flagged, but
    that is vanishingly rare in this tree and not a false-positive
    pattern worth handling.
    """
    out = []
    i = 0
    n = len(text)
    in_block = False
    while i < n:
        if in_block:
            j = text.find("*/", i)
            if j == -1:
                # Block runs to EOF: drop everything but preserve newlines.
                out.append("\n" * text.count("\n", i))
                i = n
            else:
                out.append("\n" * text.count("\n", i, j + 2))
                i = j + 2
                in_block = False
        else:
            block = text.find("/*", i)
            line = text.find("//", i)
            nl = text.find("\n", i)
            # Find the next interesting token among /*, //, \n.
            candidates = [c for c in (block, line, nl) if c != -1]
            if not candidates:
                out.append(text[i:])
                i = n
                continue
            j = min(candidates)
            if j == block:
                out.append(text[i:block])
                i = block + 2
                in_block = True
            elif j == line:
                # Strip from "//" to end of line; preserve the newline.
                end_of_line = text.find("\n", line)
                if end_of_line == -1:
                    out.append(text[i:line])
                    i = n
                else:
                    out.append(text[i:line])
                    out.append("\n")
                    i = end_of_line + 1
            else:  # j == nl
                out.append(text[i:nl + 1])
                i = nl + 1
    return "".join(out)


def is_state_query(prefix: str) -> bool:
    """
    True iff the text before MEM_COMMIT on the same line is a comparison
    or `case` label rather than an allocation-type argument.
    """
    p = prefix.rstrip()
    if p.endswith("==") or p.endswith("!="):
        return True
    if CASE_TAIL_RE.search(p):
        return True
    return False


def scan_file(path: Path, rel_path: str) -> list:
    """Return a list of (rel_path, lineno, raw_line) violations for `path`."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    if "MEM_COMMIT" not in text:
        return []
    code = strip_comments(text)
    raw_lines = text.splitlines()
    code_lines = code.splitlines()
    violations = []
    # `splitlines` may diverge by one if the file ends without a newline
    # in one but not the other; clamp to the shorter to be safe.
    upper = min(len(raw_lines), len(code_lines))
    for idx in range(upper):
        cl = code_lines[idx]
        if "MEM_COMMIT" not in cl:
            continue
        flagged = False
        for m in MEM_COMMIT_RE.finditer(cl):
            if not is_state_query(cl[:m.start()]):
                flagged = True
                break
        if flagged:
            violations.append((rel_path, idx + 1, raw_lines[idx].rstrip()))
    return violations


def collect_files(repo_root: Path) -> list:
    files = []
    for rel in SCAN_ROOTS:
        root = repo_root / rel
        if not root.exists():
            continue
        for p in root.rglob("*"):
            if p.is_file() and p.suffix.lower() in SCAN_EXTS:
                files.append(p)
    return files


def main() -> int:
    parser = ArgumentParser(
        description=(
            "Enforce: every MEM_COMMIT allocation-type use lives in nt_pal::."
        )
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[3],
        help="Repo root (default: derived from script path).",
    )
    args = parser.parse_args()
    repo_root = args.root.resolve()
    if not (repo_root / "libc" / "CMakeLists.txt").exists():
        print(f"error: {repo_root} does not look like an llvm-project root",
              file=sys.stderr)
        return 2

    files = collect_files(repo_root)
    violations = []
    scanned = 0
    for f in files:
        try:
            rel = f.resolve().relative_to(repo_root).as_posix()
        except ValueError:
            continue
        if is_exempt(rel):
            continue
        scanned += 1
        violations.extend(scan_file(f, rel))

    if not violations:
        print(f"check_no_mem_commit: scanned {scanned} non-exempt file(s); "
              "no MEM_COMMIT use outside nt_pal::.")
        return 0

    print(f"check_no_mem_commit: {len(violations)} violation(s) — "
          "MEM_COMMIT used outside nt_pal::.", file=sys.stderr)
    print("Every private commit must go through nt_pal::commit_replace "
          "(MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | MEM_WRITE_WATCH); see "
          "NTPOSIX_MEMORY_ARCHITECTURE_DESIGN §6.0 / §17.1 and "
          "libc/src/__support/OSUtil/windows/nt_pal/placeholder.h.",
          file=sys.stderr)
    print("", file=sys.stderr)
    for rel, lineno, text in violations:
        print(f"  {rel}:{lineno}: {text}", file=sys.stderr)
    print("", file=sys.stderr)
    print("If a use is intentional and architecturally exempt, add the file "
          "or directory to EXEMPT_PATHS in this script with a one-line "
          "rationale.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
