//===-- FILE slot pool for Windows -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Lock-free demand-committed pool of 512-byte slots for File objects
// (WindowsFile, IoRingFile). Backed by SlabPool with flat VA reservation.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FILE_POOL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FILE_POOL_H

#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace file_pool {

// Slot size must fit the largest File subclass (IoRingFile ~320 bytes).
// Validated by static_assert in file_pool.cpp where IoRingFile is visible.
inline constexpr unsigned SLOT_SIZE = 512;
inline constexpr unsigned SLOT_ALIGN = 16;

// Reserve VA for this many slots. At 512 bytes each, 8192 slots = 4MB VA.
// Zero physical memory until pages are committed.
inline constexpr unsigned MAX_SLOTS = 8192;

/// Reserve the VA range. Called once at process startup, before init_std_fds.
void init();

/// Allocate a SLOT_SIZE-byte slot. Lock-free. Returns nullptr on failure.
void *alloc();

/// Return a slot to the freelist for reuse. Lock-free. Passing nullptr
/// is a no-op.
void free(void *slot);

/// Reset after fork. Lock-free pools need no lock reset, but this is
/// called for consistency with the fork_reinit section ordering.
void fork_reinit();

} // namespace file_pool
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FILE_POOL_H
