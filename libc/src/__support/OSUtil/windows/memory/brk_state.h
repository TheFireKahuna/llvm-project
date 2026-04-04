//===-- Program break (brk/sbrk) engine for NT-POSIX -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Emulates the Linux brk(2) syscall on Windows NT using demand-growing
// page reservations. The brk region is a contiguous VA segment that starts
// small (one allocation granularity = 64 KB) and extends into adjacent VA
// as needed, capped by RLIMIT_DATA.
//
// State lives in the ProcessControlBlock (g_pcb.brk). Initialization
// runs from brk_startup_init() (Phase 4b of __libc_dll_init()).
// Fork reinit resets the lock via brk_fork_reinit().
//
// Sits at the same level as SlabPool -- directly on page_alloc.h, fully
// independent of mmap, mapping_table, and malloc.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_BRK_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_BRK_STATE_H

#include "src/__support/macros/config.h"

#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Initialize the brk subsystem. Reserves the initial VA region and
// populates g_pcb.brk fields. Called once from brk_startup_init().
void brk_init();

// Linux brk(2) kernel semantics:
//   - addr == 0: return current break.
//   - addr > current break: grow (commit pages, extend reservation if needed).
//   - addr < current break: shrink (decommit pages above new break).
//   - addr < brk_base: no-op, return current break.
//   - On success: return new break as intptr_t.
//   - On failure: return current break unchanged (never returns -errno).
//
// Thread-safe: serialized by g_pcb.brk.lock.
intptr_t sys_brk(void *addr);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_BRK_STATE_H
