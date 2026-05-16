//===- mincore.h - POSIX-layer mincore declaration --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MINCORE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MINCORE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"
#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Per-page residency probe. Writes one byte per page covering `[addr,
// addr + length)` to `vec`; bit 0 is set when the page is resident (kernel
// Valid bit OR Invalid.Location == MemoryLocationResident, i.e. standby /
// modified list), bits 1..7 are POSIX-reserved and must not be touched.
// Returns 0 on success or Linux-flavoured `-errno` (negative internally; the
// public POSIX shim flips the sign). Mid-range MEM_FREE yields `-ENOMEM`.
intptr_t mincore(void *addr, size_t length, unsigned char *vec);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MINCORE_H
