#!/usr/bin/env python3
# ===----------------------------------------------------------------------===##
#
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# ===----------------------------------------------------------------------===##
"""Checks the images of a deployed directory for vague entities they split.

A Windows Itanium or NT-POSIX DLL linked with -import-slots exports each
observable vague entity with default visibility that it keeps, and imports
one that a DLL it imports from offers rather than keeping a copy, as ELF gives
a program one instance of such an entity among the shared objects it needs.
This script reads the export and import tables of every image in a directory,
with llvm-readobj, and reports:

  - two images that both export an entity, neither of which imports from the
    other, directly or through other images of the directory;
  - an image that exports its own copy of an entity, not a forwarder, while
    importing from an image that exports the entity too: it was linked
    against a release of that image that did not define the entity.

Only Itanium-mangled names count. The exit code is 1 if anything is reported.
"""

import argparse
import os
import shutil
import subprocess
import sys


def read_image(readobj, path):
    out = subprocess.check_output(
        [readobj, "--coff-exports", "--coff-imports", path],
        stderr=subprocess.DEVNULL,
    ).decode(errors="replace")
    exports, imports = {}, set()
    block, name, forward = None, None, False
    for line in out.splitlines():
        line = line.strip()
        if line == "Export {":
            block, name, forward = "export", None, False
        elif line in ("Import {", "DelayImport {"):
            block = "import"
        elif line == "}":
            if block == "export" and name:
                exports[name] = forward
            block = None
        elif block == "export" and line.startswith("Name: "):
            name = line[len("Name: ") :]
        elif block == "export" and line.startswith("ForwardedTo: "):
            forward = True
        elif block == "import" and line.startswith("Name: "):
            imports.add(line[len("Name: ") :].lower())
    return exports, imports


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("directory", help="the directory of deployed images")
    parser.add_argument(
        "--readobj", default=shutil.which("llvm-readobj"), help="llvm-readobj"
    )
    args = parser.parse_args()
    if not args.readobj:
        sys.exit("error: llvm-readobj not found; pass --readobj")

    images = {}
    for entry in sorted(os.listdir(args.directory)):
        if os.path.splitext(entry)[1].lower() in (".dll", ".exe"):
            images[entry.lower()] = read_image(
                args.readobj, os.path.join(args.directory, entry)
            )

    # Which images each image reaches through the imports of the directory.
    reaches = {}
    for name in images:
        seen, work = set(), list(images[name][1])
        while work:
            dll = work.pop()
            if dll in seen or dll not in images:
                continue
            seen.add(dll)
            work.extend(images[dll][1])
        reaches[name] = seen

    keepers = {}
    for name, (exports, _) in images.items():
        for entity, forward in exports.items():
            if entity.startswith("_Z") and not forward:
                keepers.setdefault(entity, []).append(name)

    reported = 0
    for entity, names in sorted(keepers.items()):
        for i, a in enumerate(names):
            for b in names[i + 1 :]:
                if b in reaches[a] or a in reaches[b]:
                    importer, owner = (a, b) if b in reaches[a] else (b, a)
                    print(
                        f"{importer}: keeps its own copy of {entity}, which "
                        f"{owner}, which it imports from, defines; relink "
                        f"{importer}"
                    )
                else:
                    print(
                        f"{entity} is defined by both {a} and {b}, neither of "
                        f"which imports from the other"
                    )
                reported += 1
    return 1 if reported else 0


if __name__ == "__main__":
    sys.exit(main())
