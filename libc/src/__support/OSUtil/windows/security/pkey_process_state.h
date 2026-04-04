//===-- Process-wide pkey state ----------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SECURITY_PKEY_PROCESS_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SECURITY_PKEY_PROCESS_STATE_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

inline constexpr int PKEY_COUNT = 16;

struct PkeyProcessState {
  cpp::Atomic<uint32_t> allocated;
  cpp::Atomic<uint32_t> rights[PKEY_COUNT];
  void *range_table;
  cpp::Atomic<uint32_t> initialized;
};

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SECURITY_PKEY_PROCESS_STATE_H
