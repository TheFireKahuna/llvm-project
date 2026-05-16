//===- mlock_policy.h - mlockall flag accessors + lock helpers --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Split from `mlock_process_state.h` to break a PCB include cycle: the
// PCB pulls the struct definition; the accessors pull the PCB. Each
// header has one job, neither has to forward-declare.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MLOCK_POLICY_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MLOCK_POLICY_H

#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt_pal/lock.h"
#include "src/__support/OSUtil/windows/nt_pal/working_set.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

// RELAXED: a stale read in the post-mmap window only governs whether
// *this* mapping is locked, not future ones; the mlockall return
// imposes its own happens-before on any caller that observed it.
[[nodiscard]] LIBC_INLINE bool mcl_future_enabled() {
  return (::LIBC_NAMESPACE::g_pcb.mlock.mcl_flags.load(
              ::LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED) &
          static_cast<unsigned>(MCL_FUTURE)) != 0u;
}

[[nodiscard]] LIBC_INLINE bool mcl_onfault_enabled() {
  return (::LIBC_NAMESPACE::g_pcb.mlock.mcl_flags.load(
              ::LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED) &
          static_cast<unsigned>(MCL_ONFAULT)) != 0u;
}

// Best-effort post-mmap lock. Lock failure is swallowed so MAP_LOCKED
// cannot demote the parent mmap from success to failure (Linux
// contract).
LIBC_INLINE void lock_range(void *addr, SIZE_T size) {
  HANDLE process = NtCurrentProcess();
  ::LIBC_NAMESPACE::nt_pal::expand_working_set(process, size);
  (void)::LIBC_NAMESPACE::nt_pal::lock_range(addr, size);
}

// MCL_FUTURE policy gate after a successful mmap. MCL_ONFAULT degrades
// to immediate lock here — the legacy mmap engine has no per-region
// desc to host `region_flag::LOCK_ONFAULT`; the desc-aware path comes
// online once mmap routes through `va_tracker::acquire`.
LIBC_INLINE void lock_if_future(void *addr, SIZE_T size) {
  if (LIBC_LIKELY(!mcl_future_enabled()))
    return;
  lock_range(addr, size);
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MLOCK_POLICY_H
