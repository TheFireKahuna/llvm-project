# wincrt

`clang_rt.wincrt.lib` is the start-up library of `x86_64-` and
`aarch64-unknown-windows-itanium` images. Microsoft's C runtime is split into
the Universal CRT (`ucrtbase.dll`, the C library) and vcruntime (start-up,
compiler support and the MSVC C++ ABI). Windows Itanium keeps the Universal
CRT and replaces vcruntime: wincrt supplies what vcruntime supplied, and
compiler-rt's builtins, libunwind and libc++abi supply the rest. No Visual C++
header or library is used.

The builtins define the bounds of the `.CRT$X??` tables, `__dso_handle` and
`_fltused` (`crt_windows.c`).

## What it provides

| Area | Symbols | Source |
| --- | --- | --- |
| Entry points | `mainCRTStartup`, `wmainCRTStartup`, `WinMainCRTStartup`, `wWinMainCRTStartup`, `_DllMainCRTStartup`, a default `DllMain`, `_CRT_INIT` | `entry_*.cpp` |
| Start-up | `.CRT$XI*` C initializers and `.CRT$XC*` constructors, the Universal CRT's arguments and environment, a program's `_matherr`, the unhandled-exception filter | `init.cpp` |
| Stack protector | `__security_cookie`, `__security_init_cookie`, `__security_check_cookie`, `__report_gsfailure` | `security.cpp` |
| Load configuration | `_load_config_used` | `loadconfig.cpp` |
| Thread-local storage | `_tls_used`, `_tls_index`, `_tls_start`, `_tls_end`, `__xl_a`, `__xl_z` | `tls.cpp` |
| Universal CRT stdio | `__local_stdio_printf_options`, `__local_stdio_scanf_options` for C images, and the ISO wide-specifier marker | `ucrt_stdio.c` |

The linker picks the entry point from the program's main function, and
extracts only that entry point's file from the archive.

`clang_rt.ucrt_memory.lib` is an import library, generated from
`ucrt_memory.def`, of the functions that `ucrtbase.dll` exports but the SDK's
`ucrt.lib` does not, because Visual C++ imports them through `vcruntime.lib`:
`memcpy` and the other memory and string functions the compiler calls,
`setjmp` and `longjmp`, `__C_specific_handler`, and the pure virtual call
handler accessors. The driver links it with wincrt.

`oldnames.lib`, generated from `oldnames.def`, maps the POSIX and other
traditional names of Universal CRT functions (`open`, `strdup` and so on) to
the underscored names `ucrtbase.dll` exports, as Visual C++'s library of that
name does. Each is an import, with no wrapper code.

## Uncaught exceptions

The executable's start-up installs the filter ntdll runs for an exception that
no frame of its thread handled. An Itanium exception that reaches it is
resumed, so that `__cxa_throw` calls `std::terminate` with nothing unwound.
The filter finds the unwinder through its exports, so it recognizes only a
libunwind DLL, not one linked statically.

## Tests

The tests are C programs in `compiler-rt/test/builtins/Unit/windows`, which
run under `check-builtins` when `COMPILER_RT_BUILD_CRT` is on (the `crt` lit
feature).
