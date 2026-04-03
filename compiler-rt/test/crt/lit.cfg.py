# -*- Python -*-

import os
import sys

import lit.formats


def get_required_attr(config, attr_name):
    attr_value = getattr(config, attr_name, None)
    if attr_value is None:
        lit_config.fatal(
            "No attribute %r in test configuration! You may need to run "
            "tests from your build directory or add this attribute "
            "to lit.site.cfg.py " % attr_name
        )
    return attr_value


config.name = "CRT" + config.name_suffix
config.test_source_root = os.path.dirname(__file__)
config.suffixes = [".c", ".cpp"]

crt_lit_source_dir = get_required_attr(config, "crt_lit_source_dir")
crt_test_libdir = get_required_attr(config, "crt_test_libdir")
target_arch = get_required_attr(config, "target_arch")
is_msvc = get_required_attr(config, "is_msvc")

if sys.platform in ["win32"]:
    crt_test_libdir = crt_test_libdir.replace("\\", "/")

if target_arch == "x86_64":
    target_suffix = "-x86_64"
elif target_arch == "i386":
    target_suffix = "-i386"
elif target_arch == "aarch64":
    target_suffix = "-aarch64"
elif target_arch == "arm":
    target_suffix = "-arm"
else:
    target_suffix = ""

if is_msvc:
    crt_obj_ext = ".obj"
else:
    crt_obj_ext = ".o"

crt_main_obj = os.path.join(crt_test_libdir, "clang_rt.crt_main" + target_suffix + crt_obj_ext)
crt_wmain_obj = os.path.join(crt_test_libdir, "clang_rt.crt_wmain" + target_suffix + crt_obj_ext)
crt_winmain_obj = os.path.join(crt_test_libdir, "clang_rt.crt_winmain" + target_suffix + crt_obj_ext)
crt_wwinmain_obj = os.path.join(crt_test_libdir, "clang_rt.crt_wwinmain" + target_suffix + crt_obj_ext)
crt_dllmain_obj = os.path.join(crt_test_libdir, "clang_rt.crt_dllmain" + target_suffix + crt_obj_ext)

if sys.platform in ["win32"]:
    crt_main_obj = crt_main_obj.replace("\\", "/")
    crt_wmain_obj = crt_wmain_obj.replace("\\", "/")
    crt_winmain_obj = crt_winmain_obj.replace("\\", "/")
    crt_wwinmain_obj = crt_wwinmain_obj.replace("\\", "/")
    crt_dllmain_obj = crt_dllmain_obj.replace("\\", "/")

common_cflags = [
    "-target", "x86_64-unknown-windows-itanium",
    "-fuse-ld=lld",
    "-Xlinker", "-lkernel32",
    "-Xlinker", "-lucrt",
]

config.substitutions.append(
    ("%clang_crt_main",
     " " + config.clang + " " + " ".join(common_cflags) + " " + crt_main_obj + " ")
)
config.substitutions.append(
    ("%clang_crt_wmain",
     " " + config.clang + " " + " ".join(common_cflags) + " " + crt_wmain_obj + " ")
)
config.substitutions.append(
    ("%clang_crt_winmain",
     " " + config.clang + " " + " ".join(common_cflags) + " " + crt_winmain_obj + " -Xlinker -luser32 ")
)
config.substitutions.append(
    ("%clang_crt_wwinmain",
     " " + config.clang + " " + " ".join(common_cflags) + " " + crt_wwinmain_obj + " -Xlinker -luser32 ")
)
config.substitutions.append(
    ("%clang_crt_dll",
     " " + config.clang + " " + " ".join(common_cflags) + " -shared " + crt_dllmain_obj + " ")
)
config.substitutions.append(("%crt_obj_main", crt_main_obj))
config.substitutions.append(("%crt_obj_wmain", crt_wmain_obj))
config.substitutions.append(("%crt_obj_winmain", crt_winmain_obj))
config.substitutions.append(("%crt_obj_wwinmain", crt_wwinmain_obj))
config.substitutions.append(("%crt_obj_dllmain", crt_dllmain_obj))
config.substitutions.append(("%run", ""))
config.substitutions.append(
    ("%clang_crt_main_cfg",
     " " + config.clang + " " + " ".join(common_cflags) + " -Xclang -cfguard " + crt_main_obj + " ")
)

config.available_features.add("windows")
config.available_features.add("crt")

if target_arch == "x86_64":
    config.available_features.add("x86_64")
elif target_arch == "i386":
    config.available_features.add("i386")
elif target_arch == "aarch64":
    config.available_features.add("aarch64")
elif target_arch == "arm":
    config.available_features.add("arm")

config.available_features.add("native-run")
