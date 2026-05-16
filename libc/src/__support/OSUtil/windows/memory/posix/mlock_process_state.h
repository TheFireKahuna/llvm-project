//===- mlock_process_state.h - PCB-resident mlockall flags ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MLOCK_PROCESS_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MLOCK_PROCESS_STATE_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// PCB Zone 1 home for `mlockall` flag state. Bit layout matches POSIX
// `MCL_*` byte-for-byte so `mlockall` can `fetch_or` straight into the
// field without a translation step:
//
//   bit 0 (MCL_CURRENT) — historically set; meaningless after the
//                         one-shot walk completes. No consumer reads it.
//   bit 1 (MCL_FUTURE)  — every successful subsequent mmap calls
//                         `windows::lock_if_future`, which checks this
//                         bit and locks the new range.
//   bit 2 (MCL_ONFAULT) — modifier on MCL_FUTURE. (Today's mmap path
//                         degrades to immediate lock; the per-region
//                         LOCK_ONFAULT bit kicks in once mmap routes
//                         through `va_tracker::acquire`.)
//
// Per-region lock-on-fault state lives on the va_tracker desc as
// `region_flag::LOCK_ONFAULT` (the internal bit; distinct from the
// POSIX `MLOCK_ONFAULT` user-flag set by `mlock2`). Kernel-side
// per-page lock counts live in `NtLockVirtualMemory`. Fork resets
// `mcl_flags` to zero via `mlock_policy_fork_reinit`; per-desc bits
// are stripped by the va_tracker fork serializer.
struct MlockProcessState {
  ::LIBC_NAMESPACE::cpp::Atomic<unsigned> mcl_flags{0u};
};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MLOCK_PROCESS_STATE_H
