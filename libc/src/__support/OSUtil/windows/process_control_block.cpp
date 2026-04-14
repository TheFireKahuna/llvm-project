//===-- Process Control Block PE section instantiation -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Instantiates the process-wide ProcessControlBlock in a dedicated PE section.
//
// The ".pcb" section has IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE |
// IMAGE_SCN_CNT_INITIALIZED_DATA. The linker emits it with VirtualSize =
// sizeof(ProcessControlBlock) and SizeOfRawData = 0 (all-zero initializer
// needs no file backing). The NT loader demand-zero-fills pages on first
// touch — no runtime page_reserve or NtCreateSection needed.
//
// After CRT init completes, pcb_seal_readonly() protects page 0 as
// PAGE_READONLY (read-only constants + identity). This provides hardware-
// enforced immutability for page_size, security_cookie, module_handle, and
// the zone canary.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/process_control_block.h"

#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/pcb_init_access.h"
#include "src/__support/OSUtil/windows/tls/tls_cleanup.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// =========================================================================
// PE section placement
//
// The ".pcb" section is a dedicated PE section for the ProcessControlBlock.
// - `read, write` → IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE
// - `long` → IMAGE_SCN_CNT_INITIALIZED_DATA (ensures the section is
//   included in the PE even though SizeOfRawData is 0)
//
// __declspec(allocate(".pcb")) places g_pcb in this section. The `= {}`
// zero-initializer means the linker can emit SizeOfRawData = 0 (pure BSS).
//
// The NT loader maps the section and demand-zero-fills pages. This is
// identical to how .bss works but in a dedicated section, giving us:
// - Spatial locality: all PCB fields in contiguous pages
// - Debugger visibility: ".pcb" appears in PE section dumps
// - Protection boundary: page 0 can be sealed PAGE_READONLY independently
// =========================================================================

// Note: `long` and `short` are silently ignored by Clang's #pragma section
// parser — only `read`, `write`, and `execute` are meaningful. The section
// gets IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE. lld-link still emits the
// section with VirtualSize = sizeof(g_pcb) and SizeOfRawData = 0 for the
// zero-initialized global.
#pragma section(".pcb", read, write)

// constinit: guarantees the zero-initializer is resolved at compile time.
// Prevents dynamic-init-ordering bugs if a field type ever gains a non-trivial
// constructor. All PCB field types (scalars, cpp::Atomic, RawMutex) have
// constexpr default/value constructors, so this is satisfied today and will
// break loudly if that invariant is violated in the future.
// C++20 constinit equivalent for C++17; Clang-only (NTPOSIX target).
[[clang::require_constant_initialization]]
__declspec(allocate(".pcb")) ProcessControlBlock g_pcb = {};

// =========================================================================
// Layout and size validation
// =========================================================================

// Total PCB size guard — catch unexpected growth from field additions.
static_assert(sizeof(ProcessControlBlock) <= 65536,
              "PCB exceeds 64 KB — review field placement and consider "
              "moving large arrays to external demand-commit storage");

// =========================================================================
// Protection zone: page 0 seal / unseal
//
// Page 0 layout (4096 bytes on x64):
//   [0..56)     Read-only constants (page_size, cookie, module_handle, ...)
//   [56..64)    Zone canary
//   [64..72)    Process identity (pid, parent_pid)
//   [72..4096)  Padding (_zone0_pad) — keeps mutable fields in page 1+
//
// After init, page 0 is sealed PAGE_READONLY via NtProtectVirtualMemory.
// Fork reinit temporarily unseals to update PID, then re-seals.
// All mutable fields (umask, uid/gid, signal handlers, etc.) live in
// page 1+ and are always PAGE_READWRITE.
//
// The seal/unseal boundary is exactly sizeof(PcbZone0) = one page.
// =========================================================================

bool pcb_seal_readonly() {
  // page_size must be initialized before sealing.
  if (g_pcb.zone0.page_size() == 0)
    return false;
  return internal::page_protect(&g_pcb, g_pcb.zone0.page_size(), PAGE_READONLY);
}

bool pcb_unseal_readonly() {
  if (g_pcb.zone0.page_size() == 0)
    return false;
  return internal::page_protect(&g_pcb, g_pcb.zone0.page_size(), PAGE_READWRITE);
}

} // namespace LIBC_NAMESPACE_DECL

// =========================================================================
// TLS cleanup — out-of-line so crt_tls.obj doesn't reference g_pcb directly
// =========================================================================

namespace LIBC_NAMESPACE_DECL {
namespace internal {

void tls_cleanup_run_all() {
  auto &state = g_pcb.tls_cleanup;
  unsigned count = state.count.load(cpp::MemoryOrder::ACQUIRE);
  if (count > TLS_CLEANUP_MAX_SLOTS)
    count = TLS_CLEANUP_MAX_SLOTS;
  for (unsigned i = 0; i < count; ++i) {
    void *val = teb_tls_get(state.entries[i].tls_index);
    if (val) {
      state.entries[i].callback(val);
      teb_tls_set(state.entries[i].tls_index, nullptr);
    }
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
