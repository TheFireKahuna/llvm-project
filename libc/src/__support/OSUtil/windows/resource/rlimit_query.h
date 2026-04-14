//===-- Lightweight rlimit accessors for enforcement -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin inline accessors for RLIMIT soft limits, used by the fd table and I/O
// paths to enforce RLIMIT_NOFILE and RLIMIT_FSIZE. Named accessors keep
// call sites readable without exposing raw PCB field indexing.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_RESOURCE_RLIMIT_QUERY_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_RESOURCE_RLIMIT_QUERY_H

#include "rlimit_state.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

// Return the current RLIMIT_NOFILE soft limit. Returns RLIM_INFINITY if
// no limit has been set (caller should treat as "no limit").
[[gnu::always_inline]] inline rlim_t get_nofile_limit() {
  ensure_rlimit_init();
  return g_pcb.rlimit.limits[RLIMIT_NOFILE].rlim_cur;
}

// Return the current RLIMIT_FSIZE soft limit. Returns RLIM_INFINITY if
// no limit has been set.
[[gnu::always_inline]] inline rlim_t get_fsize_limit() {
  ensure_rlimit_init();
  return g_pcb.rlimit.limits[RLIMIT_FSIZE].rlim_cur;
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_RESOURCE_RLIMIT_QUERY_H
