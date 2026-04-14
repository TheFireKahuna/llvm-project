//===-- Canonical VEH state for Windows --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Lightweight process-wide state types for the unified VEH dispatcher.
// These are the canonical storage types used directly in the PCB.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VEH_VEH_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VEH_VEH_STATE_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/futex_utils.h"

#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {

struct VehFilter {
  uint32_t exception_mask;                 // Bitmask of VehExceptionBit
  LONG (*handler)(EXCEPTION_POINTERS *ep); // Filter callback
  uint8_t priority;                        // Lower = earlier dispatch
};

inline constexpr uint8_t VEH_PRIORITY_MEMORY = 10;
inline constexpr uint8_t VEH_PRIORITY_MLOCK = 15;
inline constexpr uint8_t VEH_PRIORITY_SIGNAL = 20;

inline constexpr int VEH_MAX_FILTERS = 8;

struct VehState {
  VehFilter filters[VEH_MAX_FILTERS];
  cpp::Atomic<int> filter_count;
  Futex filter_lock{0};
  cpp::Atomic<void *> handler_handle; // RtlAddVEH return
  void *dll_notify_cookie;            // LdrRegisterDllNotification
  unsigned reentry_tls_index;
  unsigned fault_guard_tls_index;     // FaultGuard chain head (TEB slot)
};

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VEH_VEH_STATE_H
