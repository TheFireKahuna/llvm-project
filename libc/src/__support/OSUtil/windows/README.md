# POSIX-on-NT: LLVM libc Windows Backend

Native POSIX runtime implemented directly on NT kernel primitives, bypassing Win32.

## Overview

A complete POSIX C runtime (~94K lines) that maps (and/or translates) POSIX system calls into
NT native API calls (`NtCreateFile`, `NtAllocateVirtualMemoryEx`, etc.) without
using UCRT, the Windows SDK, or any Win32 wrapper layer. Compiled as an LLVM triple (`x86_64-pc-windows-ntposix`), 
ships as `c.dll` — the sole C library for the NTPOSIX target environment. 

Key properties:
- **No Windows SDK dependency** — all NT types and function declarations are in-tree
- **No UCRT/MSVCRT** — llvm-libc is the complete C runtime
- **Direct NT syscalls** — `NtReadFile` not `ReadFile`, `NtAllocateVirtualMemoryEx` not `VirtualAlloc`
- **POSIX-first semantics** — fork-exec, signals, mmap, AF_UNIX sockets, epoll, pthreads, termios
- **Minimum target**: Windows 11 (build 22631+), no fallbacks

## Architecture

### Syscall Dispatch (`syscall.h`)

Linux `SYS_*` numbers are used as compile-time dispatch IDs. Every call site passes
a constant, so the compiler constant-folds the switch and emits a direct call:

```
syscall_impl(SYS_read, fd, buf, n)
  → constant-folds → internal::read(fd, buf, n)
```

Two `[[gnu::always_inline]]` layers ensure zero dispatch overhead. Trivial targets
(TEB reads for getpid/gettid) are force-inlined; complex targets (mmap, read, kill)
are out-of-line.

### Subsystem Directory Map

```
windows/
├── alloc/          Layered NT memory substrate (page_alloc, PlaceholderRange,
│                   SectionView, SlabPool, SlabRegistry, IndexedPool)
├── concurrent/     (reserved)
├── dlfcn/          dlopen/dlsym/dlclose via LdrLoadDll/LdrGetProcedureAddress
├── docs/           (this directory)
├── fd/             File descriptor table: TaggedOfd (8-byte atomic, ABA-safe),
│                   OpenFileDescription (56-byte refcounted, cache-line optimized),
│                   IndexedPool-backed with guard pages and demand-commit
├── imports/        .def import libraries (ntdll, bcryptprimitives, combase,
│                   sspicli) — the minimal DLL surface
├── io/             Full I/O surface: read/write/pread/pwrite/scatter-gather,
│                   stat/statvfs, path ops, directory ops, IoRing integration,
│                   per-thread IoRing (ThreadRing), BatchEngine pipeline, alertable I/O
├── ipc/            FIFO (SipHash-named shared-memory ring), socketpair, AFD sockets
│                   (AF_UNIX), poll/select/epoll (IOCP-backed), sendfile, ConDrv console,
│                   inotify, SysV semaphores
├── memory/         mmap/munmap/mremap/mprotect engine: 3-level radix tree mapping table,
│                   MEM_RESERVE_PLACEHOLDER lifecycle, VEH fault handler (remap guard,
│                   demand-commit, file MAP_PRIVATE demand-read, NUMA interleave),
│                   CoW preservation, MAP_FIXED hardening, MmapLock
├── nt/             Complete NT kernel type system (~15K lines): PEB/TEB (Win11 24H2),
│                   NTSTATUS codes, file/memory/process/thread/security types,
│                   AFD socket structures, IoRing v3, ALPC, context records, path resolver
├── process/        POSIX identity (three-ID model, S4U logon, privilege tiers),
│                   fork (NtCreateUserProcess clone), exec, posix_spawn,
│                   waitpid/waitid, PTY master/slave, console TTY
├── reactor/        Process-wide IOCP backbone: single completion port, drain thread pool,
│                   generation-tagged keys, CompletionRouter for epoll, RCU-style fence
├── resource/       getrlimit/setrlimit/getrusage via NT job objects and quota APIs
├── sched/          CPU affinity (multi-group ThreadSelectedCpuSets), scheduler policy,
│                   priority mapping (POSIX 1-99 → NT 1-15)
├── security/       Three-tier permissions (READONLY, NTFS DACL, WSL EAs),
│                   software pkey enforcement (PKRU + mprotect), chmod/chown
├── signal/         Four-layer signal subsystem:
│   ├── pending/      Atomic bitmask + Vyukov MPSC RT queue
│   ├── transport/    VEH (hardware), APC (cross-thread), ALPC (cross-process), Console
│   ├── dispatch/     Three-state dispatch engine, handler table, SA_RESTART stacking
│   └── control/      SIGSTOP/SIGCONT cooperative protocol with dead-coordinator recovery
├── syscall_wrappers/ ~155 thin ErrorOr<T> wrappers over internal:: engine functions
├── time/           POSIX clocks (QPC-based monotonic, RtlGetSystemTimePrecise realtime),
│                   per-process timers via NtCreateTimer2 + reactor, interval timers
├── tls/            Direct TEB TLS slot access (gs:0xE10 / x18+0x1480), lock-free
│                   allocation via PEB bitmap CAS, .CRT$XLC cleanup
└── veh/            Master VEH handler with priority-sorted filter table, DLL-load
                    re-registration for front-of-chain maintenance, TEB reentry guard
```

### Process Control Block (`process_control_block.h`)

All process-wide state in a single `g_pcb` global placed in a `.pcb` PE section:
- **Zone 0** (page 0, 4096 bytes): Read-only after init. Sealed `PAGE_READONLY`
  after Phase 9 of startup. Holds page_size, security_cookie, PID, NT build number,
  capability bitmask. Compile-time guarded via `PcbInitAccess` friend class.
- **Zone 1** (page 1+): Mutable. Identity, signal handlers, console/TTY, environment,
  memory subsystem, thread registry, reactor, resource limits, VEH, pkey, itimer, etc.

### Initialization Phases (`libc_subsystem_init.cpp`)

```
Phase 0: PCB constants, NT capabilities, hard OS version floor (Win11 23H2+)
Phase 1: VEH infrastructure
Phase 2: Process identity (uid/gid from token)
Phase 3: Allocator + mmap subsystem
Phase 4: Object pools (OFD pool, file pool, wait slots, named semaphores)
Phase 5: VEH fault handlers (demand-commit, mlock onfault)
Phase 6: Fd table
Phase 7: Reactor (IOCP + drain threads) + signal (ALPC port)
Phase 8: Stdio + console (std fds, PTY, console Ctrl handler)
Phase 9: Seal PCB Zone 0 PAGE_READONLY
```

### Threading (`libc/src/__support/threads/windows/`)

- **8-byte CAS-64 Treiber futex** (`futex_utils.h`): Value-check + waiter-push in
  one atomic CAS. Three-phase adaptive wait: UMWAIT/MWAITX hardware monitor → slot
  spin → kernel sleep (`NtWaitForAlertByThreadId`).
- **ThreadLifecycle**: Separately pool-allocated (SlabPool), outlives thread stack.
  Carries exit_word futex, detach state, cancel word, robust mutex list, signal state,
  IoRing pointer, registry slot coordinates.
- **Thread registry**: Epoch-based reclamation (EBR) with TID hash table.
  RegistrySlot = 64 bytes (one cache line), RegistryPage = 4096 bytes (one NT page).
- **Robust mutexes**: Death detection via registry handle borrow +
  `NtWaitForSingleObject(timeout=0)`.

### Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| Linux SYS_* numbers as dispatch IDs | Compile-time constant folding; compatibility |
| 8-byte TaggedOfd (ptr + gen + cloexec) | Lock-free fd lookup in one atomic load |
| Per-thread IoRing | Zero ring contention; three-tier CQE wait |
| Placeholder API for mmap | Correct partial munmap/mremap on NT's view-granular sections |
| ALPC for cross-process signals | Kernel-attested sender identity; private namespace isolation |
| SlabPool with XOR freelists + canaries | Hardened against heap exploitation; no malloc dependency |
| VEH for demand-commit and remap guard | Transparent to application code; no signal/exception masking |
| Process-wide IOCP reactor | Single kernel object for all async subsystems |

### Known Limitations

| Area | Limitation |
|------|-----------|
| Network sockets | AF_INET/AF_INET6 not yet implemented (TBD, AF_UNIX works) |
| setuid-bit exec | Requires PRIV_TCB (SYSTEM/service process), platform limitation |
| RTLD_NEXT | Not implemented (returns ENOSYS) |
| F_GETLK owner | Returns l_pid=-1 (NT doesn't expose lock owner) |
| /proc filesystem | Not available (would require kernel driver) |
| Priority granularity | POSIX 1-99 maps to NT 1-15 (coarse) |
| pkey enforcement | Software only (PKRU written but PTE keys not set by NT) |

### Import Surface

Four `.def` files define the DLL surface:
- **ntdll.dll** (358 exports): NT native API — process/thread/memory/file/ALPC/IoRing
- **bcryptprimitives.dll** (1 export): `ProcessPrng` CSPRNG
- **combase.dll** (5 exports): COM init for LLVM tools
- **sspicli.dll** (3 exports): LSA S4U logon for setuid
