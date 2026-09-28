# clang_rt.aligned_alloc, built beside wincrt, provides aligned_alloc and
# posix_memalign, which the Universal CRT lacks. It lies beside the builtins.
import shlex
import subprocess

if not config.unsupported:
    builtins = subprocess.check_output(
        [config.clang.strip(), "-print-libgcc-file-name"]
        + shlex.split(config.target_cflags),
        env=config.environment,
        universal_newlines=True,
    ).strip()
    library = builtins.replace("clang_rt.builtins", "clang_rt.aligned_alloc")
    config.substitutions.append(("%aligned_alloc", " " + library + " "))
