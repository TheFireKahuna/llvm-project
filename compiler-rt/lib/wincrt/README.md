# wincrt: vcruntime replacement for Windows Itanium

`clang_rt.wincrt.lib` is the CRT startup library for `x86_64-` and
`aarch64-unknown-windows-itanium` images; 32-bit targets are not supported.
Microsoft's toolchain splits the C runtime into `ucrtbase.dll` (the C library)
and `vcruntime` (compiler support, startup, MSVC C++ ABI machinery).
This target keeps UCRT unchanged and replaces vcruntime entirely: everything
that vcruntime would have supplied comes from wincrt, compiler-rt builtins,
libunwind (SEH) and libc++abi. No MSVC tools header or library is on the
include or link path.

## What the library provides

| Area | Symbols | Source |
| --- | --- | --- |
| Entry points | `mainCRTStartup`, `wmainCRTStartup`, `WinMainCRTStartup`, `wWinMainCRTStartup`, `_DllMainCRTStartup`, default `DllMain`, `_CRT_INIT` | `entry_*.cpp`, `init.cpp` |
| Initialization | `.CRT$XI*` C initializers, `.CRT$XC*` constructors, pseudo-relocations, UCRT argv/environment setup | `init.cpp` |
| Termination | UCRT exit callback, `.CRT$XP*`/`.CRT$XT*`, `_amsg_exit`, `_is_c_termination_complete` | `init.cpp` |
| Itanium registries | `__cxa_atexit`, `__cxa_finalize`, `__cxa_at_quick_exit`, `__cxa_thread_atexit_impl`, `__cxa_thread_finalize` | `cxa_atexit.cpp`, `cxa_thread_atexit.cpp` |
| C registration | `atexit`, `at_quick_exit`, `_onexit`, `__dllonexit` | `atexit.cpp` |
| Security | `__security_cookie`, `__security_check_cookie`, `__security_init_cookie`, `__report_gsfailure` | `security.cpp` |
| Control Flow Guard | `__guard_*_fptr`, `_guard_icall_checks_enforced`, unchecked x86_64 dispatcher | `cfguard.cpp`, `cfguard_dispatch.S` |
| Load configuration | `_load_config_used` | `loadconfig.cpp` |
| Thread-local storage | `_tls_used`, `_tls_index`, `_tls_start`, `_tls_end`, `__xl_a`, `__xl_z` | `tls.cpp` |
| Delay loading | `__delayLoadHelper2`, `__HrLoadAllImportsForDll`, `delayimp.h` hooks | `delayload.cpp` |
| MSVC bridges | `_purecall` over UCRT's handler | `purecall.cpp` |
| Sized deallocation | The four sized `operator delete` forms, each forwarding to the image's unsized one | `delete_sized*.cpp` |
| UCRT stdio | `__local_stdio_*_options` for C images, ISO wide-specifier marker | `ucrt_stdio_*.c` |
| Import library | `clang_rt.ucrt_memory.lib`: `memcpy` and friends, `setjmp`/`longjmp`, `__C_specific_handler`, purecall handler accessors, which `ucrtbase.dll` exports but `ucrt.lib` omits | `ucrt_memory.def` |

The section sentinels (`__xi_a` ... `__xt_z`), `__dso_handle` and `_fltused`
come from compiler-rt builtins (`crt_begin_windows.c`, `crt_end_windows.c`),
as does the pseudo-relocation runtime.

## Lifetime ownership

`cxa_atexit.cpp` and `cxa_thread_atexit.cpp` are compiled twice. Compiled
into the shared libc++ with `WINCRT_SHARED_CXX_RUNTIME` (`libcxx.cmake`,
selected by the cache files through `CMAKE_PROJECT_Runtimes_INCLUDE`), they
export the process-wide registries from `c++.dll`. Compiled into
`clang_rt.wincrt.lib` they define the same code as `__wincrt_local_*`, and
`/alternatename` directives select that copy only in images that do not link
`c++.lib`: C programs and the runtime DLLs themselves during bootstrap. C++
images therefore share one registry across every EXE and DLL, so destruction
interleaves in reverse registration order across modules and a DLL's
registrations are removed when it unloads.

The registries take no locks. Each registering image owns an arena of
entries and a lock-free stack over them; every entry carries a global
sequence number, and the process-wide drain merges the image stacks by
sequence, which is exactly the order a single list would produce: reverse
registration across every image, with entries registered during the drain
running next. Consumption is a CAS pop shared by the exit drain and by an
image's own detach, so an entry runs at most once; stack heads are
`{tag, slot}` words so slot reuse cannot confuse a concurrent pop; the arenas
of an unloaded image are freed once no drainer is active and consumed slots
are reused, so memory never grows with history. A thread killed by process
termination in the middle of any step leaves a consistent registry.

Termination has one rule per image kind:

- An executable registers a callback with
  `_register_thread_local_exe_atexit_callback` and announces itself to the
  owner together with its terminators. On `exit`, UCRT runs that callback
  first (thread-locals of the exiting thread), then its own atexit table.
- A registry whose image lives for the whole process posts exactly one
  `_crt_atexit` token (and one `_crt_at_quick_exit` token) into that table
  when it first registers: the shared owner, which is retained for the
  process lifetime, and a C-only executable's local copy. UCRT runs the token
  during `exit()` in reverse order with any host's own atexit handlers, and
  the token drains the exiting thread's thread-locals, then every static
  registration, then the executable's terminators. Across runtimes the order
  is therefore registry-granular: an MSVC host's handlers registered after
  the registry's first registration run before it, earlier ones after. A
  C-only DLL's local copy posts nothing, because UCRT cannot unregister a
  token and the DLL may unload.
- A DLL finalizes its own registrations at `DLL_PROCESS_DETACH`, both for
  `FreeLibrary` and at process exit, after its user `DllMain` has run, exactly
  as vcruntime DLLs do. At process exit this finds nothing left when the token
  ran; when the host is a wincrt executable that terminated abruptly (`_Exit`,
  `quick_exit`, `ExitProcess`) the owner suppresses it so no destructor runs;
  under any other host that terminated abruptly the DLL behaves like an MSVC
  DLL.

A pending thread-local destructor holds a reference to the image that
registered it until the owning thread completes, so `FreeLibrary` cannot
unmap code that a later thread exit will call. `std::thread` drains before
returning to UCRT; threads created without libc++ drain from the PE TLS
callback under the loader lock, and their image references are released from
the thread pool.

## Coexistence with MSVC code

`ucrtbase.dll` is shared with every MSVC-built module in the process:
`errno`, locales, stdio, the heap, `_set_purecall_handler`,
`_set_invalid_parameter_handler` and the quick-exit table are process-wide.
The C++ runtimes are independent: MSVC exceptions do not cross into Itanium
frames and vice versa, and objects must be destroyed by the runtime that
created them. MSVC-compiled C static libraries link into Itanium images; the
vcruntime symbols they reference (`_onexit`, `__dllonexit`, `_CRT_INIT`,
`_is_c_termination_complete`, `_purecall`) are provided here.

## Tests

`compiler-rt/test/crt` (`check-crt`) covers startup, termination ordering,
DLL and thread lifetimes, foreign-host behaviour, CFG, the load configuration,
signals and UCRT integration. After changing wincrt in a runtimes build,
rebuild the archive and relink `c++.dll` and `unwind.dll`; the runtime build
graph does not track the archive as a dependency.
