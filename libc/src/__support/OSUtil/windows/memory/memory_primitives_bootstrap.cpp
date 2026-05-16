//===-- .libcmem bookends + walker --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Runs at Tier A Phase 0c.5. See memory_primitives_bootstrap.h for the
// protocol; see unified-memory-bootstrap.md for the design.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/memory_primitives_bootstrap.h"

#include "src/__support/OSUtil/windows/memory/legacy/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/va_inventory.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/section_registry.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/config.h"

LIBC_DEFINE_SECTION_BOOKENDS(libcmem,
                             ::LIBC_NAMESPACE::internal::MemPrimitiveInitEntry)

// Force-pull every TU that calls LIBC_REGISTER_MEMORY_PRIMITIVE. The
// `__libc_mem_anchor_<tag>` extern emitted by the macro guarantees the
// contributor lands in the link even when its only outward artifact is
// the .libcmem$P<phase> section record.
LIBC_FORCE_PULL_GLOB("__libc_mem_anchor_*");

namespace LIBC_NAMESPACE_DECL {
namespace internal {

void memory_primitives_startup_init() {
  // State precondition: PCB Zone 0 / 0b constants are already written
  // (handlers may read them) and the master VEH is live (handlers may
  // fault safely). pcb_seal_readonly_a has NOT yet run — handlers that
  // need to write sealed-for-life Zone 0 fields (e.g. substrate secrets)
  // must run here, before Phase 2.
  LIBC_ASSERT(pcb_init_state() == PcbInitState::TierA_PcbWritten &&
              "memory_primitives_startup_init() called out of order — "
              "requires pcb_startup_init() to have advanced "
              "pcb_init_state to TierA_PcbWritten first");

  auto registry = libc_libcmem_registry();

  // Release-mode hard fail. Same threat model as .libcveh: if the
  // dispatch table is writable, an arbitrary-write primitive can repoint
  // an init_fn to attacker code and trigger bootstrap to call it.
  enforce_section_readonly_or_fastfail(g_pcb.zone0.module_handle(),
                                       registry.probe_address(),
                                       ".libcmem");

  // Pass 1: run every handler in phase order. Accumulate receipts on the
  // walker's stack. Phase ordering is link-enforced: `.libcmem$P1` merges
  // before `$P2` before `$P3`, and iteration follows that order. A
  // handler at phase N may consume any subsystem a phase M<N handler
  // brought up (substrate secrets → mapping-table init → future
  // primitives).
  //
  // Padding slots (zero-init from section bookends or linker gap
  // alignment) read back with init_fn == nullptr and must be skipped.
  // A non-null `name` paired with a null init_fn indicates a malformed
  // LIBC_REGISTER_MEMORY_PRIMITIVE call; assert in debug, skip in
  // release (better to miss one primitive than jump through null).
  Receipt receipts[kMemPrimitiveReceiptCap];
  uint32_t n_receipts = 0;

  for (const auto &entry : registry) {
    if (entry.init_fn == nullptr) {
      LIBC_ASSERT(entry.name == nullptr &&
                  "LIBC_REGISTER_MEMORY_PRIMITIVE passed a null init_fn "
                  "with a non-null name — the handler would be silently "
                  "dropped");
      continue;
    }

    const uint32_t cap_remaining = kMemPrimitiveReceiptCap - n_receipts;
    const uint32_t produced =
        entry.init_fn(&receipts[n_receipts], cap_remaining);
    // Overflow signals a static configuration bug: kMemPrimitiveReceiptCap
    // is sized from the primitives we know about at build time.
    LIBC_ASSERT(produced <= cap_remaining &&
                "MemPrimitiveInitFn overflowed the receipts buffer — "
                "bump kMemPrimitiveReceiptCap or split the handler");
    n_receipts += produced;
  }

  // Pass 2: batch-register every receipt into the now-live mapping
  // table. By this point every Pass 1 handler has returned — including
  // the MappingTable's own init — so the table is observable to all
  // Pass 2 calls.
  //
  // Atomic-complete semantics (I2): the table goes from "empty of
  // internal regions" to "all internal regions stamped" within this
  // one loop. No other thread is running (single-threaded Tier A), so
  // no observer can see a partially-populated table.
  for (uint32_t i = 0; i < n_receipts; ++i) {
    if (LIBC_UNLIKELY(!LIBC_NAMESPACE::windows::g_mapping_table
                           .register_mapping_internal(
                               receipts[i].base, receipts[i].size)))
      __builtin_trap();
  }

  // Atomically latch ready=true and drain any receipts that queued up
  // during Pass 2's `register_mapping_internal` loop (Pass 2 may have
  // transitively triggered substrate `reserve_new_arena` calls whose
  // auto-enqueue went to the pending queue, since ready was still
  // false). The lock-held latch ensures any post-finalize enqueuer
  // sees ready=true and inline-stamps. See `mapping_table_finalize_init`
  // in mapping_table.cpp.
  mapping_table_finalize_init();

  // Final step of Phase 0c.5: stamp every pre-existing VA range (main
  // thread TEB/PEB/stack as KERNEL_REGION, every loaded module as
  // IMAGE_REGION, anything else as FOREIGN_SENTINEL) and register the
  // DLL notification callback so future LdrLoadDll/LdrUnloadDll events
  // maintain IMAGE_REGION coverage.
  //
  // Not a primitive itself — it's a one-shot bootstrap task that
  // requires the mapping table to be live. Failure is non-fatal: a
  // partial inventory degrades gracefully via lazy reconciliation on
  // first touch.
  (void)LIBC_NAMESPACE::windows::va_inventory_startup_discover();
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
