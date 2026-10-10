# wincrt, the start-up library of Windows Itanium images, is built as the C
# runtime start files there. Its tests link through the driver, so that a test
# gets the libraries and the entry point that a user's program gets. They are
# C programs, which need no unwinder, and libunwind may not be built yet.

import glob
import os
import re
import subprocess

if "crt" not in config.available_features or not config.target_triple.endswith(
    "-windows-itanium"
):
    config.unsupported = True

config.substitutions.append(
    (
        "%clang_wincrt",
        " " + config.clang + " " + config.target_cflags + " --unwindlib=none ",
    )
)

# Every image imports the process's termination registries from
# clang_rt.wincrt_dynamic.dll, which the builtins build places beside wincrt.
config.environment["PATH"] = os.pathsep.join(
    [config.compiler_rt_libdir, config.environment.get("PATH", "")]
)
config.substitutions.append(("%wincrt_libdir", config.compiler_rt_libdir))


# The Windows SDK's private ntdllp.lib imports some of the runtime's functions
# from ntdll.dll. Tests that link it, or compare it with ucrt.lib, look for
# them where the driver links the SDK's libraries from.
def find_sdk_lib(name):
    output = subprocess.run(
        [
            *config.clang.split(),
            *config.target_cflags.split(),
            "-###",
            "-x",
            "c",
            os.devnull,
        ],
        capture_output=True,
        text=True,
    ).stderr
    for libpath in re.findall(r'"-libpath:([^"]*)"', output):
        path = os.path.join(libpath.replace("\\\\", "\\"), name)
        if os.path.exists(path):
            return path
    return None


# Visual C++'s vcruntime.lib imports some of the same functions from
# vcruntime140.dll. The driver never links it; tests that compare it with the
# runtime look for it in the newest Visual C++ installation.
def find_vcruntime():
    vswhere = os.path.join(
        os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
        "Microsoft Visual Studio",
        "Installer",
        "vswhere.exe",
    )
    if not os.path.exists(vswhere):
        return None
    install = subprocess.run(
        [vswhere, "-latest", "-products", "*", "-property", "installationPath"],
        capture_output=True,
        text=True,
    ).stdout.strip()
    arch = "arm64" if config.target_triple.startswith("aarch64") else "x64"
    pattern = os.path.join(install, "VC", "Tools", "MSVC", "*", "lib", arch)
    libs = sorted(glob.glob(os.path.join(pattern, "vcruntime.lib")))
    return libs[-1] if install and libs else None


if not config.unsupported:
    ntdllp = find_sdk_lib("ntdllp.lib")
    if ntdllp:
        config.available_features.add("ntdllp")
        config.substitutions.append(("%ntdllp_lib", ntdllp))
    onecoreuap = find_sdk_lib("OneCoreUAP_apiset.lib")
    if onecoreuap:
        config.available_features.add("onecoreuap-lib")
        config.substitutions.append(("%onecoreuap_lib", onecoreuap))
    ucrt = find_sdk_lib("ucrt.lib")
    if ucrt:
        config.available_features.add("ucrt-lib")
        config.substitutions.append(("%ucrt_lib", ucrt))
    vcruntime = find_vcruntime()
    if vcruntime:
        config.available_features.add("vcruntime")
        config.substitutions.append(("%vcruntime_lib", vcruntime))
