//===-- .libcfin bookends + reverse-phase walker --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/libc_fini_registry.h"

#include "src/__support/OSUtil/windows/nt/nt_debug.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/section_registry.h"
#include "src/__support/OSUtil/windows/veh/fault_guard.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/config.h"

// $A/$Z bookends + `libc_libcfin_registry()` factory. The phased
// `$P0..$P9` slots are declared by LIBC_DECLARE_SECTION_REGISTRY_PHASED
// in the header; the bookends close the merged range.
LIBC_DEFINE_SECTION_BOOKENDS(libcfin, ::LIBC_NAMESPACE::internal::FiniEntry)

// Force-pull every TU that calls LIBC_REGISTER_FINI. Each emits a
// `__libc_fin_anchor_<tag>` extern; without this glob the contributing
// .obj files would be dropped from per-test trimmed libc.lib and their
// teardown thunks would never run on DLL_PROCESS_DETACH.
LIBC_FORCE_PULL_GLOB("__libc_fin_anchor_*");

namespace LIBC_NAMESPACE_DECL {
namespace internal {

void run_all_finis() {
  auto registry = libc_libcfin_registry();

  // Hard fail in release. The fini table is the dispatch list executed
  // during DLL_PROCESS_DETACH; a writable section means an arbitrary-write
  // primitive can substitute attacker code into a fini slot to be called
  // by the loader at module unload (or earlier via FreeLibrary).
  enforce_section_readonly_or_fastfail(g_pcb.zone0.module_handle(),
                                       registry.probe_address(),
                                       ".libcfin");

  // Reverse phase order: $P9 records walk first, then $P8, ..., $P0.
  // Within a bucket linker merge order applies; every bucket's entries
  // are documented in libc_fini_registry.h to be order-independent.
  //
  // Per-fini fault isolation: each entry runs inside its own FaultGuard
  // so a faulting subsystem dtor can't skip every later phase. Without
  // this, a $P5 fault would propagate out of _DllMainCRTStartup with the
  // reactor ($P7), IOCP drain thread, ALPC port ($P9), and the VEH master
  // handler ($P0) still live — the drain thread would crash on its next
  // wakeup against unmapped c.dll code, and subsequent LoadLibrary calls
  // would see a partially-torn-down address space.
  //
  // FAULT_GUARD_DTOR catches hardware faults but not breakpoints, single
  // steps, or stack overflows (see fault_guard.h for rationale). If a
  // fini faults we log via DbgPrintEx, pop the guard implicitly via the
  // longjmp, and continue with the next entry.
  for (const auto &entry : registry.reverse()) {
    windows::FaultGuard fg;
    if (windows::fault_guard_enter(&fg, windows::FAULT_GUARD_DTOR)) {
      // A fini faulting is a bug, not an expected degradation path —
      // trip the assert in debug so it gets noticed. Release builds fall
      // through to the DbgPrintEx + continue so a live host process loses
      // one subsystem's reclamation rather than every later one.
      LIBC_ASSERT(false && "subsystem fini faulted during __libc_dll_fini");
      ::DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "[libc] __libc_dll_fini: fini faulted (0x%08lx), skipping\n",
                   static_cast<unsigned long>(fg.exception_code));
      continue;
    }
    entry.fini();
    windows::fault_guard_leave(&fg);
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
