//===-- MappingTable process-wide mutable state ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Runtime-mutable half of the mapping table. Lives in PCB Zone 1 as
// `g_pcb.mapping_table`. The sealed-for-life half (l1 / l1_size /
// max_view_base / remap_guards) is already in PCB Zone 0 —
// g_pcb.zone0.mapping_table_* — and is written exactly once during
// Tier A Phase 0c.5 via PcbInitAccess.
//
// This header is deliberately lightweight: it declares only the fields
// (no methods, no PCB dependency) so that process_control_block.h can
// include it without pulling in mapping_table.h (which needs PCB
// accessors, creating a cycle). MappingTable methods live in
// mapping_table.h and reference `g_pcb.mapping_table.<field>` directly.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_MAPPING_TABLE_PROCESS_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_MAPPING_TABLE_PROCESS_STATE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/memory/commit_region.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/futex_utils.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace memory {

// Layout note: `active_remap_count_` is alignas(64) — it's the hot-read
// field for every VEH fault-path check and warrants its own cache line.
// The surrounding fields are cold (bootstrap init, reconciliation, fini).
struct MappingTableProcessState {
  // Initialisation phase latch. FutexValueType default-constructs to 0
  // which matches MappingTable::INIT_UNINITIALIZED; ensure_init() CASes
  // it through INIT_IN_PROGRESS → INIT_READY.
  Futex init_state_{0};

  // VEH hot-read: in-flight REMAPPING operation count. A Futex so
  // reconciliation can wait for drain without polling; every transition
  // to zero wakes waiters.
  alignas(64) Futex active_remap_count_{0};

  // Diagnostic: WRITING slots recovered from dead owners.
  cpp::Atomic<uint32_t> forced_write_recoveries_{0};

  // Remap-guard array. Demand-committed; initialised by ensure_init().
  internal::CommitRegion guard_region_;
  cpp::Atomic<uint32_t> guard_high_water_{0};
  cpp::Atomic<uint32_t> alloc_cursor_{0};
};

} // namespace memory
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_MAPPING_TABLE_PROCESS_STATE_H
