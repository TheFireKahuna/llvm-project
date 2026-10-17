"""Checks that wincrt wraps every Universal CRT function that another import
library also offers, in each entry object, and that clang_rt.ucrt_memory.lib
imports each __wrap_ name as the function, from the DLL that binds it.

Usage: shared_imports.py <directory of the wincrt libraries> <ucrt.lib>
                         <other import library>
"""

import os
import re
import subprocess
import sys


def run(*args):
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


def imports(lib):
    """Maps each __imp_ name of an import library to its DLL and export."""
    result = {}
    for member in run("llvm-readobj", lib).split("\n\n"):
        dll = re.search(r"^File: (\S+)$", member, re.M)
        export = re.search(r"^Export name: (\S+)$", member, re.M)
        imp = re.search(r"^Symbol: __imp_(\S+)$", member, re.M)
        if dll and export and imp:
            result[imp.group(1)] = (dll.group(1).lower(), export.group(1))
    return result


def main():
    libdir, ucrt, other = sys.argv[1:]
    memory = imports(os.path.join(libdir, "clang_rt.ucrt_memory.lib"))
    wrappers = {
        name[len("__wrap_") :]: target
        for name, target in memory.items()
        if name.startswith("__wrap_")
    }
    bound = imports(ucrt)
    bound.update(
        (name, target)
        for name, target in memory.items()
        if not name.startswith("__wrap_")
    )

    # The names the runtime defines itself are not imports.
    defined = set()
    for lib in ("clang_rt.wincrt.lib", "clang_rt.wincrt_static.lib"):
        nm = run("llvm-nm", os.path.join(libdir, lib))
        defined |= set(re.findall(r" T (\S+)$", nm, re.M))
    offered = set(imports(other))
    shared = sorted(n for n in offered & set(bound) if n not in defined)

    errors = []
    # Every entry object carries the same wraps, so that each image has them.
    directives = run(
        "llvm-readobj", "--coff-directives", os.path.join(libdir, "clang_rt.wincrt.lib")
    )
    entries = [
        set(re.findall(r"/wrap:(\S+)", line))
        for line in directives.splitlines()
        if "/wrap:" in line
    ]
    if not entries:
        errors.append("no entry object wraps anything")
    for wraps in entries:
        errors += ["not wrapped: " + n for n in shared if n not in wraps]

    # __wrap_X imports what X binds, from the same DLL.
    for name in shared:
        if wrappers.get(name) != bound[name]:
            errors.append(
                "__wrap_%s imports %s, not %s" % (name, wrappers.get(name), bound[name])
            )

    print("\n".join(errors or ["wrapped %d: %s" % (len(shared), " ".join(shared))]))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
