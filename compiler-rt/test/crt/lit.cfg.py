# -*- Python -*-

import os
import shlex

import lit.formats

config.name = "CRT" + config.name_suffix
config.test_source_root = os.path.join(config.crt_lit_source_dir, "TestCases")
config.suffixes = [".c", ".cpp"]
config.excludes = ["Inputs"]
config.test_format = lit.formats.ShTest(execute_external=False)

runtime_suffix = "" if config.enable_per_target_runtime_dir else "-" + config.target_arch
wincrt = os.path.join(config.crt_test_libdir, "clang_rt.wincrt" + runtime_suffix + ".lib")
if not os.path.isfile(wincrt):
    lit_config.fatal("wincrt library not built: " + wincrt)

common_flags = shlex.split(config.target_cflags) + [
    "--target=" + config.target_triple,
    "--rtlib=compiler-rt",
    "-fuse-ld=lld",
    "-resource-dir=" + config.compiler_rt_output_dir,
]
for directory in config.crt_test_link_dirs.split(";"):
    if directory:
        common_flags.append("-L" + directory)

cxx_flags = []
if config.crt_test_include_dirs:
    cxx_flags.append("-nostdinc++")
    for directory in config.crt_test_include_dirs.split(";"):
        cxx_flags.append("-I" + directory)

# Longest substitutions go first: lit substitutions also match prefixes.
# Use the C++ driver for C++ inputs so the tested libc++ supplies the ABI hooks.
for prefix, flags in [
    ("%clangxx_crt", ["--driver-mode=g++"] + cxx_flags),
    ("%clang_crt", ["--driver-mode=gcc"]),
]:
    compiler = config.clang + " " + shlex.join(flags + common_flags)
    for suffix, entry_flags in [
        ("_main_cfg", ["-mguard=cf"]),
        ("_wwinmain", ["-mwindows", "-Wl,-entry:wWinMainCRTStartup"]),
        ("_winmain", ["-mwindows"]),
        ("_wmain", ["-Wl,-entry:wmainCRTStartup"]),
        ("_main", []),
        ("_dll", ["-shared"]),
        ("", []),
    ]:
        config.substitutions.append(
            (prefix + suffix, compiler + " " + shlex.join(entry_flags) + " "),
        )

config.substitutions.append(("%run", ""))
for directory in reversed(config.crt_test_dll_dirs.split(";")):
    if directory:
        config.environment["PATH"] = directory + os.pathsep + config.environment["PATH"]

config.available_features.update(["windows", "crt", config.target_arch])
