"""Checks that each function an image binds from its C library that ntdllp.lib
offers too is wrapped by every entry object, and that the __wrap_ name of
every function an entry object wraps is defined for an image that imports the
runtime and for one linked with -static.

Usage: wrapped_names.py <clang command>
"""

import re
import subprocess
import sys


def run(*args):
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


def find(name):
    return run(*sys.argv[1:], "-print-file-name=" + name).strip()


# The functions an import library offers, without the ARM64EC names that an
# AArch64 one holds beside them.
def imports(lib):
    members = run("llvm-readobj", lib)
    return set(re.findall(r"^Symbol: (?!__imp_|#)(\S+)$", members, re.M))


def defined(lib):
    return set(re.findall(r" T (\S+)$", run("llvm-nm", lib), re.M))


runtime = imports(find("clang_rt.wincrt_dynamic.lib"))
memory = imports(find("clang_rt.ucrt_memory.lib"))
static = defined(find("clang_rt.wincrt_static.lib"))
bound = imports(find("ucrt.lib")) | imports(find("oldnames.lib")) | runtime | memory
shared = sorted(
    (imports(find("ntdllp.lib")) & bound) - defined(find("clang_rt.wincrt.lib"))
)

errors = []
directives = run("llvm-readobj", "--coff-directives", find("clang_rt.wincrt.lib"))
entries = [set(re.findall(r"/wrap:(\S+)", d)) for d in directives.split("File:")]
for wraps in filter(None, entries):
    errors += ["not wrapped: " + n for n in shared if n not in wraps]
wrapped = set().union(*entries)
for name in sorted(wrapped):
    wrap = "__wrap_" + name
    if wrap not in memory and not (wrap in runtime and wrap in static):
        errors.append("not defined: " + wrap)
summary = "wrapped %d, ntdllp's %d: %s" % (len(wrapped), len(shared), " ".join(shared))
print("\n".join(errors or [summary]))
sys.exit(bool(errors))
