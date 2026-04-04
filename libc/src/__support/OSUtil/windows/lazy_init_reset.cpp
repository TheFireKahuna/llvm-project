//===-- .libclzr bookends + walker -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/lazy_init_reset.h"

#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/section_registry.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/config.h"

// $A/$Z bookends + `libc_libclzr_registry()` factory.
LIBC_DEFINE_SECTION_BOOKENDS(libclzr,
                             ::LIBC_NAMESPACE::internal::LazyInitResetEntry)

// Force-pull every TU that calls LIBC_REGISTER_LAZY_RESET. The
// `__libc_lzr_anchor_<tag>` extern emitted by the macro is the linker
// hook; the static reset thunk inside the TU has internal linkage and
// can't be globbed directly.
LIBC_FORCE_PULL_GLOB("__libc_lzr_anchor_*");

namespace LIBC_NAMESPACE_DECL {
namespace internal {

void reset_all_lazy_inits() {
  auto registry = libc_libclzr_registry();

  // Release-mode hard fail. exec_self_hollow() runs this on every exec
  // and the table tells us which LazyInit gates to clear; if it's
  // writable, an attacker with arbitrary-write can repoint a reset thunk
  // to attacker code, called at the next exec(). Continuous-audit failure
  // here is non-negotiable — reseat the process rather than ship a
  // poisoned reset table.
  enforce_section_readonly_or_fastfail(g_pcb.zone0.module_handle(),
                                       registry.probe_address(),
                                       ".libclzr");

  for (const auto &entry : registry)
    entry.reset();
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
