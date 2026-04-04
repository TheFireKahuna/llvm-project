#!/usr/bin/env python3
#
# ====- Find orphan .cpp files not declared in any CMakeLists.txt -*- python -*-==#
#
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# ==-------------------------------------------------------------------------==#
"""
Layer-1 orphan detector for the Windows libc (and the rest of libc/).

Background — there are two failure modes that silently drop libc TUs from
hermetic test EXEs:

  Layer 1 (this script): a `.cpp` file with no add_object_library /
  add_entrypoint_object / add_libc_test SRCS entry is never compiled,
  let alone added to any archive. Symptom: the file looks alive and
  `#include`-able but its symbols are absent from every link. Caught
  recently in `process/fork_quiesce.cpp` — the file existed but had no
  CMake target, so its `libc_fork_quiesce` strong override never got
  built and hermetic test fork() children trapped on the weak fallback.

  Layer 2 (handled by /includeglob: + LIBC_FORCE_PULL_GLOB): once the
  file is in the archive, COFF demand-driven selection drops it if
  the only outward artifact is a .libcveh / .libcfin / etc. section
  record. That layer is solved at the linker level.

This script catches Layer 1 only. Walks libc/src, libc/startup, libc/test
for `.cpp` files; parses every CMakeLists.txt for SRCS arguments to the
add_* family; reports any `.cpp` not referenced. Pure text scan — no CMake
invocation, no compile, runs in well under a second on the full tree.

Usage:
  python3 libc/utils/depcheck/depcheck.py [--root <path>]

Exit codes:
  0  no orphans
  1  orphans found (printed to stderr)
"""

from argparse import ArgumentParser
from pathlib import Path
import re
import sys


# Roots to scan for .cpp files. Paths are relative to the repo root.
SCAN_ROOTS = ("libc/src", "libc/startup", "libc/test")

# CMake `add_*` macros that take a SRCS keyword followed by file paths.
# The full set in llvm-libc, derived from libc/cmake/modules/.
ADD_MACROS = (
    "add_object_library",
    "add_entrypoint_object",
    "add_entrypoint_external",
    "add_libc_test",
    "add_libc_unittest",
    "add_libc_hermetic",
    "add_libc_hermetic_test",
    "add_integration_test",
    "add_libc_perf_test",
    "add_libc_fuzzer",
    "add_libc_benchmark",
    "add_libc_benchmark_unittest",
    "add_startup_object",
)

# `.cpp` files that are intentionally not compiled as standalone TUs:
# typically `#include`d from another .cpp or used as test inputs. Add
# entries with a one-line justification.
ALLOWLIST = frozenset({
    # (none yet — add as discovered)
})


def collect_cpp_files(repo_root: Path) -> set:
    """Return relative paths (POSIX-style strings) of all .cpp under SCAN_ROOTS."""
    out = set()
    for rel in SCAN_ROOTS:
        root = repo_root / rel
        if not root.exists():
            continue
        for p in root.rglob("*.cpp"):
            out.add(p.relative_to(repo_root).as_posix())
    return out


# Match any token ending in `.cpp`. CMake variable indirection
# (`set(_src foo.cpp)` then `SRCS ${_src}`) defeats SRCS-clause scanning,
# so we accept any `.cpp` token anywhere in a CMakeLists.txt. The
# alternative — a full CMake parser — is overkill for Layer-1 orphan
# detection.
CPP_TOKEN_RE = re.compile(r"[A-Za-z0-9_./\-]+\.cpp\b")

# Detect generator-style CMake patterns that produce `.cpp` paths from
# variable expansion (`${prefix}.cpp`, `stdc_${suffix}_u.cpp`, etc.). When
# present, we can't statically resolve which files the CMakeLists drives,
# so we treat every .cpp file in its directory subtree as covered.
CPP_INTERP_RE = re.compile(r"\$[\{(][A-Za-z0-9_]+[)}][A-Za-z0-9_./\-]*\.cpp\b"
                           r"|[A-Za-z0-9_./\-]*\$[\{(][A-Za-z0-9_]+[)}]"
                           r"[A-Za-z0-9_./\-]*\.cpp\b")


def collect_cmake_srcs(repo_root: Path) -> tuple:
    """
    Return (referenced_files, generator_dirs) where:
      - referenced_files: POSIX-style rel paths of .cpp explicitly named in
        any CMakeLists.txt (resolved against the CMakeLists.txt's dir).
      - generator_dirs: POSIX-style rel paths of directories whose
        CMakeLists.txt contains variable-interpolated .cpp tokens. Files
        in these subtrees are treated as covered (we can't statically
        verify, but they're under a generator).
    """
    referenced = set()
    generator_dirs = set()
    repo_root_resolved = repo_root.resolve()
    libc_root = repo_root / "libc"
    if not libc_root.exists():
        return referenced, generator_dirs
    for cml in libc_root.rglob("CMakeLists.txt"):
        text = cml.read_text(encoding="utf-8", errors="replace")
        cml_dir = cml.parent
        if CPP_INTERP_RE.search(text):
            try:
                rel_dir = cml_dir.resolve().relative_to(repo_root_resolved)
                generator_dirs.add(rel_dir.as_posix())
            except ValueError:
                pass
        for token in CPP_TOKEN_RE.findall(text):
            if token.startswith("/") or (len(token) > 1 and token[1] == ":"):
                continue
            try:
                resolved = (cml_dir / token).resolve()
                rel_path = resolved.relative_to(repo_root_resolved)
                referenced.add(rel_path.as_posix())
            except (ValueError, OSError):
                pass
    return referenced, generator_dirs


def is_under_generator(rel_path: str, generator_dirs: set) -> bool:
    """True if `rel_path` lives under any directory in `generator_dirs`."""
    for d in generator_dirs:
        if rel_path == d or rel_path.startswith(d + "/"):
            return True
    return False


def main() -> int:
    parser = ArgumentParser(description=__doc__.split("\n")[0])
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

    cpp_files = collect_cpp_files(repo_root)
    referenced, generator_dirs = collect_cmake_srcs(repo_root)
    candidate_orphans = cpp_files - referenced - ALLOWLIST
    orphans = sorted(p for p in candidate_orphans
                     if not is_under_generator(p, generator_dirs))

    if not orphans:
        print(f"depcheck: scanned {len(cpp_files)} .cpp files, no orphans.")
        return 0

    print(f"depcheck: {len(orphans)} orphan .cpp file(s) — present in tree but "
          "not declared in any add_object_library / add_libc_test / "
          "add_startup_object / etc. SRCS clause:", file=sys.stderr)
    for o in orphans:
        print(f"  {o}", file=sys.stderr)
    print("", file=sys.stderr)
    print("If a file is intentionally #include'd from another .cpp rather than "
          "compiled standalone, add it to ALLOWLIST in this script with a "
          "one-line justification.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
