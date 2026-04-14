//===-- Open file description pool for Windows ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Lock-free demand-committed pool of OpenFileDescription objects. Backed by
// SlabPool with flat VA reservation.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_OFD_POOL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_OFD_POOL_H

#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

struct OpenFileDescription; // Forward decl — defined in fd_table.h.

namespace ofd_pool {

// Slot size must fit OpenFileDescription. Validated by static_assert in
// ofd_pool.cpp where OpenFileDescription is visible.
inline constexpr unsigned SLOT_SIZE = 64;

// Reserve VA for this many slots. At 64 bytes each, 65536 slots = 4MB VA.
// Zero physical memory until pages are committed.
inline constexpr unsigned MAX_SLOTS = 65536;

/// Reserve the VA range. Called once at process startup (ofd_pool_startup_init(), Phase 4).
void init();

/// Allocate a SLOT_SIZE-byte slot and zero it. Lock-free.
/// Returns nullptr on failure.
OpenFileDescription *alloc();

/// Return a slot to the freelist for reuse. Lock-free. Passing nullptr
/// is a no-op.
void free(OpenFileDescription *ofd);

/// Reset after fork.
void fork_reinit();

} // namespace ofd_pool
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_OFD_POOL_H
