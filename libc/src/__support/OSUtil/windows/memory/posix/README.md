# memory/posix — POSIX memory-op layer

Thin translator between the Linux ABI memory syscalls
(`mmap`/`munmap`/`mprotect`/`mremap`/`madvise`/`mlock`/`msync`/`mincore`/
`brk`/`shm_open`/`shmget`/`mbind`/...) and the substrate's
`va_tracker::` typed-op surface plus the read-only `nt_pal::` surfaces.
Per-op files validate POSIX inputs, build typed-op metadata, call one or
two typed ops, translate the return. **Read the Ethos block in
`POSIX_ROADMAP.md` before contributing.**

## Files (P0 + planned)

```
posix_errno.h          NTSTATUS → errno mapping; ErrorOr → int
posix_validation.h     page rounding; flag/prot validation; HUGETLB probe
posix_meta.h           AcquireMeta builders per intent
posix_mutators.{h,cpp} DescMutator free-function callbacks

mmap/                  P2 + P3 + P4 — mmap dispatch by flag shape
munmap.cpp             P2
mprotect.cpp           P2 — mprotect + pkey_mprotect
mremap/                P7 — six files, one per flag protocol
madvise/               P5 — entry + per-advice file
mlock.cpp              P1 — mlock / mlock2 / munlock
mlockall.cpp           P9 — mlockall / munlockall + MCL_FUTURE
msync.cpp              P1
mincore.cpp            P1
brk.cpp                P6 — brk / sbrk
remap_file_pages.cpp   P9 — deprecated; thin implementation
numa/                  P9 — set_mempolicy / get_mempolicy / mbind
posix_shm/             P8 — shm_open / shm_unlink / memfd_create
sysv_shm/              P8 — shmget / shmat / shmdt / shmctl
```

## Fork-reinit priority table

```
30  crystalline_fork_reinit          (substrate)
31  va_substrate_fork_reinit         (substrate)
32  pal_fork_reinit                  (substrate)
33  allocator_fork_reinit            (substrate)
34  va_tracker_fork_reinit           (substrate)
---  POSIX layer below ---
150 brk_fork_reinit                  brk.cpp
200 sysv_shm_fork_reinit             sysv_shm/sysv_shm_fork.cpp
210 posix_shm_fork_reinit            posix_shm/shm_namespace.cpp
220 mlockall_fork_reinit             mlockall.cpp
230 numa_policy_fork_reinit          numa/numa_validate.cpp
```

Each POSIX-layer hook is a free function registered at file scope. Exec
teardown runs in reverse priority order; see `POSIX_ROADMAP.md` §6.2.

## Structural test

A file here is correctly designed iff, after stripping validation and
errno mapping, what remains is at most two `va_tracker::` calls plus at
most one `DescMutator`. Anything else is absorbing substrate concerns
and belongs inside the typed op as a new shape variant.

Apply ruthlessly at review.

## Phase status

* P0 — Foundations (this directory's header utilities + mutators). **Done.**
* P1 — `mincore` / `msync` / `mlock`.
* P2 — anon-private `mmap` + `munmap` + `mprotect`.
* P3 — `MAP_FIXED` + split.
* P4 — section-backed mmap shapes.
* P5 — `madvise` family.
* P6 — `brk`.
* P7 — `mremap`.
* P8 — POSIX shm + SysV shm.
* P9 — `mlockall` / NUMA / `remap_file_pages`.
