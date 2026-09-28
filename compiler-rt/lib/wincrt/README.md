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
| Termination | `__cxa_atexit`, `__cxa_finalize`, `__cxa_at_quick_exit`, the `.CRT$XP*` pre-terminators and `.CRT$XT*` terminators, `_is_c_termination_complete` | `cxa_atexit.cpp`, `init.cpp` |
| Thread-local destructors | `__cxa_thread_atexit_impl`, `__cxa_thread_finalize`, a TLS callback | `cxa_thread_atexit.cpp` |
| C registration | `atexit`, `_onexit`, `onexit`, `at_quick_exit` | `atexit.cpp` |
| Pure virtual calls | `_purecall`, over the Universal CRT's handler | `purecall.cpp` |
| Stack protector | `__security_cookie`, `__security_init_cookie`, `__security_check_cookie`, `__report_gsfailure` | `security.cpp` |
| MSVC `/GS` objects | `__GSHandlerCheck`, `__GSHandlerCheck_SEH`, `__report_rangecheckfailure`, and on AArch64 `__security_push_cookie`, `__security_pop_cookie` | `gshandler.cpp`, `gs_cookie.S` |
| Load configuration | `_load_config_used` | `loadconfig.cpp` |
| Control Flow Guard | `__guard_check_icall_fptr`, `__guard_dispatch_icall_fptr`, `_guard_icall_checks_enforced` | `cfguard.cpp`, `cfguard_dispatch.S` |
| Delay-load imports | `__delayLoadHelper2`, `__FUnloadDelayLoadedDLL2`, `__HrLoadAllImportsForDll` | `delayload.cpp` |
| Thread-local storage | `_tls_used`, `_tls_index`, `_tls_start`, `_tls_end`, `__xl_a`, `__xl_z` | `tls.cpp` |
| Universal CRT math data | `_HUGE`, `HUGE` | `ucrt_math.c` |
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
name does. Each is an import, with no wrapper code. The traditional names of
what `ucrtbase.dll` does not export, `onexit` and `HUGE`, are weak definitions
in wincrt instead, so that a program's own definition takes precedence.

## Termination

Static destructors and `atexit` functions run in exact reverse order of
registration across the images of a process. The registries are lock-free:
each registering image has a record of entries, every entry a global
sequence number, and exit merges the records by it. A registry that lives as
long as the process posts one `_crt_atexit` token, which orders it among other
runtimes' `atexit` functions; a DLL runs its own registrations at its detach,
after its `DllMain`. A thread's thread-local destructors run when it exits,
and the exiting thread's run before any static destructor; a pending one keeps
its image loaded. Compiled into the shared C++ runtime with
`WINCRT_SHARED_CXX_RUNTIME`, the registries serve every image of the process;
compiled into wincrt, they have local names that alternate names select only
in an image that does not import the shared ones.

## The segment heap

A Windows Itanium executable runs on the segment heap. The driver embeds
`segment_heap.manifest`, installed beside wincrt, in every executable, and the
executable's start-up fails with an error if the process heap is not a
segment heap, as it is not when the process was started without the
executable's activation context.

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
