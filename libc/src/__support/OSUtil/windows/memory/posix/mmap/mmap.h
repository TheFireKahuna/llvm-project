//===- mmap.h - POSIX-layer mmap declaration --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Top-level POSIX mmap entry. Both declared functions return the mapped
// address as an `intptr_t` on success and a negative Linux errno on failure.
// Two callers consume that convention: `windows_syscalls::mmap`
// (syscall_wrappers/mmap.h) flips the sign for ErrorOr, while the
// `SYS_mmap` arm in `syscall.h` returns the raw `intptr_t` to preserve
// Linux syscall(2) semantics. `mmap_anon_private` is exposed so future
// per-shape dispatchers and direct tests can bypass the top-level
// flag-matrix work.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MMAP_MMAP_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MMAP_MMAP_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/off_t.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t mmap(void *addr, size_t size, int prot, int flags, int fd,
              off_t offset);

intptr_t mmap_anon_private(void *addr, size_t size, int prot, int flags);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MMAP_MMAP_H
