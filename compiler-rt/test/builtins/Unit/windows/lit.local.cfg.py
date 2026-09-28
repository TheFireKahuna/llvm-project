# wincrt, the start-up library of Windows Itanium images, is built as the C
# runtime start files there. Its tests link through the driver, so that a test
# gets the libraries and the entry point that a user's program gets. They are
# C programs, which need no unwinder, and libunwind may not be built yet.
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
