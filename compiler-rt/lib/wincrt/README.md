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
| Termination | `__cxa_atexit`, `__llvm_kcfi_cxa_atexit`, `__cxa_finalize`, `__cxa_at_quick_exit`, the `.CRT$XP*` pre-terminators and `.CRT$XT*` terminators, `_is_c_termination_complete` | `cxa_atexit.cpp`, `init.cpp` |
| Thread-local destructors | `__cxa_thread_atexit_impl`, `__llvm_kcfi_cxa_thread_atexit_impl`, `__cxa_thread_finalize`, a TLS callback | `cxa_thread_atexit.cpp` |
| C registration | `atexit`, `_onexit`, `onexit`, `at_quick_exit` | `atexit.cpp` |
| Pure virtual calls | `_purecall`, over the Universal CRT's handler | `purecall.cpp` |
| Stack protector | `__security_cookie`, `__security_init_cookie`, `__security_check_cookie`, `__report_gsfailure` | `security.cpp` |
| MSVC `/GS` objects | `__GSHandlerCheck`, `__GSHandlerCheck_SEH`, `__report_rangecheckfailure`, and on AArch64 `__security_push_cookie`, `__security_pop_cookie` | `gshandler.cpp`, `gs_cookie.S` |
| Load configuration | `_load_config_used` | `loadconfig.cpp` |
| Control Flow Guard | `__guard_check_icall_fptr`, `__guard_dispatch_icall_fptr`, `_guard_icall_checks_enforced` | `cfguard.cpp`, `cfguard_dispatch.S` |
| Delay-load imports | `__delayLoadHelper2`, `__FUnloadDelayLoadedDLL2`, `__HrLoadAllImportsForDll` | `delayload.cpp` |
| Thread-local storage | `_tls_used`, `_tls_index`, `_tls_start`, `_tls_end`, `__xl_a`, `__xl_z` | `tls.cpp` |
| Universal CRT math data | `_HUGE`, `HUGE` | `ucrt_math.c` |
| Universal CRT stdio | the ISO wide-specifier marker | `ucrt_stdio.c` |

The linker picks the entry point from the program's main function, and
extracts only that entry point's file from the archive. Under the GUI
subsystem it picks `WinMainCRTStartup`; a program that defines `main` rather
than `WinMain` gets wincrt's `WinMain` (`winmain_main.cpp`), which calls its
`main`, so that such a program links and runs as it does with MinGW.

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
its image loaded. The registries are one per process: every image imports
them from `clang_rt.wincrt_dynamic.dll`, which pins itself, and finalizes its
own registrations at its detach. A program linked with `-static` keeps them in
the executable, from `clang_rt.wincrt_static.lib`. wincrt's `atexit`,
`_onexit` and `at_quick_exit` stay in each image, since they pass the registry
the image's own `__dso_handle`.

Under kcfi, clang's destructors carry the type `void(void *)` salted
`"__cxa_dtor"`, while a function that any other caller passes to
`__cxa_atexit` has the plain type. Clang registers its destructors through the
`__llvm_kcfi_` entry points instead, which take the same arguments, and each
entry is called through the type of the entry point that registered it.

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
