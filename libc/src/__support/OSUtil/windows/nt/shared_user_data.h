//===-- Single accessor for KUSER_SHARED_DATA -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Windows maps KUSER_SHARED_DATA at a fixed virtual address in every process.
// This header provides a single typed accessor so call sites need not repeat
// the raw reinterpret_cast.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_SHARED_USER_DATA_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_SHARED_USER_DATA_H

#include "src/__support/OSUtil/windows/nt/nt_context_types.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_util {

// KUSER_SHARED_DATA is mapped read-only at 0x7FFE0000 in every Windows
// process (32-bit and 64-bit, WoW64 included). The layout is ABI-stable
// and checked by static_asserts in nt_context_types.h.
LIBC_INLINE const KUSER_SHARED_DATA *shared_user_data() {
  return reinterpret_cast<const KUSER_SHARED_DATA *>(0x7FFE0000);
}

} // namespace windows_util
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_SHARED_USER_DATA_H
