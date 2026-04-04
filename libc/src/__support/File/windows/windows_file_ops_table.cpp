//===-- .libcfio bookends + walker ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Hosts the `.libcfio$A/$Z` bookends, the flat WindowsFileKind ->
// WindowsFileOps* dispatch array, the section walker, and the
// kind_to_windows_file_ops() lookup accessor.
//
// Optional/all-present kinds register into `.libcfio$M` from their
// owning TUs via LIBC_REGISTER_WINDOWS_FILE_OPS. If an owning TU isn't
// pulled into the link, that kind's slot stays null and
// kind_to_windows_file_ops() traps on null.
//
//===----------------------------------------------------------------------===//

#include "src/__support/File/windows/windows_file_ops_section.h"

#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/section_registry.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/config.h"

LIBC_DEFINE_SECTION_BOOKENDS(
    libcfio, ::LIBC_NAMESPACE::internal::WindowsFileOpsRegistration)

// Force-pull every TU that calls LIBC_REGISTER_WINDOWS_FILE_OPS. Each
// emits a `__libc_fio_anchor_<kind>` extern; without this the .obj's
// only outward artifact is the .libcfio$M section record and archive
// selection would silently drop optional WindowsFile kinds.
LIBC_FORCE_PULL_GLOB("__libc_fio_anchor_*");

namespace LIBC_NAMESPACE_DECL {
namespace internal {

static constexpr size_t kKindCount =
    static_cast<size_t>(WindowsFileKind::Count);
static const WindowsFileOps *g_windows_file_ops_table[kKindCount] = {};

void init_windows_file_ops_table() {
  auto registry = libc_libcfio_registry();

  // Release-mode hard fail. Same rationale as the .libcops table:
  // every host-side FILE* open routes through this dispatch vector,
  // so a writable .libcfio is direct CFG bypass.
  enforce_section_readonly_or_fastfail(g_pcb.zone0.module_handle(),
                                       registry.probe_address(), ".libcfio");

  // The iteration range may include padding slots: WindowsFileOpsRegistration
  // is 16 bytes with alignof 8; the $A/$Z bookend subsections get 16-byte
  // alignment (MSVC default for const aggregates) while $M entries get
  // 8-byte alignment. sizeof already matches the bookend alignment, so no
  // padding is expected between $M and $Z today, but the null-ops guard
  // below makes the walker resilient to a future enum growth that changes
  // the alignment relationship. Zero-init bookends and any padding produce
  // that pattern; a real LIBC_REGISTER_WINDOWS_FILE_OPS always supplies a
  // concrete ops pointer.
  for (const auto &rec : registry) {
    if (rec.ops == nullptr)
      continue;
    size_t idx = static_cast<size_t>(rec.kind);
    LIBC_ASSERT(idx < kKindCount &&
                "WindowsFileOpsRegistration kind out of range");
    g_windows_file_ops_table[idx] = rec.ops;
  }
}

const WindowsFileOps *kind_to_windows_file_ops(WindowsFileKind kind) {
  size_t idx = static_cast<size_t>(kind);
  LIBC_ASSERT(idx < kKindCount && "WindowsFileKind out of range");
  const WindowsFileOps *ops = g_windows_file_ops_table[idx];
  LIBC_ASSERT(ops != nullptr &&
              "WindowsFileKind ops not registered -- owning TU wasn't linked "
              "or init_windows_file_ops_table() hasn't run yet");
  return ops;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
