//===-- .libcveh bookends + walker ---------------------------*- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/veh/veh_filter_registry.h"

#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/section_registry.h"
#include "src/__support/OSUtil/windows/veh/veh_core.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/config.h"

LIBC_DEFINE_SECTION_BOOKENDS(libcveh, ::LIBC_NAMESPACE::windows::VehFilter)

// ---------------------------------------------------------------------------
// VEH filter TU pull
// ---------------------------------------------------------------------------
//
// Every TU that calls LIBC_REGISTER_VEH_FILTER emits both a section record
// and an extern "C" anchor function `__libc_veh_anchor_<tag>` (see
// veh_filter_registry.h). The .obj's only outward artifacts are those
// two — COFF static-archive selection won't pull the .obj in via the
// section record alone (`.drectve /INCLUDE:` is processed only after the
// member is selected), which used to cause silent filter loss when the
// consumer was a per-test trimmed libc.lib.
//
// The directive below glob-matches every anchor symbol from this registry
// and force-pulls the contributing archive members. Decentralised — a new
// LIBC_REGISTER_VEH_FILTER call site needs no edit here. The matching
// happens after the linker's main convergence loop so every archive's
// symbol index is fully populated when the glob runs.
LIBC_FORCE_PULL_GLOB("__libc_veh_anchor_*");

namespace LIBC_NAMESPACE_DECL {
namespace internal {

void register_all_static_veh_filters() {
  // Enforce mlock_policy_startup_init() has run. The mlock filter
  // installed below dereferences OnfaultState inside its VEH handler;
  // sweeping before the state is constructed would leave a live filter
  // reading zero-init storage. pcb_init_state() must be
  // TierA_MlockPolicy exactly — any other value means the caller
  // reordered Tier A phases.
  LIBC_ASSERT(pcb_init_state() == PcbInitState::TierA_MlockPolicy &&
              "register_all_static_veh_filters() called out of order — "
              "requires mlock_policy_startup_init() to have advanced "
              "pcb_init_state to TierA_MlockPolicy first");

  auto registry = libc_libcveh_registry();

  // Release-mode hard fail. The .libcveh table is the dispatch surface
  // every fault on every thread eventually consults; if it is writable
  // the master VEH becomes a CFG-bypass primitive (an arbitrary-write
  // gadget can repoint a filter handler at attacker code, then trigger
  // any fault to call it). Debug-only enforcement was insufficient.
  enforce_section_readonly_or_fastfail(g_pcb.zone0.module_handle(),
                                       registry.probe_address(),
                                       ".libcveh");

  // The iteration range may include padding slots: sizeof(VehFilter) is 24
  // and the $A/$Z bookend sections get 16-byte alignment (MSVC default for
  // const aggregates) while the $M subsection is 8-byte aligned, so lld
  // inserts up to 8 bytes of zero padding between $M and $Z. Those padding
  // bytes read back as a zeroed record. Skip any entry whose handler is
  // null — only zero-init bookend or padding produces that pattern; a real
  // LIBC_REGISTER_VEH_FILTER always supplies a concrete handler.
  //
  // Paired invariant: a genuine padding slot has handler == nullptr AND
  // exception_mask == 0. A non-zero mask paired with a null handler can
  // only come from a malformed LIBC_REGISTER_VEH_FILTER (someone passed
  // nullptr explicitly) — silently skipping that record would drop the
  // filter without any signal. Trip the assert in debug; in release the
  // skip still happens (better to lose one filter than to dispatch into
  // a null pointer), but the build catches it before ship.
  for (const auto &entry : registry) {
    if (entry.handler == nullptr) {
      LIBC_ASSERT(entry.exception_mask == 0 &&
                  "LIBC_REGISTER_VEH_FILTER passed a null handler with a "
                  "non-zero exception mask — dispatch table would be "
                  "missing this filter at runtime");
      continue;
    }
    LIBC_NAMESPACE::windows::insert_static_veh_filter(entry);
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
