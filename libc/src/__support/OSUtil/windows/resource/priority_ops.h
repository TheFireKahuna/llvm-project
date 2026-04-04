//===-- Internal getpriority/setpriority declarations -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Engine functions for POSIX getpriority() and setpriority().
//
// getpriority returns (kNZero - nice_value) on success (range 1..40) to avoid
// ambiguity with negative errno codes. Callers convert back via kNZero - ret.
// setpriority returns 0 on success, -errno on failure.
//
// Both support PRIO_PROCESS, PRIO_PGRP, and PRIO_USER with system-wide
// process enumeration via NtGetNextProcess.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_RESOURCE_PRIORITY_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_RESOURCE_PRIORITY_OPS_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/id_t.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// POSIX NZERO: the value such that nice 0 maps to priority 20.
// Use a non-macro-shaped identifier so this internal API does not collide
// with the public NZERO macro from limits-macros.h.
inline constexpr int kNZero = 20;

// Returns (kNZero - nice_value) on success, or -errno on error.
intptr_t getpriority(int which, id_t who);

// Returns 0 on success, or -errno on error.
intptr_t setpriority(int which, id_t who, int nice);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_RESOURCE_PRIORITY_OPS_H
