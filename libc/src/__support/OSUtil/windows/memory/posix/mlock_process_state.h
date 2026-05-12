//===- mlock_process_state.h - PCB-resident mlockall flags ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Process-wide `mlockall` flag state. The atomic word lives in PCB
/// Zone 1 next to the brk cursor and the PAL state — same shape as the
/// other process-wide POSIX-subsystem fields, same fork-CoW handling,
/// same write-once-mostly access pattern.
///
/// The bit layout matches the POSIX `MCL_*` macros byte-for-byte so a
/// `mlockall(flags)` call can `fetch_or` straight into the field
/// without a translation step:
///
///   bit 0 (MCL_CURRENT) — historically set; meaningless after the
///                          one-shot walk completes; left untouched by
///                          this header (callers may stamp it for
///                          telemetry but no consumer reads it).
///   bit 1 (MCL_FUTURE)  — every successful subsequent mmap calls
///                          `lock_if_future`, which checks this bit
///                          and either locks the new range or arms it
///                          via the per-region `MLOCK_ONFAULT` bit.
///   bit 2 (MCL_ONFAULT) — modifier on MCL_FUTURE: when both are set,
///                          new mappings get armed for fault-time lock
///                          rather than locked immediately at mmap.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MLOCK_PROCESS_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MLOCK_PROCESS_STATE_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

/// PCB Zone 1 home for `mlockall` flag state.
///
/// One atomic word; everything else about mlock state is per-region
/// (`region_flag::MLOCK_ONFAULT` on the va_tracker desc) or kernel-
/// owned (`NtLockVirtualMemory`'s per-page lock count). The PCB
/// placement gives uniform fork-CoW handling at no fork-reinit cost
/// against the POSIX-mandated reset (a small posix-band fork hook
/// stores zero into `mcl_flags`; per-desc bits are stripped by the
/// va_tracker fork serializer).
struct MlockProcessState {
  /// Active `mlockall` flag bitmask. Layout matches POSIX `MCL_*`.
  ::LIBC_NAMESPACE::cpp::Atomic<unsigned> mcl_flags{0u};
};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MLOCK_PROCESS_STATE_H
