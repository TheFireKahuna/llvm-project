# wincrt, the start-up library of Windows Itanium images, is built as the C
# runtime start files there. Its tests link through the driver, so that a test
# gets the libraries and the entry point that a user's program gets.

import os
import sys

if "crt" not in config.available_features or not config.target_triple.endswith(
    "-windows-itanium"
):
    config.unsupported = True

inputs = os.path.join(os.path.dirname(os.path.abspath(__file__)), "Inputs")
config.substitutions += [
    # Every image imports the process's termination registries from this DLL,
    # which the builtins build places beside wincrt.
    (
        "%wincrt_dll",
        os.path.join(
            config.compiler_rt_libdir,
            "clang_rt.wincrt_dynamic%s.dll" % config.target_suffix,
        ),
    ),
    ("%exit_status", '"%s" %s' % (sys.executable, os.path.join(inputs, "exit_status.py"))),
]
