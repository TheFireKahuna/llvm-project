//===-- Fork reinit registration for mapping table --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Calls g_mapping_table.fork_reinit() from mapping_table_fork_reinit() so
// stale REMAPPING entries in the radix tree are cleaned up in the fork child.
// Must run before fd_table_fork_reinit() since fd operations may trigger mmap
// lookups. Also handles shutdown teardown via mapping_table_startup_fini()
// so the backing store is explicitly released instead of relying on
// process-exit reclamation.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/mapping_table.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"

void LIBC_NAMESPACE::internal::mapping_table_fork_reinit() {
  LIBC_NAMESPACE::windows::g_mapping_table.fork_reinit();
}

void LIBC_NAMESPACE::internal::mapping_table_startup_fini() {
  LIBC_NAMESPACE::windows::g_mapping_table.destroy();
}
