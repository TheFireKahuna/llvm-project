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
| Initialization | `.CRT$XI*` C initializers, `.CRT$XC*` constructors, UCRT argv/environment setup | `init.cpp` |
| Termination | UCRT exit callback, `.CRT$XP*`/`.CRT$XT*`, `_amsg_exit`, `_is_c_termination_complete` | `init.cpp` |
| Itanium registries | `__cxa_atexit`, `__cxa_finalize`, `__cxa_at_quick_exit`, `__cxa_thread_atexit_impl`, `__cxa_thread_finalize` | `cxa_atexit.cpp`, `cxa_thread_atexit.cpp` |
| C registration | `atexit`, `at_quick_exit`, `_onexit`, `__dllonexit` | `atexit.cpp` |
| Security | `__security_cookie`, `__security_check_cookie`, `__security_init_cookie`, `__report_gsfailure` | `security.cpp` |
| /GS frame handlers | `__GSHandlerCheck`, `__GSHandlerCheck_SEH`, `__report_rangecheckfailure`: the language handlers MSVC-built `/GS` objects register on frames with `__try` or `alloca` (x86_64) | `gshandler.cpp` |
| Control Flow Guard | `__guard_*_fptr`, `_guard_icall_checks_enforced`, unchecked x86_64 dispatcher | `cfguard.cpp`, `cfguard_dispatch.S` |
| Load configuration | `_load_config_used` | `loadconfig.cpp` |
| Thread-local storage | `_tls_used`, `_tls_index`, `_tls_start`, `_tls_end`, `__xl_a`, `__xl_z` | `tls.cpp` |
| Delay loading | `__delayLoadHelper2`, `__HrLoadAllImportsForDll`, `delayimp.h` hooks | `delayload.cpp` |
| MSVC bridges | `_purecall` over UCRT's handler | `purecall.cpp` |
| Sized deallocation | The four sized `operator delete` forms, each forwarding to the image's unsized one | `delete_sized*.cpp` |
| UCRT stdio | `__local_stdio_*_options` for C images, ISO wide-specifier marker | `ucrt_stdio_*.c` |
| Import library | `clang_rt.ucrt_memory.lib`: `memcpy` and friends, `setjmp`/`longjmp`, `__C_specific_handler`, purecall handler accessors, which `ucrtbase.dll` exports but `ucrt.lib` omits | `ucrt_memory.def` |

The section sentinels (`__xi_a` ... `__xt_z`), `__dso_handle` and `_fltused`
come from compiler-rt builtins (`crt_begin_windows.c`, `crt_end_windows.c`).

## C aligned allocation

`aligned_alloc` and `posix_memalign` share native allocation for every alignment.
For `aligned_alloc`, alignments 1, 2,
4, 8 and 16 use `HeapAlloc` on either heap family; its Kernel32 import forwards
directly to `RtlAllocateHeap`. Extended power-of-two alignments use compact
native size classes, segment-heap page ranges or large allocations. Every successful result works
with ordinary release-UCRT `free` and `realloc`, across threads and DLLs and
after the allocating DLL unloads. No allocation record points into wincrt;
there is no allocation registry, interior user pointer, or `free` interception.

Every nonzero, representable request checks heap identity before either native
path. On first use, the public entry retains the handle returned by UCRT's
`_get_heap_handle`, independently of the PEB. Each call compares the current
PEB handle against that owner and rejects null/mismatch,
before dereferencing heap metadata. The backend receives the verified handle
instead of reloading the PEB. A forged segment signature on another pointer is
therefore insufficient. The retained handle is process memory too: this catches
handle substitution but is not authentication against arbitrary process writes.

The private adapter in `aligned_alloc.cpp` is qualified for **x64 ntdll
10.0.26100.9539**, CodeView **0D5BBF21-0A19-B691-5591-A9BB852F417A**, age 1.
Other revisions, AArch64 and non-segment hosts reject nonzero extended requests
with `EINVAL` from `aligned_alloc` or `ENOMEM` from `posix_memalign`.
The private page/large paths also reject unsupported heap
environments and diagnostic/tagging modes.
Segment-heap selection alone does not qualify these private entry points.
Updating Windows may therefore disable extended allocation until another
revision is reviewed and qualified. This is an intentional compatibility limit,
not a stable Windows ABI or a claim of general upstream readiness.

There is no fixed alignment ceiling such as 64 MiB. Arithmetic that cannot be
represented safely by native RTL is rejected before acquiring resources;
otherwise native address-space/commit exhaustion returns null with `ENOMEM`.
Large alignment can reserve at least that much address space. Padding is not
committed. Requests whose size and alignment are at most 2048 first select the
next power-of-two native size class. LFH aligns both the first block and the
stride to that size. An actual-address check also covers cold/disabled buckets
served by VS: an unsuitable block is freed and the page path supplies alignment.
There is one candidate allocation, no retry-until-aligned loop. Native usable
size can exceed the requested size. The implementation is not
claimed to be faster than Microsoft's separately paired aligned allocator.

The C17/C23 contract includes the final
[WG14 DR 460 correction](https://www.open-std.org/jtc1/sc22/wg14/issues/c11c17/issue0460.html):
sizes need not be alignment multiples. Wincrt accepts these semantics in C11
mode too. Invalid (zero or non-power-of-two) alignments return null with
`EINVAL`. Zero-sized requests return null with `EINVAL` before backend setup.
This is the zero-size rejection permitted by
[POSIX.1-2024](https://pubs.opengroup.org/onlinepubs/9799919799/functions/aligned_alloc.html);
leaving errno unchanged on that null result would not satisfy POSIX. Excessive
sizes and allocation failures report `ENOMEM`. No alignment invokes UCRT's
invalid-parameter handler or its optional Microsoft malloc/new-handler retry
mode. That retry-mode change also applies to alignments at most 16.

`posix_memalign` requires a power-of-two alignment that is a multiple of
`sizeof(void *)` (8 on this target). It returns `EINVAL` for invalid alignment,
`ENOMEM` for an unavailable allocation capability or allocation failure, and
zero on success. It never changes `errno` and writes the caller's output slot
only on success. Valid zero-size requests succeed with a null pointer before
backend setup. Sizes need not be alignment multiples. Internally, invalid
alignment, unavailable capability and exhaustion remain distinct; the POSIX
interface does not misreport unsupported heap policy as invalid alignment.
The revision-dependent availability limit above still applies.

For the same valid nonzero inputs, both APIs request the same native storage.
Alignment 8 needs no extra padding: both Win64 heap families already provide
at least 16-byte alignment. The minimum alignment is an argument constraint,
not a minimum allocation size. `posix_memalign` adds an output-slot store and
returns status instead of a pointer; no relative latency claim is made.
Neither API change switches libc++'s existing internal `_aligned_malloc` /
`_aligned_free` pair.

Cold qualification checks the loaded image identity and shared UCRT heap
owner and registers exact CFG entries through the SDK's `onecore.lib` import
of `SetProcessValidCallTargets`. Indirect calls retain CFG instrumentation.
Publication uses a single atomic word with no waiting initialization state;
concurrent callers may perform identical registrations. No loader-dependent
initialization lock, constructor, TLS cache or per-allocation lookup is needed.

Before private allocation, the adapter publishes `TEB.HeapWalkContext` and
checks current heap modes in the same order as RTL. RTL's exclusive side
sets its lock bit, flushes process write buffers, and waits for these markers.
The adapter drops its marker before waiting through the exported
`RtlWaitOnAddress`, then republishes and rechecks the flags on each wake. The
exclusive owner may allocate without waiting. Ordinary contention is not an
allocation failure. Nested marked entry and unsupported private modes return
`EINVAL` from `aligned_alloc` or `ENOMEM` from `posix_memalign`. SEH cleanup clears
its marker on unwind; corruption exceptions are not swallowed. Large-path
rollback releases native metadata and reservations at each failure boundary.

The Microsoft family remains separate: `_aligned_malloc` and
`_aligned_offset_malloc` still require `_aligned_free`. They are not made
compatible with `free`, and wincrt results must not go to `_aligned_free`.
The cross-image guarantee assumes the shared release UCRT domain used by this
target, not unrelated custom allocators or a different debug CRT.

Tests cover exact alignment, disjoint writable storage, non-multiple and
zero sizes, failures, ordinary free/realloc, DLL and thread lifetime, early
initialization, concurrent first use and heap walking, foreign-host fallback,
CFG refusal, POSIX error/output/errno semantics, compact size classes,
lock-owner allocation, another thread's
blocked allocation, and deterministic rollback. Production integration exercises
alignments through **16 GiB**, which is a test range rather than an API cap.
The ordinary page-range path has one CFG-protected native allocation call;
fixed-size header reads and entry-address arithmetic emit no helper calls.
These code-generation checks do not establish an optimal latency bound.

The allocator is one translation unit. `heap.h` retains only the inline heap
identity operations shared with startup. The public entries share a local
allocator with explicit failure classification and no errno access. Its
extended backend keeps its SEH frame off the fundamental path; the larger
reservation routine and cold qualification/wait routines remain out of line.
Scalar native-storage copies and entry-address helpers inline without calls.
`PendingLargeAllocation` owns metadata and reservation resources until tree
insertion transfers them to RTL. Ordinary failure returns release acquired
resources in reverse order using the same native in/out slots. The scope owner
is non-copyable and needs neither the C++ standard library nor exception support;
the enclosing SEH guard remains responsible for marker cleanup during native
exceptions. Shared owner and qualification state use lock-free Clang atomics;
the large tree uses RTL's SRW
lock, and its independent counters use relaxed atomic updates. These boundaries
preserve CFG on private ntdll calls without introducing virtual dispatch.

Executable startup still checks the target's segment-heap policy after security
initialization and before application constructors. Allocation-time ownership
and private-revision checks serve a separate purpose and remain in place.

## Lifetime ownership

`cxa_atexit.cpp` and `cxa_thread_atexit.cpp` are compiled twice. Compiled
into the shared libc++ with `WINCRT_SHARED_CXX_RUNTIME` (`libcxx.cmake`,
selected by the cache files through `CMAKE_PROJECT_Runtimes_INCLUDE`), they
export the process-wide registries from `libc++.dll`. Compiled into
`clang_rt.wincrt.lib` they define the same code as `__wincrt_local_*`, and
`/alternatename` directives select that copy only in images that do not link
`libc++.dll.lib`: C programs and the runtime DLLs themselves during bootstrap. C++
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
`_is_c_termination_complete`, `_purecall`, `__GSHandlerCheck`,
`__GSHandlerCheck_SEH`, `__report_rangecheckfailure`) are provided here.
MSVC C++ objects do not link: they need `__CxxFrameHandler` and the MSVC
STL, and their C++ ABI differs. A structured exception, raised by hardware or
by `RaiseException` from MSVC-built code, is offered to `catch (...)` in the
Itanium frames it reaches and runs their cleanups on the way, as MSVC `/EHa`
does; `_set_se_translator` (`<eh.h>`, provided by libc++abi) turns it into a
C++ exception of a chosen type, and `-fasync-exceptions` extends the coverage
from calls to every instruction of a try block or an object's lifetime. Those
live in libunwind and libc++abi, not here.

## Tests

`compiler-rt/test/crt` (`check-crt`) covers startup, termination ordering,
DLL and thread lifetimes, foreign-host behaviour, CFG, the load configuration,
signals and UCRT integration. After changing wincrt in a runtimes build,
rebuild the archive and relink `libc++.dll` and `libunwind.dll`; the runtime build
graph does not track the archive as a dependency.
