"""Checks that wincrt wraps every name that both ntdllp.lib and the runtime DLL
offer, in each entry object and in both forms of the runtime.

Usage: ntdllp_wraps.py <ntdllp.lib> <directory of the wincrt libraries>
"""

import os
import re
import subprocess
import sys


def run(*args):
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


def main():
    ntdllp, libdir = sys.argv[1:]
    offered = set(re.findall(r" __imp_(\S+)$", run("llvm-nm", ntdllp), re.M))
    dll = os.path.join(libdir, "clang_rt.wincrt_dynamic.dll")
    exports = run("llvm-readobj", "--coff-exports", dll)
    exported = set(re.findall(r"Name: (\S+)$", exports, re.M))
    shadowed = sorted(offered & exported)

    errors = []
    # Every entry object carries the same wraps, so that each image has them.
    wincrt = os.path.join(libdir, "clang_rt.wincrt.lib")
    directives = run("llvm-readobj", "--coff-directives", wincrt)
    entries = [
        set(re.findall(r"/wrap:(\S+)", line))
        for line in directives.splitlines()
        if "/wrap:" in line
    ]
    if not entries:
        errors.append("no entry object wraps anything")
    for wraps in entries:
        errors += ["not wrapped: " + name for name in shadowed if name not in wraps]

    # The import library offers __wrap_X as an import of the name X, which
    # remains the DLL's export.
    implib = run("llvm-readobj", os.path.join(libdir, "clang_rt.wincrt_dynamic.lib"))
    imported = set(
        re.findall(r"Export name: (\S+)\n\s*Symbol: __imp___wrap_\1$", implib, re.M)
    )
    errors += ["no import of __wrap_" + n for n in shadowed if n not in imported]

    # The static runtime defines __wrap_X for a program linked with -static.
    static = run("llvm-nm", os.path.join(libdir, "clang_rt.wincrt_static.lib"))
    defined = set(re.findall(r" T __wrap_(\S+)$", static, re.M))
    errors += ["no definition of __wrap_" + n for n in shadowed if n not in defined]

    print("\n".join(errors or ["wrapped: " + " ".join(shadowed)]))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
